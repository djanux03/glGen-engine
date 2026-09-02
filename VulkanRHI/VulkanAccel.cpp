#include "VulkanAccel.h"

#include <algorithm>
#include <cstring>

namespace vkrhi {

namespace {

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation alloc = VK_NULL_HANDLE;
  void *mapped = nullptr;
};

Buffer createBuffer(VmaAllocator allocator, VkDeviceSize size,
                    VkBufferUsageFlags usage, bool hostVisible,
                    VkDeviceSize minAlignment = 0) {
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = size;
  bci.usage = usage;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo aci{};
  aci.usage = VMA_MEMORY_USAGE_AUTO;
  if (hostVisible)
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;

  Buffer out;
  VmaAllocationInfo info{};
  if (minAlignment > 0) {
    VK_CHECK(vmaCreateBufferWithAlignment(allocator, &bci, &aci, minAlignment,
                                          &out.buffer, &out.alloc, &info));
  } else {
    VK_CHECK(vmaCreateBuffer(allocator, &bci, &aci, &out.buffer, &out.alloc,
                             &info));
  }
  out.mapped = info.pMappedData;
  return out;
}

VkDeviceAddress bufferAddress(VkDevice device, VkBuffer buffer) {
  VkBufferDeviceAddressInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  info.buffer = buffer;
  return vkGetBufferDeviceAddress(device, &info);
}

// glm is column-major; VkTransformMatrixKHR is a row-major 3x4.
VkTransformMatrixKHR toVkTransform(const glm::mat4 &m) {
  VkTransformMatrixKHR t{};
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 4; ++col)
      t.matrix[row][col] = m[col][row];
  return t;
}

constexpr VkBufferUsageFlags kAsStorageUsage =
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
constexpr VkBufferUsageFlags kScratchUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

} // namespace

bool VulkanAccel::loadFunctions(VulkanContext &ctx) {
  mCtx = &ctx;
  VkDevice device = ctx.device();
  auto load = [&](const char *name) {
    return vkGetDeviceProcAddr(device, name);
  };
  pfnGetBuildSizes =
      reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
          load("vkGetAccelerationStructureBuildSizesKHR"));
  pfnCreateAccel = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
      load("vkCreateAccelerationStructureKHR"));
  pfnCmdBuild = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
      load("vkCmdBuildAccelerationStructuresKHR"));
  pfnGetAccelAddress =
      reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
          load("vkGetAccelerationStructureDeviceAddressKHR"));
  pfnDestroyAccel = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
      load("vkDestroyAccelerationStructureKHR"));
  if (!pfnGetBuildSizes || !pfnCreateAccel || !pfnCmdBuild ||
      !pfnGetAccelAddress || !pfnDestroyAccel) {
    std::fprintf(stderr, "[VulkanAccel] failed to load AS functions\n");
    return false;
  }

  VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
  asProps.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
  VkPhysicalDeviceProperties2 props2{};
  props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props2.pNext = &asProps;
  vkGetPhysicalDeviceProperties2(ctx.physicalDevice(), &props2);
  mScratchAlignment = asProps.minAccelerationStructureScratchOffsetAlignment;
  return true;
}

namespace {

// Records the build of a single BLAS for `in` into `cmd`, writing the
// created handle/buffer/alloc/address into the four out-params and the
// build's scratch buffer into `outScratch` -- the caller must keep that
// alive until the build has executed, then free it. Shared core of
// buildOneBlas() (blocking immediate submit) and
// VulkanAccel::recordBlasBuildAt() (streamed, recorded into the frame's
// command buffer).
void recordOneBlas(VulkanContext &ctx, VkCommandBuffer cmd,
                   const VulkanAccel::BlasInput &in,
                   PFN_vkGetAccelerationStructureBuildSizesKHR pfnGetBuildSizes,
                   PFN_vkCreateAccelerationStructureKHR pfnCreateAccel,
                   PFN_vkCmdBuildAccelerationStructuresKHR pfnCmdBuild,
                   PFN_vkGetAccelerationStructureDeviceAddressKHR pfnGetAccelAddress,
                   VkDeviceSize scratchAlignment, VkAccelerationStructureKHR &outBlas,
                   VkBuffer &outBuffer, VmaAllocation &outAlloc,
                   VkDeviceAddress &outAddress, Buffer &outScratch) {
  VkDevice device = ctx.device();
  VmaAllocator allocator = ctx.allocator();

  VkAccelerationStructureGeometryKHR geom{};
  geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
  geom.geometry.triangles.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
  geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
  geom.geometry.triangles.vertexData.deviceAddress = in.vertexAddress;
  geom.geometry.triangles.vertexStride = in.vertexStride;
  geom.geometry.triangles.maxVertex = in.vertexCount - 1;
  geom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
  geom.geometry.triangles.indexData.deviceAddress = in.indexAddress;

  const uint32_t primCount = in.indexCount / 3;

  VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
  buildInfo.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  buildInfo.geometryCount = 1;
  buildInfo.pGeometries = &geom;

  VkAccelerationStructureBuildSizesInfoKHR sizes{};
  sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
  pfnGetBuildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                   &buildInfo, &primCount, &sizes);

  Buffer asBuf = createBuffer(allocator, sizes.accelerationStructureSize,
                              kAsStorageUsage, false);
  outBuffer = asBuf.buffer;
  outAlloc = asBuf.alloc;

  VkAccelerationStructureCreateInfoKHR ci{};
  ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
  ci.buffer = outBuffer;
  ci.size = sizes.accelerationStructureSize;
  ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  VK_CHECK(pfnCreateAccel(device, &ci, nullptr, &outBlas));

  outScratch = createBuffer(allocator, sizes.buildScratchSize, kScratchUsage,
                            false, scratchAlignment);
  buildInfo.dstAccelerationStructure = outBlas;
  buildInfo.scratchData.deviceAddress = bufferAddress(device, outScratch.buffer);

  VkAccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount = primCount;
  const VkAccelerationStructureBuildRangeInfoKHR *pRange = &range;
  pfnCmdBuild(cmd, 1, &buildInfo, &pRange);

  VkAccelerationStructureDeviceAddressInfoKHR addrInfo{};
  addrInfo.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
  addrInfo.accelerationStructure = outBlas;
  outAddress = pfnGetAccelAddress(device, &addrInfo);
}

// Blocking wrapper: records via `submit` (an immediate fence-waited submit)
// and frees the scratch right after. Init-time only -- streamed builds go
// through VulkanAccel::recordBlasBuildAt() instead.
void buildOneBlas(VulkanContext &ctx, const VulkanAccel::SubmitFn &submit,
                  const VulkanAccel::BlasInput &in,
                  PFN_vkGetAccelerationStructureBuildSizesKHR pfnGetBuildSizes,
                  PFN_vkCreateAccelerationStructureKHR pfnCreateAccel,
                  PFN_vkCmdBuildAccelerationStructuresKHR pfnCmdBuild,
                  PFN_vkGetAccelerationStructureDeviceAddressKHR pfnGetAccelAddress,
                  VkDeviceSize scratchAlignment, VkAccelerationStructureKHR &outBlas,
                  VkBuffer &outBuffer, VmaAllocation &outAlloc,
                  VkDeviceAddress &outAddress) {
  Buffer scratch;
  submit([&](VkCommandBuffer cmd) {
    recordOneBlas(ctx, cmd, in, pfnGetBuildSizes, pfnCreateAccel, pfnCmdBuild,
                  pfnGetAccelAddress, scratchAlignment, outBlas, outBuffer,
                  outAlloc, outAddress, scratch);
  });
  vmaDestroyBuffer(ctx.allocator(), scratch.buffer, scratch.alloc);
}

} // namespace

bool VulkanAccel::buildBlas(VulkanContext &ctx, const SubmitFn &submit,
                            const std::vector<BlasInput> &blases) {
  if (!loadFunctions(ctx))
    return false;

  const size_t n = blases.size();
  mBlas.resize(n, VK_NULL_HANDLE);
  mBlasBuffers.resize(n, VK_NULL_HANDLE);
  mBlasAllocs.resize(n, VK_NULL_HANDLE);
  mBlasAddresses.resize(n, 0);

  for (size_t i = 0; i < n; ++i) {
    // Dead mesh slots (evicted terrain chunks awaiting reuse) come through
    // as zeroed inputs on a full rebuild -- leave the slot empty.
    if (blases[i].vertexCount == 0 || blases[i].indexCount == 0)
      continue;
    buildOneBlas(ctx, submit, blases[i], pfnGetBuildSizes, pfnCreateAccel,
                pfnCmdBuild, pfnGetAccelAddress, mScratchAlignment, mBlas[i],
                mBlasBuffers[i], mBlasAllocs[i], mBlasAddresses[i]);
  }

  std::fprintf(stderr, "[VulkanAccel] Built %zu BLAS\n", n);
  return true;
}

bool VulkanAccel::appendBlas(VulkanContext &ctx, const SubmitFn &submit,
                             const std::vector<BlasInput> &newBlases) {
  // First-ever call: nothing built yet, behave like buildBlas().
  if (mBlas.empty())
    return buildBlas(ctx, submit, newBlases);

  if (!loadFunctions(ctx))
    return false;
  if (newBlases.empty())
    return true;

  const size_t base = mBlas.size();
  const size_t n = base + newBlases.size();
  mBlas.resize(n, VK_NULL_HANDLE);
  mBlasBuffers.resize(n, VK_NULL_HANDLE);
  mBlasAllocs.resize(n, VK_NULL_HANDLE);
  mBlasAddresses.resize(n, 0);

  for (size_t i = 0; i < newBlases.size(); ++i) {
    const size_t dst = base + i;
    buildOneBlas(ctx, submit, newBlases[i], pfnGetBuildSizes, pfnCreateAccel,
                pfnCmdBuild, pfnGetAccelAddress, mScratchAlignment,
                mBlas[dst], mBlasBuffers[dst], mBlasAllocs[dst],
                mBlasAddresses[dst]);
  }

  std::fprintf(stderr, "[VulkanAccel] Appended %zu BLAS (total %zu)\n",
              newBlases.size(), n);
  return true;
}

bool VulkanAccel::recordBlasBuildAt(VulkanContext &ctx, VkCommandBuffer cmd,
                                    size_t index, const BlasInput &input,
                                    PendingGarbage &garbage) {
  if (!loadFunctions(ctx))
    return false;

  if (index >= mBlas.size()) {
    const size_t n = index + 1;
    mBlas.resize(n, VK_NULL_HANDLE);
    mBlasBuffers.resize(n, VK_NULL_HANDLE);
    mBlasAllocs.resize(n, VK_NULL_HANDLE);
    mBlasAddresses.resize(n, 0);
  }

  // Replacing a live slot (brush rebuild): retire, don't destroy -- an
  // in-flight frame's TLAS may still hold the old device address.
  if (mBlas[index]) {
    garbage.accels.push_back(mBlas[index]);
    garbage.buffers.emplace_back(mBlasBuffers[index], mBlasAllocs[index]);
  }
  mBlas[index] = VK_NULL_HANDLE;
  mBlasBuffers[index] = VK_NULL_HANDLE;
  mBlasAllocs[index] = VK_NULL_HANDLE;
  mBlasAddresses[index] = 0;

  Buffer scratch;
  recordOneBlas(ctx, cmd, input, pfnGetBuildSizes, pfnCreateAccel, pfnCmdBuild,
                pfnGetAccelAddress, mScratchAlignment, mBlas[index],
                mBlasBuffers[index], mBlasAllocs[index], mBlasAddresses[index],
                scratch);
  garbage.buffers.emplace_back(scratch.buffer, scratch.alloc);
  return true;
}

void VulkanAccel::releaseBlas(size_t index, PendingGarbage &garbage) {
  if (index >= mBlas.size())
    return;
  if (mBlas[index]) {
    garbage.accels.push_back(mBlas[index]);
    garbage.buffers.emplace_back(mBlasBuffers[index], mBlasAllocs[index]);
  }
  mBlas[index] = VK_NULL_HANDLE;
  mBlasBuffers[index] = VK_NULL_HANDLE;
  mBlasAllocs[index] = VK_NULL_HANDLE;
  mBlasAddresses[index] = 0;
}

void VulkanAccel::freeGarbage(VulkanContext &ctx, PendingGarbage &garbage) {
  if (garbage.accels.empty() && garbage.buffers.empty())
    return;
  if (!pfnDestroyAccel && !loadFunctions(ctx))
    return;
  for (VkAccelerationStructureKHR as : garbage.accels)
    if (as)
      pfnDestroyAccel(ctx.device(), as, nullptr);
  for (auto &[buf, alloc] : garbage.buffers)
    if (buf)
      vmaDestroyBuffer(ctx.allocator(), buf, alloc);
  garbage.accels.clear();
  garbage.buffers.clear();
}

bool VulkanAccel::createTlas(VulkanContext &ctx, uint32_t framesInFlight,
                             uint32_t maxInstances) {
  if (!mCtx && !loadFunctions(ctx))
    return false;

  VkDevice device = ctx.device();
  VmaAllocator allocator = ctx.allocator();

  mMaxInstances = maxInstances;

  // Worst-case build sizes for maxInstances (monotonic in instance count, so
  // per-frame rebuilds with fewer instances always fit).
  VkAccelerationStructureGeometryKHR tlasGeom{};
  tlasGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  tlasGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  tlasGeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
  tlasGeom.geometry.instances.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  tlasGeom.geometry.instances.arrayOfPointers = VK_FALSE;

  VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{};
  tlasBuild.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
  tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  tlasBuild.geometryCount = 1;
  tlasBuild.pGeometries = &tlasGeom;

  VkAccelerationStructureBuildSizesInfoKHR tlasSizes{};
  tlasSizes.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
  pfnGetBuildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                   &tlasBuild, &maxInstances, &tlasSizes);

  const VkDeviceSize instBytes =
      sizeof(VkAccelerationStructureInstanceKHR) * maxInstances;

  mTlas.resize(framesInFlight, VK_NULL_HANDLE);
  mTlasBuffers.resize(framesInFlight, VK_NULL_HANDLE);
  mTlasAllocs.resize(framesInFlight, VK_NULL_HANDLE);
  mInstanceBuffers.resize(framesInFlight, VK_NULL_HANDLE);
  mInstanceAllocs.resize(framesInFlight, VK_NULL_HANDLE);
  mInstanceMapped.resize(framesInFlight, nullptr);
  mInstanceAddresses.resize(framesInFlight, 0);
  mScratchBuffers.resize(framesInFlight, VK_NULL_HANDLE);
  mScratchAllocs.resize(framesInFlight, VK_NULL_HANDLE);
  mScratchAddresses.resize(framesInFlight, 0);

  for (uint32_t f = 0; f < framesInFlight; ++f) {
    Buffer tlasBuf = createBuffer(
        allocator, tlasSizes.accelerationStructureSize, kAsStorageUsage, false);
    mTlasBuffers[f] = tlasBuf.buffer;
    mTlasAllocs[f] = tlasBuf.alloc;

    VkAccelerationStructureCreateInfoKHR tlasCi{};
    tlasCi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    tlasCi.buffer = mTlasBuffers[f];
    tlasCi.size = tlasSizes.accelerationStructureSize;
    tlasCi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VK_CHECK(pfnCreateAccel(device, &tlasCi, nullptr, &mTlas[f]));

    Buffer instBuf = createBuffer(
        allocator, instBytes,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        true);
    mInstanceBuffers[f] = instBuf.buffer;
    mInstanceAllocs[f] = instBuf.alloc;
    mInstanceMapped[f] = instBuf.mapped;
    mInstanceAddresses[f] = bufferAddress(device, instBuf.buffer);

    Buffer scratch = createBuffer(allocator, tlasSizes.buildScratchSize,
                                  kScratchUsage, false, mScratchAlignment);
    mScratchBuffers[f] = scratch.buffer;
    mScratchAllocs[f] = scratch.alloc;
    mScratchAddresses[f] = bufferAddress(device, scratch.buffer);
  }

  std::fprintf(stderr,
               "[VulkanAccel] Created %u per-frame TLAS (max %u instances)\n",
               framesInFlight, maxInstances);
  return true;
}

void VulkanAccel::recordTlasBuild(VkCommandBuffer cmd, uint32_t frame,
                                  const std::vector<InstanceInput> &instances) {
  if (mTlas.empty() || frame >= mTlas.size())
    return;

  uint32_t count = static_cast<uint32_t>(instances.size());
  if (count > mMaxInstances) {
    // Rate-limited: the caller grows capacity on overflow, so this firing
    // repeatedly would mean per-frame stderr spam for a known condition --
    // only warn when the overflow grows past the last warned size.
    if (count > mWarnedOverflowCount) {
      std::fprintf(stderr,
                   "[VulkanAccel] %u instances exceed TLAS capacity %u; "
                   "dropping the rest\n",
                   count, mMaxInstances);
      mWarnedOverflowCount = count;
    }
    count = mMaxInstances;
  }

  // Host-write this frame's instance buffer (the caller has already fenced
  // this frame slot). Instances whose BLAS slot has been released since the
  // input list was assembled write a null reference, which the traversal
  // ignores -- safer than referencing a stale address.
  auto *vkInstances =
      static_cast<VkAccelerationStructureInstanceKHR *>(mInstanceMapped[frame]);
  uint32_t written = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const InstanceInput &in = instances[i];
    if (in.blasIndex >= mBlasAddresses.size() || mBlasAddresses[in.blasIndex] == 0)
      continue;
    VkAccelerationStructureInstanceKHR inst{};
    inst.transform = toVkTransform(in.transform);
    inst.instanceCustomIndex = written;
    inst.mask = 0xFF;
    inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    inst.accelerationStructureReference = mBlasAddresses[in.blasIndex];
    vkInstances[written++] = inst;
  }
  count = written;

  VkAccelerationStructureGeometryKHR tlasGeom{};
  tlasGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  tlasGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  tlasGeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
  tlasGeom.geometry.instances.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  tlasGeom.geometry.instances.arrayOfPointers = VK_FALSE;
  tlasGeom.geometry.instances.data.deviceAddress = mInstanceAddresses[frame];

  VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{};
  tlasBuild.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
  tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  tlasBuild.geometryCount = 1;
  tlasBuild.pGeometries = &tlasGeom;
  tlasBuild.dstAccelerationStructure = mTlas[frame];
  tlasBuild.scratchData.deviceAddress = mScratchAddresses[frame];

  VkAccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount = count;
  const VkAccelerationStructureBuildRangeInfoKHR *pRange = &range;
  pfnCmdBuild(cmd, 1, &tlasBuild, &pRange);

  // Make the build visible to fragment-shader ray queries.
  VkMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  barrier.srcStageMask =
      VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
  VkDependencyInfo dep{};
  dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dep.memoryBarrierCount = 1;
  dep.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dep);
}

bool VulkanAccel::build(VulkanContext &ctx, const SubmitFn &submit,
                        const std::vector<BlasInput> &blases,
                        const std::vector<InstanceInput> &instances,
                        uint32_t framesInFlight) {
  if (!buildBlas(ctx, submit, blases))
    return false;
  // Sized generously (not just 2x current instances) so streamed-in chunk
  // instances (up to ~(2*viewDistanceChunks+1)^2, e.g. 169 at the default
  // viewDistanceChunks=6) don't force a TLAS recreate mid-stream -- that
  // recreate isn't incremental (unlike appendBlas()) and would stall.
  const uint32_t capacity =
      std::max<uint32_t>(static_cast<uint32_t>(instances.size()) * 2, 4096);
  if (!createTlas(ctx, framesInFlight, capacity))
    return false;
  // Build every frame's TLAS once so descriptors are valid before the first
  // per-frame rebuild.
  for (uint32_t f = 0; f < framesInFlight; ++f) {
    submit([&](VkCommandBuffer cmd) { recordTlasBuild(cmd, f, instances); });
  }
  std::fprintf(stderr, "[VulkanAccel] Built %zu BLAS + TLAS (%zu instances)\n",
               blases.size(), instances.size());
  return true;
}

void VulkanAccel::destroyTlasResources(VulkanContext &ctx) {
  VmaAllocator allocator = ctx.allocator();
  for (VkAccelerationStructureKHR tlas : mTlas)
    if (tlas)
      pfnDestroyAccel(ctx.device(), tlas, nullptr);
  for (size_t i = 0; i < mInstanceBuffers.size(); ++i)
    if (mInstanceBuffers[i])
      vmaDestroyBuffer(allocator, mInstanceBuffers[i], mInstanceAllocs[i]);
  for (size_t i = 0; i < mTlasBuffers.size(); ++i)
    if (mTlasBuffers[i])
      vmaDestroyBuffer(allocator, mTlasBuffers[i], mTlasAllocs[i]);
  for (size_t i = 0; i < mScratchBuffers.size(); ++i)
    if (mScratchBuffers[i])
      vmaDestroyBuffer(allocator, mScratchBuffers[i], mScratchAllocs[i]);
  mTlas.clear();
  mTlasBuffers.clear();
  mTlasAllocs.clear();
  mInstanceBuffers.clear();
  mInstanceAllocs.clear();
  mInstanceMapped.clear();
  mInstanceAddresses.clear();
  mScratchBuffers.clear();
  mScratchAllocs.clear();
  mScratchAddresses.clear();
  mMaxInstances = 0;
}

bool VulkanAccel::recreateTlas(VulkanContext &ctx, uint32_t framesInFlight,
                               uint32_t maxInstances) {
  if (!loadFunctions(ctx))
    return false;
  destroyTlasResources(ctx);
  const bool ok = createTlas(ctx, framesInFlight, maxInstances);
  if (ok)
    std::fprintf(stderr, "[VulkanAccel] TLAS capacity grown to %u instances\n",
                 maxInstances);
  return ok;
}

void VulkanAccel::destroy(VulkanContext &ctx) {
  VmaAllocator allocator = ctx.allocator();
  destroyTlasResources(ctx);
  for (VkAccelerationStructureKHR blas : mBlas)
    if (blas)
      pfnDestroyAccel(ctx.device(), blas, nullptr);
  for (size_t i = 0; i < mBlasBuffers.size(); ++i)
    if (mBlasBuffers[i])
      vmaDestroyBuffer(allocator, mBlasBuffers[i], mBlasAllocs[i]);
  mBlas.clear();
  mBlasBuffers.clear();
  mBlasAllocs.clear();
  mBlasAddresses.clear();
}

} // namespace vkrhi

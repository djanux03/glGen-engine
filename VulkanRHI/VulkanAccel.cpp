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

bool VulkanAccel::buildBlas(VulkanContext &ctx, const SubmitFn &submit,
                            const std::vector<BlasInput> &blases) {
  if (!loadFunctions(ctx))
    return false;

  VkDevice device = ctx.device();
  VmaAllocator allocator = ctx.allocator();

  const size_t n = blases.size();
  mBlas.resize(n, VK_NULL_HANDLE);
  mBlasBuffers.resize(n, VK_NULL_HANDLE);
  mBlasAllocs.resize(n, VK_NULL_HANDLE);
  mBlasAddresses.resize(n, 0);

  for (size_t i = 0; i < n; ++i) {
    const BlasInput &in = blases[i];

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
    sizes.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    pfnGetBuildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                     &buildInfo, &primCount, &sizes);

    Buffer asBuf = createBuffer(allocator, sizes.accelerationStructureSize,
                                kAsStorageUsage, false);
    mBlasBuffers[i] = asBuf.buffer;
    mBlasAllocs[i] = asBuf.alloc;

    VkAccelerationStructureCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    ci.buffer = mBlasBuffers[i];
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VK_CHECK(pfnCreateAccel(device, &ci, nullptr, &mBlas[i]));

    Buffer scratch = createBuffer(allocator, sizes.buildScratchSize,
                                  kScratchUsage, false, mScratchAlignment);
    buildInfo.dstAccelerationStructure = mBlas[i];
    buildInfo.scratchData.deviceAddress = bufferAddress(device, scratch.buffer);

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primCount;
    const VkAccelerationStructureBuildRangeInfoKHR *pRange = &range;
    submit([&](VkCommandBuffer cmd) { pfnCmdBuild(cmd, 1, &buildInfo, &pRange); });
    vmaDestroyBuffer(allocator, scratch.buffer, scratch.alloc);

    VkAccelerationStructureDeviceAddressInfoKHR addrInfo{};
    addrInfo.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addrInfo.accelerationStructure = mBlas[i];
    mBlasAddresses[i] = pfnGetAccelAddress(device, &addrInfo);
  }

  std::fprintf(stderr, "[VulkanAccel] Built %zu BLAS\n", n);
  return true;
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
    std::fprintf(stderr,
                 "[VulkanAccel] %u instances exceed TLAS capacity %u; "
                 "dropping the rest\n",
                 count, mMaxInstances);
    count = mMaxInstances;
  }

  // Host-write this frame's instance buffer (the caller has already fenced
  // this frame slot).
  auto *vkInstances =
      static_cast<VkAccelerationStructureInstanceKHR *>(mInstanceMapped[frame]);
  for (uint32_t i = 0; i < count; ++i) {
    const InstanceInput &in = instances[i];
    VkAccelerationStructureInstanceKHR inst{};
    inst.transform = toVkTransform(in.transform);
    inst.instanceCustomIndex = i;
    inst.mask = 0xFF;
    inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    inst.accelerationStructureReference = mBlasAddresses[in.blasIndex];
    vkInstances[i] = inst;
  }

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
  const uint32_t capacity =
      std::max<uint32_t>(static_cast<uint32_t>(instances.size()) * 2, 256);
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

void VulkanAccel::destroy(VulkanContext &ctx) {
  VmaAllocator allocator = ctx.allocator();
  for (VkAccelerationStructureKHR tlas : mTlas)
    if (tlas)
      pfnDestroyAccel(ctx.device(), tlas, nullptr);
  for (VkAccelerationStructureKHR blas : mBlas)
    if (blas)
      pfnDestroyAccel(ctx.device(), blas, nullptr);
  for (size_t i = 0; i < mInstanceBuffers.size(); ++i)
    if (mInstanceBuffers[i])
      vmaDestroyBuffer(allocator, mInstanceBuffers[i], mInstanceAllocs[i]);
  for (size_t i = 0; i < mTlasBuffers.size(); ++i)
    if (mTlasBuffers[i])
      vmaDestroyBuffer(allocator, mTlasBuffers[i], mTlasAllocs[i]);
  for (size_t i = 0; i < mScratchBuffers.size(); ++i)
    if (mScratchBuffers[i])
      vmaDestroyBuffer(allocator, mScratchBuffers[i], mScratchAllocs[i]);
  for (size_t i = 0; i < mBlasBuffers.size(); ++i)
    if (mBlasBuffers[i])
      vmaDestroyBuffer(allocator, mBlasBuffers[i], mBlasAllocs[i]);
  mBlas.clear();
  mBlasBuffers.clear();
  mBlasAllocs.clear();
  mBlasAddresses.clear();
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

} // namespace vkrhi

#include "VulkanAccel.h"

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

} // namespace

bool VulkanAccel::build(VulkanContext &ctx, const SubmitFn &submit,
                        const std::vector<BlasInput> &blases,
                        const std::vector<InstanceInput> &instances) {
  VkDevice device = ctx.device();
  VmaAllocator allocator = ctx.allocator();

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

  const VkBufferUsageFlags asStorageUsage =
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  const VkBufferUsageFlags scratchUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

  // ---- One BLAS per mesh ----
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
                                asStorageUsage, false);
    mBlasBuffers[i] = asBuf.buffer;
    mBlasAllocs[i] = asBuf.alloc;

    VkAccelerationStructureCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    ci.buffer = mBlasBuffers[i];
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VK_CHECK(pfnCreateAccel(device, &ci, nullptr, &mBlas[i]));

    Buffer scratch = createBuffer(allocator, sizes.buildScratchSize,
                                  scratchUsage, false, mScratchAlignment);
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

  // ---- TLAS over the instances ----
  std::vector<VkAccelerationStructureInstanceKHR> vkInstances(instances.size());
  for (size_t i = 0; i < instances.size(); ++i) {
    const InstanceInput &in = instances[i];
    VkAccelerationStructureInstanceKHR &inst = vkInstances[i];
    inst = {};
    inst.transform = toVkTransform(in.transform);
    inst.instanceCustomIndex = static_cast<uint32_t>(i);
    inst.mask = 0xFF;
    inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    inst.accelerationStructureReference = mBlasAddresses[in.blasIndex];
  }

  const VkDeviceSize instBytes =
      sizeof(VkAccelerationStructureInstanceKHR) * vkInstances.size();
  Buffer instBuf = createBuffer(
      allocator, instBytes,
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      true);
  std::memcpy(instBuf.mapped, vkInstances.data(),
              static_cast<size_t>(instBytes));
  mInstanceBuffer = instBuf.buffer;
  mInstanceAlloc = instBuf.alloc;

  VkAccelerationStructureGeometryKHR tlasGeom{};
  tlasGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  tlasGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  tlasGeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
  tlasGeom.geometry.instances.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  tlasGeom.geometry.instances.arrayOfPointers = VK_FALSE;
  tlasGeom.geometry.instances.data.deviceAddress =
      bufferAddress(device, mInstanceBuffer);

  const uint32_t instCount = static_cast<uint32_t>(vkInstances.size());

  VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{};
  tlasBuild.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  tlasBuild.geometryCount = 1;
  tlasBuild.pGeometries = &tlasGeom;

  VkAccelerationStructureBuildSizesInfoKHR tlasSizes{};
  tlasSizes.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
  pfnGetBuildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                   &tlasBuild, &instCount, &tlasSizes);

  Buffer tlasBuf = createBuffer(allocator, tlasSizes.accelerationStructureSize,
                                asStorageUsage, false);
  mTlasBuffer = tlasBuf.buffer;
  mTlasAlloc = tlasBuf.alloc;

  VkAccelerationStructureCreateInfoKHR tlasCi{};
  tlasCi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
  tlasCi.buffer = mTlasBuffer;
  tlasCi.size = tlasSizes.accelerationStructureSize;
  tlasCi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  VK_CHECK(pfnCreateAccel(device, &tlasCi, nullptr, &mTlas));

  Buffer tlasScratch = createBuffer(allocator, tlasSizes.buildScratchSize,
                                    scratchUsage, false, mScratchAlignment);
  tlasBuild.dstAccelerationStructure = mTlas;
  tlasBuild.scratchData.deviceAddress =
      bufferAddress(device, tlasScratch.buffer);

  VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
  tlasRange.primitiveCount = instCount;
  const VkAccelerationStructureBuildRangeInfoKHR *pTlasRange = &tlasRange;
  submit([&](VkCommandBuffer cmd) {
    pfnCmdBuild(cmd, 1, &tlasBuild, &pTlasRange);
  });
  vmaDestroyBuffer(allocator, tlasScratch.buffer, tlasScratch.alloc);

  std::fprintf(stderr, "[VulkanAccel] Built %zu BLAS + TLAS (%u instances)\n", n,
               instCount);
  return true;
}

void VulkanAccel::destroy(VulkanContext &ctx) {
  VmaAllocator allocator = ctx.allocator();
  if (mTlas)
    pfnDestroyAccel(ctx.device(), mTlas, nullptr);
  for (VkAccelerationStructureKHR blas : mBlas)
    if (blas)
      pfnDestroyAccel(ctx.device(), blas, nullptr);
  if (mInstanceBuffer)
    vmaDestroyBuffer(allocator, mInstanceBuffer, mInstanceAlloc);
  if (mTlasBuffer)
    vmaDestroyBuffer(allocator, mTlasBuffer, mTlasAlloc);
  for (size_t i = 0; i < mBlasBuffers.size(); ++i)
    if (mBlasBuffers[i])
      vmaDestroyBuffer(allocator, mBlasBuffers[i], mBlasAllocs[i]);
  mBlas.clear();
  mBlasBuffers.clear();
  mBlasAllocs.clear();
  mBlasAddresses.clear();
  mTlas = VK_NULL_HANDLE;
  mInstanceBuffer = VK_NULL_HANDLE;
  mTlasBuffer = VK_NULL_HANDLE;
}

} // namespace vkrhi

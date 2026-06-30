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

} // namespace

bool VulkanAccel::build(VulkanContext &ctx, const SubmitFn &submit,
                        VkDeviceAddress vertexAddress, uint32_t vertexCount,
                        uint32_t vertexStride, VkDeviceAddress indexAddress,
                        uint32_t indexCount) {
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

  // Required scratch alignment.
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

  // ---- BLAS over the model triangles ----
  VkAccelerationStructureGeometryKHR blasGeom{};
  blasGeom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  blasGeom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  blasGeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
  blasGeom.geometry.triangles.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
  blasGeom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
  blasGeom.geometry.triangles.vertexData.deviceAddress = vertexAddress;
  blasGeom.geometry.triangles.vertexStride = vertexStride;
  blasGeom.geometry.triangles.maxVertex = vertexCount - 1;
  blasGeom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
  blasGeom.geometry.triangles.indexData.deviceAddress = indexAddress;

  const uint32_t blasPrimCount = indexCount / 3;

  VkAccelerationStructureBuildGeometryInfoKHR blasBuild{};
  blasBuild.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  blasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  blasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  blasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  blasBuild.geometryCount = 1;
  blasBuild.pGeometries = &blasGeom;

  VkAccelerationStructureBuildSizesInfoKHR blasSizes{};
  blasSizes.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
  pfnGetBuildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                   &blasBuild, &blasPrimCount, &blasSizes);

  Buffer blasBuf = createBuffer(allocator, blasSizes.accelerationStructureSize,
                                asStorageUsage, false);
  mBlasBuffer = blasBuf.buffer;
  mBlasAlloc = blasBuf.alloc;

  VkAccelerationStructureCreateInfoKHR blasCi{};
  blasCi.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
  blasCi.buffer = mBlasBuffer;
  blasCi.size = blasSizes.accelerationStructureSize;
  blasCi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  VK_CHECK(pfnCreateAccel(device, &blasCi, nullptr, &mBlas));

  Buffer blasScratch = createBuffer(allocator, blasSizes.buildScratchSize,
                                    scratchUsage, false, mScratchAlignment);
  blasBuild.dstAccelerationStructure = mBlas;
  blasBuild.scratchData.deviceAddress =
      bufferAddress(device, blasScratch.buffer);

  VkAccelerationStructureBuildRangeInfoKHR blasRange{};
  blasRange.primitiveCount = blasPrimCount;
  const VkAccelerationStructureBuildRangeInfoKHR *pBlasRange = &blasRange;
  submit([&](VkCommandBuffer cmd) {
    pfnCmdBuild(cmd, 1, &blasBuild, &pBlasRange);
  });
  vmaDestroyBuffer(allocator, blasScratch.buffer, blasScratch.alloc);

  VkAccelerationStructureDeviceAddressInfoKHR blasAddrInfo{};
  blasAddrInfo.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
  blasAddrInfo.accelerationStructure = mBlas;
  const VkDeviceAddress blasAddress =
      pfnGetAccelAddress(device, &blasAddrInfo);

  // ---- TLAS with a single identity instance ----
  VkAccelerationStructureInstanceKHR instance{};
  instance.transform.matrix[0][0] = 1.0f;
  instance.transform.matrix[1][1] = 1.0f;
  instance.transform.matrix[2][2] = 1.0f;
  instance.instanceCustomIndex = 0;
  instance.mask = 0xFF;
  instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
  instance.accelerationStructureReference = blasAddress;

  Buffer instBuf = createBuffer(
      allocator, sizeof(instance),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      true);
  std::memcpy(instBuf.mapped, &instance, sizeof(instance));
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

  const uint32_t tlasPrimCount = 1;

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
                   &tlasBuild, &tlasPrimCount, &tlasSizes);

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
  tlasRange.primitiveCount = tlasPrimCount;
  const VkAccelerationStructureBuildRangeInfoKHR *pTlasRange = &tlasRange;
  // Separate submit: the BLAS build above has already completed (fenced), so
  // the TLAS build safely reads it.
  submit([&](VkCommandBuffer cmd) {
    pfnCmdBuild(cmd, 1, &tlasBuild, &pTlasRange);
  });
  vmaDestroyBuffer(allocator, tlasScratch.buffer, tlasScratch.alloc);

  std::fprintf(stderr,
               "[VulkanAccel] Built BLAS (%u triangles) + TLAS (1 instance)\n",
               blasPrimCount);
  return true;
}

void VulkanAccel::destroy(VulkanContext &ctx) {
  VmaAllocator allocator = ctx.allocator();
  if (mTlas)
    pfnDestroyAccel(ctx.device(), mTlas, nullptr);
  if (mBlas)
    pfnDestroyAccel(ctx.device(), mBlas, nullptr);
  if (mInstanceBuffer)
    vmaDestroyBuffer(allocator, mInstanceBuffer, mInstanceAlloc);
  if (mTlasBuffer)
    vmaDestroyBuffer(allocator, mTlasBuffer, mTlasAlloc);
  if (mBlasBuffer)
    vmaDestroyBuffer(allocator, mBlasBuffer, mBlasAlloc);
  mTlas = VK_NULL_HANDLE;
  mBlas = VK_NULL_HANDLE;
  mInstanceBuffer = VK_NULL_HANDLE;
  mTlasBuffer = VK_NULL_HANDLE;
  mBlasBuffer = VK_NULL_HANDLE;
}

} // namespace vkrhi

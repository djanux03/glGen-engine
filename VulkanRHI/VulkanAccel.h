#pragma once

#include "VulkanContext.h"

#include <vk_mem_alloc.h>

#include <functional>

namespace vkrhi {

// Builds and owns the ray-tracing acceleration structures: a BLAS over the
// model's triangles and a single-instance TLAS. The TLAS is what shaders query
// (via ray queries) for hardware ray-traced shadows.
class VulkanAccel {
public:
  using SubmitFn =
      std::function<void(const std::function<void(VkCommandBuffer)> &)>;

  // Builds BLAS + TLAS from device addresses of the model's vertex/index
  // buffers (which must have been created with SHADER_DEVICE_ADDRESS +
  // ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY usage).
  bool build(VulkanContext &ctx, const SubmitFn &submit,
             VkDeviceAddress vertexAddress, uint32_t vertexCount,
             uint32_t vertexStride, VkDeviceAddress indexAddress,
             uint32_t indexCount);
  void destroy(VulkanContext &ctx);

  VkAccelerationStructureKHR tlas() const { return mTlas; }

private:
  PFN_vkGetAccelerationStructureBuildSizesKHR pfnGetBuildSizes = nullptr;
  PFN_vkCreateAccelerationStructureKHR pfnCreateAccel = nullptr;
  PFN_vkCmdBuildAccelerationStructuresKHR pfnCmdBuild = nullptr;
  PFN_vkGetAccelerationStructureDeviceAddressKHR pfnGetAccelAddress = nullptr;
  PFN_vkDestroyAccelerationStructureKHR pfnDestroyAccel = nullptr;

  VkAccelerationStructureKHR mBlas = VK_NULL_HANDLE;
  VkBuffer mBlasBuffer = VK_NULL_HANDLE;
  VmaAllocation mBlasAlloc = VK_NULL_HANDLE;

  VkAccelerationStructureKHR mTlas = VK_NULL_HANDLE;
  VkBuffer mTlasBuffer = VK_NULL_HANDLE;
  VmaAllocation mTlasAlloc = VK_NULL_HANDLE;

  VkBuffer mInstanceBuffer = VK_NULL_HANDLE;
  VmaAllocation mInstanceAlloc = VK_NULL_HANDLE;

  uint32_t mScratchAlignment = 256;
};

} // namespace vkrhi

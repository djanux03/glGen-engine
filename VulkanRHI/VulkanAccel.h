#pragma once

#include "VulkanContext.h"

#include <vk_mem_alloc.h>

#include <glm/glm.hpp>

#include <functional>
#include <vector>

namespace vkrhi {

// Builds and owns the ray-tracing acceleration structures: one BLAS per mesh
// and a single TLAS over a list of instances (each referencing a BLAS with its
// own transform). Shaders query the TLAS for ray-traced shadows.
class VulkanAccel {
public:
  using SubmitFn =
      std::function<void(const std::function<void(VkCommandBuffer)> &)>;

  struct BlasInput {
    VkDeviceAddress vertexAddress;
    uint32_t vertexCount;
    uint32_t vertexStride;
    VkDeviceAddress indexAddress;
    uint32_t indexCount;
  };
  struct InstanceInput {
    uint32_t blasIndex;
    glm::mat4 transform;
  };

  bool build(VulkanContext &ctx, const SubmitFn &submit,
             const std::vector<BlasInput> &blases,
             const std::vector<InstanceInput> &instances);
  void destroy(VulkanContext &ctx);

  VkAccelerationStructureKHR tlas() const { return mTlas; }

private:
  PFN_vkGetAccelerationStructureBuildSizesKHR pfnGetBuildSizes = nullptr;
  PFN_vkCreateAccelerationStructureKHR pfnCreateAccel = nullptr;
  PFN_vkCmdBuildAccelerationStructuresKHR pfnCmdBuild = nullptr;
  PFN_vkGetAccelerationStructureDeviceAddressKHR pfnGetAccelAddress = nullptr;
  PFN_vkDestroyAccelerationStructureKHR pfnDestroyAccel = nullptr;

  std::vector<VkAccelerationStructureKHR> mBlas;
  std::vector<VkBuffer> mBlasBuffers;
  std::vector<VmaAllocation> mBlasAllocs;
  std::vector<VkDeviceAddress> mBlasAddresses;

  VkAccelerationStructureKHR mTlas = VK_NULL_HANDLE;
  VkBuffer mTlasBuffer = VK_NULL_HANDLE;
  VmaAllocation mTlasAlloc = VK_NULL_HANDLE;
  VkBuffer mInstanceBuffer = VK_NULL_HANDLE;
  VmaAllocation mInstanceAlloc = VK_NULL_HANDLE;

  uint32_t mScratchAlignment = 256;
};

} // namespace vkrhi

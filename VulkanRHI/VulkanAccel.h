#pragma once

#include "VulkanContext.h"

#include <vk_mem_alloc.h>

#include <glm/glm.hpp>

#include <functional>
#include <vector>

namespace vkrhi {

// Builds and owns the ray-tracing acceleration structures: one BLAS per mesh
// (static geometry, built once) and one TLAS per frame in flight so instance
// transforms can change every frame without racing frames still on the GPU.
// Shaders query the TLAS for ray-traced shadows.
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

  // Builds one BLAS per mesh (immediate submit; call once per scene).
  bool buildBlas(VulkanContext &ctx, const SubmitFn &submit,
                 const std::vector<BlasInput> &blases);

  // Creates per-frame TLAS objects + instance/scratch buffers sized for
  // maxInstances. The TLASes are unbuilt until the first recordTlasBuild.
  bool createTlas(VulkanContext &ctx, uint32_t framesInFlight,
                  uint32_t maxInstances);

  // Records a full TLAS rebuild for `frame` into `cmd` (host-writes the
  // frame's instance buffer, then a build + the barrier that makes it
  // visible to fragment-shader ray queries). Instances beyond maxInstances()
  // are dropped with a warning.
  void recordTlasBuild(VkCommandBuffer cmd, uint32_t frame,
                       const std::vector<InstanceInput> &instances);

  // Convenience for a static scene: BLAS + TLAS built immediately.
  bool build(VulkanContext &ctx, const SubmitFn &submit,
             const std::vector<BlasInput> &blases,
             const std::vector<InstanceInput> &instances,
             uint32_t framesInFlight);

  void destroy(VulkanContext &ctx);

  VkAccelerationStructureKHR tlas(uint32_t frame) const {
    return mTlas.empty() ? VK_NULL_HANDLE : mTlas[frame % mTlas.size()];
  }
  uint32_t maxInstances() const { return mMaxInstances; }

private:
  bool loadFunctions(VulkanContext &ctx);

  PFN_vkGetAccelerationStructureBuildSizesKHR pfnGetBuildSizes = nullptr;
  PFN_vkCreateAccelerationStructureKHR pfnCreateAccel = nullptr;
  PFN_vkCmdBuildAccelerationStructuresKHR pfnCmdBuild = nullptr;
  PFN_vkGetAccelerationStructureDeviceAddressKHR pfnGetAccelAddress = nullptr;
  PFN_vkDestroyAccelerationStructureKHR pfnDestroyAccel = nullptr;

  VulkanContext *mCtx = nullptr;

  std::vector<VkAccelerationStructureKHR> mBlas;
  std::vector<VkBuffer> mBlasBuffers;
  std::vector<VmaAllocation> mBlasAllocs;
  std::vector<VkDeviceAddress> mBlasAddresses;

  // Per frame in flight.
  std::vector<VkAccelerationStructureKHR> mTlas;
  std::vector<VkBuffer> mTlasBuffers;
  std::vector<VmaAllocation> mTlasAllocs;
  std::vector<VkBuffer> mInstanceBuffers;
  std::vector<VmaAllocation> mInstanceAllocs;
  std::vector<void *> mInstanceMapped;
  std::vector<VkBuffer> mScratchBuffers;
  std::vector<VmaAllocation> mScratchAllocs;
  std::vector<VkDeviceAddress> mScratchAddresses;
  std::vector<VkDeviceAddress> mInstanceAddresses;

  uint32_t mMaxInstances = 0;
  uint32_t mScratchAlignment = 256;
};

} // namespace vkrhi

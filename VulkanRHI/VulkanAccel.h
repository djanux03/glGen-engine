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

  // Resources whose GPU lifetime the CALLER must extend past in-flight
  // frames: recordBlasBuildAt()/releaseBlas() push retired buffers and
  // acceleration structures here instead of destroying them, and the caller
  // frees them once the frame that last referenced them has fenced (the
  // renderer keeps one such list per frame in flight).
  struct PendingGarbage {
    std::vector<std::pair<VkBuffer, VmaAllocation>> buffers;
    std::vector<VkAccelerationStructureKHR> accels;
  };
  // Frees everything in `garbage` immediately. Only call once the GPU can no
  // longer reference any of it (frame fence signaled, or after waitIdle).
  void freeGarbage(VulkanContext &ctx, PendingGarbage &garbage);

  // Builds one BLAS per mesh (immediate submit; call once per scene).
  bool buildBlas(VulkanContext &ctx, const SubmitFn &submit,
                 const std::vector<BlasInput> &blases);

  // Builds BLAS only for `newBlases`, appending to the existing set built by
  // buildBlas()/a prior appendBlas() call. Does not touch or rebuild any
  // existing entry, and does not call vkDeviceWaitIdle -- safe to call every
  // frame that new meshes appear, unlike destroy()+buildBlas(). If nothing
  // has been built yet, behaves like buildBlas().
  bool appendBlas(VulkanContext &ctx, const SubmitFn &submit,
                  const std::vector<BlasInput> &newBlases);

  // Records a BLAS (re)build for slot `index` into `cmd` -- the streaming
  // counterpart to buildBlas()/appendBlas()'s blocking immediate submits.
  // Grows the BLAS arrays if `index` is past the end (slot reuse after
  // releaseBlas() keeps mesh handle == BLAS index stable). Any existing AS
  // at the slot plus the build's scratch buffer are pushed to `garbage`
  // rather than destroyed -- in-flight frames' TLASes may still reference
  // the old AS by device address. The caller owns the barrier between the
  // vertex/index copies this build reads and the build itself, and between
  // this build and any TLAS build recorded after it in the same cmd.
  bool recordBlasBuildAt(VulkanContext &ctx, VkCommandBuffer cmd, size_t index,
                         const BlasInput &input, PendingGarbage &garbage);

  // Marks slot `index` empty (address 0 -- instances must stop referencing
  // it) and queues its AS + buffer on `garbage`. The slot can be rebuilt
  // later via recordBlasBuildAt() with the same index.
  void releaseBlas(size_t index, PendingGarbage &garbage);

  // True if the slot currently holds a live BLAS (nonzero device address).
  bool blasValid(size_t index) const {
    return index < mBlasAddresses.size() && mBlasAddresses[index] != 0;
  }

  // Creates per-frame TLAS objects + instance/scratch buffers sized for
  // maxInstances. The TLASes are unbuilt until the first recordTlasBuild.
  bool createTlas(VulkanContext &ctx, uint32_t framesInFlight,
                  uint32_t maxInstances);

  // Destroys and recreates every frame's TLAS resources at a larger
  // capacity, leaving the BLAS set untouched. The caller must have idled the
  // device (in-flight frames reference the old TLAS objects) and must
  // re-write its TLAS descriptor sets afterwards; every frame slot needs a
  // fresh recordTlasBuild() before its next use.
  bool recreateTlas(VulkanContext &ctx, uint32_t framesInFlight,
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
  size_t blasCount() const { return mBlas.size(); }

private:
  bool loadFunctions(VulkanContext &ctx);
  void destroyTlasResources(VulkanContext &ctx);

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
  // Rate-limits the capacity-overflow warning in recordTlasBuild(): only
  // printed when the dropped count grows past the last warned value.
  uint32_t mWarnedOverflowCount = 0;
};

} // namespace vkrhi

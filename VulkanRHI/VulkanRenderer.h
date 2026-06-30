#pragma once

#include "VulkanAccel.h"
#include "VulkanBindless.h"
#include "VulkanMesh.h"
#include "VulkanPipelineCache.h"
#include "VulkanSwapchain.h"

#include <vk_mem_alloc.h>

#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vkrhi {

// Phase 2 renderer. Frame = shadow pass (cascaded depth) -> scene pass (HDR,
// shadowed) -> tonemap pass (swapchain). Offscreen targets are per-frame-in-
// flight so the two in-flight frames never alias.
class VulkanRenderer {
public:
  bool init(VulkanContext &ctx, VkSurfaceKHR surface,
            const std::string &shaderDir,
            std::function<void(uint32_t &, uint32_t &)> queryFramebufferSize);

  // --- engine-drivable scene API ---------------------------------------
  // Build the scene after init(): create meshes, place instances, then call
  // finalizeScene() to build the ray-tracing acceleration structures.
  using MeshHandle = uint32_t;
  MeshHandle createMeshFromObj(const std::string &path);
  void addInstance(MeshHandle mesh, const glm::mat4 &transform);
  bool finalizeScene();

  void drawFrame();
  void waitIdle();
  void shutdown();

  // One-shot debug capture: the next drawFrame copies the presented image to a
  // PNG at `path`. Used to visually verify rendering headlessly.
  void requestCapture(const std::string &path);

  // Live, UI/input-driven render parameters.
  struct Params {
    // Free-fly camera (driven by mouse-look / scroll / WASD in the app).
    glm::vec3 camPos = glm::vec3(0.0f, 0.7f, 3.0f);
    float camYawDeg = 180.0f;   // facing -Z toward the origin
    float camPitchDeg = -8.0f;
    float fovDeg = 55.0f;
    // Lighting / rendering.
    float exposure = 1.1f;
    float lightYawDeg = 215.0f;
    float lightPitchDeg = 50.0f;
    // Terrain generator.
    bool drawTerrain = true;
    float terrainAmplitude = 1.3f;
    float terrainFrequency = 0.35f;
    float terrainOctaves = 4.0f;
    float terrainSeed = 0.0f;
  };
  Params &params() { return mParams; }

  // Records an overlay (e.g. ImGui) inside the tonemap pass, on the swapchain.
  void setOverlayCallback(std::function<void(VkCommandBuffer)> cb) {
    mOverlay = std::move(cb);
  }
  VkFormat swapchainColorFormat() const { return mSwapchain.imageFormat(); }
  uint32_t swapchainImageCount() const { return mSwapchain.imageCount(); }

private:
  static constexpr uint32_t kFramesInFlight = 2;
  static constexpr uint32_t kTerrainPatches = 24; // grid is kTerrainPatches^2

  struct FrameDataGpu {
    glm::mat4 viewProj;
    glm::mat4 view;
    glm::vec4 lightDir; // xyz world-space
    glm::vec4 terrain;  // x=amplitude, y=frequency, z=octaves, w=seed
  };

  struct DrawItem {
    uint32_t indexOffset;
    uint32_t indexCount;
    uint32_t textureIndex;
  };

  // A loaded mesh: device-local geometry + its per-material submesh draws.
  struct Mesh {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAlloc = VK_NULL_HANDLE;
    std::vector<DrawItem> drawItems;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
  };

  // A placed object: which mesh + its world transform.
  struct Instance {
    uint32_t meshIndex;
    glm::mat4 model;
  };

  bool createCommandPool();
  bool createDescriptorsAndFrameData();
  bool createSampler();
  bool createSceneTargets();
  void destroySceneTargets();
  bool createTlasDescriptors();
  void writeTlasDescriptors();
  bool createTonemapResources();
  void updateTonemapSets();
  bool createScenePipeline(const std::string &shaderDir);
  bool createTerrainPipeline(const std::string &shaderDir);
  bool createSkyPipeline(const std::string &shaderDir);
  bool createTonemapPipeline(const std::string &shaderDir);
  bool loadMeshFromObj(const std::string &path, Mesh &outMesh);
  bool createSyncObjects();
  void recreateSwapchain();

  uint32_t addTexture(const uint8_t *rgba, uint32_t w, uint32_t h,
                      VkFormat format);
  uint32_t loadTextureFile(const std::string &path);
  uint32_t createDefaultTexture();

  VkShaderModule loadShaderModule(const std::string &path);
  void immediateSubmit(const std::function<void(VkCommandBuffer)> &record);
  void createDeviceLocalBuffer(const void *data, VkDeviceSize size,
                               VkBufferUsageFlags usage, VkBuffer &outBuffer,
                               VmaAllocation &outAlloc);

  VulkanContext *mCtx = nullptr;
  VkSurfaceKHR mSurface = VK_NULL_HANDLE;
  VulkanSwapchain mSwapchain;
  std::function<void(uint32_t &, uint32_t &)> mQueryFbSize;

  // --- foundations ---
  VulkanBindless mBindless;
  VulkanPipelineCache mPipelineCache;
  VkDescriptorSetLayout mFrameSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool mFrameDescPool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> mFrameSets;
  std::vector<VkBuffer> mFrameUBOs;
  std::vector<VmaAllocation> mFrameUBOAllocs;
  std::vector<void *> mFrameUBOMapped;

  // --- textures (bindless) ---
  VkSampler mSampler = VK_NULL_HANDLE;
  std::vector<VkImage> mTextureImages;
  std::vector<VmaAllocation> mTextureAllocs;
  std::vector<VkImageView> mTextureViews;
  uint32_t mDefaultTexIndex = 0;

  // --- offscreen scene targets (per frame in flight) ---
  VkFormat mHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  std::vector<VkImage> mHdrImages;
  std::vector<VmaAllocation> mHdrAllocs;
  std::vector<VkImageView> mHdrViews;
  VkFormat mDepthFormat = VK_FORMAT_D32_SFLOAT;
  std::vector<VkImage> mDepthImages;
  std::vector<VmaAllocation> mDepthAllocs;
  std::vector<VkImageView> mDepthViews;

  // --- ray tracing: acceleration structures + TLAS descriptor (set 2) ---
  VulkanAccel mAccel;
  VkDescriptorSetLayout mTlasSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool mTlasPool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> mTlasSets; // per frame in flight (same TLAS)

  // --- scene (mesh) pipeline ---
  VkPipelineLayout mScenePipelineLayout = VK_NULL_HANDLE;
  VkPipeline mScenePipeline = VK_NULL_HANDLE;

  // --- procedural sky (fullscreen, push-constant only) ---
  VkPipelineLayout mSkyPipelineLayout = VK_NULL_HANDLE;
  VkPipeline mSkyPipeline = VK_NULL_HANDLE;

  // --- terrain (mesh-shader) pipeline; reuses the scene pipeline layout ---
  VkPipeline mTerrainPipeline = VK_NULL_HANDLE;
  PFN_vkCmdDrawMeshTasksEXT mDrawMeshTasks = nullptr;
  uint32_t mTerrainTexIndex = 0;

  // --- tonemap pipeline ---
  VkDescriptorSetLayout mTonemapSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool mTonemapPool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> mTonemapSets;
  VkPipelineLayout mTonemapPipelineLayout = VK_NULL_HANDLE;
  VkPipeline mTonemapPipeline = VK_NULL_HANDLE;

  // --- mesh (device-local) ---
  std::vector<Mesh> mMeshes;
  std::vector<Instance> mInstances;

  // --- frame loop ---
  VkCommandPool mCommandPool = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> mCommandBuffers;
  std::vector<VkSemaphore> mImageAvailable;
  std::vector<VkFence> mInFlight;
  std::vector<VkSemaphore> mRenderFinished;

  uint32_t mCurrentFrame = 0;
  std::chrono::steady_clock::time_point mStartTime;

  bool mCapture = false;
  std::string mCapturePath;

  Params mParams;
  std::function<void(VkCommandBuffer)> mOverlay;
};

} // namespace vkrhi

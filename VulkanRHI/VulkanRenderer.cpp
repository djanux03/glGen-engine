#include <filesystem>
#include "VulkanRenderer.h"

#include "Assets/MeshData.h" // engine CPU model data (::MeshData)
#include "Rendering/Material.h" // MaterialAsset
#include "Rendering/SkyAtmosphere.h"

#include <glm/gtc/matrix_transform.hpp>

#include "stb_image.h"
#include "stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace vkrhi {

namespace {

// Mirrors sky.frag's push block. Drives BOTH the visible fullscreen sky and
// the six per-frame environment-cubemap faces (the env pass zeroes the
// disc/star params so IBL doesn't double-count the analytic direct light).
struct SkyPush {
  glm::mat4 invViewProj;
  glm::vec4 sunDir;  // xyz = TOWARD sun; w = sun outer radiance scale
  glm::vec4 moonDir; // xyz = TOWARD moon; w = moon outer radiance scale
  glm::vec4 camPos;  // xyz; w = timeSeconds (star twinkle)
  glm::vec4 passFlags; // x=environment cubemap pass, y=moon angular radius
};
static_assert(sizeof(SkyPush) == 128, "SkyPush must fit Vulkan minimum");

struct VolumetricPush {
  glm::mat4 invViewProj;
};

struct VolCompositePush {
  glm::mat4 invViewProj;
  glm::vec4 camPosMaxDist; // xyz = camera world pos, w = volumetric max dist
};

// Mirrors clouds.frag / cloudComposite.frag. Camera position, sun direction
// and the clock all already live in FrameData (which the march binds anyway
// for the atmosphere and its own dials), so only the world<-clip matrix and
// the per-frame dither phase need pushing.
struct CloudPush {
  glm::mat4 invViewProj;
  glm::vec4 jitter; // x = dither phase; yzw reserved
};

// Water needs world<-clip to build its view ray and clip<-world to project
// SSR march samples back to screen. Only the inverse goes here: the forward
// viewProj, the camera position and the clock are all already in FrameData,
// which water.frag binds anyway for fog and sun radiance.
struct WaterPush {
  glm::mat4 invViewProj;
};

// --- CPU mirror of skyModel.glsl's sun transmittance -----------------------
// Packs FrameDataGpu.sunRadiance each frame so every lit shader gets the
// atmosphere-colored direct light without evaluating the atmosphere itself.
// Constants and the Chapman approximation MUST stay in sync with
// skyModel.glsl (the GLSL side owns the derivation comments).
namespace atm {
constexpr float kRg = 6371e3f, kHr = 8500.0f, kHm = 1200.0f;
constexpr glm::vec3 kBetaR{5.802e-6f, 13.558e-6f, 33.1e-6f};
constexpr glm::vec3 kBetaO{1.15e-6f, 3.32e-6f, 0.16e-6f};
constexpr float kMieAbsorb = 1.11f;
// The sun's top-of-atmosphere radiance at Params::sunIntensity == 1 -- the
// single anchor the whole HDR scale hangs off (sky in-scatter, IBL, fog and
// volumetrics all inherit it through the shared model).
constexpr float kSunOuterRadiance = 20.0f;
// The atmosphere model's value above is a radiance chosen to give the sky
// and visible solar disc useful HDR headroom.  A directional BRDF, however,
// expects incident irradiance; feeding the disc-radiance number straight to
// every surface makes ordinary diffuse ground land several stops above white
// before bloom/tonemapping (and produces the large clipped terrain patches
// seen at noon).  Keep the atmosphere at its existing scale, but calibrate
// the analytic surface light separately.
constexpr float kSurfaceIrradianceFromSunRadiance = 0.25f;
// Moonlight: full moon, opposite the sun, cool-shifted. ~1/250 of sunlight
// (games cheat WAY up from the real 1/400000 so night is playable).
constexpr glm::vec3 kMoonTint{0.72f, 0.82f, 1.0f};
constexpr float kMoonOuterFactor = 0.004f;

inline float betaMie(float haze) {
  return glm::mix(2.0e-6f, 2.4e-5f, haze * haze);
}

inline float chapman(float x, float cosChi) {
  const float c = std::sqrt(1.57079632679f * x);
  if (cosChi >= 0.0f)
    return c / ((c - 1.0f) * cosChi + 1.0f);
  const float sinChi =
      std::sqrt(glm::clamp(1.0f - cosChi * cosChi, 0.0f, 1.0f));
  const float x0 = x * sinChi;
  const float c0 = std::sqrt(1.57079632679f * x0);
  return 2.0f * c0 * std::exp(std::min(x - x0, 80.0f)) -
         c / ((c - 1.0f) * (-cosChi) + 1.0f);
}

// Transmittance from ground level toward a light at elevation sinEl
// (= toLight.y). Mirrors atmTransmittanceToSpace at h ~ 0.
inline glm::vec3 transmittance(float sinEl, float haze) {
  const float amR = kHr * chapman(kRg / kHr, sinEl);
  const float amM = kHm * chapman(kRg / kHm, sinEl);
  const glm::vec3 tau =
      (kBetaR + kBetaO) * amR + glm::vec3(betaMie(haze) * kMieAbsorb) * amM;
  return glm::exp(-tau);
}
} // namespace atm

struct TonemapPush {
  float exposure;
  float gamma;
  float saturation;
  float contrast;
  float vignette;
  int tonemapMode;
  float bloomIntensity;
  // R3 camera grade: small, smoothed exposure bias (stops, applied as
  // exposure *= exp2(bias)) + a white-balance tint multiplied onto linear
  // HDR before the tonemap curve. Both 0/white when a debug view is active
  // or cameraGradeEnabled is false (see drawFrame()). gradeTint is vec4
  // (rgb tint, w unused) rather than vec3 -- a lone trailing vec3 in a
  // push-constant block is ambiguous between C++ and std140-ish GLSL
  // alignment; every other push struct in this file (see SkyPush) already
  // sidesteps that by only ever using vec4. NOTE: adding a bare scalar
  // field here (instead of into an existing vec4's spare lane) previously
  // broke this exact invariant -- see gradeTint.w's reuse for
  // bloomWideIntensity below instead of a new trailing float.
  float gradeExposureBias;
  glm::vec4 gradeTint; // rgb = white-balance tint, w = bloomWideIntensity
  glm::mat4 invProj;
};

struct SSAOPush {
  glm::mat4 invProj; // NDC+depth -> view-space position
  glm::mat4 proj;    // view-space -> NDC, to re-project kernel samples
  float radius;
  float bias;
  float strength;
  float pad;
};

struct BlurPush {
  glm::mat4 invProj; // NDC+depth -> view-space position (bilateral weight)
};

struct BloomExtractPush {
  float threshold;
  float knee;
};

struct BloomBlurPush {
  glm::vec2 direction; // (1,0) horizontal pass, (0,1) vertical pass
};

// Scene / terrain push constant. model is per-instance (vertex stage);
// everything else selects bindless material data (fragment stage only).
// mesh.vert/terrain.frag only declare the model+textureIndex prefix they
// actually use; mesh.frag declares the rest for the PBR BRDF.
struct ScenePush {
  glm::mat4 model;
  uint32_t textureIndex;
  uint32_t roughnessIndex;
  uint32_t metallicIndex;
  uint32_t aoIndex;
  uint32_t normalIndex;
  uint32_t opacityIndex;
  float roughnessScalar;
  float metallicScalar;
  float aoScalar;
  float alphaCutoff;
  uint32_t materialFlags;
  // R5 vegetation wind, read by meshInstanced.vert (the VERTEX stage -- the
  // only push-constant member besides `model` that is). Every other
  // consumer of this layout leaves them zero, and a zero strength
  // short-circuits the sway, so no other shader needs to declare them.
  float windStrength;
  float windSpeed;
  float windMeshHeight;
  float groundOcclusion;
  float foliageSssStrength;
};
// Exactly the 128 bytes Vulkan guarantees. New per-material state belongs in
// a buffer, not here: normal/opacity support intentionally consumes the last
// spare lanes rather than silently exceeding low-end device limits.
static_assert(sizeof(ScenePush) == 128, "ScenePush layout must stay at 128 bytes");

void imageBarrier(VkCommandBuffer cmd, VkImage image,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                  VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                  uint32_t layerCount = 1, uint32_t baseMip = 0,
                  uint32_t mipCount = 1) {
  VkImageMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  barrier.srcStageMask = srcStage;
  barrier.srcAccessMask = srcAccess;
  barrier.dstStageMask = dstStage;
  barrier.dstAccessMask = dstAccess;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = aspect;
  barrier.subresourceRange.baseMipLevel = baseMip;
  barrier.subresourceRange.levelCount = mipCount;
  barrier.subresourceRange.layerCount = layerCount;

  VkDependencyInfo dep{};
  dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

bool VulkanRenderer::init(
    VulkanContext &ctx, VkSurfaceKHR surface, const std::string &shaderDir,
    std::function<void(uint32_t &, uint32_t &)> queryFramebufferSize,
    const std::string &assetDir) {
  mCtx = &ctx;
  mSurface = surface;
  mQueryFbSize = std::move(queryFramebufferSize);
  mStartTime = std::chrono::steady_clock::now();

  uint32_t w = 0, h = 0;
  mQueryFbSize(w, h);
  if (!mSwapchain.create(ctx, surface, w, h)) {
    std::fprintf(stderr, "[VulkanRHI] Initial swapchain creation failed.\n");
    return false;
  }
  if (!mPipelineCache.init(ctx, "vk_pipeline_cache.bin"))
    return false;
  if (!createCommandPool())
    return false;
  if (!createDescriptorsAndFrameData())
    return false;
  if (!createSampler())
    return false;
  mDefaultTexIndex = createDefaultTexture();
  if (!createSceneTargets())
    return false;
  if (!createTlasDescriptors())
    return false;
  if (!createTonemapResources())
    return false;
  if (!createTemporalResources())
    return false;
  updateTemporalSets();
  updateTonemapSets();
  // Before createScenePipeline: the scene pipeline layout's 4th descriptor
  // set (AO texture) is mAOSamplerSetLayout, created here.
  if (!createSSAOResources())
    return false;
  updateSSAOSets();
  // Reuses mAOSamplerSetLayout from createSSAOResources() above.
  // Env cubemap + volumetric sampler sets also reuse mAOSamplerSetLayout;
  // the volumetric scatter image itself lives in createSceneTargets().
  if (!createEnvMapResources())
    return false;
  // Cloud noise must load before createCloudPipelines (its descriptor set
  // layout is part of the march's pipeline layout) and before updateCloudSets
  // (which needs the pool the load allocates from).
  if (!createCloudNoiseResources(assetDir))
    return false;
  updateCloudSets();
  if (!createScenePipeline(shaderDir))
    return false;
  if (!createTerrainChunkPipeline(shaderDir))
    return false;
  if (!createVegetationPipeline(shaderDir))
    return false;
  if (!createDepthPrepassPipelines(shaderDir))
    return false;
  if (!mAtmosphere.init(ctx, shaderDir, mFrameSetLayout, mTlasSetLayout, mEnvironmentSetLayout,mCloudNoiseSetLayout))
    return false;
  if(!mCloudHistory.init(ctx,shaderDir))return false;
  if(!mExposureMeter.init(ctx,shaderDir))return false;
  if (!createSkyPipeline(shaderDir))
    return false;
  if (!createEnvMapPipeline(shaderDir))
    return false;
  if (!createWaterPipeline(shaderDir))
    return false;
  if (!createCloudPipelines(shaderDir))
    return false;
  if (!createSSAOPipeline(shaderDir))
    return false;
  if (!createBlurPipeline(shaderDir))
    return false;
  if (!createLinePipeline(shaderDir))
    return false;
  if (!createTonemapPipeline(shaderDir))
    return false;
  if (!createTemporalPipeline(shaderDir))
    return false;
  // Scene is supplied by the caller via createMeshFromObj()/addInstance() and
  // finalized with finalizeScene() before the first drawFrame().
  if (!createSyncObjects())
    return false;
  return true;
}

bool VulkanRenderer::createCommandPool() {
  VkCommandPoolCreateInfo poolCi{};
  poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolCi.queueFamilyIndex = mCtx->graphicsQueueFamily();
  VK_CHECK(
      vkCreateCommandPool(mCtx->device(), &poolCi, nullptr, &mCommandPool));
  return true;
}

void VulkanRenderer::immediateSubmit(
    const std::function<void(VkCommandBuffer)> &record) {
  VkCommandBufferAllocateInfo allocCi{};
  allocCi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocCi.commandPool = mCommandPool;
  allocCi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocCi.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateCommandBuffers(mCtx->device(), &allocCi, &cmd));

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
  record(cmd);
  VK_CHECK(vkEndCommandBuffer(cmd));

  VkCommandBufferSubmitInfo cmdInfo{};
  cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  cmdInfo.commandBuffer = cmd;
  VkSubmitInfo2 submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &cmdInfo;

  VkFenceCreateInfo fenceCi{};
  fenceCi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  VK_CHECK(vkCreateFence(mCtx->device(), &fenceCi, nullptr, &fence));
  VK_CHECK(vkQueueSubmit2(mCtx->graphicsQueue(), 1, &submit, fence));
  VK_CHECK(vkWaitForFences(mCtx->device(), 1, &fence, VK_TRUE, UINT64_MAX));
  vkDestroyFence(mCtx->device(), fence, nullptr);
  vkFreeCommandBuffers(mCtx->device(), mCommandPool, 1, &cmd);
}

void VulkanRenderer::createDeviceLocalBuffer(const void *data, VkDeviceSize size,
                                             VkBufferUsageFlags usage,
                                             VkBuffer &outBuffer,
                                             VmaAllocation &outAlloc) {
  VkBufferCreateInfo stagingCi{};
  stagingCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  stagingCi.size = size;
  stagingCi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  stagingCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo stagingAlloc{};
  stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
  stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VkBuffer staging = VK_NULL_HANDLE;
  VmaAllocation stagingMem = VK_NULL_HANDLE;
  VmaAllocationInfo stagingInfo{};
  VK_CHECK(vmaCreateBuffer(mCtx->allocator(), &stagingCi, &stagingAlloc,
                           &staging, &stagingMem, &stagingInfo));
  std::memcpy(stagingInfo.pMappedData, data, static_cast<size_t>(size));

  VkBufferCreateInfo bufferCi{};
  bufferCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferCi.size = size;
  bufferCi.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bufferCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo deviceAlloc{};
  deviceAlloc.usage = VMA_MEMORY_USAGE_AUTO;
  VK_CHECK(vmaCreateBuffer(mCtx->allocator(), &bufferCi, &deviceAlloc,
                           &outBuffer, &outAlloc, nullptr));

  if (mSceneReady) {
    // Streaming path (terrain chunks arriving while the app runs): queue the
    // copy for drawFrame() to record into the frame's own command buffer.
    // The old behavior -- one blocking submit + fence wait PER BUFFER -- cost
    // a dozen full CPU<->GPU sync round-trips per frame while flying and was
    // the primary source of streaming hitches. The staging buffer rides the
    // deferred-garbage path so it outlives the frame that copies from it.
    mPendingCopies.push_back({staging, outBuffer, size});
    mPendingGarbage.buffers.emplace_back(staging, stagingMem);
    return;
  }

  immediateSubmit([&](VkCommandBuffer cmd) {
    VkBufferCopy copy{};
    copy.size = size;
    vkCmdCopyBuffer(cmd, staging, outBuffer, 1, &copy);
  });
  vmaDestroyBuffer(mCtx->allocator(), staging, stagingMem);
}

// Records every queued staging->device copy via one immediate submit and
// clears the queue -- for the rare paths that need buffer contents valid
// BEFORE the next drawFrame() (finalizeScene()'s blocking BLAS builds).
void VulkanRenderer::flushPendingCopiesImmediate() {
  if (mPendingCopies.empty())
    return;
  immediateSubmit([&](VkCommandBuffer cmd) {
    for (const PendingCopy &pc : mPendingCopies) {
      VkBufferCopy copy{};
      copy.size = pc.size;
      vkCmdCopyBuffer(cmd, pc.src, pc.dst, 1, &copy);
    }
  });
  mPendingCopies.clear();
  // Staging buffers stay in mPendingGarbage; freed on the normal schedule.
}

VulkanRenderer::MeshHandle VulkanRenderer::acquireMeshSlot(Mesh &&mesh) {
  ++mRtAlphaVersion;
  if (!mFreeMeshSlots.empty()) {
    const MeshHandle slot = mFreeMeshSlots.back();
    mFreeMeshSlots.pop_back();
    mMeshes[slot] = std::move(mesh);
    return slot;
  }
  mMeshes.push_back(std::move(mesh));
  return static_cast<MeshHandle>(mMeshes.size() - 1);
}

bool VulkanRenderer::createSampler() {
  VkSamplerCreateInfo samplerCi{};
  samplerCi.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerCi.magFilter = VK_FILTER_LINEAR;
  samplerCi.minFilter = VK_FILTER_LINEAR;
  samplerCi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  samplerCi.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerCi.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerCi.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerCi.maxLod = VK_LOD_CLAMP_NONE;
  VK_CHECK(vkCreateSampler(mCtx->device(), &samplerCi, nullptr, &mSampler));

  // Material sampler: same addressing, plus anisotropy. Ground textures are
  // seen at grazing angles almost everywhere in a terrain game; isotropic
  // trilinear picks the mip from the LONG axis of the pixel footprint and
  // blurs a meadow into a flat smear a few metres from the camera.
  VkPhysicalDeviceFeatures feats{};
  vkGetPhysicalDeviceFeatures(mCtx->physicalDevice(), &feats);
  if (feats.samplerAnisotropy) {
    samplerCi.anisotropyEnable = VK_TRUE;
    samplerCi.maxAnisotropy =
        std::min(16.0f, mCtx->properties().limits.maxSamplerAnisotropy);
  }
  VK_CHECK(vkCreateSampler(mCtx->device(), &samplerCi, nullptr,
                           &mMaterialSampler));
  return true;
}

namespace {
// Software opacity micromaps (after VK_EXT_opacity_micromap, which Ampere and
// older lack). For each alpha-tested triangle, a 16x16 barycentric
// subdivision gives 256 micro-triangles; each gets one bit: does the
// material's alpha pass at that spot. A shadow ray's candidate hit then
// costs one bit test instead of fetching three indices, three UVs and a
// texel through buffer references -- measured at ~28 ms/frame in a spruce
// forest with the direct lookup, versus ~1 ms for a free test. The mask is
// built from a coarse COVERAGE grid rather than point samples, because a
// micro-triangle on a needle card spans many needles; a shadow needs their
// silhouette, which is what the eye resolves from a cast shadow anyway.
//
// Micro-triangle numbering (shared with surfaceShadow.glsl): with
// x = b1*N, y = b2*N, row j = floor(y), column i = floor(x), and "upper"
// when fract(x)+fract(y) > 1, index = 2Nj - j^2 + 2i + upper.
void buildTriangleMicromap(const glm::vec2 uv[3], const std::vector<float> &cov,
                           uint32_t gridW, uint32_t gridH, uint32_t *words) {
  constexpr uint32_t N = 16;
  const auto sample = [&](glm::vec2 t) {
    t -= glm::floor(t); // REPEAT addressing, like the material sampler
    const uint32_t gx = std::min(static_cast<uint32_t>(t.x * gridW), gridW - 1);
    const uint32_t gy = std::min(static_cast<uint32_t>(t.y * gridH), gridH - 1);
    return cov[static_cast<size_t>(gy) * gridW + gx];
  };
  for (uint32_t w = 0; w < N * N / 32; ++w)
    words[w] = 0;
  for (uint32_t j = 0; j < N; ++j)
    for (uint32_t i = 0; i + j < N; ++i)
      for (uint32_t upper = 0; upper < 2; ++upper) {
        if (upper && i + j + 2 > N)
          continue;
        const float x = (static_cast<float>(i) + (upper ? 2.0f : 1.0f) / 3.0f) / N;
        const float y = (static_cast<float>(j) + (upper ? 2.0f : 1.0f) / 3.0f) / N;
        const glm::vec2 t = uv[0] * (1.0f - x - y) + uv[1] * x + uv[2] * y;
        // Footprint threshold, as in the cut-out mips' distant levels: any
        // meaningful share of needles in the cell makes it cast shadow.
        if (sample(t) < 0.18f)
          continue;
        const uint32_t index = 2 * N * j - j * j + 2 * i + upper;
        words[index >> 5] |= 1u << (index & 31u);
      }
}

// Cut-out textures (foliage cards, fences, hair) need their mips built with
// alpha COVERAGE preserved. Averaging alpha shrinks a sparse needle mask
// toward its mean at every level -- a spray covering 13% of its card has
// alpha ~0.13 by mip 3 -- so past a few metres every texel fails a 0.5
// cutoff and whole branches vanish. Castano (2010): after box-filtering each
// level, rescale its alpha so the fraction of texels passing the cutoff
// matches the base level. Colour is averaged in linear space for sRGB data.
// Returns the concatenated levels, largest first.
std::vector<uint8_t> buildCoveragePreservingMips(const uint8_t *rgba, uint32_t w,
                                                 uint32_t h, bool srgb,
                                                 uint32_t levels,
                                                 std::vector<VkDeviceSize> &offsets,
                                                 bool growFootprint) {
  static float toLinear[256];
  static bool tableReady = false;
  if (!tableReady) {
    for (int i = 0; i < 256; ++i) {
      const float c = i / 255.0f;
      toLinear[i] = c <= 0.04045f ? c / 12.92f
                                  : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    tableReady = true;
  }
  const auto encode = [&](float v) -> uint8_t {
    v = std::clamp(v, 0.0f, 1.0f);
    if (srgb)
      v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(v * 255.0f));
  };
  constexpr float kCutoff = 0.5f;
  const auto coverage = [&](const std::vector<float> &a, float scale) {
    size_t n = 0;
    for (float v : a)
      n += (v * scale >= kCutoff) ? 1 : 0;
    return static_cast<float>(n) / static_cast<float>(std::max<size_t>(a.size(), 1));
  };

  // Linear float working copy of the current level.
  uint32_t cw = w, ch = h;
  std::vector<float> cur(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
    for (int c = 0; c < 3; ++c)
      cur[i * 4 + c] = srgb ? toLinear[rgba[i * 4 + c]] : rgba[i * 4 + c] / 255.0f;
    cur[i * 4 + 3] = rgba[i * 4 + 3] / 255.0f;
  }
  std::vector<float> alpha0(static_cast<size_t>(w) * h);
  for (size_t i = 0; i < alpha0.size(); ++i)
    alpha0[i] = cur[i * 4 + 3];
  const float targetCoverage = coverage(alpha0, 1.0f);

  std::vector<uint8_t> out;
  offsets.clear();
  for (uint32_t level = 0; level < levels; ++level) {
    if (level > 0) {
      const uint32_t nw = std::max(cw / 2, 1u), nh = std::max(ch / 2, 1u);
      std::vector<float> next(static_cast<size_t>(nw) * nh * 4, 0.0f);
      for (uint32_t y = 0; y < nh; ++y)
        for (uint32_t x = 0; x < nw; ++x) {
          float sum[4] = {0, 0, 0, 0};
          float alphaSum = 0.0f;
          int taps = 0;
          for (uint32_t dy = 0; dy < 2; ++dy)
            for (uint32_t dx = 0; dx < 2; ++dx) {
              const uint32_t sx = std::min(x * 2 + dx, cw - 1);
              const uint32_t sy = std::min(y * 2 + dy, ch - 1);
              const float *p = &cur[(static_cast<size_t>(sy) * cw + sx) * 4];
              // Alpha-weighted colour: transparent texels in cut-out art are
              // often black or garbage, and would darken the fringe.
              for (int c = 0; c < 3; ++c)
                sum[c] += p[c] * p[3];
              alphaSum += p[3];
              sum[3] += p[3];
              ++taps;
            }
          float *q = &next[(static_cast<size_t>(y) * nw + x) * 4];
          for (int c = 0; c < 3; ++c)
            q[c] = alphaSum > 1e-5f ? sum[c] / alphaSum : 0.0f;
          q[3] = sum[3] / static_cast<float>(taps);
        }
      cur.swap(next);
      cw = nw;
      ch = nh;
      // Binary-search the alpha scale that restores the base coverage.
      std::vector<float> a(static_cast<size_t>(cw) * ch);
      for (size_t i = 0; i < a.size(); ++i)
        a[i] = cur[i * 4 + 3];
      // ...except that preserving the base coverage exactly is too thin for
      // foliage. A needle spray covers ~13% of its card; far away, many
      // sub-pixel needles on overlapping sprays merge into a solid mass,
      // but a 13%-opaque card stays 13% opaque at any distance and a
      // spruce stand turns into bare poles. From mip 2 on, the target eases
      // toward the spray's FOOTPRINT -- texels with any real alpha in them
      // -- which is the silhouette the eye resolves at that distance.
      size_t footprint = 0;
      for (float v : a)
        footprint += v > 0.12f ? 1 : 0;
      const float footprintCoverage =
          static_cast<float>(footprint) / static_cast<float>(std::max<size_t>(a.size(), 1));
      const float grow = growFootprint
          ? std::clamp((static_cast<float>(level) - 1.0f) / 3.0f, 0.0f, 1.0f) : 0.0f;
      const float levelTarget =
          std::max(targetCoverage,
                   targetCoverage + (footprintCoverage * 0.85f - targetCoverage) * grow);
      float lo = 0.0f, hi = 8.0f, scale = 1.0f;
      for (int it = 0; it < 12; ++it) {
        scale = 0.5f * (lo + hi);
        if (coverage(a, scale) < levelTarget)
          lo = scale;
        else
          hi = scale;
      }
      for (size_t i = 0; i < a.size(); ++i)
        cur[i * 4 + 3] = std::min(a[i] * scale, 1.0f);
    }
    offsets.push_back(out.size());
    const size_t px = static_cast<size_t>(cw) * ch;
    const size_t base = out.size();
    out.resize(base + px * 4);
    for (size_t i = 0; i < px; ++i) {
      for (int c = 0; c < 3; ++c)
        out[base + i * 4 + c] = level == 0 ? rgba[i * 4 + c] : encode(cur[i * 4 + c]);
      out[base + i * 4 + 3] = level == 0 ? rgba[i * 4 + 3]
          : static_cast<uint8_t>(std::lround(std::clamp(cur[i * 4 + 3], 0.0f, 1.0f) * 255.0f));
    }
  }
  return out;
}

// True for a genuine cut-out mask: a meaningful share of texels on each side
// of the cutoff. Opaque textures (alpha all 255) and soft vignettes are not.
bool isCutoutAlpha(const uint8_t *rgba, uint32_t w, uint32_t h) {
  const size_t px = static_cast<size_t>(w) * h;
  if (px < 64)
    return false;
  size_t below = 0;
  for (size_t i = 0; i < px; ++i)
    below += rgba[i * 4 + 3] < 128 ? 1 : 0;
  const double f = static_cast<double>(below) / static_cast<double>(px);
  return f > 0.02 && f < 0.98;
}
} // namespace

uint32_t VulkanRenderer::addTexture(const uint8_t *rgba, uint32_t w, uint32_t h,
                                    VkFormat format, bool growCutoutFootprint) {
  // Full mip count; cut-out textures get a CPU-built, coverage-preserving
  // chain (see buildCoveragePreservingMips), everything else GPU blits.
  const uint32_t fullMips =
      static_cast<uint32_t>(std::floor(std::log2(
          static_cast<float>(std::max(w, h))))) + 1u;
  std::vector<VkDeviceSize> cpuMipOffsets;
  std::vector<uint8_t> cpuMips;
  if (fullMips > 1 && isCutoutAlpha(rgba, w, h)) {
    cpuMips = buildCoveragePreservingMips(
        rgba, w, h, format == VK_FORMAT_R8G8B8A8_SRGB, fullMips, cpuMipOffsets,
        growCutoutFootprint);
    rgba = cpuMips.data();
    std::fprintf(stderr, "[VulkanRHI] cut-out texture %ux%u: coverage-preserving mips\n", w, h);
  }
  const bool cpuMipped = !cpuMips.empty();
  const VkDeviceSize byteSize =
      cpuMipped ? static_cast<VkDeviceSize>(cpuMips.size())
                : static_cast<VkDeviceSize>(w) * h * 4;

  VkBufferCreateInfo stagingCi{};
  stagingCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  stagingCi.size = byteSize;
  stagingCi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  stagingCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo stagingAlloc{};
  stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
  stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VkBuffer staging = VK_NULL_HANDLE;
  VmaAllocation stagingMem = VK_NULL_HANDLE;
  VmaAllocationInfo stagingInfo{};
  VK_CHECK(vmaCreateBuffer(mCtx->allocator(), &stagingCi, &stagingAlloc,
                           &staging, &stagingMem, &stagingInfo));
  std::memcpy(stagingInfo.pMappedData, rgba, static_cast<size_t>(byteSize));

  VkImageCreateInfo imageCi{};
  imageCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageCi.imageType = VK_IMAGE_TYPE_2D;
  imageCi.format = format;
  imageCi.extent = {w, h, 1};
  // Full mip chain. Every material texture used to be created with ONE
  // level: a 2048^2 ground texture minified to a few pixels per texel
  // aliased into shimmering noise past ~10 m, which is why the terrain
  // leaned on flat painted colours and kept its photo layers at a few
  // percent. Mips are generated on the GPU below with linear blits, which
  // all desktop drivers support for R8G8B8A8 UNORM/SRGB (checked anyway).
  VkFormatProperties formatProps{};
  vkGetPhysicalDeviceFormatProperties(mCtx->physicalDevice(), format,
                                      &formatProps);
  const bool canBlit =
      (formatProps.optimalTilingFeatures &
       VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
  const uint32_t mipLevels = (canBlit || cpuMipped) ? fullMips : 1u;
  imageCi.mipLevels = mipLevels;
  imageCi.arrayLayers = 1;
  imageCi.samples = VK_SAMPLE_COUNT_1_BIT;
  imageCi.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageCi.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo imageAlloc{};
  imageAlloc.usage = VMA_MEMORY_USAGE_AUTO;
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation alloc = VK_NULL_HANDLE;
  VK_CHECK(vmaCreateImage(mCtx->allocator(), &imageCi, &imageAlloc, &image,
                          &alloc, nullptr));

  immediateSubmit([&](VkCommandBuffer cmd) {
    imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, mipLevels);
    if (cpuMipped) {
      std::vector<VkBufferImageCopy> regions(mipLevels);
      for (uint32_t level = 0; level < mipLevels; ++level) {
        VkBufferImageCopy &r = regions[level];
        r = {};
        r.bufferOffset = cpuMipOffsets[level];
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        r.imageExtent = {std::max(w >> level, 1u), std::max(h >> level, 1u), 1};
      }
      vkCmdCopyBufferToImage(cmd, staging, image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             static_cast<uint32_t>(regions.size()),
                             regions.data());
      imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, mipLevels);
      return;
    }
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    // Downsample level i-1 -> i. Each source level is flipped to
    // TRANSFER_SRC just before it is read, then straight to SHADER_READ once
    // its child exists, so every level ends in the same layout.
    int32_t mw = static_cast<int32_t>(w), mh = static_cast<int32_t>(h);
    for (uint32_t level = 1; level < mipLevels; ++level) {
      imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_ASPECT_COLOR_BIT, 1, level - 1, 1);
      const int32_t nw = std::max(mw / 2, 1), nh = std::max(mh / 2, 1);
      VkImageBlit blit{};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
      blit.srcOffsets[1] = {mw, mh, 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
      blit.dstOffsets[1] = {nw, nh, 1};
      vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                     VK_FILTER_LINEAR);
      imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_ASPECT_COLOR_BIT, 1, level - 1, 1);
      mw = nw;
      mh = nh;
    }
    imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_ASPECT_COLOR_BIT, 1, mipLevels - 1, 1);
  });
  vmaDestroyBuffer(mCtx->allocator(), staging, stagingMem);

  VkImageViewCreateInfo viewCi{};
  viewCi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewCi.image = image;
  viewCi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewCi.format = format;
  viewCi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  viewCi.subresourceRange.levelCount = mipLevels;
  viewCi.subresourceRange.layerCount = 1;
  VkImageView view = VK_NULL_HANDLE;
  VK_CHECK(vkCreateImageView(mCtx->device(), &viewCi, nullptr, &view));

  mTextureImages.push_back(image);
  mTextureAllocs.push_back(alloc);
  mTextureViews.push_back(view);
  return mBindless.registerTexture(*mCtx, view, mMaterialSampler);
}

uint32_t VulkanRenderer::createDefaultTexture() {
  const uint32_t size = 256;
  const uint32_t cell = 32;
  std::vector<uint8_t> pixels(size * size * 4);
  for (uint32_t y = 0; y < size; ++y) {
    for (uint32_t x = 0; x < size; ++x) {
      const bool on = ((x / cell) + (y / cell)) % 2 == 0;
      uint8_t *p = &pixels[(y * size + x) * 4];
      const uint8_t v = on ? 200 : 70;
      p[0] = v;
      p[1] = v;
      p[2] = v;
      p[3] = 255;
    }
  }
  return addTexture(pixels.data(), size, size, VK_FORMAT_R8G8B8A8_UNORM);
}

uint32_t VulkanRenderer::loadTextureFile(const std::string &path, bool flipY,
                                         bool srgb) {
  int w = 0, h = 0, channels = 0;
  stbi_set_flip_vertically_on_load(flipY ? 1 : 0);
  stbi_uc *pixels = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
  stbi_set_flip_vertically_on_load(0);
  if (!pixels) {
    std::fprintf(stderr,
                 "[VulkanRHI] Failed to load texture '%s' (%s); using default\n",
                 path.c_str(), stbi_failure_reason());
    return mDefaultTexIndex;
  }
  const uint32_t index = addTexture(
      pixels, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
      srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
  stbi_image_free(pixels);
  std::fprintf(stderr, "[VulkanRHI] Loaded texture '%s' (%dx%d) -> bindless %u\n",
               path.c_str(), w, h, index);
  return index;
}

void VulkanRenderer::resolveTerrainMaterialsIfDirty() {
  if (!mParams.terrainMaterialsDirty)
    return;
  // Preset switching must reuse texture descriptors, not exhaust the bindless array.
  auto map = [&](const std::string &path, bool srgb) -> uint32_t {
    if (path.empty() || !std::filesystem::exists(path)) return UINT32_MAX;
    const std::string key = path + (srgb ? "#srgb" : "#linear");
    auto found = mTerrainTextureCache.find(key);
    if (found != mTerrainTextureCache.end()) return found->second;
    uint32_t tex = loadTextureFile(path, true, srgb);
    if (tex == mDefaultTexIndex) return UINT32_MAX;
    return mTerrainTextureCache.emplace(key, tex).first->second;
  };
  auto resolve = [&](const Params::TerrainMaterialSlot &slot) {
    TerrainMaterialResolved r;
    r.albedoTex = map(slot.albedoPath, true);
    r.normalTex = map(slot.normalPath, false);
    r.roughnessTex = map(slot.roughnessPath, false);
    r.heightTex = map(slot.heightPath, false);
    return r;
  };
  for (size_t i = 0; i < mParams.terrainMaterialSlots.size(); ++i)
    mTerrainMaterialTex[i] = resolve(mParams.terrainMaterialSlots[i]);
  mSnowMaterialTex = resolve(mParams.snowMaterial);
  mParams.terrainMaterialsDirty = false;
}

bool VulkanRenderer::createDescriptorsAndFrameData() {
  if (!mBindless.init(*mCtx))
    return false;

  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  binding.descriptorCount = 1;
  // Also read from the task/mesh stages for terrain.
  binding.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
      VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutCi.bindingCount = 1;
  layoutCi.pBindings = &binding;
  VK_CHECK(vkCreateDescriptorSetLayout(mCtx->device(), &layoutCi, nullptr,
                                       &mFrameSetLayout));

  VkDescriptorPoolSize poolSize{};
  poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  poolSize.descriptorCount = kFramesInFlight;
  VkDescriptorPoolCreateInfo poolCi{};
  poolCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolCi.maxSets = kFramesInFlight;
  poolCi.poolSizeCount = 1;
  poolCi.pPoolSizes = &poolSize;
  VK_CHECK(
      vkCreateDescriptorPool(mCtx->device(), &poolCi, nullptr, &mFrameDescPool));

  mFrameSets.resize(kFramesInFlight);
  mFrameUBOs.resize(kFramesInFlight);
  mFrameUBOAllocs.resize(kFramesInFlight);
  mFrameUBOMapped.resize(kFramesInFlight);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkBufferCreateInfo bufferCi{};
    bufferCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferCi.size = sizeof(FrameDataGpu);
    bufferCi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufferCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocCi{};
    allocCi.usage = VMA_MEMORY_USAGE_AUTO;
    allocCi.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(mCtx->allocator(), &bufferCi, &allocCi,
                             &mFrameUBOs[i], &mFrameUBOAllocs[i], &info));
    mFrameUBOMapped[i] = info.pMappedData;

    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = mFrameDescPool;
    setAlloc.descriptorSetCount = 1;
    setAlloc.pSetLayouts = &mFrameSetLayout;
    VK_CHECK(
        vkAllocateDescriptorSets(mCtx->device(), &setAlloc, &mFrameSets[i]));

    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = mFrameUBOs[i];
    bufInfo.range = sizeof(FrameDataGpu);
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = mFrameSets[i];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &bufInfo;
    vkUpdateDescriptorSets(mCtx->device(), 1, &write, 0, nullptr);
  }
  return true;
}

bool VulkanRenderer::createSceneTargets() {
  const VkExtent2D extent = mSwapchain.extent();
  mHdrImages.resize(kFramesInFlight);
  mHdrAllocs.resize(kFramesInFlight);
  mHdrViews.resize(kFramesInFlight);
  mDepthImages.resize(kFramesInFlight);
  mDepthAllocs.resize(kFramesInFlight);
  mDepthViews.resize(kFramesInFlight);
  mViewmodelDepthImages.resize(kFramesInFlight);
  mViewmodelDepthAllocs.resize(kFramesInFlight);
  mViewmodelDepthViews.resize(kFramesInFlight);

  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkImageCreateInfo hdrCi{};
    hdrCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    hdrCi.imageType = VK_IMAGE_TYPE_2D;
    hdrCi.format = mHdrFormat;
    hdrCi.extent = {extent.width, extent.height, 1};
    hdrCi.mipLevels = 1;
    hdrCi.arrayLayers = 1;
    hdrCi.samples = VK_SAMPLE_COUNT_1_BIT;
    hdrCi.tiling = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_SRC: the water pass copies this to mSceneCopyImages so it can
    // sample the opaque scene it is about to draw over (refraction + SSR).
    hdrCi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    hdrCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo hdrAlloc{};
    hdrAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(mCtx->allocator(), &hdrCi, &hdrAlloc,
                            &mHdrImages[i], &mHdrAllocs[i], nullptr));

    VkImageViewCreateInfo hdrView{};
    hdrView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    hdrView.image = mHdrImages[i];
    hdrView.viewType = VK_IMAGE_VIEW_TYPE_2D;
    hdrView.format = mHdrFormat;
    hdrView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    hdrView.subresourceRange.levelCount = 1;
    hdrView.subresourceRange.layerCount = 1;
    VK_CHECK(
        vkCreateImageView(mCtx->device(), &hdrView, nullptr, &mHdrViews[i]));

    VkImageCreateInfo depthCi{};
    depthCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthCi.imageType = VK_IMAGE_TYPE_2D;
    depthCi.format = mDepthFormat;
    depthCi.extent = {extent.width, extent.height, 1};
    depthCi.mipLevels = 1;
    depthCi.arrayLayers = 1;
    depthCi.samples = VK_SAMPLE_COUNT_1_BIT;
    depthCi.tiling = VK_IMAGE_TILING_OPTIMAL;
    // SAMPLED: the SSAO pass reads this depth buffer (written by a prepass)
    // to reconstruct view-space position/normal.
    depthCi.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT;
    depthCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo depthAlloc{};
    depthAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(mCtx->allocator(), &depthCi, &depthAlloc,
                            &mDepthImages[i], &mDepthAllocs[i], nullptr));

    VkImageViewCreateInfo depthView{};
    depthView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthView.image = mDepthImages[i];
    depthView.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthView.format = mDepthFormat;
    depthView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthView.subresourceRange.levelCount = 1;
    depthView.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(mCtx->device(), &depthView, nullptr,
                               &mDepthViews[i]));

    // Separate foreground depth preserves terrain depth for SSAO, water and
    // temporal history. Cleared each frame, with no world occlusion of the gun.
    depthCi.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VK_CHECK(vmaCreateImage(mCtx->allocator(), &depthCi, &depthAlloc,
        &mViewmodelDepthImages[i], &mViewmodelDepthAllocs[i], nullptr));
    depthView.image = mViewmodelDepthImages[i];
    VK_CHECK(vkCreateImageView(mCtx->device(), &depthView, nullptr, &mViewmodelDepthViews[i]));
    immediateSubmit([&](VkCommandBuffer cmd) {
      imageBarrier(cmd, mViewmodelDepthImages[i], VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
          VK_IMAGE_ASPECT_DEPTH_BIT);
    });
    immediateSubmit([&](VkCommandBuffer cmd) {
      imageBarrier(cmd, mDepthImages[i], VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                   VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_ASPECT_DEPTH_BIT);
    });
  }

  // Opaque-scene snapshot for the water pass (refraction source + SSR colour
  // source). Same format/extent as the HDR target so the copy is a straight
  // vkCmdCopyImage rather than a format-converting blit.
  mSceneCopyImages.resize(kFramesInFlight);
  mSceneCopyAllocs.resize(kFramesInFlight);
  mSceneCopyViews.resize(kFramesInFlight);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = mHdrFormat;
    ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(mCtx->allocator(), &ci, &alloc,
                            &mSceneCopyImages[i], &mSceneCopyAllocs[i],
                            nullptr));

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = mSceneCopyImages[i];
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = mHdrFormat;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(mCtx->device(), &view, nullptr,
                               &mSceneCopyViews[i]));
  }

  // Bloom and volumetrics keep their half-resolution targets.
  const VkExtent2D halfExtent = {std::max(1u, extent.width / 2),
                                 std::max(1u, extent.height / 2)};

  // SSAO resolves at full resolution: half-res sampling skipped thin grass
  // roots, and upsampling erased their contact pockets. Keep the bounded
  // 24-tap kernel; this adds depth samples, never grass shadow rays.
  mSSAOImages.resize(kFramesInFlight);
  mSSAOAllocs.resize(kFramesInFlight);
  mSSAOViews.resize(kFramesInFlight);
  mSSAOBlurImages.resize(kFramesInFlight);
  mSSAOBlurAllocs.resize(kFramesInFlight);
  mSSAOBlurViews.resize(kFramesInFlight);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    for (int pass = 0; pass < 2; ++pass) {
      VkImage &img = pass == 0 ? mSSAOImages[i] : mSSAOBlurImages[i];
      VmaAllocation &alloc = pass == 0 ? mSSAOAllocs[i] : mSSAOBlurAllocs[i];
      VkImageView &view = pass == 0 ? mSSAOViews[i] : mSSAOBlurViews[i];
      const VkExtent2D aoExtent = extent;

      VkImageCreateInfo aoCi{};
      aoCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      aoCi.imageType = VK_IMAGE_TYPE_2D;
      aoCi.format = mAOFormat;
      aoCi.extent = {aoExtent.width, aoExtent.height, 1};
      aoCi.mipLevels = 1;
      aoCi.arrayLayers = 1;
      aoCi.samples = VK_SAMPLE_COUNT_1_BIT;
      aoCi.tiling = VK_IMAGE_TILING_OPTIMAL;
      aoCi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      aoCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VmaAllocationCreateInfo aoAlloc{};
      aoAlloc.usage = VMA_MEMORY_USAGE_AUTO;
      VK_CHECK(vmaCreateImage(mCtx->allocator(), &aoCi, &aoAlloc, &img, &alloc,
                              nullptr));

      VkImageViewCreateInfo aoView{};
      aoView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      aoView.image = img;
      aoView.viewType = VK_IMAGE_VIEW_TYPE_2D;
      aoView.format = mAOFormat;
      aoView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      aoView.subresourceRange.levelCount = 1;
      aoView.subresourceRange.layerCount = 1;
      VK_CHECK(vkCreateImageView(mCtx->device(), &aoView, nullptr, &view));
    }
  }

  // Fog and bloom images are owned by VulkanAtmosphereRenderer.
  // Cloud details are kilometres away. Half-resolution plus temporal resolve
  // retains their shape while leaving GPU time for foreground foliage.
  mCloudExtent = halfExtent;
  mCloudImages.resize(kFramesInFlight);
  mCloudAllocs.resize(kFramesInFlight);
  mCloudViews.resize(kFramesInFlight);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = mHdrFormat;
    ci.extent = {mCloudExtent.width, mCloudExtent.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(mCtx->allocator(), &ci, &alloc, &mCloudImages[i],
                            &mCloudAllocs[i], nullptr));

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = mCloudImages[i];
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = mHdrFormat;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(mCtx->device(), &view, nullptr, &mCloudViews[i]));
  }
  mTemporalValid = false;
  mTemporalImages.resize(kFramesInFlight);
  mTemporalAllocs.resize(kFramesInFlight);
  mTemporalViews.resize(kFramesInFlight);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = mHdrFormat;
    ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(mCtx->allocator(), &ci, &alloc, &mTemporalImages[i],
                            &mTemporalAllocs[i], nullptr));
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = mTemporalImages[i];
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = mHdrFormat;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = view.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(mCtx->device(), &view, nullptr, &mTemporalViews[i]));
  }
  // Invalid history still has a statically used descriptor in the shader.
  // Initialize both images so the first frame can bind it safely.
  immediateSubmit([&](VkCommandBuffer cmd) {
    for (VkImage image : mTemporalImages) {
      imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
          VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkClearColorValue black{};
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           &black, 1, &range);
      imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_CLEAR_BIT,
          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
  });
  return true;
}

void VulkanRenderer::destroySceneTargets() {
  mTemporalValid = false;
  for (VkImageView view : mTemporalViews)
    vkDestroyImageView(mCtx->device(), view, nullptr);
  for (uint32_t i = 0; i < mTemporalImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mTemporalImages[i], mTemporalAllocs[i]);
  mTemporalViews.clear(); mTemporalImages.clear(); mTemporalAllocs.clear();
  for (VkImageView v : mSceneCopyViews)
    vkDestroyImageView(mCtx->device(), v, nullptr);
  for (uint32_t i = 0; i < mSceneCopyImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mSceneCopyImages[i],
                    mSceneCopyAllocs[i]);
  mSceneCopyViews.clear();
  mSceneCopyImages.clear();
  mSceneCopyAllocs.clear();

  for (VkImageView v : mVolumetricViews)
    vkDestroyImageView(mCtx->device(), v, nullptr);
  for (uint32_t i = 0; i < mVolumetricImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mVolumetricImages[i], mVolumetricAllocs[i]);
  mVolumetricViews.clear();
  mVolumetricImages.clear();
  mVolumetricAllocs.clear();

  for (VkImageView v : mCloudViews)
    vkDestroyImageView(mCtx->device(), v, nullptr);
  for (uint32_t i = 0; i < mCloudImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mCloudImages[i], mCloudAllocs[i]);
  mCloudViews.clear();
  mCloudImages.clear();
  mCloudAllocs.clear();

  for (VkImageView v : mHdrViews)
    vkDestroyImageView(mCtx->device(), v, nullptr);
  for (uint32_t i = 0; i < mHdrImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mHdrImages[i], mHdrAllocs[i]);
  mHdrViews.clear();
  mHdrImages.clear();
  mHdrAllocs.clear();

  for (VkImageView v : mDepthViews)
    vkDestroyImageView(mCtx->device(), v, nullptr);
  for (uint32_t i = 0; i < mDepthImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mDepthImages[i], mDepthAllocs[i]);
  for (auto v : mViewmodelDepthViews) vkDestroyImageView(mCtx->device(), v, nullptr);
  for (size_t i=0; i<mViewmodelDepthImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(),mViewmodelDepthImages[i],mViewmodelDepthAllocs[i]);
  mViewmodelDepthViews.clear(); mViewmodelDepthImages.clear(); mViewmodelDepthAllocs.clear();
  mDepthViews.clear();
  mDepthImages.clear();
  mDepthAllocs.clear();

  for (VkImageView v : mSSAOViews)
    vkDestroyImageView(mCtx->device(), v, nullptr);
  for (uint32_t i = 0; i < mSSAOImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mSSAOImages[i], mSSAOAllocs[i]);
  mSSAOViews.clear();
  mSSAOImages.clear();
  mSSAOAllocs.clear();

  for (VkImageView v : mSSAOBlurViews)
    vkDestroyImageView(mCtx->device(), v, nullptr);
  for (uint32_t i = 0; i < mSSAOBlurImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mSSAOBlurImages[i], mSSAOBlurAllocs[i]);
  mSSAOBlurViews.clear();
  mSSAOBlurImages.clear();
  mSSAOBlurAllocs.clear();

  for (VkImageView v : mBloomExtractViews)
    vkDestroyImageView(mCtx->device(), v, nullptr);
  for (uint32_t i = 0; i < mBloomExtractImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mBloomExtractImages[i],
                    mBloomExtractAllocs[i]);
  mBloomExtractViews.clear();
  mBloomExtractImages.clear();
  mBloomExtractAllocs.clear();

  for (VkImageView v : mBloomBlurHViews)
    vkDestroyImageView(mCtx->device(), v, nullptr);
  for (uint32_t i = 0; i < mBloomBlurHImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mBloomBlurHImages[i],
                    mBloomBlurHAllocs[i]);
  mBloomBlurHViews.clear();
  mBloomBlurHImages.clear();
  mBloomBlurHAllocs.clear();

  for (VkImageView v : mBloomBlurVViews)
    vkDestroyImageView(mCtx->device(), v, nullptr);
  for (uint32_t i = 0; i < mBloomBlurVImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mBloomBlurVImages[i],
                    mBloomBlurVAllocs[i]);
  mBloomBlurVViews.clear();
  mBloomBlurVImages.clear();
  mBloomBlurVAllocs.clear();

  for (VkImageView v : mBloomBlurQ2HViews)
    vkDestroyImageView(mCtx->device(), v, nullptr);
  for (uint32_t i = 0; i < mBloomBlurQ2HImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mBloomBlurQ2HImages[i],
                    mBloomBlurQ2HAllocs[i]);
  mBloomBlurQ2HViews.clear();
  mBloomBlurQ2HImages.clear();
  mBloomBlurQ2HAllocs.clear();

  for (VkImageView v : mBloomBlurQ2VViews)
    vkDestroyImageView(mCtx->device(), v, nullptr);
  for (uint32_t i = 0; i < mBloomBlurQ2VImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mBloomBlurQ2VImages[i],
                    mBloomBlurQ2VAllocs[i]);
  mBloomBlurQ2VViews.clear();
  mBloomBlurQ2VImages.clear();
  mBloomBlurQ2VAllocs.clear();
}

bool VulkanRenderer::createTlasDescriptors() {
  // Set 2 in the scene/terrain pipelines: the TLAS, queried for RT shadows.
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutCi.bindingCount = 1;
  layoutCi.pBindings = &binding;
  VK_CHECK(vkCreateDescriptorSetLayout(mCtx->device(), &layoutCi, nullptr,
                                       &mTlasSetLayout));

  VkDescriptorPoolSize poolSize{};
  poolSize.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  poolSize.descriptorCount = kFramesInFlight;
  VkDescriptorPoolCreateInfo poolCi{};
  poolCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolCi.maxSets = kFramesInFlight;
  poolCi.poolSizeCount = 1;
  poolCi.pPoolSizes = &poolSize;
  VK_CHECK(vkCreateDescriptorPool(mCtx->device(), &poolCi, nullptr, &mTlasPool));

  mTlasSets.resize(kFramesInFlight);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = mTlasPool;
    setAlloc.descriptorSetCount = 1;
    setAlloc.pSetLayouts = &mTlasSetLayout;
    VK_CHECK(vkAllocateDescriptorSets(mCtx->device(), &setAlloc, &mTlasSets[i]));
  }
  return true;
}

void VulkanRenderer::writeTlasDescriptors() {
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkAccelerationStructureKHR tlas = mAccel.tlas(i);
    VkWriteDescriptorSetAccelerationStructureKHR asInfo{};
    asInfo.sType =
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asInfo.accelerationStructureCount = 1;
    asInfo.pAccelerationStructures = &tlas;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext = &asInfo;
    write.dstSet = mTlasSets[i];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(mCtx->device(), 1, &write, 0, nullptr);
  }
}

bool VulkanRenderer::createTonemapResources() {
  // Binding 0: HDR scene color. Binding 1: blurred bloom (half-res, tight
  // glow). Binding 2: the second, quarter-res blur pass (Task 4.3, a wider
  // softer glow) -- added here rather than new sets, since all three are
  // simple fragment-stage samplers read by the same single pass.
  VkDescriptorSetLayoutBinding bindings[4]{};
  bindings[0].binding = 0;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[1].binding = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[2].binding = 2;
  bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[2].descriptorCount = 1;
  bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[3].binding = 3;
  bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[3].descriptorCount = 1;
  bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutCi.bindingCount = 4;
  layoutCi.pBindings = bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(mCtx->device(), &layoutCi, nullptr,
                                       &mTonemapSetLayout));

  VkDescriptorPoolSize poolSize{};
  poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  poolSize.descriptorCount = kFramesInFlight * 4;
  VkDescriptorPoolCreateInfo poolCi{};
  poolCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolCi.maxSets = kFramesInFlight;
  poolCi.poolSizeCount = 1;
  poolCi.pPoolSizes = &poolSize;
  VK_CHECK(
      vkCreateDescriptorPool(mCtx->device(), &poolCi, nullptr, &mTonemapPool));

  mTonemapSets.resize(kFramesInFlight);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = mTonemapPool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &mTonemapSetLayout;
    VK_CHECK(vkAllocateDescriptorSets(mCtx->device(), &alloc, &mTonemapSets[i]));
  }
  return true;
}

void VulkanRenderer::updateTonemapSets() {
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkDescriptorImageInfo hdrImage{};
    hdrImage.sampler = mSampler;
    hdrImage.imageView = mHdrViews[i];
    hdrImage.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo bloomImage{};
    bloomImage.sampler = mSampler;
    bloomImage.imageView = mHdrViews[i];
    bloomImage.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo bloomWideImage{};
    bloomWideImage.sampler = mSampler;
    bloomWideImage.imageView = mHdrViews[i];
    bloomWideImage.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo depthImage{};
    depthImage.sampler = mSampler;
    depthImage.imageView = mDepthViews[i];
    depthImage.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[4]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = mTonemapSets[i];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &hdrImage;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = mTonemapSets[i];
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &bloomImage;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = mTonemapSets[i];
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &bloomWideImage;
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = mTonemapSets[i];
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].pImageInfo = &depthImage;
    vkUpdateDescriptorSets(mCtx->device(), 4, writes, 0, nullptr);
  }
}

bool VulkanRenderer::createTemporalResources() {
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight * 4};
  VkDescriptorPoolCreateInfo pool{};
  pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool.maxSets = kFramesInFlight;
  pool.poolSizeCount = 1;
  pool.pPoolSizes = &size;
  VK_CHECK(vkCreateDescriptorPool(mCtx->device(), &pool, nullptr, &mTemporalPool));
  mTemporalSets.resize(kFramesInFlight);
  for (auto &set : mTemporalSets) {
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = mTemporalPool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &mTonemapSetLayout;
    VK_CHECK(vkAllocateDescriptorSets(mCtx->device(), &alloc, &set));
  }
  return true;
}

void VulkanRenderer::updateTemporalSets() {
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkDescriptorImageInfo images[4]{};
    images[0] = {mSampler, mHdrViews[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    images[1] = {mSampler, mDepthViews[i], VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    images[2] = {mSampler, mTemporalViews[(i + kFramesInFlight - 1) % kFramesInFlight],
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    images[3] = images[1]; // unused binding, shared descriptor layout
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t b = 0; b < 4; ++b) {
      writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[b].dstSet = mTemporalSets[i];
      writes[b].dstBinding = b;
      writes[b].descriptorCount = 1;
      writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[b].pImageInfo = &images[b];
    }
    vkUpdateDescriptorSets(mCtx->device(), 4, writes, 0, nullptr);
  }
}

// Three single-sampler descriptor sets per frame in flight, one shared
// layout (mirrors the tonemap pattern): SSAO pass samples depth, blur pass
// samples raw AO, the main scene pass samples the blurred AO.
bool VulkanRenderer::createSSAOResources() {
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutCi.bindingCount = 1;
  layoutCi.pBindings = &binding;
  VK_CHECK(vkCreateDescriptorSetLayout(mCtx->device(), &layoutCi, nullptr,
                                       &mAOSamplerSetLayout));

  // 4 per frame: SSAO depth, blur input, scene AO, and the water pass's
  // opaque-scene copy (same one-sampler shape, so it shares this layout).
  VkDescriptorPoolSize poolSize{};
  poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  poolSize.descriptorCount = kFramesInFlight * 4;
  VkDescriptorPoolCreateInfo poolCi{};
  poolCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolCi.maxSets = kFramesInFlight * 4;
  poolCi.poolSizeCount = 1;
  poolCi.pPoolSizes = &poolSize;
  VK_CHECK(vkCreateDescriptorPool(mCtx->device(), &poolCi, nullptr,
                                  &mAOSamplerPool));

  mSSAODepthSets.resize(kFramesInFlight);
  mBlurInputSets.resize(kFramesInFlight);
  mSceneAOSets.resize(kFramesInFlight);
  mSceneCopySets.resize(kFramesInFlight);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    for (VkDescriptorSet *set :
        {&mSSAODepthSets[i], &mBlurInputSets[i], &mSceneAOSets[i],
         &mSceneCopySets[i]}) {
      VkDescriptorSetAllocateInfo alloc{};
      alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      alloc.descriptorPool = mAOSamplerPool;
      alloc.descriptorSetCount = 1;
      alloc.pSetLayouts = &mAOSamplerSetLayout;
      VK_CHECK(vkAllocateDescriptorSets(mCtx->device(), &alloc, set));
    }
  }
  return true;
}

void VulkanRenderer::updateSSAOSets() {
  auto writeSet = [&](VkDescriptorSet set, VkImageView view,
                      VkImageLayout layout) {
    VkDescriptorImageInfo image{};
    image.sampler = mSampler;
    image.imageView = view;
    image.imageLayout = layout;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(mCtx->device(), 1, &write, 0, nullptr);
  };
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    writeSet(mSSAODepthSets[i], mDepthViews[i],
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    writeSet(mBlurInputSets[i], mSSAOViews[i],
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    writeSet(mSceneAOSets[i], mSSAOBlurViews[i],
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    writeSet(mSceneCopySets[i], mSceneCopyViews[i],
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
}

// Five single-sampler sets per frame in flight, reusing mAOSamplerSetLayout
// (same shape: one combined-image-sampler at binding 0, fragment stage) --
// extract samples full-res HDR, blur-H samples the extract target, blur-V
// samples the blur-H target; the quarter-res H/V pair (Task 4.3's second,
// wider blur) chains off blur-V the same way blur-H/V chain off extract.
bool VulkanRenderer::createBloomResources() {
  VkDescriptorPoolSize poolSize{};
  poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  poolSize.descriptorCount = kFramesInFlight * 5;
  VkDescriptorPoolCreateInfo poolCi{};
  poolCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolCi.maxSets = kFramesInFlight * 5;
  poolCi.poolSizeCount = 1;
  poolCi.pPoolSizes = &poolSize;
  VK_CHECK(vkCreateDescriptorPool(mCtx->device(), &poolCi, nullptr,
                                  &mBloomSamplerPool));

  mBloomExtractInputSets.resize(kFramesInFlight);
  mBloomBlurHInputSets.resize(kFramesInFlight);
  mBloomBlurVInputSets.resize(kFramesInFlight);
  mBloomBlurQ2HInputSets.resize(kFramesInFlight);
  mBloomBlurQ2VInputSets.resize(kFramesInFlight);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    for (VkDescriptorSet *set :
        {&mBloomExtractInputSets[i], &mBloomBlurHInputSets[i],
         &mBloomBlurVInputSets[i], &mBloomBlurQ2HInputSets[i],
         &mBloomBlurQ2VInputSets[i]}) {
      VkDescriptorSetAllocateInfo alloc{};
      alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      alloc.descriptorPool = mBloomSamplerPool;
      alloc.descriptorSetCount = 1;
      alloc.pSetLayouts = &mAOSamplerSetLayout;
      VK_CHECK(vkAllocateDescriptorSets(mCtx->device(), &alloc, set));
    }
  }
  return true;
}

void VulkanRenderer::updateBloomSets() {
  auto writeSet = [&](VkDescriptorSet set, VkImageView view) {
    VkDescriptorImageInfo image{};
    image.sampler = mSampler;
    image.imageView = view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(mCtx->device(), 1, &write, 0, nullptr);
  };
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    writeSet(mBloomExtractInputSets[i], mHdrViews[i]);
    writeSet(mBloomBlurHInputSets[i], mBloomExtractViews[i]);
    writeSet(mBloomBlurVInputSets[i], mBloomBlurHViews[i]);
    writeSet(mBloomBlurQ2HInputSets[i], mBloomBlurVViews[i]);
    writeSet(mBloomBlurQ2VInputSets[i], mBloomBlurQ2HViews[i]);
  }
}

// Sky environment cubemap: 128^2 x 6 RGBA16F with a full mip chain, per
// frame in flight. Rendered every frame by the sky shader (so IBL tracks
// the sun in real time), mips generated with per-face blits, sampled by the
// scene pipelines as set 4. Fixed-size -- created once, survives swapchain
// recreation. Also allocates the volumetric-composite's sampler sets (same
// pool; the scatter IMAGE is swapchain-sized and lives in
// createSceneTargets, so its set is (re)written in updateVolumetricSets).
bool VulkanRenderer::createEnvMapResources() {
  VkDescriptorSetLayoutBinding environmentBindings[]={
    {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
    {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
    {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
  VkDescriptorSetLayoutCreateInfo environmentLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  environmentLayout.bindingCount=3;environmentLayout.pBindings=environmentBindings;
  VK_CHECK(vkCreateDescriptorSetLayout(mCtx->device(),&environmentLayout,nullptr,&mEnvironmentSetLayout));
  mEnvImages.resize(kFramesInFlight);
  mEnvAllocs.resize(kFramesInFlight);
  mEnvCubeViews.resize(kFramesInFlight);
  mEnvFaceViews.resize(kFramesInFlight);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkImageCreateInfo envCi{};
    envCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    envCi.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    envCi.imageType = VK_IMAGE_TYPE_2D;
    envCi.format = mHdrFormat;
    envCi.extent = {kEnvFaceSize, kEnvFaceSize, 1};
    envCi.mipLevels = kEnvMipCount;
    envCi.arrayLayers = 6;
    envCi.samples = VK_SAMPLE_COUNT_1_BIT;
    envCi.tiling = VK_IMAGE_TILING_OPTIMAL;
    envCi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    envCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo envAlloc{};
    envAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(mCtx->allocator(), &envCi, &envAlloc,
                            &mEnvImages[i], &mEnvAllocs[i], nullptr));

    VkImageViewCreateInfo cubeView{};
    cubeView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    cubeView.image = mEnvImages[i];
    cubeView.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    cubeView.format = mHdrFormat;
    cubeView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cubeView.subresourceRange.levelCount = kEnvMipCount;
    cubeView.subresourceRange.layerCount = 6;
    VK_CHECK(vkCreateImageView(mCtx->device(), &cubeView, nullptr,
                               &mEnvCubeViews[i]));

    for (uint32_t face = 0; face < 6; ++face) {
      VkImageViewCreateInfo faceView{};
      faceView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      faceView.image = mEnvImages[i];
      faceView.viewType = VK_IMAGE_VIEW_TYPE_2D;
      faceView.format = mHdrFormat;
      faceView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      faceView.subresourceRange.baseMipLevel = 0;
      faceView.subresourceRange.levelCount = 1;
      faceView.subresourceRange.baseArrayLayer = face;
      faceView.subresourceRange.layerCount = 1;
      VK_CHECK(vkCreateImageView(mCtx->device(), &faceView, nullptr,
                                 &mEnvFaceViews[i][face]));
    }
  }

  // Environment sets also carry cloud transmission and queue-ordered sky SH;
  // the legacy scatter sets retain the single-sampler layout.
  VkDescriptorPoolSize poolSize{};
  poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  poolSize.descriptorCount = kFramesInFlight * 3;
  VkDescriptorPoolCreateInfo poolCi{};
  poolCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolCi.maxSets = kFramesInFlight * 2;
  VkDescriptorPoolSize envPoolSizes[]={poolSize,{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,kFramesInFlight}};
  poolCi.poolSizeCount = 2;
  poolCi.pPoolSizes = envPoolSizes;
  VK_CHECK(vkCreateDescriptorPool(mCtx->device(), &poolCi, nullptr,
                                  &mEnvVolPool));

  mSceneEnvSets.resize(kFramesInFlight);
  mVolScatterSets.resize(kFramesInFlight);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    for (VkDescriptorSet *set : {&mSceneEnvSets[i], &mVolScatterSets[i]}) {
      VkDescriptorSetAllocateInfo alloc{};
      alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      alloc.descriptorPool = mEnvVolPool;
      alloc.descriptorSetCount = 1;
      alloc.pSetLayouts = set==&mSceneEnvSets[i]?&mEnvironmentSetLayout:&mAOSamplerSetLayout;
      VK_CHECK(vkAllocateDescriptorSets(mCtx->device(), &alloc, set));
    }
  }

  // The env cube view never changes -- write its sets once here.
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkDescriptorImageInfo image{};
    image.sampler = mSampler;
    image.imageView = mEnvCubeViews[i];
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = mSceneEnvSets[i];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(mCtx->device(), 1, &write, 0, nullptr);
  }
  return true;
}

void VulkanRenderer::destroyEnvMapResources() {
  for (auto &faces : mEnvFaceViews)
    for (VkImageView v : faces)
      vkDestroyImageView(mCtx->device(), v, nullptr);
  mEnvFaceViews.clear();
  for (VkImageView v : mEnvCubeViews)
    vkDestroyImageView(mCtx->device(), v, nullptr);
  mEnvCubeViews.clear();
  for (uint32_t i = 0; i < mEnvImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mEnvImages[i], mEnvAllocs[i]);
  mEnvImages.clear();
  mEnvAllocs.clear();
  if (mEnvVolPool)
    vkDestroyDescriptorPool(mCtx->device(), mEnvVolPool, nullptr);
  mEnvVolPool = VK_NULL_HANDLE;
  if(mEnvironmentSetLayout)vkDestroyDescriptorSetLayout(mCtx->device(),mEnvironmentSetLayout,nullptr);
  mEnvironmentSetLayout=VK_NULL_HANDLE;
  mSceneEnvSets.clear();
  mVolScatterSets.clear();
}

// (Re)points the volumetric-composite's scatter set at the current half-res
// scatter view -- called at init and after every swapchain recreation (the
// scatter image is swapchain-sized).
void VulkanRenderer::updateVolumetricSets() {
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkDescriptorImageInfo image{};
    image.sampler = mSampler;
    image.imageView = mVolumetricViews[i];
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = mVolScatterSets[i];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(mCtx->device(), 1, &write, 0, nullptr);
  }
}

VkShaderModule VulkanRenderer::loadShaderModule(const std::string &path) {
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file.is_open()) {
    std::fprintf(stderr, "[VulkanRHI] Cannot open SPIR-V: %s\n", path.c_str());
    std::abort();
  }
  const size_t size = static_cast<size_t>(file.tellg());
  std::vector<char> code(size);
  file.seekg(0);
  file.read(code.data(), static_cast<std::streamsize>(size));

  VkShaderModuleCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize = size;
  ci.pCode = reinterpret_cast<const uint32_t *>(code.data());
  VkShaderModule module = VK_NULL_HANDLE;
  VK_CHECK(vkCreateShaderModule(mCtx->device(), &ci, nullptr, &module));
  return module;
}

bool VulkanRenderer::createScenePipeline(const std::string &shaderDir) {
  // Sets 3 (AO) / 4 (sky env cubemap) are sampled only by the lit fragment
  // shaders; the depth prepass pipelines reuse this same layout without
  // ever binding/reading them.
  VkDescriptorSetLayout setLayouts[5] = {mBindless.layout(), mFrameSetLayout,
                                         mTlasSetLayout, mAOSamplerSetLayout,
                                         mEnvironmentSetLayout};
  // model (vertex) + textureIndex (fragment). Terrain reuses this layout.
  VkPushConstantRange pcRange{};
  pcRange.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  pcRange.offset = 0;
  pcRange.size = sizeof(ScenePush);

  VkPipelineLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutCi.setLayoutCount = 5;
  layoutCi.pSetLayouts = setLayouts;
  layoutCi.pushConstantRangeCount = 1;
  layoutCi.pPushConstantRanges = &pcRange;
  VK_CHECK(vkCreatePipelineLayout(mCtx->device(), &layoutCi, nullptr,
                                  &mScenePipelineLayout));

  VkShaderModule vert = loadShaderModule(shaderDir + "/mesh.vert.spv");
  VkShaderModule frag = loadShaderModule(shaderDir + "/mesh.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkVertexInputBindingDescription binding{};
  binding.binding = 0;
  binding.stride = sizeof(MeshVertex);
  binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription attrs[3]{};
  attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, pos)};
  attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal)};
  attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv)};
  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 3;
  vertexInput.pVertexAttributeDescriptions = attrs;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // Depth-test only, no write: a Z-prepass already wrote correct depth for
  // this frame (see createDepthPrepassPipelines) -- this also lets early-z
  // reject occluded fragments before they're shaded.
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_FALSE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments = &blendAttachment;

  VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT,
                               VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamics;

  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.colorAttachmentCount = 1;
  renderingCi.pColorAttachmentFormats = &mHdrFormat;
  renderingCi.depthAttachmentFormat = mDepthFormat;

  VkGraphicsPipelineCreateInfo pipelineCi{};
  pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCi.pNext = &renderingCi;
  pipelineCi.stageCount = 2;
  pipelineCi.pStages = stages;
  pipelineCi.pVertexInputState = &vertexInput;
  pipelineCi.pInputAssemblyState = &inputAssembly;
  pipelineCi.pViewportState = &viewport;
  pipelineCi.pRasterizationState = &raster;
  pipelineCi.pMultisampleState = &multisample;
  pipelineCi.pDepthStencilState = &depthStencil;
  pipelineCi.pColorBlendState = &colorBlend;
  pipelineCi.pDynamicState = &dynamicState;
  pipelineCi.layout = mScenePipelineLayout;

  uint64_t key = fnv1a64Str("mesh.shadowed.noZwrite");
  key = fnv1a64(&mHdrFormat, sizeof(mHdrFormat), key);
  key = fnv1a64(&mDepthFormat, sizeof(mDepthFormat), key);
  mScenePipeline = mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
    VkPipeline p = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                       nullptr, &p));
    return p;
  });

  depthStencil.depthWriteEnable = VK_TRUE;
  // Keep temporal-history alpha untouched: it stores WORLD depth, not opacity.
  blendAttachment.colorWriteMask &= ~VK_COLOR_COMPONENT_A_BIT;
  mViewmodelPipeline = mPipelineCache.getOrCreate(fnv1a64Str("mesh.viewmodel.depthwrite.rgb"),
    [&](VkPipelineCache pc) {
      VkPipeline p = VK_NULL_HANDLE;
      VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi, nullptr, &p));
      return p;
    });
  vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  vkDestroyShaderModule(mCtx->device(), frag, nullptr);
  return true;
}

bool VulkanRenderer::createTerrainChunkPipeline(const std::string &shaderDir) {
  // A regular indexed-mesh pipeline, structurally identical to
  // createScenePipeline() (same layout, depth state) -- terrain chunks are
  // ordinary CPU-built meshes now (Engine/Terrain/), shaded by
  // terrainChunk.frag (height/slope material blend). R1: the vertex stage
  // is terrainChunk.vert, not mesh.vert -- it reads a 4th vertex attribute
  // (MeshVertex::terrainParams: curvature/rockMask/wForest/wMountain) that
  // prop pipelines don't declare; see terrainChunk.vert's own comment for
  // why that has to be a separate shader file rather than added to the
  // shared mesh.vert.
  VkShaderModule vert = loadShaderModule(shaderDir + "/terrainChunk.vert.spv");
  VkShaderModule frag = loadShaderModule(shaderDir + "/terrainChunk.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkVertexInputBindingDescription binding{};
  binding.binding = 0;
  binding.stride = sizeof(MeshVertex);
  binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription attrs[5]{};
  attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, pos)};
  attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal)};
  attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv)};
  attrs[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
              offsetof(MeshVertex, terrainParams)};
  attrs[4] = {4, 0, VK_FORMAT_R32_SFLOAT,
              offsetof(MeshVertex, grassGroundOcclusion)};
  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 5;
  vertexInput.pVertexAttributeDescriptions = attrs;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // Depth-test only, no write -- see createScenePipeline's comment; the
  // shared depth prepass (createDepthPrepassPipelines) writes depth first.
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_FALSE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments = &blendAttachment;

  VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT,
                               VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamics;

  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.colorAttachmentCount = 1;
  renderingCi.pColorAttachmentFormats = &mHdrFormat;
  renderingCi.depthAttachmentFormat = mDepthFormat;

  VkGraphicsPipelineCreateInfo pipelineCi{};
  pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCi.pNext = &renderingCi;
  pipelineCi.stageCount = 2;
  pipelineCi.pStages = stages;
  pipelineCi.pVertexInputState = &vertexInput;
  pipelineCi.pInputAssemblyState = &inputAssembly;
  pipelineCi.pViewportState = &viewport;
  pipelineCi.pRasterizationState = &raster;
  pipelineCi.pMultisampleState = &multisample;
  pipelineCi.pDepthStencilState = &depthStencil;
  pipelineCi.pColorBlendState = &colorBlend;
  pipelineCi.pDynamicState = &dynamicState;
  pipelineCi.layout = mScenePipelineLayout;

  uint64_t key = fnv1a64Str("terrainChunk.shadowed.noZwrite");
  key = fnv1a64(&mHdrFormat, sizeof(mHdrFormat), key);
  key = fnv1a64(&mDepthFormat, sizeof(mDepthFormat), key);
  mTerrainChunkPipeline =
      mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
        VkPipeline p = VK_NULL_HANDLE;
        VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                           nullptr, &p));
        return p;
      });

  vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  vkDestroyShaderModule(mCtx->device(), frag, nullptr);
  return true;
}

bool VulkanRenderer::createVegetationPipeline(const std::string &shaderDir) {
  // GPU-instanced vegetation (Phase 3): structurally identical to
  // createScenePipeline() -- same layout, depth state, mesh.frag -- except
  // meshInstanced.vert and a 2nd vertex binding for the per-instance model
  // matrix (see setVegetationBatches()/drawFrame()'s vegetation loops).
  // R4: meshInstanced.frag (a fork of mesh.frag) rather than mesh.frag
  // itself -- it declares 2 extra varyings (color jitter, biome weights)
  // that mesh.vert's non-instanced callers never produce; see
  // meshInstanced.frag's own comment for why that has to be a separate
  // fragment shader, same reasoning as terrainChunk.vert (R1).
  VkShaderModule vert = loadShaderModule(shaderDir + "/meshInstanced.vert.spv");
  VkShaderModule frag = loadShaderModule(shaderDir + "/meshInstanced.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkVertexInputBindingDescription bindings[2]{};
  bindings[0].binding = 0;
  bindings[0].stride = sizeof(MeshVertex);
  bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  bindings[1].binding = 1;
  bindings[1].stride = sizeof(VegInstanceGpu);
  bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

  VkVertexInputAttributeDescription attrs[9]{};
  attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, pos)};
  attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal)};
  attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv)};
  // Instance model matrix: 4 vec4 attributes, one per column, at binding 1.
  attrs[3] = {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VegInstanceGpu, model) + 0 * sizeof(glm::vec4)};
  attrs[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VegInstanceGpu, model) + 1 * sizeof(glm::vec4)};
  attrs[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VegInstanceGpu, model) + 2 * sizeof(glm::vec4)};
  attrs[6] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VegInstanceGpu, model) + 3 * sizeof(glm::vec4)};
  // R4: per-instance color jitter + baked biome weights.
  attrs[7] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VegInstanceGpu, colorJitter)};
  attrs[8] = {8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VegInstanceGpu, biomeWeights)};

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount = 2;
  vertexInput.pVertexBindingDescriptions = bindings;
  vertexInput.vertexAttributeDescriptionCount = 9;
  vertexInput.pVertexAttributeDescriptions = attrs;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_FALSE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments = &blendAttachment;

  VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT,
                               VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamics;

  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.colorAttachmentCount = 1;
  renderingCi.pColorAttachmentFormats = &mHdrFormat;
  renderingCi.depthAttachmentFormat = mDepthFormat;

  VkGraphicsPipelineCreateInfo pipelineCi{};
  pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCi.pNext = &renderingCi;
  pipelineCi.stageCount = 2;
  pipelineCi.pStages = stages;
  pipelineCi.pVertexInputState = &vertexInput;
  pipelineCi.pInputAssemblyState = &inputAssembly;
  pipelineCi.pViewportState = &viewport;
  pipelineCi.pRasterizationState = &raster;
  pipelineCi.pMultisampleState = &multisample;
  pipelineCi.pDepthStencilState = &depthStencil;
  pipelineCi.pColorBlendState = &colorBlend;
  pipelineCi.pDynamicState = &dynamicState;
  pipelineCi.layout = mScenePipelineLayout;

  uint64_t key = fnv1a64Str("vegetation.instanced.shadowed.noZwrite");
  key = fnv1a64(&mHdrFormat, sizeof(mHdrFormat), key);
  key = fnv1a64(&mDepthFormat, sizeof(mDepthFormat), key);
  mVegetationPipeline =
      mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
        VkPipeline p = VK_NULL_HANDLE;
        VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                           nullptr, &p));
        return p;
      });

  vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  vkDestroyShaderModule(mCtx->device(), frag, nullptr);
  return true;
}

// Depth-only Z-prepass: the exact same vertex/task/mesh stages as the main
// scene/terrain pipelines (so depth values are bit-identical), just with no
// fragment shader and no color attachment. Feeds the SSAO pass its depth,
// and lets the main scene pass test-only (see createScenePipeline).
bool VulkanRenderer::createDepthPrepassPipelines(const std::string &shaderDir) {
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_TRUE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 0; // no color attachments in this pass

  VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT,
                               VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamics;

  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.colorAttachmentCount = 0;
  renderingCi.depthAttachmentFormat = mDepthFormat;

  // -- mesh (vertex-only, mesh.vert unchanged) --
  {
    VkShaderModule vert = loadShaderModule(shaderDir + "/mesh.vert.spv");
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vert;
    stage.pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(MeshVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, pos)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv)};
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkGraphicsPipelineCreateInfo pipelineCi{};
    pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCi.pNext = &renderingCi;
    pipelineCi.stageCount = 1;
    pipelineCi.pStages = &stage;
    pipelineCi.pVertexInputState = &vertexInput;
    pipelineCi.pInputAssemblyState = &inputAssembly;
    pipelineCi.pViewportState = &viewport;
    pipelineCi.pRasterizationState = &raster;
    pipelineCi.pMultisampleState = &multisample;
    pipelineCi.pDepthStencilState = &depthStencil;
    pipelineCi.pColorBlendState = &colorBlend;
    pipelineCi.pDynamicState = &dynamicState;
    pipelineCi.layout = mScenePipelineLayout;

    uint64_t key = fnv1a64Str("depthprepass.mesh");
    key = fnv1a64(&mDepthFormat, sizeof(mDepthFormat), key);
    mDepthPrepassPipeline =
        mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
          VkPipeline p = VK_NULL_HANDLE;
          VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                             nullptr, &p));
          return p;
        });

    // Cutout materials need the same alpha decision here as in mesh.frag.
    // Otherwise invisible pixels write depth, suppress objects behind them,
    // and create rectangular SSAO silhouettes even though the color pass
    // later discards those fragments.
    VkShaderModule alphaFrag =
        loadShaderModule(shaderDir + "/depthAlpha.frag.spv");
    VkPipelineShaderStageCreateInfo alphaStages[2]{};
    alphaStages[0] = stage;
    alphaStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    alphaStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    alphaStages[1].module = alphaFrag;
    alphaStages[1].pName = "main";
    pipelineCi.stageCount = 2;
    pipelineCi.pStages = alphaStages;
    key = fnv1a64Str("depthprepass.mesh.alpha");
    key = fnv1a64(&mDepthFormat, sizeof(mDepthFormat), key);
    mDepthAlphaPrepassPipeline =
        mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
          VkPipeline p = VK_NULL_HANDLE;
          VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                             nullptr, &p));
          return p;
        });
    vkDestroyShaderModule(mCtx->device(), alphaFrag, nullptr);
    vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  }
  // Terrain chunks share this same pipeline: they use the identical
  // MeshVertex vertex layout (Engine/Terrain/TerrainChunkMesher builds
  // ordinary MeshData), so no separate terrain depth-prepass pipeline is
  // needed anymore -- the generic instance loop in drawFrame() covers them.

  // -- vegetation (GPU-instanced, meshInstanced.vert, 2nd vertex binding) --
  {
    VkShaderModule vert = loadShaderModule(shaderDir + "/meshInstanced.vert.spv");
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vert;
    stage.pName = "main";

    VkVertexInputBindingDescription bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].stride = sizeof(MeshVertex);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(VegInstanceGpu);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    // Depth-only: still must declare every attribute meshInstanced.vert
    // reads (colorJitter/biomeWeights included), even though this pass has
    // no fragment stage to consume them -- the vertex shader is shared with
    // the color pipeline above and binds the exact same per-instance buffer.
    VkVertexInputAttributeDescription attrs[9]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, pos)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv)};
    attrs[3] = {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VegInstanceGpu, model) + 0 * sizeof(glm::vec4)};
    attrs[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VegInstanceGpu, model) + 1 * sizeof(glm::vec4)};
    attrs[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VegInstanceGpu, model) + 2 * sizeof(glm::vec4)};
    attrs[6] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VegInstanceGpu, model) + 3 * sizeof(glm::vec4)};
    attrs[7] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VegInstanceGpu, colorJitter)};
    attrs[8] = {8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VegInstanceGpu, biomeWeights)};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = 9;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkGraphicsPipelineCreateInfo pipelineCi{};
    pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCi.pNext = &renderingCi;
    pipelineCi.stageCount = 1;
    pipelineCi.pStages = &stage;
    pipelineCi.pVertexInputState = &vertexInput;
    pipelineCi.pInputAssemblyState = &inputAssembly;
    pipelineCi.pViewportState = &viewport;
    pipelineCi.pRasterizationState = &raster;
    pipelineCi.pMultisampleState = &multisample;
    pipelineCi.pDepthStencilState = &depthStencil;
    pipelineCi.pColorBlendState = &colorBlend;
    pipelineCi.pDynamicState = &dynamicState;
    pipelineCi.layout = mScenePipelineLayout;

    uint64_t key = fnv1a64Str("depthprepass.vegetation");
    key = fnv1a64(&mDepthFormat, sizeof(mDepthFormat), key);
    mVegetationDepthPrepassPipeline =
        mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
          VkPipeline p = VK_NULL_HANDLE;
          VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                             nullptr, &p));
          return p;
        });

    VkShaderModule alphaFrag =
        loadShaderModule(shaderDir + "/depthAlpha.frag.spv");
    VkPipelineShaderStageCreateInfo alphaStages[2]{};
    alphaStages[0] = stage;
    alphaStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    alphaStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    alphaStages[1].module = alphaFrag;
    alphaStages[1].pName = "main";
    pipelineCi.stageCount = 2;
    pipelineCi.pStages = alphaStages;
    key = fnv1a64Str("depthprepass.vegetation.alpha");
    key = fnv1a64(&mDepthFormat, sizeof(mDepthFormat), key);
    mVegetationDepthAlphaPrepassPipeline =
        mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
          VkPipeline p = VK_NULL_HANDLE;
          VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                             nullptr, &p));
          return p;
        });
    vkDestroyShaderModule(mCtx->device(), alphaFrag, nullptr);
    vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  }
  return true;
}

bool VulkanRenderer::createSkyPipeline(const std::string &shaderDir) {
  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  pcRange.offset = 0;
  pcRange.size = sizeof(SkyPush);

  // Set 1 is the half-res volumetric cloud buffer, which sky.frag blends into
  // its radiance before grading. It is declared unconditionally so there is
  // one sky pipeline layout rather than two: when the cloud assets failed to
  // load the set is never bound and the shader is told so through
  // passFlags.z, which short-circuits the sample.
  VkDescriptorSetLayout skySetLayouts[3] = {mFrameSetLayout, mAOSamplerSetLayout,mAtmosphere.layout()};
  VkPipelineLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutCi.setLayoutCount = 3;
  layoutCi.pSetLayouts = skySetLayouts;
  layoutCi.pushConstantRangeCount = 1;
  layoutCi.pPushConstantRanges = &pcRange;
  VK_CHECK(vkCreatePipelineLayout(mCtx->device(), &layoutCi, nullptr,
                                  &mSkyPipelineLayout));

  VkShaderModule vert = loadShaderModule(shaderDir + "/sky.vert.spv");
  VkShaderModule frag = loadShaderModule(shaderDir + "/sky.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // Sky is a background: no depth test or write (drawn before geometry).
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_FALSE;
  depthStencil.depthWriteEnable = VK_FALSE;

  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments = &blendAttachment;

  VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT,
                               VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamics;

  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.colorAttachmentCount = 1;
  renderingCi.pColorAttachmentFormats = &mHdrFormat;
  renderingCi.depthAttachmentFormat = mDepthFormat; // pass has a depth attachment

  VkGraphicsPipelineCreateInfo pipelineCi{};
  pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCi.pNext = &renderingCi;
  pipelineCi.stageCount = 2;
  pipelineCi.pStages = stages;
  pipelineCi.pVertexInputState = &vertexInput;
  pipelineCi.pInputAssemblyState = &inputAssembly;
  pipelineCi.pViewportState = &viewport;
  pipelineCi.pRasterizationState = &raster;
  pipelineCi.pMultisampleState = &multisample;
  pipelineCi.pDepthStencilState = &depthStencil;
  pipelineCi.pColorBlendState = &colorBlend;
  pipelineCi.pDynamicState = &dynamicState;
  pipelineCi.layout = mSkyPipelineLayout;

  uint64_t key = fnv1a64Str("sky.atmosphere");
  key = fnv1a64(&mHdrFormat, sizeof(mHdrFormat), key);
  mSkyPipeline = mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
    VkPipeline p = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                       nullptr, &p));
    return p;
  });

  vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  vkDestroyShaderModule(mCtx->device(), frag, nullptr);
  return true;
}

bool VulkanRenderer::createLinePipeline(const std::string &shaderDir) {
  VkPushConstantRange pcRange{};
  // Both stages: the vertex shader transforms, the fragment shader fades.
  pcRange.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  pcRange.offset = 0;
  pcRange.size = sizeof(LinePush);

  // No descriptor sets at all: everything the shader needs is in the push
  // constant, so recording this mid-scene-pass cannot disturb the bindless
  // table / frame UBO / TLAS bindings the mesh and terrain draws rely on.
  VkPipelineLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutCi.setLayoutCount = 0;
  layoutCi.pushConstantRangeCount = 1;
  layoutCi.pPushConstantRanges = &pcRange;
  VK_CHECK(vkCreatePipelineLayout(mCtx->device(), &layoutCi, nullptr,
                                  &mLinePipelineLayout));

  VkShaderModule vert = loadShaderModule(shaderDir + "/line.vert.spv");
  VkShaderModule frag = loadShaderModule(shaderDir + "/line.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkVertexInputBindingDescription binding{};
  binding.binding = 0;
  binding.stride = sizeof(DebugLineVertex);
  binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

  VkVertexInputAttributeDescription attributes[2]{};
  attributes[0].location = 0;
  attributes[0].binding = 0;
  attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
  attributes[0].offset = offsetof(DebugLineVertex, pos);
  attributes[1].location = 1;
  attributes[1].binding = 0;
  attributes[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
  attributes[1].offset = offsetof(DebugLineVertex, color);

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 2;
  vertexInput.pVertexAttributeDescriptions = attributes;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  // 1.0 is the only width guaranteed without the wideLines feature.
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // Tested so the grid is occluded by hills and geometry -- the whole point
  // of a real pipeline over ImGui's foreground draw list. Not written, so a
  // line never hides anything drawn after it.
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_FALSE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.blendEnable = VK_TRUE;
  blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
  blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments = &blendAttachment;

  VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT,
                               VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamics;

  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.colorAttachmentCount = 1;
  renderingCi.pColorAttachmentFormats = &mHdrFormat;
  renderingCi.depthAttachmentFormat = mDepthFormat;

  VkGraphicsPipelineCreateInfo pipelineCi{};
  pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCi.pNext = &renderingCi;
  pipelineCi.stageCount = 2;
  pipelineCi.pStages = stages;
  pipelineCi.pVertexInputState = &vertexInput;
  pipelineCi.pInputAssemblyState = &inputAssembly;
  pipelineCi.pViewportState = &viewport;
  pipelineCi.pRasterizationState = &raster;
  pipelineCi.pMultisampleState = &multisample;
  pipelineCi.pDepthStencilState = &depthStencil;
  pipelineCi.pColorBlendState = &colorBlend;
  pipelineCi.pDynamicState = &dynamicState;
  pipelineCi.layout = mLinePipelineLayout;

  uint64_t key = fnv1a64Str("editor.lines");
  key = fnv1a64(&mHdrFormat, sizeof(mHdrFormat), key);
  mLinePipeline = mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
    VkPipeline p = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                       nullptr, &p));
    return p;
  });

  vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  vkDestroyShaderModule(mCtx->device(), frag, nullptr);

  mLineBuffers.assign(kFramesInFlight, VK_NULL_HANDLE);
  mLineAllocs.assign(kFramesInFlight, VK_NULL_HANDLE);
  mLineMapped.assign(kFramesInFlight, nullptr);
  mLineCapacity.assign(kFramesInFlight, 0);
  return true;
}

void VulkanRenderer::setDebugLines(const std::vector<DebugLineVertex> &lines) {
  mDebugLines = lines;
}

bool VulkanRenderer::createTonemapPipeline(const std::string &shaderDir) {
  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  pcRange.offset = 0;
  pcRange.size = sizeof(TonemapPush);

  VkPipelineLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  VkDescriptorSetLayout layouts[2] = {mTonemapSetLayout, mFrameSetLayout};
  layoutCi.setLayoutCount = 2;
  layoutCi.pSetLayouts = layouts;
  layoutCi.pushConstantRangeCount = 1;
  layoutCi.pPushConstantRanges = &pcRange;
  VK_CHECK(vkCreatePipelineLayout(mCtx->device(), &layoutCi, nullptr,
                                  &mTonemapPipelineLayout));

  VkShaderModule vert = loadShaderModule(shaderDir + "/tonemap.vert.spv");
  VkShaderModule frag = loadShaderModule(shaderDir + "/tonemap.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments = &blendAttachment;

  VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT,
                               VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamics;

  const VkFormat swapFormat = mSwapchain.imageFormat();
  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.colorAttachmentCount = 1;
  renderingCi.pColorAttachmentFormats = &swapFormat;

  VkGraphicsPipelineCreateInfo pipelineCi{};
  pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCi.pNext = &renderingCi;
  pipelineCi.stageCount = 2;
  pipelineCi.pStages = stages;
  pipelineCi.pVertexInputState = &vertexInput;
  pipelineCi.pInputAssemblyState = &inputAssembly;
  pipelineCi.pViewportState = &viewport;
  pipelineCi.pRasterizationState = &raster;
  pipelineCi.pMultisampleState = &multisample;
  pipelineCi.pDepthStencilState = &depthStencil;
  pipelineCi.pColorBlendState = &colorBlend;
  pipelineCi.pDynamicState = &dynamicState;
  pipelineCi.layout = mTonemapPipelineLayout;

  uint64_t key = fnv1a64Str("tonemap");
  key = fnv1a64(&swapFormat, sizeof(swapFormat), key);
  mTonemapPipeline = mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
    VkPipeline p = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                       nullptr, &p));
    return p;
  });

  vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  vkDestroyShaderModule(mCtx->device(), frag, nullptr);
  return true;
}

// Shared by createSSAOPipeline/createBlurPipeline: both are fullscreen-
// triangle passes (reusing tonemap.vert) into a single R8 color attachment,
// no depth test, one sampler input at set 0 binding 0.
static void fullscreenAOPipelineState(
    VkPipelineVertexInputStateCreateInfo &vertexInput,
    VkPipelineInputAssemblyStateCreateInfo &inputAssembly,
    VkPipelineViewportStateCreateInfo &viewport,
    VkPipelineRasterizationStateCreateInfo &raster,
    VkPipelineMultisampleStateCreateInfo &multisample,
    VkPipelineDepthStencilStateCreateInfo &depthStencil,
    VkPipelineColorBlendAttachmentState &blendAttachment,
    VkPipelineColorBlendStateCreateInfo &colorBlend,
    VkPipelineDynamicStateCreateInfo &dynamicState, VkDynamicState *dynamics) {
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments = &blendAttachment;
  dynamics[0] = VK_DYNAMIC_STATE_VIEWPORT;
  dynamics[1] = VK_DYNAMIC_STATE_SCISSOR;
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamics;
}

bool VulkanRenderer::createSSAOPipeline(const std::string &shaderDir) {
  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  pcRange.offset = 0;
  pcRange.size = sizeof(SSAOPush);

  VkPipelineLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutCi.setLayoutCount = 1;
  layoutCi.pSetLayouts = &mAOSamplerSetLayout;
  layoutCi.pushConstantRangeCount = 1;
  layoutCi.pPushConstantRanges = &pcRange;
  VK_CHECK(vkCreatePipelineLayout(mCtx->device(), &layoutCi, nullptr,
                                  &mSSAOPipelineLayout));

  VkShaderModule vert = loadShaderModule(shaderDir + "/tonemap.vert.spv");
  VkShaderModule frag = loadShaderModule(shaderDir + "/ssao.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  VkPipelineViewportStateCreateInfo viewport{};
  VkPipelineRasterizationStateCreateInfo raster{};
  VkPipelineMultisampleStateCreateInfo multisample{};
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  VkPipelineColorBlendAttachmentState blendAttachment{};
  VkPipelineColorBlendStateCreateInfo colorBlend{};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  VkDynamicState dynamics[2]{};
  fullscreenAOPipelineState(vertexInput, inputAssembly, viewport, raster,
                            multisample, depthStencil, blendAttachment,
                            colorBlend, dynamicState, dynamics);

  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.colorAttachmentCount = 1;
  renderingCi.pColorAttachmentFormats = &mAOFormat;

  VkGraphicsPipelineCreateInfo pipelineCi{};
  pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCi.pNext = &renderingCi;
  pipelineCi.stageCount = 2;
  pipelineCi.pStages = stages;
  pipelineCi.pVertexInputState = &vertexInput;
  pipelineCi.pInputAssemblyState = &inputAssembly;
  pipelineCi.pViewportState = &viewport;
  pipelineCi.pRasterizationState = &raster;
  pipelineCi.pMultisampleState = &multisample;
  pipelineCi.pDepthStencilState = &depthStencil;
  pipelineCi.pColorBlendState = &colorBlend;
  pipelineCi.pDynamicState = &dynamicState;
  pipelineCi.layout = mSSAOPipelineLayout;

  uint64_t key = fnv1a64Str("ssao");
  key = fnv1a64(&mAOFormat, sizeof(mAOFormat), key);
  mSSAOPipeline = mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
    VkPipeline p = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                       nullptr, &p));
    return p;
  });

  vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  vkDestroyShaderModule(mCtx->device(), frag, nullptr);
  return true;
}

bool VulkanRenderer::createTemporalPipeline(const std::string &shaderDir) {
  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  pcRange.offset = 0;
  pcRange.size = sizeof(glm::mat4) * 2;

  VkPipelineLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutCi.setLayoutCount = 1;
  layoutCi.pSetLayouts = &mTonemapSetLayout;
  layoutCi.pushConstantRangeCount = 1;
  layoutCi.pPushConstantRanges = &pcRange;
  VK_CHECK(vkCreatePipelineLayout(mCtx->device(), &layoutCi, nullptr,
                                  &mTemporalPipelineLayout));

  VkShaderModule vert = loadShaderModule(shaderDir + "/tonemap.vert.spv");
  VkShaderModule frag = loadShaderModule(shaderDir + "/temporal.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  VkPipelineViewportStateCreateInfo viewport{};
  VkPipelineRasterizationStateCreateInfo raster{};
  VkPipelineMultisampleStateCreateInfo multisample{};
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  VkPipelineColorBlendAttachmentState blendAttachment{};
  VkPipelineColorBlendStateCreateInfo colorBlend{};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  VkDynamicState dynamics[2]{};
  fullscreenAOPipelineState(vertexInput, inputAssembly, viewport, raster,
                            multisample, depthStencil, blendAttachment,
                            colorBlend, dynamicState, dynamics);

  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.colorAttachmentCount = 1;
  renderingCi.pColorAttachmentFormats = &mHdrFormat;

  VkGraphicsPipelineCreateInfo pipelineCi{};
  pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCi.pNext = &renderingCi;
  pipelineCi.stageCount = 2;
  pipelineCi.pStages = stages;
  pipelineCi.pVertexInputState = &vertexInput;
  pipelineCi.pInputAssemblyState = &inputAssembly;
  pipelineCi.pViewportState = &viewport;
  pipelineCi.pRasterizationState = &raster;
  pipelineCi.pMultisampleState = &multisample;
  pipelineCi.pDepthStencilState = &depthStencil;
  pipelineCi.pColorBlendState = &colorBlend;
  pipelineCi.pDynamicState = &dynamicState;
  pipelineCi.layout = mTemporalPipelineLayout;

  uint64_t key = fnv1a64Str("temporal.resolve");
  key = fnv1a64(&mHdrFormat, sizeof(mHdrFormat), key);
  mTemporalPipeline = mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
    VkPipeline p = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                       nullptr, &p));
    return p;
  });

  vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  vkDestroyShaderModule(mCtx->device(), frag, nullptr);
  return true;
}

bool VulkanRenderer::createBlurPipeline(const std::string &shaderDir) {
  // Set 0: full-res raw AO (mBlurInputSets). Set 1: full-res depth
  // (mSSAODepthSets, reused from the SSAO pass -- same layout shape, same
  // image) for the bilateral upsample weight. Both use mAOSamplerSetLayout.
  VkDescriptorSetLayout blurSetLayouts[2] = {mAOSamplerSetLayout,
                                             mAOSamplerSetLayout};
  VkPushConstantRange blurPcRange{};
  blurPcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  blurPcRange.offset = 0;
  blurPcRange.size = sizeof(BlurPush);

  VkPipelineLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutCi.setLayoutCount = 2;
  layoutCi.pSetLayouts = blurSetLayouts;
  layoutCi.pushConstantRangeCount = 1;
  layoutCi.pPushConstantRanges = &blurPcRange;
  VK_CHECK(vkCreatePipelineLayout(mCtx->device(), &layoutCi, nullptr,
                                  &mBlurPipelineLayout));

  VkShaderModule vert = loadShaderModule(shaderDir + "/tonemap.vert.spv");
  VkShaderModule frag = loadShaderModule(shaderDir + "/blur.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  VkPipelineViewportStateCreateInfo viewport{};
  VkPipelineRasterizationStateCreateInfo raster{};
  VkPipelineMultisampleStateCreateInfo multisample{};
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  VkPipelineColorBlendAttachmentState blendAttachment{};
  VkPipelineColorBlendStateCreateInfo colorBlend{};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  VkDynamicState dynamics[2]{};
  fullscreenAOPipelineState(vertexInput, inputAssembly, viewport, raster,
                            multisample, depthStencil, blendAttachment,
                            colorBlend, dynamicState, dynamics);

  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.colorAttachmentCount = 1;
  renderingCi.pColorAttachmentFormats = &mAOFormat;

  VkGraphicsPipelineCreateInfo pipelineCi{};
  pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCi.pNext = &renderingCi;
  pipelineCi.stageCount = 2;
  pipelineCi.pStages = stages;
  pipelineCi.pVertexInputState = &vertexInput;
  pipelineCi.pInputAssemblyState = &inputAssembly;
  pipelineCi.pViewportState = &viewport;
  pipelineCi.pRasterizationState = &raster;
  pipelineCi.pMultisampleState = &multisample;
  pipelineCi.pDepthStencilState = &depthStencil;
  pipelineCi.pColorBlendState = &colorBlend;
  pipelineCi.pDynamicState = &dynamicState;
  pipelineCi.layout = mBlurPipelineLayout;

  uint64_t key = fnv1a64Str("ssao.blur");
  key = fnv1a64(&mAOFormat, sizeof(mAOFormat), key);
  mBlurPipeline = mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
    VkPipeline p = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                       nullptr, &p));
    return p;
  });

  vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  vkDestroyShaderModule(mCtx->device(), frag, nullptr);
  return true;
}

// Env-cubemap face pass: the sky shaders again (same mSkyPipelineLayout /
// SkyPush -- drawFrame zeroes the disc/star params), but rendering to a
// bare color attachment (the 128^2 face views have no depth image).
bool VulkanRenderer::createEnvMapPipeline(const std::string &shaderDir) {
  VkShaderModule vert = loadShaderModule(shaderDir + "/sky.vert.spv");
  VkShaderModule frag = loadShaderModule(shaderDir + "/sky.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  VkPipelineViewportStateCreateInfo viewport{};
  VkPipelineRasterizationStateCreateInfo raster{};
  VkPipelineMultisampleStateCreateInfo multisample{};
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  VkPipelineColorBlendAttachmentState blendAttachment{};
  VkPipelineColorBlendStateCreateInfo colorBlend{};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  VkDynamicState dynamics[2]{};
  fullscreenAOPipelineState(vertexInput, inputAssembly, viewport, raster,
                            multisample, depthStencil, blendAttachment,
                            colorBlend, dynamicState, dynamics);

  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.colorAttachmentCount = 1;
  renderingCi.pColorAttachmentFormats = &mHdrFormat;

  VkGraphicsPipelineCreateInfo pipelineCi{};
  pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCi.pNext = &renderingCi;
  pipelineCi.stageCount = 2;
  pipelineCi.pStages = stages;
  pipelineCi.pVertexInputState = &vertexInput;
  pipelineCi.pInputAssemblyState = &inputAssembly;
  pipelineCi.pViewportState = &viewport;
  pipelineCi.pRasterizationState = &raster;
  pipelineCi.pMultisampleState = &multisample;
  pipelineCi.pDepthStencilState = &depthStencil;
  pipelineCi.pColorBlendState = &colorBlend;
  pipelineCi.pDynamicState = &dynamicState;
  pipelineCi.layout = mSkyPipelineLayout;

  uint64_t key = fnv1a64Str("sky.envmap");
  key = fnv1a64(&mHdrFormat, sizeof(mHdrFormat), key);
  mEnvMapPipeline = mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
    VkPipeline p = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                       nullptr, &p));
    return p;
  });

  vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  vkDestroyShaderModule(mCtx->device(), frag, nullptr);
  return true;
}

// Volumetric god-ray pipelines: the half-res ray-query march (sets: depth /
// frame UBO / TLAS) and the depth-aware additive composite onto the HDR
// target (sets: scatter / depth).
bool VulkanRenderer::createVolumetricPipelines(const std::string &shaderDir) {
  {
    VkDescriptorSetLayout setLayouts[3] = {mAOSamplerSetLayout, mFrameSetLayout,
                                           mTlasSetLayout};
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(VolumetricPush);
    VkPipelineLayoutCreateInfo layoutCi{};
    layoutCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCi.setLayoutCount = 3;
    layoutCi.pSetLayouts = setLayouts;
    layoutCi.pushConstantRangeCount = 1;
    layoutCi.pPushConstantRanges = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(mCtx->device(), &layoutCi, nullptr,
                                    &mVolumetricPipelineLayout));
  }
  {
    VkDescriptorSetLayout setLayouts[2] = {mAOSamplerSetLayout,
                                           mAOSamplerSetLayout};
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(VolCompositePush);
    VkPipelineLayoutCreateInfo layoutCi{};
    layoutCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCi.setLayoutCount = 2;
    layoutCi.pSetLayouts = setLayouts;
    layoutCi.pushConstantRangeCount = 1;
    layoutCi.pPushConstantRanges = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(mCtx->device(), &layoutCi, nullptr,
                                    &mVolCompositePipelineLayout));
  }

  auto buildFullscreen = [&](const char *fragName, VkPipelineLayout layout,
                             bool additive, const char *cacheTag) {
    VkShaderModule vert = loadShaderModule(shaderDir + "/sky.vert.spv");
    VkShaderModule frag =
        loadShaderModule(shaderDir + "/" + std::string(fragName) + ".spv");
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    VkPipelineViewportStateCreateInfo viewport{};
    VkPipelineRasterizationStateCreateInfo raster{};
    VkPipelineMultisampleStateCreateInfo multisample{};
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    VkPipelineColorBlendAttachmentState blendAttachment{};
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    VkDynamicState dynamics[2]{};
    fullscreenAOPipelineState(vertexInput, inputAssembly, viewport, raster,
                              multisample, depthStencil, blendAttachment,
                              colorBlend, dynamicState, dynamics);
    if (additive) {
      // Composite ADDS in-scattered light onto the shaded scene.
      blendAttachment.blendEnable = VK_TRUE;
      blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
      blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
      blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
      blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineRenderingCreateInfo renderingCi{};
    renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingCi.colorAttachmentCount = 1;
    renderingCi.pColorAttachmentFormats = &mHdrFormat;

    VkGraphicsPipelineCreateInfo pipelineCi{};
    pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCi.pNext = &renderingCi;
    pipelineCi.stageCount = 2;
    pipelineCi.pStages = stages;
    pipelineCi.pVertexInputState = &vertexInput;
    pipelineCi.pInputAssemblyState = &inputAssembly;
    pipelineCi.pViewportState = &viewport;
    pipelineCi.pRasterizationState = &raster;
    pipelineCi.pMultisampleState = &multisample;
    pipelineCi.pDepthStencilState = &depthStencil;
    pipelineCi.pColorBlendState = &colorBlend;
    pipelineCi.pDynamicState = &dynamicState;
    pipelineCi.layout = layout;

    uint64_t key = fnv1a64Str(cacheTag);
    key = fnv1a64(&mHdrFormat, sizeof(mHdrFormat), key);
    VkPipeline pipeline = mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
      VkPipeline p = VK_NULL_HANDLE;
      VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                         nullptr, &p));
      return p;
    });

    vkDestroyShaderModule(mCtx->device(), vert, nullptr);
    vkDestroyShaderModule(mCtx->device(), frag, nullptr);
    return pipeline;
  };

  mVolumetricPipeline = buildFullscreen("volumetric.frag",
                                        mVolumetricPipelineLayout, false,
                                        "volumetric.march");
  mVolCompositePipeline = buildFullscreen("volumetricComposite.frag",
                                          mVolCompositePipelineLayout, true,
                                          "volumetric.composite");
  return true;
}

// --- volumetric clouds -----------------------------------------------------
// Loads the two 128^3 noise volumes and the two 2D maps the cloud marcher
// samples. The volumes ship as .glvol (see Tools/pack_cloud_volumes.py): an
// 8-byte magic, four uint32 dimensions, then flat RGBA8 slice-major data --
// one fread and one vkCmdCopyBufferToImage instead of decoding 128 TGAs at
// every startup.
//
// A failure here is NOT fatal. mCloudNoiseReady stays false, drawFrame skips
// the cloud passes entirely, and the sky renders exactly as it did before the
// system existed -- a missing optional asset should not take the engine down.
bool VulkanRenderer::createCloudNoiseResources(const std::string &assetDir) {
  mCloudNoiseReady = false;

  // The descriptor plumbing is built whether or not the assets load, because
  // sky.frag samples the cloud buffer unconditionally (a dynamic branch would
  // still leave the binding statically used, and an unbound set is invalid).
  // With no assets the march simply never runs and the buffer stays cleared
  // to "no light, full transmittance", which blends to nothing.
  {
    VkDescriptorSetLayoutBinding bindings[5]{};
    for (uint32_t i = 0; i < 5; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutCi{};
    layoutCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCi.bindingCount = 5;
    layoutCi.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(mCtx->device(), &layoutCi, nullptr,
                                         &mCloudNoiseSetLayout));

    // Five noise images + one per frame in flight for the sky's input.
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 5 + kFramesInFlight;
    VkDescriptorPoolCreateInfo poolCi{};
    poolCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCi.maxSets = 1 + kFramesInFlight;
    poolCi.poolSizeCount = 1;
    poolCi.pPoolSizes = &poolSize;
    VK_CHECK(
        vkCreateDescriptorPool(mCtx->device(), &poolCi, nullptr, &mCloudPool));

    mCloudSampleSets.resize(kFramesInFlight);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
      VkDescriptorSetAllocateInfo alloc{};
      alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      alloc.descriptorPool = mCloudPool;
      alloc.descriptorSetCount = 1;
      alloc.pSetLayouts = &mAOSamplerSetLayout;
      VK_CHECK(
          vkAllocateDescriptorSets(mCtx->device(), &alloc, &mCloudSampleSets[i]));
    }
  }

  if (assetDir.empty())
    return true;
  const std::string dir = assetDir + "/clouds";

  // Sampler: REPEAT on every axis (the volumes tile by construction) with the
  // full mip range exposed, since the marcher raises the mip with distance to
  // keep the horizon from shimmering.
  {
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = VK_FILTER_LINEAR;
    ci.minFilter = VK_FILTER_LINEAR;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.maxLod = VK_LOD_CLAMP_NONE;
    ci.anisotropyEnable = VK_FALSE;
    VK_CHECK(vkCreateSampler(mCtx->device(), &ci, nullptr, &mCloudSampler));
  }

  // Shared upload: stage RGBA8, copy into mip 0, then blit the mip chain
  // down. 3D blits carry the depth extent, so the same loop covers both the
  // volumes and the flat maps.
  auto upload = [&](const uint8_t *pixels, uint32_t w, uint32_t h, uint32_t d,
                    VkImage *outImage, VmaAllocation *outAlloc,
                    VkImageView *outView) {
    const bool is3D = d > 1;
    uint32_t mips = 1;
    {
      uint32_t m = std::max(w, std::max(h, d));
      while (m > 1) {
        m >>= 1;
        ++mips;
      }
    }

    const VkDeviceSize byteSize =
        static_cast<VkDeviceSize>(w) * h * d * 4;
    VkBufferCreateInfo stagingCi{};
    stagingCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingCi.size = byteSize;
    stagingCi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo stagingAlloc{};
    stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingMem = VK_NULL_HANDLE;
    VmaAllocationInfo stagingInfo{};
    VK_CHECK(vmaCreateBuffer(mCtx->allocator(), &stagingCi, &stagingAlloc,
                             &staging, &stagingMem, &stagingInfo));
    std::memcpy(stagingInfo.pMappedData, pixels, static_cast<size_t>(byteSize));

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = is3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {w, h, d};
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(mCtx->allocator(), &ci, &alloc, outImage, outAlloc,
                            nullptr));
    VkImage image = *outImage;

    immediateSubmit([&](VkCommandBuffer cmd) {
      auto barrier = [&](uint32_t baseMip, uint32_t mipCount,
                         VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                         VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                         VkImageLayout oldLayout, VkImageLayout newLayout) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = oldLayout;
        b.newLayout = newLayout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.baseMipLevel = baseMip;
        b.subresourceRange.levelCount = mipCount;
        b.subresourceRange.layerCount = 1;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
      };

      barrier(0, mips, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
              VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

      VkBufferImageCopy region{};
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      region.imageSubresource.layerCount = 1;
      region.imageExtent = {w, h, d};
      vkCmdCopyBufferToImage(cmd, staging, image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

      int32_t mw = static_cast<int32_t>(w), mh = static_cast<int32_t>(h),
              md = static_cast<int32_t>(d);
      for (uint32_t level = 1; level < mips; ++level) {
        barrier(level - 1, 1, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = level - 1;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1] = {mw, mh, md};
        mw = std::max(mw / 2, 1);
        mh = std::max(mh / 2, 1);
        md = std::max(md / 2, 1);
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = level;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[1] = {mw, mh, md};
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
        barrier(level - 1, 1, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      }
      // The last mip was only ever a blit destination.
      barrier(mips - 1, 1, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
              VK_ACCESS_2_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    vmaDestroyBuffer(mCtx->allocator(), staging, stagingMem);

    VkImageViewCreateInfo viewCi{};
    viewCi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCi.image = image;
    viewCi.viewType = is3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    viewCi.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewCi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCi.subresourceRange.levelCount = mips;
    viewCi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(mCtx->device(), &viewCi, nullptr, outView));
  };

  auto loadVolume = [&](const std::string &path, VkImage *img,
                        VmaAllocation *alloc, VkImageView *view) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      std::fprintf(stderr, "[clouds] missing volume '%s'\n", path.c_str());
      return false;
    }
    char magic[8] = {};
    file.read(magic, 8);
    if (std::memcmp(magic, "GLVOL1\0\0", 8) != 0) {
      std::fprintf(stderr, "[clouds] '%s' is not a .glvol\n", path.c_str());
      return false;
    }
    uint32_t dims[4] = {};
    file.read(reinterpret_cast<char *>(dims), sizeof(dims));
    if (!file || dims[0] == 0 || dims[1] == 0 || dims[2] == 0 || dims[3] != 4) {
      std::fprintf(stderr, "[clouds] '%s' has a bad header\n", path.c_str());
      return false;
    }
    const size_t byteSize =
        static_cast<size_t>(dims[0]) * dims[1] * dims[2] * 4;
    std::vector<uint8_t> pixels(byteSize);
    file.read(reinterpret_cast<char *>(pixels.data()),
              static_cast<std::streamsize>(byteSize));
    if (static_cast<size_t>(file.gcount()) != byteSize) {
      std::fprintf(stderr, "[clouds] '%s' is truncated\n", path.c_str());
      return false;
    }
    upload(pixels.data(), dims[0], dims[1], dims[2], img, alloc, view);
    return true;
  };

  auto loadMap = [&](const std::string &path, VkImage *img, VmaAllocation *alloc,
                     VkImageView *view) {
    int w = 0, h = 0, ch = 0;
    stbi_uc *pixels = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
    if (!pixels) {
      std::fprintf(stderr, "[clouds] missing map '%s'\n", path.c_str());
      return false;
    }
    upload(pixels, static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1, img,
           alloc, view);
    stbi_image_free(pixels);
    return true;
  };

  if (!loadVolume(dir + "/cloud_shape.glvol", &mCloudShapeImage,
                  &mCloudShapeAlloc, &mCloudShapeView) ||
      !loadVolume(dir + "/nubis_detail.glvol", &mCloudDetailImage,
                  &mCloudDetailAlloc, &mCloudDetailView) ||
      !loadVolume(dir + "/cloud_detail_periodic.glvol", &mCloudPeriodicDetailImage,
                  &mCloudPeriodicDetailAlloc, &mCloudPeriodicDetailView) ||
      !loadMap(dir + "/weather.png", &mCloudWeatherImage, &mCloudWeatherAlloc,
               &mCloudWeatherView) ||
      !loadMap(dir + "/curl_noise.png", &mCloudCurlImage, &mCloudCurlAlloc,
               &mCloudCurlView)) {
    std::fprintf(stderr,
                 "[clouds] volumetric clouds disabled (assets not loaded)\n");
    return true;
  }

  VkDescriptorSetAllocateInfo setAlloc{};
  setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  setAlloc.descriptorPool = mCloudPool;
  setAlloc.descriptorSetCount = 1;
  setAlloc.pSetLayouts = &mCloudNoiseSetLayout;
  VK_CHECK(vkAllocateDescriptorSets(mCtx->device(), &setAlloc, &mCloudNoiseSet));

  VmaAllocationInfo periodicAllocation{};
  vmaGetAllocationInfo(mCtx->allocator(),mCloudPeriodicDetailAlloc,&periodicAllocation);
  mCloudPeriodicDetailBytes=periodicAllocation.size;
  VkImageView views[5] = {mCloudShapeView, mCloudDetailView, mCloudWeatherView,
                          mCloudCurlView,mCloudPeriodicDetailView};
  VkDescriptorImageInfo images[5]{};
  VkWriteDescriptorSet writes[5]{};
  for (uint32_t i = 0; i < 5; ++i) {
    images[i].sampler = mCloudSampler;
    images[i].imageView = views[i];
    images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = mCloudNoiseSet;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i].pImageInfo = &images[i];
  }
  vkUpdateDescriptorSets(mCtx->device(), 5, writes, 0, nullptr);

  mCloudNoiseReady = true;
  return true;
}

void VulkanRenderer::destroyCloudNoiseResources() {
  VkDevice device = mCtx->device();
  VkImageView views[5] = {mCloudShapeView, mCloudDetailView, mCloudWeatherView,
                          mCloudCurlView,mCloudPeriodicDetailView};
  for (VkImageView v : views)
    if (v)
      vkDestroyImageView(device, v, nullptr);
  if (mCloudShapeImage)
    vmaDestroyImage(mCtx->allocator(), mCloudShapeImage, mCloudShapeAlloc);
  if (mCloudDetailImage)
    vmaDestroyImage(mCtx->allocator(), mCloudDetailImage, mCloudDetailAlloc);
  if(mCloudPeriodicDetailImage)
    vmaDestroyImage(mCtx->allocator(),mCloudPeriodicDetailImage,mCloudPeriodicDetailAlloc);
  if (mCloudWeatherImage)
    vmaDestroyImage(mCtx->allocator(), mCloudWeatherImage, mCloudWeatherAlloc);
  if (mCloudCurlImage)
    vmaDestroyImage(mCtx->allocator(), mCloudCurlImage, mCloudCurlAlloc);
  if (mCloudSampler)
    vkDestroySampler(device, mCloudSampler, nullptr);
  if (mCloudPool)
    vkDestroyDescriptorPool(device, mCloudPool, nullptr);
  if (mCloudNoiseSetLayout)
    vkDestroyDescriptorSetLayout(device, mCloudNoiseSetLayout, nullptr);
  mCloudShapeView = mCloudDetailView = mCloudWeatherView = mCloudCurlView =
      VK_NULL_HANDLE;
  mCloudShapeImage = mCloudDetailImage = mCloudWeatherImage = mCloudCurlImage =
      VK_NULL_HANDLE;
  mCloudPeriodicDetailImage=VK_NULL_HANDLE;mCloudPeriodicDetailView=VK_NULL_HANDLE;
  mCloudPeriodicDetailBytes=0;
  mCloudSampler = VK_NULL_HANDLE;
  mCloudPool = VK_NULL_HANDLE;
  mCloudNoiseSetLayout = VK_NULL_HANDLE;
  mCloudNoiseSet = VK_NULL_HANDLE;
  mCloudSampleSets.clear();
  mCloudNoiseReady = false;
}

void VulkanRenderer::updateCloudSets() {
  if (mCloudSampleSets.empty() || mCloudViews.empty())
    return;
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkDescriptorImageInfo image{};
    image.sampler = mSampler;
    image.imageView = mCloudViews[i];
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = mCloudSampleSets[i];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(mCtx->device(), 1, &write, 0, nullptr);
  }
}

bool VulkanRenderer::createCloudPipelines(const std::string &shaderDir) {
  if (!mCloudNoiseReady)
    return true;
  {
    VkDescriptorSetLayout setLayouts[2] = {mCloudNoiseSetLayout, mFrameSetLayout};
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(CloudPush);
    VkPipelineLayoutCreateInfo layoutCi{};
    layoutCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCi.setLayoutCount = 2;
    layoutCi.pSetLayouts = setLayouts;
    layoutCi.pushConstantRangeCount = 1;
    layoutCi.pPushConstantRanges = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(mCtx->device(), &layoutCi, nullptr,
                                    &mCloudPipelineLayout));
  }
  auto build = [&](const char *fragName, VkPipelineLayout layout, bool overBlend,
                   const char *cacheTag) {
    VkShaderModule vert = loadShaderModule(shaderDir + "/sky.vert.spv");
    VkShaderModule frag =
        loadShaderModule(shaderDir + "/" + std::string(fragName) + ".spv");
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    VkPipelineViewportStateCreateInfo viewport{};
    VkPipelineRasterizationStateCreateInfo raster{};
    VkPipelineMultisampleStateCreateInfo multisample{};
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    VkPipelineColorBlendAttachmentState blendAttachment{};
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    VkDynamicState dynamics[2]{};
    fullscreenAOPipelineState(vertexInput, inputAssembly, viewport, raster,
                              multisample, depthStencil, blendAttachment,
                              colorBlend, dynamicState, dynamics);
    (void)overBlend;

    VkPipelineColorBlendAttachmentState cloudAttachments[]={blendAttachment,blendAttachment};
    colorBlend.attachmentCount=2;colorBlend.pAttachments=cloudAttachments;

    VkPipelineRenderingCreateInfo renderingCi{};
    renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    const VkFormat cloudFormats[]={mHdrFormat,VK_FORMAT_R32_SFLOAT};
    renderingCi.colorAttachmentCount = 2;
    renderingCi.pColorAttachmentFormats = cloudFormats;

    VkGraphicsPipelineCreateInfo pipelineCi{};
    pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCi.pNext = &renderingCi;
    pipelineCi.stageCount = 2;
    pipelineCi.pStages = stages;
    pipelineCi.pVertexInputState = &vertexInput;
    pipelineCi.pInputAssemblyState = &inputAssembly;
    pipelineCi.pViewportState = &viewport;
    pipelineCi.pRasterizationState = &raster;
    pipelineCi.pMultisampleState = &multisample;
    pipelineCi.pDepthStencilState = &depthStencil;
    pipelineCi.pColorBlendState = &colorBlend;
    pipelineCi.pDynamicState = &dynamicState;
    pipelineCi.layout = layout;

    uint64_t key = fnv1a64Str(cacheTag);
    key = fnv1a64(&mHdrFormat, sizeof(mHdrFormat), key);
    VkPipeline pipeline = mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
      VkPipeline p = VK_NULL_HANDLE;
      VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                         nullptr, &p));
      return p;
    });

    vkDestroyShaderModule(mCtx->device(), vert, nullptr);
    vkDestroyShaderModule(mCtx->device(), frag, nullptr);
    return pipeline;
  };

  mCloudPipeline = build("clouds.frag", mCloudPipelineLayout, false, "clouds.march.depth.v2");
  return true;
}

VulkanRenderer::UiTexture VulkanRenderer::createUiTexture(const uint8_t *rgba,
                                                         uint32_t w, uint32_t h) {
  UiTexture out;
  if (!rgba || w == 0 || h == 0)
    return out;
  // addTexture() already does the staging upload and layout transitions, but
  // it only hands back a bindless index and keeps the view private. UI needs
  // the view itself, so this repeats the creation rather than reaching into
  // the bindless table -- a few images, once.
  const VkDeviceSize byteSize = static_cast<VkDeviceSize>(w) * h * 4;
  VkBufferCreateInfo stagingCi{};
  stagingCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  stagingCi.size = byteSize;
  stagingCi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VmaAllocationCreateInfo stagingAlloc{};
  stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
  stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VkBuffer staging = VK_NULL_HANDLE;
  VmaAllocation stagingMem = VK_NULL_HANDLE;
  VmaAllocationInfo stagingInfo{};
  VK_CHECK(vmaCreateBuffer(mCtx->allocator(), &stagingCi, &stagingAlloc, &staging,
                           &stagingMem, &stagingInfo));
  std::memcpy(stagingInfo.pMappedData, rgba, static_cast<size_t>(byteSize));

  VkImageCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = VK_FORMAT_R8G8B8A8_UNORM;
  ci.extent = {w, h, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation mem = VK_NULL_HANDLE;
  VK_CHECK(vmaCreateImage(mCtx->allocator(), &ci, &alloc, &image, &mem, nullptr));

  immediateSubmit([&](VkCommandBuffer cmd) {
    imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  });
  vmaDestroyBuffer(mCtx->allocator(), staging, stagingMem);

  VkImageViewCreateInfo viewCi{};
  viewCi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewCi.image = image;
  viewCi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewCi.format = VK_FORMAT_R8G8B8A8_UNORM;
  viewCi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  viewCi.subresourceRange.levelCount = 1;
  viewCi.subresourceRange.layerCount = 1;
  VkImageView view = VK_NULL_HANDLE;
  VK_CHECK(vkCreateImageView(mCtx->device(), &viewCi, nullptr, &view));

  mUiImages.push_back(image);
  mUiAllocs.push_back(mem);
  mUiViews.push_back(view);
  out.view = view;
  out.sampler = mSampler;
  return out;
}

void VulkanRenderer::setWaterHeightField(const float *heights, uint32_t resolution,
                                         glm::vec2 worldOrigin, float worldSize) {
  if (!heights || resolution == 0)
    return;
  mWaterFieldOrigin = worldOrigin;
  mWaterFieldSize = worldSize;

  const VkDeviceSize byteSize =
      static_cast<VkDeviceSize>(resolution) * resolution * sizeof(float);

  if (mWaterFieldResolution != resolution) {
    // Resolution changes are rare (settings edits), so a full teardown is
    // fine -- but it must wait for frames in flight that may still be
    // sampling the old image.
    vkDeviceWaitIdle(mCtx->device());
    if (mWaterFieldView)
      vkDestroyImageView(mCtx->device(), mWaterFieldView, nullptr);
    if (mWaterFieldImage)
      vmaDestroyImage(mCtx->allocator(), mWaterFieldImage, mWaterFieldAlloc);
    mWaterFieldView = VK_NULL_HANDLE;
    mWaterFieldImage = VK_NULL_HANDLE;

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R32_SFLOAT;
    ci.extent = {resolution, resolution, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(mCtx->allocator(), &ci, &alloc, &mWaterFieldImage,
                            &mWaterFieldAlloc, nullptr));

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = mWaterFieldImage;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R32_SFLOAT;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(mCtx->device(), &view, nullptr, &mWaterFieldView));
    mWaterFieldResolution = resolution;
    mSnowWaterTex = mBindless.registerTexture(*mCtx, mWaterFieldView, mSampler);

    if (mWaterFieldSet == VK_NULL_HANDLE) {
      VkDescriptorPoolSize poolSize{};
      poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      poolSize.descriptorCount = 1;
      VkDescriptorPoolCreateInfo poolCi{};
      poolCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      poolCi.maxSets = 1;
      poolCi.poolSizeCount = 1;
      poolCi.pPoolSizes = &poolSize;
      VK_CHECK(vkCreateDescriptorPool(mCtx->device(), &poolCi, nullptr,
                                      &mWaterFieldPool));
      VkDescriptorSetAllocateInfo setAlloc{};
      setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      setAlloc.descriptorPool = mWaterFieldPool;
      setAlloc.descriptorSetCount = 1;
      setAlloc.pSetLayouts = &mAOSamplerSetLayout;
      VK_CHECK(vkAllocateDescriptorSets(mCtx->device(), &setAlloc, &mWaterFieldSet));
    }
    VkDescriptorImageInfo image{};
    image.sampler = mSampler;
    image.imageView = mWaterFieldView;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = mWaterFieldSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(mCtx->device(), 1, &write, 0, nullptr);
  }

  VkBufferCreateInfo stagingCi{};
  stagingCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  stagingCi.size = byteSize;
  stagingCi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VmaAllocationCreateInfo stagingAlloc{};
  stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
  stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VkBuffer staging = VK_NULL_HANDLE;
  VmaAllocation stagingMem = VK_NULL_HANDLE;
  VmaAllocationInfo stagingInfo{};
  VK_CHECK(vmaCreateBuffer(mCtx->allocator(), &stagingCi, &stagingAlloc, &staging,
                           &stagingMem, &stagingInfo));
  std::memcpy(stagingInfo.pMappedData, heights, static_cast<size_t>(byteSize));

  immediateSubmit([&](VkCommandBuffer cmd) {
    imageBarrier(cmd, mWaterFieldImage, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {resolution, resolution, 1};
    vkCmdCopyBufferToImage(cmd, staging, mWaterFieldImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    imageBarrier(cmd, mWaterFieldImage, VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  });
  vmaDestroyBuffer(mCtx->allocator(), staging, stagingMem);
}

// Water: one fullscreen pass, alpha-blended over the opaque HDR image.
// Sets are (0) the opaque-scene copy, (1) the prepass depth, (2) FrameData,
// (3) the sky cubemap -- the first two so the surface can refract what is
// behind it and march screen-space reflections, the last as the reflection
// fallback wherever that march leaves the screen.
//
// Alpha blend rather than additive (volumetrics) or opaque (everything else):
// water covers only the pixels where the plane is actually in front of the
// scene, and it returns coverage in alpha, so untouched pixels cost a blend
// with alpha 0 instead of a full-screen read-modify-write of the HDR image.
bool VulkanRenderer::createWaterPipeline(const std::string &shaderDir) {
  VkDescriptorSetLayout setLayouts[7] = {mAOSamplerSetLayout,
                                         mAOSamplerSetLayout, mFrameSetLayout,
                                         mEnvironmentSetLayout,
                                         mAOSamplerSetLayout, mTlasSetLayout, mAtmosphere.layout()};
  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  pcRange.offset = 0;
  pcRange.size = sizeof(WaterPush);
  VkPipelineLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutCi.setLayoutCount = 7;
  layoutCi.pSetLayouts = setLayouts;
  layoutCi.pushConstantRangeCount = 1;
  layoutCi.pPushConstantRanges = &pcRange;
  VK_CHECK(vkCreatePipelineLayout(mCtx->device(), &layoutCi, nullptr,
                                  &mWaterPipelineLayout));

  VkShaderModule vert = loadShaderModule(shaderDir + "/sky.vert.spv");
  VkShaderModule frag = loadShaderModule(shaderDir + "/water.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  VkPipelineViewportStateCreateInfo viewport{};
  VkPipelineRasterizationStateCreateInfo raster{};
  VkPipelineMultisampleStateCreateInfo multisample{};
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  VkPipelineColorBlendAttachmentState blendAttachment{};
  VkPipelineColorBlendStateCreateInfo colorBlend{};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  VkDynamicState dynamics[2]{};
  fullscreenAOPipelineState(vertexInput, inputAssembly, viewport, raster,
                            multisample, depthStencil, blendAttachment,
                            colorBlend, dynamicState, dynamics);
  blendAttachment.blendEnable = VK_TRUE;
  blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
  blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.colorAttachmentCount = 1;
  renderingCi.pColorAttachmentFormats = &mHdrFormat;

  VkGraphicsPipelineCreateInfo pipelineCi{};
  pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCi.pNext = &renderingCi;
  pipelineCi.stageCount = 2;
  pipelineCi.pStages = stages;
  pipelineCi.pVertexInputState = &vertexInput;
  pipelineCi.pInputAssemblyState = &inputAssembly;
  pipelineCi.pViewportState = &viewport;
  pipelineCi.pRasterizationState = &raster;
  pipelineCi.pMultisampleState = &multisample;
  pipelineCi.pDepthStencilState = &depthStencil;
  pipelineCi.pColorBlendState = &colorBlend;
  pipelineCi.pDynamicState = &dynamicState;
  pipelineCi.layout = mWaterPipelineLayout;

  uint64_t key = fnv1a64Str("water.surface");
  key = fnv1a64(&mHdrFormat, sizeof(mHdrFormat), key);
  mWaterPipeline = mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
    VkPipeline p = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                       nullptr, &p));
    return p;
  });

  vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  vkDestroyShaderModule(mCtx->device(), frag, nullptr);
  return true;
}

bool VulkanRenderer::createBloomExtractPipeline(const std::string &shaderDir) {
  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  pcRange.offset = 0;
  pcRange.size = sizeof(BloomExtractPush);

  VkPipelineLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutCi.setLayoutCount = 1;
  layoutCi.pSetLayouts = &mAOSamplerSetLayout;
  layoutCi.pushConstantRangeCount = 1;
  layoutCi.pPushConstantRanges = &pcRange;
  VK_CHECK(vkCreatePipelineLayout(mCtx->device(), &layoutCi, nullptr,
                                  &mBloomExtractPipelineLayout));

  VkShaderModule vert = loadShaderModule(shaderDir + "/tonemap.vert.spv");
  VkShaderModule frag = loadShaderModule(shaderDir + "/bloomExtract.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  VkPipelineViewportStateCreateInfo viewport{};
  VkPipelineRasterizationStateCreateInfo raster{};
  VkPipelineMultisampleStateCreateInfo multisample{};
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  VkPipelineColorBlendAttachmentState blendAttachment{};
  VkPipelineColorBlendStateCreateInfo colorBlend{};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  VkDynamicState dynamics[2]{};
  fullscreenAOPipelineState(vertexInput, inputAssembly, viewport, raster,
                            multisample, depthStencil, blendAttachment,
                            colorBlend, dynamicState, dynamics);

  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.colorAttachmentCount = 1;
  renderingCi.pColorAttachmentFormats = &mHdrFormat;

  VkGraphicsPipelineCreateInfo pipelineCi{};
  pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCi.pNext = &renderingCi;
  pipelineCi.stageCount = 2;
  pipelineCi.pStages = stages;
  pipelineCi.pVertexInputState = &vertexInput;
  pipelineCi.pInputAssemblyState = &inputAssembly;
  pipelineCi.pViewportState = &viewport;
  pipelineCi.pRasterizationState = &raster;
  pipelineCi.pMultisampleState = &multisample;
  pipelineCi.pDepthStencilState = &depthStencil;
  pipelineCi.pColorBlendState = &colorBlend;
  pipelineCi.pDynamicState = &dynamicState;
  pipelineCi.layout = mBloomExtractPipelineLayout;

  uint64_t key = fnv1a64Str("bloom.extract");
  key = fnv1a64(&mHdrFormat, sizeof(mHdrFormat), key);
  mBloomExtractPipeline =
      mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
        VkPipeline p = VK_NULL_HANDLE;
        VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                           nullptr, &p));
        return p;
      });

  vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  vkDestroyShaderModule(mCtx->device(), frag, nullptr);
  return true;
}

bool VulkanRenderer::createBloomBlurPipeline(const std::string &shaderDir) {
  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  pcRange.offset = 0;
  pcRange.size = sizeof(BloomBlurPush);

  VkPipelineLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutCi.setLayoutCount = 1;
  layoutCi.pSetLayouts = &mAOSamplerSetLayout;
  layoutCi.pushConstantRangeCount = 1;
  layoutCi.pPushConstantRanges = &pcRange;
  VK_CHECK(vkCreatePipelineLayout(mCtx->device(), &layoutCi, nullptr,
                                  &mBloomBlurPipelineLayout));

  VkShaderModule vert = loadShaderModule(shaderDir + "/tonemap.vert.spv");
  VkShaderModule frag = loadShaderModule(shaderDir + "/bloomBlur.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  VkPipelineViewportStateCreateInfo viewport{};
  VkPipelineRasterizationStateCreateInfo raster{};
  VkPipelineMultisampleStateCreateInfo multisample{};
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  VkPipelineColorBlendAttachmentState blendAttachment{};
  VkPipelineColorBlendStateCreateInfo colorBlend{};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  VkDynamicState dynamics[2]{};
  fullscreenAOPipelineState(vertexInput, inputAssembly, viewport, raster,
                            multisample, depthStencil, blendAttachment,
                            colorBlend, dynamicState, dynamics);

  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.colorAttachmentCount = 1;
  renderingCi.pColorAttachmentFormats = &mHdrFormat;

  VkGraphicsPipelineCreateInfo pipelineCi{};
  pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCi.pNext = &renderingCi;
  pipelineCi.stageCount = 2;
  pipelineCi.pStages = stages;
  pipelineCi.pVertexInputState = &vertexInput;
  pipelineCi.pInputAssemblyState = &inputAssembly;
  pipelineCi.pViewportState = &viewport;
  pipelineCi.pRasterizationState = &raster;
  pipelineCi.pMultisampleState = &multisample;
  pipelineCi.pDepthStencilState = &depthStencil;
  pipelineCi.pColorBlendState = &colorBlend;
  pipelineCi.pDynamicState = &dynamicState;
  pipelineCi.layout = mBloomBlurPipelineLayout;

  uint64_t key = fnv1a64Str("bloom.blur");
  key = fnv1a64(&mHdrFormat, sizeof(mHdrFormat), key);
  mBloomBlurPipeline = mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
    VkPipeline p = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                       nullptr, &p));
    return p;
  });

  vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  vkDestroyShaderModule(mCtx->device(), frag, nullptr);
  return true;
}

bool VulkanRenderer::loadMeshFromObj(const std::string &path, Mesh &outMesh) {
  MeshData data;
  if (!loadObj(path, data))
    return false;

  outMesh.indexCount = static_cast<uint32_t>(data.indices.size());
  outMesh.vertexCount = static_cast<uint32_t>(data.vertices.size());
  for (const MeshVertex &v : data.vertices) {
    if (!outMesh.hasBounds) {
      outMesh.boundsMin = outMesh.boundsMax = v.pos;
      outMesh.hasBounds = true;
    } else {
      outMesh.boundsMin = glm::min(outMesh.boundsMin, v.pos);
      outMesh.boundsMax = glm::max(outMesh.boundsMax, v.pos);
    }
  }

  // Mesh buffers also feed the BLAS: device-address + AS-build-input usage.
  const VkBufferUsageFlags asInput =
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  createDeviceLocalBuffer(data.vertices.data(),
                          data.vertices.size() * sizeof(MeshVertex),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | asInput,
                          outMesh.vertexBuffer, outMesh.vertexAlloc);
  createDeviceLocalBuffer(data.indices.data(),
                          data.indices.size() * sizeof(uint32_t),
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT | asInput,
                          outMesh.indexBuffer, outMesh.indexAlloc);

  std::vector<uint32_t> materialTexIndex(data.materials.size(),
                                         mDefaultTexIndex);
  for (size_t i = 0; i < data.materials.size(); ++i)
    if (!data.materials[i].diffuseTexturePath.empty())
      materialTexIndex[i] = loadTextureFile(data.materials[i].diffuseTexturePath);

  for (const SubMesh &sub : data.submeshes) {
    DrawItem item{};
    item.indexOffset = sub.indexOffset;
    item.indexCount = sub.indexCount;
    item.textureIndex =
        (sub.materialId >= 0 &&
         sub.materialId < static_cast<int>(materialTexIndex.size()))
            ? materialTexIndex[sub.materialId]
            : mDefaultTexIndex;
    outMesh.drawItems.push_back(item);
  }
  return true;
}

VulkanAccel::BlasInput VulkanRenderer::blasInputForMesh(const Mesh &mesh) const {
  auto deviceAddress = [&](VkBuffer buffer) {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(mCtx->device(), &info);
  };
  VulkanAccel::BlasInput in{};
  if (!mesh.vertexBuffer || !mesh.indexBuffer)
    return in; // dead slot: zeroed input, skipped by BLAS builders
  in.vertexAddress = deviceAddress(mesh.vertexBuffer);
  in.vertexCount = mesh.vertexCount;
  in.vertexStride = sizeof(MeshVertex);
  in.indexAddress = deviceAddress(mesh.indexBuffer);
  in.indexCount = mesh.indexCount;
  in.opaque = !mesh.alphaTested;
  return in;
}

glm::uvec4 VulkanRenderer::updateRtAlphaTable(uint32_t frame) {
  if (mRtAlphaBuffers.size() < kFramesInFlight)
    mRtAlphaBuffers.resize(kFramesInFlight);
  RtAlphaFrameBuffer &fb = mRtAlphaBuffers[frame];
  const auto packed = [&]() {
    if (!fb.address)
      return glm::uvec4(0u);
    return glm::uvec4(static_cast<uint32_t>(fb.address & 0xFFFFFFFFull),
                      static_cast<uint32_t>(fb.address >> 32), 0u, 0u);
  };
  if (fb.version == mRtAlphaVersion)
    return packed();

  std::vector<RtAlphaMeshGpu> meshes(mMeshes.size());
  bool any = false;
  for (size_t m = 0; m < mMeshes.size(); ++m) {
    RtAlphaMeshGpu rec{};
    if (mMeshes[m].alphaMaskBuffer) {
      VkBufferDeviceAddressInfo info{};
      info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
      info.buffer = mMeshes[m].alphaMaskBuffer;
      rec.masks = vkGetBufferDeviceAddress(mCtx->device(), &info);
      any = true;
    }
    meshes[m] = rec;
  }
  fb.version = mRtAlphaVersion;
  if (!any) {
    fb.address = 0; // keep the buffer for reuse; the shader sees "none"
    return glm::uvec4(0u);
  }
  const VkDeviceSize total = meshes.size() * sizeof(RtAlphaMeshGpu);
  if (total > fb.capacity) {
    // This frame slot's fence has signalled, so the old buffer is idle.
    if (fb.buffer)
      vmaDestroyBuffer(mCtx->allocator(), fb.buffer, fb.alloc);
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = std::max<VkDeviceSize>(total * 3 / 2, 4096);
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(mCtx->allocator(), &bci, &aci, &fb.buffer,
                             &fb.alloc, &info));
    fb.mapped = info.pMappedData;
    fb.capacity = bci.size;
    VkBufferDeviceAddressInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    ai.buffer = fb.buffer;
    fb.address = vkGetBufferDeviceAddress(mCtx->device(), &ai);
  }
  std::memcpy(fb.mapped, meshes.data(), static_cast<size_t>(total));
  vmaFlushAllocation(mCtx->allocator(), fb.alloc, 0, VK_WHOLE_SIZE);
  if (!fb.address) {
    VkBufferDeviceAddressInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    ai.buffer = fb.buffer;
    fb.address = vkGetBufferDeviceAddress(mCtx->device(), &ai);
  }
  return packed();
}

VulkanRenderer::MeshHandle
VulkanRenderer::createMeshFromObj(const std::string &path) {
  Mesh mesh;
  if (!loadMeshFromObj(path, mesh)) {
    std::fprintf(stderr, "[VulkanRHI] failed to load mesh '%s'\n", path.c_str());
    return UINT32_MAX;
  }
  const uint32_t indexCount = mesh.indexCount;
  const MeshHandle handle = acquireMeshSlot(std::move(mesh));
  std::fprintf(stderr, "[VulkanRHI] mesh %u: '%s' (%u indices)\n", handle,
               path.c_str(), indexCount);
  if (mSceneReady)
    mPendingBlasBuilds.emplace_back(handle, blasInputForMesh(mMeshes[handle]));
  return handle;
}

VulkanRenderer::MeshHandle
VulkanRenderer::createMeshFromData(const ::MeshData &data,
                                   const std::string &debugName) {
  Mesh mesh;
  if (!buildMeshFromData(data, debugName, mesh))
    return UINT32_MAX;

  const uint32_t indexCount = mesh.indexCount;
  const MeshHandle handle = acquireMeshSlot(std::move(mesh));
  std::fprintf(stderr, "[VulkanRHI] mesh %u: '%s' (%u indices, engine data)\n",
               handle, debugName.c_str(), indexCount);
  // Streaming path: queue the BLAS build for drawFrame()'s batched recording
  // (the vertex/index copies are pending in the same queue, ordered by a
  // barrier there). Init path: finalizeScene() builds every BLAS itself.
  if (mSceneReady)
    mPendingBlasBuilds.emplace_back(handle, blasInputForMesh(mMeshes[handle]));
  return handle;
}

bool VulkanRenderer::updateMeshFromData(MeshHandle handle,
                                        const ::MeshData &data) {
  if (handle >= mMeshes.size())
    return false;

  Mesh newMesh;
  if (!buildMeshFromData(data, "updated", newMesh))
    return false;

  // Retire the old buffers instead of destroying them -- frames still in
  // flight reference them (this used to be an unguarded destroy, and the
  // BLAS swap used to vkDeviceWaitIdle per brush stroke).
  Mesh &old = mMeshes[handle];
  ++mRtAlphaVersion;
  if (old.alphaMaskBuffer)
    mPendingGarbage.buffers.emplace_back(old.alphaMaskBuffer, old.alphaMaskAlloc);
  if (old.vertexBuffer)
    mPendingGarbage.buffers.emplace_back(old.vertexBuffer, old.vertexAlloc);
  if (old.indexBuffer)
    mPendingGarbage.buffers.emplace_back(old.indexBuffer, old.indexAlloc);
  old = std::move(newMesh);

  if (mSceneReady) {
    mPendingBlasBuilds.emplace_back(handle, blasInputForMesh(old));
    // Non-stale TLAS slots would keep referencing the retired BLAS address.
    markTlasAllStale();
  }
  return true;
}

void VulkanRenderer::destroyMesh(MeshHandle handle) {
  if (handle >= mMeshes.size())
    return;
  Mesh &mesh = mMeshes[handle];
  if (!mesh.vertexBuffer && !mesh.indexBuffer)
    return; // already dead
  ++mRtAlphaVersion;
  // Created and evicted within the same frame: drop the not-yet-recorded
  // copies into these buffers along with the buffers themselves.
  for (auto it = mPendingCopies.begin(); it != mPendingCopies.end();) {
    if (it->dst == mesh.vertexBuffer || it->dst == mesh.indexBuffer ||
        (mesh.alphaMaskBuffer && it->dst == mesh.alphaMaskBuffer))
      it = mPendingCopies.erase(it);
    else
      ++it;
  }
  if (mesh.vertexBuffer)
    mPendingGarbage.buffers.emplace_back(mesh.vertexBuffer, mesh.vertexAlloc);
  if (mesh.indexBuffer)
    mPendingGarbage.buffers.emplace_back(mesh.indexBuffer, mesh.indexAlloc);
  if (mesh.alphaMaskBuffer)
    mPendingGarbage.buffers.emplace_back(mesh.alphaMaskBuffer, mesh.alphaMaskAlloc);
  mesh = Mesh{};
  mAccel.releaseBlas(handle, mPendingGarbage);
  // Drop any not-yet-recorded BLAS build for this slot (created and evicted
  // within the same frame -- possible during fast streaming).
  for (auto it = mPendingBlasBuilds.begin(); it != mPendingBlasBuilds.end();) {
    if (it->first == handle)
      it = mPendingBlasBuilds.erase(it);
    else
      ++it;
  }
  mFreeMeshSlots.push_back(handle);
  markTlasAllStale();
}

bool VulkanRenderer::buildMeshFromData(const ::MeshData &data,
                                       const std::string &debugName,
                                       Mesh &outMesh) {
  // Flatten the engine submeshes into one vertex/index buffer with a
  // per-submesh draw item (engine MeshVertex is pos/uv/normal; ours is
  // pos/normal/uv).
  std::vector<MeshVertex> vertices;
  std::vector<uint32_t> indices;
  Mesh &mesh = outMesh;
  std::unordered_map<std::string, uint32_t> texByPath;
  // Solid-color fallbacks use the renderer-wide mSolidColorTex cache: with a
  // per-call cache every streamed terrain chunk minted its own identical 1x1
  // texture into the bindless array (thousands over a long session).
  std::unordered_map<uint32_t, uint32_t> &texByColor = mSolidColorTex;

  // srgb=false for linear (non-color) data: roughness/metallic/AO maps.
  // Cached per (path, srgb) since the same physical file could in principle
  // be requested both ways (e.g. reused as both albedo and a mask).
  // `solidColor` is the MaterialAsset::baseColor fallback used when there's
  // no texture path at all (procedural meshes: terrain, vegetation,
  // MeshPrimitives) -- without this, such meshes would always render at
  // mDefaultTexIndex's flat color regardless of baseColor, since nothing
  // else in this renderer ever reads baseColor.
  auto textureFor = [&](const std::string &path, bool srgb = true,
                       glm::vec4 solidColor = glm::vec4(-1.0f)) -> uint32_t {
    if (path.empty()) {
      if (solidColor.r < 0.0f)
        return mDefaultTexIndex;
      const uint8_t r = static_cast<uint8_t>(glm::clamp(solidColor.r, 0.0f, 1.0f) * 255.0f);
      const uint8_t g = static_cast<uint8_t>(glm::clamp(solidColor.g, 0.0f, 1.0f) * 255.0f);
      const uint8_t b = static_cast<uint8_t>(glm::clamp(solidColor.b, 0.0f, 1.0f) * 255.0f);
      const uint8_t a = static_cast<uint8_t>(glm::clamp(solidColor.a, 0.0f, 1.0f) * 255.0f);
      const uint32_t key = (static_cast<uint32_t>(r) << 24) |
                           (static_cast<uint32_t>(g) << 16) |
                           (static_cast<uint32_t>(b) << 8) | a;
      auto cached = texByColor.find(key);
      if (cached != texByColor.end())
        return cached->second;
      const uint8_t rgba[4] = {r, g, b, a};
      const uint32_t tex =
          addTexture(rgba, 1, 1, VK_FORMAT_R8G8B8A8_SRGB);
      texByColor[key] = tex;
      return tex;
    }
    const std::string cacheKey = path + (srgb ? "#srgb" : "#linear");
    auto it = texByPath.find(cacheKey);
    if (it != texByPath.end())
      return it->second;

    uint32_t tex = mDefaultTexIndex;
    if (const ::MeshImage *img = data.findImage(path)) {
      // Embedded payload: expand 1/3-channel data to tightly packed RGBA.
      const size_t pixelCount = static_cast<size_t>(img->width) * img->height;
      std::vector<uint8_t> rgba(pixelCount * 4);
      const uint8_t *src = img->pixels.data();
      if (img->component == 4) {
        std::memcpy(rgba.data(), src, rgba.size());
      } else if (img->component == 3) {
        for (size_t i = 0; i < pixelCount; ++i) {
          rgba[i * 4 + 0] = src[i * 3 + 0];
          rgba[i * 4 + 1] = src[i * 3 + 1];
          rgba[i * 4 + 2] = src[i * 3 + 2];
          rgba[i * 4 + 3] = 255;
        }
      } else {
        for (size_t i = 0; i < pixelCount; ++i) {
          const uint8_t v = src[i * img->component];
          rgba[i * 4 + 0] = v;
          rgba[i * 4 + 1] = v;
          rgba[i * 4 + 2] = v;
          rgba[i * 4 + 3] = 255;
        }
      }
      tex = addTexture(rgba.data(), static_cast<uint32_t>(img->width),
                       static_cast<uint32_t>(img->height),
                       srgb ? VK_FORMAT_R8G8B8A8_SRGB
                            : VK_FORMAT_R8G8B8A8_UNORM,
                       // This stock grass atlas must retain gaps between its
                       // leaves. Tree-crown footprint growth merged the fine
                       // photographed blades into opaque fern-like clumps.
                       path.find("meadow_grass_atlas.png") == std::string::npos &&
                       path.find("woodland_grass_scan_rgba.png") == std::string::npos);
    } else {
      // Engine model UVs are raw (OBJ bottom-left origin); flip the file
      // like the GL engine does so sampling matches.
      tex = loadTextureFile(path, /*flipY=*/true, srgb);
    }
    texByPath[cacheKey] = tex;
    return tex;
  };

  std::vector<AlphaSource> alphaSources;
  for (const auto &sd : data.submeshes) {
    if (sd.vertices.empty())
      continue;
    const uint32_t base = static_cast<uint32_t>(vertices.size());

    DrawItem item{};
    item.indexOffset = static_cast<uint32_t>(indices.size());
    for (const auto &v : sd.vertices) {
      vertices.push_back({v.pos, v.normal, v.uv, v.terrainParams, v.grassGroundOcclusion});
      if (!mesh.hasBounds) {
        mesh.boundsMin = mesh.boundsMax = v.pos;
        mesh.hasBounds = true;
      } else {
        mesh.boundsMin = glm::min(mesh.boundsMin, v.pos);
        mesh.boundsMax = glm::max(mesh.boundsMax, v.pos);
      }
    }
    if (!sd.indices.empty()) {
      for (uint32_t idx : sd.indices)
        indices.push_back(base + idx);
      item.indexCount = static_cast<uint32_t>(sd.indices.size());
    } else {
      for (uint32_t i = 0; i < static_cast<uint32_t>(sd.vertices.size()); ++i)
        indices.push_back(base + i);
      item.indexCount = static_cast<uint32_t>(sd.vertices.size());
    }
    item.textureIndex =
        textureFor(sd.material.texDiffusePath, /*srgb=*/true, sd.material.baseColor);

    // PBR material: texture if the asset has one, scalar fallback otherwise
    // (MaterialAsset::roughness/metallic/ao default to 0.8/0.0/1.0). Channel
    // selectors and the gloss/roughness flip are packed into materialFlags
    // for the shader to unpack.
    const auto &mat = sd.material;
    item.roughnessScalar = mat.roughness;
    item.metallicScalar = mat.metallic;
    item.aoScalar = mat.ao;
    uint32_t flags = 0;
    if (!mat.texRoughnessPath.empty()) {
      item.roughnessIndex = textureFor(mat.texRoughnessPath, /*srgb=*/false);
      flags |= kHasRoughnessMap;
    }
    if (!mat.texMetallicPath.empty()) {
      item.metallicIndex = textureFor(mat.texMetallicPath, /*srgb=*/false);
      flags |= kHasMetallicMap;
    }
    if (!mat.texAOPath.empty()) {
      item.aoIndex = textureFor(mat.texAOPath, /*srgb=*/false);
      flags |= kHasAOMap;
    }
    if (!mat.texNormalPath.empty()) {
      item.normalIndex = textureFor(mat.texNormalPath, /*srgb=*/false);
      flags |= kHasNormalMap;
    }
    if (!mat.texOpacityPath.empty()) {
      item.opacityIndex = textureFor(mat.texOpacityPath, /*srgb=*/false);
      flags |= kHasOpacityMap;
    }
    item.alphaCutoff = std::max(mat.alphaCutoff, 0.0f);
    if (item.alphaCutoff > 0.0f) {
      const bool opacityMap = !mat.texOpacityPath.empty();
      alphaSources.push_back(
          {item.indexOffset, 0u,
           opacityMap ? mat.texOpacityPath : mat.texDiffusePath,
           opacityMap ? static_cast<uint32_t>(mat.opacityChannel & 0x3) : 3u,
           item.alphaCutoff});
    }
    item.emissiveColor = glm::max(mat.emissiveColor, glm::vec3(0.0f));
    item.emissiveStrength = std::max(mat.emissiveStrength, 0.0f);
    if (mat.roughnessMapIsGloss)
      flags |= kRoughnessIsGloss;
    flags |= static_cast<uint32_t>(mat.roughnessChannel & 0x3)
             << kRoughnessChannelShift;
    flags |= static_cast<uint32_t>(mat.metallicChannel & 0x3)
             << kMetallicChannelShift;
    flags |= static_cast<uint32_t>(mat.aoChannel & 0x3) << kAOChannelShift;
    flags |= static_cast<uint32_t>(mat.opacityChannel & 0x3)
             << kOpacityChannelShift;
    item.materialFlags = flags;
    mesh.drawItems.push_back(item);
  }

  for (const DrawItem &di : mesh.drawItems)
    mesh.alphaTested = mesh.alphaTested || di.alphaCutoff > 0.0f;
  // Draw items and alpha sources were pushed in the same order; the index
  // counts are only final now.
  for (AlphaSource &src : alphaSources)
    for (const DrawItem &di : mesh.drawItems)
      if (di.indexOffset == src.indexOffset)
        src.indexCount = di.indexCount;

  if (vertices.empty() || indices.empty()) {
    std::fprintf(stderr, "[VulkanRHI] mesh data '%s' has no geometry\n",
                 debugName.c_str());
    return false;
  }

  mesh.indexCount = static_cast<uint32_t>(indices.size());
  mesh.vertexCount = static_cast<uint32_t>(vertices.size());

  const VkBufferUsageFlags asInput =
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  createDeviceLocalBuffer(vertices.data(),
                          vertices.size() * sizeof(MeshVertex),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | asInput,
                          mesh.vertexBuffer, mesh.vertexAlloc);
  createDeviceLocalBuffer(indices.data(), indices.size() * sizeof(uint32_t),
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT | asInput,
                          mesh.indexBuffer, mesh.indexAlloc);

  if (mesh.alphaTested) {
    // Opacity micromaps: every triangle starts fully opaque (bark, and any
    // submesh without a cut-out); alpha-masked submeshes are then carved
    // from a coverage grid of their alpha source.
    const size_t triCount = indices.size() / 3;
    std::vector<uint32_t> words(triCount * kRtMicroWords, 0xFFFFFFFFu);
    for (const AlphaSource &src : alphaSources) {
      std::vector<uint8_t> rgba;
      int w = 0, h = 0;
      if (const ::MeshImage *img = data.findImage(src.path)) {
        w = img->width;
        h = img->height;
        rgba.resize(static_cast<size_t>(w) * h * 4, 255);
        for (size_t p = 0; p < static_cast<size_t>(w) * h; ++p)
          for (int c = 0; c < std::min(img->component, 4); ++c)
            rgba[p * 4 + c] = img->pixels[p * img->component + c];
      } else {
        // Same orientation as loadTextureFile's GPU upload (flipped).
        stbi_set_flip_vertically_on_load(1);
        int channels = 0;
        stbi_uc *px = stbi_load(src.path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
        stbi_set_flip_vertically_on_load(0);
        if (px) {
          rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
          stbi_image_free(px);
        }
      }
      if (rgba.empty() || w <= 0 || h <= 0)
        continue; // leave opaque: better a solid shadow than none
      // Coverage grid: fraction of texels passing the cutoff per cell.
      const uint32_t gw = std::min<uint32_t>(64u, static_cast<uint32_t>(w));
      const uint32_t gh = std::min<uint32_t>(64u, static_cast<uint32_t>(h));
      std::vector<float> cov(static_cast<size_t>(gw) * gh, 0.0f);
      std::vector<uint32_t> cnt(cov.size(), 0);
      const uint8_t cut = static_cast<uint8_t>(std::clamp(src.cutoff, 0.0f, 1.0f) * 255.0f);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const size_t cell = static_cast<size_t>(y * gh / h) * gw + (x * gw / w);
          cov[cell] += rgba[(static_cast<size_t>(y) * w + x) * 4 + src.channel] >= cut ? 1.0f : 0.0f;
          ++cnt[cell];
        }
      for (size_t c = 0; c < cov.size(); ++c)
        cov[c] /= static_cast<float>(std::max(cnt[c], 1u));
      for (uint32_t t = src.indexOffset / 3; t < (src.indexOffset + src.indexCount) / 3; ++t) {
        const glm::vec2 uv[3] = {vertices[indices[t * 3 + 0]].uv,
                                 vertices[indices[t * 3 + 1]].uv,
                                 vertices[indices[t * 3 + 2]].uv};
        buildTriangleMicromap(uv, cov, gw, gh, &words[t * kRtMicroWords]);
      }
    }
    createDeviceLocalBuffer(words.data(), words.size() * sizeof(uint32_t),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                            mesh.alphaMaskBuffer, mesh.alphaMaskAlloc);
  }
  return true;
}

void VulkanRenderer::addInstance(MeshHandle mesh, const glm::mat4 &transform,
                                 bool isTerrain, const MaterialAsset *material,
                                 bool castsShadow, bool isViewModel, uint32_t viewModelFlags) {
  if (mesh >= mMeshes.size())
    return;
  Instance inst;
  inst.meshIndex = mesh;
  inst.model = transform;
  inst.isTerrain = isTerrain;
  inst.castsShadow = castsShadow;
  inst.isViewModel = isViewModel;
  inst.viewModelFlags = viewModelFlags;
  if(material) {
    inst.hasMaterialOverride=true;
    auto &out=inst.materialOverride;
    // Share the solid-color texture cache with imported/procedural assets.
    // Scalar ECS overrides previously never reached this backend at all.
    uint8_t rgba[4];uint32_t key=0;
    for(int i=0;i<4;++i){
      rgba[i]=static_cast<uint8_t>(glm::clamp(material->baseColor[i],0.0f,1.0f)*255.0f);
      key=(key<<8)|rgba[i];
    }
    auto found=mSolidColorTex.find(key);
    if(found==mSolidColorTex.end()) {
      const uint32_t texture=addTexture(rgba,1,1,VK_FORMAT_R8G8B8A8_SRGB);
      found=mSolidColorTex.emplace(key,texture).first;
    }
    out.textureIndex=found->second;
    out.roughnessScalar=glm::clamp(material->roughness,.06f,1.0f);
    out.metallicScalar=glm::clamp(material->metallic,0.0f,1.0f);
    out.aoScalar=glm::clamp(material->ao,0.0f,1.0f);
    out.alphaCutoff=glm::clamp(material->alphaCutoff,0.0f,1.0f);
    out.emissiveColor=glm::max(material->emissiveColor,glm::vec3(0));
    out.emissiveStrength=std::max(material->emissiveStrength,0.0f);
  }
  mInstances.push_back(inst);
}

void VulkanRenderer::setVegetationBatches(const std::vector<VegBatch> &batches) {
  // Signals the TLAS change tracker that the vegetation set is new.
  ++mVegGeneration;

  for (const VegBatch &batch : batches) {
    if (batch.mesh >= mMeshes.size() || batch.instances.empty())
      continue;

    VegSpeciesBuffer *vb = nullptr;
    for (VegSpeciesBuffer &existing : mVegBuffers) {
      if (existing.mesh == batch.mesh && existing.layerKey == batch.layerKey) {
        vb = &existing;
        break;
      }
    }
    if (!vb) {
      mVegBuffers.push_back(VegSpeciesBuffer{});
      vb = &mVegBuffers.back();
      vb->mesh = batch.mesh;
      vb->layerKey = batch.layerKey;
    }

    const size_t needed = batch.instances.size();
    if (needed > vb->capacity) {
      // Retire (not destroy) the outgrown buffer: in-flight frames' draws
      // may still be reading it.
      if (vb->buffer)
        mPendingGarbage.buffers.emplace_back(vb->buffer, vb->alloc);
      const size_t newCapacity = std::max(needed, vb->capacity * 2);

      VkBufferCreateInfo bufferCi{};
      bufferCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      bufferCi.size = sizeof(VegInstanceGpu) * newCapacity;
      bufferCi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
      bufferCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      VmaAllocationCreateInfo allocCi{};
      allocCi.usage = VMA_MEMORY_USAGE_AUTO;
      allocCi.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT;
      VmaAllocationInfo info{};
      VK_CHECK(vmaCreateBuffer(mCtx->allocator(), &bufferCi, &allocCi,
                               &vb->buffer, &vb->alloc, &info));
      vb->mapped = info.pMappedData;
      vb->capacity = newCapacity;
    }

    std::memcpy(vb->mapped, batch.instances.data(),
               sizeof(VegInstanceGpu) * needed);
    // Flush for non-coherent memory (VMA may select non-HOST_COHERENT heaps).
    vmaFlushAllocation(mCtx->allocator(), vb->alloc, 0, VK_WHOLE_SIZE);
    vb->count = static_cast<uint32_t>(needed);
    vb->cpu = batch.instances;
    vb->ranges = batch.ranges;
    vb->drawDistance = batch.drawDistance;
    vb->meshLods = batch.meshLods;
    vb->shadowMesh = batch.shadowMesh;
    vb->grass = batch.grass;
    vb->rayTracedShadows = batch.rayTracedShadows;
    vb->windStrength = batch.windStrength;
    vb->windSpeed = batch.windSpeed;
    vb->windMeshHeight = batch.windMeshHeight;
    vb->groundOcclusion = batch.groundOcclusion;
    vb->foliageSssStrength = batch.foliageSssStrength;
    vb->receivesSnow = batch.receivesSnow;
    // A batch without range info still draws, just uncullable as one span.
    if (vb->ranges.empty() && vb->count > 0) {
      VegRange all{};
      all.first = 0;
      all.count = vb->count;
      vb->ranges.push_back(all);
    }
  }

  // Species tracked previously but absent (or empty) now get their count
  // zeroed so drawFrame() skips them -- the buffer itself stays allocated
  // to avoid realloc churn on the next reload.
  for (VegSpeciesBuffer &vb : mVegBuffers) {
    bool stillPresent = false;
    for (const VegBatch &batch : batches) {
      if (batch.mesh == vb.mesh && batch.layerKey == vb.layerKey && !batch.instances.empty()) {
        stillPresent = true;
        break;
      }
    }
    if (!stillPresent) {
      vb.count = 0;
      vb.cpu.clear();
      vb.ranges.clear();
    }
  }
}

bool VulkanRenderer::finalizeScene() {
  if (mMeshes.empty()) {
    std::fprintf(stderr, "[VulkanRHI] finalizeScene: no meshes\n");
    return false;
  }

  // The blocking BLAS builds below read vertex/index buffers directly --
  // any copies still queued for the batched path must land first.
  flushPendingCopiesImmediate();
  // A full rebuild covers every live slot; individually queued builds would
  // just rebuild the same BLASes again next drawFrame.
  mPendingBlasBuilds.clear();

  // Re-finalizing (new meshes appeared): tear down the previous
  // acceleration structures first. Rare, so a device stall is acceptable.
  if (mSceneReady) {
    vkDeviceWaitIdle(mCtx->device());
    for (auto &garbage : mFrameGarbage)
      mAccel.freeGarbage(*mCtx, garbage);
    mAccel.freeGarbage(*mCtx, mPendingGarbage);
    mAccel.destroy(*mCtx);
    mSceneReady = false;
  }

  // Build acceleration structures: one BLAS per live mesh (dead slots get a
  // zeroed input and stay empty), TLAS from the instances.
  std::vector<VulkanAccel::BlasInput> blasInputs(mMeshes.size());
  for (size_t i = 0; i < mMeshes.size(); ++i)
    blasInputs[i] = blasInputForMesh(mMeshes[i]);
  std::vector<VulkanAccel::InstanceInput> instInputs;
  instInputs.reserve(mInstances.size());
  for (const Instance &inst : mInstances) {
    if (inst.isViewModel || !inst.castsShadow || !mMeshes[inst.meshIndex].vertexBuffer)
      continue;
    VulkanAccel::InstanceInput in;
    in.blasIndex = inst.meshIndex;
    in.transform = inst.model;
    instInputs.push_back(in);
  }
  auto submit = [this](const std::function<void(VkCommandBuffer)> &fn) {
    immediateSubmit(fn);
  };
  if (!mAccel.build(*mCtx, submit, blasInputs, instInputs, kFramesInFlight))
    return false;
  writeTlasDescriptors();
  // Force drawFrame's change detection to rebuild each frame slot's TLAS
  // with the full current input list (including vegetation).
  mTlasSceneInputsPrev.clear();
  mVegTlasGeneration = ~0ull;
  markTlasAllStale();
  mSceneReady = true;
  return true;
}

bool VulkanRenderer::growScene() {
  // Once the scene is live, new meshes queue their own BLAS builds at
  // creation (recorded batched into the next drawFrame) -- nothing to do
  // here. Kept for the init-order case where meshes exist before the first
  // finalize.
  if (!mSceneReady)
    return finalizeScene();
  return true;
}

bool VulkanRenderer::createSyncObjects() {
  mCommandBuffers.resize(kFramesInFlight);
  VkCommandBufferAllocateInfo allocCi{};
  allocCi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocCi.commandPool = mCommandPool;
  allocCi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocCi.commandBufferCount = kFramesInFlight;
  VK_CHECK(vkAllocateCommandBuffers(mCtx->device(), &allocCi,
                                    mCommandBuffers.data()));

  mImageAvailable.resize(kFramesInFlight);
  mInFlight.resize(kFramesInFlight);
  VkSemaphoreCreateInfo semCi{};
  semCi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  VkFenceCreateInfo fenceCi{};
  fenceCi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceCi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VK_CHECK(vkCreateSemaphore(mCtx->device(), &semCi, nullptr,
                               &mImageAvailable[i]));
    VK_CHECK(vkCreateFence(mCtx->device(), &fenceCi, nullptr, &mInFlight[i]));
  }
  mRenderFinished.resize(mSwapchain.imageCount());
  for (uint32_t i = 0; i < mSwapchain.imageCount(); ++i)
    VK_CHECK(vkCreateSemaphore(mCtx->device(), &semCi, nullptr,
                               &mRenderFinished[i]));
  return true;
}

void VulkanRenderer::recreateSwapchain() {
  uint32_t w = 0, h = 0;
  mQueryFbSize(w, h);
  if (w == 0 || h == 0)
    return;

  vkDeviceWaitIdle(mCtx->device());

  for (VkSemaphore s : mRenderFinished)
    vkDestroySemaphore(mCtx->device(), s, nullptr);
  mRenderFinished.clear();
  destroySceneTargets();

  mSwapchain.destroy(*mCtx);
  if (!mSwapchain.create(*mCtx, mSurface, w, h))
    return;
  createSceneTargets();
  updateTemporalSets();
  updateTonemapSets();
  updateSSAOSets();
  updateCloudSets();      // cloud march target is swapchain-derived too

  mRenderFinished.resize(mSwapchain.imageCount());
  VkSemaphoreCreateInfo semCi{};
  semCi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  for (uint32_t i = 0; i < mSwapchain.imageCount(); ++i)
    VK_CHECK(vkCreateSemaphore(mCtx->device(), &semCi, nullptr,
                               &mRenderFinished[i]));
}

namespace {

// Gribb-Hartmann frustum planes from a view-projection matrix (works with
// the Vulkan 0..1 depth range and the proj[1][1] Y-flip since planes come
// from the final matrix). Plane xyz = normal, w = distance; a point p is
// inside plane i when dot(normal, p) + w >= 0.
struct Frustum {
  glm::vec4 planes[6];
};

Frustum frustumFromViewProj(const glm::mat4 &m) {
  const glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
  const glm::vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
  const glm::vec4 r2(m[0][2], m[1][2], m[2][2], m[3][2]);
  const glm::vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);
  Frustum f;
  f.planes[0] = r3 + r0; // left
  f.planes[1] = r3 - r0; // right
  f.planes[2] = r3 + r1; // bottom
  f.planes[3] = r3 - r1; // top
  f.planes[4] = r2;      // near (z >= 0 in clip space)
  f.planes[5] = r3 - r2; // far
  return f;
}

// Positive-vertex AABB test: conservative (never culls a visible box).
bool aabbInFrustum(const Frustum &f, glm::vec3 mn, glm::vec3 mx) {
  for (const glm::vec4 &p : f.planes) {
    const glm::vec3 v(p.x > 0.0f ? mx.x : mn.x, p.y > 0.0f ? mx.y : mn.y,
                      p.z > 0.0f ? mx.z : mn.z);
    if (glm::dot(glm::vec3(p), v) + p.w < 0.0f)
      return false;
  }
  return true;
}

// World-space AABB of a transformed local AABB (center/extent + |M| trick).
void transformAabb(const glm::mat4 &m, glm::vec3 mn, glm::vec3 mx,
                   glm::vec3 &outMn, glm::vec3 &outMx) {
  const glm::vec3 c = (mn + mx) * 0.5f;
  const glm::vec3 e = (mx - mn) * 0.5f;
  const glm::vec3 wc = glm::vec3(m * glm::vec4(c, 1.0f));
  glm::mat3 absM(m);
  for (int col = 0; col < 3; ++col)
    for (int row = 0; row < 3; ++row)
      absM[col][row] = std::abs(absM[col][row]);
  const glm::vec3 we = absM * e;
  outMn = wc - we;
  outMx = wc + we;
}

} // namespace

void VulkanRenderer::drawFrame() {
  VkDevice device = mCtx->device();
  VK_CHECK(vkWaitForFences(device, 1, &mInFlight[mCurrentFrame], VK_TRUE,
                           UINT64_MAX));

  // This frame slot's previous submission has fully retired -- free the
  // resources that were queued for deletion when it was recorded.
  mAccel.freeGarbage(*mCtx, mFrameGarbage[mCurrentFrame]);
  mFrameStats = FrameStats{};

  if (!mGpuTimestampsInitialized) {
    mGpuTimestampsInitialized = true;
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(mCtx->physicalDevice(), &families, nullptr);
    std::vector<VkQueueFamilyProperties> props(families);
    vkGetPhysicalDeviceQueueFamilyProperties(mCtx->physicalDevice(), &families, props.data());
    mGpuTimestampBits = props[mCtx->graphicsQueueFamily()].timestampValidBits;
    if (mGpuTimestampBits) {
      VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
      ci.queryCount = kGpuTimestampCount;
      for (auto &pool : mGpuTimestampPools)
        VK_CHECK(vkCreateQueryPool(device, &ci, nullptr, &pool));
    }
  }
  const VkQueryPool gpuQueries = mGpuTimestampPools[mCurrentFrame];
  if (gpuQueries && mGpuTimestampsWritten[mCurrentFrame]) {
    uint64_t ticks[kGpuTimestampCount]{};
    if (vkGetQueryPoolResults(device, gpuQueries, 0, kGpuTimestampCount,
                             sizeof(ticks), ticks, sizeof(uint64_t),
                             VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      const uint64_t mask = mGpuTimestampBits == 64 ? UINT64_MAX
                              : (uint64_t{1} << mGpuTimestampBits) - 1;
      const double toMs = mCtx->properties().limits.timestampPeriod * 1e-6;
      auto elapsed = [&](int a, int b) {
        return float(((ticks[b] - ticks[a]) & mask) * toMs);
      };
      mFrameStats.gpuTotalMs = elapsed(0, 6);
      mFrameStats.gpuPrepareMs = elapsed(0, 1);
      mFrameStats.gpuDepthMs = elapsed(1, 2);
      mFrameStats.gpuAtmosphereMs = elapsed(2, 3);
      mFrameStats.gpuSceneMs = elapsed(3, 4);
      mFrameStats.gpuWaterMs = elapsed(11, 12);
      mFrameStats.gpuPostMs = elapsed(5, 6);
      mFrameStats.gpuCloudsMs = elapsed(7, 13);
      mFrameStats.gpuSkyEnvironmentMs = elapsed(8, 9);
      mFrameStats.gpuSSAOMs = elapsed(2, 10);
    }
  }

  uint32_t imageIndex = 0;
  VkResult acquire = vkAcquireNextImageKHR(
      device, mSwapchain.handle(), UINT64_MAX, mImageAvailable[mCurrentFrame],
      VK_NULL_HANDLE, &imageIndex);
  if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
    recreateSwapchain();
    return;
  }
  if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
    std::fprintf(stderr, "[VulkanRHI] vkAcquireNextImageKHR failed: %s\n",
                 resultString(acquire));
    std::abort();
  }

  const VkExtent2D extent = mSwapchain.extent();
  const float aspect =
      static_cast<float>(extent.width) / static_cast<float>(extent.height);

  // Free-fly camera from the input-driven params.
  const float yaw = glm::radians(mParams.camYawDeg);
  const float pitch = glm::radians(mParams.camPitchDeg);
  const glm::vec3 forward = glm::normalize(
      glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                std::cos(pitch) * std::cos(yaw)));
  const glm::vec3 eye = mParams.camPos;
  const glm::mat4 viewMat =
      glm::lookAt(eye, eye + forward, glm::vec3(0.0f, 1.0f, 0.0f));
  // Far plane covers the terrain streaming radius (set by
  // VkTerrainSubsystem from viewDistanceChunks * chunkWorldSize) with
  // margin; shadow-ray tMax in the frag shaders is a separate fixed 420m
  // (ray-traced shadow reach, not camera visibility -- terrain beyond that
  // just casts an unshadowed-by-distant-occluders result, which is fine).
  glm::mat4 proj = glm::perspective(glm::radians(mParams.fovDeg), aspect,
                                    0.05f, std::max(mParams.farPlane, 10.0f));
  proj[1][1] *= -1.0f;
  const glm::mat4 unjitteredProjection = proj;
  mAtmosphere.prepare(mCurrentFrame, mFrameCounter, extent, mParams.atmosphere.quality);
  mAtmosphere.updateInputs(mCurrentFrame, mSceneCopyViews[mCurrentFrame], mDepthViews[mCurrentFrame]);
  mCloudHistory.prepare(mCurrentFrame,mFrameCounter,mCloudExtent);
  mFrameStats.gpuCloudHistoryMs=mCloudHistory.milliseconds();
  mFrameStats.cloudHistoryAllocatedBytes=mCloudHistory.allocatedBytes();
  VkDescriptorImageInfo cloudShadowInput{mSampler,mAtmosphere.cloudShadowView(),VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet cloudShadowWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};cloudShadowWrite.dstSet=mSceneEnvSets[mCurrentFrame];cloudShadowWrite.dstBinding=1;
  cloudShadowWrite.descriptorCount=1;cloudShadowWrite.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;cloudShadowWrite.pImageInfo=&cloudShadowInput;
  vkUpdateDescriptorSets(mCtx->device(),1,&cloudShadowWrite,0,nullptr);
  VkDescriptorBufferInfo irradianceInput{mAtmosphere.skyIrradianceBuffer(),0,16*sizeof(glm::vec4)};
  cloudShadowWrite.dstBinding=2;cloudShadowWrite.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  cloudShadowWrite.pImageInfo=nullptr;cloudShadowWrite.pBufferInfo=&irradianceInput;
  vkUpdateDescriptorSets(mCtx->device(),1,&cloudShadowWrite,0,nullptr);
  const auto fogTimings = mAtmosphere.timings();
  mFrameStats.gpuFogVisibilityMs = fogTimings.visibility;
  mFrameStats.gpuFogInjectionMs = fogTimings.injection;
  mFrameStats.gpuFogHistoryMs = fogTimings.history;
  mFrameStats.gpuFogIntegrationMs = fogTimings.integration + fogTimings.composition;
  // Cloud reconstruction is a new atmosphere allocation, rather than an
  // existing cloud asset. Include it in the total; the separate statistic
  // explains the phase-two increase without hiding it from budget reports.
  mFrameStats.cloudDetailAllocatedBytes=mCloudPeriodicDetailBytes;
  mFrameStats.atmosphereAllocatedBytes = mAtmosphere.allocatedBytes()+mCloudHistory.allocatedBytes()+mExposureMeter.allocatedBytes()+mCloudPeriodicDetailBytes;
  mFrameStats.gpuBloomMs = mAtmosphere.bloomMilliseconds();
  mFrameStats.gpuSkyLutsMs = mAtmosphere.skyMilliseconds();
  mFrameStats.gpuCloudShadowMs = mAtmosphere.cloudShadowMilliseconds();
  const bool temporalOn = mParams.temporalAA && mParams.debugViewMode == 0;
  if (temporalOn) {
    // Eight sub-pixel positions cover both axes without shifting camera state,
    // picking rays, or the user's saved view. Depth/sky/water use this matrix.
    static constexpr glm::vec2 jitter[8] = {
        {0.0f,-.166667f},{-.25f,.166667f},{.25f,-.388889f},{-.375f,-.055556f},
        {.125f,.277778f},{-.125f,-.277778f},{.375f,.055556f},{-.4375f,.388889f}};
    const auto offset = jitter[mFrameCounter % 8];
    proj[2][0] += offset.x * 2.0f / extent.width;
    proj[2][1] += offset.y * 2.0f / extent.height;
  }

  const float ly = glm::radians(mParams.lightYawDeg);
  const float lp = glm::radians(mParams.lightPitchDeg);
  const glm::vec3 sunTravelDir = glm::normalize(glm::vec3(
      std::cos(lp) * std::sin(ly), -std::sin(lp), std::cos(lp) * std::cos(ly)));

  // Atmosphere-driven direct light. The sun's ground-level radiance is its
  // top-of-atmosphere radiance x Chapman transmittance (the CPU mirror of
  // skyModel.glsl) -- white at noon, gold at dusk, gone below the horizon.
  // The moon is modeled full and antipodal to the sun; once the sun drops
  // into deep twilight the ACTIVE direct light (direction + radiance +
  // shadow rays + volumetrics, all via FrameData) hands off to it. The
  // handoff snaps direction, but both radiances are near-zero there.
  const glm::vec3 toSun = -sunTravelDir;
  const float sunEl = toSun.y;
  const float haze = glm::clamp(mParams.atmosphereHaze, 0.0f, 1.0f);
  const float sunOuterScale = atm::kSunOuterRadiance * mParams.sunIntensity;
  const bool physicalSky=mParams.atmosphere.enabled&&mParams.atmosphere.physicalSky&&mParams.debugViewMode==0;
  const auto directTransmission=[&](float elevation) {
    return physicalSky?glm::vec3(atmosphere::sky::transmittance(6371+std::clamp(double(eye.y+4)*.001,.002,40.0),elevation,haze))
                     :atm::transmittance(elevation,haze);
  };
  const glm::vec3 sunRad = sunOuterScale * directTransmission(sunEl);
  const float moonOuterScale = sunOuterScale * atm::kMoonOuterFactor *
                               std::max(mParams.moonIntensity, 0.0f);
  const glm::vec3 toMoon = -toSun;
  const glm::vec3 moonRad =
      atm::kMoonTint * moonOuterScale * directTransmission(toMoon.y);
  const bool sunActive = sunEl > -0.035f;
  const glm::vec3 lightDir = sunActive ? sunTravelDir : -sunTravelDir;
  const glm::vec3 activeRad = sunActive ? sunRad : moonRad;
  const glm::vec3 activeSurfaceIrradiance =
      activeRad * atm::kSurfaceIrradianceFromSunRadiance;
  const float activeLuma =
      glm::dot(activeSurfaceIrradiance,
               glm::vec3(0.2126f, 0.7152f, 0.0722f));

  mExposureMeter.prepare(mCurrentFrame);
  mFrameStats.gpuExposureMeterMs=mExposureMeter.milliseconds();mFrameStats.meteredLuminance=float(mExposureMeter.luminance());
  const bool histogramExposure=mParams.atmosphere.enabled&&mParams.atmosphere.histogramExposure&&mParams.debugViewMode==0;
  const auto exposureNow=std::chrono::steady_clock::now();
  float exposureDt=mHasLastExposureUpdate?
      std::chrono::duration<float>(exposureNow-mLastExposureUpdate).count():1.0f/60.0f;
  mLastExposureUpdate=exposureNow;
  mHasLastExposureUpdate=true;
  if(mParams.fixedTimeSeconds>=0.0f)exposureDt=1.0f/60.0f;
  if (mParams.autoExposure) {
    // Bright noon -> autoExposureMin, dark night -> autoExposureMax, driven
    // by the TRUE sun elevation (lightDir itself flips to the moon at
    // night). Eased rather than snapped so a moving sun doesn't pop.
    const float day = glm::smoothstep(-0.10f, 0.18f, sunEl);
    float target =
        glm::mix(mParams.autoExposureMax, mParams.autoExposureMin, day);
    if(histogramExposure&&mExposureMeter.ready())target=float(atmosphere::exposureTarget(
        mExposureMeter.luminance(),mParams.atmosphere.exposureKey,mParams.atmosphere.exposureMin,mParams.atmosphere.exposureMax));
    // Preserve the existing speed dial at 60 Hz while making adaptation
    // independent of frame rate. Per-frame easing flickered during stalls.
    const float alpha=1.0f-std::pow(1.0f-glm::clamp(mParams.autoExposureSpeed,0.0f,1.0f),
                                  glm::clamp(exposureDt,0.0f,.1f)*60.0f);
    if(histogramExposure&&mExposureMeter.ready())mParams.exposure=float(atmosphere::adaptExposure(mParams.exposure,target,alpha));
    else mParams.exposure += (target-mParams.exposure)*alpha;
  }

  const auto physicalAtmosphere=atmosphere::fromLegacy(mParams);
  FrameDataGpu frameData{};
  frameData.atmosphereParams=glm::vec4(physicalAtmosphere.enabled?1.f:0.f,physicalAtmosphere.range,physicalAtmosphere.dustExtinction,mParams.volumetricHeightFalloffScale);
  frameData.skyLutParams=glm::vec4(physicalSky?1.f:0.f,32000,0,0);
  const bool cloudShadowEnabled=physicalAtmosphere.enabled&&physicalAtmosphere.cloudShadows&&mCloudNoiseReady&&mParams.style.cloudVolumetricEnabled&&!mParams.style.paintedClouds&&mParams.style.cloudStrength>0&&mParams.debugViewMode==0;
  frameData.cloudShadowField=glm::vec4(glm::floor(glm::vec2(eye.x,eye.z)/128.f)*128.f-glm::vec2(4096),8192,cloudShadowEnabled?glm::clamp(mParams.style.cloudStrength,0.f,1.f):0.f);
  frameData.viewProj = proj * viewMat;
  frameData.view = viewMat;
  // w = the active light's LUMINANCE: legacy scalar consumers (grass
  // translucency/sheen/glint) scale with real light strength through the
  // whole day/night cycle instead of a fixed editor number.
  frameData.lightDir = glm::vec4(lightDir, activeLuma);
  frameData.fogParams =
      glm::vec4(mParams.fogDensity, mParams.fogStart, mParams.fogMaxOpacity,
                mParams.fogHeightFalloff);
  frameData.fogDayColor = glm::vec4(mParams.fogDayColor, 0.0f);
  frameData.fogNightColor = glm::vec4(mParams.fogNightColor, 0.0f);
  frameData.fogParams2 =
      glm::vec4(mParams.fogAerialStrength, mParams.fogSunInscatter,
                mParams.fogNoiseStrength, mParams.fogNoiseScale);
  frameData.fogParams3 = glm::vec4(mParams.fogAnisotropy,
                                   mParams.fogNoiseWindSpeed,
                                   mParams.fogSkyStrength, 0.0f);
  frameData.lightParams =
      glm::vec4(mParams.ambientIntensity, mParams.shadowStrength,
                mParams.shadowSoftness,
                static_cast<float>(mParams.shadowSamples));
  frameData.terrainMat1 = glm::vec4(mParams.grassStart, mParams.grassEnd,
                                    mParams.snowStart, mParams.snowEnd);
  frameData.terrainMat2 =
      glm::vec4(mParams.rockSlopeStart, mParams.rockSlopeEnd,
                mParams.terrainDetailScale, mParams.terrainDetailStrength);
  frameData.terrainColorSand = glm::vec4(mParams.terrainColorSand, 0.0f);
  frameData.terrainColorGrass = glm::vec4(mParams.terrainColorGrass, 0.0f);
  frameData.terrainColorRock = glm::vec4(mParams.terrainColorRock, 0.0f);
  frameData.terrainColorSnow = glm::vec4(mParams.terrainColorSnow, 0.0f);
  frameData.camPosWS = glm::vec4(mParams.camPos, 0.0f);
  frameData.skyAmbientParams =
      glm::vec4(mParams.nightSkyBrightness, mParams.duskStrength, 0.0f, 0.0f);
  frameData.terrainMat3 =
      glm::vec4(mParams.terrainBiomeTintEnabled ? 1.0f : 0.0f,
               mParams.terrainBiomeTintIntensity,
               mParams.terrainMacroVariationStrength,
               mParams.terrainRockDetailStrength);
  // miscParams.z: wall-clock seconds since renderer start, for shader
  // effects that gently animate (grass dew twinkle, wind gust drift).
  // Wrapped at ~4.6 h so float precision never degrades the animation.
  static const auto sTimeOrigin = std::chrono::steady_clock::now();
  // A negative fixedTime means "use the wall clock". Pinning it is what makes
  // a capture reproducible: cloud drift, dew twinkle and volumetric turbulence
  // all key off this, so two renders of the same scene taken a second apart
  // differ in thousands of pixels and no golden-image comparison is possible.
  const float timeSeconds =
      (mParams.fixedTimeSeconds >= 0.0f)
          ? mParams.fixedTimeSeconds
          : std::fmod(std::chrono::duration<float>(
                          std::chrono::steady_clock::now() - sTimeOrigin)
                          .count(),
                      16384.0f);
  frameData.miscParams =
      glm::vec4(static_cast<float>(mParams.debugViewMode), mParams.fogHeightRef,
                timeSeconds, 0.0f);

  // R2: resolve any changed terrain material paths (no-op most frames),
  // then pack the cached bindless indices + tiling scales.
  resolveTerrainMaterialsIfDirty();
  const auto &tex = mTerrainMaterialTex;
  const auto &slots = mParams.terrainMaterialSlots;
  frameData.terrainTexA = glm::uvec4(tex[0].albedoTex, tex[0].normalTex,
                                     tex[0].roughnessTex, tex[1].albedoTex);
  frameData.terrainTexB = glm::uvec4(tex[1].normalTex, tex[1].roughnessTex,
                                     tex[2].albedoTex, tex[2].normalTex);
  frameData.terrainTexC = glm::uvec4(tex[2].roughnessTex, tex[3].albedoTex,
                                     tex[3].normalTex, tex[3].roughnessTex);
  frameData.terrainTexD = glm::uvec4(tex[4].albedoTex, tex[4].normalTex,
                                     tex[4].roughnessTex, 0u);
  frameData.terrainTiling0 =
      glm::vec4(slots[0].tiling, slots[1].tiling, slots[2].tiling, slots[3].tiling);
  frameData.terrainTiling1 = glm::vec4(slots[4].tiling, mAuthoredTerrainSurface ? 1.0f : 0.0f, 0.0f, 0.0f);
  frameData.terrainHeightTex = glm::uvec4(tex[0].heightTex, tex[1].heightTex,
                                       tex[2].heightTex, tex[3].heightTex);
  frameData.terrainHeightTexExtra = glm::uvec4(tex[4].heightTex, UINT32_MAX,
                                            UINT32_MAX, UINT32_MAX);
  frameData.terrainReliefDepth0 = glm::vec4(slots[0].reliefDepth, slots[1].reliefDepth,
                                         slots[2].reliefDepth, slots[3].reliefDepth);
  frameData.terrainReliefDepth1 = glm::vec4(slots[4].reliefDepth, 0, 0, 0);

  // R3: biome lighting & atmosphere.
  frameData.biomeAmbientMeadow =
      glm::vec4(mParams.biomeAmbientTintMeadow, mParams.biomeAmbientIntensityMeadow);
  frameData.biomeAmbientForest =
      glm::vec4(mParams.biomeAmbientTintForest, mParams.biomeAmbientIntensityForest);
  frameData.biomeAmbientMountain =
      glm::vec4(mParams.biomeAmbientTintMountain, mParams.biomeAmbientIntensityMountain);
  frameData.biomeDirectParams =
      glm::vec4(mParams.forestCanopyOcclusion, mParams.forestLightShaftStrength,
               mParams.mountainDirectBoost, mParams.biomeLightingStrength);
  frameData.biomeFogForest = glm::vec4(mParams.forestFogTint, mParams.forestFogDensityMult);
  frameData.biomeFogMountain = glm::vec4(mParams.mountainFogTint, mParams.mountainFogDensityMult);
  frameData.biomeMountainExtra =
      glm::vec4(mParams.mountainAerialStrength, 0.0f, 0.0f, 0.0f);

  // Lighting overhaul: atmosphere-driven direct light, env-cubemap IBL
  // mip params (diffuse from the 4x4 mip, spec capped one below the 1x1
  // average so roughness 1 keeps a hint of direction), volumetrics.
  frameData.sunRadiance = glm::vec4(activeSurfaceIrradiance, sunEl);
  frameData.iblParams =
      glm::vec4(static_cast<float>(kEnvMipCount) - 3.0f,
                static_cast<float>(kEnvMipCount) - 2.5f,
                std::max(mParams.iblSpecularIntensity, 0.0f),
                std::max(mParams.terrainSkyReflectIntensity, 0.0f));
  frameData.volumetricParams = glm::vec4(
      mParams.volumetricEnabled ? std::max(mParams.volumetricIntensity, 0.0f)
                                : 0.0f,
      glm::clamp(mParams.volumetricAnisotropy, 0.0f, 0.95f),
      std::max(mParams.volumetricMaxDist, 1.0f),
      static_cast<float>(glm::clamp(mParams.volumetricSteps, 4, 32)));
  frameData.volumetricParams2 =
      glm::vec4(std::max(mParams.volumetricDensityScale, 0.0f),
               std::max(mParams.volumetricHeightFalloffScale, 0.0f),
               glm::clamp(mParams.volumetricTurbulence, 0.0f, 1.0f),
               std::max(mParams.volumetricWindSpeed, 0.0f));
  frameData.volumetricTint =
      glm::vec4(mParams.volumetricTintColor,
               glm::clamp(mParams.volumetricTintStrength, 0.0f, 1.0f));
  const auto &style = mParams.style;
  frameData.stylePaint0 =
      glm::vec4(0.18f, 8.0f, style.worldPaperStrength,
                style.washEdgeDarkening);
  frameData.stylePaint1 =
      glm::vec4(style.facetStrength, style.autumnAmount, 1.0f, 0.0f);
  frameData.stylePost0 =
      glm::vec4(style.vibrance, style.splitBalance, style.outlineWidth,
                style.outlineStrength);
  frameData.stylePost1 =
      glm::vec4(style.outlineDepthThreshold, style.outlineNormalThreshold,
                style.outlineDistance, style.screenPaperStrength);
  frameData.styleOutlineColor =
      glm::vec4(style.outlineColor, style.fxaaEnabled ? 1.0f : 0.0f);
  frameData.styleSplitShadow = glm::vec4(style.splitShadow, 0.0f);
  frameData.styleSplitHighlight = glm::vec4(style.splitHighlight, 0.0f);
  frameData.styleSky0 =
      glm::vec4(style.skyGradeStrength, style.skyBands,
                style.skyBandSoftness, style.sunSoftness);
  frameData.styleSkyZenith =
      glm::vec4(style.skyZenith, mParams.atmosphereHaze);
  frameData.styleSkyHorizon =
      glm::vec4(style.skyHorizon, mParams.skyBrightness);
  frameData.styleCloud0 =
      glm::vec4(style.cloudCoverage, style.cloudSoftness, style.cloudWind);
  frameData.styleCloudLit =
      glm::vec4(style.cloudLit, mParams.starIntensity);
  frameData.styleCloudMid =
      glm::vec4(style.cloudMid, mParams.sunDiscIntensity);
  frameData.styleCloudBase =
      glm::vec4(style.cloudBase, mParams.moonGlowIntensity);
  frameData.styleCloud1 =
      glm::vec4(style.cloudDeckHeight, style.cloudFeatureScale,
                style.cloudOpticalDensity, style.cloudSunOcclusion);
  frameData.styleCloud2 =
      glm::vec4(std::max(style.cloudLayerThickness, 50.0f),
                std::max(style.cloudShapeScale, 50.0f),
                std::max(style.cloudDetailScale, 5.0f),
                std::max(style.cloudWeatherScale, 100.0f));
  frameData.styleCloud3 =
      glm::vec4(std::max(style.cloudDensityMultiplier, 0.0f),
                std::max(style.cloudLightAbsorption, 0.0f),
                std::max(style.cloudAmbientStrength, 0.0f),
                std::max(style.cloudCurlStrength, 0.0f));
  frameData.styleCloud4 =
      glm::vec4(glm::clamp(style.cloudPhaseG, 0.0f, 0.95f),
                std::max(style.cloudSilverIntensity, 0.0f),
                std::max(style.cloudSilverSpread, 0.0f),
                glm::clamp(style.cloudPowderStrength, 0.0f, 1.0f));
  frameData.styleCloud5 =
      glm::vec4(std::max(style.cloudMaxMarchDist, 1000.0f),
                glm::clamp(style.cloudMaxSteps, 24.0f, 192.0f),
                glm::clamp(style.cloudLightTaps, 1.0f, 6.0f),
                glm::clamp(style.cloudTypeBias, -1.0f, 1.0f));
  frameData.snowParams = glm::vec4(mParams.snowCoverage, mParams.snowPatchScale,
      mParams.snowSlopeLimit, mParams.snowAltitudeBoost);
  frameData.snowColor = glm::vec4(mParams.snowTint, mParams.snowRoughness);
  frameData.snowTextures = glm::uvec4(mSnowMaterialTex.albedoTex,
      mSnowMaterialTex.normalTex, mSnowMaterialTex.roughnessTex,
      mParams.waterEnabled ? mSnowWaterTex : UINT32_MAX);
  frameData.snowExtra = glm::vec4(mParams.snowMaterial.tiling, 0, 0, 0);
  frameData.styleCloud6 =
      glm::vec4(glm::clamp(style.cloudDetailStrength, 0.0f, 1.0f), 0.0f, 0.0f,
                0.0f);
  frameData.waterParams0 =
      glm::vec4(mParams.waterLevel, std::max(mParams.waterClarity, 0.05f),
                glm::clamp(mParams.waterRoughness, 0.005f, 1.0f),
                mParams.waterReflectionStrength);
  frameData.waterParams1 =
      glm::vec4(mParams.waveAmplitude, mParams.waveScale, mParams.waveSpeed,
                static_cast<float>(glm::clamp(mParams.waterSsrSteps, 4, 128)));
  {
    glm::vec2 waveDir = mParams.waveDirection;
    if (glm::dot(waveDir, waveDir) < 1e-6f)
      waveDir = glm::vec2(1.0f, 0.0f);
    waveDir = glm::normalize(waveDir);
    frameData.waterParams2 =
        glm::vec4(waveDir.x, waveDir.y, std::max(mParams.waterSsrThickness, 0.01f),
                  std::max(mParams.waterFoamDepth, 0.001f));
  }
  // yz = the field's world origin, w = its size in metres. Size 0 means "no
  // field uploaded": the shader then uses the flat sea plane alone.
  frameData.waterParams3 =
      glm::vec4(mParams.waterFoamStrength, mWaterFieldOrigin.x,
                mWaterFieldOrigin.y, mWaterFieldSize);
  frameData.waterShallowColor = glm::vec4(mParams.waterShallowColor, 0.0f);
  frameData.waterDeepColor = glm::vec4(mParams.waterDeepColor, 0.0f);
  for (size_t i = 0; i < style.terrain.size(); ++i) {
    frameData.terrainPaintLit[i] =
        glm::vec4(style.terrain[i].lit, style.terrain[i].mottleScale);
    frameData.terrainPaintShade[i] =
        glm::vec4(style.terrain[i].shade, style.terrain[i].overlayStrength);
  }
  const uint32_t pointLightCount =
      std::min<uint32_t>(mParams.pointLightCount,
                         static_cast<uint32_t>(mParams.pointLights.size()));
  for (uint32_t i = 0; i < pointLightCount; ++i) {
    const auto &light = mParams.pointLights[i];
    frameData.pointLightPositionRadius[i] =
        glm::vec4(light.position, std::max(light.radius, 0.1f));
    frameData.pointLightColorIntensity[i] =
        glm::vec4(glm::max(light.color, glm::vec3(0.0f)),
                  std::max(light.intensity, 0.0f));
  }
  frameData.pointLightParams =
      glm::vec4(static_cast<float>(pointLightCount), 0.0f, 0.0f, 0.0f);

  // The Long Dark graphics overhaul parameters
  frameData.snowParams2 =
      glm::vec4(mParams.snowSparkle, 120.0f, mParams.snowWindDrift,
                mParams.snowSubsurface);
  frameData.iceParams =
      glm::vec4(mParams.iceEnabled ? 1.0f : 0.0f, mParams.iceCracksStrength,
                mParams.iceFrostCoverage, mParams.iceClarity);
  frameData.iceColor = glm::vec4(mParams.iceTint, mParams.iceRoughness);
  frameData.auroraParams =
      glm::vec4(mParams.auroraEnabled ? mParams.auroraIntensity : 0.0f,
                mParams.auroraSpeed, 1.0f, mParams.auroraGroundGlow);
  frameData.auroraColorBase = glm::vec4(0.08f, 0.96f, 0.58f, 1.0f);
  frameData.auroraColorTip = glm::vec4(0.72f, 0.16f, 0.88f, 1.0f);
  frameData.blizzardParams =
      glm::vec4(mParams.blizzardStrength, 12.0f, mParams.blizzardStrength,
                mParams.frostVignetteStrength);
  frameData.tldStyleParams =
      glm::vec4(mParams.stylizedLightingRamp, mParams.shadowCoolBias, 1.0f,
                temporalOn ? glm::clamp(mParams.temporalSharpness, 0.0f, .5f) : 0.0f);
  frameData.realismParams = glm::clamp(
      glm::vec4(mParams.terrainPhotoAlbedo, mParams.foliageNormalSoften,
                mParams.specularOcclusion, mParams.terrainTriplanar),
      glm::vec4(0.0f), glm::vec4(1.0f));

  frameData.rtAlphaTable = updateRtAlphaTable(mCurrentFrame);
  frameData.postAAParams = glm::vec4(glm::clamp(mParams.edgeSoftness, 0.0f, 1.0f),
                                    mParams.grassGroundDrawDistance, 0.0f, 0.0f);
  std::memcpy(mFrameUBOMapped[mCurrentFrame], &frameData, sizeof(frameData));
  // Flush for non-coherent memory (VMA may select non-HOST_COHERENT heaps).
  vmaFlushAllocation(mCtx->allocator(), mFrameUBOAllocs[mCurrentFrame], 0, VK_WHOLE_SIZE);

  // ---- TLAS input assembly + change detection (CPU only; the build is
  // recorded into the command buffer further down, and ONLY for frame slots
  // whose TLAS is stale). The scene is almost entirely static -- rebuilding
  // the TLAS every frame from scratch (including one instance per placed
  // plant) was pure per-frame overhead that scaled with terrain size.
  if (mSceneReady) {
    mTlasSceneInputs.clear();
    mTlasSceneInputs.reserve(mInstances.size());
    for (const Instance &inst : mInstances) {
      if (inst.isViewModel || !inst.castsShadow || inst.meshIndex >= mMeshes.size() ||
          !mMeshes[inst.meshIndex].vertexBuffer)
        continue;
      VulkanAccel::InstanceInput in{};
      in.blasIndex = inst.meshIndex;
      in.transform = inst.model;
      mTlasSceneInputs.push_back(in);
    }

    // Vegetation section: only instances near the camera -- a tree many
    // hundreds of meters away contributes no visible ray-traced shadow but
    // costs TLAS build time and traversal. Recomputed when the scatter set
    // changes or the camera has moved far enough to shift the eligible set.
    const bool grassCasters = std::any_of(mVegBuffers.begin(),mVegBuffers.end(),
        [](const VegSpeciesBuffer &vb){return vb.grass && vb.rayTracedShadows;});
    // Fine grass has much shorter LOD thresholds than trees. A 16m cache
    // step could leave coarse shadows beneath full-detail foreground blades.
    const float shadowCameraStep = grassCasters ? 4.0f : 16.0f;
    const bool vegStale =
        mVegGeneration != mVegTlasGeneration || !mHasVegTlasCamPos ||
        glm::distance(glm::vec2(eye.x, eye.z),
                      glm::vec2(mVegTlasCamPos.x, mVegTlasCamPos.z)) > shadowCameraStep;
    bool tlasListChanged = false;
    if (vegStale) {
      mTlasVegInputs.clear();
      const float shadowDist = std::max(mParams.vegShadowDistance, 0.0f);
      for (const VegSpeciesBuffer &vb : mVegBuffers) {
        if (vb.count == 0 || vb.mesh >= mMeshes.size() ||
            !mMeshes[vb.mesh].vertexBuffer)
          continue;
        // Opt-out batches never enter the TLAS at all. Checked
        // before the per-range loop, not inside it -- the whole point is to
        // not pay per-instance costs for these.
        if (!vb.rayTracedShadows)
          continue;
        // Individual short-blade shadows are sub-pixel past 45m; distant
        // swards retain root occlusion and SSAO. Tying casting to the entire
        // draw radius put >100k dense grass instances into the TLAS after the
        // bare gaps were filled. Keep full casting nearby, plus a cache-step
        // margin, without making distant coverage pay for invisible leaf rays.
        const float grassShadowDist = vb.drawDistance > 0.0f
            ? std::min(vb.drawDistance,45.0f) : 45.0f;
        const float batchShadowDist = vb.grass
            ? std::min(shadowDist,grassShadowDist+shadowCameraStep) : shadowDist;
        for (const VegRange &r : vb.ranges) {
          if (r.boundsMin != r.boundsMax) {
            const glm::vec3 cp = glm::clamp(eye, r.boundsMin, r.boundsMax);
            if (glm::distance(cp, eye) > batchShadowDist)
              continue;
          }
          // Ordinary shadow casters use the same detail policy as raster geometry.
          // Leaving distant crowns at full detail kept ray queries walking
          // thousands of tiny leaves even after raster LOD reduced the draw.
          MeshHandle shadowMesh = vb.mesh;
          const float distance = glm::distance(eye, glm::clamp(eye, r.boundsMin, r.boundsMax));
          for (const auto &lod : vb.meshLods)
            if (distance >= lod.distance && lod.mesh < mMeshes.size() &&
                mMeshes[lod.mesh].vertexBuffer)
              shadowMesh = lod.mesh;
          // Grass casts with selected real blades, not every rendered ribbon.
          // Keep this independent of raster LOD so nearby swards have soft,
          // interrupted shade rather than a dense cage of black blade lines.
          if (vb.shadowMesh < mMeshes.size() && mMeshes[vb.shadowMesh].vertexBuffer)
            shadowMesh = vb.shadowMesh;
          const uint32_t rangeEnd = std::min<uint32_t>(
              r.first + r.count, static_cast<uint32_t>(vb.cpu.size()));
          for (uint32_t i = r.first; i < rangeEnd; ++i) {
            VulkanAccel::InstanceInput in{};
            in.blasIndex = shadowMesh;
            in.grass = vb.grass;
            in.transform = vb.cpu[i].model;
            mTlasVegInputs.push_back(in);
          }
        }
      }
      mVegTlasGeneration = mVegGeneration;
      mVegTlasCamPos = eye;
      mHasVegTlasCamPos = true;
      tlasListChanged = true;
    }

    auto inputsEqual = [](const std::vector<VulkanAccel::InstanceInput> &a,
                          const std::vector<VulkanAccel::InstanceInput> &b) {
      if (a.size() != b.size())
        return false;
      for (size_t i = 0; i < a.size(); ++i)
        if (a[i].blasIndex != b[i].blasIndex || a[i].grass != b[i].grass ||
            a[i].transform != b[i].transform)
          return false;
      return true;
    };
    if (!inputsEqual(mTlasSceneInputs, mTlasSceneInputsPrev))
      tlasListChanged = true;
    // Queued BLAS (re)builds change device addresses this frame even when
    // the instance list is byte-identical (brush rebuilds).
    if (!mPendingBlasBuilds.empty())
      tlasListChanged = true;

    if (tlasListChanged) {
      mTlasSceneInputsPrev = mTlasSceneInputs;
      mTlasBuildInputs = mTlasSceneInputs;
      mTlasBuildInputs.insert(mTlasBuildInputs.end(), mTlasVegInputs.begin(),
                              mTlasVegInputs.end());
      markTlasAllStale();

      // Capacity growth instead of the old fixed 4096 + per-frame warning
      // spam. Rare (amortized 2x), and must happen before any recording
      // that references the TLAS objects/descriptors.
      if (mTlasBuildInputs.size() > mAccel.maxInstances()) {
        const uint32_t needed = static_cast<uint32_t>(mTlasBuildInputs.size());
        const uint32_t newCap =
            std::max(needed + needed / 2, mAccel.maxInstances() * 2);
        vkDeviceWaitIdle(device);
        for (auto &garbage : mFrameGarbage)
          mAccel.freeGarbage(*mCtx, garbage);
        if (mAccel.recreateTlas(*mCtx, kFramesInFlight, newCap))
          writeTlasDescriptors();
      }
    }
    mFrameStats.tlasInstances = static_cast<uint32_t>(mTlasBuildInputs.size());
  }

  VK_CHECK(vkResetFences(device, 1, &mInFlight[mCurrentFrame]));

  // Everything retired since the last drawFrame joins this frame's garbage
  // list, freed when this slot's fence next signals. (Done after the early
  // returns above so an aborted frame can't free staging buffers whose
  // copies haven't executed yet.)
  auto &slotGarbage = mFrameGarbage[mCurrentFrame];
  slotGarbage.buffers.insert(slotGarbage.buffers.end(),
                             mPendingGarbage.buffers.begin(),
                             mPendingGarbage.buffers.end());
  slotGarbage.accels.insert(slotGarbage.accels.end(),
                            mPendingGarbage.accels.begin(),
                            mPendingGarbage.accels.end());
  mPendingGarbage.buffers.clear();
  mPendingGarbage.accels.clear();

  VkCommandBuffer cmd = mCommandBuffers[mCurrentFrame];
  VK_CHECK(vkResetCommandBuffer(cmd, 0));
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
  auto gpuStamp = [&](uint32_t point) {
    if (gpuQueries)
      vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gpuQueries, point);
  };
  if (gpuQueries) vkCmdResetQueryPool(cmd, gpuQueries, 0, kGpuTimestampCount);
  gpuStamp(0);

  // ---- Batched streaming uploads: record every queued staging->device
  // copy and BLAS build into THIS command buffer, replacing the old
  // blocking submit-and-fence-wait per buffer/BLAS (up to ~a dozen full
  // GPU sync round-trips per frame while chunks streamed in). ----
  if (!mPendingCopies.empty() || !mPendingBlasBuilds.empty()) {
    const bool hadCopies = !mPendingCopies.empty();
    for (const PendingCopy &pc : mPendingCopies) {
      VkBufferCopy copy{};
      copy.size = pc.size;
      vkCmdCopyBuffer(cmd, pc.src, pc.dst, 1, &copy);
    }
    mPendingCopies.clear();

    if (hadCopies) {
      // Copies -> (vertex/index fetch this frame, BLAS geometry reads).
      VkMemoryBarrier2 barrier{};
      barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
      barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      barrier.dstStageMask =
          VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
          VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
          VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; // opacity micromaps
      barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
                              VK_ACCESS_2_INDEX_READ_BIT |
                              VK_ACCESS_2_SHADER_READ_BIT |
                              VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
      VkDependencyInfo dep{};
      dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      dep.memoryBarrierCount = 1;
      dep.pMemoryBarriers = &barrier;
      vkCmdPipelineBarrier2(cmd, &dep);
    }

    if (!mPendingBlasBuilds.empty()) {
      for (const auto &[slot, input] : mPendingBlasBuilds) {
        if (input.vertexCount == 0 || input.indexCount == 0)
          continue;
        mAccel.recordBlasBuildAt(*mCtx, cmd, slot, input, mPendingGarbage);
      }
      mPendingBlasBuilds.clear();
      // BLAS writes -> the TLAS build recorded below reads them.
      VkMemoryBarrier2 barrier{};
      barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
      barrier.srcStageMask =
          VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
      barrier.dstStageMask =
          VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
      VkDependencyInfo dep{};
      dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      dep.memoryBarrierCount = 1;
      dep.pMemoryBarriers = &barrier;
      vkCmdPipelineBarrier2(cmd, &dep);
    }
  }

  // Rebuild this frame slot's TLAS only when marked stale by the change
  // detection above (instance set changed, BLAS addresses changed, veg
  // shadow set recentered) -- a static view rebuilds nothing.
  if (mSceneReady && mTlasSlotStale[mCurrentFrame]) {
    mAccel.recordTlasBuild(cmd, mCurrentFrame, mTlasBuildInputs);
    mTlasSlotStale[mCurrentFrame] = false;
    mFrameStats.tlasRebuiltThisFrame = true;
  }

  gpuStamp(7);
  // ---- Pass C: volumetric cloud march (half-res) ----
  // Runs before BOTH sky passes because sky.frag consumes the result: the
  // clouds are blended into the sky's radiance there, ahead of the painterly
  // grade and ahead of the sun/moon/star highlights, so the deck is washed
  // with the same house look as the sky behind it and correctly occludes the
  // disc. Compositing it afterwards as its own pass (the first arrangement)
  // left ungraded clouds sitting on a graded sky and a sun that shone
  // straight through solid overcast.
  //
  // Nothing in the scene feeds this -- the layer sits kilometres past the far
  // plane, so no depth and no TLAS. When clouds are off the target is still
  // cleared to (0,0,0,1): sky.frag's sample is statically reachable, so the
  // image must be in a readable layout regardless, and a transmittance of 1
  // with no scattered light makes the blend an exact no-op.
  const bool cloudsOn = mCloudNoiseReady && mParams.style.cloudVolumetricEnabled &&
                        !mParams.style.paintedClouds &&
                        mParams.style.cloudStrength > 0.001f &&
                        mParams.debugViewMode == 0;
  // Always transition + clear -- sky.frag samples this image unconditionally,
  // so it must be in a valid layout even when cloud noise hasn't loaded yet.
  // (Leaving it in UNDEFINED violates VUID-vkCmdDraw-None-08114.)
  imageBarrier(cmd, mCloudImages[mCurrentFrame],
               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  VkRenderingAttachmentInfo cloudColor{};
  cloudColor.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  cloudColor.imageView = mCloudViews[mCurrentFrame];
  cloudColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  cloudColor.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  cloudColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  cloudColor.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  imageBarrier(cmd,mCloudHistory.depthImage(mCurrentFrame),
      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo cloudDepth=cloudColor;cloudDepth.imageView=mCloudHistory.depthView(mCurrentFrame);
  cloudDepth.clearValue.color={{0,0,0,0}};
  const VkRenderingAttachmentInfo cloudAttachments[]={cloudColor,cloudDepth};

  const VkExtent2D cloudExtent = mCloudExtent;
  VkViewport cloudVp{};
  cloudVp.width = static_cast<float>(cloudExtent.width);
  cloudVp.height = static_cast<float>(cloudExtent.height);
  cloudVp.minDepth = 0.0f;
  cloudVp.maxDepth = 1.0f;
  VkRect2D cloudScissor{};
  cloudScissor.extent = cloudExtent;

  VkRenderingInfo cloudRender{};
  cloudRender.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  cloudRender.renderArea.extent = cloudExtent;
  cloudRender.layerCount = 1;
  cloudRender.colorAttachmentCount = 2;
  cloudRender.pColorAttachments = cloudAttachments;
  vkCmdBeginRendering(cmd, &cloudRender);
  if (cloudsOn) {
    const glm::mat4 cloudInvViewProj = glm::inverse(proj * viewMat);
    vkCmdSetViewport(cmd, 0, 1, &cloudVp);
    vkCmdSetScissor(cmd, 0, 1, &cloudScissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mCloudPipeline);
    VkDescriptorSet cloudSets[2] = {mCloudNoiseSet, mFrameSets[mCurrentFrame]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            mCloudPipelineLayout, 0, 2, cloudSets, 0, nullptr);
    CloudPush cloudPush{};
    cloudPush.invViewProj = cloudInvViewProj;
    // Rotating the dither phase per frame turns the march's fixed step
    // lattice into noise that the eye integrates away over a few frames,
    // rather than a static grain locked to the screen.
    cloudPush.jitter = glm::vec4(
        // Spatial asset reviews have no temporal resolve to average
        // frame-dependent noise; pin their phase so repeat reviews match.
        mParams.deterministicCapture
            ? 0.0f : static_cast<float>(mFrameCounter % 8u) * 13.7f,
        float(cloudExtent.width),float(cloudExtent.height),0.0f);
    vkCmdPushConstants(cmd, mCloudPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(CloudPush), &cloudPush);
    vkCmdDraw(cmd, 3, 1, 0, 0);
  }
  vkCmdEndRendering(cmd);

  imageBarrier(cmd, mCloudImages[mCurrentFrame],
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  imageBarrier(cmd,mCloudHistory.depthImage(mCurrentFrame),VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  uint64_t reconstructionSignature=fnv1a64(&cloudsOn,sizeof(cloudsOn));
  for(const auto& cloud:{frameData.styleCloud0,frameData.styleCloud1,frameData.styleCloud2,frameData.styleCloud3,frameData.styleCloud4,frameData.styleCloud5,frameData.styleCloud6})
    reconstructionSignature=fnv1a64(&cloud,sizeof(cloud),reconstructionSignature);
  // Slow light motion is filtered; abrupt lighting changes and the sun/moon
  // handoff reject old cloud radiance. The depth remains independent of TAA.
  const glm::vec4 quantizedLight=glm::floor(frameData.sunRadiance*4.f);
  reconstructionSignature=fnv1a64(&quantizedLight,sizeof(quantizedLight),reconstructionSignature);
  VulkanCloudHistory::Data cloudHistoryData;
  cloudHistoryData.invViewProj=glm::inverse(proj*viewMat);cloudHistoryData.camera=glm::vec4(eye,timeSeconds);
  cloudHistoryData.wind=glm::vec4(frameData.styleCloud0.z*40*timeSeconds,0,frameData.styleCloud0.w*40*timeSeconds,sunActive?1.f:0.f);
  cloudHistoryData.controls=glm::vec4(0,.9f,mParams.fovDeg,mParams.farPlane);
  mCloudHistory.resolve(cmd,mCurrentFrame,mCloudImages[mCurrentFrame],mCloudViews[mCurrentFrame],cloudHistoryData,reconstructionSignature,
      cloudsOn&&mParams.atmosphere.cloudHistory&&mParams.atmosphere.enabled&&!mParams.deterministicCapture);
  VkDescriptorImageInfo reconstructedCloud{mSampler,mCloudHistory.outputView(mCurrentFrame),VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet cloudHistoryWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};cloudHistoryWrite.dstSet=mCloudSampleSets[mCurrentFrame];cloudHistoryWrite.descriptorCount=1;
  cloudHistoryWrite.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;cloudHistoryWrite.pImageInfo=&reconstructedCloud;
  vkUpdateDescriptorSets(mCtx->device(),1,&cloudHistoryWrite,0,nullptr);

  // Shared sky push-constant packing for the visible sky and the env faces.
  // The env pass zeroes disc/stars: the scene treats sun/moon as analytic
  // direct lights, so the cubemap carrying the disc would double-count it.
  const auto fillSkyPush = [&](const glm::mat4 &invViewProj,
                               bool forEnv) -> SkyPush {
    SkyPush push{};
    push.invViewProj = invViewProj;
    push.sunDir = glm::vec4(toSun, sunOuterScale);
    push.moonDir = glm::vec4(toMoon, moonOuterScale);
    push.camPos = glm::vec4(eye, timeSeconds);
    const float cloudStr = std::clamp(mParams.style.cloudStrength, 0.0f, 1.0f);
    const float f0 = forEnv ? 1.0f : 0.0f;
    const float f1 = 0.0105f;
    const float f2 = cloudsOn ? cloudStr : 0.0f;
    const float f3 = (mParams.style.paintedClouds && mParams.debugViewMode == 0)
                         ? cloudStr
                         : 0.0f;
    push.passFlags = glm::vec4(f0, f1, f2, f3);
    return push;
  };

  VulkanAtmosphereRenderer::SkyData skyData;
  gpuStamp(13);
  skyData.invViewProj=glm::inverse(unjitteredProjection*viewMat);
  skyData.camera=glm::vec4(eye,haze);skyData.sun=glm::vec4(toSun,sunOuterScale);skyData.moon=glm::vec4(toMoon,moonOuterScale);
  mAtmosphere.recordSky(cmd,mCurrentFrame,skyData,physicalSky);
  uint64_t cloudSignature=fnv1a64(&frameData.cloudShadowField,sizeof(frameData.cloudShadowField));
  uint64_t cloudStyleSignature=fnv1a64(&frameData.cloudShadowField.w,sizeof(float));
  for(const auto& cloud: {frameData.styleCloud0,frameData.styleCloud1,frameData.styleCloud2,frameData.styleCloud3,frameData.styleCloud4,frameData.styleCloud5,frameData.styleCloud6,frameData.lightDir})
    cloudSignature=fnv1a64(&cloud,sizeof(cloud),cloudSignature);
  for(const auto& cloud: {frameData.styleCloud0,frameData.styleCloud1,frameData.styleCloud2,frameData.styleCloud3,frameData.styleCloud4,frameData.styleCloud5,frameData.styleCloud6})
    cloudStyleSignature=fnv1a64(&cloud,sizeof(cloud),cloudStyleSignature);
  // Authoring a new deck rejects old fog illumination immediately. Continuous
  // drift instead uses current-neighbour clipping, preserving noise reduction.
  if(cloudStyleSignature!=mCloudStyleSignature)mAtmosphere.invalidate();
  mCloudStyleSignature=cloudStyleSignature;
  cloudSignature=fnv1a64(&timeSeconds,sizeof(timeSeconds),cloudSignature);
  mAtmosphere.recordCloudShadow(cmd,mCurrentFrame,mCloudNoiseSet,mFrameSets[mCurrentFrame],frameData.cloudShadowField,cloudSignature,cloudShadowEnabled);
  gpuStamp(8);
  // ---- Pass E: sky environment cubemap (IBL) ----
  // Six 128^2 faces of pure atmosphere, then a blit mip chain. Every lit
  // shader samples this for ambient/reflections/fog color, so the whole
  // scene's ambient tracks the sun in real time. Face bases come straight
  // from the cube-face addressing table (u right, v down per face) -- built
  // as an affine "inverse view-proj" the sky shader can unproject through.
  {
    const glm::vec3 kFaceF[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                 {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const glm::vec3 kFaceRt[6] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0},
                                  {1, 0, 0},  {1, 0, 0}, {-1, 0, 0}};
    const glm::vec3 kFaceDn[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1},
                                  {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};

    imageBarrier(cmd, mEnvImages[mCurrentFrame],
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_ASPECT_COLOR_BIT, 6, 0, 1);

    VkViewport envVp{};
    envVp.width = static_cast<float>(kEnvFaceSize);
    envVp.height = static_cast<float>(kEnvFaceSize);
    envVp.minDepth = 0.0f;
    envVp.maxDepth = 1.0f;
    VkRect2D envScissor{};
    envScissor.extent = {kEnvFaceSize, kEnvFaceSize};

    for (uint32_t face = 0; face < 6; ++face) {
      VkRenderingAttachmentInfo faceColor{};
      faceColor.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      faceColor.imageView = mEnvFaceViews[mCurrentFrame][face];
      faceColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      faceColor.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      faceColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

      VkRenderingInfo faceRender{};
      faceRender.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
      faceRender.renderArea.extent = envScissor.extent;
      faceRender.layerCount = 1;
      faceRender.colorAttachmentCount = 1;
      faceRender.pColorAttachments = &faceColor;
      vkCmdBeginRendering(cmd, &faceRender);
      vkCmdSetViewport(cmd, 0, 1, &envVp);
      vkCmdSetScissor(cmd, 0, 1, &envScissor);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mEnvMapPipeline);
      // The env faces never sample the cloud buffer (passFlags.x forces the
      // analytic deck instead -- the half-res march is the MAIN camera's
      // view, which says nothing about what a cube face sees), but the layout
      // is shared, so set 1 still has to be bound.
      VkDescriptorSet envSkySets[3] = {mFrameSets[mCurrentFrame],
                                       mCloudSampleSets[mCurrentFrame],mAtmosphere.set(mCurrentFrame)};
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              mSkyPipelineLayout, 0, 3, envSkySets, 0, nullptr);

      // Affine unprojector: (ndc.x, ndc.y, 1, 1) -> camPos + F + x*Rt + y*Dn,
      // so the shader's `normalize(farPoint - camPos)` spans this face.
      glm::mat4 faceInv(glm::vec4(kFaceRt[face], 0.0f),
                        glm::vec4(kFaceDn[face], 0.0f),
                        glm::vec4(kFaceF[face], 0.0f), glm::vec4(eye, 1.0f));
      SkyPush envPush = fillSkyPush(faceInv, true);
      vkCmdPushConstants(cmd, mSkyPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(SkyPush), &envPush);
      vkCmdDraw(cmd, 3, 1, 0, 0);
      vkCmdEndRendering(cmd);
    }

    // Mip chain: face-preserving blits, each level half the previous. Deep
    // mips are what the lit shaders read as diffuse irradiance.
    imageBarrier(cmd, mEnvImages[mCurrentFrame],
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_ASPECT_COLOR_BIT, 6, 0, 1);
    imageBarrier(cmd, mEnvImages[mCurrentFrame],
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_ASPECT_COLOR_BIT, 6, 1, kEnvMipCount - 1);
    for (uint32_t mip = 1; mip < kEnvMipCount; ++mip) {
      const int32_t srcSize = static_cast<int32_t>(kEnvFaceSize >> (mip - 1));
      const int32_t dstSize = static_cast<int32_t>(kEnvFaceSize >> mip);
      VkImageBlit blit{};
      blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      blit.srcSubresource.mipLevel = mip - 1;
      blit.srcSubresource.layerCount = 6;
      blit.srcOffsets[1] = {srcSize, srcSize, 1};
      blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      blit.dstSubresource.mipLevel = mip;
      blit.dstSubresource.layerCount = 6;
      blit.dstOffsets[1] = {dstSize, dstSize, 1};
      vkCmdBlitImage(cmd, mEnvImages[mCurrentFrame],
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     mEnvImages[mCurrentFrame],
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                     VK_FILTER_LINEAR);
      if (mip + 1 < kEnvMipCount)
        imageBarrier(cmd, mEnvImages[mCurrentFrame],
                     VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_ASPECT_COLOR_BIT, 6, mip, 1);
    }
    // Levels 0..N-2 are TRANSFER_SRC, the last is TRANSFER_DST -- move all
    // to shader-read for the scene passes.
    imageBarrier(cmd, mEnvImages[mCurrentFrame], VK_PIPELINE_STAGE_2_BLIT_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_ASPECT_COLOR_BIT, 6, 0, kEnvMipCount - 1);
    imageBarrier(cmd, mEnvImages[mCurrentFrame], VK_PIPELINE_STAGE_2_BLIT_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_ASPECT_COLOR_BIT, 6, kEnvMipCount - 1, 1);
  }

  mAtmosphere.recordSkyIrradiance(cmd,mCurrentFrame,mFrameSets[mCurrentFrame],mTlasSets[mCurrentFrame],mSceneEnvSets[mCurrentFrame],physicalSky);
  VkDeviceSize vbOffset = 0;

  // Optional one-shot framebuffer capture (debug / headless verification).
  const bool doCapture = mCapture;
  VkBuffer captureBuf = VK_NULL_HANDLE;
  VmaAllocation captureAlloc = VK_NULL_HANDLE;
  void *captureMapped = nullptr;
  if (doCapture) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(mCtx->allocator(), &bci, &aci, &captureBuf,
                             &captureAlloc, &info));
    captureMapped = info.pMappedData;
  }

  // (Shadows are ray-traced in the scene fragment shader — no shadow pass.)
  (void)vbOffset;

  // ---- Frustum culling (CPU, shared by depth prepass + scene pass) ----
  // Every raster pass previously drew the ENTIRE streamed disc around the
  // camera -- at large view distances most of it behind the viewer. The
  // TLAS keeps the full set (off-screen geometry still casts shadows).
  gpuStamp(9);
  const Frustum frustum = frustumFromViewProj(frameData.viewProj);
  mInstanceVisible.assign(mInstances.size(), 0);
  for (size_t i = 0; i < mInstances.size(); ++i) {
    const Instance &inst = mInstances[i];
    if (inst.meshIndex >= mMeshes.size())
      continue;
    const Mesh &mesh = mMeshes[inst.meshIndex];
    if (!mesh.vertexBuffer)
      continue; // dead slot (evicted while an instance still referenced it)
    bool visible = true;
    if (mesh.hasBounds) {
      glm::vec3 mn, mx;
      transformAabb(inst.model, mesh.boundsMin, mesh.boundsMax, mn, mx);
      visible = aabbInFrustum(frustum, mn, mx);
    }
    mInstanceVisible[i] = visible ? 1 : 0;
    if (visible)
      ++mFrameStats.instancesDrawn;
    else
      ++mFrameStats.instancesCulled;
  }

  // Vegetation: cull per chunk-range, then merge adjacent visible ranges
  // back into contiguous instanced-draw spans (used by both passes below).
  const float globalVegDrawDist = std::max(mParams.vegDrawDistance, 0.0f);
  for (VegSpeciesBuffer &vb : mVegBuffers) {
    vb.visibleSpans.clear();
    if (vb.count == 0 || vb.mesh >= mMeshes.size() ||
        !mMeshes[vb.mesh].vertexBuffer)
      continue;
    // R5: a batch's own draw distance wins over the global one when set.
    // Grass sets a much shorter distance than trees; with per-chunk ranges
    // that would barely matter, but grass batches also carry sub-chunk
    // ranges (ScatterLayer::cullCellSize), so this test has the resolution
    // to actually discard most of a chunk.
    const float vegDrawDist =
        vb.drawDistance > 0.0f ? vb.drawDistance : globalVegDrawDist;
    for (const VegRange &r : vb.ranges) {
      const glm::vec3 cp = glm::clamp(eye, r.boundsMin, r.boundsMax);
      const float distance = glm::distance(cp, eye);
      const bool bounded = r.boundsMin != r.boundsMax;
      if (bounded && (!aabbInFrustum(frustum, r.boundsMin, r.boundsMax) ||
                      (vegDrawDist > 0.0f && distance > vegDrawDist))) {
        ++mFrameStats.vegRangesCulled;
        continue;
      }
      MeshHandle selected = vb.mesh;
      for (const auto &lod : vb.meshLods)
        if (distance >= lod.distance && lod.mesh < mMeshes.size() &&
            mMeshes[lod.mesh].vertexBuffer)
          selected = lod.mesh;
      ++mFrameStats.vegRangesDrawn;
      mFrameStats.vegInstancesDrawn += r.count;
      for (const auto &item : mMeshes[selected].drawItems) {
        const uint64_t triangles = uint64_t(item.indexCount / 3) * r.count;
        mFrameStats.vegTrianglesDrawn += triangles;
        (vb.grass ? mFrameStats.grassTrianglesDrawn : mFrameStats.treeTrianglesDrawn) += triangles;
      }
      if (!vb.visibleSpans.empty() && vb.visibleSpans.back().mesh == selected &&
          vb.visibleSpans.back().first + vb.visibleSpans.back().count == r.first)
        vb.visibleSpans.back().count += r.count;
      else
        vb.visibleSpans.push_back({r.first, r.count, selected});
    }
  }
  mFrameStats.meshSlotsFree = static_cast<uint32_t>(mFreeMeshSlots.size());
  mFrameStats.meshSlotsLive =
      static_cast<uint32_t>(mMeshes.size()) - mFrameStats.meshSlotsFree;

  VkViewport vp{};
  vp.width = static_cast<float>(extent.width);
  vp.height = static_cast<float>(extent.height);
  vp.minDepth = 0.0f;
  vp.maxDepth = 1.0f;
  VkRect2D scissor{};
  scissor.extent = extent;

  // Half-res viewport for volumetrics; SSAO now uses the full-res viewport.
  // Keep this separate from AO's extent when recreating render targets.
  const VkExtent2D ssaoExtent = {std::max(1u, extent.width / 2),
                                 std::max(1u, extent.height / 2)};
  VkViewport ssaoVp{};
  ssaoVp.width = static_cast<float>(ssaoExtent.width);
  ssaoVp.height = static_cast<float>(ssaoExtent.height);
  ssaoVp.minDepth = 0.0f;
  ssaoVp.maxDepth = 1.0f;
  VkRect2D ssaoScissor{};
  ssaoScissor.extent = ssaoExtent;

  const VkShaderStageFlags pushStages =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  // Sets 3 (AO) / 4 (sky env cubemap) are only actually read by the lit
  // fragment shaders, but every scene-layout pipeline (including the depth
  // prepass) binds all 5 sets -- simplest to keep one bind call correct
  // everywhere.
  VkDescriptorSet sets[5] = {mBindless.set(), mFrameSets[mCurrentFrame],
                             mTlasSets[mCurrentFrame],
                             mSceneAOSets[mCurrentFrame],
                             mSceneEnvSets[mCurrentFrame]};

  gpuStamp(1);
  // ---- Pass 0: depth prepass ----
  // Writes real depth first so SSAO (Pass 0b) can sample it, and the main
  // scene pass (Pass 1) can test-only with early-z rejecting occluded
  // fragments before they're shaded (see createScenePipeline's comment).
  imageBarrier(cmd, mDepthImages[mCurrentFrame],
               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
               VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
               VK_IMAGE_ASPECT_DEPTH_BIT);

  VkRenderingAttachmentInfo prepassDepth{};
  prepassDepth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  prepassDepth.imageView = mDepthViews[mCurrentFrame];
  prepassDepth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  prepassDepth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  prepassDepth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  prepassDepth.clearValue.depthStencil = {1.0f, 0};

  VkRenderingInfo prepassRender{};
  prepassRender.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  prepassRender.renderArea.extent = extent;
  prepassRender.layerCount = 1;
  prepassRender.pDepthAttachment = &prepassDepth;
  vkCmdBeginRendering(cmd, &prepassRender);
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mDepthPrepassPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mScenePipelineLayout, 0, 5, sets, 0, nullptr);
  bool alphaDepthPipeline = false;
  for (size_t i = 0; i < mInstances.size(); ++i) {
    if (!mInstanceVisible[i] || mInstances[i].isViewModel)
      continue;
    const Instance &inst = mInstances[i];
    const Mesh &mesh = mMeshes[inst.meshIndex];
    vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertexBuffer, &vbOffset);
    vkCmdBindIndexBuffer(cmd, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    for (const DrawItem &source : mesh.drawItems) {
      DrawItem item=inst.hasMaterialOverride?inst.materialOverride:source;
      item.indexCount=source.indexCount;
      item.indexOffset=source.indexOffset;
      const bool needsAlpha = item.alphaCutoff > 0.0f;
      if (needsAlpha != alphaDepthPipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          needsAlpha ? mDepthAlphaPrepassPipeline
                                     : mDepthPrepassPipeline);
        alphaDepthPipeline = needsAlpha;
      }
      ScenePush push{};
      push.model = inst.model;
      push.textureIndex = item.textureIndex;
      push.opacityIndex = item.opacityIndex;
      push.alphaCutoff = item.alphaCutoff;
      push.materialFlags = item.materialFlags;
      vkCmdPushConstants(cmd, mScenePipelineLayout, pushStages, 0,
                         sizeof(ScenePush), &push);
      vkCmdDrawIndexed(cmd, item.indexCount, 1, item.indexOffset, 0, 0);
    }
  }

  // Vegetation: one instanced draw per visible span per submesh, instead of
  // one draw per placed plant. Material push constants are irrelevant here
  // (the depth-only stage has no fragment shader, and meshInstanced.vert
  // ignores pc.model), but the WIND constants must be pushed and must match
  // the scene pass below exactly: this pass writes the depth the scene pass
  // then tests EQUAL-ish against, so a blade swayed in one pass and not the
  // other would fail its own depth test and vanish.
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    mVegetationDepthPrepassPipeline);
  alphaDepthPipeline = false;
      for (const VegSpeciesBuffer &vb : mVegBuffers) {
    if (vb.visibleSpans.empty())
      continue;
    for (const auto &span : vb.visibleSpans) {
      const Mesh &mesh = mMeshes[span.mesh];
      VkBuffer vertexBuffers[2] = {mesh.vertexBuffer, vb.buffer};
      VkDeviceSize offsets[2] = {0, 0};
      vkCmdBindVertexBuffers(cmd, 0, 2, vertexBuffers, offsets);
      vkCmdBindIndexBuffer(cmd, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
      for (const DrawItem &item : mesh.drawItems) {
        const bool needsAlpha = item.alphaCutoff > 0.0f;
        if (needsAlpha != alphaDepthPipeline) {
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            needsAlpha ? mVegetationDepthAlphaPrepassPipeline
                                       : mVegetationDepthPrepassPipeline);
          alphaDepthPipeline = needsAlpha;
        }
        ScenePush windPush{};
        windPush.textureIndex = item.textureIndex;
        windPush.opacityIndex = item.opacityIndex;
        windPush.alphaCutoff = item.alphaCutoff;
        windPush.materialFlags = item.materialFlags;
        windPush.windStrength = vb.windStrength;
        windPush.windSpeed = vb.windSpeed;
        windPush.windMeshHeight = vb.windMeshHeight;
        vkCmdPushConstants(cmd, mScenePipelineLayout, pushStages, 0,
                           sizeof(ScenePush), &windPush);
        vkCmdDrawIndexed(cmd, item.indexCount, span.count, item.indexOffset, 0,
                         span.first);
      }
    }
  }
  vkCmdEndRendering(cmd);

  imageBarrier(cmd, mDepthImages[mCurrentFrame],
               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
               VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
               VK_IMAGE_ASPECT_DEPTH_BIT);

  gpuStamp(2);
  // ---- Pass 0b: SSAO (full-res, samples the full-res prepass depth) ----
  imageBarrier(cmd, mSSAOImages[mCurrentFrame],
               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  VkRenderingAttachmentInfo ssaoColor{};
  ssaoColor.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  ssaoColor.imageView = mSSAOViews[mCurrentFrame];
  ssaoColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  ssaoColor.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  ssaoColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  VkRenderingInfo ssaoRender{};
  ssaoRender.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  ssaoRender.renderArea.extent = extent;
  ssaoRender.layerCount = 1;
  ssaoRender.colorAttachmentCount = 1;
  ssaoRender.pColorAttachments = &ssaoColor;
  vkCmdBeginRendering(cmd, &ssaoRender);
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mSSAOPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mSSAOPipelineLayout, 0, 1,
                          &mSSAODepthSets[mCurrentFrame], 0, nullptr);
  SSAOPush ssaoPush{};
  ssaoPush.invProj = glm::inverse(proj);
  ssaoPush.proj = proj;
  ssaoPush.radius = mParams.aoRadius;
  ssaoPush.bias = mParams.aoBias;
  ssaoPush.strength = mParams.aoStrength;
  vkCmdPushConstants(cmd, mSSAOPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                     sizeof(SSAOPush), &ssaoPush);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);

  imageBarrier(cmd, mSSAOImages[mCurrentFrame],
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  // ---- Pass 0c: blur (denoise the raw SSAO kernel noise) ----
  imageBarrier(cmd, mSSAOBlurImages[mCurrentFrame],
               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  VkRenderingAttachmentInfo blurColor{};
  blurColor.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  blurColor.imageView = mSSAOBlurViews[mCurrentFrame];
  blurColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  blurColor.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  blurColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  VkRenderingInfo blurRender{};
  blurRender.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  blurRender.renderArea.extent = extent;
  blurRender.layerCount = 1;
  blurRender.colorAttachmentCount = 1;
  blurRender.pColorAttachments = &blurColor;
  vkCmdBeginRendering(cmd, &blurRender);
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mBlurPipeline);
  // Set 0: full-res raw AO. Set 1: full-res depth (reuses mSSAODepthSets,
  // same image the SSAO pass above just read) for the bilateral upsample
  // weight in blur.frag.
  VkDescriptorSet blurSets[2] = {mBlurInputSets[mCurrentFrame],
                                 mSSAODepthSets[mCurrentFrame]};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mBlurPipelineLayout, 0, 2, blurSets, 0, nullptr);
  BlurPush blurPush{};
  blurPush.invProj = glm::inverse(proj);
  vkCmdPushConstants(cmd, mBlurPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                     sizeof(BlurPush), &blurPush);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);

  imageBarrier(cmd, mSSAOBlurImages[mCurrentFrame],
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  const glm::mat4 invViewProj = glm::inverse(proj * viewMat);
  gpuStamp(10);
  VulkanAtmosphereRenderer::GpuData atmosphereData;
  const auto& physical = physicalAtmosphere;
  atmosphereData.invViewProj = glm::inverse(unjitteredProjection * viewMat);
  atmosphereData.viewProj = unjitteredProjection * viewMat;
  atmosphereData.invSurfaceViewProj = invViewProj;
  atmosphereData.view = viewMat;
  atmosphereData.camera = glm::vec4(eye, 1);
  atmosphereData.grid.w = physical.range;
  atmosphereData.ground = glm::vec4(physical.groundExtinction, physical.groundFalloff, physical.groundReference, physical.start);
  atmosphereData.groundAlbedo = glm::vec4(physical.groundAlbedo, physical.groundAnisotropy);
  atmosphereData.dust = glm::vec4(physical.dustExtinction, mParams.volumetricHeightFalloffScale, physical.dustAnisotropy, 0);
  atmosphereData.dustAlbedo = glm::vec4(physical.dustAlbedo, physical.referenceSceneRadiance);
  atmosphereData.history = glm::vec4(physical.history ? 1.f : 0.f, physical.historyWeight,
      mParams.fixedTimeSeconds >= 0 ? 0.f : float(mFrameCounter % 8),
      physical.enabled && mParams.debugViewMode == 0 ? (mSceneReady ? 1.f : -1.f) : 0.f);
  atmosphereData.artistic = glm::vec4(physical.groundTintStrength, mParams.fogSkyStrength, mParams.volumetricTurbulence, mParams.volumetricWindSpeed);
  atmosphereData.reference = glm::vec4(physical.referenceRadiance, physical.referenceExtinction);
  atmosphereData.terrain = glm::vec4(physical.terrainExtinction, physical.terrainFalloff, physical.waterBoost, physical.terrainMist ? 1.f : 0.f);
  atmosphereData.valley=glm::vec4(physical.valleyExtinction,physical.valleyDepth,physical.valleyPooling?1.f:0.f,0);
  if(mFrameStats.tlasRebuiltThisFrame) mAtmosphere.invalidate();
  uint64_t fogLightSignature=fnv1a64(frameData.pointLightPositionRadius.data(),sizeof(frameData.pointLightPositionRadius));
  fogLightSignature=fnv1a64(frameData.pointLightColorIntensity.data(),sizeof(frameData.pointLightColorIntensity),fogLightSignature);
  if(fogLightSignature!=mFogLightSignature) mAtmosphere.invalidate();
  mFogLightSignature=fogLightSignature;
  atmosphereData.controls = glm::vec4(physical.directionalShadows ? 1.f : 0.f,
      physical.skyVisibility ? 1.f : 0.f, sunActive ? 1.f : 0.f, mParams.fovDeg);
  for(uint32_t i = 0; i < pointLightCount; ++i)
    atmosphereData.pointControls[i] = glm::vec4(mParams.pointLights[i].volumetricParticipation,
        mParams.pointLights[i].volumetricShadows ? 1.f : 0.f, 0, 0);
  mAtmosphere.record(cmd, mCurrentFrame, atmosphereData, mFrameSets[mCurrentFrame],
      mTlasSets[mCurrentFrame], mSceneEnvSets[mCurrentFrame]);

  gpuStamp(3);
  // ---- Pass 1: scene -> HDR ----
  imageBarrier(cmd, mHdrImages[mCurrentFrame],
               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  VkRenderingAttachmentInfo sceneColor{};
  sceneColor.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  sceneColor.imageView = mHdrViews[mCurrentFrame];
  sceneColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  sceneColor.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  sceneColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  sceneColor.clearValue.color = {{0.03f, 0.04f, 0.07f, 1.0f}};

  // Reuses the prepass depth: LOAD (not CLEAR) + read-only layout (matches
  // the barrier above) since createScenePipeline's pipeline no longer writes
  // depth, only tests against what the prepass already wrote.
  VkRenderingAttachmentInfo sceneDepth{};
  sceneDepth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  sceneDepth.imageView = mDepthViews[mCurrentFrame];
  sceneDepth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  sceneDepth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  sceneDepth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  VkRenderingInfo sceneRender{};
  sceneRender.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  sceneRender.renderArea.extent = extent;
  sceneRender.layerCount = 1;
  sceneRender.colorAttachmentCount = 1;
  sceneRender.pColorAttachments = &sceneColor;
  sceneRender.pDepthAttachment = &sceneDepth;
  vkCmdBeginRendering(cmd, &sceneRender);

  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  // Sky background first (no depth test/write); geometry draws over it.
  {
    SkyPush skyPush = fillSkyPush(invViewProj, false);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mSkyPipeline);
    VkDescriptorSet skySets[3] = {mFrameSets[mCurrentFrame],
                                  mCloudSampleSets[mCurrentFrame],mAtmosphere.set(mCurrentFrame)};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            mSkyPipelineLayout, 0, 3, skySets, 0, nullptr);
    vkCmdPushConstants(cmd, mSkyPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(SkyPush), &skyPush);
    vkCmdDraw(cmd, 3, 1, 0, 0);
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mScenePipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mScenePipelineLayout, 0, 5, sets, 0, nullptr);

  // One push of the model matrix per instance; one draw per material submesh.
  // Terrain-chunk instances are skipped here and drawn below with
  // mTerrainChunkPipeline (same vertex layout/geometry path, different
  // fragment shader). Both loops draw only frustum-visible instances.
  for (size_t i = 0; i < mInstances.size(); ++i) {
    if (!mInstanceVisible[i] || mInstances[i].isTerrain || mInstances[i].isViewModel)
      continue;
    const Instance &inst = mInstances[i];
    const Mesh &mesh = mMeshes[inst.meshIndex];
    vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertexBuffer, &vbOffset);
    vkCmdBindIndexBuffer(cmd, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    for (const DrawItem &source : mesh.drawItems) {
      DrawItem item=inst.hasMaterialOverride?inst.materialOverride:source;
      item.indexCount=source.indexCount;
      item.indexOffset=source.indexOffset;
      ScenePush push{};
      push.model = inst.model;
      push.textureIndex = item.textureIndex;
      push.roughnessIndex = item.roughnessIndex;
      push.metallicIndex = item.metallicIndex;
      push.aoIndex = item.aoIndex;
      push.normalIndex = item.normalIndex;
      push.opacityIndex = item.opacityIndex;
      push.roughnessScalar = item.roughnessScalar;
      push.metallicScalar = item.metallicScalar;
      push.aoScalar = item.aoScalar;
      push.alphaCutoff = item.alphaCutoff;
      push.materialFlags = item.materialFlags;
      // Regular meshes do not consume the vegetation tail of ScenePush, so
      // use it for per-submesh emissive data without exceeding Vulkan's
      // guaranteed 128-byte push-constant budget.
      push.windStrength = item.emissiveColor.r;
      push.windSpeed = item.emissiveColor.g;
      push.windMeshHeight = item.emissiveColor.b;
      push.groundOcclusion = item.emissiveStrength;
      vkCmdPushConstants(cmd, mScenePipelineLayout, pushStages, 0,
                         sizeof(ScenePush), &push);
      vkCmdDrawIndexed(cmd, item.indexCount, 1, item.indexOffset, 0, 0);
    }
  }

  // Terrain chunks: ordinary CPU-built meshes (Engine/Terrain/), shaded by
  // terrainChunk.frag's height/slope material blend instead of mesh.frag.
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    mTerrainChunkPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mScenePipelineLayout, 0, 5, sets, 0, nullptr);
  for (size_t i = 0; i < mInstances.size(); ++i) {
    if (!mInstanceVisible[i] || !mInstances[i].isTerrain)
      continue;
    const Instance &inst = mInstances[i];
    const Mesh &mesh = mMeshes[inst.meshIndex];
    vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertexBuffer, &vbOffset);
    vkCmdBindIndexBuffer(cmd, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    for (const DrawItem &item : mesh.drawItems) {
      ScenePush push{};
      push.model = inst.model;
      push.textureIndex = item.textureIndex;
      vkCmdPushConstants(cmd, mScenePipelineLayout, pushStages, 0,
                         sizeof(ScenePush), &push);
      vkCmdDrawIndexed(cmd, item.indexCount, 1, item.indexOffset, 0, 0);
    }
  }

  // Vegetation: GPU-instanced -- one draw call per species per submesh
  // regardless of placement count (see setVegetationBatches()). Model comes
  // from the per-instance buffer (binding 1), not the push constant; the
  // rest of ScenePush (material) is constant across the batch, same as any
  // other submesh's DrawItem.
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mVegetationPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mScenePipelineLayout, 0, 5, sets, 0, nullptr);
  for (const VegSpeciesBuffer &vb : mVegBuffers) {
    if (vb.visibleSpans.empty())
      continue;
    for (const auto &span : vb.visibleSpans) {
      const Mesh &mesh = mMeshes[span.mesh];
      VkBuffer vertexBuffers[2] = {mesh.vertexBuffer, vb.buffer};
      VkDeviceSize offsets[2] = {0, 0};
      vkCmdBindVertexBuffers(cmd, 0, 2, vertexBuffers, offsets);
      vkCmdBindIndexBuffer(cmd, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
      for (const DrawItem &item : mesh.drawItems) {
        ScenePush push{};
        push.textureIndex = item.textureIndex;
        push.roughnessIndex = item.roughnessIndex;
        push.metallicIndex = item.metallicIndex;
        push.aoIndex = item.aoIndex;
        push.normalIndex = item.normalIndex;
        push.opacityIndex = item.opacityIndex;
        push.roughnessScalar = item.roughnessScalar;
        push.metallicScalar = item.metallicScalar;
        push.aoScalar = item.aoScalar;
        push.alphaCutoff = item.alphaCutoff;
        push.materialFlags = item.materialFlags;
        // Must match the depth prepass's wind push exactly -- see the comment
        // there.
        push.windStrength = vb.windStrength;
        push.windSpeed = vb.windSpeed;
        push.windMeshHeight = vb.windMeshHeight;
        push.groundOcclusion = vb.groundOcclusion;
        push.foliageSssStrength = vb.foliageSssStrength;
        if (vb.receivesSnow) push.materialFlags |= 1u << 14;
        if (vb.grass) push.materialFlags |= 1u << 15;
        vkCmdPushConstants(cmd, mScenePipelineLayout, pushStages, 0,
                           sizeof(ScenePush), &push);
        vkCmdDrawIndexed(cmd, item.indexCount, span.count, item.indexOffset, 0,
                         span.first);
      }
    }
  }

  // ---- editor debug lines (grid, axes, outlines) ----
  // Last in the scene pass so it depth-tests against everything solid, and
  // still inside it so the lines are part of the HDR image the tonemapper
  // grades -- lines composited after tonemapping would not sit in the scene,
  // they would sit on the screen.
  if (mLinePipeline != VK_NULL_HANDLE && !mDebugLines.empty()) {
    const size_t needed = mDebugLines.size();
    VkBuffer &buffer = mLineBuffers[mCurrentFrame];
    // Grown like a std::vector and never shrunk; the retired buffer waits out
    // the frames still in flight rather than being freed under them.
    if (mLineCapacity[mCurrentFrame] < needed) {
      const size_t capacity = std::max<size_t>(needed * 2, 4096);
      if (buffer != VK_NULL_HANDLE) {
        mPendingGarbage.buffers.emplace_back(buffer, mLineAllocs[mCurrentFrame]);
      }
      VkBufferCreateInfo bufferCi{};
      bufferCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      bufferCi.size = capacity * sizeof(DebugLineVertex);
      bufferCi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
      bufferCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      VmaAllocationCreateInfo allocCi{};
      allocCi.usage = VMA_MEMORY_USAGE_AUTO;
      allocCi.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT;
      VmaAllocationInfo allocInfo{};
      if (vmaCreateBuffer(mCtx->allocator(), &bufferCi, &allocCi, &buffer,
                          &mLineAllocs[mCurrentFrame], &allocInfo) == VK_SUCCESS) {
        mLineMapped[mCurrentFrame] = allocInfo.pMappedData;
        mLineCapacity[mCurrentFrame] = capacity;
      } else {
        buffer = VK_NULL_HANDLE;
        mLineCapacity[mCurrentFrame] = 0;
      }
    }

    if (buffer != VK_NULL_HANDLE && mLineMapped[mCurrentFrame]) {
      std::memcpy(mLineMapped[mCurrentFrame], mDebugLines.data(),
                  needed * sizeof(DebugLineVertex));
      // Flush for non-coherent memory (VMA may select non-HOST_COHERENT heaps).
      vmaFlushAllocation(mCtx->allocator(), mLineAllocs[mCurrentFrame], 0, VK_WHOLE_SIZE);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLinePipeline);
      LinePush linePush{};
      linePush.viewProj = frameData.viewProj;
      linePush.camPosFade =
          glm::vec4(eye, mDebugLineFade > 0.0f ? mDebugLineFade : 1e9f);
      vkCmdPushConstants(cmd, mLinePipelineLayout,
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(LinePush), &linePush);
      VkDeviceSize lineOffset = 0;
      vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &lineOffset);
      vkCmdDraw(cmd, static_cast<uint32_t>(needed), 1, 0, 0);
      mDebugLineCount = static_cast<uint32_t>(needed / 2);
    }
  } else {
    mDebugLineCount = 0;
  }

  vkCmdEndRendering(cmd);

  gpuStamp(4);
  // Preserve raw opaque colour for water before camera-medium composition.
  {
    imageBarrier(cmd, mHdrImages[mCurrentFrame],
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    imageBarrier(cmd, mSceneCopyImages[mCurrentFrame],
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.layerCount = 1;
    region.extent = {extent.width, extent.height, 1};
    vkCmdCopyImage(cmd, mHdrImages[mCurrentFrame],
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   mSceneCopyImages[mCurrentFrame],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    imageBarrier(cmd, mSceneCopyImages[mCurrentFrame],
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    imageBarrier(cmd, mHdrImages[mCurrentFrame],
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  }
  mAtmosphere.compose(cmd, mCurrentFrame, mHdrViews[mCurrentFrame], extent,
      mFrameSets[mCurrentFrame], mTlasSets[mCurrentFrame], mSceneEnvSets[mCurrentFrame]);
  gpuStamp(11);
  if (mParams.waterEnabled && mSceneReady) {
    VkRenderingAttachmentInfo waterColor{};
    waterColor.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    waterColor.imageView = mHdrViews[mCurrentFrame];
    waterColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    waterColor.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    waterColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo waterRender{};
    waterRender.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    waterRender.renderArea.extent = extent;
    waterRender.layerCount = 1;
    waterRender.colorAttachmentCount = 1;
    waterRender.pColorAttachments = &waterColor;
    vkCmdBeginRendering(cmd, &waterRender);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mWaterPipeline);
    // The field set falls back to the scene-copy set when no field has been
    // uploaded yet (terrain off, or the first frames of a load). The shader
    // detects that case from waterParams3.z == 0 and uses the flat sea plane,
    // so what it would sample there is never read.
    VkDescriptorSet waterFieldSet =
        mWaterFieldSet ? mWaterFieldSet : mSceneCopySets[mCurrentFrame];
    VkDescriptorSet waterSets[7] = {
        mSceneCopySets[mCurrentFrame], mSSAODepthSets[mCurrentFrame],
        mFrameSets[mCurrentFrame], mSceneEnvSets[mCurrentFrame], waterFieldSet,
        mTlasSets[mCurrentFrame], mAtmosphere.set(mCurrentFrame)};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            mWaterPipelineLayout, 0, 7, waterSets, 0, nullptr);
    WaterPush waterPush{invViewProj};
    vkCmdPushConstants(cmd, mWaterPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(WaterPush), &waterPush);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
  }

  gpuStamp(12);
  imageBarrier(cmd, mHdrImages[mCurrentFrame],
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  gpuStamp(5);
  // Temporal history is its own attachment, never read and written in one
  // draw. Cross-frame barriers include previous fragment reads before this
  // slot is overwritten, so two frames in flight need no extra CPU wait.
  {
    imageBarrier(cmd, mTemporalImages[mCurrentFrame],
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = mTemporalViews[mCurrentFrame];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo render{};
    render.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    render.renderArea.extent = extent;
    render.layerCount = 1;
    render.colorAttachmentCount = 1;
    render.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &render);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mTemporalPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        mTemporalPipelineLayout, 0, 1, &mTemporalSets[mCurrentFrame], 0, nullptr);
    // Reject cuts/teleports, resize, diagnostics and toggling AA. Wind and
    // smaller camera moves are handled by per-pixel clipping and depth checks.
    const bool reuse = temporalOn && mTemporalValid &&
        glm::distance(eye,mTemporalPrevEye) < 4.0f &&
        glm::dot(forward,mTemporalPrevForward) > .94f;
    const glm::mat4 matrices[2] = {glm::inverse(frameData.viewProj),
        reuse ? mTemporalPrevViewProj : glm::mat4(0.0f)};
    vkCmdPushConstants(cmd, mTemporalPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(matrices), matrices);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    imageBarrier(cmd, mTemporalImages[mCurrentFrame],
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    mTemporalPrevViewProj = frameData.viewProj;
    mTemporalPrevEye = eye; mTemporalPrevForward = forward;
    mTemporalValid = temporalOn;
  }
  // Meter world radiance before weapon, bloom, grading and UI. The readback
  // is only 256 bins, consumed after an existing frame fence on a later frame.
  if(histogramExposure&&mParams.autoExposure)mExposureMeter.record(cmd,mCurrentFrame,mTemporalViews[mCurrentFrame],extent);
  // Copy resolved WORLD color to the presentation target. Never composite the
  // gun into history: reprojecting yesterday's rifle would leave recoil trails.
  imageBarrier(cmd,mTemporalImages[mCurrentFrame],
      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
      VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  imageBarrier(cmd,mHdrImages[mCurrentFrame],
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
      VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageCopy resolvedCopy{};
  resolvedCopy.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
  resolvedCopy.dstSubresource=resolvedCopy.srcSubresource;
  resolvedCopy.extent={extent.width,extent.height,1};
  vkCmdCopyImage(cmd,mTemporalImages[mCurrentFrame],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      mHdrImages[mCurrentFrame],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&resolvedCopy);
  imageBarrier(cmd,mTemporalImages[mCurrentFrame],
      VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  imageBarrier(cmd,mHdrImages[mCurrentFrame],
      VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  // Foreground after temporal resolve, before bloom/tonemapping. Animated
  // viewmodels must not become world history (which produced recoil ghosts).
  if (std::any_of(mInstances.begin(),mInstances.end(),[](const Instance &i){return i.isViewModel;})) {
    imageBarrier(cmd,mHdrImages[mCurrentFrame],
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    imageBarrier(cmd,mViewmodelDepthImages[mCurrentFrame],
        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_ASPECT_DEPTH_BIT);
    VkRenderingAttachmentInfo color{},depth{};
    color.sType=depth.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView=mHdrViews[mCurrentFrame];color.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    depth.imageView=mViewmodelDepthViews[mCurrentFrame];depth.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;depth.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil={1.f,0};
    VkRenderingInfo render{};render.sType=VK_STRUCTURE_TYPE_RENDERING_INFO;
    render.renderArea.extent=extent;render.layerCount=1;
    render.colorAttachmentCount=1;render.pColorAttachments=&color;render.pDepthAttachment=&depth;
    vkCmdBeginRendering(cmd,&render);
    vkCmdSetViewport(cmd,0,1,&vp);vkCmdSetScissor(cmd,0,1,&scissor);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,mViewmodelPipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,mScenePipelineLayout,0,5,sets,0,nullptr);
    for (const Instance &inst:mInstances) {
      if (!inst.isViewModel) continue;
      const Mesh &mesh=mMeshes[inst.meshIndex];
      vkCmdBindVertexBuffers(cmd,0,1,&mesh.vertexBuffer,&vbOffset);
      vkCmdBindIndexBuffer(cmd,mesh.indexBuffer,0,VK_INDEX_TYPE_UINT32);
      for (const DrawItem &source:mesh.drawItems) {
        const DrawItem &item=inst.hasMaterialOverride?inst.materialOverride:source;
        ScenePush push{};push.model=inst.model;
        push.textureIndex=item.textureIndex;push.normalIndex=item.normalIndex;
        push.roughnessIndex=item.roughnessIndex;push.metallicIndex=item.metallicIndex;push.aoIndex=item.aoIndex;
        push.opacityIndex=item.opacityIndex;push.alphaCutoff=item.alphaCutoff;
        push.roughnessScalar=item.roughnessScalar;push.metallicScalar=item.metallicScalar;push.aoScalar=item.aoScalar;
        push.materialFlags=item.materialFlags | (1u<<14) | inst.viewModelFlags;
        push.foliageSssStrength=65.0f;
        push.windStrength=item.emissiveColor.r;push.windSpeed=item.emissiveColor.g;
        push.windMeshHeight=item.emissiveColor.b;push.groundOcclusion=item.emissiveStrength;
        vkCmdPushConstants(cmd,mScenePipelineLayout,pushStages,0,sizeof(push),&push);
        vkCmdDrawIndexed(cmd,source.indexCount,1,source.indexOffset,0,0);
      }
    }
    vkCmdEndRendering(cmd);
    imageBarrier(cmd,mHdrImages[mCurrentFrame],
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  // Normalized six-level bloom redistributes energy in the tonemapper.
  imageBarrier(cmd, mHdrImages[mCurrentFrame], VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  mAtmosphere.bloom(cmd, mCurrentFrame, mHdrImages[mCurrentFrame], mHdrViews[mCurrentFrame],
      mParams.atmosphere.bloomFireflySuppression);
  VkDescriptorImageInfo bloomImage{mSampler, mAtmosphere.bloomView(mCurrentFrame), VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet bloomWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  bloomWrite.dstSet=mTonemapSets[mCurrentFrame];bloomWrite.dstBinding=1;
  bloomWrite.descriptorCount=1;bloomWrite.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bloomWrite.pImageInfo=&bloomImage;vkUpdateDescriptorSets(device,1,&bloomWrite,0,nullptr);

  // ---- Pass 2: tonemap -> swapchain ----
  imageBarrier(cmd, mSwapchain.image(imageIndex),
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  VkRenderingAttachmentInfo tonemapColor{};
  tonemapColor.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  tonemapColor.imageView = mSwapchain.imageView(imageIndex);
  tonemapColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  tonemapColor.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  tonemapColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  VkRenderingInfo tonemapRender{};
  tonemapRender.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  tonemapRender.renderArea.extent = extent;
  tonemapRender.layerCount = 1;
  tonemapRender.colorAttachmentCount = 1;
  tonemapRender.pColorAttachments = &tonemapColor;
  vkCmdBeginRendering(cmd, &tonemapRender);

  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mTonemapPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mTonemapPipelineLayout, 0, 1,
                          &mTonemapSets[mCurrentFrame], 0, nullptr);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mTonemapPipelineLayout, 1, 1,
                          &mFrameSets[mCurrentFrame], 0, nullptr);
  TonemapPush tp{};
  tp.exposure = mParams.exposure;
  tp.gamma = mParams.gamma;
  tp.saturation = mParams.saturation;
  tp.contrast = mParams.contrast;
  tp.vignette = mParams.vignette;
  tp.tonemapMode = mParams.tonemapMode;
  tp.bloomIntensity = mParams.atmosphere.bloomStrength;
  tp.gradeExposureBias = 0.0f;
  tp.gradeTint = glm::vec4(1.0f, 1.0f, 1.0f, mParams.bloomWideIntensity);
  tp.invProj = glm::inverse(proj);

  // R3 camera grade: exponentially smooth the raw (this-frame) camera
  // biome weights toward a ~cameraGradeSmoothTime-second time constant, so
  // crossing a biome boundary eases the whole-frame feel in rather than
  // popping. A small, fixed exposure-bias/tint table per biome (meadow is
  // the neutral baseline, so only forest/mountain contribute a delta) --
  // deliberately NOT the same uniform table as the per-pixel terrain
  // response, since this is a much smaller, whole-frame nudge (plan:
  // "grade deltas are deliberately small ... the per-pixel work carries
  // most of the identity").
  if (mParams.cameraGradeEnabled && mParams.debugViewMode == 0) {
    const auto now = std::chrono::steady_clock::now();
    float dt = 1.0f / 60.0f;
    if (mHasLastGradeUpdate)
      dt = std::chrono::duration<float>(now - mLastGradeUpdate).count();
    mLastGradeUpdate = now;
    mHasLastGradeUpdate = true;

    const float tau = std::max(0.05f, mParams.cameraGradeSmoothTime);
    const float alpha = 1.0f - std::exp(-dt / tau);
    mCameraBiomeWeightsSmoothed =
        glm::mix(mCameraBiomeWeightsSmoothed, mCameraBiomeWeightsRaw, alpha);

    const float wMeadow = mCameraBiomeWeightsSmoothed.x;
    const float wForest = mCameraBiomeWeightsSmoothed.y;
    const float wMountain = mCameraBiomeWeightsSmoothed.z;
    (void)wMeadow; // meadow is the neutral baseline (0 EV, white tint)

    // ~-0.3 EV and a cool-blue-green tint under canopy; ~+0.15 EV and a
    // crisp cool-blue tint on high ground -- both scaled by the global
    // biomeLightingStrength dial (0 = grade fully off) and the panel's
    // own cameraGradeStrength.
    const float strength = mParams.biomeLightingStrength * mParams.cameraGradeStrength;
    const float bias = (-0.3f * wForest + 0.15f * wMountain) * strength;
    const glm::vec3 tint =
        glm::mix(glm::vec3(1.0f),
                 glm::vec3(0.92f, 0.97f, 0.98f) * wForest +
                     glm::vec3(0.95f, 0.98f, 1.05f) * wMountain +
                     glm::vec3(1.0f) * wMeadow,
                 strength);
    tp.gradeExposureBias = bias;
    tp.gradeTint = glm::vec4(tint, mParams.bloomWideIntensity);
  }

  if (mParams.debugViewMode != 0) {
    // Debug views write raw data, not light: present it linearly (gamma
    // still applies so dark ranges stay readable), no grade, no bloom.
    tp.exposure = 1.0f;
    tp.saturation = 1.0f;
    tp.contrast = 1.0f;
    tp.vignette = 0.0f;
    tp.tonemapMode = 3; // linear clamp
    tp.bloomIntensity = 0.0f;
    tp.gradeExposureBias = 0.0f;
    tp.gradeTint = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
  }
  vkCmdPushConstants(cmd, mTonemapPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(TonemapPush), &tp);
  vkCmdDraw(cmd, 3, 1, 0, 0);

  // UI / overlay (e.g. ImGui) draws on top of the tonemapped image.
  if (mOverlay)
    mOverlay(cmd);

  vkCmdEndRendering(cmd);

  if (doCapture) {
    imageBarrier(cmd, mSwapchain.image(imageIndex),
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, mSwapchain.image(imageIndex),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuf, 1,
                           &region);
    imageBarrier(cmd, mSwapchain.image(imageIndex),
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  } else {
    imageBarrier(cmd, mSwapchain.image(imageIndex),
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  }
  gpuStamp(6);
  VK_CHECK(vkEndCommandBuffer(cmd));

  VkSemaphoreSubmitInfo waitSem{};
  waitSem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  waitSem.semaphore = mImageAvailable[mCurrentFrame];
  waitSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSemaphoreSubmitInfo signalSem{};
  signalSem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  signalSem.semaphore = mRenderFinished[imageIndex];
  // Presentation must wait for the layout transition and capture transfer,
  // not just the earlier color write. Acquire remains scoped to color output.
  signalSem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkCommandBufferSubmitInfo cmdInfo{};
  cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  cmdInfo.commandBuffer = cmd;
  VkSubmitInfo2 submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submit.waitSemaphoreInfoCount = 1;
  submit.pWaitSemaphoreInfos = &waitSem;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &cmdInfo;
  submit.signalSemaphoreInfoCount = 1;
  submit.pSignalSemaphoreInfos = &signalSem;
  VK_CHECK(vkQueueSubmit2(mCtx->graphicsQueue(), 1, &submit,
                          mInFlight[mCurrentFrame]));

  VkPresentInfoKHR present{};
  present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &mRenderFinished[imageIndex];
  VkSwapchainKHR sc = mSwapchain.handle();
  present.swapchainCount = 1;
  present.pSwapchains = &sc;
  present.pImageIndices = &imageIndex;
  VkResult presentResult = vkQueuePresentKHR(mCtx->presentQueue(), &present);
  if (presentResult == VK_ERROR_OUT_OF_DATE_KHR ||
      presentResult == VK_SUBOPTIMAL_KHR) {
    recreateSwapchain();
  } else if (presentResult != VK_SUCCESS) {
    std::fprintf(stderr, "[VulkanRHI] vkQueuePresentKHR failed: %s\n",
                 resultString(presentResult));
    std::abort();
  }

  if (doCapture) {
    vkDeviceWaitIdle(device);
    vmaInvalidateAllocation(mCtx->allocator(), captureAlloc, 0, VK_WHOLE_SIZE);
    const uint32_t w = extent.width, h = extent.height;
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    const uint8_t *src = static_cast<const uint8_t *>(captureMapped);
    // Swapchain is B8G8R8A8 -> swizzle to RGBA for the PNG.
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
      rgba[i * 4 + 0] = src[i * 4 + 2];
      rgba[i * 4 + 1] = src[i * 4 + 1];
      rgba[i * 4 + 2] = src[i * 4 + 0];
      rgba[i * 4 + 3] = 255;
    }
    // Optional downscale before writing. A full-resolution capture is ~1.4 MB
    // of PNG, which becomes ~1.9 MB of base64 once an agent bridge inlines it
    // into a model's context -- unusable at a few frames per turntable. A box
    // filter here is the cheapest place to fix that: the alternative is a
    // Python image dependency on the far side of the socket.
    uint32_t outW = w, outH = h;
    std::vector<uint8_t> scaled;
    if (mCaptureMaxDim > 0 && (w > mCaptureMaxDim || h > mCaptureMaxDim)) {
      const uint32_t factor =
          std::max(1u, (std::max(w, h) + mCaptureMaxDim - 1) / mCaptureMaxDim);
      outW = std::max(1u, w / factor);
      outH = std::max(1u, h / factor);
      scaled.resize(static_cast<size_t>(outW) * outH * 4);
      for (uint32_t y = 0; y < outH; ++y) {
        for (uint32_t x = 0; x < outW; ++x) {
          uint32_t acc[4] = {0, 0, 0, 0};
          uint32_t samples = 0;
          for (uint32_t sy = 0; sy < factor; ++sy) {
            const uint32_t srcY = y * factor + sy;
            if (srcY >= h)
              break;
            for (uint32_t sx = 0; sx < factor; ++sx) {
              const uint32_t srcX = x * factor + sx;
              if (srcX >= w)
                break;
              const size_t si = (static_cast<size_t>(srcY) * w + srcX) * 4;
              for (int c = 0; c < 4; ++c)
                acc[c] += rgba[si + c];
              ++samples;
            }
          }
          const size_t di = (static_cast<size_t>(y) * outW + x) * 4;
          for (int c = 0; c < 4; ++c)
            scaled[di + c] =
                static_cast<uint8_t>(samples ? acc[c] / samples : 0);
        }
      }
    }
    const uint8_t *pixels = scaled.empty() ? rgba.data() : scaled.data();

    if (stbi_write_png(mCapturePath.c_str(), static_cast<int>(outW),
                       static_cast<int>(outH), 4, pixels,
                       static_cast<int>(outW) * 4))
      std::fprintf(stderr, "[VulkanRHI] Captured frame -> %s (%ux%u)\n",
                   mCapturePath.c_str(), outW, outH);
    else
      std::fprintf(stderr, "[VulkanRHI] Capture write failed: %s\n",
                   mCapturePath.c_str());
    mCaptureMaxDim = 0; // one-shot, like mCapture itself
    vmaDestroyBuffer(mCtx->allocator(), captureBuf, captureAlloc);
    mCapture = false;
  }

  mGpuTimestampsWritten[mCurrentFrame] = gpuQueries != VK_NULL_HANDLE;
  mCurrentFrame = (mCurrentFrame + 1) % kFramesInFlight;
  ++mFrameCounter;
}

void VulkanRenderer::requestCapture(const std::string &path,
                                    uint32_t maxDimension) {
  mCapture = true;
  mCapturePath = path;
  mCaptureMaxDim = maxDimension;
}

void VulkanRenderer::waitIdle() {
  if (mCtx && mCtx->device())
    vkDeviceWaitIdle(mCtx->device());
}

void VulkanRenderer::shutdown() {
  if (!mCtx || !mCtx->device())
    return;
  VkDevice device = mCtx->device();
  vkDeviceWaitIdle(device);

  for (VkSemaphore s : mRenderFinished)
    vkDestroySemaphore(device, s, nullptr);
  mRenderFinished.clear();
  for (VkSemaphore s : mImageAvailable)
    vkDestroySemaphore(device, s, nullptr);
  mImageAvailable.clear();
  for (VkFence f : mInFlight)
    vkDestroyFence(device, f, nullptr);
  mInFlight.clear();
  for (auto &pool : mGpuTimestampPools) {
    if (pool) vkDestroyQueryPool(device, pool, nullptr);
    pool = VK_NULL_HANDLE;
  }
  mAtmosphere.destroy();
  mCloudHistory.destroy();
  mExposureMeter.destroy();
  mGpuTimestampsWritten.fill(false);
  mGpuTimestampsInitialized = false;


  // Deferred-destruction backlog: the device is idle, so everything queued
  // (staging buffers, retired mesh buffers, released BLAS) can go now.
  for (auto &garbage : mFrameGarbage)
    mAccel.freeGarbage(*mCtx, garbage);
  mAccel.freeGarbage(*mCtx, mPendingGarbage);
  mPendingCopies.clear();
  mPendingBlasBuilds.clear();

  for (Mesh &mesh : mMeshes) {
    if (mesh.indexBuffer)
      vmaDestroyBuffer(mCtx->allocator(), mesh.indexBuffer, mesh.indexAlloc);
    if (mesh.vertexBuffer)
      vmaDestroyBuffer(mCtx->allocator(), mesh.vertexBuffer, mesh.vertexAlloc);
    if (mesh.alphaMaskBuffer)
      vmaDestroyBuffer(mCtx->allocator(), mesh.alphaMaskBuffer, mesh.alphaMaskAlloc);
  }
  mMeshes.clear();
  mInstances.clear();

  for (VegSpeciesBuffer &vb : mVegBuffers)
    if (vb.buffer)
      vmaDestroyBuffer(mCtx->allocator(), vb.buffer, vb.alloc);
  mVegBuffers.clear();

  for (VkImageView v : mTextureViews)
    vkDestroyImageView(device, v, nullptr);
  for (size_t i = 0; i < mTextureImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mTextureImages[i], mTextureAllocs[i]);
  mTextureViews.clear();
  mTextureImages.clear();
  mTextureAllocs.clear();
  if (mSampler)
    vkDestroySampler(device, mSampler, nullptr);
  mSampler = VK_NULL_HANDLE;
  if (mMaterialSampler)
    vkDestroySampler(device, mMaterialSampler, nullptr);
  mMaterialSampler = VK_NULL_HANDLE;

  mAccel.destroy(*mCtx);
  if (mTlasPool)
    vkDestroyDescriptorPool(device, mTlasPool, nullptr);
  if (mTlasSetLayout)
    vkDestroyDescriptorSetLayout(device, mTlasSetLayout, nullptr);
  mTlasPool = VK_NULL_HANDLE;
  mTlasSetLayout = VK_NULL_HANDLE;
  destroySceneTargets();
  destroyEnvMapResources();
  destroyCloudNoiseResources();

  if (mTonemapPool)
    vkDestroyDescriptorPool(device, mTonemapPool, nullptr);
  if (mTonemapSetLayout)
    vkDestroyDescriptorSetLayout(device, mTonemapSetLayout, nullptr);
  mTonemapPool = VK_NULL_HANDLE;
  mTonemapSetLayout = VK_NULL_HANDLE;

  if (mTemporalPool) vkDestroyDescriptorPool(device, mTemporalPool, nullptr);
  if (mTemporalPipelineLayout) vkDestroyPipelineLayout(device, mTemporalPipelineLayout, nullptr);
  mTemporalPool = VK_NULL_HANDLE;
  mTemporalPipelineLayout = VK_NULL_HANDLE;
  mTemporalPipeline = VK_NULL_HANDLE; // pipeline cache owns pipeline lifetime

  if (mAOSamplerPool)
    vkDestroyDescriptorPool(device, mAOSamplerPool, nullptr);
  if (mAOSamplerSetLayout)
    vkDestroyDescriptorSetLayout(device, mAOSamplerSetLayout, nullptr);
  mAOSamplerPool = VK_NULL_HANDLE;
  mAOSamplerSetLayout = VK_NULL_HANDLE;

  if (mBloomSamplerPool)
    vkDestroyDescriptorPool(device, mBloomSamplerPool, nullptr);
  mBloomSamplerPool = VK_NULL_HANDLE;

  for (RtAlphaFrameBuffer &fb : mRtAlphaBuffers)
    if (fb.buffer)
      vmaDestroyBuffer(mCtx->allocator(), fb.buffer, fb.alloc);
  mRtAlphaBuffers.clear();
  for (uint32_t i = 0; i < mFrameUBOs.size(); ++i)
    vmaDestroyBuffer(mCtx->allocator(), mFrameUBOs[i], mFrameUBOAllocs[i]);
  mFrameUBOs.clear();
  mFrameUBOAllocs.clear();
  mFrameUBOMapped.clear();
  if (mFrameDescPool)
    vkDestroyDescriptorPool(device, mFrameDescPool, nullptr);
  if (mFrameSetLayout)
    vkDestroyDescriptorSetLayout(device, mFrameSetLayout, nullptr);
  mFrameDescPool = VK_NULL_HANDLE;
  mFrameSetLayout = VK_NULL_HANDLE;

  mBindless.destroy(*mCtx);

  if (mCommandPool) {
    vkDestroyCommandPool(device, mCommandPool, nullptr);
    mCommandPool = VK_NULL_HANDLE;
  }
  if (mTonemapPipelineLayout)
    vkDestroyPipelineLayout(device, mTonemapPipelineLayout, nullptr);
  if (mSkyPipelineLayout)
    vkDestroyPipelineLayout(device, mSkyPipelineLayout, nullptr);
  if (mScenePipelineLayout)
    vkDestroyPipelineLayout(device, mScenePipelineLayout, nullptr);
  if (mLinePipelineLayout)
    vkDestroyPipelineLayout(device, mLinePipelineLayout, nullptr);
  // Line vertex buffers are host-visible and persistently mapped; vmaDestroy
  // handles the unmap. (The pipeline itself belongs to mPipelineCache.)
  for (size_t i = 0; i < mLineBuffers.size(); ++i)
    if (mLineBuffers[i] != VK_NULL_HANDLE)
      vmaDestroyBuffer(mCtx->allocator(), mLineBuffers[i], mLineAllocs[i]);
  mLineBuffers.clear();
  mLineAllocs.clear();
  mLineMapped.clear();
  mLineCapacity.clear();
  if (mSSAOPipelineLayout)
    vkDestroyPipelineLayout(device, mSSAOPipelineLayout, nullptr);
  if (mBlurPipelineLayout)
    vkDestroyPipelineLayout(device, mBlurPipelineLayout, nullptr);
  if (mBloomExtractPipelineLayout)
    vkDestroyPipelineLayout(device, mBloomExtractPipelineLayout, nullptr);
  if (mBloomBlurPipelineLayout)
    vkDestroyPipelineLayout(device, mBloomBlurPipelineLayout, nullptr);
  if (mVolumetricPipelineLayout)
    vkDestroyPipelineLayout(device, mVolumetricPipelineLayout, nullptr);
  if (mVolCompositePipelineLayout)
    vkDestroyPipelineLayout(device, mVolCompositePipelineLayout, nullptr);
  if (mCloudPipelineLayout)
    vkDestroyPipelineLayout(device, mCloudPipelineLayout, nullptr);
  if (mWaterPipelineLayout)
    vkDestroyPipelineLayout(device, mWaterPipelineLayout, nullptr);
  for (VkImageView v : mUiViews)
    vkDestroyImageView(device, v, nullptr);
  for (size_t i = 0; i < mUiImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mUiImages[i], mUiAllocs[i]);
  mUiViews.clear();
  mUiImages.clear();
  mUiAllocs.clear();
  if (mWaterFieldView)
    vkDestroyImageView(device, mWaterFieldView, nullptr);
  if (mWaterFieldImage)
    vmaDestroyImage(mCtx->allocator(), mWaterFieldImage, mWaterFieldAlloc);
  if (mWaterFieldPool)
    vkDestroyDescriptorPool(device, mWaterFieldPool, nullptr);
  mWaterFieldView = VK_NULL_HANDLE;
  mWaterFieldImage = VK_NULL_HANDLE;
  mWaterFieldPool = VK_NULL_HANDLE;
  mWaterFieldSet = VK_NULL_HANDLE;
  mWaterPipelineLayout = VK_NULL_HANDLE;
  mTonemapPipelineLayout = VK_NULL_HANDLE;
  mSkyPipelineLayout = VK_NULL_HANDLE;
  mScenePipelineLayout = VK_NULL_HANDLE;
  mSSAOPipelineLayout = VK_NULL_HANDLE;
  mBlurPipelineLayout = VK_NULL_HANDLE;
  mBloomExtractPipelineLayout = VK_NULL_HANDLE;
  mBloomBlurPipelineLayout = VK_NULL_HANDLE;
  mVolumetricPipelineLayout = VK_NULL_HANDLE;
  mVolCompositePipelineLayout = VK_NULL_HANDLE;
  mCloudPipelineLayout = VK_NULL_HANDLE;

  mPipelineCache.destroy(*mCtx);
  mScenePipeline = VK_NULL_HANDLE;
  mTerrainChunkPipeline = VK_NULL_HANDLE;
  mVegetationPipeline = VK_NULL_HANDLE;
  mSkyPipeline = VK_NULL_HANDLE;
  mEnvMapPipeline = VK_NULL_HANDLE;
  mVolumetricPipeline = VK_NULL_HANDLE;
  mVolCompositePipeline = VK_NULL_HANDLE;
  mCloudPipeline = VK_NULL_HANDLE;
  mTonemapPipeline = VK_NULL_HANDLE;
  mSSAOPipeline = VK_NULL_HANDLE;
  mBlurPipeline = VK_NULL_HANDLE;
  mDepthPrepassPipeline = VK_NULL_HANDLE;
  mDepthAlphaPrepassPipeline = VK_NULL_HANDLE;
  mVegetationDepthPrepassPipeline = VK_NULL_HANDLE;
  mVegetationDepthAlphaPrepassPipeline = VK_NULL_HANDLE;
  mBloomExtractPipeline = VK_NULL_HANDLE;
  mBloomBlurPipeline = VK_NULL_HANDLE;

  mSwapchain.destroy(*mCtx);
}

} // namespace vkrhi

#pragma once
#include "VulkanContext.h"
#include "Rendering/AtmosphereSettings.h"
#include <glm/glm.hpp>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace vkrhi {
// Fog owns resources and passes; the renderer still owns frame ordering.
// All methods run on the render thread. Retirement uses existing frame fences.
class VulkanAtmosphereRenderer {
public:
  struct alignas(16) GpuData {
    glm::mat4 invViewProj{1}, prevViewProj{1}, prevView{1}, invSurfaceViewProj{1}, view{1};
    glm::vec4 camera{0}, previousCamera{0};
    glm::vec4 grid{160,90,64,180};
    glm::vec4 ground{.006f,.045f,-2,0};
    glm::vec4 groundAlbedo{.9f,.9f,.9f,.55f};
    glm::vec4 dust{.00175f,1,.87f,0};
    glm::vec4 dustAlbedo{.8f,.8f,.8f,0};
    glm::vec4 history{0,.9f,0,0}; // valid, weight, sampling index, enabled
    glm::vec4 controls{1,1,0,0}; // directional shadow, sky visibility, sun active, FOV
    glm::vec4 terrain{.002f,.4f,1,0};
    glm::vec4 terrainField{0,0,0,0}; // origin XZ, span, validity
    std::array<glm::vec4,4> pointControls{}; // participation, shadow enable
    glm::vec4 reference{.7f,.7f,.7f,-1};
    glm::vec4 artistic{0,1,0,0}; // tint strength, sky strength, turbulence, wind
    glm::mat4 viewProj{1}; // cache the unjittered inverse's inverse on the CPU
    glm::vec4 valley{.001f,8,1,0}; // extinction, maximum pool depth, enabled
  };
  static_assert(offsetof(GpuData,camera)==320,"atmosphereData.glsl layout");
  static_assert(offsetof(GpuData,pointControls)==496,"atmosphereData.glsl layout");
  static_assert(offsetof(GpuData,viewProj)==592,"atmosphereData.glsl view projection");
  static_assert(offsetof(GpuData,valley)==656,"atmosphereData.glsl valley");
  static_assert(sizeof(GpuData)==672,"atmosphereData.glsl layout");
  struct SkyData {glm::mat4 invViewProj{1};glm::vec4 camera{0},sun{0},moon{0},controls{0};};
  static_assert(sizeof(SkyData)==128,"skyAtmosphere.comp push layout");
  struct Timings {float visibility=0,injection=0,history=0,integration=0,composition=0;};
  const Timings& timings() const {return mTimings;}
  bool init(VulkanContext&,const std::string&,VkDescriptorSetLayout frame,
            VkDescriptorSetLayout tlas,VkDescriptorSetLayout environment,VkDescriptorSetLayout cloudNoise);
  // Called after the current frame's existing fence completes.
  void prepare(uint32_t frame,uint64_t serial,VkExtent2D viewport,atmosphere::Quality);
  void updateInputs(uint32_t frame,VkImageView unfogged,VkImageView depth);
  void recordSky(VkCommandBuffer,uint32_t frame,SkyData,bool enabled);
  float skyMilliseconds() const {return mSkyMilliseconds;}
  float cloudShadowMilliseconds() const {return mCloudShadowMilliseconds;}
  void recordCloudShadow(VkCommandBuffer,uint32_t frame,VkDescriptorSet noise,VkDescriptorSet shared,glm::vec4 field,uint64_t signature,bool enabled);
  VkImageView cloudShadowView() const {return mResources->cloudShadow.view;}
  VkBuffer skyIrradianceBuffer() const {return mResources->skyIrradiance;}
  void recordSkyIrradiance(VkCommandBuffer,uint32_t frame,VkDescriptorSet shared,VkDescriptorSet tlas,VkDescriptorSet environment,bool enabled);
  void record(VkCommandBuffer,uint32_t frame,GpuData,VkDescriptorSet frameSet,
              VkDescriptorSet tlas,VkDescriptorSet environment);
  void compose(VkCommandBuffer,uint32_t frame,VkImageView hdr,VkExtent2D,
               VkDescriptorSet frameSet,VkDescriptorSet tlas,VkDescriptorSet environment);
  void bloom(VkCommandBuffer,uint32_t frame,VkImage hdrImage,VkImageView hdr,bool fireflySuppression);
  VkImageView bloomView(uint32_t frame) const {return mResources->bloom[frame][0].view;}
  float bloomMilliseconds() const {return mBloomMilliseconds;}
  float cpuFogMilliseconds() const {return mCpuPrepareMs+mCpuRecordMs+mCpuCompositionMs;}
  float cpuBloomMilliseconds() const {return mCpuBloomMs;}
  void enableProbe(bool enabled) {mProbeEnabled=enabled;}
  nlohmann::json probe() const {return mProbe;}
  VkDescriptorSetLayout layout() const {return mLayout;}
  VkDescriptorSet set(uint32_t frame) const;
  void invalidate() {mHistoryValid=false;mSamplingIndex=0;}
  void terrainField(std::vector<glm::vec4> values,glm::vec2 origin,float span) {
    mTerrainValues=std::move(values);mTerrainOrigin=origin;mTerrainSpan=span;
    ++mTerrainRevision;invalidate();
  }
  uint64_t allocatedBytes() const;
  void destroy();
private:
  struct Image {VkImage image=VK_NULL_HANDLE;VmaAllocation allocation=nullptr;VkImageView view=VK_NULL_HANDLE;uint64_t bytes=0;};
  struct Resources {
    std::array<Image,2> raw,history,integrated,visibility;
    std::array<std::array<Image,6>,2> bloom;
    std::array<Image,2> terrain;
    std::array<Image,6> sky;
    Image cloudShadow;
    VkBuffer skyIrradiance=VK_NULL_HANDLE;
    VmaAllocation skyIrradianceAllocation=nullptr;
    bool skyIrradianceInitialized=false;
    bool cloudShadowInitialized=false,cloudShadowValid=false;
    uint64_t cloudShadowSignature=0;
    bool skyInitialized=false,skyStaticValid=false,skyViewValid=false;
    SkyData previousSky{};
    std::array<VkBuffer,2> terrainStaging{};
    std::array<VmaAllocation,2> terrainStagingAllocations{};
    std::array<void*,2> terrainMapped{};
    std::array<uint64_t,2> terrainRevision{};
    std::array<VkBuffer,2> probeBuffers{};
    std::array<VmaAllocation,2> probeAllocations{};
    std::array<void*,2> probeMapped{};
    std::array<bool,2> probeWritten{};
    std::array<GpuData,2> probeData{};
    std::array<SkyData,2> probeSkyData{};
    uint64_t skyStaticBuilds=0,skyViewBuilds=0,skyAerialBuilds=0;
    std::array<std::array<VkDescriptorSet,11>,2> bloomSets{};
    VkDescriptorPool bloomPool=VK_NULL_HANDLE;
    std::array<VkBuffer,2> uniforms{};
    std::array<VmaAllocation,2> uniformAllocations{};
    std::array<void*,2> mapped{};
    VkDescriptorPool pool=VK_NULL_HANDLE;
    std::array<VkDescriptorSet,2> sets{};
    VkExtent3D grid{};
    VkExtent2D viewport{};
    bool initialized=false;
  };
  struct Retired {std::unique_ptr<Resources> resources;uint64_t serial;};
  std::unique_ptr<Resources> createResources(VkExtent3D,VkExtent2D);
  Image createImage(VkExtent3D,bool twoDimensional=false,VkFormat format=VK_FORMAT_R16G16B16A16_SFLOAT);
  void release(Resources&);
  VulkanContext* mContext=nullptr;
  VkDescriptorSetLayout mLayout=VK_NULL_HANDLE;
  VkPipelineLayout mPipelineLayout=VK_NULL_HANDLE;
  std::array<VkPipeline,4> mCompute{};
  VkPipeline mComposite=VK_NULL_HANDLE;
  VkPipelineLayout mSkyLayout=VK_NULL_HANDLE;
  VkPipeline mSkyCompute=VK_NULL_HANDLE;
  VkPipeline mSkyIrradiancePipeline=VK_NULL_HANDLE;
  VkPipelineLayout mCloudShadowLayout=VK_NULL_HANDLE;
  VkPipeline mCloudShadowPipeline=VK_NULL_HANDLE;
  std::array<VkQueryPool,2> mSkyQueries{};
  std::array<bool,2> mSkyQueryWritten{};
  float mSkyMilliseconds=0;
  float mCloudShadowMilliseconds=0;
  VkDescriptorSetLayout mBloomLayout=VK_NULL_HANDLE;
  VkPipelineLayout mBloomPipelineLayout=VK_NULL_HANDLE;
  VkPipeline mBloomPipeline=VK_NULL_HANDLE;
  VkSampler mSampler=VK_NULL_HANDLE;
  std::unique_ptr<Resources> mResources;
  std::vector<Retired> mRetired;
  GpuData mPrevious{};
  bool mHistoryValid=false;
  std::array<VkQueryPool,2> mQueries{};
  std::array<bool,2> mQueryWritten{};
  Timings mTimings;
  float mBloomMilliseconds=0;
  std::vector<glm::vec4> mTerrainValues;
  glm::vec2 mTerrainOrigin{0};
  float mTerrainSpan=0;
  uint64_t mTerrainRevision=1;
  bool mProbeEnabled=false;
  nlohmann::json mProbe;
  uint32_t mSamplingIndex=0;
  float mCpuPrepareMs=0,mCpuRecordMs=0,mCpuCompositionMs=0,mCpuBloomMs=0;
};
} // namespace vkrhi

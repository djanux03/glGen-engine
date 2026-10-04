#pragma once
#include "VulkanContext.h"
#include <glm/glm.hpp>
#include <array>
#include <memory>
#include <string>
#include <vector>
#include <json.hpp>

namespace vkrhi {
// Cloud reconstruction has its own scattering depth and history. Scene TAA
// cannot reproject a kilometre-distant volume as the sky's infinite ray.
class VulkanCloudHistory {
public:
  struct alignas(16) Data {
    glm::mat4 invViewProj{1},previousViewProj{1};
    glm::vec4 camera{0},previousCamera{0},wind{0},controls{0};
  };
  static_assert(sizeof(Data)==192,"cloudHistory.comp uniform layout");
  bool init(VulkanContext&,const std::string& directory);
  void prepare(uint32_t frame,uint64_t serial,VkExtent2D);
  VkImage depthImage(uint32_t frame) const {return mResources->depth[frame].image;}
  VkImageView depthView(uint32_t frame) const {return mResources->depth[frame].view;}
  VkImageView outputView(uint32_t frame) const {return mResources->history[frame].view;}
  void resolve(VkCommandBuffer,uint32_t frame,VkImage rawImage,VkImageView raw,Data,uint64_t signature,bool enabled);
  void enableProbe(bool enabled) {mProbeEnabled=enabled;}
  nlohmann::json probe() const {return mProbe;}
  void invalidate() {mValid=false;}
  float milliseconds() const {return mMilliseconds;}
  uint64_t allocatedBytes() const;
  void destroy();
private:
  struct Image {VkImage image=VK_NULL_HANDLE;VkImageView view=VK_NULL_HANDLE;VmaAllocation allocation=nullptr;uint64_t bytes=0;};
  struct Resources {
    VkExtent2D size{};
    std::array<Image,2> depth,history,historyDepth;
    std::array<VkBuffer,2> uniform{};
    std::array<VmaAllocation,2> uniformAllocation{};
    std::array<void*,2> mapped{};
    std::array<VkDescriptorSet,2> sets{};
    std::array<VkBuffer,2> probeBuffers{};
    std::array<VmaAllocation,2> probeAllocations{};
    std::array<void*,2> probeMapped{};
    std::array<bool,2> probeWritten{},probeReused{};
    VkDescriptorPool pool=VK_NULL_HANDLE;
    bool initialized=false;
  };
  struct Retired {std::unique_ptr<Resources> resources;uint64_t serial;};
  Image createImage(VkExtent2D,VkFormat,bool attachment=false);
  std::unique_ptr<Resources> createResources(VkExtent2D);
  void release(Resources&);
  VulkanContext* mContext=nullptr;
  VkDescriptorSetLayout mLayout=VK_NULL_HANDLE;
  VkPipelineLayout mPipelineLayout=VK_NULL_HANDLE;
  VkPipeline mPipeline=VK_NULL_HANDLE;
  VkSampler mSampler=VK_NULL_HANDLE;
  std::array<VkQueryPool,2> mQueries{};
  std::array<bool,2> mQueryWritten{};
  uint64_t mResetCount=0;
  std::unique_ptr<Resources> mResources;
  std::vector<Retired> mRetired;
  Data mPrevious{};
  glm::vec3 mPreviousWind{0};
  uint64_t mSignature=0;
  bool mValid=false;
  float mMilliseconds=0;
  bool mProbeEnabled=false;
  nlohmann::json mProbe;
};
} // namespace vkrhi

#pragma once
#include "VulkanContext.h"
#include "Rendering/ExposureMeter.h"
#include <array>
#include <string>

namespace vkrhi {
// Histograms retire with the renderer's existing frame-slot fence. No queue
// wait or synchronous pixel readback is added to automatic exposure.
class VulkanExposureMeter {
public:
  bool init(VulkanContext&,const std::string& directory);
  void prepare(uint32_t frame);
  void record(VkCommandBuffer,uint32_t frame,VkImageView world,VkExtent2D);
  bool ready() const {return mReady;}
  double luminance() const {return mLuminance;}
  float milliseconds() const {return mMilliseconds;}
  uint64_t allocatedBytes() const {return 2*sizeof(atmosphere::LuminanceHistogram);}
  void destroy();
private:
  VulkanContext* mContext=nullptr;
  VkDescriptorSetLayout mLayout=VK_NULL_HANDLE;
  VkPipelineLayout mPipelineLayout=VK_NULL_HANDLE;
  VkPipeline mPipeline=VK_NULL_HANDLE;
  VkDescriptorPool mPool=VK_NULL_HANDLE;
  VkSampler mSampler=VK_NULL_HANDLE;
  std::array<VkDescriptorSet,2> mSets{};
  std::array<VkBuffer,2> mBuffers{};
  std::array<VmaAllocation,2> mAllocations{};
  std::array<void*,2> mMapped{};
  std::array<VkQueryPool,2> mQueries{};
  std::array<bool,2> mWritten{};
  bool mReady=false;
  double mLuminance=0;
  float mMilliseconds=0;
};
} // namespace vkrhi

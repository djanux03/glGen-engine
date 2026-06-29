#pragma once

#include "VulkanContext.h"

namespace vkrhi {

// Global bindless descriptor set (set 0): a single large, partially-bound,
// update-after-bind array of combined image samplers. Every texture the
// renderer uses is registered here once and thereafter referenced by a uint
// index (delivered via push constant / uniform), never by re-binding.
//
// This is intentionally the *first* thing built in the Vulkan rewrite: it is
// the foundation the eventual GPU-driven and ray-tracing paths depend on, and
// it kills the per-draw descriptor churn that makes naive ports slow.
class VulkanBindless {
public:
  static constexpr uint32_t kMaxTextures = 1024;
  static constexpr uint32_t kTextureBinding = 0;

  bool init(VulkanContext &ctx);
  void destroy(VulkanContext &ctx);

  VkDescriptorSetLayout layout() const { return mLayout; }
  VkDescriptorSet set() const { return mSet; }

  // Writes the view+sampler into the next free slot and returns its index.
  uint32_t registerTexture(VulkanContext &ctx, VkImageView view,
                           VkSampler sampler);

private:
  VkDescriptorSetLayout mLayout = VK_NULL_HANDLE;
  VkDescriptorPool mPool = VK_NULL_HANDLE;
  VkDescriptorSet mSet = VK_NULL_HANDLE;
  uint32_t mNextSlot = 0;
};

} // namespace vkrhi

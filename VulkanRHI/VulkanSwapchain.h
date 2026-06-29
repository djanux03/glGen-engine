#pragma once

#include "VulkanContext.h"

#include <vector>

namespace vkrhi {

// Thin wrapper over VkSwapchainKHR + its image views. Recreatable on resize.
class VulkanSwapchain {
public:
  // `fbWidth/fbHeight` are the framebuffer pixel size (caller clamps to the
  // surface caps). Returns false if the surface area is zero (minimized).
  bool create(VulkanContext &ctx, VkSurfaceKHR surface, uint32_t fbWidth,
              uint32_t fbHeight);
  void destroy(VulkanContext &ctx);

  VkSwapchainKHR handle() const { return mSwapchain; }
  VkFormat imageFormat() const { return mFormat; }
  VkExtent2D extent() const { return mExtent; }
  uint32_t imageCount() const { return static_cast<uint32_t>(mImages.size()); }
  VkImage image(uint32_t i) const { return mImages[i]; }
  VkImageView imageView(uint32_t i) const { return mImageViews[i]; }

private:
  VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;
  VkFormat mFormat = VK_FORMAT_UNDEFINED;
  VkExtent2D mExtent{};
  std::vector<VkImage> mImages;       // owned by the swapchain
  std::vector<VkImageView> mImageViews;
};

} // namespace vkrhi

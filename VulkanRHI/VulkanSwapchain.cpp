#include "VulkanSwapchain.h"

#include <algorithm>
#include <vector>

namespace vkrhi {

namespace {

VkSurfaceFormatKHR chooseFormat(VkPhysicalDevice dev, VkSurfaceKHR surface) {
  uint32_t count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &count, formats.data());
  for (const auto &f : formats) {
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      return f;
  }
  return formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                              VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                         : formats[0];
}

VkPresentModeKHR choosePresentMode(VkPhysicalDevice dev, VkSurfaceKHR surface) {
  uint32_t count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface, &count, nullptr);
  std::vector<VkPresentModeKHR> modes(count);
  vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface, &count, modes.data());
  for (auto m : modes)
    if (m == VK_PRESENT_MODE_MAILBOX_KHR)
      return m;
  return VK_PRESENT_MODE_FIFO_KHR; // always available
}

} // namespace

bool VulkanSwapchain::create(VulkanContext &ctx, VkSurfaceKHR surface,
                             uint32_t fbWidth, uint32_t fbHeight) {
  VkSurfaceCapabilitiesKHR caps{};
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physicalDevice(), surface,
                                            &caps);

  VkExtent2D extent;
  if (caps.currentExtent.width != UINT32_MAX) {
    extent = caps.currentExtent;
  } else {
    extent.width = std::clamp(fbWidth, caps.minImageExtent.width,
                              caps.maxImageExtent.width);
    extent.height = std::clamp(fbHeight, caps.minImageExtent.height,
                               caps.maxImageExtent.height);
  }
  if (extent.width == 0 || extent.height == 0)
    return false; // minimized; caller retries later

  uint32_t desiredImages = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && desiredImages > caps.maxImageCount)
    desiredImages = caps.maxImageCount;

  const VkSurfaceFormatKHR surfaceFormat =
      chooseFormat(ctx.physicalDevice(), surface);
  mFormat = surfaceFormat.format;
  mExtent = extent;

  VkSwapchainCreateInfoKHR ci{};
  ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  ci.surface = surface;
  ci.minImageCount = desiredImages;
  ci.imageFormat = surfaceFormat.format;
  ci.imageColorSpace = surfaceFormat.colorSpace;
  ci.imageExtent = extent;
  ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  // TRANSFER_SRC lets us copy the presented image out for debug capture.
  if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
    ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

  const uint32_t families[] = {ctx.graphicsQueueFamily(),
                               ctx.presentQueueFamily()};
  if (ctx.graphicsQueueFamily() != ctx.presentQueueFamily()) {
    ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    ci.queueFamilyIndexCount = 2;
    ci.pQueueFamilyIndices = families;
  } else {
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }

  ci.preTransform = caps.currentTransform;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  ci.presentMode = choosePresentMode(ctx.physicalDevice(), surface);
  ci.clipped = VK_TRUE;
  ci.oldSwapchain = VK_NULL_HANDLE;

  VK_CHECK(vkCreateSwapchainKHR(ctx.device(), &ci, nullptr, &mSwapchain));

  uint32_t count = 0;
  vkGetSwapchainImagesKHR(ctx.device(), mSwapchain, &count, nullptr);
  mImages.resize(count);
  vkGetSwapchainImagesKHR(ctx.device(), mSwapchain, &count, mImages.data());

  mImageViews.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = mImages[i];
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = mFormat;
    vi.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                     VK_COMPONENT_SWIZZLE_IDENTITY,
                     VK_COMPONENT_SWIZZLE_IDENTITY};
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel = 0;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.device(), &vi, nullptr, &mImageViews[i]));
  }
  return true;
}

void VulkanSwapchain::destroy(VulkanContext &ctx) {
  for (VkImageView view : mImageViews)
    if (view)
      vkDestroyImageView(ctx.device(), view, nullptr);
  mImageViews.clear();
  mImages.clear();
  if (mSwapchain) {
    vkDestroySwapchainKHR(ctx.device(), mSwapchain, nullptr);
    mSwapchain = VK_NULL_HANDLE;
  }
}

} // namespace vkrhi

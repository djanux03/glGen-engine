#pragma once

#include "VulkanCommon.h"

#include <vk_mem_alloc.h>

#include <vector>

namespace vkrhi {

// Owns the device-independent + device-level Vulkan objects: instance, debug
// messenger, physical/logical device, queues, and the VMA allocator.
//
// Deliberately GLFW-agnostic. The caller supplies the required instance
// extensions (from glfwGetRequiredInstanceExtensions) and, after the instance
// exists, creates a VkSurfaceKHR which is handed back for device selection.
class VulkanContext {
public:
  struct CreateInfo {
    std::vector<const char *> instanceExtensions;
    bool enableValidation = true;
    const char *appName = "glGen Vulkan";
  };

  // Modern-feature support discovered on the chosen physical device. Phase 0
  // *requires* all of these (see kRequiredDeviceExtensions) so we fail loud on
  // a GPU that cannot host the eventual ray-tracing / mesh-shader paths.
  struct Features {
    bool dynamicRendering = false;
    bool synchronization2 = false;
    bool bufferDeviceAddress = false;
    bool descriptorIndexing = false;
    bool accelerationStructure = false;
    bool rayTracingPipeline = false;
    bool rayQuery = false;
    bool meshShader = false;
  };

  bool createInstance(const CreateInfo &info);
  // `surface` must be created by the caller from instance().
  bool selectAndCreateDevice(VkSurfaceKHR surface);
  void destroy();

  VkInstance instance() const { return mInstance; }
  VkPhysicalDevice physicalDevice() const { return mPhysicalDevice; }
  VkDevice device() const { return mDevice; }
  VmaAllocator allocator() const { return mAllocator; }

  uint32_t graphicsQueueFamily() const { return mGraphicsFamily; }
  uint32_t presentQueueFamily() const { return mPresentFamily; }
  VkQueue graphicsQueue() const { return mGraphicsQueue; }
  VkQueue presentQueue() const { return mPresentQueue; }

  const VkPhysicalDeviceProperties &properties() const { return mProps; }
  const Features &features() const { return mFeatures; }

private:
  bool pickPhysicalDevice(VkSurfaceKHR surface);
  bool createLogicalDevice();
  bool createAllocator();

  VkInstance mInstance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT mDebugMessenger = VK_NULL_HANDLE;
  VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
  VkDevice mDevice = VK_NULL_HANDLE;
  VmaAllocator mAllocator = VK_NULL_HANDLE;

  VkSurfaceKHR mSurface = VK_NULL_HANDLE; // not owned

  uint32_t mGraphicsFamily = UINT32_MAX;
  uint32_t mPresentFamily = UINT32_MAX;
  VkQueue mGraphicsQueue = VK_NULL_HANDLE;
  VkQueue mPresentQueue = VK_NULL_HANDLE;

  VkPhysicalDeviceProperties mProps{};
  Features mFeatures{};
  bool mValidationEnabled = false;
};

} // namespace vkrhi

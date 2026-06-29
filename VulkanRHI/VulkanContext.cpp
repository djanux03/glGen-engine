#include "VulkanContext.h"

#include <array>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace vkrhi {

const char *resultString(VkResult result) {
  switch (result) {
  case VK_SUCCESS: return "VK_SUCCESS";
  case VK_NOT_READY: return "VK_NOT_READY";
  case VK_TIMEOUT: return "VK_TIMEOUT";
  case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
  case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
  case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
  case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
  case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
  case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
  case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
  case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
  case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
  default: return "VK_ERROR_<other>";
  }
}

namespace {

// Phase 0 hard requirement: the GPU must be able to host the modern paths the
// rewrite is being done for. Easy to relax later by trimming this list.
const std::array<const char *, 5> kRequiredDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_EXT_MESH_SHADER_EXTENSION_NAME,
};

constexpr const char *kValidationLayer = "VK_LAYER_KHRONOS_validation";

VKAPI_ATTR VkBool32 VKAPI_CALL
debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
              VkDebugUtilsMessageTypeFlagsEXT /*type*/,
              const VkDebugUtilsMessengerCallbackDataEXT *data,
              void * /*user*/) {
  const char *tag = "INFO";
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    tag = "ERROR";
  else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    tag = "WARNING";
  // Skip verbose/info spam; surface warnings and errors.
  if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) {
    std::fprintf(stderr, "[Vulkan][%s] %s\n", tag, data->pMessage);
  }
  return VK_FALSE;
}

bool hasLayer(const char *name) {
  uint32_t count = 0;
  vkEnumerateInstanceLayerProperties(&count, nullptr);
  std::vector<VkLayerProperties> layers(count);
  vkEnumerateInstanceLayerProperties(&count, layers.data());
  for (const auto &l : layers)
    if (std::strcmp(l.layerName, name) == 0)
      return true;
  return false;
}

VkDebugUtilsMessengerCreateInfoEXT makeDebugCreateInfo() {
  VkDebugUtilsMessengerCreateInfoEXT ci{};
  ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
  ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                   VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  ci.pfnUserCallback = debugCallback;
  return ci;
}

} // namespace

bool VulkanContext::createInstance(const CreateInfo &info) {
  mValidationEnabled = info.enableValidation && hasLayer(kValidationLayer);
  if (info.enableValidation && !mValidationEnabled) {
    std::fprintf(stderr, "[VulkanRHI] Validation requested but layer '%s' not "
                         "found; continuing without it.\n",
                 kValidationLayer);
  }

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = info.appName;
  app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
  app.pEngineName = "glGen";
  app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
  app.apiVersion = VK_API_VERSION_1_3;

  std::vector<const char *> extensions = info.instanceExtensions;
  if (mValidationEnabled)
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

  VkInstanceCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ci.pApplicationInfo = &app;
  ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  ci.ppEnabledExtensionNames = extensions.data();

  VkDebugUtilsMessengerCreateInfoEXT debugCi = makeDebugCreateInfo();
  const char *layers[] = {kValidationLayer};
  if (mValidationEnabled) {
    ci.enabledLayerCount = 1;
    ci.ppEnabledLayerNames = layers;
    // Chained here so instance creation/destruction is itself validated.
    ci.pNext = &debugCi;
  }

  VK_CHECK(vkCreateInstance(&ci, nullptr, &mInstance));

  if (mValidationEnabled) {
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(mInstance, "vkCreateDebugUtilsMessengerEXT"));
    if (create)
      VK_CHECK(create(mInstance, &debugCi, nullptr, &mDebugMessenger));
  }
  return true;
}

bool VulkanContext::pickPhysicalDevice(VkSurfaceKHR surface) {
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(mInstance, &count, nullptr);
  if (count == 0) {
    std::fprintf(stderr, "[VulkanRHI] No Vulkan-capable GPUs found.\n");
    return false;
  }
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(mInstance, &count, devices.data());

  VkPhysicalDevice best = VK_NULL_HANDLE;
  int bestScore = -1;

  for (VkPhysicalDevice dev : devices) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(dev, &props);

    // Required device extensions present?
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, exts.data());
    std::set<std::string> available;
    for (const auto &e : exts)
      available.insert(e.extensionName);
    bool hasAll = true;
    for (const char *req : kRequiredDeviceExtensions) {
      if (!available.count(req)) {
        hasAll = false;
        break;
      }
    }
    if (!hasAll)
      continue;

    // Must have a graphics queue family and a present-capable family.
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, qprops.data());
    bool hasGfx = false, hasPresent = false;
    for (uint32_t i = 0; i < qCount; ++i) {
      if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        hasGfx = true;
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &present);
      if (present)
        hasPresent = true;
    }
    if (!hasGfx || !hasPresent)
      continue;

    int score = 0;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
      score += 1000;
    score += static_cast<int>(props.limits.maxImageDimension2D / 1024);
    if (score > bestScore) {
      bestScore = score;
      best = dev;
    }
  }

  if (best == VK_NULL_HANDLE) {
    std::fprintf(stderr,
                 "[VulkanRHI] No GPU satisfies Phase 0 requirements "
                 "(swapchain + acceleration structure + ray tracing pipeline + "
                 "mesh shader).\n");
    return false;
  }

  mPhysicalDevice = best;
  vkGetPhysicalDeviceProperties(mPhysicalDevice, &mProps);
  std::fprintf(stderr, "[VulkanRHI] Selected GPU: %s (Vulkan %u.%u.%u)\n",
               mProps.deviceName, VK_VERSION_MAJOR(mProps.apiVersion),
               VK_VERSION_MINOR(mProps.apiVersion),
               VK_VERSION_PATCH(mProps.apiVersion));

  // Resolve queue families (prefer a combined graphics+present family).
  uint32_t qCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &qCount, nullptr);
  std::vector<VkQueueFamilyProperties> qprops(qCount);
  vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &qCount,
                                           qprops.data());
  for (uint32_t i = 0; i < qCount; ++i) {
    const bool gfx = (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(mPhysicalDevice, i, surface, &present);
    if (gfx && present) {
      mGraphicsFamily = i;
      mPresentFamily = i;
      break;
    }
    if (gfx && mGraphicsFamily == UINT32_MAX)
      mGraphicsFamily = i;
    if (present && mPresentFamily == UINT32_MAX)
      mPresentFamily = i;
  }
  return mGraphicsFamily != UINT32_MAX && mPresentFamily != UINT32_MAX;
}

bool VulkanContext::createLogicalDevice() {
  // Query the full modern-feature chain so we both verify support and learn
  // what to turn on.
  VkPhysicalDeviceMeshShaderFeaturesEXT mesh{};
  mesh.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;

  VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt{};
  rt.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
  rt.pNext = &mesh;

  VkPhysicalDeviceAccelerationStructureFeaturesKHR accel{};
  accel.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
  accel.pNext = &rt;

  VkPhysicalDeviceVulkan13Features v13{};
  v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  v13.pNext = &accel;

  VkPhysicalDeviceVulkan12Features v12{};
  v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  v12.pNext = &v13;

  VkPhysicalDeviceFeatures2 features2{};
  features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features2.pNext = &v12;
  vkGetPhysicalDeviceFeatures2(mPhysicalDevice, &features2);

  mFeatures.dynamicRendering = v13.dynamicRendering;
  mFeatures.synchronization2 = v13.synchronization2;
  mFeatures.bufferDeviceAddress = v12.bufferDeviceAddress;
  mFeatures.descriptorIndexing = v12.descriptorIndexing;
  mFeatures.accelerationStructure = accel.accelerationStructure;
  mFeatures.rayTracingPipeline = rt.rayTracingPipeline;
  mFeatures.meshShader = mesh.meshShader;

  if (!mFeatures.dynamicRendering || !mFeatures.synchronization2) {
    std::fprintf(stderr, "[VulkanRHI] GPU lacks dynamic rendering / "
                         "synchronization2 (Vulkan 1.3 core).\n");
    return false;
  }
  std::fprintf(stderr,
               "[VulkanRHI] Modern features: dynamicRendering=%d sync2=%d "
               "bufferDeviceAddress=%d descriptorIndexing=%d accelStruct=%d "
               "rayTracingPipeline=%d meshShader=%d\n",
               mFeatures.dynamicRendering, mFeatures.synchronization2,
               mFeatures.bufferDeviceAddress, mFeatures.descriptorIndexing,
               mFeatures.accelerationStructure, mFeatures.rayTracingPipeline,
               mFeatures.meshShader);

  // Re-use the queried chain as the *enable* chain: every bit reported above is
  // supported. But a few mesh-shader sub-features carry dependencies on other
  // features we do not turn on in Phase 0 (multiview, primitive fragment
  // shading rate). Leaving them enabled makes vkCreateDevice fail validation,
  // so disable them explicitly. meshShader/taskShader stay on for the eventual
  // mesh-shader terrain path.
  mesh.multiviewMeshShader = VK_FALSE;
  mesh.primitiveFragmentShadingRateMeshShader = VK_FALSE;

  const float priority = 1.0f;
  std::set<uint32_t> uniqueFamilies = {mGraphicsFamily, mPresentFamily};
  std::vector<VkDeviceQueueCreateInfo> queueInfos;
  for (uint32_t fam : uniqueFamilies) {
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = fam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    queueInfos.push_back(qi);
  }

  std::vector<const char *> deviceExtensions(kRequiredDeviceExtensions.begin(),
                                             kRequiredDeviceExtensions.end());

  VkDeviceCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  ci.pNext = &features2; // feature chain; pEnabledFeatures must stay null
  ci.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
  ci.pQueueCreateInfos = queueInfos.data();
  ci.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
  ci.ppEnabledExtensionNames = deviceExtensions.data();

  VK_CHECK(vkCreateDevice(mPhysicalDevice, &ci, nullptr, &mDevice));
  vkGetDeviceQueue(mDevice, mGraphicsFamily, 0, &mGraphicsQueue);
  vkGetDeviceQueue(mDevice, mPresentFamily, 0, &mPresentQueue);
  return true;
}

bool VulkanContext::createAllocator() {
  VmaAllocatorCreateInfo ci{};
  ci.physicalDevice = mPhysicalDevice;
  ci.device = mDevice;
  ci.instance = mInstance;
  ci.vulkanApiVersion = VK_API_VERSION_1_3;
  // We enable bufferDeviceAddress on the device; VMA must know so allocations
  // are flagged correctly (needed later for ray-tracing acceleration structs).
  if (mFeatures.bufferDeviceAddress)
    ci.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  VK_CHECK(vmaCreateAllocator(&ci, &mAllocator));
  return true;
}

bool VulkanContext::selectAndCreateDevice(VkSurfaceKHR surface) {
  mSurface = surface;
  if (!pickPhysicalDevice(surface))
    return false;
  if (!createLogicalDevice())
    return false;
  if (!createAllocator())
    return false;
  return true;
}

void VulkanContext::destroy() {
  if (mAllocator) {
    vmaDestroyAllocator(mAllocator);
    mAllocator = VK_NULL_HANDLE;
  }
  if (mDevice) {
    vkDestroyDevice(mDevice, nullptr);
    mDevice = VK_NULL_HANDLE;
  }
  if (mDebugMessenger) {
    auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(mInstance, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroy)
      destroy(mInstance, mDebugMessenger, nullptr);
    mDebugMessenger = VK_NULL_HANDLE;
  }
  if (mInstance) {
    vkDestroyInstance(mInstance, nullptr);
    mInstance = VK_NULL_HANDLE;
  }
}

} // namespace vkrhi

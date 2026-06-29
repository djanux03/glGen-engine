#pragma once

// Central include + error-handling helpers for the experimental Vulkan RHI
// (Phase 0). Kept deliberately free of GLFW and of the rest of the engine so
// the RHI can be built and reasoned about in isolation.
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>

namespace vkrhi {

const char *resultString(VkResult result);

} // namespace vkrhi

// Aborts loudly on any non-success VkResult. During the port we want every
// Vulkan error to be a hard stop with file/line, not a silent miscompose.
#define VK_CHECK(expr)                                                          \
  do {                                                                         \
    VkResult vk_check_result__ = (expr);                                       \
    if (vk_check_result__ != VK_SUCCESS) {                                     \
      std::fprintf(stderr, "[VulkanRHI] VK_CHECK failed: %s -> %s at %s:%d\n", \
                   #expr, ::vkrhi::resultString(vk_check_result__), __FILE__,  \
                   __LINE__);                                                  \
      std::abort();                                                            \
    }                                                                          \
  } while (0)

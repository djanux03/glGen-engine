// Compiles the stb_image_write implementation for the Vulkan smoke test's
// framebuffer-capture path. The engine's `stb` lib only compiles the image
// *read* implementation, so VulkanRHI provides the *write* one here (separate
// target, no ODR clash).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

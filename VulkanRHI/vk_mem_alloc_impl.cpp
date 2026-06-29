// Single translation unit that compiles the Vulkan Memory Allocator
// implementation. Every other file includes <vk_mem_alloc.h> for declarations
// only; the implementation lives here exactly once.
//
// We link against vulkan-1.lib from the LunarG SDK, so VMA's default of
// statically resolving Vulkan entry points is correct.
#include <vulkan/vulkan.h>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#pragma once

#include "VulkanContext.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace vkrhi {

// FNV-1a 64-bit, used to build pipeline keys from shader ids + render state.
inline uint64_t fnv1a64(const void *data, size_t size, uint64_t seed = 1469598103934665603ull) {
  const auto *bytes = static_cast<const unsigned char *>(data);
  uint64_t hash = seed;
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return hash;
}
inline uint64_t fnv1a64Str(const std::string &s, uint64_t seed = 1469598103934665603ull) {
  return fnv1a64(s.data(), s.size(), seed);
}

// Get-or-create cache for VkPipeline keyed by a caller-computed hash, layered
// on top of a driver-level VkPipelineCache that is persisted to disk so repeat
// runs skip shader recompilation. Vulkan pipelines are immutable and expensive
// to build, so this is the replacement for OpenGL's GLStateCache / per-draw
// state juggling.
class VulkanPipelineCache {
public:
  bool init(VulkanContext &ctx, const std::string &diskPath);
  void destroy(VulkanContext &ctx);

  // `build` is invoked only on a miss; it must create the pipeline using the
  // provided VkPipelineCache so driver-level caching applies. The cache owns
  // the returned pipelines and destroys them in destroy().
  VkPipeline getOrCreate(uint64_t key,
                         const std::function<VkPipeline(VkPipelineCache)> &build);

  VkPipelineCache handle() const { return mCache; }

private:
  VkDevice mDevice = VK_NULL_HANDLE;
  VkPipelineCache mCache = VK_NULL_HANDLE;
  std::unordered_map<uint64_t, VkPipeline> mPipelines;
  std::string mDiskPath;
};

} // namespace vkrhi

#include "VulkanPipelineCache.h"

#include <fstream>
#include <vector>

namespace vkrhi {

bool VulkanPipelineCache::init(VulkanContext &ctx, const std::string &diskPath) {
  mDevice = ctx.device();
  mDiskPath = diskPath;

  std::vector<char> initialData;
  std::ifstream in(diskPath, std::ios::binary | std::ios::ate);
  if (in.is_open()) {
    const std::streamsize size = in.tellg();
    if (size > 0) {
      initialData.resize(static_cast<size_t>(size));
      in.seekg(0);
      in.read(initialData.data(), size);
      std::fprintf(stderr,
                   "[VulkanRHI] Loaded pipeline cache (%lld bytes) from %s\n",
                   static_cast<long long>(size), diskPath.c_str());
    }
  }

  VkPipelineCacheCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  ci.initialDataSize = initialData.size();
  ci.pInitialData = initialData.empty() ? nullptr : initialData.data();
  VK_CHECK(vkCreatePipelineCache(mDevice, &ci, nullptr, &mCache));
  return true;
}

VkPipeline VulkanPipelineCache::getOrCreate(
    uint64_t key, const std::function<VkPipeline(VkPipelineCache)> &build) {
  auto it = mPipelines.find(key);
  if (it != mPipelines.end()) {
    std::fprintf(stderr, "[VulkanRHI] PSO cache hit (key=%016llx)\n",
                 static_cast<unsigned long long>(key));
    return it->second;
  }
  std::fprintf(stderr, "[VulkanRHI] PSO cache miss (key=%016llx), building\n",
               static_cast<unsigned long long>(key));
  VkPipeline pipeline = build(mCache);
  mPipelines.emplace(key, pipeline);
  return pipeline;
}

void VulkanPipelineCache::destroy(VulkanContext &ctx) {
  if (mCache) {
    // Persist driver blob so future runs skip recompilation.
    size_t size = 0;
    if (vkGetPipelineCacheData(mDevice, mCache, &size, nullptr) == VK_SUCCESS &&
        size > 0) {
      std::vector<char> data(size);
      if (vkGetPipelineCacheData(mDevice, mCache, &size, data.data()) ==
          VK_SUCCESS) {
        std::ofstream out(mDiskPath, std::ios::binary | std::ios::trunc);
        if (out.is_open())
          out.write(data.data(), static_cast<std::streamsize>(size));
      }
    }
  }
  for (auto &kv : mPipelines)
    vkDestroyPipeline(ctx.device(), kv.second, nullptr);
  mPipelines.clear();
  if (mCache) {
    vkDestroyPipelineCache(ctx.device(), mCache, nullptr);
    mCache = VK_NULL_HANDLE;
  }
}

} // namespace vkrhi

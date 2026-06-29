#include "VulkanRenderer.h"

#include <glm/gtc/matrix_transform.hpp>

#include "stb_image.h"
#include "stb_image_write.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace vkrhi {

namespace {

void imageBarrier(VkCommandBuffer cmd, VkImage image,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                  VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                  uint32_t layerCount = 1) {
  VkImageMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  barrier.srcStageMask = srcStage;
  barrier.srcAccessMask = srcAccess;
  barrier.dstStageMask = dstStage;
  barrier.dstAccessMask = dstAccess;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = aspect;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = layerCount;

  VkDependencyInfo dep{};
  dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

bool VulkanRenderer::init(
    VulkanContext &ctx, VkSurfaceKHR surface, const std::string &shaderDir,
    const std::string &modelPath,
    std::function<void(uint32_t &, uint32_t &)> queryFramebufferSize) {
  mCtx = &ctx;
  mSurface = surface;
  mQueryFbSize = std::move(queryFramebufferSize);
  mStartTime = std::chrono::steady_clock::now();
  mDrawMeshTasks = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(
      vkGetDeviceProcAddr(ctx.device(), "vkCmdDrawMeshTasksEXT"));

  uint32_t w = 0, h = 0;
  mQueryFbSize(w, h);
  if (!mSwapchain.create(ctx, surface, w, h)) {
    std::fprintf(stderr, "[VulkanRHI] Initial swapchain creation failed.\n");
    return false;
  }
  if (!mPipelineCache.init(ctx, "vk_pipeline_cache.bin"))
    return false;
  if (!createCommandPool())
    return false;
  if (!createDescriptorsAndFrameData())
    return false;
  if (!createSampler())
    return false;
  mDefaultTexIndex = createDefaultTexture();
  if (!createSceneTargets())
    return false;
  if (!createShadowResources())
    return false;
  if (!createTonemapResources())
    return false;
  updateTonemapSets();
  if (!createShadowPipeline(shaderDir))
    return false;
  if (!createScenePipeline(shaderDir))
    return false;
  if (!createTerrainPipeline(shaderDir))
    return false;
  if (!createTonemapPipeline(shaderDir))
    return false;
  if (!loadModel(modelPath))
    return false;
  if (!createSyncObjects())
    return false;
  return true;
}

bool VulkanRenderer::createCommandPool() {
  VkCommandPoolCreateInfo poolCi{};
  poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolCi.queueFamilyIndex = mCtx->graphicsQueueFamily();
  VK_CHECK(
      vkCreateCommandPool(mCtx->device(), &poolCi, nullptr, &mCommandPool));
  return true;
}

void VulkanRenderer::immediateSubmit(
    const std::function<void(VkCommandBuffer)> &record) {
  VkCommandBufferAllocateInfo allocCi{};
  allocCi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocCi.commandPool = mCommandPool;
  allocCi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocCi.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateCommandBuffers(mCtx->device(), &allocCi, &cmd));

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
  record(cmd);
  VK_CHECK(vkEndCommandBuffer(cmd));

  VkCommandBufferSubmitInfo cmdInfo{};
  cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  cmdInfo.commandBuffer = cmd;
  VkSubmitInfo2 submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &cmdInfo;

  VkFenceCreateInfo fenceCi{};
  fenceCi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  VK_CHECK(vkCreateFence(mCtx->device(), &fenceCi, nullptr, &fence));
  VK_CHECK(vkQueueSubmit2(mCtx->graphicsQueue(), 1, &submit, fence));
  VK_CHECK(vkWaitForFences(mCtx->device(), 1, &fence, VK_TRUE, UINT64_MAX));
  vkDestroyFence(mCtx->device(), fence, nullptr);
  vkFreeCommandBuffers(mCtx->device(), mCommandPool, 1, &cmd);
}

void VulkanRenderer::createDeviceLocalBuffer(const void *data, VkDeviceSize size,
                                             VkBufferUsageFlags usage,
                                             VkBuffer &outBuffer,
                                             VmaAllocation &outAlloc) {
  VkBufferCreateInfo stagingCi{};
  stagingCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  stagingCi.size = size;
  stagingCi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  stagingCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo stagingAlloc{};
  stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
  stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VkBuffer staging = VK_NULL_HANDLE;
  VmaAllocation stagingMem = VK_NULL_HANDLE;
  VmaAllocationInfo stagingInfo{};
  VK_CHECK(vmaCreateBuffer(mCtx->allocator(), &stagingCi, &stagingAlloc,
                           &staging, &stagingMem, &stagingInfo));
  std::memcpy(stagingInfo.pMappedData, data, static_cast<size_t>(size));

  VkBufferCreateInfo bufferCi{};
  bufferCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferCi.size = size;
  bufferCi.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bufferCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo deviceAlloc{};
  deviceAlloc.usage = VMA_MEMORY_USAGE_AUTO;
  VK_CHECK(vmaCreateBuffer(mCtx->allocator(), &bufferCi, &deviceAlloc,
                           &outBuffer, &outAlloc, nullptr));

  immediateSubmit([&](VkCommandBuffer cmd) {
    VkBufferCopy copy{};
    copy.size = size;
    vkCmdCopyBuffer(cmd, staging, outBuffer, 1, &copy);
  });
  vmaDestroyBuffer(mCtx->allocator(), staging, stagingMem);
}

bool VulkanRenderer::createSampler() {
  VkSamplerCreateInfo samplerCi{};
  samplerCi.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerCi.magFilter = VK_FILTER_LINEAR;
  samplerCi.minFilter = VK_FILTER_LINEAR;
  samplerCi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  samplerCi.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerCi.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerCi.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerCi.maxLod = VK_LOD_CLAMP_NONE;
  VK_CHECK(vkCreateSampler(mCtx->device(), &samplerCi, nullptr, &mSampler));
  return true;
}

uint32_t VulkanRenderer::addTexture(const uint8_t *rgba, uint32_t w, uint32_t h,
                                    VkFormat format) {
  const VkDeviceSize byteSize = static_cast<VkDeviceSize>(w) * h * 4;

  VkBufferCreateInfo stagingCi{};
  stagingCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  stagingCi.size = byteSize;
  stagingCi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  stagingCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo stagingAlloc{};
  stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
  stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VkBuffer staging = VK_NULL_HANDLE;
  VmaAllocation stagingMem = VK_NULL_HANDLE;
  VmaAllocationInfo stagingInfo{};
  VK_CHECK(vmaCreateBuffer(mCtx->allocator(), &stagingCi, &stagingAlloc,
                           &staging, &stagingMem, &stagingInfo));
  std::memcpy(stagingInfo.pMappedData, rgba, static_cast<size_t>(byteSize));

  VkImageCreateInfo imageCi{};
  imageCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageCi.imageType = VK_IMAGE_TYPE_2D;
  imageCi.format = format;
  imageCi.extent = {w, h, 1};
  imageCi.mipLevels = 1;
  imageCi.arrayLayers = 1;
  imageCi.samples = VK_SAMPLE_COUNT_1_BIT;
  imageCi.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageCi.usage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo imageAlloc{};
  imageAlloc.usage = VMA_MEMORY_USAGE_AUTO;
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation alloc = VK_NULL_HANDLE;
  VK_CHECK(vmaCreateImage(mCtx->allocator(), &imageCi, &imageAlloc, &image,
                          &alloc, nullptr));

  immediateSubmit([&](VkCommandBuffer cmd) {
    imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  });
  vmaDestroyBuffer(mCtx->allocator(), staging, stagingMem);

  VkImageViewCreateInfo viewCi{};
  viewCi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewCi.image = image;
  viewCi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewCi.format = format;
  viewCi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  viewCi.subresourceRange.levelCount = 1;
  viewCi.subresourceRange.layerCount = 1;
  VkImageView view = VK_NULL_HANDLE;
  VK_CHECK(vkCreateImageView(mCtx->device(), &viewCi, nullptr, &view));

  mTextureImages.push_back(image);
  mTextureAllocs.push_back(alloc);
  mTextureViews.push_back(view);
  return mBindless.registerTexture(*mCtx, view, mSampler);
}

uint32_t VulkanRenderer::createDefaultTexture() {
  const uint32_t size = 256;
  const uint32_t cell = 32;
  std::vector<uint8_t> pixels(size * size * 4);
  for (uint32_t y = 0; y < size; ++y) {
    for (uint32_t x = 0; x < size; ++x) {
      const bool on = ((x / cell) + (y / cell)) % 2 == 0;
      uint8_t *p = &pixels[(y * size + x) * 4];
      const uint8_t v = on ? 200 : 70;
      p[0] = v;
      p[1] = v;
      p[2] = v;
      p[3] = 255;
    }
  }
  return addTexture(pixels.data(), size, size, VK_FORMAT_R8G8B8A8_UNORM);
}

uint32_t VulkanRenderer::loadTextureFile(const std::string &path) {
  int w = 0, h = 0, channels = 0;
  stbi_uc *pixels = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
  if (!pixels) {
    std::fprintf(stderr,
                 "[VulkanRHI] Failed to load texture '%s' (%s); using default\n",
                 path.c_str(), stbi_failure_reason());
    return mDefaultTexIndex;
  }
  const uint32_t index =
      addTexture(pixels, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                 VK_FORMAT_R8G8B8A8_SRGB);
  stbi_image_free(pixels);
  std::fprintf(stderr, "[VulkanRHI] Loaded texture '%s' (%dx%d) -> bindless %u\n",
               path.c_str(), w, h, index);
  return index;
}

bool VulkanRenderer::createDescriptorsAndFrameData() {
  if (!mBindless.init(*mCtx))
    return false;

  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  binding.descriptorCount = 1;
  // Also read from the task/mesh stages for terrain.
  binding.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
      VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;

  VkDescriptorSetLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutCi.bindingCount = 1;
  layoutCi.pBindings = &binding;
  VK_CHECK(vkCreateDescriptorSetLayout(mCtx->device(), &layoutCi, nullptr,
                                       &mFrameSetLayout));

  VkDescriptorPoolSize poolSize{};
  poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  poolSize.descriptorCount = kFramesInFlight;
  VkDescriptorPoolCreateInfo poolCi{};
  poolCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolCi.maxSets = kFramesInFlight;
  poolCi.poolSizeCount = 1;
  poolCi.pPoolSizes = &poolSize;
  VK_CHECK(
      vkCreateDescriptorPool(mCtx->device(), &poolCi, nullptr, &mFrameDescPool));

  mFrameSets.resize(kFramesInFlight);
  mFrameUBOs.resize(kFramesInFlight);
  mFrameUBOAllocs.resize(kFramesInFlight);
  mFrameUBOMapped.resize(kFramesInFlight);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkBufferCreateInfo bufferCi{};
    bufferCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferCi.size = sizeof(FrameDataGpu);
    bufferCi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufferCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocCi{};
    allocCi.usage = VMA_MEMORY_USAGE_AUTO;
    allocCi.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(mCtx->allocator(), &bufferCi, &allocCi,
                             &mFrameUBOs[i], &mFrameUBOAllocs[i], &info));
    mFrameUBOMapped[i] = info.pMappedData;

    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = mFrameDescPool;
    setAlloc.descriptorSetCount = 1;
    setAlloc.pSetLayouts = &mFrameSetLayout;
    VK_CHECK(
        vkAllocateDescriptorSets(mCtx->device(), &setAlloc, &mFrameSets[i]));

    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = mFrameUBOs[i];
    bufInfo.range = sizeof(FrameDataGpu);
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = mFrameSets[i];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &bufInfo;
    vkUpdateDescriptorSets(mCtx->device(), 1, &write, 0, nullptr);
  }
  return true;
}

bool VulkanRenderer::createSceneTargets() {
  const VkExtent2D extent = mSwapchain.extent();
  mHdrImages.resize(kFramesInFlight);
  mHdrAllocs.resize(kFramesInFlight);
  mHdrViews.resize(kFramesInFlight);
  mDepthImages.resize(kFramesInFlight);
  mDepthAllocs.resize(kFramesInFlight);
  mDepthViews.resize(kFramesInFlight);

  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkImageCreateInfo hdrCi{};
    hdrCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    hdrCi.imageType = VK_IMAGE_TYPE_2D;
    hdrCi.format = mHdrFormat;
    hdrCi.extent = {extent.width, extent.height, 1};
    hdrCi.mipLevels = 1;
    hdrCi.arrayLayers = 1;
    hdrCi.samples = VK_SAMPLE_COUNT_1_BIT;
    hdrCi.tiling = VK_IMAGE_TILING_OPTIMAL;
    hdrCi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT;
    hdrCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo hdrAlloc{};
    hdrAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(mCtx->allocator(), &hdrCi, &hdrAlloc,
                            &mHdrImages[i], &mHdrAllocs[i], nullptr));

    VkImageViewCreateInfo hdrView{};
    hdrView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    hdrView.image = mHdrImages[i];
    hdrView.viewType = VK_IMAGE_VIEW_TYPE_2D;
    hdrView.format = mHdrFormat;
    hdrView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    hdrView.subresourceRange.levelCount = 1;
    hdrView.subresourceRange.layerCount = 1;
    VK_CHECK(
        vkCreateImageView(mCtx->device(), &hdrView, nullptr, &mHdrViews[i]));

    VkImageCreateInfo depthCi{};
    depthCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthCi.imageType = VK_IMAGE_TYPE_2D;
    depthCi.format = mDepthFormat;
    depthCi.extent = {extent.width, extent.height, 1};
    depthCi.mipLevels = 1;
    depthCi.arrayLayers = 1;
    depthCi.samples = VK_SAMPLE_COUNT_1_BIT;
    depthCi.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthCi.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo depthAlloc{};
    depthAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(mCtx->allocator(), &depthCi, &depthAlloc,
                            &mDepthImages[i], &mDepthAllocs[i], nullptr));

    VkImageViewCreateInfo depthView{};
    depthView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthView.image = mDepthImages[i];
    depthView.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthView.format = mDepthFormat;
    depthView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthView.subresourceRange.levelCount = 1;
    depthView.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(mCtx->device(), &depthView, nullptr,
                               &mDepthViews[i]));

    immediateSubmit([&](VkCommandBuffer cmd) {
      imageBarrier(cmd, mDepthImages[i], VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                   VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_ASPECT_DEPTH_BIT);
    });
  }
  return true;
}

void VulkanRenderer::destroySceneTargets() {
  for (VkImageView v : mHdrViews)
    vkDestroyImageView(mCtx->device(), v, nullptr);
  for (uint32_t i = 0; i < mHdrImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mHdrImages[i], mHdrAllocs[i]);
  mHdrViews.clear();
  mHdrImages.clear();
  mHdrAllocs.clear();

  for (VkImageView v : mDepthViews)
    vkDestroyImageView(mCtx->device(), v, nullptr);
  for (uint32_t i = 0; i < mDepthImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mDepthImages[i], mDepthAllocs[i]);
  mDepthViews.clear();
  mDepthImages.clear();
  mDepthAllocs.clear();
}

bool VulkanRenderer::createShadowResources() {
  // Comparison sampler for hardware PCF.
  VkSamplerCreateInfo samplerCi{};
  samplerCi.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerCi.magFilter = VK_FILTER_LINEAR;
  samplerCi.minFilter = VK_FILTER_LINEAR;
  samplerCi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerCi.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerCi.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerCi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerCi.compareEnable = VK_TRUE;
  samplerCi.compareOp = VK_COMPARE_OP_LESS;
  samplerCi.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  VK_CHECK(
      vkCreateSampler(mCtx->device(), &samplerCi, nullptr, &mShadowSampler));

  // Descriptor set layout (set 2 in the scene pipeline): sampler2DArrayShadow.
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutCi.bindingCount = 1;
  layoutCi.pBindings = &binding;
  VK_CHECK(vkCreateDescriptorSetLayout(mCtx->device(), &layoutCi, nullptr,
                                       &mShadowSetLayout));

  VkDescriptorPoolSize poolSize{};
  poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  poolSize.descriptorCount = kFramesInFlight;
  VkDescriptorPoolCreateInfo poolCi{};
  poolCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolCi.maxSets = kFramesInFlight;
  poolCi.poolSizeCount = 1;
  poolCi.pPoolSizes = &poolSize;
  VK_CHECK(
      vkCreateDescriptorPool(mCtx->device(), &poolCi, nullptr, &mShadowPool));

  mShadowImages.resize(kFramesInFlight);
  mShadowAllocs.resize(kFramesInFlight);
  mShadowArrayViews.resize(kFramesInFlight);
  mShadowLayerViews.resize(kFramesInFlight);
  mShadowSets.resize(kFramesInFlight);

  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkImageCreateInfo imageCi{};
    imageCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCi.imageType = VK_IMAGE_TYPE_2D;
    imageCi.format = mDepthFormat;
    imageCi.extent = {kShadowRes, kShadowRes, 1};
    imageCi.mipLevels = 1;
    imageCi.arrayLayers = kShadowCascades;
    imageCi.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCi.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCi.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT;
    imageCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(mCtx->allocator(), &imageCi, &alloc,
                            &mShadowImages[i], &mShadowAllocs[i], nullptr));

    // Array view for sampling.
    VkImageViewCreateInfo arrayView{};
    arrayView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    arrayView.image = mShadowImages[i];
    arrayView.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    arrayView.format = mDepthFormat;
    arrayView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    arrayView.subresourceRange.levelCount = 1;
    arrayView.subresourceRange.layerCount = kShadowCascades;
    VK_CHECK(vkCreateImageView(mCtx->device(), &arrayView, nullptr,
                               &mShadowArrayViews[i]));

    // One single-layer view per cascade for rendering.
    for (uint32_t c = 0; c < kShadowCascades; ++c) {
      VkImageViewCreateInfo layerView{};
      layerView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      layerView.image = mShadowImages[i];
      layerView.viewType = VK_IMAGE_VIEW_TYPE_2D;
      layerView.format = mDepthFormat;
      layerView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
      layerView.subresourceRange.levelCount = 1;
      layerView.subresourceRange.baseArrayLayer = c;
      layerView.subresourceRange.layerCount = 1;
      VK_CHECK(vkCreateImageView(mCtx->device(), &layerView, nullptr,
                                 &mShadowLayerViews[i][c]));
    }

    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = mShadowPool;
    setAlloc.descriptorSetCount = 1;
    setAlloc.pSetLayouts = &mShadowSetLayout;
    VK_CHECK(
        vkAllocateDescriptorSets(mCtx->device(), &setAlloc, &mShadowSets[i]));

    VkDescriptorImageInfo image{};
    image.sampler = mShadowSampler;
    image.imageView = mShadowArrayViews[i];
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = mShadowSets[i];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(mCtx->device(), 1, &write, 0, nullptr);
  }
  return true;
}

void VulkanRenderer::destroyShadowResources() {
  for (uint32_t i = 0; i < mShadowImages.size(); ++i) {
    for (uint32_t c = 0; c < kShadowCascades; ++c)
      vkDestroyImageView(mCtx->device(), mShadowLayerViews[i][c], nullptr);
    vkDestroyImageView(mCtx->device(), mShadowArrayViews[i], nullptr);
    vmaDestroyImage(mCtx->allocator(), mShadowImages[i], mShadowAllocs[i]);
  }
  mShadowImages.clear();
  mShadowAllocs.clear();
  mShadowArrayViews.clear();
  mShadowLayerViews.clear();
  if (mShadowPool)
    vkDestroyDescriptorPool(mCtx->device(), mShadowPool, nullptr);
  if (mShadowSetLayout)
    vkDestroyDescriptorSetLayout(mCtx->device(), mShadowSetLayout, nullptr);
  if (mShadowSampler)
    vkDestroySampler(mCtx->device(), mShadowSampler, nullptr);
  mShadowPool = VK_NULL_HANDLE;
  mShadowSetLayout = VK_NULL_HANDLE;
  mShadowSampler = VK_NULL_HANDLE;
}

bool VulkanRenderer::createTonemapResources() {
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutCi.bindingCount = 1;
  layoutCi.pBindings = &binding;
  VK_CHECK(vkCreateDescriptorSetLayout(mCtx->device(), &layoutCi, nullptr,
                                       &mTonemapSetLayout));

  VkDescriptorPoolSize poolSize{};
  poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  poolSize.descriptorCount = kFramesInFlight;
  VkDescriptorPoolCreateInfo poolCi{};
  poolCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolCi.maxSets = kFramesInFlight;
  poolCi.poolSizeCount = 1;
  poolCi.pPoolSizes = &poolSize;
  VK_CHECK(
      vkCreateDescriptorPool(mCtx->device(), &poolCi, nullptr, &mTonemapPool));

  mTonemapSets.resize(kFramesInFlight);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = mTonemapPool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &mTonemapSetLayout;
    VK_CHECK(vkAllocateDescriptorSets(mCtx->device(), &alloc, &mTonemapSets[i]));
  }
  return true;
}

void VulkanRenderer::updateTonemapSets() {
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkDescriptorImageInfo image{};
    image.sampler = mSampler;
    image.imageView = mHdrViews[i];
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = mTonemapSets[i];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(mCtx->device(), 1, &write, 0, nullptr);
  }
}

VkShaderModule VulkanRenderer::loadShaderModule(const std::string &path) {
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  if (!file.is_open()) {
    std::fprintf(stderr, "[VulkanRHI] Cannot open SPIR-V: %s\n", path.c_str());
    std::abort();
  }
  const size_t size = static_cast<size_t>(file.tellg());
  std::vector<char> code(size);
  file.seekg(0);
  file.read(code.data(), static_cast<std::streamsize>(size));

  VkShaderModuleCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize = size;
  ci.pCode = reinterpret_cast<const uint32_t *>(code.data());
  VkShaderModule module = VK_NULL_HANDLE;
  VK_CHECK(vkCreateShaderModule(mCtx->device(), &ci, nullptr, &module));
  return module;
}

bool VulkanRenderer::createShadowPipeline(const std::string &shaderDir) {
  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  pcRange.offset = 0;
  pcRange.size = sizeof(glm::mat4);

  VkPipelineLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutCi.pushConstantRangeCount = 1;
  layoutCi.pPushConstantRanges = &pcRange;
  VK_CHECK(vkCreatePipelineLayout(mCtx->device(), &layoutCi, nullptr,
                                  &mShadowPipelineLayout));

  VkShaderModule vert = loadShaderModule(shaderDir + "/shadow.vert.spv");
  VkPipelineShaderStageCreateInfo stage{};
  stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
  stage.module = vert;
  stage.pName = "main";

  // Only position is needed; the binding still spans the full vertex stride.
  VkVertexInputBindingDescription binding{};
  binding.binding = 0;
  binding.stride = sizeof(MeshVertex);
  binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription attr{};
  attr = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, pos)};
  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 1;
  vertexInput.pVertexAttributeDescriptions = &attr;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  // Depth bias fights shadow acne.
  raster.depthBiasEnable = VK_TRUE;
  raster.depthBiasConstantFactor = 1.25f;
  raster.depthBiasSlopeFactor = 1.75f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_TRUE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

  VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT,
                               VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamics;

  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.depthAttachmentFormat = mDepthFormat;

  VkGraphicsPipelineCreateInfo pipelineCi{};
  pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCi.pNext = &renderingCi;
  pipelineCi.stageCount = 1;
  pipelineCi.pStages = &stage;
  pipelineCi.pVertexInputState = &vertexInput;
  pipelineCi.pInputAssemblyState = &inputAssembly;
  pipelineCi.pViewportState = &viewport;
  pipelineCi.pRasterizationState = &raster;
  pipelineCi.pMultisampleState = &multisample;
  pipelineCi.pDepthStencilState = &depthStencil;
  pipelineCi.pDynamicState = &dynamicState;
  pipelineCi.layout = mShadowPipelineLayout;

  uint64_t key = fnv1a64Str("shadow.depth");
  key = fnv1a64(&mDepthFormat, sizeof(mDepthFormat), key);
  mShadowPipeline = mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
    VkPipeline p = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                       nullptr, &p));
    return p;
  });

  vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  return true;
}

bool VulkanRenderer::createScenePipeline(const std::string &shaderDir) {
  VkDescriptorSetLayout setLayouts[3] = {mBindless.layout(), mFrameSetLayout,
                                         mShadowSetLayout};
  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  pcRange.offset = 0;
  pcRange.size = sizeof(uint32_t);

  VkPipelineLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutCi.setLayoutCount = 3;
  layoutCi.pSetLayouts = setLayouts;
  layoutCi.pushConstantRangeCount = 1;
  layoutCi.pPushConstantRanges = &pcRange;
  VK_CHECK(vkCreatePipelineLayout(mCtx->device(), &layoutCi, nullptr,
                                  &mScenePipelineLayout));

  VkShaderModule vert = loadShaderModule(shaderDir + "/mesh.vert.spv");
  VkShaderModule frag = loadShaderModule(shaderDir + "/mesh.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkVertexInputBindingDescription binding{};
  binding.binding = 0;
  binding.stride = sizeof(MeshVertex);
  binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription attrs[3]{};
  attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, pos)};
  attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal)};
  attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv)};
  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 3;
  vertexInput.pVertexAttributeDescriptions = attrs;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_TRUE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments = &blendAttachment;

  VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT,
                               VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamics;

  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.colorAttachmentCount = 1;
  renderingCi.pColorAttachmentFormats = &mHdrFormat;
  renderingCi.depthAttachmentFormat = mDepthFormat;

  VkGraphicsPipelineCreateInfo pipelineCi{};
  pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCi.pNext = &renderingCi;
  pipelineCi.stageCount = 2;
  pipelineCi.pStages = stages;
  pipelineCi.pVertexInputState = &vertexInput;
  pipelineCi.pInputAssemblyState = &inputAssembly;
  pipelineCi.pViewportState = &viewport;
  pipelineCi.pRasterizationState = &raster;
  pipelineCi.pMultisampleState = &multisample;
  pipelineCi.pDepthStencilState = &depthStencil;
  pipelineCi.pColorBlendState = &colorBlend;
  pipelineCi.pDynamicState = &dynamicState;
  pipelineCi.layout = mScenePipelineLayout;

  uint64_t key = fnv1a64Str("mesh.shadowed");
  key = fnv1a64(&mHdrFormat, sizeof(mHdrFormat), key);
  key = fnv1a64(&mDepthFormat, sizeof(mDepthFormat), key);
  mScenePipeline = mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
    VkPipeline p = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                       nullptr, &p));
    return p;
  });

  vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  vkDestroyShaderModule(mCtx->device(), frag, nullptr);
  return true;
}

bool VulkanRenderer::createTerrainPipeline(const std::string &shaderDir) {
  // Reuses the scene pipeline layout (same 3 sets + fragment push constant).
  VkShaderModule task = loadShaderModule(shaderDir + "/terrain.task.spv");
  VkShaderModule mesh = loadShaderModule(shaderDir + "/terrain.mesh.spv");
  VkShaderModule frag = loadShaderModule(shaderDir + "/terrain.frag.spv");

  VkPipelineShaderStageCreateInfo stages[3]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_TASK_BIT_EXT;
  stages[0].module = task;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
  stages[1].module = mesh;
  stages[1].pName = "main";
  stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[2].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[2].module = frag;
  stages[2].pName = "main";

  // Mesh-shader pipelines have no vertex input / input assembly state.
  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_TRUE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments = &blendAttachment;

  VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT,
                               VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamics;

  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.colorAttachmentCount = 1;
  renderingCi.pColorAttachmentFormats = &mHdrFormat;
  renderingCi.depthAttachmentFormat = mDepthFormat;

  VkGraphicsPipelineCreateInfo pipelineCi{};
  pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCi.pNext = &renderingCi;
  pipelineCi.stageCount = 3;
  pipelineCi.pStages = stages;
  pipelineCi.pVertexInputState = nullptr;   // mesh shader: no vertex input
  pipelineCi.pInputAssemblyState = nullptr; // mesh shader: no input assembly
  pipelineCi.pViewportState = &viewport;
  pipelineCi.pRasterizationState = &raster;
  pipelineCi.pMultisampleState = &multisample;
  pipelineCi.pDepthStencilState = &depthStencil;
  pipelineCi.pColorBlendState = &colorBlend;
  pipelineCi.pDynamicState = &dynamicState;
  pipelineCi.layout = mScenePipelineLayout;

  uint64_t key = fnv1a64Str("terrain.mesh");
  key = fnv1a64(&mHdrFormat, sizeof(mHdrFormat), key);
  key = fnv1a64(&mDepthFormat, sizeof(mDepthFormat), key);
  mTerrainPipeline = mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
    VkPipeline p = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                       nullptr, &p));
    return p;
  });

  vkDestroyShaderModule(mCtx->device(), task, nullptr);
  vkDestroyShaderModule(mCtx->device(), mesh, nullptr);
  vkDestroyShaderModule(mCtx->device(), frag, nullptr);
  return true;
}

bool VulkanRenderer::createTonemapPipeline(const std::string &shaderDir) {
  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  pcRange.offset = 0;
  pcRange.size = sizeof(float);

  VkPipelineLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutCi.setLayoutCount = 1;
  layoutCi.pSetLayouts = &mTonemapSetLayout;
  layoutCi.pushConstantRangeCount = 1;
  layoutCi.pPushConstantRanges = &pcRange;
  VK_CHECK(vkCreatePipelineLayout(mCtx->device(), &layoutCi, nullptr,
                                  &mTonemapPipelineLayout));

  VkShaderModule vert = loadShaderModule(shaderDir + "/tonemap.vert.spv");
  VkShaderModule frag = loadShaderModule(shaderDir + "/tonemap.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments = &blendAttachment;

  VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT,
                               VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamics;

  const VkFormat swapFormat = mSwapchain.imageFormat();
  VkPipelineRenderingCreateInfo renderingCi{};
  renderingCi.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  renderingCi.colorAttachmentCount = 1;
  renderingCi.pColorAttachmentFormats = &swapFormat;

  VkGraphicsPipelineCreateInfo pipelineCi{};
  pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCi.pNext = &renderingCi;
  pipelineCi.stageCount = 2;
  pipelineCi.pStages = stages;
  pipelineCi.pVertexInputState = &vertexInput;
  pipelineCi.pInputAssemblyState = &inputAssembly;
  pipelineCi.pViewportState = &viewport;
  pipelineCi.pRasterizationState = &raster;
  pipelineCi.pMultisampleState = &multisample;
  pipelineCi.pDepthStencilState = &depthStencil;
  pipelineCi.pColorBlendState = &colorBlend;
  pipelineCi.pDynamicState = &dynamicState;
  pipelineCi.layout = mTonemapPipelineLayout;

  uint64_t key = fnv1a64Str("tonemap");
  key = fnv1a64(&swapFormat, sizeof(swapFormat), key);
  mTonemapPipeline = mPipelineCache.getOrCreate(key, [&](VkPipelineCache pc) {
    VkPipeline p = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(mCtx->device(), pc, 1, &pipelineCi,
                                       nullptr, &p));
    return p;
  });

  vkDestroyShaderModule(mCtx->device(), vert, nullptr);
  vkDestroyShaderModule(mCtx->device(), frag, nullptr);
  return true;
}

bool VulkanRenderer::loadModel(const std::string &modelPath) {
  MeshData mesh;
  if (!loadObj(modelPath, mesh))
    return false;

  // The ground is now mesh-shader terrain (see createTerrainPipeline); the
  // model still casts its shadow, which the terrain receives.
  mTotalIndexCount = static_cast<uint32_t>(mesh.indices.size());
  mTerrainTexIndex = mDefaultTexIndex;

  createDeviceLocalBuffer(mesh.vertices.data(),
                          mesh.vertices.size() * sizeof(MeshVertex),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, mVertexBuffer,
                          mVertexAlloc);
  createDeviceLocalBuffer(mesh.indices.data(),
                          mesh.indices.size() * sizeof(uint32_t),
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT, mIndexBuffer,
                          mIndexAlloc);

  std::vector<uint32_t> materialTexIndex(mesh.materials.size(),
                                         mDefaultTexIndex);
  for (size_t i = 0; i < mesh.materials.size(); ++i) {
    if (!mesh.materials[i].diffuseTexturePath.empty())
      materialTexIndex[i] = loadTextureFile(mesh.materials[i].diffuseTexturePath);
  }

  for (const SubMesh &sub : mesh.submeshes) {
    DrawItem item{};
    item.indexOffset = sub.indexOffset;
    item.indexCount = sub.indexCount;
    item.textureIndex =
        (sub.materialId >= 0 &&
         sub.materialId < static_cast<int>(materialTexIndex.size()))
            ? materialTexIndex[sub.materialId]
            : mDefaultTexIndex;
    mDrawItems.push_back(item);
  }
  std::fprintf(stderr, "[VulkanRHI] %zu draw item(s); %u total indices\n",
               mDrawItems.size(), mTotalIndexCount);
  return true;
}

bool VulkanRenderer::createSyncObjects() {
  mCommandBuffers.resize(kFramesInFlight);
  VkCommandBufferAllocateInfo allocCi{};
  allocCi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocCi.commandPool = mCommandPool;
  allocCi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocCi.commandBufferCount = kFramesInFlight;
  VK_CHECK(vkAllocateCommandBuffers(mCtx->device(), &allocCi,
                                    mCommandBuffers.data()));

  mImageAvailable.resize(kFramesInFlight);
  mInFlight.resize(kFramesInFlight);
  VkSemaphoreCreateInfo semCi{};
  semCi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  VkFenceCreateInfo fenceCi{};
  fenceCi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceCi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VK_CHECK(vkCreateSemaphore(mCtx->device(), &semCi, nullptr,
                               &mImageAvailable[i]));
    VK_CHECK(vkCreateFence(mCtx->device(), &fenceCi, nullptr, &mInFlight[i]));
  }
  mRenderFinished.resize(mSwapchain.imageCount());
  for (uint32_t i = 0; i < mSwapchain.imageCount(); ++i)
    VK_CHECK(vkCreateSemaphore(mCtx->device(), &semCi, nullptr,
                               &mRenderFinished[i]));
  return true;
}

void VulkanRenderer::recreateSwapchain() {
  uint32_t w = 0, h = 0;
  mQueryFbSize(w, h);
  if (w == 0 || h == 0)
    return;

  vkDeviceWaitIdle(mCtx->device());

  for (VkSemaphore s : mRenderFinished)
    vkDestroySemaphore(mCtx->device(), s, nullptr);
  mRenderFinished.clear();
  destroySceneTargets();

  mSwapchain.destroy(*mCtx);
  if (!mSwapchain.create(*mCtx, mSurface, w, h))
    return;
  createSceneTargets();
  updateTonemapSets();

  mRenderFinished.resize(mSwapchain.imageCount());
  VkSemaphoreCreateInfo semCi{};
  semCi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  for (uint32_t i = 0; i < mSwapchain.imageCount(); ++i)
    VK_CHECK(vkCreateSemaphore(mCtx->device(), &semCi, nullptr,
                               &mRenderFinished[i]));
}

void VulkanRenderer::computeCascades(const glm::mat4 &view, float fovY,
                                     float aspect, const glm::vec3 &lightDir,
                                     glm::mat4 outLightSpace[kShadowCascades],
                                     glm::vec4 &outSplits) {
  const float nearPlane = 0.1f;
  const float farPlane = 8.0f;
  const float lambda = 0.5f;

  float cascadeFar[kShadowCascades];
  for (uint32_t i = 0; i < kShadowCascades; ++i) {
    const float p = float(i + 1) / float(kShadowCascades);
    const float logSplit = nearPlane * std::pow(farPlane / nearPlane, p);
    const float uniSplit = nearPlane + (farPlane - nearPlane) * p;
    cascadeFar[i] = glm::mix(uniSplit, logSplit, lambda);
  }
  outSplits = glm::vec4(cascadeFar[0], cascadeFar[1], cascadeFar[2], farPlane);

  float sliceNear = nearPlane;
  for (uint32_t i = 0; i < kShadowCascades; ++i) {
    const float sliceFar = cascadeFar[i];
    glm::mat4 sliceProj = glm::perspective(fovY, aspect, sliceNear, sliceFar);
    sliceProj[1][1] *= -1.0f; // match the render projection
    const glm::mat4 invVP = glm::inverse(sliceProj * view);

    glm::vec3 corners[8];
    int idx = 0;
    for (int x = 0; x < 2; ++x)
      for (int y = 0; y < 2; ++y)
        for (int z = 0; z < 2; ++z) {
          glm::vec4 pt = invVP * glm::vec4(x ? 1.0f : -1.0f, y ? 1.0f : -1.0f,
                                           z ? 1.0f : 0.0f, 1.0f);
          corners[idx++] = glm::vec3(pt) / pt.w;
        }

    glm::vec3 center(0.0f);
    for (const glm::vec3 &c : corners)
      center += c;
    center /= 8.0f;
    float radius = 0.0f;
    for (const glm::vec3 &c : corners)
      radius = glm::max(radius, glm::length(c - center));
    radius = std::ceil(radius * 16.0f) / 16.0f;

    const glm::vec3 ld = glm::normalize(lightDir);
    glm::vec3 up = std::abs(ld.y) > 0.95f ? glm::vec3(1, 0, 0)
                                          : glm::vec3(0, 1, 0);
    const glm::vec3 eye = center - ld * (radius + 2.0f);
    const glm::mat4 lightView = glm::lookAt(eye, center, up);
    const glm::mat4 lightProj =
        glm::ortho(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + 4.0f);
    outLightSpace[i] = lightProj * lightView;
    sliceNear = sliceFar;
  }
}

void VulkanRenderer::drawFrame() {
  VkDevice device = mCtx->device();
  VK_CHECK(vkWaitForFences(device, 1, &mInFlight[mCurrentFrame], VK_TRUE,
                           UINT64_MAX));

  uint32_t imageIndex = 0;
  VkResult acquire = vkAcquireNextImageKHR(
      device, mSwapchain.handle(), UINT64_MAX, mImageAvailable[mCurrentFrame],
      VK_NULL_HANDLE, &imageIndex);
  if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
    recreateSwapchain();
    return;
  }
  if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
    std::fprintf(stderr, "[VulkanRHI] vkAcquireNextImageKHR failed: %s\n",
                 resultString(acquire));
    std::abort();
  }

  const float seconds =
      std::chrono::duration<float>(std::chrono::steady_clock::now() -
                                   mStartTime)
          .count();
  const VkExtent2D extent = mSwapchain.extent();
  const float aspect =
      static_cast<float>(extent.width) / static_cast<float>(extent.height);
  const float fovY = glm::radians(50.0f);

  // Camera + light from the (UI-driven) params.
  const glm::vec3 center(0.0f, -0.1f, 0.0f);
  const float yaw = glm::radians(mParams.autoOrbit
                                     ? seconds * 23.0f + mParams.camYawDeg
                                     : mParams.camYawDeg);
  const float pitch = glm::radians(mParams.camPitchDeg);
  const glm::vec3 eye =
      center + mParams.camDistance *
                   glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                             std::cos(pitch) * std::cos(yaw));
  const glm::mat4 viewMat =
      glm::lookAt(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
  glm::mat4 proj = glm::perspective(fovY, aspect, 0.1f, 100.0f);
  proj[1][1] *= -1.0f;

  const float ly = glm::radians(mParams.lightYawDeg);
  const float lp = glm::radians(mParams.lightPitchDeg);
  const glm::vec3 lightDir = glm::normalize(glm::vec3(
      std::cos(lp) * std::sin(ly), -std::sin(lp), std::cos(lp) * std::cos(ly)));
  glm::mat4 lightSpace[kShadowCascades];
  glm::vec4 splits;
  computeCascades(viewMat, fovY, aspect, lightDir, lightSpace, splits);

  FrameDataGpu frameData{};
  frameData.viewProj = proj * viewMat;
  frameData.view = viewMat;
  for (uint32_t i = 0; i < kShadowCascades; ++i)
    frameData.lightSpace[i] = lightSpace[i];
  frameData.lightDir = glm::vec4(lightDir, 0.0f);
  frameData.cascadeSplits = splits;
  std::memcpy(mFrameUBOMapped[mCurrentFrame], &frameData, sizeof(frameData));

  VK_CHECK(vkResetFences(device, 1, &mInFlight[mCurrentFrame]));

  VkCommandBuffer cmd = mCommandBuffers[mCurrentFrame];
  VK_CHECK(vkResetCommandBuffer(cmd, 0));
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

  VkDeviceSize vbOffset = 0;

  // Optional one-shot framebuffer capture (debug / headless verification).
  const bool doCapture = mCapture;
  VkBuffer captureBuf = VK_NULL_HANDLE;
  VmaAllocation captureAlloc = VK_NULL_HANDLE;
  void *captureMapped = nullptr;
  if (doCapture) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(mCtx->allocator(), &bci, &aci, &captureBuf,
                             &captureAlloc, &info));
    captureMapped = info.pMappedData;
  }

  // ---- Pass 0: shadow cascades ----
  imageBarrier(cmd, mShadowImages[mCurrentFrame],
               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
               VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
               VK_IMAGE_ASPECT_DEPTH_BIT, kShadowCascades);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mShadowPipeline);
  vkCmdBindVertexBuffers(cmd, 0, 1, &mVertexBuffer, &vbOffset);
  vkCmdBindIndexBuffer(cmd, mIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

  VkViewport shadowVp{};
  shadowVp.width = static_cast<float>(kShadowRes);
  shadowVp.height = static_cast<float>(kShadowRes);
  shadowVp.minDepth = 0.0f;
  shadowVp.maxDepth = 1.0f;
  VkRect2D shadowScissor{};
  shadowScissor.extent = {kShadowRes, kShadowRes};

  for (uint32_t c = 0; c < kShadowCascades; ++c) {
    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = mShadowLayerViews[mCurrentFrame][c];
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo shadowRender{};
    shadowRender.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    shadowRender.renderArea.extent = {kShadowRes, kShadowRes};
    shadowRender.layerCount = 1;
    shadowRender.pDepthAttachment = &depthAttachment;
    vkCmdBeginRendering(cmd, &shadowRender);

    vkCmdSetViewport(cmd, 0, 1, &shadowVp);
    vkCmdSetScissor(cmd, 0, 1, &shadowScissor);
    vkCmdPushConstants(cmd, mShadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(glm::mat4), &lightSpace[c]);
    vkCmdDrawIndexed(cmd, mTotalIndexCount, 1, 0, 0, 0);
    vkCmdEndRendering(cmd);
  }

  imageBarrier(cmd, mShadowImages[mCurrentFrame],
               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_IMAGE_ASPECT_DEPTH_BIT, kShadowCascades);

  VkViewport vp{};
  vp.width = static_cast<float>(extent.width);
  vp.height = static_cast<float>(extent.height);
  vp.minDepth = 0.0f;
  vp.maxDepth = 1.0f;
  VkRect2D scissor{};
  scissor.extent = extent;

  // ---- Pass 1: scene -> HDR ----
  imageBarrier(cmd, mHdrImages[mCurrentFrame],
               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  VkRenderingAttachmentInfo sceneColor{};
  sceneColor.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  sceneColor.imageView = mHdrViews[mCurrentFrame];
  sceneColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  sceneColor.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  sceneColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  sceneColor.clearValue.color = {{0.03f, 0.04f, 0.07f, 1.0f}};

  VkRenderingAttachmentInfo sceneDepth{};
  sceneDepth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  sceneDepth.imageView = mDepthViews[mCurrentFrame];
  sceneDepth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  sceneDepth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  sceneDepth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  sceneDepth.clearValue.depthStencil = {1.0f, 0};

  VkRenderingInfo sceneRender{};
  sceneRender.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  sceneRender.renderArea.extent = extent;
  sceneRender.layerCount = 1;
  sceneRender.colorAttachmentCount = 1;
  sceneRender.pColorAttachments = &sceneColor;
  sceneRender.pDepthAttachment = &sceneDepth;
  vkCmdBeginRendering(cmd, &sceneRender);

  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mScenePipeline);
  VkDescriptorSet sets[3] = {mBindless.set(), mFrameSets[mCurrentFrame],
                             mShadowSets[mCurrentFrame]};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mScenePipelineLayout, 0, 3, sets, 0, nullptr);
  vkCmdBindVertexBuffers(cmd, 0, 1, &mVertexBuffer, &vbOffset);
  vkCmdBindIndexBuffer(cmd, mIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
  for (const DrawItem &item : mDrawItems) {
    vkCmdPushConstants(cmd, mScenePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(uint32_t), &item.textureIndex);
    vkCmdDrawIndexed(cmd, item.indexCount, 1, item.indexOffset, 0, 0);
  }

  // Terrain via mesh shaders (same layout/sets; task shader frustum-culls
  // patches, mesh shader generates the displaced grid on the GPU).
  if (mDrawMeshTasks && mParams.drawTerrain) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mTerrainPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            mScenePipelineLayout, 0, 3, sets, 0, nullptr);
    vkCmdPushConstants(cmd, mScenePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(uint32_t), &mTerrainTexIndex);
    mDrawMeshTasks(cmd, kTerrainPatches * kTerrainPatches, 1, 1);
  }
  vkCmdEndRendering(cmd);

  imageBarrier(cmd, mHdrImages[mCurrentFrame],
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  // ---- Pass 2: tonemap -> swapchain ----
  imageBarrier(cmd, mSwapchain.image(imageIndex),
               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  VkRenderingAttachmentInfo tonemapColor{};
  tonemapColor.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  tonemapColor.imageView = mSwapchain.imageView(imageIndex);
  tonemapColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  tonemapColor.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  tonemapColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  VkRenderingInfo tonemapRender{};
  tonemapRender.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  tonemapRender.renderArea.extent = extent;
  tonemapRender.layerCount = 1;
  tonemapRender.colorAttachmentCount = 1;
  tonemapRender.pColorAttachments = &tonemapColor;
  vkCmdBeginRendering(cmd, &tonemapRender);

  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mTonemapPipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mTonemapPipelineLayout, 0, 1,
                          &mTonemapSets[mCurrentFrame], 0, nullptr);
  const float exposure = mParams.exposure;
  vkCmdPushConstants(cmd, mTonemapPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(float), &exposure);
  vkCmdDraw(cmd, 3, 1, 0, 0);

  // UI / overlay (e.g. ImGui) draws on top of the tonemapped image.
  if (mOverlay)
    mOverlay(cmd);

  vkCmdEndRendering(cmd);

  if (doCapture) {
    imageBarrier(cmd, mSwapchain.image(imageIndex),
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, mSwapchain.image(imageIndex),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuf, 1,
                           &region);
    imageBarrier(cmd, mSwapchain.image(imageIndex),
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  } else {
    imageBarrier(cmd, mSwapchain.image(imageIndex),
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  }
  VK_CHECK(vkEndCommandBuffer(cmd));

  VkSemaphoreSubmitInfo waitSem{};
  waitSem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  waitSem.semaphore = mImageAvailable[mCurrentFrame];
  waitSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSemaphoreSubmitInfo signalSem{};
  signalSem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  signalSem.semaphore = mRenderFinished[imageIndex];
  signalSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkCommandBufferSubmitInfo cmdInfo{};
  cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  cmdInfo.commandBuffer = cmd;
  VkSubmitInfo2 submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submit.waitSemaphoreInfoCount = 1;
  submit.pWaitSemaphoreInfos = &waitSem;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &cmdInfo;
  submit.signalSemaphoreInfoCount = 1;
  submit.pSignalSemaphoreInfos = &signalSem;
  VK_CHECK(vkQueueSubmit2(mCtx->graphicsQueue(), 1, &submit,
                          mInFlight[mCurrentFrame]));

  VkPresentInfoKHR present{};
  present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &mRenderFinished[imageIndex];
  VkSwapchainKHR sc = mSwapchain.handle();
  present.swapchainCount = 1;
  present.pSwapchains = &sc;
  present.pImageIndices = &imageIndex;
  VkResult presentResult = vkQueuePresentKHR(mCtx->presentQueue(), &present);
  if (presentResult == VK_ERROR_OUT_OF_DATE_KHR ||
      presentResult == VK_SUBOPTIMAL_KHR) {
    recreateSwapchain();
  } else if (presentResult != VK_SUCCESS) {
    std::fprintf(stderr, "[VulkanRHI] vkQueuePresentKHR failed: %s\n",
                 resultString(presentResult));
    std::abort();
  }

  if (doCapture) {
    vkDeviceWaitIdle(device);
    vmaInvalidateAllocation(mCtx->allocator(), captureAlloc, 0, VK_WHOLE_SIZE);
    const uint32_t w = extent.width, h = extent.height;
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    const uint8_t *src = static_cast<const uint8_t *>(captureMapped);
    // Swapchain is B8G8R8A8 -> swizzle to RGBA for the PNG.
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
      rgba[i * 4 + 0] = src[i * 4 + 2];
      rgba[i * 4 + 1] = src[i * 4 + 1];
      rgba[i * 4 + 2] = src[i * 4 + 0];
      rgba[i * 4 + 3] = 255;
    }
    if (stbi_write_png(mCapturePath.c_str(), static_cast<int>(w),
                       static_cast<int>(h), 4, rgba.data(),
                       static_cast<int>(w) * 4))
      std::fprintf(stderr, "[VulkanRHI] Captured frame -> %s (%ux%u)\n",
                   mCapturePath.c_str(), w, h);
    else
      std::fprintf(stderr, "[VulkanRHI] Capture write failed: %s\n",
                   mCapturePath.c_str());
    vmaDestroyBuffer(mCtx->allocator(), captureBuf, captureAlloc);
    mCapture = false;
  }

  mCurrentFrame = (mCurrentFrame + 1) % kFramesInFlight;
}

void VulkanRenderer::requestCapture(const std::string &path) {
  mCapture = true;
  mCapturePath = path;
}

void VulkanRenderer::waitIdle() {
  if (mCtx && mCtx->device())
    vkDeviceWaitIdle(mCtx->device());
}

void VulkanRenderer::shutdown() {
  if (!mCtx || !mCtx->device())
    return;
  VkDevice device = mCtx->device();
  vkDeviceWaitIdle(device);

  for (VkSemaphore s : mRenderFinished)
    vkDestroySemaphore(device, s, nullptr);
  mRenderFinished.clear();
  for (VkSemaphore s : mImageAvailable)
    vkDestroySemaphore(device, s, nullptr);
  mImageAvailable.clear();
  for (VkFence f : mInFlight)
    vkDestroyFence(device, f, nullptr);
  mInFlight.clear();

  if (mIndexBuffer)
    vmaDestroyBuffer(mCtx->allocator(), mIndexBuffer, mIndexAlloc);
  if (mVertexBuffer)
    vmaDestroyBuffer(mCtx->allocator(), mVertexBuffer, mVertexAlloc);
  mIndexBuffer = VK_NULL_HANDLE;
  mVertexBuffer = VK_NULL_HANDLE;

  for (VkImageView v : mTextureViews)
    vkDestroyImageView(device, v, nullptr);
  for (size_t i = 0; i < mTextureImages.size(); ++i)
    vmaDestroyImage(mCtx->allocator(), mTextureImages[i], mTextureAllocs[i]);
  mTextureViews.clear();
  mTextureImages.clear();
  mTextureAllocs.clear();
  if (mSampler)
    vkDestroySampler(device, mSampler, nullptr);
  mSampler = VK_NULL_HANDLE;

  destroyShadowResources();
  destroySceneTargets();

  if (mTonemapPool)
    vkDestroyDescriptorPool(device, mTonemapPool, nullptr);
  if (mTonemapSetLayout)
    vkDestroyDescriptorSetLayout(device, mTonemapSetLayout, nullptr);
  mTonemapPool = VK_NULL_HANDLE;
  mTonemapSetLayout = VK_NULL_HANDLE;

  for (uint32_t i = 0; i < mFrameUBOs.size(); ++i)
    vmaDestroyBuffer(mCtx->allocator(), mFrameUBOs[i], mFrameUBOAllocs[i]);
  mFrameUBOs.clear();
  mFrameUBOAllocs.clear();
  mFrameUBOMapped.clear();
  if (mFrameDescPool)
    vkDestroyDescriptorPool(device, mFrameDescPool, nullptr);
  if (mFrameSetLayout)
    vkDestroyDescriptorSetLayout(device, mFrameSetLayout, nullptr);
  mFrameDescPool = VK_NULL_HANDLE;
  mFrameSetLayout = VK_NULL_HANDLE;

  mBindless.destroy(*mCtx);

  if (mCommandPool) {
    vkDestroyCommandPool(device, mCommandPool, nullptr);
    mCommandPool = VK_NULL_HANDLE;
  }
  if (mTonemapPipelineLayout)
    vkDestroyPipelineLayout(device, mTonemapPipelineLayout, nullptr);
  if (mScenePipelineLayout)
    vkDestroyPipelineLayout(device, mScenePipelineLayout, nullptr);
  if (mShadowPipelineLayout)
    vkDestroyPipelineLayout(device, mShadowPipelineLayout, nullptr);
  mTonemapPipelineLayout = VK_NULL_HANDLE;
  mScenePipelineLayout = VK_NULL_HANDLE;
  mShadowPipelineLayout = VK_NULL_HANDLE;

  mPipelineCache.destroy(*mCtx);
  mScenePipeline = VK_NULL_HANDLE;
  mTerrainPipeline = VK_NULL_HANDLE;
  mTonemapPipeline = VK_NULL_HANDLE;
  mShadowPipeline = VK_NULL_HANDLE;

  mSwapchain.destroy(*mCtx);
}

} // namespace vkrhi

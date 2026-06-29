#include "VulkanBindless.h"

namespace vkrhi {

bool VulkanBindless::init(VulkanContext &ctx) {
  // One binding: an array of combined image samplers sized to kMaxTextures.
  // partially-bound  -> slots may be left unwritten without validation errors.
  // update-after-bind -> descriptors may be updated even while the set is bound.
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = kTextureBinding;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = kMaxTextures;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  const VkDescriptorBindingFlags bindingFlags =
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

  VkDescriptorSetLayoutBindingFlagsCreateInfo flagsCi{};
  flagsCi.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
  flagsCi.bindingCount = 1;
  flagsCi.pBindingFlags = &bindingFlags;

  VkDescriptorSetLayoutCreateInfo layoutCi{};
  layoutCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutCi.pNext = &flagsCi;
  layoutCi.flags =
      VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  layoutCi.bindingCount = 1;
  layoutCi.pBindings = &binding;
  VK_CHECK(
      vkCreateDescriptorSetLayout(ctx.device(), &layoutCi, nullptr, &mLayout));

  VkDescriptorPoolSize poolSize{};
  poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  poolSize.descriptorCount = kMaxTextures;

  VkDescriptorPoolCreateInfo poolCi{};
  poolCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolCi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  poolCi.maxSets = 1;
  poolCi.poolSizeCount = 1;
  poolCi.pPoolSizes = &poolSize;
  VK_CHECK(vkCreateDescriptorPool(ctx.device(), &poolCi, nullptr, &mPool));

  VkDescriptorSetAllocateInfo allocCi{};
  allocCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocCi.descriptorPool = mPool;
  allocCi.descriptorSetCount = 1;
  allocCi.pSetLayouts = &mLayout;
  VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &allocCi, &mSet));
  return true;
}

uint32_t VulkanBindless::registerTexture(VulkanContext &ctx, VkImageView view,
                                         VkSampler sampler) {
  const uint32_t slot = mNextSlot++;

  VkDescriptorImageInfo image{};
  image.sampler = sampler;
  image.imageView = view;
  image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = mSet;
  write.dstBinding = kTextureBinding;
  write.dstArrayElement = slot;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &image;
  vkUpdateDescriptorSets(ctx.device(), 1, &write, 0, nullptr);
  return slot;
}

void VulkanBindless::destroy(VulkanContext &ctx) {
  if (mPool) {
    vkDestroyDescriptorPool(ctx.device(), mPool, nullptr);
    mPool = VK_NULL_HANDLE;
  }
  if (mLayout) {
    vkDestroyDescriptorSetLayout(ctx.device(), mLayout, nullptr);
    mLayout = VK_NULL_HANDLE;
  }
  mSet = VK_NULL_HANDLE;
  mNextSlot = 0;
}

} // namespace vkrhi

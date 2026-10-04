#include "VulkanExposureMeter.h"
#include <fstream>
#include <vector>
#include <cstring>

namespace vkrhi {
bool VulkanExposureMeter::init(VulkanContext& context,const std::string& directory) {
  mContext=&context;
  VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
  VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};layout.bindingCount=2;layout.pBindings=bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context.device(),&layout,nullptr,&mLayout));
  VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pipelineLayout.setLayoutCount=1;pipelineLayout.pSetLayouts=&mLayout;
  VK_CHECK(vkCreatePipelineLayout(context.device(),&pipelineLayout,nullptr,&mPipelineLayout));
  std::ifstream file(directory+"/exposureHistogram.comp.spv",std::ios::binary|std::ios::ate);if(!file)return false;
  std::vector<uint32_t> code(size_t(file.tellg())/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),code.size()*4);
  VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader.codeSize=code.size()*4;shader.pCode=code.data();VkShaderModule module;
  VK_CHECK(vkCreateShaderModule(context.device(),&shader,nullptr,&module));
  VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};compute.layout=mPipelineLayout;compute.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  compute.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;compute.stage.module=module;compute.stage.pName="main";
  VK_CHECK(vkCreateComputePipelines(context.device(),VK_NULL_HANDLE,1,&compute,nullptr,&mPipeline));vkDestroyShaderModule(context.device(),module,nullptr);
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VK_CHECK(vkCreateSampler(context.device(),&sampler,nullptr,&mSampler));
  VkDescriptorPoolSize counts[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=2;pool.poolSizeCount=2;pool.pPoolSizes=counts;
  VK_CHECK(vkCreateDescriptorPool(context.device(),&pool,nullptr,&mPool));
  VkDescriptorSetLayout layouts[]={mLayout,mLayout};VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=mPool;allocate.descriptorSetCount=2;allocate.pSetLayouts=layouts;
  VK_CHECK(vkAllocateDescriptorSets(context.device(),&allocate,mSets.data()));
  uint32_t queueCount=0;vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice(),&queueCount,nullptr);
  std::vector<VkQueueFamilyProperties> queues(queueCount);vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice(),&queueCount,queues.data());
  for(uint32_t i=0;i<2;++i) {
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.size=sizeof(atmosphere::LuminanceHistogram);buffer.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo allocation{};allocation.usage=VMA_MEMORY_USAGE_AUTO;allocation.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info;VK_CHECK(vmaCreateBuffer(context.allocator(),&buffer,&allocation,&mBuffers[i],&mAllocations[i],&info));mMapped[i]=info.pMappedData;
    VkDescriptorBufferInfo input{mBuffers[i],0,VK_WHOLE_SIZE};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=mSets[i];write.dstBinding=1;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&input;
    vkUpdateDescriptorSets(context.device(),1,&write,0,nullptr);
    if(queues[context.graphicsQueueFamily()].timestampValidBits) {
      VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};query.queryType=VK_QUERY_TYPE_TIMESTAMP;query.queryCount=2;VK_CHECK(vkCreateQueryPool(context.device(),&query,nullptr,&mQueries[i]));
    }
  }
  return true;
}
void VulkanExposureMeter::prepare(uint32_t frame) {
  if(!mWritten[frame])return;
  vmaInvalidateAllocation(mContext->allocator(),mAllocations[frame],0,VK_WHOLE_SIZE);
  atmosphere::LuminanceHistogram histogram;memcpy(histogram.data(),mMapped[frame],sizeof(histogram));
  mLuminance=atmosphere::meteredLuminance(histogram);mReady=true;mWritten[frame]=false;
  if(mQueries[frame]) {
    uint64_t stamps[2]{};if(vkGetQueryPoolResults(mContext->device(),mQueries[frame],0,2,sizeof(stamps),stamps,8,VK_QUERY_RESULT_64_BIT)==VK_SUCCESS)
      mMilliseconds=float(stamps[1]-stamps[0])*mContext->properties().limits.timestampPeriod*1e-6f;
  }
}
void VulkanExposureMeter::record(VkCommandBuffer cmd,uint32_t frame,VkImageView world,VkExtent2D size) {
  VkDescriptorImageInfo image{mSampler,world,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=mSets[frame];write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;
  vkUpdateDescriptorSets(mContext->device(),1,&write,0,nullptr);
  auto barrier=[&](VkPipelineStageFlags2 src,VkAccessFlags2 access,VkPipelineStageFlags2 dst,VkAccessFlags2 next) {
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};b.srcStageMask=src;b.srcAccessMask=access;b.dstStageMask=dst;b.dstAccessMask=next;b.buffer=mBuffers[frame];b.size=VK_WHOLE_SIZE;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};info.bufferMemoryBarrierCount=1;info.pBufferMemoryBarriers=&b;vkCmdPipelineBarrier2(cmd,&info);
  };
  if(mQueries[frame]){vkCmdResetQueryPool(cmd,mQueries[frame],0,2);vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,mQueries[frame],0);}
  barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
  vkCmdFillBuffer(cmd,mBuffers[frame],0,VK_WHOLE_SIZE,0);
  barrier(VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mPipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mPipelineLayout,0,1,&mSets[frame],0,nullptr);
  vkCmdDispatch(cmd,(size.width+63)/64,(size.height+63)/64,1);
  barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_READ_BIT);
  if(mQueries[frame])vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,mQueries[frame],1);mWritten[frame]=true;
}
void VulkanExposureMeter::destroy() {
  if(!mContext)return;vkDestroyPipeline(mContext->device(),mPipeline,nullptr);vkDestroyPipelineLayout(mContext->device(),mPipelineLayout,nullptr);vkDestroyDescriptorSetLayout(mContext->device(),mLayout,nullptr);vkDestroyDescriptorPool(mContext->device(),mPool,nullptr);vkDestroySampler(mContext->device(),mSampler,nullptr);
  for(uint32_t i=0;i<2;++i){vmaDestroyBuffer(mContext->allocator(),mBuffers[i],mAllocations[i]);if(mQueries[i])vkDestroyQueryPool(mContext->device(),mQueries[i],nullptr);}mContext=nullptr;
}
} // namespace vkrhi

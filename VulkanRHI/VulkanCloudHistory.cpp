#include "VulkanCloudHistory.h"
#include <fstream>
#include <cstring>
#include <cstdio>
#include <glm/gtc/packing.hpp>
#include <cmath>

namespace vkrhi {
namespace {
constexpr size_t kProbeBytes=64*(8+8+4+4);
void transition(VkCommandBuffer cmd,VkImage image,VkPipelineStageFlags2 src,VkAccessFlags2 access,VkPipelineStageFlags2 dst,VkAccessFlags2 next,VkImageLayout old=VK_IMAGE_LAYOUT_GENERAL) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};b.srcStageMask=src;b.srcAccessMask=access;b.dstStageMask=dst;b.dstAccessMask=next;
  b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.oldLayout=old;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};info.imageMemoryBarrierCount=1;info.pImageMemoryBarriers=&b;vkCmdPipelineBarrier2(cmd,&info);
}
glm::vec3 forward(const VulkanCloudHistory::Data& d) {
  const glm::vec4 far=d.invViewProj*glm::vec4(0,0,1,1);
  return glm::normalize(glm::vec3(far)/far.w-glm::vec3(d.camera));
}
}
bool VulkanCloudHistory::init(VulkanContext& context,const std::string& directory) {
  mContext=&context;
  for(VkFormat format:{VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R32_SFLOAT}) {
    VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(context.physicalDevice(),format,&properties);
    const auto required=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if((properties.optimalTilingFeatures&required)!=required)return false;
  }
  std::array<VkDescriptorSetLayoutBinding,7> bindings{};
  for(uint32_t i=0;i<7;++i)bindings[i]={i,i<4?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:i<6?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
  VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};layout.bindingCount=uint32_t(bindings.size());layout.pBindings=bindings.data();
  VK_CHECK(vkCreateDescriptorSetLayout(context.device(),&layout,nullptr,&mLayout));
  VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pipelineLayout.setLayoutCount=1;pipelineLayout.pSetLayouts=&mLayout;
  VK_CHECK(vkCreatePipelineLayout(context.device(),&pipelineLayout,nullptr,&mPipelineLayout));
  std::ifstream file(directory+"/cloudHistory.comp.spv",std::ios::binary|std::ios::ate);if(!file)return false;
  std::vector<uint32_t> code(size_t(file.tellg())/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),code.size()*4);
  VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader.codeSize=code.size()*4;shader.pCode=code.data();VkShaderModule module;
  VK_CHECK(vkCreateShaderModule(context.device(),&shader,nullptr,&module));
  VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};compute.layout=mPipelineLayout;compute.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  compute.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;compute.stage.module=module;compute.stage.pName="main";
  VK_CHECK(vkCreateComputePipelines(context.device(),VK_NULL_HANDLE,1,&compute,nullptr,&mPipeline));vkDestroyShaderModule(context.device(),module,nullptr);
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR;sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VK_CHECK(vkCreateSampler(context.device(),&sampler,nullptr,&mSampler));
  uint32_t queueCount=0;vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice(),&queueCount,nullptr);
  std::vector<VkQueueFamilyProperties> queues(queueCount);vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice(),&queueCount,queues.data());
  if(queues[context.graphicsQueueFamily()].timestampValidBits)for(auto& query:mQueries) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=2;
    VK_CHECK(vkCreateQueryPool(context.device(),&info,nullptr,&query));
  }
  return true;
}
VulkanCloudHistory::Image VulkanCloudHistory::createImage(VkExtent2D size,VkFormat format,bool attachment) {
  Image result;VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.imageType=VK_IMAGE_TYPE_2D;image.format=format;image.extent={size.width,size.height,1};
  image.mipLevels=image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;
  image.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|(attachment?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:VK_IMAGE_USAGE_STORAGE_BIT);
  VmaAllocationCreateInfo allocation{};allocation.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  VK_CHECK(vmaCreateImage(mContext->allocator(),&image,&allocation,&result.image,&result.allocation,nullptr));
  VmaAllocationInfo allocated;vmaGetAllocationInfo(mContext->allocator(),result.allocation,&allocated);result.bytes=allocated.size;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=result.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=format;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  VK_CHECK(vkCreateImageView(mContext->device(),&view,nullptr,&result.view));return result;
}
std::unique_ptr<VulkanCloudHistory::Resources> VulkanCloudHistory::createResources(VkExtent2D size) {
  auto r=std::make_unique<Resources>();r->size=size;
  VkDescriptorPoolSize counts[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,8},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,4},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2}};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=2;pool.poolSizeCount=3;pool.pPoolSizes=counts;
  VK_CHECK(vkCreateDescriptorPool(mContext->device(),&pool,nullptr,&r->pool));
  VkDescriptorSetLayout layouts[]={mLayout,mLayout};VkDescriptorSetAllocateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sets.descriptorPool=r->pool;sets.descriptorSetCount=2;sets.pSetLayouts=layouts;
  VK_CHECK(vkAllocateDescriptorSets(mContext->device(),&sets,r->sets.data()));
  for(uint32_t i=0;i<2;++i) {
    r->depth[i]=createImage(size,VK_FORMAT_R32_SFLOAT,true);r->historyDepth[i]=createImage(size,VK_FORMAT_R32_SFLOAT);r->history[i]=createImage(size,VK_FORMAT_R16G16B16A16_SFLOAT);
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.size=sizeof(Data);buffer.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo allocation{};allocation.usage=VMA_MEMORY_USAGE_AUTO;allocation.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info;VK_CHECK(vmaCreateBuffer(mContext->allocator(),&buffer,&allocation,&r->uniform[i],&r->uniformAllocation[i],&info));r->mapped[i]=info.pMappedData;
    buffer.size=kProbeBytes;buffer.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    allocation.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VK_CHECK(vmaCreateBuffer(mContext->allocator(),&buffer,&allocation,&r->probeBuffers[i],&r->probeAllocations[i],&info));r->probeMapped[i]=info.pMappedData;
  }
  for(uint32_t i=0;i<2;++i) {
    const Image* images[]={&r->depth[i],&r->history[1-i],&r->historyDepth[1-i],&r->history[i],&r->historyDepth[i]};
    for(uint32_t j=1;j<6;++j) {
      VkDescriptorImageInfo image{j<4?mSampler:VK_NULL_HANDLE,images[j-1]->view,j==1?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL};
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=r->sets[i];write.dstBinding=j;write.descriptorCount=1;write.descriptorType=j<4?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&image;
      vkUpdateDescriptorSets(mContext->device(),1,&write,0,nullptr);
    }
    VkDescriptorBufferInfo buffer{r->uniform[i],0,sizeof(Data)};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=r->sets[i];write.dstBinding=6;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;write.pBufferInfo=&buffer;
    vkUpdateDescriptorSets(mContext->device(),1,&write,0,nullptr);
  }
  return r;
}
void VulkanCloudHistory::prepare(uint32_t frame,uint64_t serial,VkExtent2D size) {
  if(mResources&&mResources->probeWritten[frame]) {
    auto& r=*mResources;vmaInvalidateAllocation(mContext->allocator(),r.probeAllocations[frame],0,VK_WHOLE_SIZE);
    const auto* values=static_cast<const uint64_t*>(r.probeMapped[frame]);
    const auto* depth=reinterpret_cast<const float*>(values+128);
    mProbe={{"reused",r.probeReused[frame]},{"resets",mResetCount},{"samples",nlohmann::json::array()},{"finite",true}};
    for(int i=0;i<64;++i) {
      const auto raw=glm::unpackHalf4x16(values[i]),filtered=glm::unpackHalf4x16(values[64+i]);
      bool finite=std::isfinite(depth[i])&&std::isfinite(depth[64+i]);
      for(int c=0;c<4;++c)finite=finite&&std::isfinite(raw[c])&&std::isfinite(filtered[c]);
      mProbe["finite"]=mProbe["finite"].get<bool>()&&finite;
      mProbe["samples"].push_back({{"raw",{raw.x,raw.y,raw.z,raw.w}},{"filtered",{filtered.x,filtered.y,filtered.z,filtered.w}},{"depth",depth[i]},{"historyDepth",depth[64+i]}});
    }
    r.probeWritten[frame]=false;
  }
  if(mQueryWritten[frame]&&mQueries[frame]) {
    uint64_t stamps[2]{};if(vkGetQueryPoolResults(mContext->device(),mQueries[frame],0,2,sizeof(stamps),stamps,8,VK_QUERY_RESULT_64_BIT)==VK_SUCCESS)
      mMilliseconds=float(stamps[1]-stamps[0])*mContext->properties().limits.timestampPeriod*1e-6f;
  }
  for(auto it=mRetired.begin();it!=mRetired.end();) {
    if(serial>=it->serial+2){release(*it->resources);it=mRetired.erase(it);}else ++it;
  }
  if(!mResources||mResources->size.width!=size.width||mResources->size.height!=size.height) {
    if(mResources)mRetired.push_back({std::move(mResources),serial});
    mResources=createResources(size);invalidate();
  }
}
void VulkanCloudHistory::resolve(VkCommandBuffer cmd,uint32_t frame,VkImage rawImage,VkImageView raw,Data data,uint64_t signature,bool enabled) {
  auto& r=*mResources;
  const glm::vec3 wind(data.wind);
  if(!enabled||signature!=mSignature||glm::distance(glm::vec3(data.camera),glm::vec3(mPrevious.camera))>50||
      glm::dot(forward(data),forward(mPrevious))<.94f||data.controls.z!=mPrevious.controls.z||data.controls.w!=mPrevious.controls.w||
      data.wind.w!=mPrevious.wind.w||std::abs(data.camera.w-mPrevious.camera.w)>1) {invalidate();++mResetCount;}
  data.previousViewProj=glm::inverse(mPrevious.invViewProj);data.previousCamera=mPrevious.camera;
  data.controls.x=enabled&&mValid?1.f:0.f;data.wind=glm::vec4(wind-mPreviousWind,data.wind.w);
  memcpy(r.mapped[frame],&data,sizeof(data));vmaFlushAllocation(mContext->allocator(),r.uniformAllocation[frame],0,sizeof(data));
  VkDescriptorImageInfo image{mSampler,raw,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=r.sets[frame];write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;
  vkUpdateDescriptorSets(mContext->device(),1,&write,0,nullptr);
  if(mQueries[frame]) {vkCmdResetQueryPool(cmd,mQueries[frame],0,2);vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,mQueries[frame],0);}
  if(!r.initialized) {
    for(uint32_t i=0;i<2;++i)for(Image* target:{&r.history[i],&r.historyDepth[i]}) {
      transition(cmd,target->image,VK_PIPELINE_STAGE_2_NONE,0,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_IMAGE_LAYOUT_UNDEFINED);
      VkClearColorValue clear{};if(target==&r.history[i])clear.float32[3]=1;
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(cmd,target->image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);
      transition(cmd,target->image,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
    }
    r.initialized=true;
  }
  for(Image* target:{&r.history[frame],&r.historyDepth[frame]})transition(cmd,target->image,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT);
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mPipeline);
  vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mPipelineLayout,0,1,&r.sets[frame],0,nullptr);vkCmdDispatch(cmd,(r.size.width+7)/8,(r.size.height+7)/8,1);
  for(Image* target:{&r.history[frame],&r.historyDepth[frame]})transition(cmd,target->image,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT);
  if(mQueries[frame])vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,mQueries[frame],1);mQueryWritten[frame]=true;
  if(mProbeEnabled) {
    const VkImage images[]={rawImage,r.history[frame].image,r.depth[frame].image,r.historyDepth[frame].image};
    VkDeviceSize offset=0;
    for(int target=0;target<4;++target) {
      const bool attachment=target==0||target==2;
      const auto old=attachment?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL;
      VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};b.image=images[target];b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
      b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.oldLayout=old;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      b.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;b.srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
      b.dstStageMask=VK_PIPELINE_STAGE_2_COPY_BIT;b.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;
      VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dependency.imageMemoryBarrierCount=1;dependency.pImageMemoryBarriers=&b;vkCmdPipelineBarrier2(cmd,&dependency);
      std::array<VkBufferImageCopy,64> copies{};
      const uint32_t stride=target<2?8:4;
      for(uint32_t i=0;i<64;++i) {
        auto& copy=copies[i];copy.bufferOffset=offset+i*stride;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.imageOffset={int((2*(i%8)+1)*r.size.width/16),int((2*(i/8)+1)*r.size.height/16),0};copy.imageExtent={1,1,1};
      }
      vkCmdCopyImageToBuffer(cmd,images[target],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.probeBuffers[frame],64,copies.data());offset+=64*stride;
      b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.newLayout=old;b.srcStageMask=VK_PIPELINE_STAGE_2_COPY_BIT;b.srcAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;
      b.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;b.dstAccessMask=VK_ACCESS_2_SHADER_READ_BIT;vkCmdPipelineBarrier2(cmd,&dependency);
    }
    VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};host.srcStageMask=VK_PIPELINE_STAGE_2_COPY_BIT;host.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;host.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;host.buffer=r.probeBuffers[frame];host.size=VK_WHOLE_SIZE;
    host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dependency.bufferMemoryBarrierCount=1;dependency.pBufferMemoryBarriers=&host;vkCmdPipelineBarrier2(cmd,&dependency);
    r.probeWritten[frame]=true;r.probeReused[frame]=data.controls.x>.5f;
  }
  mPrevious=data;mPreviousWind=wind;mSignature=signature;mValid=enabled;
}
uint64_t VulkanCloudHistory::allocatedBytes() const {
  auto total=[](const Resources& r){uint64_t n=(sizeof(Data)+kProbeBytes)*2;for(int i=0;i<2;++i)for(const Image* image:{&r.depth[i],&r.history[i],&r.historyDepth[i]})n+=image->bytes;return n;};
  uint64_t bytes=mResources?total(*mResources):0;for(const auto& r:mRetired)bytes+=total(*r.resources);return bytes;
}
void VulkanCloudHistory::release(Resources& r) {
  vkDestroyDescriptorPool(mContext->device(),r.pool,nullptr);
  for(int i=0;i<2;++i) {
    for(Image* image:{&r.depth[i],&r.history[i],&r.historyDepth[i]}){vkDestroyImageView(mContext->device(),image->view,nullptr);vmaDestroyImage(mContext->allocator(),image->image,image->allocation);}
    vmaDestroyBuffer(mContext->allocator(),r.uniform[i],r.uniformAllocation[i]);
    vmaDestroyBuffer(mContext->allocator(),r.probeBuffers[i],r.probeAllocations[i]);
  }
}
void VulkanCloudHistory::destroy() {
  if(!mContext)return;if(mResources){release(*mResources);mResources.reset();}for(auto& r:mRetired)release(*r.resources);mRetired.clear();
  vkDestroyPipeline(mContext->device(),mPipeline,nullptr);vkDestroyPipelineLayout(mContext->device(),mPipelineLayout,nullptr);vkDestroyDescriptorSetLayout(mContext->device(),mLayout,nullptr);
  vkDestroySampler(mContext->device(),mSampler,nullptr);for(auto query:mQueries)if(query)vkDestroyQueryPool(mContext->device(),query,nullptr);mContext=nullptr;
}
} // namespace vkrhi

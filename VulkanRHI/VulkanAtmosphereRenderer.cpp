#include "VulkanAtmosphereRenderer.h"
#include "Rendering/SkyIrradiance.h"
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/packing.hpp>
#include <fstream>
#include <cstring>
#include <cstdio>
#include <chrono>

namespace vkrhi {
namespace {
constexpr VkFormat kFormat=VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr size_t kFogProbeBytes=194*8+16;
constexpr std::array<size_t,6> kSkyTexels{256*64,32*32,192*108,192*108,32*32*32,32*32*32};
constexpr size_t kSkyProbeBytes=(256*64+32*32+2*192*108+2*32*32*32)*8;
constexpr size_t kCloudProbeBytes=16*16*sizeof(float);
constexpr size_t kIrradianceBytes=16*sizeof(glm::vec4);
struct CpuTimer {
  float& milliseconds;
  std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
  ~CpuTimer() {milliseconds=std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-start).count();}
};
void barrier(VkCommandBuffer cmd,VkImage image,VkPipelineStageFlags2 source,
             VkAccessFlags2 access,VkPipelineStageFlags2 destination,VkAccessFlags2 next,
             VkImageLayout old=VK_IMAGE_LAYOUT_GENERAL,VkImageLayout layout=VK_IMAGE_LAYOUT_GENERAL) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask=source;b.srcAccessMask=access;b.dstStageMask=destination;b.dstAccessMask=next;
  b.oldLayout=old;b.newLayout=layout;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount=1;dependency.pImageMemoryBarriers=&b;
  vkCmdPipelineBarrier2(cmd,&dependency);
}
VkShaderModule shader(VkDevice device,const std::string& path) {
  std::ifstream file(path,std::ios::binary|std::ios::ate);
  if(!file) {std::fprintf(stderr,"Missing atmosphere shader: %s\n",path.c_str());std::abort();}
  std::vector<uint32_t> code(size_t(file.tellg())/4);file.seekg(0);
  file.read(reinterpret_cast<char*>(code.data()),code.size()*4);
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize=code.size()*4;info.pCode=code.data();VkShaderModule module;
  VK_CHECK(vkCreateShaderModule(device,&info,nullptr,&module));return module;
}
}
bool VulkanAtmosphereRenderer::init(VulkanContext& context,const std::string& directory,
    VkDescriptorSetLayout frame,VkDescriptorSetLayout tlas,VkDescriptorSetLayout environment,VkDescriptorSetLayout cloudNoise) {
  mContext=&context;
  VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(context.physicalDevice(),kFormat,&properties);
  constexpr VkFormatFeatureFlags required=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  if((properties.optimalTilingFeatures&required)!=required) {
    std::fprintf(stderr,"Atmosphere requires filtered RGBA16F storage images\n");return false;
  }
  vkGetPhysicalDeviceFormatProperties(context.physicalDevice(),VK_FORMAT_R32_SFLOAT,&properties);
  if((properties.optimalTilingFeatures&required)!=required)return false;
  vkGetPhysicalDeviceFormatProperties(context.physicalDevice(),VK_FORMAT_R32G32B32A32_SFLOAT,&properties);
  const auto terrainFeatures=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  if((properties.optimalTilingFeatures&terrainFeatures)!=terrainFeatures) {
    std::fprintf(stderr,"Atmosphere terrain requires filtered RGBA32F sampled images\n");return false;
  }
  std::vector<VkDescriptorSetLayoutBinding> bindings;
  for(uint32_t binding=0;binding<=26;++binding) {
    VkDescriptorType type=binding==26?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:binding==0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : (binding>=4&&binding<=7)||binding>=19 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings.push_back({binding,type,1,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr});
  }
  VkDescriptorSetLayoutCreateInfo descriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  descriptor.bindingCount=uint32_t(bindings.size());descriptor.pBindings=bindings.data();
  VK_CHECK(vkCreateDescriptorSetLayout(context.device(),&descriptor,nullptr,&mLayout));
  VkDescriptorSetLayout layouts[]={mLayout,frame,tlas,environment};
  VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layoutInfo.setLayoutCount=4;layoutInfo.pSetLayouts=layouts;
  VK_CHECK(vkCreatePipelineLayout(context.device(),&layoutInfo,nullptr,&mPipelineLayout));
  // Tables are ready before the environment cubemap consumes them. This
  // layout needs only the helper set; fog retains shared frame/TLAS/IBL sets.
  VkPushConstantRange skyPush{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(SkyData)};
  VkPipelineLayoutCreateInfo skyLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  skyLayout.setLayoutCount=1;skyLayout.pSetLayouts=&mLayout;
  skyLayout.pushConstantRangeCount=1;skyLayout.pPushConstantRanges=&skyPush;
  VK_CHECK(vkCreatePipelineLayout(context.device(),&skyLayout,nullptr,&mSkyLayout));
  VkShaderModule skyShader=shader(context.device(),directory+"/skyAtmosphere.comp.spv");
  VkComputePipelineCreateInfo skyPipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  skyPipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};skyPipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
  skyPipeline.stage.module=skyShader;skyPipeline.stage.pName="main";skyPipeline.layout=mSkyLayout;
  VK_CHECK(vkCreateComputePipelines(context.device(),VK_NULL_HANDLE,1,&skyPipeline,nullptr,&mSkyCompute));
  vkDestroyShaderModule(context.device(),skyShader,nullptr);
  skyShader=shader(context.device(),directory+"/skyIrradiance.comp.spv");
  skyPipeline.stage.module=skyShader;skyPipeline.layout=mPipelineLayout;
  VK_CHECK(vkCreateComputePipelines(context.device(),VK_NULL_HANDLE,1,&skyPipeline,nullptr,&mSkyIrradiancePipeline));
  vkDestroyShaderModule(context.device(),skyShader,nullptr);
  VkDescriptorSetLayout cloudLayouts[]={mLayout,cloudNoise,frame};
  VkPushConstantRange cloudPush{VK_SHADER_STAGE_COMPUTE_BIT,0,16};
  skyLayout.setLayoutCount=3;skyLayout.pSetLayouts=cloudLayouts;skyLayout.pPushConstantRanges=&cloudPush;
  VK_CHECK(vkCreatePipelineLayout(context.device(),&skyLayout,nullptr,&mCloudShadowLayout));
  skyShader=shader(context.device(),directory+"/cloudShadow.comp.spv");
  skyPipeline.stage.module=skyShader;skyPipeline.layout=mCloudShadowLayout;
  VK_CHECK(vkCreateComputePipelines(context.device(),VK_NULL_HANDLE,1,&skyPipeline,nullptr,&mCloudShadowPipeline));
  vkDestroyShaderModule(context.device(),skyShader,nullptr);
  const char* names[]={"fogVisibility.comp","fogInject.comp","fogHistory.comp","fogIntegrate.comp"};
  for(int i=0;i<4;++i) {
    VkShaderModule module=shader(context.device(),directory+"/"+names[i]+".spv");
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pipeline.stage.module=module;pipeline.stage.pName="main";
    pipeline.layout=mPipelineLayout;
    VK_CHECK(vkCreateComputePipelines(context.device(),VK_NULL_HANDLE,1,&pipeline,nullptr,&mCompute[i]));
    vkDestroyShaderModule(context.device(),module,nullptr);
  }
  VkShaderModule vertex=shader(context.device(),directory+"/tonemap.vert.spv");
  VkShaderModule fragment=shader(context.device(),directory+"/fogComposite.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  for(int i=0;i<2;++i) {stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;stages[i].module=i?fragment:vertex;stages[i].pName="main";}
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewport.viewportCount=viewport.scissorCount=1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.lineWidth=1;raster.cullMode=VK_CULL_MODE_NONE;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=15;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&attachment;
  VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=2;dynamic.pDynamicStates=states;
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.colorAttachmentCount=1;rendering.pColorAttachmentFormats=&kFormat;
  VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pipeline.pNext=&rendering;pipeline.stageCount=2;pipeline.pStages=stages;
  pipeline.pVertexInputState=&vi;pipeline.pInputAssemblyState=&ia;pipeline.pViewportState=&viewport;pipeline.pRasterizationState=&raster;
  pipeline.pMultisampleState=&ms;pipeline.pColorBlendState=&blend;pipeline.pDynamicState=&dynamic;pipeline.layout=mPipelineLayout;
  VK_CHECK(vkCreateGraphicsPipelines(context.device(),VK_NULL_HANDLE,1,&pipeline,nullptr,&mComposite));
  vkDestroyShaderModule(context.device(),vertex,nullptr);vkDestroyShaderModule(context.device(),fragment,nullptr);
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR;
  sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VK_CHECK(vkCreateSampler(context.device(),&sampler,nullptr,&mSampler));
  VkDescriptorSetLayoutBinding bloomBindings[] = {
    {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
    {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
    {2,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
  descriptor.bindingCount=3;descriptor.pBindings=bloomBindings;
  VK_CHECK(vkCreateDescriptorSetLayout(context.device(),&descriptor,nullptr,&mBloomLayout));
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,16};
  layoutInfo.setLayoutCount=1;layoutInfo.pSetLayouts=&mBloomLayout;layoutInfo.pushConstantRangeCount=1;layoutInfo.pPushConstantRanges=&push;
  VK_CHECK(vkCreatePipelineLayout(context.device(),&layoutInfo,nullptr,&mBloomPipelineLayout));
  VkShaderModule bloomShader=shader(context.device(),directory+"/bloomPyramid.comp.spv");
  VkComputePipelineCreateInfo bloomInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  bloomInfo.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};bloomInfo.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
  bloomInfo.stage.module=bloomShader;bloomInfo.stage.pName="main";bloomInfo.layout=mBloomPipelineLayout;
  VK_CHECK(vkCreateComputePipelines(context.device(),VK_NULL_HANDLE,1,&bloomInfo,nullptr,&mBloomPipeline));
  vkDestroyShaderModule(context.device(),bloomShader,nullptr);
  uint32_t queueCount=0;vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice(),&queueCount,nullptr);
  std::vector<VkQueueFamilyProperties> queues(queueCount);vkGetPhysicalDeviceQueueFamilyProperties(context.physicalDevice(),&queueCount,queues.data());
  if(!(queues[context.graphicsQueueFamily()].queueFlags&VK_QUEUE_COMPUTE_BIT)) return false;
  if(queues[context.graphicsQueueFamily()].timestampValidBits) for(auto& query:mQueries) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=9;
    VK_CHECK(vkCreateQueryPool(context.device(),&info,nullptr,&query));
  }
  if(queues[context.graphicsQueueFamily()].timestampValidBits) for(auto& query:mSkyQueries) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=4;
    VK_CHECK(vkCreateQueryPool(context.device(),&info,nullptr,&query));
  }
  return true;
}
VulkanAtmosphereRenderer::Image VulkanAtmosphereRenderer::createImage(VkExtent3D size,bool twoDimensional,VkFormat format) {
  Image result;VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image.imageType=twoDimensional?VK_IMAGE_TYPE_2D:VK_IMAGE_TYPE_3D;image.format=format;image.extent=size;image.mipLevels=image.arrayLayers=1;
  image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;
  image.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VmaAllocationCreateInfo allocation{};allocation.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  VK_CHECK(vmaCreateImage(mContext->allocator(),&image,&allocation,&result.image,&result.allocation,nullptr));
  VmaAllocationInfo info;vmaGetAllocationInfo(mContext->allocator(),result.allocation,&info);result.bytes=info.size;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=result.image;view.viewType=twoDimensional?VK_IMAGE_VIEW_TYPE_2D:VK_IMAGE_VIEW_TYPE_3D;view.format=format;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  VK_CHECK(vkCreateImageView(mContext->device(),&view,nullptr,&result.view));return result;
}
std::unique_ptr<VulkanAtmosphereRenderer::Resources> VulkanAtmosphereRenderer::createResources(VkExtent3D grid,VkExtent2D viewport) {
  auto resources=std::make_unique<Resources>();resources->grid=grid;resources->viewport=viewport;
  VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,22},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,28},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=2;pool.poolSizeCount=4;pool.pPoolSizes=sizes;
  VK_CHECK(vkCreateDescriptorPool(mContext->device(),&pool,nullptr,&resources->pool));
  VkDescriptorSetLayout layouts[]={mLayout,mLayout};VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=resources->pool;allocate.descriptorSetCount=2;allocate.pSetLayouts=layouts;
  VK_CHECK(vkAllocateDescriptorSets(mContext->device(),&allocate,resources->sets.data()));
  resources->sky[0]=createImage({256,64,1},true);
  resources->sky[1]=createImage({32,32,1},true);
  resources->sky[2]=createImage({192,108,1},true);
  resources->sky[3]=createImage({192,108,1},true);
  resources->sky[4]=createImage({32,32,32});
  resources->sky[5]=createImage({32,32,32});
  resources->cloudShadow=createImage({512,512,1},true,VK_FORMAT_R32_SFLOAT);
  VkBufferCreateInfo irradiance{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};irradiance.size=kIrradianceBytes;
  irradiance.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VmaAllocationCreateInfo irradianceAllocation{};irradianceAllocation.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  VK_CHECK(vmaCreateBuffer(mContext->allocator(),&irradiance,&irradianceAllocation,&resources->skyIrradiance,&resources->skyIrradianceAllocation,nullptr));
  for(int i=0;i<2;++i) {
    // Injection scratch is consumed before the next graphics-queue frame.
    // Sharing it saves a whole froxel volume without reducing grid quality.
    if(i==0) resources->raw[0]=createImage(grid);resources->history[i]=createImage(grid);
    resources->integrated[i]=createImage({grid.width,grid.height,grid.depth+1});
    resources->visibility[i]=createImage({32,18,16});
    resources->terrain[i]=createImage({256,256,1},true,VK_FORMAT_R32G32B32A32_SFLOAT);
    VkBufferCreateInfo staging{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};staging.size=256*256*sizeof(glm::vec4);staging.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo stagingAllocation{};stagingAllocation.usage=VMA_MEMORY_USAGE_AUTO;
    stagingAllocation.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo stagingInfo;VK_CHECK(vmaCreateBuffer(mContext->allocator(),&staging,&stagingAllocation,&resources->terrainStaging[i],&resources->terrainStagingAllocations[i],&stagingInfo));
    resources->terrainMapped[i]=stagingInfo.pMappedData;
    VkBufferCreateInfo probe{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};probe.size=kFogProbeBytes+kSkyProbeBytes+kCloudProbeBytes+kIrradianceBytes;probe.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo probeAllocation{};probeAllocation.usage=VMA_MEMORY_USAGE_AUTO;
    probeAllocation.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo probeInfo;VK_CHECK(vmaCreateBuffer(mContext->allocator(),&probe,&probeAllocation,&resources->probeBuffers[i],&resources->probeAllocations[i],&probeInfo));
    resources->probeMapped[i]=probeInfo.pMappedData;
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.size=sizeof(GpuData);buffer.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo allocation{};allocation.usage=VMA_MEMORY_USAGE_AUTO;allocation.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info;VK_CHECK(vmaCreateBuffer(mContext->allocator(),&buffer,&allocation,&resources->uniforms[i],&resources->uniformAllocations[i],&info));resources->mapped[i]=info.pMappedData;
  }
  for(int i=0;i<2;++i) {
    VkDescriptorBufferInfo buffer{resources->uniforms[i],0,sizeof(GpuData)};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=resources->sets[i];write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;write.pBufferInfo=&buffer;
    vkUpdateDescriptorSets(mContext->device(),1,&write,0,nullptr);
    const uint32_t bindings[]={1,2,3,4,5,6,7,8,12,11};
    const Image* images[]={&resources->raw[0],&resources->history[1-i],&resources->integrated[i],&resources->raw[0],&resources->history[i],&resources->integrated[i],&resources->visibility[i],&resources->visibility[i],&resources->history[i],&resources->terrain[i]};
    for(int b=0;b<10;++b) {
      bool storage=bindings[b]>=4&&bindings[b]<=7;
      VkDescriptorImageInfo image{storage?VK_NULL_HANDLE:mSampler,images[b]->view,VK_IMAGE_LAYOUT_GENERAL};
      write.dstBinding=bindings[b];write.descriptorType=storage?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pBufferInfo=nullptr;write.pImageInfo=&image;
      vkUpdateDescriptorSets(mContext->device(),1,&write,0,nullptr);
    }
  }
  for(int frame=0;frame<2;++frame) for(int b=0;b<12;++b) {
    const bool storage=b>=6;
    VkDescriptorImageInfo image{storage?VK_NULL_HANDLE:mSampler,resources->sky[b%6].view,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=resources->sets[frame];write.dstBinding=13+b;
    write.descriptorCount=1;write.descriptorType=storage?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo=&image;vkUpdateDescriptorSets(mContext->device(),1,&write,0,nullptr);
  }
  for(auto set:resources->sets) {
    VkDescriptorImageInfo image{VK_NULL_HANDLE,resources->cloudShadow.view,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=25;
    write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&image;
    vkUpdateDescriptorSets(mContext->device(),1,&write,0,nullptr);
    VkDescriptorBufferInfo buffer{resources->skyIrradiance,0,kIrradianceBytes};
    write.dstBinding=26;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pImageInfo=nullptr;write.pBufferInfo=&buffer;
    vkUpdateDescriptorSets(mContext->device(),1,&write,0,nullptr);
  }
  VkDescriptorPoolSize bloomSizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,44},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,22}};
  pool.maxSets=22;pool.poolSizeCount=2;pool.pPoolSizes=bloomSizes;
  VK_CHECK(vkCreateDescriptorPool(mContext->device(),&pool,nullptr,&resources->bloomPool));
  for(int frame=0;frame<2;++frame) {
    for(int level=0;level<6;++level)
      resources->bloom[frame][level]=createImage({std::max(1u,viewport.width>>(level+1)),std::max(1u,viewport.height>>(level+1)),1},true);
    std::array<VkDescriptorSetLayout,11> bloomLayouts;bloomLayouts.fill(mBloomLayout);
    allocate.descriptorPool=resources->bloomPool;allocate.descriptorSetCount=11;allocate.pSetLayouts=bloomLayouts.data();
    VK_CHECK(vkAllocateDescriptorSets(mContext->device(),&allocate,resources->bloomSets[frame].data()));
    for(int pass=0;pass<11;++pass) {
      int output=pass<6?pass:10-pass;
      int input=pass<6?std::max(0,pass-1):output+1;
      for(int binding=0;binding<3;++binding) {
        VkDescriptorImageInfo image{binding==2?VK_NULL_HANDLE:mSampler,resources->bloom[frame][binding==2?output:input].view,VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=resources->bloomSets[frame][pass];write.dstBinding=binding;write.descriptorCount=1;
        write.descriptorType=binding==2?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;
        vkUpdateDescriptorSets(mContext->device(),1,&write,0,nullptr);
      }
    }
  }
  return resources;
}
void VulkanAtmosphereRenderer::prepare(uint32_t frame,uint64_t serial,VkExtent2D viewport,atmosphere::Quality quality) {
  CpuTimer timer{mCpuPrepareMs};
  if(mSkyQueries[frame]&&mSkyQueryWritten[frame]) {
    uint64_t stamps[4]{};
    if(vkGetQueryPoolResults(mContext->device(),mSkyQueries[frame],0,4,sizeof(stamps),stamps,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
      mSkyMilliseconds=float(stamps[1]-stamps[0])*mContext->properties().limits.timestampPeriod*1e-6f;
      mCloudShadowMilliseconds=float(stamps[3]-stamps[2])*mContext->properties().limits.timestampPeriod*1e-6f;
    }
  }
  if(mResources&&mResources->probeWritten[frame]) {
    auto& r=*mResources;
    vmaInvalidateAllocation(mContext->allocator(),r.probeAllocations[frame],0,VK_WHOLE_SIZE);
    const uint32_t slices=r.grid.depth;
    const auto& data=r.probeData[frame];
    mProbe={{"viewport",{r.viewport.width,r.viewport.height}},{"grid",{r.grid.width,r.grid.height,slices}},{"range",data.grid.w},{"camera",{data.camera.x,data.camera.y,data.camera.z}},
      {"boundaries",nlohmann::json::array()},{"medium",nlohmann::json::array()}};
    mProbe["terrainRevision"]=r.terrainRevision[frame];
    mProbe["terrainField"]={data.terrainField.x,data.terrainField.y,data.terrainField.z};
    const auto* packed=static_cast<const uint64_t*>(r.probeMapped[frame]);
    glm::vec2 uv((float(r.grid.width/2)+.5f)/r.grid.width,(float(r.grid.height/2)+.5f)/r.grid.height);
    glm::vec4 farPoint=data.invViewProj*glm::vec4(uv*2.f-1.f,1,1);
    glm::vec3 ray=glm::normalize(glm::vec3(farPoint)/farPoint.w-glm::vec3(data.camera));
    const float depthScale=1.f/std::max(-(data.view*glm::vec4(ray,0)).z,.001f);
    mProbe["direction"]={ray.x,ray.y,ray.z};mProbe["depthScale"]=depthScale;
    const glm::vec4 hdr=glm::unpackHalf4x16(packed[194]),bloom=glm::unpackHalf4x16(packed[195]);
    mProbe["hdr"]={hdr.x,hdr.y,hdr.z};mProbe["bloom"]={bloom.x,bloom.y,bloom.z};
    const auto& sky=r.probeSkyData[frame];
    mProbe["sky"]={{"haze",sky.camera.w},{"staticBuilds",r.skyStaticBuilds},{"viewBuilds",r.skyViewBuilds},{"aerialBuilds",r.skyAerialBuilds},
      {"sun",{sky.sun.x,sky.sun.y,sky.sun.z,sky.sun.w}},{"transmittanceSamples",nlohmann::json::array()},{"tables",nlohmann::json::array()}};
    size_t start=kFogProbeBytes/8;
    for(int table=0;table<6;++table) {
      glm::vec3 minimum(1e30f),maximum(-1e30f);bool finite=true;
      for(size_t t=0;t<kSkyTexels[table];++t) {
        const auto value=glm::vec3(glm::unpackHalf4x16(packed[start+t]));
        for(int c=0;c<3;++c)finite=finite&&std::isfinite(value[c]);
        minimum=glm::min(minimum,value);maximum=glm::max(maximum,value);
      }
      mProbe["sky"]["tables"].push_back({{"finite",finite},{"minimum",{minimum.x,minimum.y,minimum.z}},{"maximum",{maximum.x,maximum.y,maximum.z}}});
      if(table==0)for(int y:{0,8,32,63})for(int x:{0,32,128,254}) {
        auto v=glm::unpackHalf4x16(packed[start+y*256+x]);
        mProbe["sky"]["transmittanceSamples"].push_back({{"texel",{x,y}},{"value",{v.x,v.y,v.z}}});
      }
      if(table==4||table==5) {
        auto& samples=mProbe["sky"][table==4?"aerialScattering":"aerialTransmission"]=nlohmann::json::array();
        for(int z:{0,1,8,16,31}) {
          auto v=glm::unpackHalf4x16(packed[start+(z*32+16)*32+16]);
          samples.push_back({{"slice",z},{"value",{v.x,v.y,v.z}}});
        }
      }
      start+=kSkyTexels[table];
    }
    const auto* cloud=static_cast<const float*>(r.probeMapped[frame])+(kFogProbeBytes+kSkyProbeBytes)/sizeof(float);
    float minimum=1,maximum=0;bool finite=true;
    for(int i=0;i<256;++i) {finite=finite&&std::isfinite(cloud[i]);minimum=std::min(minimum,cloud[i]);maximum=std::max(maximum,cloud[i]);}
    mProbe["cloudShadow"]={{"minimum",minimum},{"maximum",maximum},{"finite",finite}};
    const auto* sh=reinterpret_cast<const glm::vec4*>(static_cast<const char*>(r.probeMapped[frame])+kFogProbeBytes+kSkyProbeBytes+kCloudProbeBytes);
    atmosphere::sky::Harmonics coefficients{};finite=true;
    for(int i=0;i<9;++i) {
      coefficients[i]=glm::dvec3(sh[i]);
      for(int c=0;c<3;++c)finite=finite&&std::isfinite(sh[i][c]);
    }
    mProbe["skyIrradiance"]={{"finite",finite},{"diffuse",nlohmann::json::array()},{"mean",{sh[15].x,sh[15].y,sh[15].z}}};
    const glm::dvec3 normals[]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for(int i=0;i<6;++i) {
      const auto value=atmosphere::sky::diffuseIrradiance(coefficients,normals[i]);
      mProbe["skyIrradiance"]["diffuse"].push_back({{"harmonic",{value.x,value.y,value.z}},{"quadrature",{sh[9+i].x,sh[9+i].y,sh[9+i].z}}});
    }
    for(uint32_t z=0;z<=slices;++z) {
      glm::vec4 v=glm::unpackHalf4x16(packed[z]);
      mProbe["boundaries"].push_back({{"depth",atmosphere::sliceBoundary(z,slices,data.grid.w)},
        {"scattering",{v.x,v.y,v.z}},{"transmittance",v.w}});
    }
    for(uint32_t z=0;z<slices;++z) {
      glm::vec4 v=glm::unpackHalf4x16(packed[97+z]);
      mProbe["medium"].push_back({v.x,v.y,v.z,v.w});
    }
    r.probeWritten[frame]=false;
  }
  if(mQueries[frame]&&mQueryWritten[frame]) {
    uint64_t stamps[9]{};
    if(vkGetQueryPoolResults(mContext->device(),mQueries[frame],0,9,sizeof(stamps),stamps,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
      auto duration=[&](int a,int b){return float(stamps[b]-stamps[a])*mContext->properties().limits.timestampPeriod*1e-6f;};
      mTimings={duration(0,1),duration(1,2),duration(2,3),duration(3,4),duration(5,6)};
      mBloomMilliseconds=duration(7,8);
    }
  }
  for(auto it=mRetired.begin();it!=mRetired.end();) {
    if(serial>=it->serial+2) {release(*it->resources);it=mRetired.erase(it);} else ++it;
  }
  uint32_t width=quality==atmosphere::Quality::High?240:160;
  VkExtent3D grid{width,std::max(1u,uint32_t(std::lround(double(width)*viewport.height/std::max(1u,viewport.width)))),quality==atmosphere::Quality::High?96u:64u};
  if(!mResources||grid.width!=mResources->grid.width||grid.height!=mResources->grid.height||grid.depth!=mResources->grid.depth||viewport.width!=mResources->viewport.width||viewport.height!=mResources->viewport.height) {
    if(mResources) mRetired.push_back({std::move(mResources),serial});
    mResources=createResources(grid,viewport);invalidate();
  }
}
void VulkanAtmosphereRenderer::updateInputs(uint32_t frame,VkImageView scene,VkImageView depth) {
  for(uint32_t binding : {9u,10u}) {
    VkDescriptorImageInfo image{mSampler,binding==9?scene:depth,binding==9?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=mResources->sets[frame];write.dstBinding=binding;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;
    vkUpdateDescriptorSets(mContext->device(),1,&write,0,nullptr);
  }
}
void VulkanAtmosphereRenderer::recordCloudShadow(VkCommandBuffer cmd,uint32_t frame,VkDescriptorSet noise,VkDescriptorSet shared,glm::vec4 field,uint64_t signature,bool enabled) {
  auto& r=*mResources;
  if(mSkyQueries[frame])vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,mSkyQueries[frame],2);
  if(!r.cloudShadowInitialized) {
    barrier(cmd,r.cloudShadow.image,VK_PIPELINE_STAGE_2_NONE,0,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue clear{};for(float& c:clear.float32)c=1;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(cmd,r.cloudShadow.image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);
    barrier(cmd,r.cloudShadow.image,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
    r.cloudShadowInitialized=true;
  }
  if(enabled&&(!r.cloudShadowValid||r.cloudShadowSignature!=signature)) {
  barrier(cmd,r.cloudShadow.image,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT);
  VkDescriptorSet sets[]={r.sets[frame],noise,shared};
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mCloudShadowPipeline);
  vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mCloudShadowLayout,0,3,sets,0,nullptr);
  vkCmdPushConstants(cmd,mCloudShadowLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,16,&field);vkCmdDispatch(cmd,64,64,1);
  barrier(cmd,r.cloudShadow.image,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT);
  r.cloudShadowValid=true;r.cloudShadowSignature=signature;
  }
  if(mSkyQueries[frame])vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,mSkyQueries[frame],3);
}
void VulkanAtmosphereRenderer::recordSky(VkCommandBuffer cmd,uint32_t frame,SkyData data,bool enabled) {
  auto& r=*mResources;
  if(mSkyQueries[frame]) {
    vkCmdResetQueryPool(cmd,mSkyQueries[frame],0,4);
    vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,mSkyQueries[frame],0);
  }
  if(!r.skyInitialized) {
    for(int i=0;i<6;++i) {
      auto& image=r.sky[i];barrier(cmd,image.image,VK_PIPELINE_STAGE_2_NONE,0,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_IMAGE_LAYOUT_UNDEFINED);
      VkClearColorValue clear{};if(i==0||i==3||i==5) for(float& c:clear.float32)c=1;
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(cmd,image.image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);
      barrier(cmd,image.image,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
    }
    r.skyInitialized=true;
  }
  if(enabled) {
    const bool rebuildStatic=!r.skyStaticValid||data.camera.w!=r.previousSky.camera.w;
    const bool rebuildView=rebuildStatic||!r.skyViewValid||data.camera.y!=r.previousSky.camera.y||data.sun!=r.previousSky.sun||data.moon!=r.previousSky.moon;
    const bool rebuildAerial=rebuildView||data.invViewProj!=r.previousSky.invViewProj||data.camera!=r.previousSky.camera;
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mSkyCompute);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mSkyLayout,0,1,&r.sets[frame],0,nullptr);
    for(int pass=0;pass<4;++pass) {
      if((pass<2&&!rebuildStatic)||(pass==2&&!rebuildView)||(pass==3&&!rebuildAerial))continue;
      int first=pass==0?0:pass==1?1:pass==2?2:4,last=pass<2?first:first+1;
      for(int i=first;i<=last;++i) barrier(cmd,r.sky[i].image,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT);
      data.controls.w=float(pass);vkCmdPushConstants(cmd,mSkyLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(data),&data);
      if(pass==0)vkCmdDispatch(cmd,32,8,1);else if(pass==1)vkCmdDispatch(cmd,4,4,1);else if(pass==2)vkCmdDispatch(cmd,24,14,1);else vkCmdDispatch(cmd,4,4,32);
      for(int i=first;i<=last;++i) barrier(cmd,r.sky[i].image,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT);
      if(pass==0)++r.skyStaticBuilds;if(pass==2)++r.skyViewBuilds;if(pass==3)++r.skyAerialBuilds;
    }
    r.skyStaticValid=r.skyViewValid=true;r.previousSky=data;
  }
  if(mSkyQueries[frame])vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,mSkyQueries[frame],1);
  mSkyQueryWritten[frame]=true;
  if(mProbeEnabled) {
    VkDeviceSize offset=kFogProbeBytes;
    for(int i=0;i<6;++i) {
      auto& image=r.sky[i];
      barrier(cmd,image.image,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
      VkBufferImageCopy copy{};copy.bufferOffset=offset;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
      copy.imageExtent=i==0?VkExtent3D{256,64,1}:i==1?VkExtent3D{32,32,1}:i<4?VkExtent3D{192,108,1}:VkExtent3D{32,32,32};
      vkCmdCopyImageToBuffer(cmd,image.image,VK_IMAGE_LAYOUT_GENERAL,r.probeBuffers[frame],1,&copy);
      barrier(cmd,image.image,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT);
      offset+=kSkyTexels[i]*8;
    }
    r.probeSkyData[frame]=data;
    // Fog's later copy adds the shared transfer->host dependency for this
    // buffer. Both copies retire with the existing frame-slot fence.
  }
}
void VulkanAtmosphereRenderer::recordSkyIrradiance(VkCommandBuffer cmd,uint32_t frame,VkDescriptorSet shared,VkDescriptorSet tlas,VkDescriptorSet environment,bool enabled) {
  auto& r=*mResources;
  auto dependency=[&](VkPipelineStageFlags2 src,VkAccessFlags2 access,VkPipelineStageFlags2 dst,VkAccessFlags2 next) {
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};b.srcStageMask=src;b.srcAccessMask=access;b.dstStageMask=dst;b.dstAccessMask=next;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.buffer=r.skyIrradiance;b.size=VK_WHOLE_SIZE;
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};info.bufferMemoryBarrierCount=1;info.pBufferMemoryBarriers=&b;vkCmdPipelineBarrier2(cmd,&info);
  };
  const auto readStages=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  // A shared queue-ordered buffer avoids a CPU readback or frame delay in sky
  // lighting. Current-frame descriptors are updated only after their fence.
  if(enabled) {
    dependency(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mSkyIrradiancePipeline);
    VkDescriptorSet sets[]={r.sets[frame],shared,tlas,environment};
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mPipelineLayout,0,4,sets,0,nullptr);vkCmdDispatch(cmd,1,1,1);
    dependency(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT,readStages,VK_ACCESS_2_SHADER_READ_BIT);
  } else {
    dependency(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cmd,r.skyIrradiance,0,VK_WHOLE_SIZE,0);
    dependency(VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,readStages,VK_ACCESS_2_SHADER_READ_BIT);
  }
  r.skyIrradianceInitialized=true;
  if(mProbeEnabled) {
    dependency(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy copy{0,kFogProbeBytes+kSkyProbeBytes+kCloudProbeBytes,kIrradianceBytes};
    vkCmdCopyBuffer(cmd,r.skyIrradiance,r.probeBuffers[frame],1,&copy);
    dependency(VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,readStages,VK_ACCESS_2_SHADER_READ_BIT);
  }
}
void VulkanAtmosphereRenderer::record(VkCommandBuffer cmd,uint32_t frame,GpuData data,VkDescriptorSet shared,VkDescriptorSet tlas,VkDescriptorSet environment) {
  CpuTimer timer{mCpuRecordMs};
  auto& r=*mResources;
  data.terrainField=glm::vec4(mTerrainOrigin,mTerrainSpan,0);
  data.grid.x=float(r.grid.width);data.grid.y=float(r.grid.height);data.grid.z=float(r.grid.depth);
  const glm::vec3 forward(data.view[0][2],data.view[1][2],data.view[2][2]);
  const glm::vec3 previousForward(mPrevious.view[0][2],mPrevious.view[1][2],mPrevious.view[2][2]);
  if(glm::dot(forward,previousForward)<.94f||glm::distance(glm::vec3(data.camera),glm::vec3(mPrevious.camera))>4||data.controls!=mPrevious.controls||data.ground!=mPrevious.ground||data.dust!=mPrevious.dust||data.groundAlbedo!=mPrevious.groundAlbedo||data.dustAlbedo!=mPrevious.dustAlbedo||data.grid!=mPrevious.grid||data.reference!=mPrevious.reference||data.terrain!=mPrevious.terrain||data.valley!=mPrevious.valley||data.artistic!=mPrevious.artistic||data.pointControls!=mPrevious.pointControls) invalidate();
  data.history.z=float(mSamplingIndex++%8);
  data.prevViewProj=glm::inverse(mPrevious.invViewProj);data.prevView=mPrevious.view;data.previousCamera=mPrevious.camera;
  if(!mHistoryValid) data.history.x=0;
  std::memcpy(r.mapped[frame],&data,sizeof(data));vmaFlushAllocation(mContext->allocator(),r.uniformAllocations[frame],0,VK_WHOLE_SIZE);
  if(!r.initialized) {
    for(int i=0;i<2;++i) for(Image* image : {&r.raw[i],&r.history[i],&r.integrated[i],&r.visibility[i],&r.terrain[i]}) {
      if(!image->image) continue;
      barrier(cmd,image->image,VK_PIPELINE_STAGE_2_NONE,0,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_IMAGE_LAYOUT_UNDEFINED);
      VkClearColorValue clear{};if(image==&r.integrated[i]||image==&r.visibility[i]) clear.float32[3]=1;
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(cmd,image->image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);
      barrier(cmd,image->image,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
    }
    for(int frame=0;frame<2;++frame) for(auto& image:r.bloom[frame]) {
      barrier(cmd,image.image,VK_PIPELINE_STAGE_2_NONE,0,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_IMAGE_LAYOUT_UNDEFINED);
      VkClearColorValue clear{};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(cmd,image.image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);
      barrier(cmd,image.image,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
    }
    r.initialized=true;
  }
  if(r.terrainRevision[frame]!=mTerrainRevision) {
    if(mTerrainValues.size()==256*256) {
      std::memcpy(r.terrainMapped[frame],mTerrainValues.data(),mTerrainValues.size()*sizeof(glm::vec4));
      vmaFlushAllocation(mContext->allocator(),r.terrainStagingAllocations[frame],0,VK_WHOLE_SIZE);
      barrier(cmd,r.terrain[frame].image,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
      VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={256,256,1};
      vkCmdCopyBufferToImage(cmd,r.terrainStaging[frame],r.terrain[frame].image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
      barrier(cmd,r.terrain[frame].image,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    r.terrainRevision[frame]=mTerrainRevision;
  }
  // The same graphics queue reads the previous history and later overwrites it.
  // Explicit WAR/WAW dependencies also cover two frames recorded ahead.
  for(Image* image : {&r.raw[0],&r.history[frame],&r.integrated[frame],&r.visibility[frame]})
    barrier(cmd,image->image,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
  VkDescriptorSet sets[]={r.sets[frame],shared,tlas,environment};
  vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mPipelineLayout,0,4,sets,0,nullptr);
  auto stamp=[&](int point) {if(mQueries[frame]) vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,mQueries[frame],point);};
  if(mQueries[frame]) vkCmdResetQueryPool(cmd,mQueries[frame],0,9);
  stamp(0);
  for(int pass=0;pass<4;++pass) {
    if(data.history.w>.5) {
      vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mCompute[pass]);
      if(pass==0) vkCmdDispatch(cmd,8,5,4);
      else if(pass==3) vkCmdDispatch(cmd,(r.grid.width+7)/8,(r.grid.height+7)/8,1);
      else vkCmdDispatch(cmd,(r.grid.width+3)/4,(r.grid.height+3)/4,(r.grid.depth+3)/4);
    } else {
      Image& image=pass==0?r.visibility[frame]:pass==1?r.raw[0]:pass==2?r.history[frame]:r.integrated[frame];
      barrier(cmd,image.image,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
      VkClearColorValue clear{};if(pass==3) clear.float32[3]=1;
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(cmd,image.image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);
      barrier(cmd,image.image,VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
    }
    Image& output=pass==0?r.visibility[frame]:pass==1?r.raw[0]:pass==2?r.history[frame]:r.integrated[frame];
    barrier(cmd,output.image,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    stamp(pass+1);
  }
  if(mProbeEnabled) {
    // Sparse half-float copies complete with this frame's existing fence. The
    // next reuse of this slot maps them; no extra queue submission or CPU wait.
    Image* images[]={&r.integrated[frame],&r.history[frame]};
    for(int i=0;i<2;++i) {
      barrier(cmd,images[i]->image,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT|VK_ACCESS_2_SHADER_READ_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
      VkBufferImageCopy copy{};copy.bufferOffset=i?97*8:0;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
      copy.imageOffset={int32_t(r.grid.width/2),int32_t(r.grid.height/2),0};copy.imageExtent={1,1,r.grid.depth+(i?0u:1u)};
      vkCmdCopyImageToBuffer(cmd,images[i]->image,VK_IMAGE_LAYOUT_GENERAL,r.probeBuffers[frame],1,&copy);
      barrier(cmd,images[i]->image,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT);
    }
    barrier(cmd,r.cloudShadow.image,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
    std::array<VkBufferImageCopy,256> cloudCopies{};
    for(int i=0;i<256;++i) {
      auto& copy=cloudCopies[i];copy.bufferOffset=kFogProbeBytes+kSkyProbeBytes+i*sizeof(float);
      copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageOffset={(i%16)*32+16,(i/16)*32+16,0};copy.imageExtent={1,1,1};
    }
    vkCmdCopyImageToBuffer(cmd,r.cloudShadow.image,VK_IMAGE_LAYOUT_GENERAL,r.probeBuffers[frame],uint32_t(cloudCopies.size()),cloudCopies.data());
    barrier(cmd,r.cloudShadow.image,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT);
    VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};host.srcStageMask=VK_PIPELINE_STAGE_2_COPY_BIT;host.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;host.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;host.buffer=r.probeBuffers[frame];host.size=VK_WHOLE_SIZE;
    host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dependency.bufferMemoryBarrierCount=1;dependency.pBufferMemoryBarriers=&host;vkCmdPipelineBarrier2(cmd,&dependency);
    r.probeWritten[frame]=true;r.probeData[frame]=data;
  }
  mPrevious=data;mHistoryValid=data.history.w>.5;
}
void VulkanAtmosphereRenderer::compose(VkCommandBuffer cmd,uint32_t frame,VkImageView hdr,VkExtent2D extent,VkDescriptorSet shared,VkDescriptorSet tlas,VkDescriptorSet environment) {
  CpuTimer timer{mCpuCompositionMs};
  if(mQueries[frame]) vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,mQueries[frame],5);
  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};color.imageView=hdr;color.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;color.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};render.renderArea.extent=extent;render.layerCount=1;render.colorAttachmentCount=1;render.pColorAttachments=&color;
  vkCmdBeginRendering(cmd,&render);
  VkViewport viewport{0,0,float(extent.width),float(extent.height),0,1};VkRect2D scissor{{0,0},extent};
  vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,mComposite);
  VkDescriptorSet sets[]={mResources->sets[frame],shared,tlas,environment};vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,mPipelineLayout,0,4,sets,0,nullptr);
  vkCmdDraw(cmd,3,1,0,0);vkCmdEndRendering(cmd);
  if(mQueries[frame]) vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,mQueries[frame],6);
  mQueryWritten[frame]=true;
}
void VulkanAtmosphereRenderer::bloom(VkCommandBuffer cmd,uint32_t frame,VkImage hdrImage,VkImageView hdr,bool firefly) {
  CpuTimer timer{mCpuBloomMs};
  auto& r=*mResources;
  VkDescriptorImageInfo source{mSampler,hdr,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=r.bloomSets[frame][0];write.dstBinding=0;write.descriptorCount=1;
  write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&source;
  vkUpdateDescriptorSets(mContext->device(),1,&write,0,nullptr);
  if(mQueries[frame]) vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,mQueries[frame],7);
  for(auto& image:r.bloom[frame]) barrier(cmd,image.image,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mBloomPipeline);
  constexpr float weights[]={.4f,.25f,.15f,.1f,.06f,.04f};
  for(int pass=0;pass<11;++pass) {
    int level=pass<6?pass:10-pass;
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,mBloomPipelineLayout,0,1,&r.bloomSets[frame][pass],0,nullptr);
    glm::vec4 controls(pass<6?0.f:1.f,weights[level],pass==6?weights[5]:1.f,firefly?1.f:0.f);
    vkCmdPushConstants(cmd,mBloomPipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,16,&controls);
    uint32_t width=std::max(1u,r.viewport.width>>(level+1)),height=std::max(1u,r.viewport.height>>(level+1));
    vkCmdDispatch(cmd,(width+7)/8,(height+7)/8,1);
    barrier(cmd,r.bloom[frame][level].image,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
  }
  if(mQueries[frame]) vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,mQueries[frame],8);
  if(mProbeEnabled) {
    barrier(cmd,hdrImage,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};copy.bufferOffset=194*8;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.imageOffset={int32_t(r.viewport.width/2),int32_t(r.viewport.height/2),0};copy.imageExtent={1,1,1};
    vkCmdCopyImageToBuffer(cmd,hdrImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.probeBuffers[frame],1,&copy);
    barrier(cmd,hdrImage,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(cmd,r.bloom[frame][0].image,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
    copy.bufferOffset=195*8;copy.imageOffset={int32_t(std::max(1u,r.viewport.width/2)/2),int32_t(std::max(1u,r.viewport.height/2)/2),0};
    vkCmdCopyImageToBuffer(cmd,r.bloom[frame][0].image,VK_IMAGE_LAYOUT_GENERAL,r.probeBuffers[frame],1,&copy);
    barrier(cmd,r.bloom[frame][0].image,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};host.srcStageMask=VK_PIPELINE_STAGE_2_COPY_BIT;host.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;host.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;host.buffer=r.probeBuffers[frame];host.size=VK_WHOLE_SIZE;
    host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dependency.bufferMemoryBarrierCount=1;dependency.pBufferMemoryBarriers=&host;vkCmdPipelineBarrier2(cmd,&dependency);
  }
}
VkDescriptorSet VulkanAtmosphereRenderer::set(uint32_t frame) const {return mResources->sets[frame];}
uint64_t VulkanAtmosphereRenderer::allocatedBytes() const {
  auto total=[](const Resources& resources) {
    uint64_t bytes=0;
    for(int i=0;i<2;++i) for(const Image* image : {&resources.raw[i],&resources.history[i],&resources.integrated[i],&resources.visibility[i],&resources.terrain[i]}) bytes+=image->bytes;
    for(int i=0;i<2;++i) for(const auto& image:resources.bloom[i]) bytes+=image.bytes;
    for(const auto& image:resources.sky)bytes+=image.bytes;
    bytes+=resources.cloudShadow.bytes;
    return bytes+sizeof(GpuData)*2+2*256*256*sizeof(glm::vec4)+kIrradianceBytes+2*(kFogProbeBytes+kSkyProbeBytes+kCloudProbeBytes+kIrradianceBytes);
  };
  uint64_t bytes=mResources?total(*mResources):0;
  // Quality transitions temporarily retain the old images until frame fences
  // retire them. Reporting only the newest grid hid that real allocation peak.
  for(const auto& retired:mRetired) bytes+=total(*retired.resources);
  return bytes;
}
void VulkanAtmosphereRenderer::release(Resources& r) {
  vkDestroyDescriptorPool(mContext->device(),r.pool,nullptr);
  vkDestroyDescriptorPool(mContext->device(),r.bloomPool,nullptr);
  for(auto& image:r.sky) {vkDestroyImageView(mContext->device(),image.view,nullptr);vmaDestroyImage(mContext->allocator(),image.image,image.allocation);}
  vkDestroyImageView(mContext->device(),r.cloudShadow.view,nullptr);vmaDestroyImage(mContext->allocator(),r.cloudShadow.image,r.cloudShadow.allocation);
  vmaDestroyBuffer(mContext->allocator(),r.skyIrradiance,r.skyIrradianceAllocation);
  for(int i=0;i<2;++i) {
    for(Image* image : {&r.raw[i],&r.history[i],&r.integrated[i],&r.visibility[i],&r.terrain[i]}) {
      if(!image->image) continue;
      vkDestroyImageView(mContext->device(),image->view,nullptr);vmaDestroyImage(mContext->allocator(),image->image,image->allocation);
    }
    for(auto& image:r.bloom[i]) {vkDestroyImageView(mContext->device(),image.view,nullptr);vmaDestroyImage(mContext->allocator(),image.image,image.allocation);}
    vmaDestroyBuffer(mContext->allocator(),r.probeBuffers[i],r.probeAllocations[i]);
    vmaDestroyBuffer(mContext->allocator(),r.terrainStaging[i],r.terrainStagingAllocations[i]);
    vmaDestroyBuffer(mContext->allocator(),r.uniforms[i],r.uniformAllocations[i]);
  }
}
void VulkanAtmosphereRenderer::destroy() {
  if(!mContext) return;
  if(mResources) {release(*mResources);mResources.reset();}
  for(auto& retired:mRetired) release(*retired.resources);mRetired.clear();
  for(auto pipeline:mCompute) vkDestroyPipeline(mContext->device(),pipeline,nullptr);
  vkDestroyPipeline(mContext->device(),mComposite,nullptr);vkDestroyPipelineLayout(mContext->device(),mPipelineLayout,nullptr);
  vkDestroyPipeline(mContext->device(),mSkyCompute,nullptr);vkDestroyPipelineLayout(mContext->device(),mSkyLayout,nullptr);
  vkDestroyPipeline(mContext->device(),mSkyIrradiancePipeline,nullptr);
  vkDestroyPipeline(mContext->device(),mCloudShadowPipeline,nullptr);vkDestroyPipelineLayout(mContext->device(),mCloudShadowLayout,nullptr);
  for(auto query:mSkyQueries)if(query)vkDestroyQueryPool(mContext->device(),query,nullptr);
  vkDestroyDescriptorSetLayout(mContext->device(),mLayout,nullptr);vkDestroySampler(mContext->device(),mSampler,nullptr);
  for(auto query:mQueries) if(query) vkDestroyQueryPool(mContext->device(),query,nullptr);
  vkDestroyPipeline(mContext->device(),mBloomPipeline,nullptr);vkDestroyPipelineLayout(mContext->device(),mBloomPipelineLayout,nullptr);vkDestroyDescriptorSetLayout(mContext->device(),mBloomLayout,nullptr);
  mContext=nullptr;
}
} // namespace vkrhi

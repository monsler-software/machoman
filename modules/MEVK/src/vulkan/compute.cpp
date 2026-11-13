#include "internal.h"
#include <utility>

namespace mevk {
ComputePipeline::Impl::~Impl() {
    std::lock_guard lock(context->mutex);
    for(auto [size,pipeline]:variants){(void)size;vkDestroyPipeline(context->device,pipeline,nullptr);}
    if(layout)vkDestroyPipelineLayout(context->device,layout,nullptr);
    if(descriptors)vkDestroyDescriptorSetLayout(context->device,descriptors,nullptr);
    if(shader)vkDestroyShaderModule(context->device,shader,nullptr);
}
ComputePipeline::ComputePipeline(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
ComputePipeline::~ComputePipeline()=default;
std::shared_ptr<ComputePipeline> Context::create_compute_pipeline(const ShaderFunction& shader,std::string& error) {
    error.clear();
    if(shader.stage!=ShaderStage::compute||shader.spirv.empty()){error="MEVK: compute pipeline requires a compute AIR function";return nullptr;}
    auto pipeline=std::make_unique<ComputePipeline::Impl>();pipeline->context=impl_;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for(auto b:shader.bindings) {
        auto expected=b.metal_index+(b.texture?32u:b.sampler?96u:0u);
        bool literal=b.sampler&&b.metal_index==UINT32_MAX&&b.binding==128&&b.literal_sampler==0x807bff0000080a49ull;
        if(!literal&&(b.metal_index>=(b.texture?32u:16u)||b.binding!=expected)){error="MEVK: invalid compute resource reflection";return nullptr;}
        if(b.texture&&b.writable&&!impl_->storage_write_without_format){error="MEVK: Vulkan device lacks storage-image writes without format";return nullptr;}
        for(auto other:pipeline->bindings)if(other.binding==b.binding){error="MEVK: duplicate compute resource";return nullptr;}
        pipeline->bindings.push_back(b);
        bindings.push_back({b.binding,b.texture?(b.writable?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE):b.sampler?VK_DESCRIPTOR_TYPE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
    }
    std::unique_lock lock(impl_->mutex);
    VkShaderModuleCreateInfo module{};module.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module.codeSize=shader.spirv.size()*sizeof(std::uint32_t);module.pCode=shader.spirv.data();
    if(!vk_check(vkCreateShaderModule(impl_->device,&module,nullptr,&pipeline->shader),"vkCreateShaderModule(compute)",error))return nullptr;
    VkDescriptorSetLayoutCreateInfo descriptors{};descriptors.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptors.bindingCount=static_cast<std::uint32_t>(bindings.size());descriptors.pBindings=bindings.data();
    if(!vk_check(vkCreateDescriptorSetLayout(impl_->device,&descriptors,nullptr,&pipeline->descriptors),"vkCreateDescriptorSetLayout(compute)",error))return nullptr;
    VkPushConstantRange offsets{VK_SHADER_STAGE_COMPUTE_BIT,0,128};
    VkPipelineLayoutCreateInfo layout{};layout.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount=1;layout.pSetLayouts=&pipeline->descriptors;layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&offsets;
    if(!vk_check(vkCreatePipelineLayout(impl_->device,&layout,nullptr,&pipeline->layout),"vkCreatePipelineLayout(compute)",error))return nullptr;
    lock.unlock();return std::shared_ptr<ComputePipeline>(new ComputePipeline(std::move(pipeline)));
}
VkPipeline ComputePipeline::Impl::pipeline(const std::array<std::uint32_t,3>& threads,std::string& error) {
    if(auto it=variants.find(threads);it!=variants.end())return it->second;
    VkSpecializationMapEntry entries[3]={{0,0,4},{1,4,4},{2,8,4}};
    VkSpecializationInfo specialization{3,entries,sizeof threads,threads.data()};
    VkComputePipelineCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module=shader;info.stage.pName="main";info.stage.pSpecializationInfo=&specialization;info.layout=layout;
    VkPipeline result=VK_NULL_HANDLE;
    if(!vk_check(vkCreateComputePipelines(context->device,VK_NULL_HANDLE,1,&info,nullptr,&result),"vkCreateComputePipelines",error))return VK_NULL_HANDLE;
    variants[threads]=result;return result;
}
bool CommandBuffer::dispatch(const std::shared_ptr<ComputePipeline>& pipeline,const RenderBindings& bindings,
                            const std::array<std::uint32_t,3>& groups,const std::array<std::uint32_t,3>& threads,std::string& error) {
    std::lock_guard lock(impl_->context->mutex);
    if(!impl_->recording(error))return false;
    if(impl_->rendering||!pipeline||pipeline->impl_->context!=impl_->context)return impl_->fail("MEVK: compute dispatch requires a compatible pipeline outside a render pass",error);
    auto& limits=impl_->context->properties.limits;
    std::uint64_t invocations=1;
    for(unsigned i=0;i<3;++i) {
        if(!threads[i]||threads[i]>limits.maxComputeWorkGroupSize[i]||groups[i]>limits.maxComputeWorkGroupCount[i])return impl_->fail("MEVK: invalid compute grid/threadgroup dimensions",error);
        invocations*=threads[i];
    }
    if(invocations>limits.maxComputeWorkGroupInvocations)return impl_->fail("MEVK: compute threadgroup exceeds device limit",error);
    if(!groups[0]||!groups[1]||!groups[2])return true;
    auto variant=pipeline->impl_->pipeline(threads,error);if(!variant)return impl_->fail(error,error);
    if(!impl_->bind_resources(pipeline->impl_->bindings,pipeline->impl_->descriptors,pipeline->impl_->layout,VK_PIPELINE_BIND_POINT_COMPUTE,bindings,error))return false;
    impl_->compute_pipelines.push_back(pipeline);
    VkMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(impl_->commands,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    vkCmdBindPipeline(impl_->commands,VK_PIPELINE_BIND_POINT_COMPUTE,variant);
    vkCmdDispatch(impl_->commands,groups[0],groups[1],groups[2]);
    barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(impl_->commands,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    return true;
}
} // namespace mevk

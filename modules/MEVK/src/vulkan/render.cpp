#include "internal.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace mevk {
namespace {
VkBlendFactor factor(std::uint32_t metal) {
    constexpr VkBlendFactor values[]={VK_BLEND_FACTOR_ZERO,VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,VK_BLEND_FACTOR_SRC_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_COLOR,VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    return values[metal];
}
}
RenderPipeline::Impl::~Impl() {
    std::lock_guard lock(context->mutex);
    for(auto [primitive,p]:variants){(void)primitive;vkDestroyPipeline(context->device,p,nullptr);}
    if(layout)vkDestroyPipelineLayout(context->device,layout,nullptr);
    if(descriptors)vkDestroyDescriptorSetLayout(context->device,descriptors,nullptr);
    if(pass)vkDestroyRenderPass(context->device,pass,nullptr);
    if(vertex)vkDestroyShaderModule(context->device,vertex,nullptr);
    if(fragment)vkDestroyShaderModule(context->device,fragment,nullptr);
}
RenderPipeline::RenderPipeline(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
RenderPipeline::~RenderPipeline()=default;
std::shared_ptr<RenderPipeline> Context::create_render_pipeline(const ShaderFunction& vertex,const ShaderFunction& fragment,
    std::uint32_t format,const BlendState& blend,std::string& error) {
    error.clear();
    if(vertex.stage!=ShaderStage::vertex||fragment.stage!=ShaderStage::fragment||vertex.spirv.empty()||fragment.spirv.empty()||
       mevk::pixel_format(format)==VK_FORMAT_UNDEFINED||blend.source_rgb>10||blend.destination_rgb>10||blend.source_alpha>10||
       blend.destination_alpha>10||blend.rgb_operation>4||blend.alpha_operation>4||blend.write_mask>15){error="MEVK: unsupported render pipeline shaders, format or blend state";return nullptr;}
    auto pipeline=std::make_unique<RenderPipeline::Impl>();pipeline->context=impl_;pipeline->pixel_format=format;pipeline->blend=blend;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for(const auto* shader:{&vertex,&fragment}) for(auto b:shader->bindings) {
        auto expected=b.metal_index+(shader->stage==ShaderStage::fragment?16u:0u)+(b.texture?32u:0u);
        if(b.metal_index>=16||b.binding!=expected){error="MEVK: invalid shader resource reflection";return nullptr;}
        for(auto other:pipeline->bindings)if(other.binding==b.binding){error="MEVK: duplicate shader binding";return nullptr;}
        pipeline->bindings.push_back(b);
        bindings.push_back({b.binding,b.texture?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,
            shader->stage==ShaderStage::vertex?VK_SHADER_STAGE_VERTEX_BIT:VK_SHADER_STAGE_FRAGMENT_BIT,nullptr});
    }
    std::unique_lock lock(impl_->mutex);
    auto create_shader=[&](const ShaderFunction& f,VkShaderModule& handle){VkShaderModuleCreateInfo info{};
        info.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;info.codeSize=f.spirv.size()*sizeof(std::uint32_t);info.pCode=f.spirv.data();
        return vk_check(vkCreateShaderModule(impl_->device,&info,nullptr,&handle),"vkCreateShaderModule",error);};
    if(!create_shader(vertex,pipeline->vertex)||!create_shader(fragment,pipeline->fragment))return nullptr;
    if(!create_color_pass(impl_->device,mevk::pixel_format(format),pipeline->pass,error))return nullptr;
    VkDescriptorSetLayoutCreateInfo descriptors{};descriptors.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptors.bindingCount=static_cast<std::uint32_t>(bindings.size());descriptors.pBindings=bindings.data();
    if(!vk_check(vkCreateDescriptorSetLayout(impl_->device,&descriptors,nullptr,&pipeline->descriptors),"vkCreateDescriptorSetLayout",error))return nullptr;
    VkPushConstantRange offsets{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,128};
    VkPipelineLayoutCreateInfo layout{};layout.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount=1;layout.pSetLayouts=&pipeline->descriptors;layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&offsets;
    if(!vk_check(vkCreatePipelineLayout(impl_->device,&layout,nullptr,&pipeline->layout),"vkCreatePipelineLayout",error))return nullptr;
    lock.unlock();return std::shared_ptr<RenderPipeline>(new RenderPipeline(std::move(pipeline)));
}
VkPipeline RenderPipeline::Impl::pipeline(std::uint32_t primitive,std::string& error) {
    if(auto found=variants.find(primitive);found!=variants.end())return found->second;
    constexpr VkPrimitiveTopology topologies[]={VK_PRIMITIVE_TOPOLOGY_POINT_LIST,VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    if(primitive>4){error="MEVK: unsupported primitive topology";return VK_NULL_HANDLE;}
    VkPipelineShaderStageCreateInfo stages[2]{};
    for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vertex;
    stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment;
    VkPipelineVertexInputStateCreateInfo inputs{};inputs.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology=topologies[primitive];
    VkPipelineViewportStateCreateInfo viewport{};viewport.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount=1;viewport.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_CLOCKWISE;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo samples{};samples.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};attachment.blendEnable=blend.enabled;
    attachment.srcColorBlendFactor=factor(blend.source_rgb);attachment.dstColorBlendFactor=factor(blend.destination_rgb);
    attachment.srcAlphaBlendFactor=factor(blend.source_alpha);attachment.dstAlphaBlendFactor=factor(blend.destination_alpha);
    attachment.colorBlendOp=static_cast<VkBlendOp>(blend.rgb_operation);attachment.alphaBlendOp=static_cast<VkBlendOp>(blend.alpha_operation);
    attachment.colorWriteMask=((blend.write_mask&8)?VK_COLOR_COMPONENT_R_BIT:0)|((blend.write_mask&4)?VK_COLOR_COMPONENT_G_BIT:0)|
                              ((blend.write_mask&2)?VK_COLOR_COMPONENT_B_BIT:0)|((blend.write_mask&1)?VK_COLOR_COMPONENT_A_BIT:0);
    VkPipelineColorBlendStateCreateInfo blending{};blending.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount=1;blending.pAttachments=&attachment;
    VkDynamicState dynamic_values[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};dynamic.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount=2;dynamic.pDynamicStates=dynamic_values;
    VkGraphicsPipelineCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount=2;info.pStages=stages;info.pVertexInputState=&inputs;info.pInputAssemblyState=&assembly;
    info.pViewportState=&viewport;info.pRasterizationState=&raster;info.pMultisampleState=&samples;info.pColorBlendState=&blending;
    info.pDynamicState=&dynamic;info.layout=layout;info.renderPass=pass;
    VkPipeline result=VK_NULL_HANDLE;
    if(!vk_check(vkCreateGraphicsPipelines(context->device,VK_NULL_HANDLE,1,&info,nullptr,&result),"vkCreateGraphicsPipelines",error))return VK_NULL_HANDLE;
    variants[primitive]=result;return result;
}

bool CommandBuffer::begin_render(const std::shared_ptr<Texture>& target,bool clear,const std::array<double,4>& color,std::string& error) {
    std::lock_guard lock(impl_->context->mutex);
    if(!impl_->recording(error))return false;
    if(impl_->rendering||!target||target->impl_->context!=impl_->context)return impl_->fail("MEVK: invalid or overlapping render target",error);
    for(auto c:color)if(!std::isfinite(c))return impl_->fail("MEVK: nonfinite clear color",error);
    impl_->textures.push_back(target);impl_->render_target=target;
    VkRenderPassBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass=target->impl_->pass;begin.framebuffer=target->impl_->framebuffer;begin.renderArea={{0,0},{target->width(),target->height()}};
    vkCmdBeginRenderPass(impl_->commands,&begin,VK_SUBPASS_CONTENTS_INLINE);impl_->rendering=true;
    if(clear){VkClearAttachment attachment{};attachment.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        for(unsigned i=0;i<4;++i)attachment.clearValue.color.float32[i]=static_cast<float>(color[i]);
        VkClearRect rectangle{begin.renderArea,0,1};vkCmdClearAttachments(impl_->commands,1,&attachment,1,&rectangle);}
    return true;
}
bool CommandBuffer::draw(const std::shared_ptr<RenderPipeline>& pipeline,const RenderBindings& bindings,
    const Viewport& viewport,const Scissor& scissor,std::uint32_t primitive,std::uint32_t start,std::uint32_t count,
    std::uint32_t instances,std::uint32_t base_instance,const std::shared_ptr<Buffer>& indices,
    std::uint64_t index_offset,std::uint32_t index_type,std::int32_t base_vertex,std::string& error) {
    std::lock_guard lock(impl_->context->mutex);
    if(!impl_->recording(error))return false;
    if(!impl_->rendering||!pipeline||pipeline->impl_->context!=impl_->context||pipeline->impl_->pixel_format!=impl_->render_target->pixel_format())
        return impl_->fail("MEVK: draw requires a compatible render pipeline and active target",error);
    auto& limits=impl_->context->properties.limits;
    if(!std::isfinite(viewport.x)||!std::isfinite(viewport.y)||!std::isfinite(viewport.width)||!std::isfinite(viewport.height)||
       !std::isfinite(viewport.znear)||!std::isfinite(viewport.zfar)||viewport.width<=0||viewport.height<=0||
       viewport.width>limits.maxViewportDimensions[0]||viewport.height>limits.maxViewportDimensions[1]||
       viewport.x<limits.viewportBoundsRange[0]||viewport.y<limits.viewportBoundsRange[0]||
       viewport.x+viewport.width>limits.viewportBoundsRange[1]||viewport.y+viewport.height>limits.viewportBoundsRange[1]||
       viewport.znear<0||viewport.zfar>1||viewport.znear>viewport.zfar||scissor.x>impl_->render_target->width()||
       scissor.width>impl_->render_target->width()-scissor.x||scissor.y>impl_->render_target->height()||
       scissor.height>impl_->render_target->height()-scissor.y)return impl_->fail("MEVK: invalid viewport or scissor",error);
    if(!count||!instances||!scissor.width||!scissor.height)return true;
    if(indices&&(indices->impl_->context!=impl_->context||index_type>1||index_offset>indices->size()||
        index_offset%(index_type?4u:2u)||std::uint64_t{count}*(index_type?4u:2u)>indices->size()-index_offset))
        return impl_->fail("MEVK: invalid index buffer range or type",error);
    for(auto b:pipeline->impl_->bindings) {
        if(b.texture) {
            auto t=bindings.textures[b.binding-32];
            if(!t||t->impl_->context!=impl_->context||t==impl_->render_target)return impl_->fail("MEVK: missing or incompatible shader texture",error);
        } else {
            auto buffer=bindings.buffers[b.binding];
            if(!buffer||buffer->impl_->context!=impl_->context||bindings.offsets[b.binding]>=buffer->size()||
               buffer->impl_->padded_size>limits.maxStorageBufferRange)return impl_->fail("MEVK: missing shader buffer or invalid offset/range",error);
        }
    }
    auto variant=pipeline->impl_->pipeline(primitive,error);if(!variant)return impl_->fail(error,error);
    if(!impl_->descriptors) {
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,32768},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,32768}};
        VkDescriptorPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;pool.maxSets=1024;
        pool.poolSizeCount=2;pool.pPoolSizes=sizes;
        if(!vk_check(vkCreateDescriptorPool(impl_->context->device,&pool,nullptr,&impl_->descriptors),"vkCreateDescriptorPool",error))return impl_->fail(error,error);
    }
    VkDescriptorSet set=VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool=impl_->descriptors;allocation.descriptorSetCount=1;allocation.pSetLayouts=&pipeline->impl_->descriptors;
    if(!vk_check(vkAllocateDescriptorSets(impl_->context->device,&allocation,&set),"vkAllocateDescriptorSets",error))return impl_->fail(error,error);
    std::vector<VkDescriptorBufferInfo> buffers;std::vector<VkDescriptorImageInfo> textures;std::vector<VkWriteDescriptorSet> writes;
    auto resource_count=pipeline->impl_->bindings.size();buffers.reserve(resource_count);textures.reserve(resource_count);writes.reserve(resource_count);
    for(auto b:pipeline->impl_->bindings) {
        VkWriteDescriptorSet write{};write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=set;
        write.dstBinding=b.binding;write.descriptorCount=1;
        if(b.texture) {
            auto t=bindings.textures[b.binding-32];impl_->textures.push_back(t);
            textures.push_back({impl_->context->sampler,t->impl_->view,VK_IMAGE_LAYOUT_GENERAL});
            write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&textures.back();
        } else {
            auto bfr=bindings.buffers[b.binding];impl_->resources.push_back(bfr);
            buffers.push_back({bfr->impl_->buffer,0,bfr->impl_->padded_size});
            write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&buffers.back();
        }
        writes.push_back(write);
    }
    vkUpdateDescriptorSets(impl_->context->device,static_cast<std::uint32_t>(writes.size()),writes.data(),0,nullptr);
    impl_->pipelines.push_back(pipeline);
    vkCmdBindPipeline(impl_->commands,VK_PIPELINE_BIND_POINT_GRAPHICS,variant);
    vkCmdBindDescriptorSets(impl_->commands,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline->impl_->layout,0,1,&set,0,nullptr);
    vkCmdPushConstants(impl_->commands,pipeline->impl_->layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,128,bindings.offsets.data());
    VkViewport vkviewport{static_cast<float>(viewport.x),static_cast<float>(viewport.y),static_cast<float>(viewport.width),
        static_cast<float>(viewport.height),static_cast<float>(viewport.znear),static_cast<float>(viewport.zfar)};
    VkRect2D vkscissor{{static_cast<std::int32_t>(scissor.x),static_cast<std::int32_t>(scissor.y)},{scissor.width,scissor.height}};
    vkCmdSetViewport(impl_->commands,0,1,&vkviewport);vkCmdSetScissor(impl_->commands,0,1,&vkscissor);
    if(indices){impl_->resources.push_back(indices);vkCmdBindIndexBuffer(impl_->commands,indices->impl_->buffer,index_offset,index_type?VK_INDEX_TYPE_UINT32:VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(impl_->commands,count,instances,0,base_vertex,base_instance);}
    else vkCmdDraw(impl_->commands,count,instances,start,base_instance);
    return true;
}
bool CommandBuffer::end_render(std::string& error) {
    std::lock_guard lock(impl_->context->mutex);
    if(!impl_->recording(error))return false;
    if(!impl_->rendering)return impl_->fail("MEVK: no render pass to end",error);
    vkCmdEndRenderPass(impl_->commands);impl_->rendering=false;impl_->render_target.reset();return true;
}
} // namespace mevk

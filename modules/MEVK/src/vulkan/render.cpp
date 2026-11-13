#include "internal.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace mevk {
namespace {
VkFormat vertex_format(std::uint32_t format) {
    constexpr VkFormat formats[]={VK_FORMAT_UNDEFINED,
        VK_FORMAT_R8G8_UINT,VK_FORMAT_R8G8B8_UINT,VK_FORMAT_R8G8B8A8_UINT,
        VK_FORMAT_R8G8_SINT,VK_FORMAT_R8G8B8_SINT,VK_FORMAT_R8G8B8A8_SINT,
        VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8G8B8_UNORM,VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_R8G8_SNORM,VK_FORMAT_R8G8B8_SNORM,VK_FORMAT_R8G8B8A8_SNORM,
        VK_FORMAT_R16G16_UINT,VK_FORMAT_R16G16B16_UINT,VK_FORMAT_R16G16B16A16_UINT,
        VK_FORMAT_R16G16_SINT,VK_FORMAT_R16G16B16_SINT,VK_FORMAT_R16G16B16A16_SINT,
        VK_FORMAT_R16G16_UNORM,VK_FORMAT_R16G16B16_UNORM,VK_FORMAT_R16G16B16A16_UNORM,
        VK_FORMAT_R16G16_SNORM,VK_FORMAT_R16G16B16_SNORM,VK_FORMAT_R16G16B16A16_SNORM,
        VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R16G16B16_SFLOAT,VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32G32B32_SFLOAT,VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_R32_SINT,VK_FORMAT_R32G32_SINT,VK_FORMAT_R32G32B32_SINT,VK_FORMAT_R32G32B32A32_SINT,
        VK_FORMAT_R32_UINT,VK_FORMAT_R32G32_UINT,VK_FORMAT_R32G32B32_UINT,VK_FORMAT_R32G32B32A32_UINT,
        VK_FORMAT_A2B10G10R10_SNORM_PACK32,VK_FORMAT_A2B10G10R10_UNORM_PACK32,VK_FORMAT_B8G8R8A8_UNORM};
    if(format<std::size(formats))return formats[format];
    switch(format){case 45:return VK_FORMAT_R8_UINT;case 46:return VK_FORMAT_R8_SINT;case 47:return VK_FORMAT_R8_UNORM;case 48:return VK_FORMAT_R8_SNORM;
        case 49:return VK_FORMAT_R16_UINT;case 50:return VK_FORMAT_R16_SINT;case 51:return VK_FORMAT_R16_UNORM;case 52:return VK_FORMAT_R16_SNORM;
        case 53:return VK_FORMAT_R16_SFLOAT;case 54:return VK_FORMAT_B10G11R11_UFLOAT_PACK32;case 55:return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        default:return VK_FORMAT_UNDEFINED;}
}
std::uint32_t vertex_bytes(std::uint32_t format) {
    if(format>=1&&format<=12)return 2+(format-1)%3;
    if(format>=13&&format<=27)return 4+2*((format-13)%3);
    if(format>=28&&format<=39)return 4+4*((format-28)%4);
    if(format>=40&&format<=42)return 4;
    if(format>=45&&format<=48)return 1;
    if(format>=49&&format<=53)return 2;
    return 4;
}
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
    std::uint32_t format,const BlendState& blend,std::string& error,const VertexInput* vertices,std::uint32_t depth_format) {
    error.clear();
    if(vertex.stage!=ShaderStage::vertex||fragment.stage!=ShaderStage::fragment||vertex.spirv.empty()||fragment.spirv.empty()||
       mevk::pixel_format(format)==VK_FORMAT_UNDEFINED||format==250||format==252||blend.source_rgb>10||blend.destination_rgb>10||blend.source_alpha>10||
       blend.destination_alpha>10||blend.rgb_operation>4||blend.alpha_operation>4||blend.write_mask>15||
       (depth_format&&depth_format!=250&&depth_format!=252)){error="MEVK: unsupported render pipeline shaders, format or blend state";return nullptr;}
    auto pipeline=std::make_unique<RenderPipeline::Impl>();pipeline->context=impl_;pipeline->pixel_format=format;pipeline->blend=blend;
    pipeline->depth_format=depth_format;
    VkFormatProperties format_properties{};vkGetPhysicalDeviceFormatProperties(impl_->physical_device,mevk::pixel_format(format),&format_properties);
    if(!(format_properties.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)){error="MEVK: pipeline color format cannot be rendered";return nullptr;}
    if(vertices) {
        const auto& limits=impl_->properties.limits;
        if(vertices->buffers.size()>limits.maxVertexInputBindings||vertices->attributes.size()>limits.maxVertexInputAttributes){error="MEVK: too many vertex bindings/attributes";return nullptr;}
        for(auto b:vertices->buffers) {
            if(b.buffer>=32||b.buffer>=limits.maxVertexInputBindings||b.stride>limits.maxVertexInputBindingStride||b.step_function>2||b.step_rate!=1){error="MEVK: unsupported vertex buffer layout";return nullptr;}
            for(auto previous:pipeline->vertex_buffers)if(previous.binding==b.buffer){error="MEVK: duplicate vertex buffer layout";return nullptr;}
            pipeline->vertex_buffers.push_back({b.buffer,b.step_function?b.stride:0,b.step_function==2?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX});
        }
        for(auto a:vertices->attributes) {
            auto vkformat=vertex_format(a.format);VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(impl_->physical_device,vkformat,&properties);
            if(a.location>=limits.maxVertexInputAttributes||a.buffer>=32||a.offset>limits.maxVertexInputAttributeOffset||vkformat==VK_FORMAT_UNDEFINED||
               !(properties.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)||
               std::none_of(pipeline->vertex_buffers.begin(),pipeline->vertex_buffers.end(),[&](auto b){return b.binding==a.buffer;})){error="MEVK: unsupported vertex attribute format, offset or binding";return nullptr;}
            for(auto previous:pipeline->vertex_attributes)if(previous.location==a.location){error="MEVK: duplicate vertex attribute location";return nullptr;}
            pipeline->vertex_attributes.push_back({a.location,a.buffer,vkformat,a.offset});
            pipeline->vertex_attribute_ends[a.buffer]=std::max(pipeline->vertex_attribute_ends[a.buffer],a.offset+vertex_bytes(a.format));
        }
    }
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for(const auto* shader:{&vertex,&fragment}) for(auto b:shader->bindings) {
        auto expected=b.metal_index+(shader->stage==ShaderStage::fragment?(b.texture?32u:16u):0u)+(b.texture?32u:b.sampler?96u:0u);
        bool literal=b.sampler&&b.metal_index==UINT32_MAX&&b.binding==128&&b.literal_sampler==0x807bff0000080a49ull;
        if(!literal&&(b.metal_index>=(b.texture?32u:16u)||b.binding!=expected)){error="MEVK: invalid shader resource reflection";return nullptr;}
        auto existing=std::find_if(bindings.begin(),bindings.end(),[&](const auto& other){return other.binding==b.binding;});
        if(existing!=bindings.end()) {
            if(!literal){error="MEVK: duplicate shader binding";return nullptr;}
            existing->stageFlags|=shader->stage==ShaderStage::vertex?VK_SHADER_STAGE_VERTEX_BIT:VK_SHADER_STAGE_FRAGMENT_BIT;
            continue;
        }
        pipeline->bindings.push_back(b);
        bindings.push_back({b.binding,b.texture?(b.writable?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE):b.sampler?VK_DESCRIPTOR_TYPE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,
            shader->stage==ShaderStage::vertex?VK_SHADER_STAGE_VERTEX_BIT:VK_SHADER_STAGE_FRAGMENT_BIT,nullptr});
    }
    std::unique_lock lock(impl_->mutex);
    auto create_shader=[&](const ShaderFunction& f,VkShaderModule& handle){VkShaderModuleCreateInfo info{};
        info.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;info.codeSize=f.spirv.size()*sizeof(std::uint32_t);info.pCode=f.spirv.data();
        return vk_check(vkCreateShaderModule(impl_->device,&info,nullptr,&handle),"vkCreateShaderModule",error);};
    if(!create_shader(vertex,pipeline->vertex)||!create_shader(fragment,pipeline->fragment))return nullptr;
    if(!create_color_depth_pass(impl_->device,mevk::pixel_format(format),depth_format?mevk::pixel_format(depth_format):VK_FORMAT_UNDEFINED,pipeline->pass,error))return nullptr;
    VkDescriptorSetLayoutCreateInfo descriptors{};descriptors.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptors.bindingCount=static_cast<std::uint32_t>(bindings.size());descriptors.pBindings=bindings.data();
    if(!vk_check(vkCreateDescriptorSetLayout(impl_->device,&descriptors,nullptr,&pipeline->descriptors),"vkCreateDescriptorSetLayout",error))return nullptr;
    VkPushConstantRange offsets{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,128};
    VkPipelineLayoutCreateInfo layout{};layout.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount=1;layout.pSetLayouts=&pipeline->descriptors;layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&offsets;
    if(!vk_check(vkCreatePipelineLayout(impl_->device,&layout,nullptr,&pipeline->layout),"vkCreatePipelineLayout",error))return nullptr;
    lock.unlock();return std::shared_ptr<RenderPipeline>(new RenderPipeline(std::move(pipeline)));
}
VkPipeline RenderPipeline::Impl::pipeline(std::uint32_t primitive,const DepthState& depth,const RasterState& raster_state,std::string& error) {
    const std::array<std::uint32_t,7> key={primitive,depth.compare,depth.write?1u:0u,raster_state.cull,raster_state.winding,raster_state.fill,raster_state.depth_clip};
    if(auto found=variants.find(key);found!=variants.end())return found->second;
    constexpr VkPrimitiveTopology topologies[]={VK_PRIMITIVE_TOPOLOGY_POINT_LIST,VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    if(primitive>4){error="MEVK: unsupported primitive topology";return VK_NULL_HANDLE;}
    VkPipelineShaderStageCreateInfo stages[2]{};
    for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vertex;
    stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment;
    VkPipelineVertexInputStateCreateInfo inputs{};inputs.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    inputs.vertexBindingDescriptionCount=static_cast<std::uint32_t>(vertex_buffers.size());inputs.pVertexBindingDescriptions=vertex_buffers.data();
    inputs.vertexAttributeDescriptionCount=static_cast<std::uint32_t>(vertex_attributes.size());inputs.pVertexAttributeDescriptions=vertex_attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology=topologies[primitive];
    VkPipelineViewportStateCreateInfo viewport{};viewport.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount=1;viewport.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode=raster_state.fill?VK_POLYGON_MODE_LINE:VK_POLYGON_MODE_FILL;
    raster.cullMode=raster_state.cull==1?VK_CULL_MODE_FRONT_BIT:raster_state.cull==2?VK_CULL_MODE_BACK_BIT:VK_CULL_MODE_NONE;
    raster.frontFace=raster_state.winding?VK_FRONT_FACE_COUNTER_CLOCKWISE:VK_FRONT_FACE_CLOCKWISE;raster.lineWidth=1;
    raster.depthClampEnable=raster_state.depth_clip==1;raster.depthBiasEnable=VK_TRUE;
    VkPipelineDepthStencilStateCreateInfo depth_info{};depth_info.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_info.depthTestEnable=depth_format!=0;depth_info.depthWriteEnable=depth.write;depth_info.depthCompareOp=static_cast<VkCompareOp>(depth.compare);
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
    VkDynamicState dynamic_values[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamic{};dynamic.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount=3;dynamic.pDynamicStates=dynamic_values;
    VkGraphicsPipelineCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount=2;info.pStages=stages;info.pVertexInputState=&inputs;info.pInputAssemblyState=&assembly;
    info.pViewportState=&viewport;info.pRasterizationState=&raster;info.pMultisampleState=&samples;info.pColorBlendState=&blending;
    info.pDepthStencilState=&depth_info;
    info.pDynamicState=&dynamic;info.layout=layout;info.renderPass=pass;
    VkPipeline result=VK_NULL_HANDLE;
    if(!vk_check(vkCreateGraphicsPipelines(context->device,VK_NULL_HANDLE,1,&info,nullptr,&result),"vkCreateGraphicsPipelines",error))return VK_NULL_HANDLE;
    variants[key]=result;return result;
}

bool CommandBuffer::begin_render(const std::shared_ptr<Texture>& target,bool clear,const std::array<double,4>& color,std::string& error,
    const DepthAttachment* depth,std::uint32_t mip,std::uint32_t slice) {
    std::lock_guard lock(impl_->context->mutex);
    if(!impl_->recording(error))return false;
    if(impl_->rendering||!target||target->impl_->context!=impl_->context)return impl_->fail("MEVK: invalid or overlapping render target",error);
    const auto& t=*target->impl_;
    if(t.aspect!=VK_IMAGE_ASPECT_COLOR_BIT||t.description.type==7||mip>=t.description.mips||slice>=t.layers||
       (t.description.usage&&!(t.description.usage&4u)))return impl_->fail("MEVK: unsupported color attachment mip/slice/type/usage",error);
    auto width=std::max(1u,t.width>>mip),height=std::max(1u,t.height>>mip);
    auto depth_texture=depth?depth->texture:nullptr;
    if(depth_texture) {
        const auto& d=*depth_texture->impl_;
        if(d.context!=impl_->context||d.aspect!=VK_IMAGE_ASPECT_DEPTH_BIT||depth->mip>=d.description.mips||depth->slice>=d.layers||
           std::max(1u,d.width>>depth->mip)!=width||std::max(1u,d.height>>depth->mip)!=height||
           (d.description.usage&&!(d.description.usage&4u))||!std::isfinite(depth->value)||depth->value<0||depth->value>1)
            return impl_->fail("MEVK: incompatible depth attachment, dimensions or clear value",error);
    }
    for(auto c:color)if(!std::isfinite(c))return impl_->fail("MEVK: nonfinite clear color",error);
    VkRenderPassBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass=t.pass;begin.framebuffer=t.framebuffer;begin.renderArea={{0,0},{width,height}};
    if(depth_texture||!begin.framebuffer||mip||slice) {
        impl_->framebuffers.emplace_back();auto& frame=impl_->framebuffers.back();
        if(!create_color_depth_pass(impl_->context->device,t.format,depth_texture?depth_texture->impl_->format:VK_FORMAT_UNDEFINED,frame.pass,error))return impl_->fail(error,error);
        auto view=[&](const Texture::Impl& texture,std::uint32_t level,std::uint32_t layer,VkImageView& handle) {
            VkImageViewCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;info.image=texture.image;
            info.viewType=VK_IMAGE_VIEW_TYPE_2D;info.format=texture.format;info.subresourceRange={texture.aspect,level,1,layer,1};
            return vk_check(vkCreateImageView(impl_->context->device,&info,nullptr,&handle),"vkCreateImageView(attachment)",error);
        };
        if(!view(t,mip,slice,frame.color)||(depth_texture&&!view(*depth_texture->impl_,depth->mip,depth->slice,frame.depth)))return impl_->fail(error,error);
        const VkImageView attachments[]={frame.color,frame.depth};
        VkFramebufferCreateInfo framebuffer{};framebuffer.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass=frame.pass;framebuffer.attachmentCount=depth_texture?2u:1u;framebuffer.pAttachments=attachments;
        framebuffer.width=width;framebuffer.height=height;framebuffer.layers=1;
        if(!vk_check(vkCreateFramebuffer(impl_->context->device,&framebuffer,nullptr,&frame.framebuffer),"vkCreateFramebuffer(attachments)",error))return impl_->fail(error,error);
        begin.renderPass=frame.pass;begin.framebuffer=frame.framebuffer;
    }
    impl_->textures.push_back(target);impl_->render_target=target;impl_->render_depth=depth_texture;
    impl_->render_width=width;impl_->render_height=height;
    if(depth_texture)impl_->textures.push_back(depth_texture);
    vkCmdBeginRenderPass(impl_->commands,&begin,VK_SUBPASS_CONTENTS_INLINE);impl_->rendering=true;
    if(clear){VkClearAttachment attachment{};attachment.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        for(unsigned i=0;i<4;++i)attachment.clearValue.color.float32[i]=static_cast<float>(color[i]);
        VkClearRect rectangle{begin.renderArea,0,1};vkCmdClearAttachments(impl_->commands,1,&attachment,1,&rectangle);}
    if(depth_texture&&depth->clear) {
        VkClearAttachment attachment{};attachment.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;attachment.clearValue.depthStencil.depth=static_cast<float>(depth->value);
        VkClearRect rectangle{begin.renderArea,0,1};vkCmdClearAttachments(impl_->commands,1,&attachment,1,&rectangle);
    }
    return true;
}
bool CommandBuffer::Impl::bind_resources(const std::vector<ShaderBinding>& reflected,VkDescriptorSetLayout set_layout,VkPipelineLayout layout,VkPipelineBindPoint point,const RenderBindings& bindings,std::string& error) {
    auto& limits=context->properties.limits;
    for(auto b:reflected) {
        if(b.texture) {
            auto t=bindings.textures[b.binding-32];
            if(!t||t->impl_->context!=context||t==render_target||t==render_depth)return fail("MEVK: missing or incompatible shader texture",error);
            if(!b.writable&&t->description().usage&&!(t->description().usage&1u))return fail("MEVK: shader texture lacks read usage",error);
            if(t->description().type!=b.texture_type)return fail("MEVK: shader texture dimension mismatch",error);
            if(b.writable&&!t->impl_->storage)return fail("MEVK: shader requires a writable texture format",error);
        } else if(b.sampler) {
            if(b.metal_index==UINT32_MAX)continue;
            auto s=bindings.samplers[b.binding-96];
            if(!s||s->impl_->context!=context)return fail("MEVK: missing or incompatible shader sampler",error);
        } else {
            auto buffer=bindings.buffers[b.binding];
            if(!buffer||buffer->impl_->context!=context||bindings.offsets[b.binding]>=buffer->size()||
               buffer->impl_->padded_size>limits.maxStorageBufferRange)return fail("MEVK: missing shader buffer or invalid offset/range",error);
        }
    }
    if(!descriptors) {
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,32768},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,65536},{VK_DESCRIPTOR_TYPE_SAMPLER,33792},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,32768}};
        VkDescriptorPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;pool.maxSets=1024;
        pool.poolSizeCount=4;pool.pPoolSizes=sizes;
        if(!vk_check(vkCreateDescriptorPool(context->device,&pool,nullptr,&descriptors),"vkCreateDescriptorPool",error))return fail(error,error);
    }
    VkDescriptorSet set=VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool=descriptors;allocation.descriptorSetCount=1;
    allocation.pSetLayouts=&set_layout;
    if(!vk_check(vkAllocateDescriptorSets(context->device,&allocation,&set),"vkAllocateDescriptorSets",error))return fail(error,error);
    std::vector<VkDescriptorBufferInfo> buffers;std::vector<VkDescriptorImageInfo> images;std::vector<VkWriteDescriptorSet> writes;
    auto resource_count=reflected.size();buffers.reserve(resource_count);images.reserve(resource_count);writes.reserve(resource_count);
    for(auto b:reflected) {
        VkWriteDescriptorSet write{};write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=set;
        write.dstBinding=b.binding;write.descriptorCount=1;
        if(b.texture) {
            auto t=bindings.textures[b.binding-32];textures.push_back(t);
            images.push_back({VK_NULL_HANDLE,b.writable?t->impl_->storage_view:t->impl_->view,VK_IMAGE_LAYOUT_GENERAL});
            write.descriptorType=b.writable?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.pImageInfo=&images.back();
        } else if(b.sampler) {
            VkSampler sampler=context->sampler;
            if(b.metal_index!=UINT32_MAX){auto s=bindings.samplers[b.binding-96];samplers.push_back(s);sampler=s->impl_->sampler;}
            images.push_back({sampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED});
            write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER;write.pImageInfo=&images.back();
        } else {
            auto bfr=bindings.buffers[b.binding];resources.push_back(bfr);
            buffers.push_back({bfr->impl_->buffer,0,bfr->impl_->padded_size});
            write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&buffers.back();
        }
        writes.push_back(write);
    }
    vkUpdateDescriptorSets(context->device,static_cast<std::uint32_t>(writes.size()),writes.data(),0,nullptr);
    vkCmdBindDescriptorSets(commands,point,layout,0,1,&set,0,nullptr);
    vkCmdPushConstants(commands,layout,point==VK_PIPELINE_BIND_POINT_COMPUTE?VK_SHADER_STAGE_COMPUTE_BIT:VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,128,bindings.offsets.data());
    return true;
}

bool CommandBuffer::draw(const std::shared_ptr<RenderPipeline>& pipeline,const RenderBindings& bindings,
    const Viewport& viewport,const Scissor& scissor,std::uint32_t primitive,std::uint32_t start,std::uint32_t count,
    std::uint32_t instances,std::uint32_t base_instance,const std::shared_ptr<Buffer>& indices,
    std::uint64_t index_offset,std::uint32_t index_type,std::int32_t base_vertex,std::string& error,const DepthState* depth_state,const RasterState* raster_state) {
    std::lock_guard lock(impl_->context->mutex);
    if(!impl_->recording(error))return false;
    if(!impl_->rendering||!pipeline||pipeline->impl_->context!=impl_->context||pipeline->impl_->pixel_format!=impl_->render_target->pixel_format()||
       pipeline->impl_->depth_format!=(impl_->render_depth?impl_->render_depth->pixel_format():0u))
        return impl_->fail("MEVK: draw requires a compatible render pipeline and active target",error);
    const DepthState depth=depth_state?*depth_state:DepthState{};const RasterState raster=raster_state?*raster_state:RasterState{};
    if(depth.compare>7||raster.cull>2||raster.winding>1||raster.fill>1||raster.depth_clip>1||
       (raster.fill&&!impl_->context->wireframe)||(raster.depth_clip&&!impl_->context->depth_clamp)||
       (raster.bias_clamp!=0&&!impl_->context->depth_bias_clamp)||!std::isfinite(raster.depth_bias)||!std::isfinite(raster.slope_scale)||!std::isfinite(raster.bias_clamp))
        return impl_->fail("MEVK: unsupported depth or raster state",error);
    auto& limits=impl_->context->properties.limits;
    if(!std::isfinite(viewport.x)||!std::isfinite(viewport.y)||!std::isfinite(viewport.width)||!std::isfinite(viewport.height)||
       !std::isfinite(viewport.znear)||!std::isfinite(viewport.zfar)||viewport.width<=0||viewport.height<=0||
       viewport.width>limits.maxViewportDimensions[0]||viewport.height>limits.maxViewportDimensions[1]||
       viewport.x<limits.viewportBoundsRange[0]||viewport.y<limits.viewportBoundsRange[0]||
       viewport.x+viewport.width>limits.viewportBoundsRange[1]||viewport.y+viewport.height>limits.viewportBoundsRange[1]||
       viewport.znear<0||viewport.zfar>1||viewport.znear>viewport.zfar||scissor.x>impl_->render_width||
       scissor.width>impl_->render_width-scissor.x||scissor.y>impl_->render_height||
       scissor.height>impl_->render_height-scissor.y)return impl_->fail("MEVK: invalid viewport or scissor",error);
    if(!count||!instances||!scissor.width||!scissor.height)return true;
    if(indices&&(indices->impl_->context!=impl_->context||index_type>1||index_offset>indices->size()||
        index_offset%(index_type?4u:2u)||std::uint64_t{count}*(index_type?4u:2u)>indices->size()-index_offset))
        return impl_->fail("MEVK: invalid index buffer range or type",error);
    for(auto binding:pipeline->impl_->vertex_buffers) {
        auto buffer=bindings.vertex_buffers[binding.binding];auto offset=bindings.vertex_offsets[binding.binding];
        std::uint64_t last=binding.inputRate==VK_VERTEX_INPUT_RATE_INSTANCE?std::uint64_t{base_instance}+instances-1:
            indices?0u:std::uint64_t{start}+count-1;
        auto bytes=last*binding.stride+pipeline->impl_->vertex_attribute_ends[binding.binding];
        if(!buffer||buffer->impl_->context!=impl_->context||offset>buffer->size()||bytes>buffer->size()-offset)
            return impl_->fail("MEVK: missing vertex buffer or attribute range outside the buffer",error);
    }
    auto variant=pipeline->impl_->pipeline(primitive,depth,raster,error);if(!variant)return impl_->fail(error,error);
    if(!impl_->bind_resources(pipeline->impl_->bindings,pipeline->impl_->descriptors,pipeline->impl_->layout,VK_PIPELINE_BIND_POINT_GRAPHICS,bindings,error))return false;
    impl_->pipelines.push_back(pipeline);
    vkCmdBindPipeline(impl_->commands,VK_PIPELINE_BIND_POINT_GRAPHICS,variant);
    for(auto binding:pipeline->impl_->vertex_buffers) {
        auto buffer=bindings.vertex_buffers[binding.binding];impl_->resources.push_back(buffer);
        const VkDeviceSize offset=bindings.vertex_offsets[binding.binding];
        vkCmdBindVertexBuffers(impl_->commands,binding.binding,1,&buffer->impl_->buffer,&offset);
    }
    vkCmdSetDepthBias(impl_->commands,raster.depth_bias,raster.bias_clamp,raster.slope_scale);
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
    vkCmdEndRenderPass(impl_->commands);impl_->rendering=false;impl_->render_target.reset();impl_->render_depth.reset();return true;
}
} // namespace mevk

#include "internal.h"
#include <cstring>
#include <limits>
#include <utility>
#include <algorithm>
#include <cmath>

namespace mevk {
VkFormat pixel_format(std::uint32_t format) {
    switch(format){case 10:return VK_FORMAT_R8_UNORM;case 13:return VK_FORMAT_R8_UINT;case 14:return VK_FORMAT_R8_SINT;
    case 20:return VK_FORMAT_R16_UNORM;case 23:return VK_FORMAT_R16_UINT;case 24:return VK_FORMAT_R16_SINT;case 25:return VK_FORMAT_R16_SFLOAT;
    case 30:return VK_FORMAT_R8G8_UNORM;case 33:return VK_FORMAT_R8G8_UINT;case 34:return VK_FORMAT_R8G8_SINT;
    case 53:return VK_FORMAT_R32_UINT;case 54:return VK_FORMAT_R32_SINT;case 55:return VK_FORMAT_R32_SFLOAT;
    case 60:return VK_FORMAT_R16G16_UNORM;case 63:return VK_FORMAT_R16G16_UINT;case 64:return VK_FORMAT_R16G16_SINT;case 65:return VK_FORMAT_R16G16_SFLOAT;
    case 73:return VK_FORMAT_R8G8B8A8_UINT;case 74:return VK_FORMAT_R8G8B8A8_SINT;
    case 103:return VK_FORMAT_R32G32_UINT;case 104:return VK_FORMAT_R32G32_SINT;case 105:return VK_FORMAT_R32G32_SFLOAT;
    case 110:return VK_FORMAT_R16G16B16A16_UNORM;case 113:return VK_FORMAT_R16G16B16A16_UINT;case 114:return VK_FORMAT_R16G16B16A16_SINT;case 115:return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 123:return VK_FORMAT_R32G32B32A32_UINT;case 124:return VK_FORMAT_R32G32B32A32_SINT;case 125:return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 70:return VK_FORMAT_R8G8B8A8_UNORM;
    case 71:return VK_FORMAT_R8G8B8A8_SRGB;case 80:return VK_FORMAT_B8G8R8A8_UNORM;
    case 81:return VK_FORMAT_B8G8R8A8_SRGB;
    case 90:return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 91:return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 92:return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 93:return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 250:return VK_FORMAT_D16_UNORM;
    case 252:return VK_FORMAT_D32_SFLOAT;
    case 260:return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 130:return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;case 131:return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case 132:return VK_FORMAT_BC2_UNORM_BLOCK;case 133:return VK_FORMAT_BC2_SRGB_BLOCK;
    case 134:return VK_FORMAT_BC3_UNORM_BLOCK;case 135:return VK_FORMAT_BC3_SRGB_BLOCK;
    case 140:return VK_FORMAT_BC4_UNORM_BLOCK;case 141:return VK_FORMAT_BC4_SNORM_BLOCK;
    case 142:return VK_FORMAT_BC5_UNORM_BLOCK;case 143:return VK_FORMAT_BC5_SNORM_BLOCK;
    case 150:return VK_FORMAT_BC6H_SFLOAT_BLOCK;case 151:return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case 152:return VK_FORMAT_BC7_UNORM_BLOCK;case 153:return VK_FORMAT_BC7_SRGB_BLOCK;
    default:return VK_FORMAT_UNDEFINED;}
}
Sampler::Impl::~Impl(){std::lock_guard lock(context->mutex);if(sampler)vkDestroySampler(context->device,sampler,nullptr);}
Sampler::Sampler(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
Sampler::~Sampler()=default;
std::shared_ptr<Sampler> Context::create_sampler(const SamplerDescription& d,std::string& error) {
    error.clear();
    if(d.min_filter>1||d.mag_filter>1||d.mip_filter>2||d.address_s>4||d.address_t>4||d.address_r>4||d.compare>7||
       !d.normalized||!std::isfinite(d.lod_min)||!std::isfinite(d.lod_max)||d.lod_min<0||d.lod_max<d.lod_min||
       !d.max_anisotropy||d.max_anisotropy>16||(!impl_->sampler_anisotropy&&d.max_anisotropy>1)) {
        error="MEVK: unsupported sampler filter/address/LOD/anisotropy";return nullptr;
    }
    constexpr VkSamplerAddressMode modes[]={VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_REPEAT,VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
    // Mirror clamp needs an optional Vulkan extension; do not silently use clamp.
    if(d.address_s==1||d.address_t==1||d.address_r==1){error="MEVK: mirror-clamp sampler unsupported";return nullptr;}
    auto sampler=std::make_unique<Sampler::Impl>();sampler->context=impl_;
    VkSamplerCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.minFilter=d.min_filter?VK_FILTER_LINEAR:VK_FILTER_NEAREST;info.magFilter=d.mag_filter?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
    info.mipmapMode=d.mip_filter==2?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU=modes[d.address_s];info.addressModeV=modes[d.address_t];info.addressModeW=modes[d.address_r];
    info.minLod=d.lod_min;info.maxLod=d.mip_filter?d.lod_max:0;
    info.anisotropyEnable=d.max_anisotropy>1;info.maxAnisotropy=std::min(static_cast<float>(d.max_anisotropy),impl_->properties.limits.maxSamplerAnisotropy);
    info.compareEnable=d.compare!=0;info.compareOp=static_cast<VkCompareOp>(d.compare);
    std::unique_lock lock(impl_->mutex);
    if(!vk_check(vkCreateSampler(impl_->device,&info,nullptr,&sampler->sampler),"vkCreateSampler",error))return nullptr;
    lock.unlock();return std::shared_ptr<Sampler>(new Sampler(std::move(sampler)));
}
bool create_color_pass(VkDevice device,VkFormat format,VkRenderPass& pass,std::string& error) {
    return create_color_depth_pass(device,format,VK_FORMAT_UNDEFINED,pass,error);
}
bool create_color_depth_pass(VkDevice device,VkFormat format,VkFormat depth_format,VkRenderPass& pass,std::string& error) {
    VkAttachmentDescription color{};color.format=format;color.samples=VK_SAMPLE_COUNT_1_BIT;
    color.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;color.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout=color.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference depth_reference{1,VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentDescription attachments[2]={color,color};attachments[1].format=depth_format;
    VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount=1;subpass.pColorAttachments=&reference;
    if(depth_format!=VK_FORMAT_UNDEFINED)subpass.pDepthStencilAttachment=&depth_reference;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;
    dependencies[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT;
    dependencies[0].dstStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    dependencies[0].srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;
    dependencies[0].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass=0;dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;dependencies[1].dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;dependencies[1].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount=depth_format==VK_FORMAT_UNDEFINED?1u:2u;info.pAttachments=attachments;info.subpassCount=1;info.pSubpasses=&subpass;
    info.dependencyCount=2;info.pDependencies=dependencies;
    return vk_check(vkCreateRenderPass(device,&info,nullptr,&pass),"vkCreateRenderPass",error);
}
Texture::Impl::~Impl() {
    std::lock_guard lock(context->mutex);
    if(framebuffer)vkDestroyFramebuffer(context->device,framebuffer,nullptr);
    if(pass)vkDestroyRenderPass(context->device,pass,nullptr);
    if(view)vkDestroyImageView(context->device,view,nullptr);
    if(storage_view)vkDestroyImageView(context->device,storage_view,nullptr);
    if(image)vkDestroyImage(context->device,image,nullptr);
    if(memory){vkFreeMemory(context->device,memory,nullptr);context->allocated_bytes-=memory_bytes;}
}
Texture::Texture(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
Texture::~Texture()=default;
std::uint32_t Texture::width()const{return impl_->width;}
std::uint32_t Texture::height()const{return impl_->height;}
std::uint32_t Texture::pixel_format()const{return impl_->pixel_format;}
std::uint32_t Texture::bytes_per_pixel()const{return impl_->block_width==1?impl_->bytes_per_pixel:0u;}
TextureDescription Texture::description()const{return impl_->description;}
bool Texture::supports_shader_write()const{return impl_->storage;}

std::shared_ptr<Texture> Context::create_texture(std::uint32_t width,std::uint32_t height,std::uint32_t format,std::string& error) {
    TextureDescription d;d.width=width;d.height=height;d.pixel_format=format;return create_texture(d,error);
}
std::shared_ptr<Texture> Context::create_texture(const TextureDescription& d,std::string& error) {
    auto width=d.width,height=d.height,format=d.pixel_format;
    const bool compressed=format>=130&&format<=153;
    error.clear();auto vkformat=mevk::pixel_format(format);
    auto limit=d.type==7?impl_->properties.limits.maxImageDimension3D:d.type==5?impl_->properties.limits.maxImageDimensionCube:impl_->properties.limits.maxImageDimension2D;
    auto layers=d.type==5?6u:d.array_length;
    if(!width||!height||!d.depth||width>limit||height>limit||d.depth>limit||!layers||layers>impl_->properties.limits.maxImageArrayLayers||
       (d.type!=2&&d.type!=3&&d.type!=5&&d.type!=7)||(d.type!=7&&d.depth!=1)||
       ((d.type==2||d.type==7||d.type==5)&&d.array_length!=1)||(d.type==5&&width!=height)||
       !d.mips||d.mips>1u+static_cast<unsigned>(std::floor(std::log2(std::max({width,height,d.depth}))))||vkformat==VK_FORMAT_UNDEFINED||
       (d.usage&~7u)||(compressed&&(!impl_->texture_compression_bc||d.type==7))){error="MEVK: unsupported texture size, type, mips, usage or pixel format";return nullptr;}
    VkFormatProperties properties{};vkGetPhysicalDeviceFormatProperties(impl_->physical_device,vkformat,&properties);
    auto usage=d.usage?d.usage:5u;
    VkFormatFeatureFlags required=0;
    if(usage&1u)required|=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if(usage&2u)required|=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    const bool depth_format=format==250||format==252||format==260;
    if(usage&4u)required|=depth_format?VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if((properties.optimalTilingFeatures&required)!=required){error="MEVK: texture format does not support requested usage";return nullptr;}
    auto texture=std::make_unique<Texture::Impl>();texture->context=impl_;texture->width=width;texture->height=height;
    texture->pixel_format=format;texture->format=vkformat;
    texture->bytes_per_pixel=format==250?2u:format==252?4u:format==260?8u:format<20?1u:format<40?2u:format<100?4u:format<120?8u:16u;
    if(compressed){texture->block_width=texture->block_height=4;texture->bytes_per_pixel=(format==130||format==131||format==140||format==141)?8u:16u;}
    texture->aspect=depth_format?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
    if(format==260)texture->aspect|=VK_IMAGE_ASPECT_STENCIL_BIT;
    texture->storage=(usage&2u)!=0;
    texture->description=d;texture->layers=layers;
    std::unique_lock lock(impl_->mutex);
    VkImageCreateInfo image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;image.imageType=d.type==7?VK_IMAGE_TYPE_3D:VK_IMAGE_TYPE_2D;
    if(d.type==5)image.flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    image.format=vkformat;image.extent={width,height,d.depth};image.mipLevels=d.mips;image.arrayLayers=layers;
    image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;
    image.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if(usage&1u)image.usage|=VK_IMAGE_USAGE_SAMPLED_BIT;
    if(usage&4u)image.usage|=depth_format?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if(texture->storage)image.usage|=VK_IMAGE_USAGE_STORAGE_BIT;
    image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;image.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    if(!vk_check(vkCreateImage(impl_->device,&image,nullptr,&texture->image),"vkCreateImage",error))return nullptr;
    VkMemoryRequirements requirements{};vkGetImageMemoryRequirements(impl_->device,texture->image,&requirements);
    std::uint32_t memory_type=impl_->memory_properties.memoryTypeCount;
    for(std::uint32_t i=0;i<impl_->memory_properties.memoryTypeCount;++i)
        if((requirements.memoryTypeBits&(1u<<i))&&(impl_->memory_properties.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){memory_type=i;break;}
    if(memory_type==impl_->memory_properties.memoryTypeCount){error="MEVK: no device-local texture memory";return nullptr;}
    VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=memory_type;
    if(!vk_check(vkAllocateMemory(impl_->device,&allocation,nullptr,&texture->memory),"vkAllocateMemory(image)",error))return nullptr;
    texture->memory_bytes=requirements.size;impl_->allocated_bytes+=texture->memory_bytes;
    if(!vk_check(vkBindImageMemory(impl_->device,texture->image,texture->memory,0),"vkBindImageMemory",error))return nullptr;
    VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image=texture->image;view.viewType=d.type==5?VK_IMAGE_VIEW_TYPE_CUBE:d.type==7?VK_IMAGE_VIEW_TYPE_3D:d.type==3?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;view.format=vkformat;
    view.subresourceRange={texture->aspect,0,d.mips,0,layers};
    // Sample the depth plane; render attachment views include both planes.
    if(format==260)view.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
    if(!vk_check(vkCreateImageView(impl_->device,&view,nullptr,&texture->view),"vkCreateImageView",error))return nullptr;
    if(texture->storage){view.subresourceRange.levelCount=1;if(!vk_check(vkCreateImageView(impl_->device,&view,nullptr,&texture->storage_view),"vkCreateImageView(storage)",error))return nullptr;}
    if(d.type==2&&d.mips==1&&(usage&4u)&&!depth_format) {
    if(!create_color_pass(impl_->device,vkformat,texture->pass,error))return nullptr;
    VkFramebufferCreateInfo framebuffer{};framebuffer.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer.renderPass=texture->pass;framebuffer.attachmentCount=1;framebuffer.pAttachments=&texture->view;
    framebuffer.width=width;framebuffer.height=height;framebuffer.layers=1;
    if(!vk_check(vkCreateFramebuffer(impl_->device,&framebuffer,nullptr,&texture->framebuffer),"vkCreateFramebuffer",error))return nullptr;
    }
    ++impl_->allocations.textures_created;
    lock.unlock();auto result=std::shared_ptr<Texture>(new Texture(std::move(texture)));
    auto command=create_command_buffer(error);if(!command)return nullptr;
    {
        std::lock_guard recording_lock(impl_->mutex);
        VkImageMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=result->impl_->image;barrier.subresourceRange={result->impl_->aspect,0,d.mips,0,layers};
        barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(command->impl_->commands,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    }
    if(!command->commit(error)||!command->wait_until_completed(error))return nullptr;
    return result;
}

bool Texture::replace(std::uint32_t x,std::uint32_t y,std::uint32_t width,std::uint32_t height,
                      const void* bytes,std::uint64_t row_bytes,std::string& error) {
    return replace(x,y,0,width,height,1,0,0,bytes,row_bytes,row_bytes*height,error);
}
bool Texture::replace(std::uint32_t x,std::uint32_t y,std::uint32_t z,std::uint32_t width,std::uint32_t height,std::uint32_t depth,
                      std::uint32_t mip,std::uint32_t slice,const void* bytes,std::uint64_t row_bytes,std::uint64_t image_bytes,std::string& error) {
    error.clear();
    if(impl_->pixel_format==260){error="MEVK: packed depth/stencil CPU uploads are unsupported";return false;}
    auto stride=((std::uint64_t{width}+impl_->block_width-1)/impl_->block_width)*impl_->bytes_per_pixel;
    auto rows=(height+impl_->block_height-1)/impl_->block_height;
    if(mip>=impl_->description.mips||slice>=impl_->layers){error="MEVK: invalid texture mip or slice";return false;}
    auto w=std::max(1u,impl_->width>>mip),h=std::max(1u,impl_->height>>mip),d=std::max(1u,impl_->description.depth>>mip);
    if(!bytes||!width||!height||!depth||x>w||width>w-x||y>h||height>h-y||z>d||depth>d-z||
       x%impl_->block_width||y%impl_->block_height||
       (width%impl_->block_width&&x+width!=w)||(height%impl_->block_height&&y+height!=h)||
       row_bytes<stride||row_bytes>std::numeric_limits<std::size_t>::max()/rows||
       (depth>1&&(image_bytes<row_bytes*rows||image_bytes>std::numeric_limits<std::size_t>::max()/depth))){error="MEVK: invalid texture upload range or stride";return false;}
    Context context(impl_->context);auto staging=context.create_buffer(stride*rows*depth,error);if(!staging)return false;
    for(std::uint32_t plane=0;plane<depth;++plane)for(std::uint32_t row=0;row<rows;++row)std::memcpy(static_cast<std::uint8_t*>(staging->contents())+(plane*rows+row)*stride,
        static_cast<const std::uint8_t*>(bytes)+plane*image_bytes+row*row_bytes,static_cast<std::size_t>(stride));
    auto command=context.create_command_buffer(error);if(!command)return false;
    {
        std::lock_guard lock(impl_->context->mutex);
        VkMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command->impl_->commands,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        VkBufferImageCopy region{};region.imageSubresource={impl_->aspect,mip,slice,1};
        region.imageOffset={static_cast<std::int32_t>(x),static_cast<std::int32_t>(y),static_cast<std::int32_t>(z)};region.imageExtent={width,height,depth};
        vkCmdCopyBufferToImage(command->impl_->commands,staging->impl_->buffer,impl_->image,VK_IMAGE_LAYOUT_GENERAL,1,&region);
        command->impl_->resources.push_back(staging);
    }
    return command->commit(error)&&command->wait_until_completed(error);
}
bool Texture::read_pixels(std::vector<std::uint8_t>& pixels,std::string& error) {
    error.clear();pixels.clear();auto size=((std::uint64_t{impl_->width}+impl_->block_width-1)/impl_->block_width)*
        ((impl_->height+impl_->block_height-1)/impl_->block_height)*impl_->description.depth*impl_->layers*impl_->bytes_per_pixel;
    if(impl_->pixel_format==260){error="MEVK: packed depth/stencil CPU readback is unsupported";return false;}
    Context context(impl_->context);auto staging=context.create_buffer(size,error);if(!staging)return false;
    auto command=context.create_command_buffer(error);if(!command)return false;
    {
        std::lock_guard lock(impl_->context->mutex);
        command->impl_->transfer_barrier();
        VkBufferImageCopy region{};region.imageSubresource={impl_->aspect,0,0,impl_->layers};region.imageExtent={impl_->width,impl_->height,impl_->description.depth};
        vkCmdCopyImageToBuffer(command->impl_->commands,impl_->image,VK_IMAGE_LAYOUT_GENERAL,staging->impl_->buffer,1,&region);
        command->impl_->resources.push_back(staging);
    }
    if(!command->commit(error)||!command->wait_until_completed(error))return false;
    pixels.resize(static_cast<std::size_t>(size));std::memcpy(pixels.data(),staging->contents(),pixels.size());return true;
}
bool CommandBuffer::copy_buffer_to_texture(const std::shared_ptr<Buffer>& source,
    std::uint64_t offset,std::uint64_t row_bytes,std::uint64_t image_bytes,
    const std::array<std::uint32_t,3>& size,const std::shared_ptr<Texture>& destination,
    std::uint32_t slice,std::uint32_t mip,const std::array<std::uint32_t,3>& origin,std::string& error) {
    std::lock_guard lock(impl_->context->mutex);
    if(!impl_->recording(error))return false;
    if(impl_->rendering||!source||!destination||source->impl_->context!=impl_->context||destination->impl_->context!=impl_->context)
        return impl_->fail("MEVK: buffer-to-texture copy requires compatible resources outside a render pass",error);
    auto& t=*destination->impl_;
    if(t.pixel_format==260)return impl_->fail("MEVK: packed depth/stencil buffer uploads are unsupported",error);
    if(mip>=t.description.mips||slice>=t.layers)return impl_->fail("MEVK: invalid buffer-to-texture mip/slice",error);
    const std::array<std::uint32_t,3> extent={std::max(1u,t.width>>mip),std::max(1u,t.height>>mip),std::max(1u,t.description.depth>>mip)};
    for(unsigned i=0;i<3;++i)if(origin[i]>extent[i]||size[i]>extent[i]-origin[i])
        return impl_->fail("MEVK: buffer-to-texture region is outside the texture",error);
    if(!size[0]||!size[1]||!size[2])return true;
    if(origin[0]%t.block_width||origin[1]%t.block_height||
       (size[0]%t.block_width&&origin[0]+size[0]!=extent[0])||(size[1]%t.block_height&&origin[1]+size[1]!=extent[1]))
        return impl_->fail("MEVK: compressed texture copy region must align to blocks or the mip edge",error);
    auto rows=(size[1]+t.block_height-1)/t.block_height;
    auto packed_row=((std::uint64_t{size[0]}+t.block_width-1)/t.block_width)*t.bytes_per_pixel;
    if(!row_bytes)row_bytes=packed_row;
    if(row_bytes<packed_row||row_bytes%t.bytes_per_pixel||row_bytes/t.bytes_per_pixel>UINT32_MAX/t.block_width||
       row_bytes>UINT64_MAX/rows||offset%4||offset%t.bytes_per_pixel)
        return impl_->fail("MEVK: invalid buffer-to-texture row stride or alignment",error);
    auto packed_image=row_bytes*rows;
    if(!image_bytes)image_bytes=packed_image;
    if(size[2]>1&&(image_bytes<packed_image||image_bytes%row_bytes||image_bytes/row_bytes>UINT32_MAX/t.block_height||image_bytes>UINT64_MAX/(size[2]-1)))
        return impl_->fail("MEVK: invalid buffer-to-texture image stride",error);
    auto planes=std::uint64_t{size[2]-1}*image_bytes;
    auto tail=row_bytes*(rows-1)+packed_row;
    if(planes>UINT64_MAX-tail||offset>source->size()||planes+tail>source->size()-offset)
        return impl_->fail("MEVK: buffer-to-texture source range is outside the buffer",error);
    VkBufferImageCopy region{};region.bufferOffset=offset;
    region.bufferRowLength=static_cast<std::uint32_t>(row_bytes/t.bytes_per_pixel)*t.block_width;
    region.bufferImageHeight=size[2]>1?static_cast<std::uint32_t>(image_bytes/row_bytes)*t.block_height:0;
    region.imageSubresource={t.aspect,mip,slice,1};
    region.imageOffset={static_cast<std::int32_t>(origin[0]),static_cast<std::int32_t>(origin[1]),static_cast<std::int32_t>(origin[2])};
    region.imageExtent={size[0],size[1],size[2]};
    impl_->resources.push_back(source);impl_->textures.push_back(destination);impl_->transfer_barrier();
    vkCmdCopyBufferToImage(impl_->commands,source->impl_->buffer,t.image,VK_IMAGE_LAYOUT_GENERAL,1,&region);
    return true;
}
} // namespace mevk

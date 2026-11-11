#include "internal.h"
#include <cstring>
#include <limits>
#include <utility>

namespace mevk {
VkFormat pixel_format(std::uint32_t format) {
    switch(format){case 10:return VK_FORMAT_R8_UNORM;case 70:return VK_FORMAT_R8G8B8A8_UNORM;
    case 71:return VK_FORMAT_R8G8B8A8_SRGB;case 80:return VK_FORMAT_B8G8R8A8_UNORM;
    case 81:return VK_FORMAT_B8G8R8A8_SRGB;default:return VK_FORMAT_UNDEFINED;}
}
bool create_color_pass(VkDevice device,VkFormat format,VkRenderPass& pass,std::string& error) {
    VkAttachmentDescription color{};color.format=format;color.samples=VK_SAMPLE_COUNT_1_BIT;
    color.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;color.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout=color.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount=1;subpass.pColorAttachments=&reference;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;
    dependencies[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT;
    dependencies[0].dstStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    dependencies[0].srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;
    dependencies[0].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass=0;dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;dependencies[1].dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;dependencies[1].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount=1;info.pAttachments=&color;info.subpassCount=1;info.pSubpasses=&subpass;
    info.dependencyCount=2;info.pDependencies=dependencies;
    return vk_check(vkCreateRenderPass(device,&info,nullptr,&pass),"vkCreateRenderPass",error);
}
Texture::Impl::~Impl() {
    std::lock_guard lock(context->mutex);
    if(framebuffer)vkDestroyFramebuffer(context->device,framebuffer,nullptr);
    if(pass)vkDestroyRenderPass(context->device,pass,nullptr);
    if(view)vkDestroyImageView(context->device,view,nullptr);
    if(image)vkDestroyImage(context->device,image,nullptr);
    if(memory)vkFreeMemory(context->device,memory,nullptr);
}
Texture::Texture(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
Texture::~Texture()=default;
std::uint32_t Texture::width()const{return impl_->width;}
std::uint32_t Texture::height()const{return impl_->height;}
std::uint32_t Texture::pixel_format()const{return impl_->pixel_format;}

std::shared_ptr<Texture> Context::create_texture(std::uint32_t width,std::uint32_t height,std::uint32_t format,std::string& error) {
    error.clear();auto vkformat=mevk::pixel_format(format);
    if(!width||!height||width>impl_->properties.limits.maxImageDimension2D||height>impl_->properties.limits.maxImageDimension2D||vkformat==VK_FORMAT_UNDEFINED){error="MEVK: unsupported texture size or pixel format";return nullptr;}
    VkFormatProperties properties{};vkGetPhysicalDeviceFormatProperties(impl_->physical_device,vkformat,&properties);
    constexpr VkFormatFeatureFlags required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if((properties.optimalTilingFeatures&required)!=required){error="MEVK: texture format does not support rendering and linear sampling";return nullptr;}
    auto texture=std::make_unique<Texture::Impl>();texture->context=impl_;texture->width=width;texture->height=height;
    texture->pixel_format=format;texture->format=vkformat;texture->bytes_per_pixel=format==10?1u:4u;
    std::unique_lock lock(impl_->mutex);
    VkImageCreateInfo image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;image.imageType=VK_IMAGE_TYPE_2D;
    image.format=vkformat;image.extent={width,height,1};image.mipLevels=1;image.arrayLayers=1;
    image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;
    image.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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
    if(!vk_check(vkBindImageMemory(impl_->device,texture->image,texture->memory,0),"vkBindImageMemory",error))return nullptr;
    VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image=texture->image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=vkformat;
    view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if(!vk_check(vkCreateImageView(impl_->device,&view,nullptr,&texture->view),"vkCreateImageView",error))return nullptr;
    if(!create_color_pass(impl_->device,vkformat,texture->pass,error))return nullptr;
    VkFramebufferCreateInfo framebuffer{};framebuffer.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer.renderPass=texture->pass;framebuffer.attachmentCount=1;framebuffer.pAttachments=&texture->view;
    framebuffer.width=width;framebuffer.height=height;framebuffer.layers=1;
    if(!vk_check(vkCreateFramebuffer(impl_->device,&framebuffer,nullptr,&texture->framebuffer),"vkCreateFramebuffer",error))return nullptr;
    ++impl_->allocations.textures_created;
    lock.unlock();auto result=std::shared_ptr<Texture>(new Texture(std::move(texture)));
    auto command=create_command_buffer(error);if(!command)return nullptr;
    {
        std::lock_guard recording_lock(impl_->mutex);
        VkImageMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=result->impl_->image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(command->impl_->commands,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    }
    if(!command->commit(error)||!command->wait_until_completed(error))return nullptr;
    return result;
}

bool Texture::replace(std::uint32_t x,std::uint32_t y,std::uint32_t width,std::uint32_t height,
                      const void* bytes,std::uint64_t row_bytes,std::string& error) {
    error.clear();auto stride=std::uint64_t{width}*impl_->bytes_per_pixel;
    if(!bytes||!width||!height||x>impl_->width||width>impl_->width-x||y>impl_->height||height>impl_->height-y||
       row_bytes<stride||row_bytes>std::numeric_limits<std::size_t>::max()/height){error="MEVK: invalid texture upload range or stride";return false;}
    Context context(impl_->context);auto staging=context.create_buffer(stride*height,error);if(!staging)return false;
    for(std::uint32_t row=0;row<height;++row)std::memcpy(static_cast<std::uint8_t*>(staging->contents())+row*stride,
        static_cast<const std::uint8_t*>(bytes)+row*row_bytes,static_cast<std::size_t>(stride));
    auto command=context.create_command_buffer(error);if(!command)return false;
    {
        std::lock_guard lock(impl_->context->mutex);
        VkMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command->impl_->commands,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        region.imageOffset={static_cast<std::int32_t>(x),static_cast<std::int32_t>(y),0};region.imageExtent={width,height,1};
        vkCmdCopyBufferToImage(command->impl_->commands,staging->impl_->buffer,impl_->image,VK_IMAGE_LAYOUT_GENERAL,1,&region);
        command->impl_->resources.push_back(staging);
    }
    return command->commit(error)&&command->wait_until_completed(error);
}
bool Texture::read_pixels(std::vector<std::uint8_t>& pixels,std::string& error) {
    error.clear();pixels.clear();auto size=std::uint64_t{impl_->width}*impl_->height*impl_->bytes_per_pixel;
    Context context(impl_->context);auto staging=context.create_buffer(size,error);if(!staging)return false;
    auto command=context.create_command_buffer(error);if(!command)return false;
    {
        std::lock_guard lock(impl_->context->mutex);
        command->impl_->transfer_barrier();
        VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={impl_->width,impl_->height,1};
        vkCmdCopyImageToBuffer(command->impl_->commands,impl_->image,VK_IMAGE_LAYOUT_GENERAL,staging->impl_->buffer,1,&region);
        command->impl_->resources.push_back(staging);
    }
    if(!command->commit(error)||!command->wait_until_completed(error))return false;
    pixels.resize(static_cast<std::size_t>(size));std::memcpy(pixels.data(),staging->contents(),pixels.size());return true;
}
} // namespace mevk

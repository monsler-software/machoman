#pragma once

#include "mevk/buffer.h"
#include "mevk/command_buffer.h"
#include "mevk/context.h"
#include "mevk/render.h"

#include <vulkan/vulkan.h>

#include <mutex>
#include <vector>
#include <map>

namespace mevk {

struct BufferAllocation {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    std::uint64_t capacity = 0, memory_bytes = 0;
};
struct CommandAllocation {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
};

inline bool vk_check(VkResult result, const char* operation, std::string& error) {
    if (result == VK_SUCCESS) return true;
    error = std::string(operation) + " failed (VkResult "
          + std::to_string(static_cast<int>(result)) + ")";
    return false;
}

struct Context::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkSampler sampler=VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    bool sampler_anisotropy=false;
    bool texture_compression_bc=false;
    bool depth_clamp=false,depth_bias_clamp=false,wireframe=false;
    bool storage_write_without_format=false;
    DeviceInfo info;
    bool wayland_surface = false, swapchain = false;
    std::vector<BufferAllocation> buffer_cache;
    std::uint64_t buffer_cache_bytes = 0;
    std::vector<CommandAllocation> command_cache;
    AllocationStats allocations;
    std::uint64_t allocated_bytes=0;
    // Also protects command-buffer state and allocation/free from the shared
    // command pool. Host access to mapped memory remains the caller's concern.
    mutable std::mutex mutex;
    ~Impl();
};

struct Buffer::Impl {
    std::shared_ptr<Context::Impl> context;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    std::uint64_t size = 0;
    std::uint64_t padded_size = 0;
    std::uint64_t capacity = 0, memory_bytes = 0;
    ~Impl();
};

struct CommandBuffer::Impl {
    std::shared_ptr<Context::Impl> context;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
    CommandBufferStatus state = CommandBufferStatus::recording;
    std::string diagnostic;
    std::vector<std::shared_ptr<Buffer>> resources;
    std::vector<std::shared_ptr<Texture>> textures;
    std::vector<std::shared_ptr<Sampler>> samplers;
    std::vector<std::shared_ptr<RenderPipeline>> pipelines;
    std::vector<std::shared_ptr<ComputePipeline>> compute_pipelines;
    std::shared_ptr<Texture> render_target;
    std::shared_ptr<Texture> render_depth;
    std::uint32_t render_width=0,render_height=0;
    struct Framebuffer {VkFramebuffer framebuffer=VK_NULL_HANDLE;VkRenderPass pass=VK_NULL_HANDLE;VkImageView color=VK_NULL_HANDLE,depth=VK_NULL_HANDLE;};
    std::vector<Framebuffer> framebuffers;
    VkDescriptorPool descriptors=VK_NULL_HANDLE;
    bool rendering=false;
    ~Impl();

    bool fail(const std::string& message, std::string& error);
    bool recording(std::string& error);
    void transfer_barrier();
    void host_barrier();
    bool bind_resources(const std::vector<ShaderBinding>&,VkDescriptorSetLayout,VkPipelineLayout,VkPipelineBindPoint,const RenderBindings&,std::string& error);
};

struct Texture::Impl {
    std::shared_ptr<Context::Impl> context;
    VkImage image=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    VkImageView view=VK_NULL_HANDLE;
    VkImageView storage_view=VK_NULL_HANDLE;
    bool storage=false;
    VkRenderPass pass=VK_NULL_HANDLE;
    VkFramebuffer framebuffer=VK_NULL_HANDLE;
    std::uint32_t width=0,height=0,pixel_format=0,bytes_per_pixel=4;
    std::uint32_t block_width=1,block_height=1;
    TextureDescription description;
    std::uint32_t layers=1;
    std::uint64_t memory_bytes=0;
    VkFormat format=VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT;
    ~Impl();
};
struct Sampler::Impl {
    std::shared_ptr<Context::Impl> context;
    VkSampler sampler=VK_NULL_HANDLE;
    ~Impl();
};
struct ComputePipeline::Impl {
    std::shared_ptr<Context::Impl> context;
    VkShaderModule shader=VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptors=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    std::vector<ShaderBinding> bindings;
    std::map<std::array<std::uint32_t,3>,VkPipeline> variants;
    ~Impl();
    VkPipeline pipeline(const std::array<std::uint32_t,3>&,std::string& error);
};
struct RenderPipeline::Impl {
    std::shared_ptr<Context::Impl> context;
    VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;
    VkRenderPass pass=VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptors=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    std::uint32_t pixel_format=0;
    std::uint32_t depth_format=0;
    std::vector<VkVertexInputBindingDescription> vertex_buffers;
    std::vector<VkVertexInputAttributeDescription> vertex_attributes;
    std::array<std::uint32_t,32> vertex_attribute_ends{};
    BlendState blend;
    std::vector<ShaderBinding> bindings;
    std::map<std::array<std::uint32_t,7>,VkPipeline> variants;
    ~Impl();
    VkPipeline pipeline(std::uint32_t primitive,const DepthState&,const RasterState&,std::string& error);
};
VkFormat pixel_format(std::uint32_t format);
bool create_color_pass(VkDevice device,VkFormat format,VkRenderPass& pass,std::string& error);
bool create_color_depth_pass(VkDevice device,VkFormat color,VkFormat depth,VkRenderPass& pass,std::string& error);

} // namespace mevk

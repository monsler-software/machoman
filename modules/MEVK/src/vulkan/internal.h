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
    DeviceInfo info;
    bool wayland_surface = false, swapchain = false;
    std::vector<BufferAllocation> buffer_cache;
    std::uint64_t buffer_cache_bytes = 0;
    std::vector<CommandAllocation> command_cache;
    AllocationStats allocations;
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
    std::vector<std::shared_ptr<RenderPipeline>> pipelines;
    std::shared_ptr<Texture> render_target;
    VkDescriptorPool descriptors=VK_NULL_HANDLE;
    bool rendering=false;
    ~Impl();

    bool fail(const std::string& message, std::string& error);
    bool recording(std::string& error);
    void transfer_barrier();
    void host_barrier();
};

struct Texture::Impl {
    std::shared_ptr<Context::Impl> context;
    VkImage image=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    VkImageView view=VK_NULL_HANDLE;
    VkRenderPass pass=VK_NULL_HANDLE;
    VkFramebuffer framebuffer=VK_NULL_HANDLE;
    std::uint32_t width=0,height=0,pixel_format=0,bytes_per_pixel=4;
    VkFormat format=VK_FORMAT_UNDEFINED;
    ~Impl();
};
struct RenderPipeline::Impl {
    std::shared_ptr<Context::Impl> context;
    VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;
    VkRenderPass pass=VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptors=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    std::uint32_t pixel_format=0;
    BlendState blend;
    std::vector<ShaderBinding> bindings;
    std::map<std::uint32_t,VkPipeline> variants;
    ~Impl();
    VkPipeline pipeline(std::uint32_t primitive,std::string& error);
};
VkFormat pixel_format(std::uint32_t format);
bool create_color_pass(VkDevice device,VkFormat format,VkRenderPass& pass,std::string& error);

} // namespace mevk

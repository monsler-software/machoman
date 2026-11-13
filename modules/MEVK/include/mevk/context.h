#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace mevk {

class Buffer;
class CommandBuffer;
class Texture;
class Sampler;
struct TextureDescription;
struct SamplerDescription;
class RenderPipeline;
class ComputePipeline;
class WaylandPresenter;
struct ShaderFunction;
struct BlendState;
struct VertexInput;

struct DeviceInfo {
    std::string name;
    std::uint32_t api_version = 0; // Vulkan's packed version.
    std::uint32_t queue_family = 0;
};
struct AllocationStats {
    std::uint64_t buffers_created = 0, buffers_reused = 0;
    std::uint64_t commands_created = 0, commands_reused = 0;
    std::uint64_t textures_created = 0;
};

// Host-side Vulkan lifetime. No Darwin ABI or Objective-C types cross this API.
// Queue and command-pool access is serialized internally. Resources retain
// the Vulkan device even if this facade is destroyed before them.
class Context final {
public:
    // Returns nullptr with a diagnostic on Vulkan initialization failure.
    // The context owns its instance, logical device, queue and command pool.
    static std::unique_ptr<Context> create(std::string& error);
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    const DeviceInfo& device_info() const;
    AllocationStats allocation_stats() const;
    std::uint64_t allocated_size() const;
    std::uint64_t recommended_working_set_size() const;
    bool wait_idle(std::string& error);
    std::shared_ptr<Buffer> create_buffer(std::uint64_t size, std::string& error);
    std::shared_ptr<CommandBuffer> create_command_buffer(std::string& error);
    std::shared_ptr<Texture> create_texture(std::uint32_t width,std::uint32_t height,
                                          std::uint32_t pixel_format,std::string& error);
    std::shared_ptr<Texture> create_texture(const TextureDescription&,std::string& error);
    std::shared_ptr<Sampler> create_sampler(const SamplerDescription&,std::string& error);
    std::shared_ptr<ComputePipeline> create_compute_pipeline(const ShaderFunction&,std::string& error);
    std::shared_ptr<RenderPipeline> create_render_pipeline(const ShaderFunction& vertex,
        const ShaderFunction& fragment,std::uint32_t pixel_format,const BlendState& blend,std::string& error,
        const VertexInput* vertices=nullptr,std::uint32_t depth_format=0);

private:
    friend class Buffer;
    friend class CommandBuffer;
    friend class Texture;
    friend class Sampler;
    friend class RenderPipeline;
    friend class ComputePipeline;
    friend class WaylandPresenter;
    struct Impl;
    explicit Context(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

} // namespace mevk

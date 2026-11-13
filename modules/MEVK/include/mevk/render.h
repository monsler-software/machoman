#pragma once
#include "mevk/shader.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mevk {
class Context;
class CommandBuffer;
class Buffer;
class WaylandPresenter;
struct TextureDescription {
    std::uint32_t width=1,height=1,depth=1,pixel_format=70,type=2,mips=1,array_length=1;
    std::uint32_t usage=5; // Metal shader read=1, shader write=2, render target=4.
};
struct VertexAttribute {std::uint32_t location=0,buffer=0,format=0,offset=0;};
struct VertexBufferLayout {std::uint32_t buffer=0,stride=0,step_function=1,step_rate=1;};
struct VertexInput {std::vector<VertexAttribute> attributes;std::vector<VertexBufferLayout> buffers;};
struct DepthState {std::uint32_t compare=7;bool write=false;};
struct RasterState {
    std::uint32_t cull=0,winding=0,fill=0,depth_clip=0;
    float depth_bias=0,slope_scale=0,bias_clamp=0;
};
struct SamplerDescription {
    std::uint32_t min_filter=0,mag_filter=0,mip_filter=0,address_s=0,address_t=0,address_r=0;
    std::uint32_t max_anisotropy=1,compare=0;
    float lod_min=0,lod_max=1000;
    bool normalized=true;
};
class Sampler final {
public:
    ~Sampler();
private:
    friend class Context; friend class CommandBuffer;
    struct Impl;
    explicit Sampler(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
class ComputePipeline final {
public:
    ~ComputePipeline();
private:
    friend class Context; friend class CommandBuffer;
    struct Impl;
    explicit ComputePipeline(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
class Texture final {
public:
    ~Texture();
    Texture(const Texture&)=delete;
    Texture& operator=(const Texture&)=delete;
    std::uint32_t width() const;
    std::uint32_t height() const;
    std::uint32_t pixel_format() const;
    std::uint32_t bytes_per_pixel() const;
    TextureDescription description() const;
    bool supports_shader_write() const;
    bool replace(std::uint32_t x,std::uint32_t y,std::uint32_t width,std::uint32_t height,
                 const void* bytes,std::uint64_t row_bytes,std::string& error);
    bool replace(std::uint32_t x,std::uint32_t y,std::uint32_t z,
                 std::uint32_t width,std::uint32_t height,std::uint32_t depth,
                 std::uint32_t mip,std::uint32_t slice,const void* bytes,
                 std::uint64_t row_bytes,std::uint64_t image_bytes,std::string& error);
    bool read_pixels(std::vector<std::uint8_t>& pixels,std::string& error);
private:
    friend class Context; friend class CommandBuffer;
    friend class WaylandPresenter;
    struct Impl;
    explicit Texture(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
struct BlendState {
    bool enabled=false;
    std::uint32_t source_rgb=1,destination_rgb=0,source_alpha=1,destination_alpha=0;
    std::uint32_t rgb_operation=0,alpha_operation=0,write_mask=15;
};
struct DepthAttachment {
    std::shared_ptr<Texture> texture;
    std::uint32_t mip=0,slice=0;
    bool clear=false;
    double value=1;
};
class RenderPipeline final {
public:
    ~RenderPipeline();
    RenderPipeline(const RenderPipeline&)=delete;
    RenderPipeline& operator=(const RenderPipeline&)=delete;
private:
    friend class Context; friend class CommandBuffer;
    struct Impl;
    explicit RenderPipeline(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
struct RenderBindings {
    // Binding 0..15: vertex buffers; 16..31: fragment buffers.
    std::array<std::shared_ptr<Buffer>,32> buffers;
    std::array<std::uint32_t,32> offsets{};
    std::array<std::shared_ptr<Texture>,64> textures;
    std::array<std::shared_ptr<Sampler>,32> samplers;
    std::array<std::shared_ptr<Buffer>,32> vertex_buffers;
    std::array<std::uint64_t,32> vertex_offsets{};
};
struct Viewport { double x=0,y=0,width=0,height=0,znear=0,zfar=1; };
struct Scissor { std::uint32_t x=0,y=0,width=0,height=0; };
} // namespace mevk

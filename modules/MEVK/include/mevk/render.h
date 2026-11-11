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
class Texture final {
public:
    ~Texture();
    Texture(const Texture&)=delete;
    Texture& operator=(const Texture&)=delete;
    std::uint32_t width() const;
    std::uint32_t height() const;
    std::uint32_t pixel_format() const;
    bool replace(std::uint32_t x,std::uint32_t y,std::uint32_t width,std::uint32_t height,
                 const void* bytes,std::uint64_t row_bytes,std::string& error);
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
    std::array<std::shared_ptr<Texture>,32> textures;
};
struct Viewport { double x=0,y=0,width=0,height=0,znear=0,zfar=1; };
struct Scissor { std::uint32_t x=0,y=0,width=0,height=0; };
} // namespace mevk

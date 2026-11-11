#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <array>

namespace mevk {

class Buffer;
class Context;
class Texture;
class RenderPipeline;
struct RenderBindings;
struct Viewport;
struct Scissor;

// Values match the corresponding MTLCommandBufferStatus values.
enum class CommandBufferStatus : std::uint32_t {
    recording = 0,
    committed = 2,
    completed = 4,
    error = 5,
};

// One-shot transfer command buffer. Commands retain their resources and
// preserve order, including transfers across submissions on this context.
// Invalid recording poisons the entire command buffer: commit submits nothing.
class CommandBuffer final {
public:
    ~CommandBuffer();
    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;

    // Initial Vulkan 1.0 path: nonempty offsets and sizes must be multiples of
    // four. Same-buffer copies are allowed only for nonoverlapping ranges.
    bool copy(const std::shared_ptr<Buffer>& source, std::uint64_t source_offset,
              const std::shared_ptr<Buffer>& destination, std::uint64_t destination_offset,
              std::uint64_t size, std::string& error);
    bool fill(const std::shared_ptr<Buffer>& buffer, std::uint64_t offset,
              std::uint64_t size, std::uint8_t value, std::string& error);
    bool commit(std::string& error);
    // Requires commit; waits on this command buffer's fence, not device idle.
    bool wait_until_completed(std::string& error);
    CommandBufferStatus status();
    std::string error_message() const;
    bool begin_render(const std::shared_ptr<Texture>& target,bool clear,
                      const std::array<double,4>& clear_color,std::string& error);
    bool draw(const std::shared_ptr<RenderPipeline>& pipeline,const RenderBindings& bindings,
              const Viewport& viewport,const Scissor& scissor,std::uint32_t primitive,
              std::uint32_t start,std::uint32_t count,std::uint32_t instances,std::uint32_t base_instance,
              const std::shared_ptr<Buffer>& indices,std::uint64_t index_offset,
              std::uint32_t index_type,std::int32_t base_vertex,std::string& error);
    bool end_render(std::string& error);

private:
    friend class Context;
    friend class Texture;
    struct Impl;
    explicit CommandBuffer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace mevk

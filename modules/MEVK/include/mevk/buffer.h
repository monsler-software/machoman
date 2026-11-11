#pragma once

#include <cstdint>
#include <memory>

namespace mevk {

class Context;
class CommandBuffer;
class Texture;

// Persistently mapped, host-visible and host-coherent Vulkan memory.
// The caller must wait for GPU completion before reading or modifying data
// used by submitted commands. No explicit flush/invalidate is required.
class Buffer final {
public:
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    std::uint64_t size() const;
    void* contents();
    const void* contents() const;

private:
    friend class Context;
    friend class CommandBuffer;
    friend class Texture;
    struct Impl;
    explicit Buffer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace mevk

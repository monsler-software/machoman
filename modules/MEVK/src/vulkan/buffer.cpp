#include "internal.h"

#include <limits>
#include <utility>

namespace mevk {

Buffer::Impl::~Impl() {
    std::lock_guard lock(context->mutex);
    // Only the final owner can recycle: submitted commands retain buffers.
    constexpr std::uint64_t cache_limit = 32 * 1024 * 1024;
    if (mapped && buffer && memory && memory_bytes <= cache_limit &&
        context->buffer_cache.size() < 256 && context->buffer_cache_bytes <= cache_limit - memory_bytes) {
        context->buffer_cache.push_back({buffer, memory, mapped, capacity, memory_bytes});
        context->buffer_cache_bytes += memory_bytes;
        return;
    }
    if (mapped) vkUnmapMemory(context->device, memory);
    if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(context->device, buffer, nullptr);
    if (memory != VK_NULL_HANDLE) {vkFreeMemory(context->device, memory, nullptr);context->allocated_bytes-=memory_bytes;}
}

Buffer::Buffer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Buffer::~Buffer() = default;
std::uint64_t Buffer::size() const { return impl_->size; }
void* Buffer::contents() { return impl_->mapped; }
const void* Buffer::contents() const { return impl_->mapped; }

std::shared_ptr<Buffer> Context::create_buffer(std::uint64_t size, std::string& error) {
    error.clear();
    if (!size || size > std::numeric_limits<std::size_t>::max()-3) {
        error = "MEVK: buffer size must be nonzero and fit the host address space";
        return nullptr;
    }
    // Construct before the lock so failure cleanup runs after unlocking.
    auto buffer = std::make_unique<Buffer::Impl>();
    buffer->context = impl_;
    buffer->size = size;
    buffer->padded_size=(size+3)&~std::uint64_t{3};
    std::unique_lock lock(impl_->mutex);
    auto best = impl_->buffer_cache.end();
    for (auto it = impl_->buffer_cache.begin(); it != impl_->buffer_cache.end(); ++it)
        if (it->capacity >= buffer->padded_size && it->capacity / 2 <= buffer->padded_size &&
            (best == impl_->buffer_cache.end() || it->capacity < best->capacity)) best = it;
    if (best != impl_->buffer_cache.end()) {
        buffer->buffer = best->buffer; buffer->memory = best->memory; buffer->mapped = best->mapped;
        buffer->capacity = best->capacity; buffer->memory_bytes = best->memory_bytes;
        impl_->buffer_cache_bytes -= best->memory_bytes;
        impl_->buffer_cache.erase(best);
        ++impl_->allocations.buffers_reused;
        lock.unlock();
        return std::shared_ptr<Buffer>(new Buffer(std::move(buffer)));
    }
    buffer->capacity = buffer->padded_size;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = buffer->padded_size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!vk_check(vkCreateBuffer(impl_->device, &info, nullptr, &buffer->buffer),
                  "vkCreateBuffer", error)) return nullptr;

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(impl_->device, buffer->buffer, &requirements);
    constexpr VkMemoryPropertyFlags required =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    std::uint32_t memory_type = impl_->memory_properties.memoryTypeCount;
    for (std::uint32_t i = 0; i < impl_->memory_properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (std::uint32_t{1} << i)) != 0 &&
            (impl_->memory_properties.memoryTypes[i].propertyFlags & required) == required) {
            memory_type = i;
            break;
        }
    }
    if (memory_type == impl_->memory_properties.memoryTypeCount) {
        error = "MEVK: no compatible host-visible, host-coherent buffer memory";
        return nullptr;
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    buffer->memory_bytes = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    if (!vk_check(vkAllocateMemory(impl_->device, &allocation, nullptr, &buffer->memory),
                  "vkAllocateMemory", error)) return nullptr;
    impl_->allocated_bytes+=buffer->memory_bytes;
    if (!vk_check(vkBindBufferMemory(impl_->device, buffer->buffer, buffer->memory, 0),
                  "vkBindBufferMemory", error)) return nullptr;
    if (!vk_check(vkMapMemory(impl_->device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped),
                  "vkMapMemory", error)) return nullptr;
    ++impl_->allocations.buffers_created;
    lock.unlock();
    return std::shared_ptr<Buffer>(new Buffer(std::move(buffer)));
}

} // namespace mevk

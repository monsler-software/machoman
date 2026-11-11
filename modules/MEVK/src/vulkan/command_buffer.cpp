#include "internal.h"

#include <limits>
#include <utility>

namespace mevk {
namespace {

bool in_bounds(const Buffer& buffer, std::uint64_t offset, std::uint64_t size) {
    return offset <= buffer.size() && size <= buffer.size() - offset;
}

} // namespace

CommandBuffer::Impl::~Impl() {
    // Resources are destroyed after this lock leaves scope. Their destructors
    // use the same mutex, so never clear the retained list while holding it.
    std::lock_guard lock(context->mutex);
    if (submitted && state != CommandBufferStatus::completed)
        vkWaitForFences(context->device, 1, &fence, VK_TRUE,
                        std::numeric_limits<std::uint64_t>::max());
    if (commands && fence && context->command_cache.size() < 32 &&
        vkResetCommandBuffer(commands, 0) == VK_SUCCESS &&
        vkResetFences(context->device, 1, &fence) == VK_SUCCESS &&
        (!descriptors || vkResetDescriptorPool(context->device, descriptors, 0) == VK_SUCCESS)) {
        context->command_cache.push_back({commands, fence, descriptors});
        return;
    }
    if (fence != VK_NULL_HANDLE) vkDestroyFence(context->device, fence, nullptr);
    if(descriptors!=VK_NULL_HANDLE)vkDestroyDescriptorPool(context->device,descriptors,nullptr);
    if (commands != VK_NULL_HANDLE)
        vkFreeCommandBuffers(context->device, context->command_pool, 1, &commands);
}

bool CommandBuffer::Impl::fail(const std::string& message, std::string& error) {
    if (diagnostic.empty()) diagnostic = message;
    state = CommandBufferStatus::error;
    error = diagnostic;
    return false;
}

bool CommandBuffer::Impl::recording(std::string& error) {
    error.clear();
    if (state == CommandBufferStatus::recording) return true;
    if (state == CommandBufferStatus::error) {
        error = diagnostic;
        return false;
    }
    error = "MEVK: command buffer has already been committed";
    return false;
}

void CommandBuffer::Impl::transfer_barrier() {
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    // Conservative dependency covering earlier transfers/submissions and host
    // writes. Narrow resource-specific barriers can replace it as encoders grow.
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void CommandBuffer::Impl::host_barrier() {
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

CommandBuffer::CommandBuffer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CommandBuffer::~CommandBuffer() = default;

std::shared_ptr<CommandBuffer> Context::create_command_buffer(std::string& error) {
    error.clear();
    auto command = std::make_unique<CommandBuffer::Impl>();
    command->context = impl_;
    std::unique_lock lock(impl_->mutex);
    const bool cached_command = !impl_->command_cache.empty();
    if (!impl_->command_cache.empty()) {
        const auto cached = impl_->command_cache.back();
        impl_->command_cache.pop_back();
        command->commands = cached.commands; command->fence = cached.fence;
        command->descriptors = cached.descriptors;
        ++impl_->allocations.commands_reused;
    }
    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = impl_->command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    if (!command->commands && !vk_check(vkAllocateCommandBuffers(impl_->device, &allocation, &command->commands),
                  "vkAllocateCommandBuffers", error)) return nullptr;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!vk_check(vkBeginCommandBuffer(command->commands, &begin),
                  "vkBeginCommandBuffer", error)) return nullptr;
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (!command->fence && !vk_check(vkCreateFence(impl_->device, &fence, nullptr, &command->fence),
                  "vkCreateFence", error)) return nullptr;
    // Count actual allocations separately from wrapper creation.
    if (!cached_command) ++impl_->allocations.commands_created;
    lock.unlock();
    return std::shared_ptr<CommandBuffer>(new CommandBuffer(std::move(command)));
}

bool CommandBuffer::copy(const std::shared_ptr<Buffer>& source, std::uint64_t source_offset,
                         const std::shared_ptr<Buffer>& destination, std::uint64_t destination_offset,
                         std::uint64_t size, std::string& error) {
    std::lock_guard lock(impl_->context->mutex);
    if (!impl_->recording(error)) return false;
    if(impl_->rendering)return impl_->fail("MEVK: copy during an active render pass",error);
    if (!source || !destination)
        return impl_->fail("MEVK: copy requires source and destination buffers", error);
    if (source->impl_->context != impl_->context || destination->impl_->context != impl_->context)
        return impl_->fail("MEVK: copy buffers must belong to the command buffer's context", error);
    if (!in_bounds(*source, source_offset, size) || !in_bounds(*destination, destination_offset, size))
        return impl_->fail("MEVK: copy range is outside a buffer", error);
    if (!size) return true;
    if ((source_offset % 4) || (destination_offset % 4) || (size % 4))
        return impl_->fail("MEVK: copy requires four-byte-aligned offsets and size", error);
    if (source == destination && source_offset < destination_offset + size &&
        destination_offset < source_offset + size)
        return impl_->fail("MEVK: overlapping same-buffer copy is unsupported", error);
    impl_->resources.push_back(source);
    impl_->resources.push_back(destination);
    impl_->transfer_barrier();
    VkBufferCopy region{source_offset, destination_offset, size};
    vkCmdCopyBuffer(impl_->commands, source->impl_->buffer, destination->impl_->buffer, 1, &region);
    return true;
}

bool CommandBuffer::fill(const std::shared_ptr<Buffer>& buffer, std::uint64_t offset,
                         std::uint64_t size, std::uint8_t value, std::string& error) {
    std::lock_guard lock(impl_->context->mutex);
    if (!impl_->recording(error)) return false;
    if(impl_->rendering)return impl_->fail("MEVK: fill during an active render pass",error);
    if (!buffer || buffer->impl_->context != impl_->context)
        return impl_->fail("MEVK: fill requires a buffer from the command buffer's context", error);
    if (!in_bounds(*buffer, offset, size))
        return impl_->fail("MEVK: fill range is outside the buffer", error);
    if (!size) return true;
    if ((offset % 4) || (size % 4))
        return impl_->fail("MEVK: fill requires four-byte-aligned offset and size", error);
    impl_->resources.push_back(buffer);
    impl_->transfer_barrier();
    vkCmdFillBuffer(impl_->commands, buffer->impl_->buffer, offset, size,
                    static_cast<std::uint32_t>(value) * 0x01010101u);
    return true;
}

bool CommandBuffer::commit(std::string& error) {
    std::lock_guard lock(impl_->context->mutex);
    if (!impl_->recording(error)) return false;
    if(impl_->rendering)return impl_->fail("MEVK: commit requires end_render",error);
    impl_->host_barrier();
    if (!vk_check(vkEndCommandBuffer(impl_->commands), "vkEndCommandBuffer", error))
        return impl_->fail(error, error);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &impl_->commands;
    if (!vk_check(vkQueueSubmit(impl_->context->queue, 1, &submit, impl_->fence),
                  "vkQueueSubmit", error)) return impl_->fail(error, error);
    impl_->submitted = true;
    impl_->state = CommandBufferStatus::committed;
    return true;
}

bool CommandBuffer::wait_until_completed(std::string& error) {
    std::unique_lock lock(impl_->context->mutex);
    error.clear();
    if (impl_->state == CommandBufferStatus::error) {
        error = impl_->diagnostic;
        return false;
    }
    if (!impl_->submitted) {
        error = "MEVK: cannot wait on an uncommitted command buffer";
        return false;
    }
    if (impl_->state == CommandBufferStatus::completed) return true;
    // The caller owns this wrapper while waiting; its fence remains alive.
    // Recording/submission on other threads must not wait on this CPU lock.
    lock.unlock();
    const VkResult result = vkWaitForFences(impl_->context->device, 1, &impl_->fence, VK_TRUE,
                                          std::numeric_limits<std::uint64_t>::max());
    lock.lock();
    if (!vk_check(result,
                  "vkWaitForFences", error)) return impl_->fail(error, error);
    impl_->state = CommandBufferStatus::completed;
    return true;
}

CommandBufferStatus CommandBuffer::status() {
    std::lock_guard lock(impl_->context->mutex);
    if (impl_->state == CommandBufferStatus::committed) {
        const VkResult result = vkGetFenceStatus(impl_->context->device, impl_->fence);
        if (result == VK_SUCCESS) impl_->state = CommandBufferStatus::completed;
        else if (result != VK_NOT_READY) {
            std::string error;
            vk_check(result, "vkGetFenceStatus", error);
            impl_->fail(error, error);
        }
    }
    return impl_->state;
}

std::string CommandBuffer::error_message() const {
    std::lock_guard lock(impl_->context->mutex);
    return impl_->diagnostic;
}

} // namespace mevk

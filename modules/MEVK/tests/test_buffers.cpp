#include "mevk/buffer.h"
#include "mevk/command_buffer.h"
#include "mevk/context.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using Status = mevk::CommandBufferStatus;

void check(bool condition, const char* description, const std::string& error = {}) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s%s%s\n", description,
                 error.empty() ? "" : ": ", error.c_str());
    std::exit(1);
}

std::shared_ptr<mevk::Buffer> buffer(mevk::Context& context, std::uint64_t size) {
    std::string error;
    auto result = context.create_buffer(size, error);
    check(result != nullptr, "allocate shared buffer", error);
    check(result->size() == size && result->contents() != nullptr, "buffer length and mapping");
    return result;
}

std::shared_ptr<mevk::CommandBuffer> commands(mevk::Context& context) {
    std::string error;
    auto result = context.create_command_buffer(error);
    check(result != nullptr, "create command buffer", error);
    check(result->status() == Status::recording, "initial command buffer state");
    return result;
}

void finish(const std::shared_ptr<mevk::CommandBuffer>& command) {
    std::string error;
    check(command->commit(error), "commit", error);
    check(command->wait_until_completed(error), "wait for completion", error);
    check(command->status() == Status::completed, "completed status");
    check(command->error_message().empty(), "no completion error");
}

void check_bytes(const mevk::Buffer& actual, const std::vector<std::uint8_t>& expected,
                 const char* description) {
    check(actual.size() == expected.size(), "readback length");
    check(std::memcmp(actual.contents(), expected.data(), expected.size()) == 0, description);
}

void copy_readback(mevk::Context& context) {
    constexpr std::size_t size = 128 * 1024;
    auto source = buffer(context, size);
    auto destination = buffer(context, size);
    std::vector<std::uint8_t> pattern(size), expected(size, 0x6b);
    for (std::size_t i = 0; i < size; ++i) pattern[i] = static_cast<std::uint8_t>((i * 37) ^ (i >> 8));
    std::memcpy(source->contents(), pattern.data(), size);
    std::memcpy(destination->contents(), expected.data(), size);

    std::string error;
    auto command = commands(context);
    check(command->copy(source, 4, destination, 8, size - 16, error), "record offset copy", error);
    // Recording must leave CPU-visible destination bytes unchanged.
    check_bytes(*destination, expected, "recording does not execute the transfer");
    finish(command);
    std::memcpy(expected.data() + 8, pattern.data() + 4, size - 16);
    check_bytes(*destination, expected, "GPU copy readback and untouched guard bytes");
    check_bytes(*source, pattern, "source unchanged by copy");
    std::puts("offset copy and readback passed");
}

void transfer_order(mevk::Context& context) {
    auto scratch = buffer(context, 256);
    auto output = buffer(context, 256);
    std::memset(scratch->contents(), 0, 256);
    std::memset(output->contents(), 0, 256);
    std::vector<std::uint8_t> expected(256, 0x12);
    std::memset(expected.data() + 64, 0xad, 64);

    std::string error;
    auto command = commands(context);
    check(command->fill(scratch, 0, 256, 0x12, error), "record whole-buffer fill", error);
    check(command->fill(scratch, 64, 64, 0xad, error), "record partial fill", error);
    check(command->copy(scratch, 0, output, 0, 256, error), "record fill-to-copy dependency", error);
    finish(command);
    check_bytes(*output, expected, "ordered fills and copy readback");

    auto first = commands(context);
    auto second = commands(context);
    check(first->fill(scratch, 0, 256, 0x47, error), "first submission fill", error);
    check(second->copy(scratch, 0, output, 0, 256, error), "second submission copy", error);
    check(first->commit(error), "submit first without waiting", error);
    check(second->commit(error), "submit dependent second without waiting", error);
    check(second->wait_until_completed(error), "wait for second submission", error);
    check(first->status() == Status::completed, "earlier submission completed");
    expected.assign(256, 0x47);
    check_bytes(*output, expected, "dependency across submissions");

    std::memset(scratch->contents(), 0xe1, 16);
    auto same_buffer = commands(context);
    check(same_buffer->copy(scratch, 0, scratch, 32, 16, error), "nonoverlapping same-buffer copy", error);
    finish(same_buffer);
    check(std::memcmp(scratch->contents(), static_cast<std::uint8_t*>(scratch->contents()) + 32, 16) == 0,
          "same-buffer copy readback");
    std::puts("transfer order within and across submissions passed");
}

void state_and_validation(mevk::Context& context) {
    std::string error;
    check(!context.create_buffer(0, error) && !error.empty(), "reject zero-sized allocation");
    auto data = buffer(context, 64);
    std::memset(data->contents(), 0x33, 64);

    auto empty = commands(context);
    check(!empty->wait_until_completed(error), "reject wait before commit");
    check(empty->status() == Status::recording, "wait misuse leaves recording usable");
    check(empty->fill(data, 64, 0, 0, error), "allow empty fill at end of buffer", error);
    check(empty->copy(data, 64, data, 64, 0, error), "allow empty copy at end of buffer", error);
    finish(empty);
    check(!empty->commit(error), "reject repeated commit");
    check(!empty->fill(data, 0, 4, 0x77, error), "reject recording after commit");
    check(empty->wait_until_completed(error), "repeated completion wait", error);

    for (int which = 0; which < 8; ++which) {
        auto invalid = commands(context);
        check(invalid->fill(data, 0, 64, 0x99, error), "record valid command before invalid command", error);
        bool accepted = true;
        switch (which) {
        case 0: accepted = invalid->copy(nullptr, 0, data, 0, 4, error); break;
        case 1: accepted = invalid->copy(data, 60, data, 0, 8, error); break;
        case 2: accepted = invalid->copy(data, 0, data, 60, 8, error); break;
        case 3: accepted = invalid->copy(data, std::numeric_limits<std::uint64_t>::max(), data, 0, 4, error); break;
        case 4: accepted = invalid->copy(data, 1, data, 32, 4, error); break;
        case 5: accepted = invalid->fill(data, 0, 3, 0, error); break;
        case 6: accepted = invalid->fill(data, 4, std::numeric_limits<std::uint64_t>::max(), 0, error); break;
        case 7: accepted = invalid->copy(data, 0, data, 4, 8, error); break;
        }
        check(!accepted && !error.empty(), "reject invalid transfer with diagnostic");
        const auto original_error = error;
        check(invalid->status() == Status::error, "failed command buffer status");
        check(!invalid->commit(error), "invalid command buffer submits nothing");
        check(error == original_error && invalid->error_message() == original_error, "preserve first error");
        check(!invalid->wait_until_completed(error), "failed command buffer cannot complete");
    }
    std::vector<std::uint8_t> expected(64, 0x33);
    check_bytes(*data, expected, "failed batches leave earlier recorded fills unexecuted");
    std::puts("state transitions, bounds, alignment and overlap rejection passed");
}

void resource_lifetime(mevk::Context& other_context) {
    std::string error;
    auto owner = mevk::Context::create(error);
    check(owner != nullptr, "create independent context", error);
    auto source = buffer(*owner, 1024);
    auto destination = buffer(*owner, 1024);
    std::vector<std::uint8_t> expected(1024, 0xda);
    std::memcpy(source->contents(), expected.data(), expected.size());

    auto wrong_context = commands(other_context);
    check(!wrong_context->fill(source, 0, 4, 0, error), "reject buffer from another context");
    auto local_buffer = buffer(other_context, 1024);
    auto wrong_copy = commands(other_context);
    check(!wrong_copy->copy(source, 0, local_buffer, 0, 4, error), "reject cross-context source");
    auto wrong_destination = commands(other_context);
    check(!wrong_destination->copy(local_buffer, 0, destination, 0, 4, error), "reject cross-context destination");

    auto command = commands(*owner);
    check(command->copy(source, 0, destination, 0, 1024, error), "record retained-resource copy", error);
    std::weak_ptr<mevk::Buffer> weak_source = source;
    source.reset();
    owner.reset();
    check(!weak_source.expired(), "recorded commands retain resources after context facade is gone");
    finish(command);
    check_bytes(*destination, expected, "device and buffers outlive context facade");
    command.reset();
    check(weak_source.expired(), "completed command buffer releases retained resources");

    auto in_flight = commands(other_context);
    check(in_flight->fill(local_buffer, 0, 1024, 0x71, error), "record destructor-wait fill", error);
    check(in_flight->commit(error), "submit destructor-wait fill", error);
    in_flight.reset();
    expected.assign(1024, 0x71);
    check_bytes(*local_buffer, expected, "destruction waits for an in-flight command buffer");
    std::puts("context, resource and in-flight command lifetimes passed");
}

void concurrent_submissions(mevk::Context& context) {
    std::atomic<int> completed{0};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&context, &completed, worker] {
            for (int iteration = 0; iteration < 8; ++iteration) {
                auto source = buffer(context, 4096);
                auto output = buffer(context, 4096);
                const auto value = static_cast<std::uint8_t>(worker * 8 + iteration + 1);
                auto command = commands(context);
                std::string error;
                check(command->fill(source, 0, 4096, value, error), "concurrent fill", error);
                check(command->copy(source, 0, output, 0, 4096, error), "concurrent copy", error);
                finish(command);
                check_bytes(*output, std::vector<std::uint8_t>(4096, value), "concurrent readback");
                ++completed;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    check(completed == 32, "all concurrent submissions completed");
    std::puts("32 submissions from four host threads passed");
}

} // namespace

int main() {
    std::string error;
    auto context = mevk::Context::create(error);
    check(context != nullptr, "initialize Vulkan device", error);
    std::printf("MEVK GPU integration tests: %s\n", context->device_info().name.c_str());
    copy_readback(*context);
    transfer_order(*context);
    state_and_validation(*context);
    resource_lifetime(*context);
    concurrent_submissions(*context);
    check(context->wait_idle(error), "final device idle", error);
    std::puts("MEVK shared-buffer and blit tests passed");
    return 0;
}

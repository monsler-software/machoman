#include "mevk/context.h"
#include "mevk/buffer.h"
#include "mevk/command_buffer.h"
#include "mevk/present.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

static void check(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "MEVK reuse: %s\n", message); std::abort(); }
}
int main() {
    std::string error;
    auto context = mevk::Context::create(error); check(bool(context), error.c_str());
    for (unsigned iteration = 0; iteration < 64; ++iteration) {
        auto buffer = context->create_buffer(128, error); check(bool(buffer), error.c_str());
        auto command = context->create_command_buffer(error); check(bool(command), error.c_str());
        check(command->fill(buffer, 0, 128, static_cast<std::uint8_t>(iteration), error), error.c_str());
        check(command->commit(error), error.c_str());
        // Drop the caller's buffer ref while the command still owns the GPU use.
        auto *bytes = static_cast<unsigned char *>(buffer->contents()); buffer.reset();
        auto other = context->create_buffer(128, error); check(bool(other), error.c_str());
        check(other->contents() != bytes, "in-flight buffer was recycled prematurely");
        check(command->wait_until_completed(error), error.c_str());
        for (unsigned i = 0; i < 128; ++i) check(bytes[i] == iteration, "recycled command/fence changed GPU result");
    }
    const auto stats = context->allocation_stats();
    check(stats.buffers_created == 2 && stats.buffers_reused == 126, "buffer allocations grow with frame count");
    check(stats.commands_created == 1 && stats.commands_reused == 63, "command/fence allocations grow with frame count");
    auto short_buffer = context->create_buffer(65, error);
    check(short_buffer && short_buffer->size() == 65, "reused capacity leaked into logical buffer size");
    auto invalid = context->create_command_buffer(error);
    check(!invalid->fill(short_buffer, 64, 4, 7, error), "pooled capacity allowed an out-of-bounds write");
    check(!invalid->commit(error), "invalid pooled batch was submitted");

    auto crop = mevk::presentation_region(512, 512, 356, 356, 356, 356, 1, 11);
    check(!crop.empty && crop.sx0 == 0 && crop.sy0 == 0 && crop.sx1 == 356 && crop.sy1 == 356 && crop.dx1 == 356,
        "GPU top-left crop includes drawable padding");
    crop = mevk::presentation_region(512, 256, 446, 107, 446, 107, 1, 11);
    check(crop.sx1 == 446 && crop.sy1 == 107 && crop.dy1 == 107, "non-square dialog placement");
    crop = mevk::presentation_region(1024, 1024, 356, 356, 356, 356, 2, 11);
    check(crop.sx1 == 712 && crop.sy1 == 712, "GPU placement ignores contentsScale");
    crop = mevk::presentation_region(4, 2, 4, 4, 4, 4, 1, 1);
    check(crop.dx0 == 0 && crop.dy0 == 1 && crop.dx1 == 4 && crop.dy1 == 3, "GPU aspect fit letterbox");
    crop = mevk::presentation_region(4, 2, 2, 2, 2, 2, 1, 3);
    check(crop.sx0 == 1 && crop.sx1 == 3 && crop.sy1 == 2, "GPU centered crop");
    check(mevk::presentation_region(4, 2, 2, 2, 2, 2, std::numeric_limits<double>::quiet_NaN(), 11).empty, "invalid GPU placement accepted");
    std::puts("MEVK resource reuse, in-flight lifetime and GPU placement passed");
}

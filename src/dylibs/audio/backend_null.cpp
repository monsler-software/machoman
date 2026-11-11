#include "backend_null.h"
#include <algorithm>

namespace mm::audio {
darwin::status null_backend::configure(const stream_config &config, output_process process, void *refcon) {
    if (running_ || processing_ || !process || !config.channels || !config.maximum_frames) return darwin::illegal_operation;
    owner_ = std::this_thread::get_id(); config_ = config; process_ = process; refcon_ = refcon;
    return darwin::ok;
}
darwin::status null_backend::start() {
    if (owner_ != std::this_thread::get_id() || !process_ || processing_) return darwin::illegal_operation;
    running_ = true; return darwin::ok;
}
void null_backend::stop() {
    if (owner_ == std::this_thread::get_id()) running_ = false;
}
darwin::status null_backend::pump(const cycle &c, std::span<float> out) noexcept {
    std::fill(out.begin(), out.end(), 0);
    if (owner_ != std::this_thread::get_id() || !running_ || processing_) return darwin::illegal_operation;
    if (c.event != cycle_event::disconnect && (!c.frames || c.frames > config_.maximum_frames ||
        out.size() != std::size_t(c.frames) * config_.channels)) return darwin::bad_size;
    processing_ = true;
    const auto status = process_(refcon_, c, out);
    processing_ = false;
    if (status != darwin::ok || c.event == cycle_event::disconnect) running_ = false;
    return status;
}
} // namespace mm::audio

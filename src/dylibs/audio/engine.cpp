#include "engine.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace mm::audio {
darwin::status output_engine::configure(const stream_config &config, render_function render, void *refcon) {
    if (!quiescent() || (state() != stream_state::created && state() != stream_state::configured && state() != stream_state::stopped))
        return darwin::illegal_operation;
    if (!render || !std::isfinite(config.sample_rate) || config.sample_rate < 8000 || config.sample_rate > 384000 ||
        (config.buffer_layout != layout::planar && config.buffer_layout != layout::interleaved) ||
        !config.channels || config.channels > 32 || !config.maximum_frames || config.maximum_frames > 16384)
        return darwin::unsupported_format;
    config_ = config; render_ = render; refcon_ = refcon;
    state_.store(stream_state::configured, std::memory_order_release);
    return darwin::ok;
}
darwin::status output_engine::initialize() {
    if (state() != stream_state::configured || !quiescent()) return darwin::illegal_operation;
    samples_.assign(std::size_t(config_.channels) * config_.maximum_frames, 0);
    const auto count = config_.buffer_layout == layout::planar ? config_.channels : 1;
    list_storage_.assign((offsetof(darwin::buffer_list, buffers) + count * sizeof(darwin::buffer)) / 8, 0);
    frame_position_ = underruns_ = callback_failures_ = 0;
    state_.store(stream_state::initialized, std::memory_order_release);
    return darwin::ok;
}
darwin::status output_engine::start() noexcept {
    if (!quiescent()) return darwin::illegal_operation;
    auto current = state();
    if (current == stream_state::running) return darwin::ok;
    if (current != stream_state::initialized && current != stream_state::stopped) return darwin::illegal_operation;
    state_.store(stream_state::running, std::memory_order_release);
    return darwin::ok;
}
void output_engine::stop() noexcept {
    auto current = state();
    while (current == stream_state::running && !state_.compare_exchange_weak(current, stream_state::stopped,
            std::memory_order_acq_rel, std::memory_order_acquire)) {}
}
darwin::status output_engine::dispose() {
    if (!quiescent() || state() == stream_state::running) return darwin::illegal_operation;
    state_.store(stream_state::disposed, std::memory_order_release);
    std::vector<float>().swap(samples_); std::vector<std::uint64_t>().swap(list_storage_);
    render_ = nullptr; refcon_ = nullptr;
    return darwin::ok;
}
darwin::status output_engine::process(const cycle &c, std::span<float> out) noexcept {
    std::fill(out.begin(), out.end(), 0);
    if (processing_.exchange(true, std::memory_order_acq_rel)) return darwin::illegal_operation;
    struct completion { std::atomic<bool> &flag; ~completion() { flag.store(false, std::memory_order_release); } } done{processing_};
    if (state() != stream_state::running) return darwin::illegal_operation;
    if (c.event == cycle_event::disconnect) {
        state_.store(stream_state::error, std::memory_order_release);
        return darwin::illegal_operation;
    }
    if (!c.frames || c.frames > config_.maximum_frames || out.size() != std::size_t(c.frames) * config_.channels)
        return darwin::bad_size;
    if (c.event == cycle_event::underrun) { ++underruns_; frame_position_ += c.frames; return darwin::ok; }
    std::fill(samples_.begin(), samples_.end(), 0);
    auto *list = reinterpret_cast<darwin::buffer_list *>(list_storage_.data());
    list->count = config_.buffer_layout == layout::planar ? config_.channels : 1;
    /* Treat the trailing array as separately addressed AudioBuffer slots,
     * rather than indexing beyond the one-element C struct member. */
    auto buffer_at = [&](std::uint32_t index) {
        return reinterpret_cast<darwin::buffer *>(reinterpret_cast<std::byte *>(list) + offsetof(darwin::buffer_list, buffers) + index * sizeof(darwin::buffer));
    };
    for (std::uint32_t i = 0; i < list->count; ++i) {
        auto *b = buffer_at(i);
        b->channels = config_.buffer_layout == layout::planar ? 1 : config_.channels;
        b->byte_size = c.frames * b->channels * 4;
        b->data = samples_.data() + (config_.buffer_layout == layout::planar ? std::size_t(i) * config_.maximum_frames : 0);
    }
    darwin::timestamp time{};
    time.sample_time = static_cast<double>(frame_position_); time.host_time = c.host_time_ns; time.rate_scalar = 1;
    time.flags = darwin::sample_time_valid | darwin::host_time_valid | darwin::rate_scalar_valid;
    const auto status = render_(refcon_, time, c.frames, list);
    frame_position_ += c.frames;
    /* Guest callbacks may resize/repoint ABL entries. This initial engine
     * accepts only its own complete buffers; failures produce silence. */
    bool valid = list->count == (config_.buffer_layout == layout::planar ? config_.channels : 1);
    for (std::uint32_t i = 0; valid && i < list->count; ++i) {
        const auto *b = buffer_at(i);
        const auto channels = config_.buffer_layout == layout::planar ? 1 : config_.channels;
        const auto *data = samples_.data() + (config_.buffer_layout == layout::planar ? std::size_t(i) * config_.maximum_frames : 0);
        valid = b->channels == channels && b->byte_size == c.frames * channels * 4 && b->data == data;
    }
    if (status != darwin::ok || !valid) {
        ++callback_failures_; state_.store(stream_state::error, std::memory_order_release);
        return status != darwin::ok ? status : darwin::bad_size;
    }
    for (std::uint32_t f = 0; f < c.frames; ++f)
        for (std::uint32_t ch = 0; ch < config_.channels; ++ch) {
            const auto index = config_.buffer_layout == layout::planar ? std::size_t(ch) * config_.maximum_frames + f : std::size_t(f) * config_.channels + ch;
            out[std::size_t(f) * config_.channels + ch] = samples_[index];
        }
    return darwin::ok;
}
} // namespace mm::audio

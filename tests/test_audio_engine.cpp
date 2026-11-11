#include "engine.h"
#include "backend_null.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace mm::audio;
static void check(bool ok) { if (!ok) std::abort(); }
struct renderer {
    output_engine *engine = nullptr;
    bool planar = false, fail = false, corrupt = false, stop_inside = false;
    unsigned calls = 0;
    double next_sample = 0;
};
static darwin::status render(void *opaque, const darwin::timestamp &t, std::uint32_t frames, darwin::buffer_list *list) noexcept {
    auto &r = *static_cast<renderer *>(opaque);
    check(t.sample_time == r.next_sample && t.flags == 7 && t.rate_scalar == 1 && t.host_time >= 1000);
    ++r.calls; r.next_sample += frames;
    for (unsigned ch = 0; ch < 2; ++ch) {
        auto *b = reinterpret_cast<darwin::buffer *>(reinterpret_cast<std::byte *>(list) + 8 + (r.planar ? ch : 0) * 16);
        check(b->channels == (r.planar ? 1 : 2) && b->byte_size == frames * b->channels * 4);
        for (unsigned f = 0; f < frames; ++f)
            static_cast<float *>(b->data)[r.planar ? f : f * 2 + ch] = float(t.sample_time + f) * 0.001f + float(ch);
    }
    if (r.stop_inside) r.engine->stop();
    if (r.corrupt) list->count = 99;
    return r.fail ? -123 : 0;
}
static void samples_and_lifecycle(bool planar) {
    output_engine engine; renderer r{&engine, planar}; null_backend backend;
    stream_config config{48000, 2, 512, planar ? layout::planar : layout::interleaved};
    check(engine.start() == darwin::illegal_operation);
    check(engine.configure(config, render, &r) == 0 && engine.initialize() == 0);
    check(backend.configure(config, output_engine::process_adapter, &engine) == 0);
    check(engine.start() == 0 && backend.start() == 0);
    std::array<float, 1024> out;
    for (auto frames : {128u, 1u, 512u, 64u}) {
        const auto start = r.next_sample;
        auto samples = std::span(out).first(frames * 2);
        check(backend.pump({frames, 1000 + unsigned(start)}, samples) == 0);
        for (unsigned f = 0; f < frames; ++f)
            for (unsigned ch = 0; ch < 2; ++ch) check(samples[f * 2 + ch] == float(start + f) * 0.001f + float(ch));
    }
    const auto calls = r.calls;
    check(backend.pump({8, 2000, cycle_event::underrun}, std::span(out).first(16)) == 0 && r.calls == calls);
    r.next_sample += 8;
    check(engine.underruns() == 1 && engine.frame_position() == unsigned(r.next_sample));
    for (unsigned i = 0; i < 16; ++i) check(out[i] == 0);
    backend.stop(); engine.stop();
    check(backend.pump({8, 2000}, std::span(out).first(16)) != 0 && r.calls == calls);
    check(engine.start() == 0 && backend.start() == 0);
    r.stop_inside = true;
    check(backend.pump({8, 2000}, std::span(out).first(16)) == 0 && engine.state() == stream_state::stopped);
    check(backend.pump({8, 2000}, std::span(out).first(16)) != 0 && r.calls == calls + 1);
    backend.stop(); check(engine.dispose() == 0 && engine.start() != 0);
}
static void callback_error(bool corrupt) {
    output_engine engine; renderer r{&engine, false, !corrupt, corrupt};
    check(engine.configure({48000, 2, 16}, render, &r) == 0 && engine.initialize() == 0 && engine.start() == 0);
    std::array<float, 32> out;
    check(engine.process({16, 1000}, out) == (corrupt ? darwin::bad_size : -123));
    for (float value : out) check(value == 0);
    check(engine.state() == stream_state::error && engine.callback_failures() == 1);
    check(engine.process({16, 1000}, out) != 0 && r.calls == 1);
    check(engine.dispose() == 0);
}
static void concurrent_stop() {
    struct blocking { std::atomic<bool> entered{false}, finish{false}; } state;
    auto callback = +[](void *p, const darwin::timestamp &, std::uint32_t, darwin::buffer_list *) noexcept -> darwin::status {
        auto &s = *static_cast<blocking *>(p); s.entered = true;
        while (!s.finish.load()) std::this_thread::yield();
        return 0;
    };
    output_engine engine;
    check(engine.configure({48000, 2, 16}, callback, &state) == 0 && engine.initialize() == 0 && engine.start() == 0);
    std::array<float, 32> out;
    std::thread worker([&] { check(engine.process({16, 1000}, out) == 0); });
    while (!state.entered.load()) std::this_thread::yield();
    engine.stop(); check(engine.dispose() == darwin::illegal_operation);
    state.finish = true; worker.join();
    check(engine.quiescent() && engine.process({16, 1000}, out) != 0 && engine.dispose() == 0);
}
int main() {
    darwin::stream_description f{48000, darwin::linear_pcm, darwin::format_float | darwin::format_packed, 8, 1, 8, 2, 32, 0};
    stream_config config;
    check(parse_format(f, 512, config) == 0 && config.buffer_layout == layout::interleaved);
    f.format_flags |= darwin::format_non_interleaved; f.bytes_per_frame = f.bytes_per_packet = 4;
    check(parse_format(f, 512, config) == 0 && config.buffer_layout == layout::planar);
    f.format_flags |= darwin::format_big_endian; check(parse_format(f, 512, config) == darwin::unsupported_format);
    f.format_flags = darwin::format_signed_integer | darwin::format_packed; check(parse_format(f, 512, config) == darwin::unsupported_format);
    f.sample_rate = NAN; check(parse_format(f, 512, config) == darwin::unsupported_format);
    samples_and_lifecycle(false); samples_and_lifecycle(true); callback_error(false); callback_error(true); concurrent_stop();
    output_engine engine; renderer r{&engine}; std::array<float, 32> out;
    null_backend backend;
    check(engine.configure({48000, 2, 16}, render, &r) == 0 && engine.initialize() == 0 && engine.start() == 0);
    check(engine.process({17, 1000}, out) == darwin::bad_size && !r.calls);
    check(backend.configure({48000, 2, 16}, output_engine::process_adapter, &engine) == 0 && backend.start() == 0);
    check(backend.pump({0, 1000, cycle_event::disconnect}, {}) != 0 && !r.calls && engine.state() == stream_state::error);
    check(backend.pump({16, 1000}, out) != 0 && !r.calls);
    check(engine.dispose() == 0);
    std::puts("audio: PCM layouts/samples, variable quantum, silence, errors, disconnect and stop/quiescence passed");
}

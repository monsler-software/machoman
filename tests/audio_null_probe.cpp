/* Test-only host adapter. No diagnostic symbols or null devices are
 * exposed by the production CoreAudio facade. */
#include "machoman/runtime.h"
#include "engine.h"
#include "backend_null.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <thread>
#include <xmmintrin.h>

using namespace mm::audio;
static const mm_host_api *api;
static thread_local void *active_handle;
#define EXPORT(name) __attribute__((visibility("default"))) __asm__("_" #name)
extern "C" __attribute__((visibility("default"))) void machoman_dylib_init(const mm_host_api *a) { api = a; }
struct bridge { void *handle; std::uint64_t refcon; };
static darwin::status guest_render(void *p, const darwin::timestamp &time, std::uint32_t, darwin::buffer_list *output) noexcept {
    auto &b = *static_cast<bridge *>(p);
    darwin::buffer_list input{}; darwin::timestamp input_time{};
    const std::uint64_t args[7] = {0x100, reinterpret_cast<std::uintptr_t>(&time), reinterpret_cast<std::uintptr_t>(&input),
        reinterpret_cast<std::uintptr_t>(&input_time), reinterpret_cast<std::uintptr_t>(output), reinterpret_cast<std::uintptr_t>(&time), b.refcon};
    mm_callback_result result;
    active_handle = b.handle;
    const auto mxcsr = _mm_getcsr();
    const int status = api->invoke_guest_callback(b.handle, args, 7, &result);
    active_handle = nullptr;
    if (status || result.status != mm_runtime_status::ok || _mm_getcsr() != mxcsr) return -1;
    return static_cast<darwin::status>(result.value);
}
extern "C" int busy_check() EXPORT(machoman_audio_callback_busy);
extern "C" int busy_check() {
    mm_callback_result result;
    void *extra = nullptr;
    return active_handle && api->invoke_guest_callback(active_handle, nullptr, 0, &result) == 16 &&
        api->release_guest_callback(active_handle) == 16 && api->prepare_guest_callback(0, 0, &extra) == 16 && !extra;
}
extern "C" int null_check(std::uint64_t function, std::uint64_t refcon, const darwin::stream_description *format) EXPORT(machoman_audio_null_check);
extern "C" int null_check(std::uint64_t function, std::uint64_t refcon, const darwin::stream_description *format) {
    if (!api || api->version < 3 || !format) return 1;
    stream_config config;
    if (parse_format(*format, 512, config) || config.channels != 2) return 2;
    int failure = 0;
    std::thread worker([&] {
        bridge b{nullptr, refcon};
        if (api->prepare_guest_callback(function, 512 * 1024, &b.handle)) { failure = 3; return; }
        output_engine engine; null_backend backend;
        std::array<float, 1024> storage;
        if (engine.configure(config, guest_render, &b) || engine.initialize() ||
            backend.configure(config, output_engine::process_adapter, &engine) || engine.start() || backend.start()) failure = 4;
        std::uint64_t position = 0;
        for (auto frames : {128u, 64u, 256u, 1u, 512u}) {
            if (failure) break;
            auto out = std::span(storage).first(frames * config.channels);
            if (backend.pump({frames, 1000000000ull + std::uint64_t(double(position) * 1e9 / config.sample_rate)}, out)) { failure = 5; break; }
            for (unsigned f = 0; f < frames; ++f)
                for (unsigned ch = 0; ch < config.channels; ++ch) {
                    const float sample = float(std::sin(double(position + f) * (6.283185307179586 * 440 / config.sample_rate))) * (ch ? -0.25f : 0.25f);
                    if (std::abs(out[f * config.channels + ch] - sample) > 1e-6f) failure = 6;
                }
            position += frames;
        }
        backend.stop(); engine.stop();
        if (backend.pump({16, 2000000000}, std::span(storage).first(32)) == 0) failure = 7;
        if (engine.dispose() || api->release_guest_callback(b.handle)) failure = 8;
    });
    worker.join();
    if (failure) std::fprintf(stderr, "audio null guest probe failed at stage %d\n", failure);
    return failure;
}
extern "C" void nested_call(std::uint64_t fault, std::uint64_t refcon) EXPORT(machoman_audio_callback_nested);
extern "C" void nested_call(std::uint64_t fault, std::uint64_t refcon) { api->call_guest(fault, &refcon, 1); }
extern "C" int callback_check(std::uint64_t fault, std::uint64_t nested, std::uint64_t good, std::uint64_t refcon) EXPORT(machoman_audio_callback_check);
extern "C" int callback_check(std::uint64_t fault, std::uint64_t nested, std::uint64_t good, std::uint64_t refcon) {
    if (!api || api->version < 3) return 1;
    int failure = 0;
    std::thread worker([&] {
        void *bad_handle = nullptr, *nested_handle = nullptr, *good_handle = nullptr;
        if (api->prepare_guest_callback(fault, 0, &bad_handle) || api->prepare_guest_callback(nested, 0, &nested_handle) ||
            api->prepare_guest_callback(good, 0, &good_handle)) { failure = 2; return; }
        mm_callback_result result;
        int other_thread_status = 0;
        std::thread wrong([&] { mm_callback_result other; other_thread_status = api->invoke_guest_callback(good_handle, nullptr, 0, &other); }); wrong.join();
        if (other_thread_status != 1) failure = 3;
        if (api->invoke_guest_callback(good_handle, nullptr, 9, &result) != 22) failure = 4;
        const std::uint64_t args[1] = {refcon};
        if (api->invoke_guest_callback(bad_handle, args, 1, &result) || result.status != mm_runtime_status::memory_fault || !result.fault_va || result.fault_addr != 1 || result.value) failure = 5;
        if (api->invoke_guest_callback(nested_handle, args, 1, &result) || result.status != mm_runtime_status::memory_fault || !result.fault_va || result.fault_addr != 1 || result.value) failure = 5;
        if (api->invoke_guest_callback(good_handle, args, 1, &result) || result.status != mm_runtime_status::ok || result.value != 42) failure = 6;
        if (api->release_guest_callback(bad_handle) || api->invoke_guest_callback(good_handle, args, 1, &result) || result.value != 42) failure = 7;
        if (api->release_guest_callback(nested_handle) || api->release_guest_callback(good_handle)) failure = 8;
    }); worker.join();
    if (failure) std::fprintf(stderr, "prepared callback guest probe failed at stage %d\n", failure);
    return failure;
}

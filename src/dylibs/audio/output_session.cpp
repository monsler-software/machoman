#include "output_session.h"
#include "machoman/objc_host.h"
#include "machoman/runtime.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <ctime>

namespace mm::audio {
thread_local output_session *output_session::current_ = nullptr;

output_session::~output_session() {
    stop();
    engine_.dispose();
}
darwin::status output_session::initialize(const stream_config &config, darwin::render_callback callback,
                                        callback_kind kind, std::uint32_t device, std::string target,
                                        std::atomic<unsigned> *active, std::uint64_t clock_origin) {
    if (!callback.function || backend_) return darwin::bad_parameter;
    api_ = mm_objc_host_api();
    if (!api_ || api_->version < 3) return darwin::illegal_operation;
    config_ = config; callback_ = callback; kind_ = kind; device_ = device; active_ = active; clock_origin_ = clock_origin;
    auto status = engine_.configure(config, render, this);
    if (status != darwin::ok) return status;
    if ((status = engine_.initialize()) != darwin::ok) return status;
    if (kind == callback_kind::io_proc)
        fixed_output_.assign(std::size_t(config.maximum_frames) * config.channels, 0);
    std::string application = api_->argc && api_->argv && api_->argv[0] ? api_->argv[0] : "machoman";
    if (const auto slash = application.find_last_of("/\\"); slash != std::string::npos) application.erase(0, slash + 1);
    backend_ = make_pipewire_output(std::move(target), {prepare, finish}, std::move(application));
    if (!backend_) return darwin::bad_device;
    return backend_->configure(config, process, this);
}
darwin::status output_session::start() {
    if (in_callback() || !backend_) return darwin::illegal_operation;
    if (running()) return darwin::ok;
    backend_->stop(); // Join a previous self-stop or transport failure.
    pending_frames_ = pending_offset_ = 0;
    const auto status = engine_.start();
    if (status != darwin::ok) return status;
    error_.store(0, std::memory_order_release);
    const auto started = backend_->start();
    if (started != darwin::ok) {
        engine_.stop(); error_.store(started, std::memory_order_release);
    }
    return started;
}
void output_session::stop() {
    engine_.stop();
    if (backend_) backend_->stop();
}
void output_session::set_active_counter(std::atomic<unsigned> *active) {
    std::lock_guard lock(activity_mutex_);
    if (counted_ && active_) active_->fetch_sub(1, std::memory_order_acq_rel);
    active_ = active;
    if (counted_ && active_) active_->fetch_add(1, std::memory_order_acq_rel);
}
darwin::status output_session::prepare(void *opaque) {
    auto &self = *static_cast<output_session *>(opaque);
    if (self.api_->is_guest_code(self.callback_.function)) {
        if (self.api_->prepare_guest_callback(self.callback_.function, 512 * 1024, &self.prepared_))
            return darwin::illegal_operation;
    }
    {
        std::lock_guard lock(self.activity_mutex_);
        self.counted_ = true;
        if (self.active_) self.active_->fetch_add(1, std::memory_order_acq_rel);
    }
    return darwin::ok;
}
void output_session::finish(void *opaque) {
    auto &self = *static_cast<output_session *>(opaque);
    if (self.prepared_) {
        self.api_->release_guest_callback(self.prepared_); self.prepared_ = nullptr;
    }
    {
        std::lock_guard lock(self.activity_mutex_);
        if (self.active_) self.active_->fetch_sub(1, std::memory_order_acq_rel);
        self.counted_ = false;
    }
    self.engine_.stop();
}
darwin::status output_session::process(void *opaque, const cycle &c, std::span<float> out) noexcept {
    auto &self = *static_cast<output_session *>(opaque);
    auto status = darwin::ok;
    if (self.fixed_output_.empty() || c.event != cycle_event::normal) {
        status = self.engine_.process(c, out);
    } else {
        // HAL promises the configured IO buffer size even when the PipeWire
        // quantum is smaller or is not an exact multiple of that size.
        std::fill(out.begin(), out.end(), 0);
        if (out.size() != std::size_t(c.frames) * self.config_.channels) return darwin::bad_size;
        if (!self.running()) return darwin::illegal_operation;
        for (std::uint32_t offset = 0; offset < c.frames && self.running();) {
            if (!self.pending_frames_) {
                const cycle render_cycle{self.config_.maximum_frames,
                    c.host_time_ns + std::uint64_t(double(offset) * 1e9 / self.config_.sample_rate)};
                status = self.engine_.process(render_cycle, self.fixed_output_);
                if (status != darwin::ok) break;
                self.pending_frames_ = self.config_.maximum_frames;
                self.pending_offset_ = 0;
            }
            const auto frames = std::min(c.frames - offset, self.pending_frames_);
            const auto channels = self.config_.channels;
            std::copy_n(self.fixed_output_.data() + std::size_t(self.pending_offset_) * channels,
                std::size_t(frames) * channels, out.data() + std::size_t(offset) * channels);
            offset += frames; self.pending_frames_ -= frames; self.pending_offset_ += frames;
        }
    }
    if (status != darwin::ok && self.engine_.state() == stream_state::error)
        self.error_.store(status, std::memory_order_release);
    return status;
}
darwin::status output_session::render(void *opaque, const darwin::timestamp &engine_time, std::uint32_t frames,
                                    darwin::buffer_list *output) noexcept {
    auto &self = *static_cast<output_session *>(opaque);
    auto time = engine_time;
    if (self.clock_origin_)
        time.sample_time = (double(time.host_time) - double(self.clock_origin_)) * self.config_.sample_rate / 1e9;
    darwin::timestamp now = time, input_time{};
    timespec host{}; clock_gettime(CLOCK_MONOTONIC, &host);
    now.host_time = std::uint64_t(host.tv_sec) * 1000000000ull + std::uint64_t(host.tv_nsec);
    now.sample_time += (double(now.host_time) - double(time.host_time)) * self.config_.sample_rate / 1e9;
    darwin::buffer_list input{};
    std::uint32_t flags = 0;
    const std::uint64_t io_args[] = {self.device_, reinterpret_cast<std::uintptr_t>(&now),
        reinterpret_cast<std::uintptr_t>(&input), reinterpret_cast<std::uintptr_t>(&input_time),
        reinterpret_cast<std::uintptr_t>(output), reinterpret_cast<std::uintptr_t>(&time), self.callback_.refcon};
    const std::uint64_t unit_args[] = {self.callback_.refcon, reinterpret_cast<std::uintptr_t>(&flags),
        reinterpret_cast<std::uintptr_t>(&time), 0, frames, reinterpret_cast<std::uintptr_t>(output)};
    std::array<darwin::buffer, 32> originals;
    const auto count = output->count;
    std::memcpy(originals.data(), reinterpret_cast<std::byte *>(output) + offsetof(darwin::buffer_list, buffers), count * sizeof(darwin::buffer));
    const auto *args = self.kind_ == callback_kind::io_proc ? io_args : unit_args;
    const int nargs = self.kind_ == callback_kind::io_proc ? 7 : 6;
    current_ = &self;
    darwin::status status;
    if (self.prepared_) {
        mm_callback_result result;
        const auto invoked = self.api_->invoke_guest_callback(self.prepared_, args, nargs, &result);
        status = invoked || result.status != mm_runtime_status::ok ? darwin::illegal_operation : static_cast<darwin::status>(result.value);
    } else if (self.kind_ == callback_kind::io_proc) {
        using function = darwin::status (*)(std::uint32_t, const darwin::timestamp *, const darwin::buffer_list *,
            const darwin::timestamp *, darwin::buffer_list *, const darwin::timestamp *, void *);
        status = reinterpret_cast<function>(self.callback_.function)(self.device_, &now, &input, &input_time,
            output, &time, reinterpret_cast<void *>(self.callback_.refcon));
    } else {
        using function = darwin::status (*)(void *, std::uint32_t *, const darwin::timestamp *, std::uint32_t,
            std::uint32_t, darwin::buffer_list *);
        status = reinterpret_cast<function>(self.callback_.function)(reinterpret_cast<void *>(self.callback_.refcon),
            &flags, &time, 0, frames, output);
    }
    current_ = nullptr;
    if (flags & (1u << 4)) // kAudioUnitRenderAction_OutputIsSilence
        for (std::uint32_t i = 0; i < count; ++i) std::memset(originals[i].data, 0, originals[i].byte_size);
    return status;
}
} // namespace mm::audio

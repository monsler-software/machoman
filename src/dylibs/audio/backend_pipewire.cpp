#include "backend_pipewire.h"

#ifdef MM_AUDIO_PIPEWIRE
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/json.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace mm::audio {
namespace {
void initialize_pipewire() {
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}
bool disabled() {
    const auto *choice = std::getenv("MACHOMAN_AUDIO_BACKEND");
    return choice && std::strcmp(choice, "none") == 0;
}
bool trace() { static const bool enabled = std::getenv("MACHOMAN_AUDIO_TRACE") != nullptr; return enabled; }
const char *property(const spa_dict *props, const char *key, const char *fallback = "") {
    const auto *value = props ? spa_dict_lookup(props, key) : nullptr;
    return value ? value : fallback;
}
std::uint64_t monotonic_ns() {
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return std::uint64_t(time.tv_sec) * 1000000000ull + std::uint64_t(time.tv_nsec);
}

/* Registry callbacks and snapshots are serialized by the thread loop lock.
 * No guest code is called under that lock. */
class device_monitor {
public:
    device_monitor() {
        initialize_pipewire();
        loop_ = pw_thread_loop_new("machoman-audio-devices", nullptr);
        if (!loop_) return;
        context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
        if (!context_) return;
        core_ = pw_context_connect(context_, nullptr, 0);
        if (!core_) return;
        static const pw_core_events core_events = [] {
            pw_core_events events{}; events.version = PW_VERSION_CORE_EVENTS;
            events.done = on_done; events.error = on_error; return events;
        }();
        pw_core_add_listener(core_, &core_listener_, &core_events, this);
        registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
        if (!registry_) return;
        static const pw_registry_events registry_events = [] {
            pw_registry_events events{}; events.version = PW_VERSION_REGISTRY_EVENTS;
            events.global = on_global; events.global_remove = on_remove; return events;
        }();
        pw_registry_add_listener(registry_, &registry_listener_, &registry_events, this);
        sync_ = pw_core_sync(core_, PW_ID_CORE, 0);
        if (sync_ < 0 || pw_thread_loop_start(loop_) < 0) return;
        started_ = true;
        pw_thread_loop_lock(loop_);
        timespec deadline{};
        pw_thread_loop_get_time(loop_, &deadline, 2000000000ll);
        while (!ready_ && !failed_)
            if (pw_thread_loop_timed_wait_full(loop_, &deadline) < 0) { failed_ = true; break; }
        pw_thread_loop_unlock(loop_);
    }
    ~device_monitor() {
        if (started_) pw_thread_loop_stop(loop_);
        if (metadata_) pw_proxy_destroy(reinterpret_cast<pw_proxy *>(metadata_));
        if (registry_) pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry_));
        if (core_) pw_core_disconnect(core_);
        if (context_) pw_context_destroy(context_);
        if (loop_) pw_thread_loop_destroy(loop_);
    }
    device_snapshot snapshot() {
        device_snapshot result;
        if (!started_) return result;
        pw_thread_loop_lock(loop_);
        if (ready_ && !failed_) {
            result.outputs = outputs_;
            const output_device *best = nullptr;
            for (const auto &device : outputs_) {
                if (device.name == default_name_) { best = &device; break; }
                if (!best || device.priority > best->priority) best = &device;
            }
            if (best) result.default_output = best->id;
        }
        pw_thread_loop_unlock(loop_);
        return result;
    }
    bool failed() {
        if (!started_) return true;
        pw_thread_loop_lock(loop_);
        const bool result = failed_;
        pw_thread_loop_unlock(loop_);
        return result;
    }
private:
    static void on_done(void *opaque, std::uint32_t id, int seq) {
        auto &self = *static_cast<device_monitor *>(opaque);
        if (id != PW_ID_CORE || seq != self.sync_) return;
        /* Binding the metadata during the registry enumeration issues new
         * requests. A second round trip includes its initial default value. */
        if (!self.synced_) {
            self.synced_ = true;
            self.sync_ = pw_core_sync(self.core_, PW_ID_CORE, seq);
        } else {
            self.ready_ = true;
            pw_thread_loop_signal(self.loop_, false);
        }
    }
    static void on_error(void *opaque, std::uint32_t id, int, int, const char *) {
        auto &self = *static_cast<device_monitor *>(opaque);
        if (id != PW_ID_CORE) return;
        self.failed_ = true;
        self.outputs_.clear();
        pw_thread_loop_signal(self.loop_, false);
    }
    static int on_metadata(void *opaque, std::uint32_t subject, const char *key, const char *, const char *value) {
        auto &self = *static_cast<device_monitor *>(opaque);
        if (subject != PW_ID_CORE || (key && std::strcmp(key, "default.audio.sink") != 0)) return 0;
        self.default_name_.clear();
        if (key && value) {
            spa_json object{};
            if (spa_json_begin_object(&object, value, std::strlen(value)) > 0) {
                char name[1024];
                const char *encoded = nullptr;
                const int length = spa_json_object_find(&object, "name", &encoded);
                if (length > 0 && spa_json_parse_stringn(encoded, length, name, sizeof name) > 0) self.default_name_ = name;
            }
        }
        return 0;
    }
    static void on_global(void *opaque, std::uint32_t id, std::uint32_t, const char *type,
                          std::uint32_t, const spa_dict *props) {
        auto &self = *static_cast<device_monitor *>(opaque);
        if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 && !self.metadata_ &&
            std::strcmp(property(props, PW_KEY_METADATA_NAME), "default") == 0) {
            self.metadata_ = static_cast<pw_metadata *>(pw_registry_bind(self.registry_, id,
                PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
            if (!self.metadata_) return;
            self.metadata_id_ = id;
            static const pw_metadata_events events{PW_VERSION_METADATA_EVENTS, on_metadata};
            pw_metadata_add_listener(self.metadata_, &self.metadata_listener_, &events, &self);
        }
        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 ||
            std::strcmp(property(props, PW_KEY_MEDIA_CLASS), "Audio/Sink") != 0) return;
        output_device device;
        device.node_id = id;
        device.serial = property(props, PW_KEY_OBJECT_SERIAL);
        device.name = property(props, PW_KEY_NODE_NAME);
        if (device.serial.empty() || device.name.empty()) return;
        device.id = next_id_.fetch_add(2);
        device.description = property(props, PW_KEY_NODE_DESCRIPTION, device.name.c_str());
        const auto channels = std::strtoul(property(props, PW_KEY_AUDIO_CHANNELS, "2"), nullptr, 10);
        device.channels = static_cast<std::uint32_t>(std::clamp(channels, 1ul, 32ul));
        device.priority = std::atoi(property(props, PW_KEY_PRIORITY_SESSION, "0"));
        self.outputs_.push_back(std::move(device));
    }
    static void on_remove(void *opaque, std::uint32_t id) {
        auto &self = *static_cast<device_monitor *>(opaque);
        std::erase_if(self.outputs_, [id](const auto &device) { return device.node_id == id; });
        if (self.metadata_ && id == self.metadata_id_) {
            pw_proxy_destroy(reinterpret_cast<pw_proxy *>(self.metadata_));
            self.metadata_ = nullptr;
            self.default_name_.clear();
        }
    }
    inline static std::atomic<std::uint32_t> next_id_{0x1000};
    pw_thread_loop *loop_ = nullptr;
    pw_context *context_ = nullptr;
    pw_core *core_ = nullptr;
    pw_registry *registry_ = nullptr;
    pw_metadata *metadata_ = nullptr;
    spa_hook core_listener_{}, registry_listener_{}, metadata_listener_{};
    std::vector<output_device> outputs_;
    std::string default_name_;
    std::uint32_t metadata_id_ = 0;
    int sync_ = 0;
    bool started_ = false, ready_ = false, synced_ = false, failed_ = false;
};

class pipewire_output final : public output_backend {
public:
    pipewire_output(std::string target, worker_hooks hooks, std::string application)
        : target_(std::move(target)), application_(std::move(application)), hooks_(hooks) {}
    ~pipewire_output() override { stop(); }
    darwin::status configure(const stream_config &config, output_process process, void *refcon) override {
        if (worker_.joinable() || !process) return darwin::illegal_operation;
        if (!std::isfinite(config.sample_rate) || config.sample_rate < 8000 || config.sample_rate > 384000 ||
            std::floor(config.sample_rate) != config.sample_rate || !config.channels || config.channels > 32 ||
            !config.maximum_frames || config.maximum_frames > 16384) return darwin::unsupported_format;
        config_ = config; process_ = process; refcon_ = refcon;
        return darwin::ok;
    }
    darwin::status start() override {
        if (disabled() || !process_ || worker_.joinable()) return darwin::illegal_operation;
        initialize_pipewire();
        loop_ = pw_main_loop_new(nullptr);
        if (!loop_) return darwin::illegal_operation;
        stopped_.store(false, std::memory_order_release);
        ready_ = false; status_ = darwin::illegal_operation;
        worker_ = std::thread([this] { run(); });
        std::unique_lock lock(start_mutex_);
        if (!start_condition_.wait_for(lock, std::chrono::seconds(3), [this] { return ready_; }))
            status_ = darwin::illegal_operation;
        const auto status = status_;
        lock.unlock();
        if (status != darwin::ok) stop();
        return status;
    }
    void stop() override {
        stopped_.store(true, std::memory_order_release);
        if (loop_) pw_main_loop_quit(loop_);
        /* Stop from an IOProc is deferred until it returns. Its owner must
         * join from a control thread before destroying this object. */
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
            worker_.join();
            pw_main_loop_destroy(loop_); loop_ = nullptr;
        }
    }
private:
    void startup(darwin::status status) {
        std::lock_guard lock(start_mutex_);
        if (ready_) return;
        ready_ = true; status_ = status;
        start_condition_.notify_one();
    }
    static void on_state(void *opaque, pw_stream_state old, pw_stream_state state, const char *) {
        auto &self = *static_cast<pipewire_output *>(opaque);
        if (trace()) std::fprintf(stderr, "audio: PipeWire target=%s state %s -> %s\n", self.target_.c_str(),
            pw_stream_state_as_string(old), pw_stream_state_as_string(state));
        if (state == PW_STREAM_STATE_STREAMING) self.startup(darwin::ok);
        if (state == PW_STREAM_STATE_ERROR || (state == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_UNCONNECTED)) {
            if (!self.stopped_.load(std::memory_order_acquire))
                self.process_(self.refcon_, {0, monotonic_ns(), cycle_event::disconnect}, {});
            self.startup(darwin::illegal_operation);
            pw_main_loop_quit(self.loop_);
        }
    }
    static void on_process(void *opaque) {
        auto &self = *static_cast<pipewire_output *>(opaque);
        auto *queued = pw_stream_dequeue_buffer(self.stream_);
        if (!queued) return;
        auto *buffer = queued->buffer;
        if (!buffer || buffer->n_datas != 1 || !buffer->datas[0].data || !buffer->datas[0].chunk) {
            pw_stream_queue_buffer(self.stream_, queued); return;
        }
        auto &data = buffer->datas[0];
        const auto stride = self.config_.channels * sizeof(float);
        const auto capacity = data.maxsize / stride;
        const auto frames = static_cast<std::uint32_t>(queued->requested ? std::min(queued->requested, capacity) : capacity);
        auto *samples = static_cast<float *>(data.data);
        std::fill_n(samples, std::size_t(frames) * self.config_.channels, 0.0f);
        auto host_time = monotonic_ns();
        pw_time time{};
        if (pw_stream_get_time_n(self.stream_, &time, sizeof time) == 0 && time.now > 0 && time.rate.denom) {
            const auto delay = std::max<std::int64_t>(time.delay, 0);
            host_time = std::uint64_t(time.now) + std::uint64_t(double(delay) * double(time.rate.num) * 1e9 / double(time.rate.denom)) +
                std::uint64_t(double(time.queued + time.buffered) * 1e9 / self.config_.sample_rate);
        }
        for (std::uint32_t offset = 0; offset < frames && !self.stopped_.load(std::memory_order_acquire);) {
            const auto count = std::min(frames - offset, self.config_.maximum_frames);
            const cycle c{count, host_time + std::uint64_t(double(offset) * 1e9 / self.config_.sample_rate)};
            const auto result = self.process_(self.refcon_, c,
                {samples + std::size_t(offset) * self.config_.channels, std::size_t(count) * self.config_.channels});
            if (result != darwin::ok) { self.stopped_.store(true, std::memory_order_release); break; }
            offset += count;
        }
        data.chunk->offset = 0; data.chunk->stride = static_cast<std::int32_t>(stride);
        data.chunk->size = frames * static_cast<std::uint32_t>(stride);
        queued->size = frames;
        pw_stream_queue_buffer(self.stream_, queued);
        if (self.stopped_.load(std::memory_order_acquire)) pw_main_loop_quit(self.loop_);
    }
    static void on_target_global(void *opaque, std::uint32_t id, std::uint32_t, const char *type,
                                 std::uint32_t, const spa_dict *props) {
        auto &self = *static_cast<pipewire_output *>(opaque);
        if (!std::strcmp(type, PW_TYPE_INTERFACE_Node) && self.target_ == property(props, PW_KEY_OBJECT_SERIAL))
            self.target_node_ = id;
    }
    static void on_target_remove(void *opaque, std::uint32_t id) {
        auto &self = *static_cast<pipewire_output *>(opaque);
        if (id != self.target_node_ || self.stopped_.exchange(true, std::memory_order_acq_rel)) return;
        // An unlinked stream can remain PAUSED indefinitely. Explicit device
        // removal must terminate IO even when the session manager keeps it.
        self.process_(self.refcon_, {0, monotonic_ns(), cycle_event::disconnect}, {});
        self.startup(darwin::bad_device);
        pw_main_loop_quit(self.loop_);
    }
    void run() {
        const auto prepared = hooks_.prepare ? hooks_.prepare(refcon_) : darwin::ok;
        if (prepared != darwin::ok) {
            if (trace()) std::fprintf(stderr, "audio: callback preparation failed: %d\n", prepared);
            startup(prepared); return;
        }
        static const pw_stream_events events = [] {
            pw_stream_events e{}; e.version = PW_VERSION_STREAM_EVENTS;
            e.state_changed = on_state; e.process = on_process; return e;
        }();
        auto *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Music", PW_KEY_APP_NAME, application_.c_str(), PW_KEY_NODE_NAME, "machoman-output",
            PW_KEY_APP_PROCESS_BINARY, application_.c_str(), PW_KEY_NODE_DESCRIPTION, application_.c_str(),
            "adapter.auto-port-config", "{ mode = dsp position = preserve }", nullptr);
        if (props) {
            const auto latency = std::to_string(config_.maximum_frames) + "/" + std::to_string(unsigned(config_.sample_rate));
            pw_properties_set(props, PW_KEY_NODE_LATENCY, latency.c_str());
            if (!target_.empty()) {
                pw_properties_set(props, PW_KEY_TARGET_OBJECT, target_.c_str());
                pw_properties_set(props, PW_KEY_NODE_DONT_RECONNECT, "true");
                pw_properties_set(props, "node.dont-fallback", "true");
            }
            stream_ = pw_stream_new_simple(pw_main_loop_get_loop(loop_), application_.c_str(), props, &events, this);
        }
        if (stream_) {
            std::uint8_t storage[1024];
            auto builder = SPA_POD_BUILDER_INIT(storage, sizeof storage);
            spa_audio_info_raw format{};
            format.format = SPA_AUDIO_FORMAT_F32;
            format.rate = static_cast<std::uint32_t>(config_.sample_rate); format.channels = config_.channels;
            if (format.channels == 1) format.position[0] = SPA_AUDIO_CHANNEL_MONO;
            else {
                constexpr std::uint32_t positions[] = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC,
                    SPA_AUDIO_CHANNEL_LFE, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR, SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR};
                for (std::uint32_t ch = 0; ch < format.channels; ++ch)
                    format.position[ch] = ch < std::size(positions) ? positions[ch] : SPA_AUDIO_CHANNEL_AUX0 + ch;
            }
            const spa_pod *params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
            auto flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS;
            if (!target_.empty()) flags |= PW_STREAM_FLAG_DONT_RECONNECT;
            /* The control loop is also the processing thread. RT_PROCESS
             * would move callbacks to a different, unprepared thread. */
            if (pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY, static_cast<pw_stream_flags>(flags), params, 1) >= 0) {
                if (!target_.empty()) {
                    target_node_ = PW_ID_ANY;
                    registry_ = pw_core_get_registry(pw_stream_get_core(stream_), PW_VERSION_REGISTRY, 0);
                    static const pw_registry_events target_events{PW_VERSION_REGISTRY_EVENTS, on_target_global, on_target_remove};
                    if (registry_) pw_registry_add_listener(registry_, &target_listener_, &target_events, this);
                }
                if ((target_.empty() || registry_) && !stopped_.load(std::memory_order_acquire)) pw_main_loop_run(loop_);
            }
            if (registry_) { pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry_)); registry_ = nullptr; }
            pw_stream_destroy(stream_); stream_ = nullptr;
        }
        if (hooks_.finish) hooks_.finish(refcon_);
        startup(darwin::illegal_operation);
    }
    std::string target_;
    std::string application_;
    worker_hooks hooks_;
    stream_config config_;
    output_process process_ = nullptr;
    void *refcon_ = nullptr;
    pw_main_loop *loop_ = nullptr;
    pw_stream *stream_ = nullptr;
    pw_registry *registry_ = nullptr;
    spa_hook target_listener_{};
    std::uint32_t target_node_ = PW_ID_ANY;
    std::thread worker_;
    std::atomic<bool> stopped_{true};
    std::mutex start_mutex_;
    std::condition_variable start_condition_;
    bool ready_ = false;
    darwin::status status_ = darwin::illegal_operation;
};
} // namespace

device_snapshot pipewire_devices() {
    if (disabled()) return {};
    static std::mutex mutex;
    static std::unique_ptr<device_monitor> monitor;
    static auto retry = std::chrono::steady_clock::time_point{};
    std::lock_guard lock(mutex);
    if ((!monitor || monitor->failed()) && std::chrono::steady_clock::now() >= retry) {
        monitor = std::make_unique<device_monitor>();
        retry = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }
    return monitor ? monitor->snapshot() : device_snapshot{};
}
std::unique_ptr<output_backend> make_pipewire_output(std::string target, worker_hooks hooks, std::string application) {
    return std::make_unique<pipewire_output>(std::move(target), hooks, std::move(application));
}
} // namespace mm::audio
#else
namespace mm::audio {
device_snapshot pipewire_devices() { return {}; }
std::unique_ptr<output_backend> make_pipewire_output(std::string, worker_hooks, std::string) { return {}; }
} // namespace mm::audio
#endif

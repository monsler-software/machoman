/* CoreAudio's object/property model over PipeWire output endpoints. */
#include "coreaudio_hal.h"
#include "output_session.h"
#include "audio_exports.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <limits>
#include <unordered_map>
#include <vector>

namespace mm::audio {
namespace {
using namespace darwin;
constexpr auto global = tag("glob"), input = tag("inpt"), output = tag("outp");
std::uint64_t host_time() {
    timespec time{}; clock_gettime(CLOCK_MONOTONIC, &time);
    return std::uint64_t(time.tv_sec) * 1000000000ull + std::uint64_t(time.tv_nsec);
}
std::mutex devices_mutex;
std::unordered_map<std::uint32_t, std::shared_ptr<hal_device>> devices;

struct property_value {
    std::vector<std::byte> bytes;
    std::string text;
    bool string = false, writable = false;
    template<class T> void value(const T &item) {
        bytes.resize(sizeof item); std::memcpy(bytes.data(), &item, sizeof item);
    }
    void ids(const std::vector<std::uint32_t> &items) {
        bytes.resize(items.size() * sizeof(std::uint32_t));
        if (!bytes.empty()) std::memcpy(bytes.data(), items.data(), bytes.size());
    }
    void name(std::string name) { string = true; text = std::move(name); }
    std::uint32_t size() const { return static_cast<std::uint32_t>(string ? sizeof(void *) : bytes.size()); }
};
status property(std::uint32_t object, const property_address *address, property_value &result,
                bool read = false, std::uint32_t qualifier_size = 0, const void *qualifier = nullptr) {
    if (!address) return bad_parameter;
    if (address->element != 0) return unknown_property;
    const auto selector = address->selector, scope = address->scope;
    if (object == 1) {
        if (scope != global) return unknown_property;
        const auto snapshot = pipewire_devices();
        switch (selector) {
        case tag("clas"): result.value(tag("asys")); break;
        case tag("bcls"): result.value(tag("aobj")); break;
        case tag("stdv"): result.value(0u); break;
        case tag("lnam"): result.name("PipeWire audio system"); break;
        case tag("lmak"): result.name("PipeWire"); break;
        case tag("dev#"): case tag("ownd"): {
            std::vector<std::uint32_t> ids;
            for (const auto &device : snapshot.outputs) ids.push_back(device.id);
            if (selector == tag("ownd") && qualifier_size) {
                if (!qualifier || qualifier_size % 4) return bad_size;
                bool wanted = false;
                for (std::uint32_t i = 0; i < qualifier_size / 4; ++i) {
                    std::uint32_t type; std::memcpy(&type, static_cast<const std::byte *>(qualifier) + i * 4, 4);
                    wanted |= type == tag("adev") || type == tag("aobj") || type == tag("****");
                }
                if (!wanted) ids.clear();
            }
            result.ids(ids); break;
        }
        case tag("dOut"): case tag("sOut"): result.value(snapshot.default_output); break;
        case tag("dIn "): result.value(0u); break;
        case tag("uidd"): {
            std::uint32_t id = 0;
            if (read) {
                if (!qualifier || qualifier_size != sizeof(void *)) return bad_size;
                void *uid; std::memcpy(&uid, qualifier, sizeof uid);
                char name[2048];
                if (!uid || !audio_cf_get_string(uid, name, sizeof name, 0x08000100)) return bad_parameter;
                for (const auto &device : snapshot.outputs)
                    if (std::string("pipewire:") + device.name == name) { id = device.id; break; }
            }
            result.value(id); break;
        }
        case tag("inot"): result.value(0u); break;
        case tag("user"): result.value(1u); break;
        default: return unknown_property;
        }
        return ok;
    }
    const bool stream = object >= 0x1001 && (object & 1u);
    const auto device = find_hal_device(stream ? object - 1 : object);
    if (!device) return bad_object;
    std::lock_guard lock(device->mutex);
    const auto id = device->info.id;
    const auto format = hal_format(*device);
    if (stream) {
        if (scope != global) return unknown_property;
        switch (selector) {
        case tag("clas"): result.value(tag("astr")); break;
        case tag("bcls"): result.value(tag("aobj")); break;
        case tag("stdv"): result.value(id); break;
        case tag("lnam"): result.name(device->info.description + " output"); break;
        case tag("ownd"): result.ids({}); break;
        case tag("sdir"): result.value(0u); break;
        case tag("sact"): result.value(1u); break;
        case tag("term"): result.value(tag("spkr")); break;
        case tag("schn"): result.value(1u); break;
        case tag("ltnc"): result.value(0u); break;
        case tag("sfmt"): result.value(format); result.writable = true; break;
        case tag("pft "): result.value(format); break;
        case tag("sfma"): case tag("pfta"): result.value(ranged_description{format, {8000, 384000}}); break;
        default: return unknown_property;
        }
        return ok;
    }
    // The device clock and IO buffer are shared by its scopes. Live queries
    // these on the output scope while other clients use the global scope.
    if (scope == global || scope == input || scope == output) {
        switch (selector) {
        case tag("nsrt"): result.value(device->sample_rate); result.writable = true; return ok;
        case tag("asrt"): result.value(device->sample_rate); return ok;
        case tag("nsr#"): result.value(value_range{8000, 384000}); return ok;
        case tag("fsiz"): result.value(device->buffer_frames); result.writable = true; return ok;
        case tag("fsz#"): result.value(value_range{16, 16384}); return ok;
        case tag("vfsz"): result.value(device->buffer_frames); return ok;
        default: break;
        }
    }
    if (scope == input || scope == output) {
        const bool out = scope == output;
        switch (selector) {
        case tag("slay"): {
            const buffer_list list{out ? 1u : 0u, {{out ? device->info.channels : 0u, 0, nullptr}}};
            const auto size = offsetof(buffer_list, buffers) + (out ? sizeof(buffer) : 0);
            result.bytes.resize(size); std::memcpy(result.bytes.data(), &list, size); break;
        }
        case tag("stm#"): result.ids(out ? std::vector<std::uint32_t>{id + 1} : std::vector<std::uint32_t>{}); break;
        case tag("dflt"): case tag("sflt"): result.value(out ? 1u : 0u); break;
        case tag("ltnc"): case tag("saft"): result.value(0u); break;
        case tag("dch2"): {
            if (!out) return unknown_property;
            const std::uint32_t channels[] = {1, std::min(device->info.channels, 2u)};
            result.value(channels); break;
        }
        case tag("sfmt"): // Legacy AudioDevice stream format.
            if (!out) return unknown_property;
            result.value(format); result.writable = true; break;
        case tag("lnam"): result.name(device->info.description); break;
        default: return unknown_property;
        }
        return ok;
    }
    if (scope != global) return unknown_property;
    switch (selector) {
    case tag("clas"): result.value(tag("adev")); break;
    case tag("bcls"): result.value(tag("aobj")); break;
    case tag("stdv"): result.value(1u); break;
    case tag("lnam"): result.name(device->info.description); break;
    case tag("lmak"): result.name("PipeWire"); break;
    case tag("uid "): result.name("pipewire:" + device->info.name); break;
    case tag("muid"): result.name("machoman.pipewire.output"); break;
    case tag("tran"): result.value(tag("virt")); break;
    case tag("akin"): result.ids({id}); break;
    case tag("ownd"): result.ids({id + 1}); break;
    case tag("ctrl"): result.ids({}); break;
    case tag("clkd"): case tag("hidn"): result.value(0u); break;
    case tag("livn"): result.value(1u); break;
    case tag("goin"): case tag("gone"): result.value(device->active.load(std::memory_order_acquire) ? 1u : 0u); break;
    case tag("oink"): result.value(std::int32_t{-1}); break;
    default: return unknown_property;
    }
    return ok;
}

struct io_proc {
    std::mutex mutex;
    std::shared_ptr<hal_device> device;
    render_callback callback;
    std::unique_ptr<output_session> session;
    std::atomic<output_session *> current{nullptr};
    bool disposed = false;
};
std::mutex procs_mutex;
std::unordered_map<void *, std::shared_ptr<io_proc>> procs;
std::uintptr_t next_proc = 0x100000;
std::shared_ptr<io_proc> get_proc(std::uint32_t device, void *handle) {
    std::lock_guard lock(procs_mutex);
    const auto found = procs.find(handle);
    return found != procs.end() && found->second->device->info.id == device ? found->second : nullptr;
}
} // namespace

std::shared_ptr<hal_device> find_hal_device(std::uint32_t id) {
    const auto snapshot = pipewire_devices();
    const auto found = std::find_if(snapshot.outputs.begin(), snapshot.outputs.end(),
        [id](const auto &device) { return device.id == id; });
    if (found == snapshot.outputs.end()) return {};
    std::lock_guard lock(devices_mutex);
    auto &device = devices[id];
    if (!device) {
        device = std::make_shared<hal_device>(); device->info = *found; device->clock_origin = host_time();
    }
    return device;
}
std::uint32_t default_hal_output() { return pipewire_devices().default_output; }
darwin::stream_description hal_format(const hal_device &device) {
    const auto bytes = device.info.channels * 4;
    return {device.sample_rate, darwin::linear_pcm, darwin::format_float | darwin::format_packed,
        bytes, 1, bytes, device.info.channels, 32, 0};
}
void quiesce_audio_hal() {
    std::unordered_map<void *, std::shared_ptr<io_proc>> retiring;
    {
        std::lock_guard lock(procs_mutex); retiring.swap(procs);
    }
    for (const auto &[handle, proc] : retiring) {
        std::lock_guard lock(proc->mutex);
        if (proc->session) proc->session->stop();
        proc->current.store(nullptr, std::memory_order_release);
        proc->session.reset(); proc->disposed = true;
    }
}
} // namespace mm::audio

extern "C" {
using namespace mm::audio;
using namespace mm::audio::darwin;

std::uint64_t AudioConvertHostTimeToNanos_(std::uint64_t t) AUDIO_EXPORT(AudioConvertHostTimeToNanos);
std::uint64_t AudioConvertHostTimeToNanos_(std::uint64_t t) { return t; }
std::uint64_t AudioConvertNanosToHostTime_(std::uint64_t t) AUDIO_EXPORT(AudioConvertNanosToHostTime);
std::uint64_t AudioConvertNanosToHostTime_(std::uint64_t t) { return t; }
std::uint64_t AudioGetCurrentHostTime_() AUDIO_EXPORT(AudioGetCurrentHostTime);
std::uint64_t AudioGetCurrentHostTime_() { return host_time(); }
double AudioGetHostClockFrequency_() AUDIO_EXPORT(AudioGetHostClockFrequency);
double AudioGetHostClockFrequency_() { return 1e9; }
std::uint32_t AudioGetHostClockMinimumTimeDelta_() AUDIO_EXPORT(AudioGetHostClockMinimumTimeDelta);
std::uint32_t AudioGetHostClockMinimumTimeDelta_() { return 1; }

std::int32_t AudioObjectGetPropertyDataSize_(std::uint32_t object, const property_address *address,
    std::uint32_t qualifier_size, const void *qualifier, std::uint32_t *size) AUDIO_EXPORT(AudioObjectGetPropertyDataSize);
std::int32_t AudioObjectGetPropertyDataSize_(std::uint32_t object, const property_address *address,
    std::uint32_t qualifier_size, const void *qualifier, std::uint32_t *size) {
    if (!size) return bad_parameter;
    property_value value;
    const auto status = property(object, address, value, false, qualifier_size, qualifier);
    if (address) audio_trace_property("HAL size", object, address->selector, address->scope, address->element, status);
    *size = status == ok ? value.size() : 0;
    return status;
}
std::int32_t AudioObjectGetPropertyData_(std::uint32_t object, const property_address *address,
    std::uint32_t qualifier_size, const void *qualifier, std::uint32_t *size, void *data) AUDIO_EXPORT(AudioObjectGetPropertyData);
std::int32_t AudioObjectGetPropertyData_(std::uint32_t object, const property_address *address,
    std::uint32_t qualifier_size, const void *qualifier, std::uint32_t *size, void *data) {
    if (!size) return bad_parameter;
    property_value value;
    const auto status = property(object, address, value, true, qualifier_size, qualifier);
    if (address) audio_trace_property("HAL get", object, address->selector, address->scope, address->element, status);
    if (status != ok) return status;
    const auto available = *size; *size = value.size();
    if (available < *size) return bad_size;
    if (*size && !data) return bad_parameter;
    if (value.string) {
        void *string = audio_cf_string(nullptr, value.text.c_str(), 0x08000100);
        if (!string) return illegal_operation;
        std::memcpy(data, &string, sizeof string);
    } else if (*size) std::memcpy(data, value.bytes.data(), *size);
    return ok;
}
std::uint32_t AudioObjectHasProperty_(std::uint32_t object, const property_address *address) AUDIO_EXPORT(AudioObjectHasProperty);
std::uint32_t AudioObjectHasProperty_(std::uint32_t object, const property_address *address) {
    property_value value; return property(object, address, value) == ok ? 1u : 0u;
}
std::int32_t AudioObjectIsPropertySettable_(std::uint32_t object, const property_address *address,
    std::uint8_t *settable) AUDIO_EXPORT(AudioObjectIsPropertySettable);
std::int32_t AudioObjectIsPropertySettable_(std::uint32_t object, const property_address *address, std::uint8_t *settable) {
    if (!settable) return bad_parameter;
    property_value value; const auto status = property(object, address, value);
    *settable = status == ok && value.writable ? 1 : 0;
    return status;
}
std::int32_t AudioObjectSetPropertyData_(std::uint32_t object, const property_address *address,
    std::uint32_t, const void *, std::uint32_t size, const void *data) AUDIO_EXPORT(AudioObjectSetPropertyData);
std::int32_t AudioObjectSetPropertyData_(std::uint32_t object, const property_address *address,
    std::uint32_t, const void *, std::uint32_t size, const void *data) {
    if (output_session::on_audio_thread()) return illegal_operation;
    property_value value; const auto status = property(object, address, value);
    if (status != ok) return status;
    audio_trace_property("HAL set", object, address->selector, address->scope, address->element, status);
    if (!value.writable) return illegal_operation;
    if (!data || size != value.size()) return bad_size;
    const auto device = find_hal_device(object & ~1u);
    if (!device) return bad_device;
    std::unique_lock lock(device->mutex);
    if (device->active.load(std::memory_order_acquire)) return illegal_operation;
    if (address->selector == tag("fsiz")) {
        std::uint32_t frames; std::memcpy(&frames, data, sizeof frames);
        if (frames < 16 || frames > 16384) return illegal_operation;
        if (device->buffer_frames == frames) return ok;
        device->buffer_frames = frames;
    } else {
        double rate;
        if (address->selector == tag("sfmt")) {
            stream_description format; std::memcpy(&format, data, sizeof format);
            stream_config config;
            if (parse_format(format, device->buffer_frames, config) != ok || config.channels != device->info.channels ||
                config.buffer_layout != layout::interleaved) return unsupported_format;
            rate = format.sample_rate;
        } else std::memcpy(&rate, data, sizeof rate);
        if (!std::isfinite(rate) || std::floor(rate) != rate || rate < 8000 || rate > 384000) return unsupported_format;
        if (device->sample_rate == rate) return ok;
        device->sample_rate = rate; device->clock_origin = host_time();
    }
    lock.unlock();
    for (const auto scope : {global, input, output}) {
        notify_audio_property(device->info.id, {address->selector == tag("fsiz") ? tag("fsiz") : tag("nsrt"), scope, 0});
        if (address->selector != tag("fsiz")) notify_audio_property(device->info.id, {tag("asrt"), scope, 0});
    }
    if (address->selector != tag("fsiz")) {
        notify_audio_property(device->info.id, {tag("sfmt"), output, 0});
        notify_audio_property(device->info.id + 1, {tag("sfmt"), global, 0});
        notify_audio_property(device->info.id + 1, {tag("pft "), global, 0});
    }
    return ok;
}

std::int32_t AudioDeviceCreateIOProcID_(std::uint32_t id, std::uint64_t function, std::uint64_t refcon,
    void **out) AUDIO_EXPORT(AudioDeviceCreateIOProcID);
std::int32_t AudioDeviceCreateIOProcID_(std::uint32_t id, std::uint64_t function, std::uint64_t refcon, void **out) {
    if (!out) return bad_parameter;
    *out = nullptr;
    if (output_session::on_audio_thread()) return illegal_operation;
    const auto device = find_hal_device(id);
    if (!device) return bad_device;
    if (!function) return bad_parameter;
    auto proc = std::make_shared<io_proc>(); proc->device = device; proc->callback = {function, refcon};
    std::lock_guard lock(procs_mutex);
    *out = reinterpret_cast<void *>(next_proc++); procs.emplace(*out, std::move(proc));
    return ok;
}
std::int32_t AudioDeviceStart_(std::uint32_t id, void *handle) AUDIO_EXPORT(AudioDeviceStart);
std::int32_t AudioDeviceStart_(std::uint32_t id, void *handle) {
    if (audio_trace_enabled()) std::fprintf(stderr, "audio: HAL start device=%u proc=%p\n", id, handle);
    if (output_session::on_audio_thread()) return illegal_operation;
    const auto proc = get_proc(id, handle);
    if (!proc || !find_hal_device(id)) return bad_device;
    std::lock_guard lock(proc->mutex);
    if (proc->disposed) return bad_device;
    if (proc->session && proc->session->running()) return ok;
    proc->current.store(nullptr, std::memory_order_release);
    proc->session.reset();
    auto session = std::make_unique<output_session>();
    auto &device = *proc->device;
    std::unique_lock device_lock(device.mutex);
    const stream_config config{device.sample_rate, device.info.channels, device.buffer_frames, layout::interleaved};
    auto status = session->initialize(config, proc->callback, callback_kind::io_proc, id, device.info.serial,
        &device.active, device.clock_origin);
    if (status != ok) return status;
    device.active.fetch_add(1, std::memory_order_acq_rel); // Reserve the format while Start connects.
    device_lock.unlock();
    proc->session = std::move(session);
    proc->current.store(proc->session.get(), std::memory_order_release);
    status = proc->session->start();
    device.active.fetch_sub(1, std::memory_order_acq_rel);
    return status;
}
std::int32_t AudioDeviceStop_(std::uint32_t id, void *handle) AUDIO_EXPORT(AudioDeviceStop);
std::int32_t AudioDeviceStop_(std::uint32_t id, void *handle) {
    const auto proc = get_proc(id, handle);
    if (!proc) return bad_device;
    if (auto *session = proc->current.load(std::memory_order_acquire); output_session::is_current(session)) {
        session->stop(); return ok;
    }
    if (output_session::on_audio_thread()) return illegal_operation;
    std::lock_guard lock(proc->mutex);
    if (proc->disposed) return bad_device;
    if (proc->session) proc->session->stop();
    return ok;
}
std::int32_t AudioDeviceDestroyIOProcID_(std::uint32_t id, void *handle) AUDIO_EXPORT(AudioDeviceDestroyIOProcID);
std::int32_t AudioDeviceDestroyIOProcID_(std::uint32_t id, void *handle) {
    if (output_session::on_audio_thread()) return illegal_operation;
    const auto proc = get_proc(id, handle);
    if (!proc) return bad_device;
    {
        std::lock_guard lock(proc->mutex);
        if (proc->disposed) return bad_device;
        if (proc->session) proc->session->stop();
        proc->current.store(nullptr, std::memory_order_release); proc->session.reset(); proc->disposed = true;
    }
    std::lock_guard lock(procs_mutex); procs.erase(handle);
    return ok;
}
std::int32_t AudioDeviceGetCurrentTime_(std::uint32_t id, timestamp *out) AUDIO_EXPORT(AudioDeviceGetCurrentTime);
std::int32_t AudioDeviceGetCurrentTime_(std::uint32_t id, timestamp *out) {
    const auto device = find_hal_device(id);
    if (!device) return bad_device;
    if (!out) return bad_parameter;
    std::lock_guard lock(device->mutex);
    if (!device->active.load(std::memory_order_acquire)) return tag("stop");
    const auto requested = out->flags & 7u;
    *out = {}; out->host_time = host_time();
    out->sample_time = (double(out->host_time) - double(device->clock_origin)) * device->sample_rate / 1e9;
    out->rate_scalar = 1; out->flags = requested ? requested : 7;
    return ok;
}
std::int32_t AudioDeviceTranslateTime_(std::uint32_t id, const timestamp *in, timestamp *out) AUDIO_EXPORT(AudioDeviceTranslateTime);
std::int32_t AudioDeviceTranslateTime_(std::uint32_t id, const timestamp *in, timestamp *out) {
    const auto device = find_hal_device(id);
    if (!device) return bad_device;
    if (!in || !out || !(in->flags & 3)) return bad_parameter;
    std::lock_guard lock(device->mutex);
    if (!device->active.load(std::memory_order_acquire)) return tag("stop");
    const auto source = *in;
    const auto requested = out->flags & 7u;
    const double host = source.flags & host_time_valid ? double(source.host_time) :
        double(device->clock_origin) + source.sample_time * 1e9 / device->sample_rate;
    if (!std::isfinite(host) || host < 0 || host >= double(std::numeric_limits<std::uint64_t>::max())) return bad_parameter;
    *out = {}; out->host_time = static_cast<std::uint64_t>(host);
    out->sample_time = (host - double(device->clock_origin)) * device->sample_rate / 1e9;
    out->rate_scalar = 1; out->flags = requested ? requested : 7;
    return ok;
}
std::int32_t AudioHardwareUnload_() AUDIO_EXPORT(AudioHardwareUnload);
std::int32_t AudioHardwareUnload_() {
    // Deprecated since macOS 10.6; modern HAL keeps the service alive.
    return ok;
}
} // extern "C"

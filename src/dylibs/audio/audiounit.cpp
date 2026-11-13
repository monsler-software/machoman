/* The three macOS output components share the PCM/PipeWire transport.
 * Capture, effect units and third-party component factories are separate work. */
#include "audio_exports.h"
#include "coreaudio_hal.h"
#include "output_session.h"
#include "notification_dispatch.h"
#include "machoman/objc_host.h"
#include "machoman/runtime.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {
using namespace mm::audio;
using namespace mm::audio::darwin;
constexpr status invalid_component = -3000, invalid_property = -10879, invalid_parameter = -10878,
    invalid_element = -10877, no_connection = -10876, too_many_frames = -10874,
    format_not_supported = -10868, uninitialized = -10867, invalid_scope = -10866,
    not_writable = -10865, cannot_do = -10863, already_initialized = -10849;
struct component { component_description description; const char *name; };
const component components[] = {
    {{tag("auou"), tag("ahal"), tag("appl"), 0, 0}, "Apple: AUHAL (PipeWire)"},
    {{tag("auou"), tag("def "), tag("appl"), 0, 0}, "Apple: Default Output (PipeWire)"},
    {{tag("auou"), tag("sys "), tag("appl"), 0, 0}, "Apple: System Output (PipeWire)"}
};
const component *get_component(void *handle) {
    for (const auto &component : components) if (&component == handle) return &component;
    return nullptr;
}
bool matches(const component_description &actual, const component_description *wanted) {
    return !wanted || ((!wanted->type || actual.type == wanted->type) &&
        (!wanted->subtype || actual.subtype == wanted->subtype) &&
        (!wanted->manufacturer || actual.manufacturer == wanted->manufacturer) &&
        (actual.flags & wanted->flags_mask) == (wanted->flags & wanted->flags_mask));
}
struct unit_listener {
    std::uint32_t property;
    std::uint64_t function, refcon;
    std::atomic<bool> enabled{true};
};
struct unit {
    std::mutex mutex;
    const component *component;
    void *handle;
    stream_description format{48000, linear_pcm, format_float | format_packed | format_non_interleaved, 4, 1, 4, 2, 32, 0};
    std::uint32_t maximum_frames = 512, selected_device = 0;
    std::uint32_t observed_device = 0;
    bool observed_running = false;
    bool output_enabled = true, initialized = false, disposed = false, rendering = false;
    render_callback callback{};
    std::shared_ptr<hal_device> device;
    std::unique_ptr<output_session> session;
    std::atomic<output_session *> current{nullptr};
    std::vector<float> render_storage;
    std::vector<std::shared_ptr<unit_listener>> listeners;
    bool follows_default() const { return component->description.subtype != tag("ahal"); }
    std::uint32_t device_id() const { return follows_default() ? default_hal_output() : selected_device; }
    bool in_callback() const { return output_session::is_current(current.load(std::memory_order_acquire)); }
};
std::mutex units_mutex;
std::unordered_map<void *, std::shared_ptr<unit>> units;
std::uintptr_t next_unit = 0x200000;
std::shared_ptr<unit> get_unit(void *handle) {
    std::lock_guard lock(units_mutex);
    const auto found = units.find(handle);
    return found == units.end() ? nullptr : found->second;
}
struct unit_delivery { std::shared_ptr<unit_listener> listener; void *handle; std::uint32_t property, scope, element; };
void deliver_unit_property(void *opaque) {
    std::unique_ptr<unit_delivery> item(static_cast<unit_delivery *>(opaque));
    if (!item->listener->enabled.load(std::memory_order_acquire)) return;
    const auto *api = mm_objc_host_api();
    const auto &listener = *item->listener;
    const std::uint64_t args[] = {listener.refcon, reinterpret_cast<std::uintptr_t>(item->handle), item->property, item->scope, item->element};
    if (api && api->is_guest_code(listener.function)) api->call_guest(listener.function, args, 5);
    else {
        using callback = void (*)(void *, void *, std::uint32_t, std::uint32_t, std::uint32_t);
        reinterpret_cast<callback>(listener.function)(reinterpret_cast<void *>(listener.refcon), item->handle,
            item->property, item->scope, item->element);
    }
}
void notify_unit(const unit &u, std::uint32_t property, std::uint32_t scope, std::uint32_t element) {
    for (const auto &listener : u.listeners)
        if (listener->property == property)
            queue_audio_notification(audio_global_queue(0, 0), new unit_delivery{listener, u.handle, property, scope, element},
                deliver_unit_property);
}
status silent_render(void *, std::uint32_t *flags, const timestamp *, std::uint32_t, std::uint32_t, buffer_list *) {
    *flags |= 1u << 4; return ok;
}
status initialize_session(unit &u) {
    auto device = find_hal_device(u.device_id());
    if (!device) return no_connection;
    stream_config config;
    if (parse_format(u.format, u.maximum_frames, config) != ok) return format_not_supported;
    auto session = std::make_unique<output_session>();
    auto callback = u.callback;
    if (!callback.function) callback = {reinterpret_cast<std::uintptr_t>(silent_render), 0};
    const auto result = session->initialize(config, callback, callback_kind::audio_unit, device->info.id,
        u.follows_default() ? std::string{} : device->info.serial, &device->active);
    if (result != ok) return result;
    u.current.store(nullptr, std::memory_order_release);
    u.session = std::move(session); u.device = std::move(device);
    u.current.store(u.session.get(), std::memory_order_release);
    return ok;
}
struct unit_property { std::uint32_t size = 0; bool writable = false; };
status describe(const unit &u, std::uint32_t property, std::uint32_t scope, std::uint32_t element, unit_property &info) {
    if (scope > 2) return invalid_scope;
    if (property == 2003 || property == 2006) { // EnableIO, HasIO
        if (!((scope == 2 && element == 0) || (scope == 1 && element == 1))) return invalid_element;
        info = {4, property == 2003}; return ok;
    }
    if (element != 0) return invalid_element;
    switch (property) {
    case 2: case 8: // SampleRate, StreamFormat
        if (scope != 1 && scope != 2) return invalid_scope;
        info = {property == 2 ? 8u : std::uint32_t(sizeof(stream_description)), scope == 1}; return ok;
    case 11: // ElementCount
        if (scope != 1 && scope != 2) return invalid_scope;
        info = {4, false}; return ok;
    case 23: // SetRenderCallback
        if (scope != 0 && scope != 1) return invalid_scope;
        info = {sizeof(render_callback), true}; return ok;
    case 51: // ShouldAllocateBuffer
        if (scope != 2) return invalid_scope;
        info = {4, true}; return ok;
    default: break;
    }
    if (scope != 0) return invalid_scope;
    switch (property) {
    case 3: info = {0, false}; break; // ParameterList
    case 12: case 20: info = {8, false}; break; // Latency, TailTime
    case 13: info = {8, false}; break; // SupportedNumChannels
    case 14: info = {4, true}; break; // MaximumFramesPerSlice
    case 22: case 2001: info = {4, false}; break; // LastRenderError, IsRunning
    case 2000: info = {4, !u.follows_default()}; break; // CurrentDevice
    default: return invalid_property;
    }
    return ok;
}
} // namespace

void mm::audio::refresh_audio_units(const device_snapshot &snapshot) {
    std::vector<std::shared_ptr<unit>> instances;
    {
        std::lock_guard lock(units_mutex);
        for (const auto &[handle, instance] : units) instances.push_back(instance);
    }
    for (const auto &u : instances) {
        std::lock_guard lock(u->mutex);
        if (u->disposed) continue;
        if (u->follows_default() && u->observed_device != snapshot.default_output) {
            auto device = find_hal_device(snapshot.default_output);
            if (u->session) u->session->set_active_counter(device ? &device->active : nullptr);
            u->device = std::move(device);
            u->observed_device = snapshot.default_output;
            notify_unit(*u, 2000, 0, 0);
        }
        const bool running = u->session && u->session->running();
        if (running != u->observed_running) {
            u->observed_running = running;
            notify_unit(*u, 2001, 0, 0);
        }
    }
}

void mm::audio::quiesce_audio_units() {
    std::unordered_map<void *, std::shared_ptr<unit>> retiring;
    {
        std::lock_guard lock(units_mutex); retiring.swap(units);
    }
    for (const auto &[handle, instance] : retiring) {
        std::lock_guard lock(instance->mutex);
        for (const auto &listener : instance->listeners) listener->enabled.store(false, std::memory_order_release);
        if (instance->session) instance->session->stop();
        instance->current.store(nullptr, std::memory_order_release);
        instance->session.reset(); instance->disposed = true;
    }
}

extern "C" {
void *AudioComponentFindNext_(void *previous, const component_description *wanted) AUDIO_EXPORT(AudioComponentFindNext);
void *AudioComponentFindNext_(void *previous, const component_description *wanted) {
    bool next = previous == nullptr;
    for (const auto &component : components) {
        if (next && matches(component.description, wanted)) return const_cast<struct component *>(&component);
        if (&component == previous) next = true;
    }
    return nullptr;
}
std::uint32_t AudioComponentCount_(const component_description *wanted) AUDIO_EXPORT(AudioComponentCount);
std::uint32_t AudioComponentCount_(const component_description *wanted) {
    std::uint32_t count = 0; for (const auto &component : components) if (matches(component.description, wanted)) ++count;
    return count;
}
std::int32_t AudioComponentGetDescription_(void *handle, component_description *out) AUDIO_EXPORT(AudioComponentGetDescription);
std::int32_t AudioComponentGetDescription_(void *handle, component_description *out) {
    const auto *component = get_component(handle);
    if (!component) return invalid_component;
    if (!out) return bad_parameter;
    *out = component->description; return ok;
}
std::int32_t AudioComponentCopyName_(void *handle, void **out) AUDIO_EXPORT(AudioComponentCopyName);
std::int32_t AudioComponentCopyName_(void *handle, void **out) {
    if (!out) return bad_parameter;
    *out = nullptr;
    const auto *component = get_component(handle);
    if (!component) return invalid_component;
    *out = audio_cf_string(nullptr, component->name, 0x08000100); return *out ? ok : cannot_do;
}
std::int32_t AudioComponentGetVersion_(void *handle, std::uint32_t *out) AUDIO_EXPORT(AudioComponentGetVersion);
std::int32_t AudioComponentGetVersion_(void *handle, std::uint32_t *out) {
    if (!get_component(handle)) return invalid_component;
    if (!out) return bad_parameter;
    *out = 0x10000; return ok;
}
std::int32_t AudioComponentInstanceNew_(void *handle, void **out) AUDIO_EXPORT(AudioComponentInstanceNew);
std::int32_t AudioComponentInstanceNew_(void *handle, void **out) {
    if (!out) return bad_parameter;
    *out = nullptr;
    if (output_session::on_audio_thread()) return cannot_do;
    const auto *component = get_component(handle);
    if (!component) return invalid_component;
    ensure_audio_device_monitor();
    auto instance = std::make_shared<unit>(); instance->component = component;
    instance->selected_device = default_hal_output();
    instance->observed_device = instance->selected_device;
    std::lock_guard lock(units_mutex);
    instance->handle = reinterpret_cast<void *>(next_unit++);
    *out = instance->handle; units.emplace(instance->handle, std::move(instance));
    return ok;
}
void *AudioComponentInstanceGetComponent_(void *handle) AUDIO_EXPORT(AudioComponentInstanceGetComponent);
void *AudioComponentInstanceGetComponent_(void *handle) {
    const auto u = get_unit(handle);
    return u ? const_cast<component *>(u->component) : nullptr;
}
std::int32_t AudioComponentInstanceDispose_(void *handle) AUDIO_EXPORT(AudioComponentInstanceDispose);
std::int32_t AudioComponentInstanceDispose_(void *handle) {
    if (output_session::on_audio_thread()) return cannot_do;
    const auto u = get_unit(handle);
    if (!u) return invalid_component;
    {
        std::lock_guard lock(u->mutex);
        if (u->disposed || u->rendering) return cannot_do;
        if (u->session) u->session->stop();
        u->current.store(nullptr, std::memory_order_release); u->session.reset(); u->disposed = true;
        for (const auto &listener : u->listeners) listener->enabled.store(false, std::memory_order_release);
    }
    std::lock_guard lock(units_mutex); units.erase(handle);
    return ok;
}
void AudioComponentInstantiate_(void *component, std::uint32_t, void *completion) AUDIO_EXPORT(AudioComponentInstantiate);
void AudioComponentInstantiate_(void *component, std::uint32_t, void *completion) {
    if (!completion) return;
    struct pending {
        void *block, *instance; status result;
        ~pending() { mm_objc_release(static_cast<mm_id>(block)); }
    };
    auto *task = new pending{mm_objc_retain(static_cast<mm_id>(completion)), nullptr, 0};
    task->result = AudioComponentInstanceNew_(component, &task->instance);
    auto invoke = +[](void *opaque) {
        std::unique_ptr<pending> p(static_cast<pending *>(opaque));
        const std::uint64_t args[] = {reinterpret_cast<std::uintptr_t>(p->instance), std::uint64_t(std::int64_t(p->result))};
        mm_objc_block_invoke(p->block, 2, args);
    };
    queue_audio_notification(audio_global_queue(0, 0), task, invoke);
}
void *AudioComponentRegister_(const component_description *, void *, std::uint32_t, void *) AUDIO_EXPORT(AudioComponentRegister);
void *AudioComponentRegister_(const component_description *, void *, std::uint32_t, void *) { return nullptr; }

std::int32_t AudioUnitGetPropertyInfo_(void *handle, std::uint32_t property, std::uint32_t scope, std::uint32_t element,
    std::uint32_t *size, std::uint8_t *writable) AUDIO_EXPORT(AudioUnitGetPropertyInfo);
std::int32_t AudioUnitGetPropertyInfo_(void *handle, std::uint32_t property, std::uint32_t scope, std::uint32_t element,
    std::uint32_t *size, std::uint8_t *writable) {
    const auto u = get_unit(handle);
    if (!u) return invalid_component;
    std::unique_lock lock(u->mutex, std::defer_lock); if (!u->in_callback()) lock.lock();
    if (u->disposed) return invalid_component;
    unit_property info; const auto status = describe(*u, property, scope, element, info);
    audio_trace_property("AU info", 0, property, scope, element, status);
    if (size) *size = status == ok ? info.size : 0;
    if (writable) *writable = status == ok && info.writable ? 1 : 0;
    return status;
}
std::int32_t AudioUnitGetProperty_(void *handle, std::uint32_t property, std::uint32_t scope, std::uint32_t element,
    void *data, std::uint32_t *size) AUDIO_EXPORT(AudioUnitGetProperty);
std::int32_t AudioUnitGetProperty_(void *handle, std::uint32_t property, std::uint32_t scope, std::uint32_t element,
    void *data, std::uint32_t *size) {
    if (!size) return invalid_parameter;
    const auto u = get_unit(handle);
    if (!u) return invalid_component;
    std::unique_lock lock(u->mutex, std::defer_lock); if (!u->in_callback()) lock.lock();
    if (u->disposed) return invalid_component;
    unit_property info; const auto status = describe(*u, property, scope, element, info);
    audio_trace_property("AU get", 0, property, scope, element, status);
    if (status != ok) return status;
    const auto available = *size; *size = info.size;
    if (available < info.size || (info.size && !data)) return invalid_parameter;
    auto copy = [data](const auto &value) { std::memcpy(data, &value, sizeof value); };
    switch (property) {
    case 2: case 8: {
        auto format = u->format;
        if (scope == 2) {
            const auto device = find_hal_device(u->device_id());
            if (!device) return no_connection;
            std::lock_guard device_lock(device->mutex); format = hal_format(*device);
        }
        if (property == 2) copy(format.sample_rate); else copy(format);
        break;
    }
    case 3: break;
    case 11: copy(std::uint32_t{2}); break;
    case 12: case 20: copy(0.0); break;
    case 13: { const std::int32_t channels[] = {-1, -1}; copy(channels); break; }
    case 14: copy(u->maximum_frames); break;
    case 22: copy(u->session ? u->session->last_error() : std::int32_t{0}); break;
    case 23: copy(u->callback); break;
    case 51: copy(std::uint32_t{1}); break;
    case 2000: copy(u->device_id()); break;
    case 2001: copy(std::uint32_t(u->session && u->session->running())); break;
    case 2003: copy(std::uint32_t(scope == 2 && u->output_enabled)); break;
    case 2006: copy(std::uint32_t(scope == 2 && u->device_id() != 0)); break;
    default: return invalid_property;
    }
    return ok;
}
std::int32_t AudioUnitSetProperty_(void *handle, std::uint32_t property, std::uint32_t scope, std::uint32_t element,
    const void *data, std::uint32_t size) AUDIO_EXPORT(AudioUnitSetProperty);
std::int32_t AudioUnitSetProperty_(void *handle, std::uint32_t property, std::uint32_t scope, std::uint32_t element,
    const void *data, std::uint32_t size) {
    if (output_session::on_audio_thread()) return cannot_do;
    const auto u = get_unit(handle);
    if (!u) return invalid_component;
    std::lock_guard lock(u->mutex);
    if (u->disposed) return invalid_component;
    if (u->rendering) return cannot_do;
    unit_property info; const auto result = describe(*u, property, scope, element, info);
    audio_trace_property("AU set", 0, property, scope, element, result);
    if (result != ok) return result;
    if (!info.writable) return not_writable;
    if (!data || size != info.size) return invalid_parameter;
    if (u->initialized) return already_initialized;
    switch (property) {
    case 2: case 8: {
        auto format = u->format;
        if (property == 2) std::memcpy(&format.sample_rate, data, sizeof(double));
        else std::memcpy(&format, data, sizeof format);
        stream_config config;
        if (parse_format(format, u->maximum_frames, config) != ok || std::floor(format.sample_rate) != format.sample_rate)
            return format_not_supported;
        u->format = format; break;
    }
    case 14: {
        std::uint32_t frames; std::memcpy(&frames, data, sizeof frames);
        if (!frames || frames > 16384) return invalid_parameter;
        u->maximum_frames = frames; break;
    }
    case 23: std::memcpy(&u->callback, data, sizeof u->callback); break;
    case 51: {
        std::uint32_t allocate; std::memcpy(&allocate, data, sizeof allocate);
        if (allocate != 1) return invalid_parameter;
        break;
    }
    case 2000: {
        std::uint32_t device; std::memcpy(&device, data, sizeof device);
        if (!find_hal_device(device)) return bad_device;
        u->selected_device = device; break;
    }
    case 2003: {
        std::uint32_t enabled; std::memcpy(&enabled, data, sizeof enabled);
        if (enabled > 1) return invalid_parameter;
        if (scope == 1 && enabled) return no_connection;
        if (scope == 2) u->output_enabled = enabled != 0;
        break;
    }
    default: return invalid_property;
    }
    notify_unit(*u, property, scope, element);
    return ok;
}
std::int32_t AudioUnitInitialize_(void *handle) AUDIO_EXPORT(AudioUnitInitialize);
std::int32_t AudioUnitInitialize_(void *handle) {
    if (output_session::on_audio_thread()) return cannot_do;
    const auto u = get_unit(handle);
    if (!u) return invalid_component;
    std::lock_guard lock(u->mutex);
    if (u->disposed) return invalid_component;
    if (u->initialized) return ok;
    const auto result = initialize_session(*u);
    if (result != ok) return result;
    u->render_storage.assign(std::size_t(u->maximum_frames) * u->format.channels_per_frame, 0);
    u->initialized = true; return ok;
}
std::int32_t AudioUnitUninitialize_(void *handle) AUDIO_EXPORT(AudioUnitUninitialize);
std::int32_t AudioUnitUninitialize_(void *handle) {
    if (output_session::on_audio_thread()) return cannot_do;
    const auto u = get_unit(handle);
    if (!u) return invalid_component;
    std::lock_guard lock(u->mutex);
    if (u->disposed) return invalid_component;
    if (u->rendering) return cannot_do;
    if (u->session) u->session->stop();
    u->current.store(nullptr, std::memory_order_release); u->session.reset(); u->device.reset();
    u->initialized = false; std::vector<float>().swap(u->render_storage);
    return ok;
}
std::int32_t AudioOutputUnitStart_(void *handle) AUDIO_EXPORT(AudioOutputUnitStart);
std::int32_t AudioOutputUnitStart_(void *handle) {
    if (audio_trace_enabled()) std::fprintf(stderr, "audio: AU start unit=%p\n", handle);
    if (output_session::on_audio_thread()) return cannot_do;
    const auto u = get_unit(handle);
    if (!u) return invalid_component;
    std::lock_guard lock(u->mutex);
    if (u->disposed) return invalid_component;
    if (!u->initialized) return uninitialized;
    if (u->rendering) return cannot_do;
    if (!u->output_enabled) return no_connection;
    if (u->session->running()) return ok;
    if (u->session->last_error() || !u->device || u->device_id() != u->device->info.id) {
        const auto result = initialize_session(*u); if (result != ok) return result;
    }
    if (!find_hal_device(u->device_id())) return no_connection;
    u->device->active.fetch_add(1, std::memory_order_acq_rel);
    const auto result = u->session->start();
    u->device->active.fetch_sub(1, std::memory_order_acq_rel);
    if (result == ok) {
        u->observed_running = true;
        notify_unit(*u, 2001, 0, 0);
    }
    return result;
}
std::int32_t AudioOutputUnitStop_(void *handle) AUDIO_EXPORT(AudioOutputUnitStop);
std::int32_t AudioOutputUnitStop_(void *handle) {
    const auto u = get_unit(handle);
    if (!u) return invalid_component;
    if (auto *session = u->current.load(std::memory_order_acquire); output_session::is_current(session)) {
        session->stop(); return ok;
    }
    if (output_session::on_audio_thread()) return cannot_do;
    std::lock_guard lock(u->mutex);
    if (u->disposed) return invalid_component;
    if (u->session) u->session->stop();
    u->observed_running = false;
    notify_unit(*u, 2001, 0, 0);
    return ok;
}
std::int32_t AudioUnitReset_(void *handle, std::uint32_t scope, std::uint32_t element) AUDIO_EXPORT(AudioUnitReset);
std::int32_t AudioUnitReset_(void *handle, std::uint32_t scope, std::uint32_t element) {
    if (output_session::on_audio_thread()) return cannot_do;
    const auto u = get_unit(handle);
    if (!u) return invalid_component;
    if (scope > 2) return invalid_scope;
    if (element) return invalid_element;
    std::lock_guard lock(u->mutex);
    if (u->disposed) return invalid_component;
    if (!u->initialized) return uninitialized;
    if (u->rendering) return cannot_do;
    if (u->session->running()) return ok;
    return initialize_session(*u);
}
std::int32_t AudioUnitRender_(void *handle, std::uint32_t *flags, const timestamp *time, std::uint32_t bus,
    std::uint32_t frames, buffer_list *data) AUDIO_EXPORT(AudioUnitRender);
std::int32_t AudioUnitRender_(void *handle, std::uint32_t *flags, const timestamp *time, std::uint32_t bus,
    std::uint32_t frames, buffer_list *data) {
    const auto u = get_unit(handle);
    if (!u) return invalid_component;
    if (bus != 0) return invalid_element; // Capture is not implemented.
    if (output_session::on_audio_thread()) return cannot_do;
    if (!flags || !time || !data) return invalid_parameter;
    std::unique_lock lock(u->mutex);
    if (!u->initialized) return uninitialized;
    if (u->disposed || u->rendering || u->session->running()) return cannot_do;
    if (frames > u->maximum_frames) return too_many_frames;
    const bool planar = (u->format.format_flags & format_non_interleaved) != 0;
    const auto count = planar ? u->format.channels_per_frame : 1;
    if (data->count != count) return invalid_parameter;
    auto buffer_at = [data](std::uint32_t i) {
        return reinterpret_cast<buffer *>(reinterpret_cast<std::byte *>(data) + offsetof(buffer_list, buffers) + i * sizeof(buffer));
    };
    for (std::uint32_t i = 0; i < count; ++i) {
        auto &buffer = *buffer_at(i);
        const auto channels = planar ? 1 : u->format.channels_per_frame;
        const auto bytes = frames * channels * 4;
        if (buffer.channels != channels || (buffer.data && buffer.byte_size < bytes)) return invalid_parameter;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        auto &buffer = *buffer_at(i);
        if (!buffer.data) buffer.data = u->render_storage.data() + (planar ? std::size_t(i) * u->maximum_frames : 0);
        buffer.byte_size = frames * buffer.channels * 4;
        std::memset(buffer.data, 0, buffer.byte_size);
    }
    if (!u->callback.function) { *flags |= 1u << 4; return ok; }
    u->rendering = true;
    const auto callback = u->callback;
    lock.unlock();
    const std::uint64_t args[] = {callback.refcon, reinterpret_cast<std::uintptr_t>(flags), reinterpret_cast<std::uintptr_t>(time),
        bus, frames, reinterpret_cast<std::uintptr_t>(data)};
    const auto *api = mm_objc_host_api();
    status result;
    if (api && api->is_guest_code(callback.function)) result = static_cast<status>(api->call_guest(callback.function, args, 6));
    else {
        using render = status (*)(void *, std::uint32_t *, const timestamp *, std::uint32_t, std::uint32_t, buffer_list *);
        result = reinterpret_cast<render>(callback.function)(reinterpret_cast<void *>(callback.refcon), flags, time, bus, frames, data);
    }
    lock.lock(); u->rendering = false;
    return result;
}
std::int32_t AudioUnitAddPropertyListener_(void *handle, std::uint32_t property, std::uint64_t function,
    std::uint64_t refcon) AUDIO_EXPORT(AudioUnitAddPropertyListener);
std::int32_t AudioUnitAddPropertyListener_(void *handle, std::uint32_t property, std::uint64_t function, std::uint64_t refcon) {
    if (output_session::on_audio_thread()) return cannot_do;
    const auto u = get_unit(handle);
    if (!u) return invalid_component;
    if (!function) return invalid_parameter;
    std::lock_guard lock(u->mutex);
    if (u->disposed) return invalid_component;
    for (const auto &listener : u->listeners)
        if (listener->property == property && listener->function == function && listener->refcon == refcon) return ok;
    auto listener = std::make_shared<unit_listener>();
    listener->property = property; listener->function = function; listener->refcon = refcon;
    u->listeners.push_back(std::move(listener)); return ok;
}
std::int32_t AudioUnitRemovePropertyListenerWithUserData_(void *handle, std::uint32_t property, std::uint64_t function,
    std::uint64_t refcon) AUDIO_EXPORT(AudioUnitRemovePropertyListenerWithUserData);
std::int32_t AudioUnitRemovePropertyListenerWithUserData_(void *handle, std::uint32_t property, std::uint64_t function, std::uint64_t refcon) {
    if (output_session::on_audio_thread()) return cannot_do;
    const auto u = get_unit(handle);
    if (!u) return invalid_component;
    std::lock_guard lock(u->mutex);
    std::erase_if(u->listeners, [&](const auto &listener) {
        const bool remove = listener->property == property && listener->function == function && listener->refcon == refcon;
        if (remove) listener->enabled.store(false, std::memory_order_release);
        return remove;
    }); return ok;
}
std::int32_t AudioUnitRemovePropertyListener_(void *handle, std::uint32_t property, std::uint64_t function) AUDIO_EXPORT(AudioUnitRemovePropertyListener);
std::int32_t AudioUnitRemovePropertyListener_(void *handle, std::uint32_t property, std::uint64_t function) {
    if (output_session::on_audio_thread()) return cannot_do;
    const auto u = get_unit(handle);
    if (!u) return invalid_component;
    std::lock_guard lock(u->mutex);
    std::erase_if(u->listeners, [&](const auto &listener) {
        const bool remove = listener->property == property && listener->function == function;
        if (remove) listener->enabled.store(false, std::memory_order_release);
        return remove;
    }); return ok;
}
std::int32_t AudioUnitGetParameter_(void *, std::uint32_t, std::uint32_t, std::uint32_t, float *value) AUDIO_EXPORT(AudioUnitGetParameter);
std::int32_t AudioUnitGetParameter_(void *, std::uint32_t, std::uint32_t, std::uint32_t, float *value) {
    if (value) *value = 0; return invalid_parameter;
}
std::int32_t AudioUnitSetParameter_(void *, std::uint32_t, std::uint32_t, std::uint32_t, float, std::uint32_t) AUDIO_EXPORT(AudioUnitSetParameter);
std::int32_t AudioUnitSetParameter_(void *, std::uint32_t, std::uint32_t, std::uint32_t, float, std::uint32_t) { return invalid_parameter; }
} // extern "C"

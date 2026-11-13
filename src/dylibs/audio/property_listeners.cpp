#include "coreaudio_hal.h"
#include "audio_exports.h"
#include "notification_dispatch.h"
#include "machoman/objc_host.h"
#include "machoman/runtime.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mm::audio {
namespace {
using namespace darwin;
constexpr auto global_scope = fourcc('g', 'l', 'o', 'b'), wildcard = fourcc('*', '*', '*', '*');
struct listener {
    std::uint32_t object;
    property_address address;
    std::uint64_t function, refcon;
    void *queue = nullptr, *original_block = nullptr, *block = nullptr;
    std::atomic<bool> enabled{true};
    ~listener() { if (block) mm_objc_release(static_cast<mm_id>(block)); }
};
bool matches(const property_address &wanted, const property_address &actual) {
    return (wanted.selector == wildcard || wanted.selector == actual.selector) &&
        (wanted.scope == wildcard || wanted.scope == actual.scope) &&
        (wanted.element == ~0u || wanted.element == actual.element);
}
struct delivery { std::shared_ptr<listener> target; std::uint32_t object; property_address address; };
void deliver(void *opaque) {
    std::unique_ptr<delivery> item(static_cast<delivery *>(opaque));
    auto &target = *item->target;
    if (!target.enabled.load(std::memory_order_acquire)) return;
    if (target.block) {
        const std::uint64_t args[] = {1, reinterpret_cast<std::uintptr_t>(&item->address)};
        mm_objc_block_invoke(target.block, 2, args);
    } else {
        const auto *api = mm_objc_host_api();
        const std::uint64_t args[] = {item->object, 1, reinterpret_cast<std::uintptr_t>(&item->address), target.refcon};
        if (api && api->is_guest_code(target.function)) api->call_guest(target.function, args, 4);
        else {
            using function = status (*)(std::uint32_t, std::uint32_t, const property_address *, void *);
            reinterpret_cast<function>(target.function)(item->object, 1, &item->address, reinterpret_cast<void *>(target.refcon));
        }
    }
}
class listeners {
public:
    listeners() : previous_(pipewire_devices()), queue_(audio_global_queue(0, 0)), worker_([this] { monitor(); }) {}
    ~listeners() { shutdown(); }
    void shutdown() {
        {
            std::lock_guard lock(mutex_); stopping_ = true;
            for (const auto &item : entries_) item->enabled.store(false, std::memory_order_release);
        }
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
        entries_.clear();
    }
    status add(std::uint32_t object, property_address address, std::uint64_t function,
               std::uint64_t refcon, void *queue, void *block) {
        if (!function && !block) return bad_parameter;
        auto entry = std::make_shared<listener>();
        entry->object = object; entry->address = address; entry->function = function; entry->refcon = refcon;
        entry->queue = queue; entry->original_block = block;
        if (block) entry->block = mm_objc_retain(static_cast<mm_id>(block));
        std::lock_guard lock(mutex_);
        for (const auto &old : entries_)
            if (same(*old, object, address, function, refcon, queue, block)) return ok;
        entries_.push_back(std::move(entry));
        return ok;
    }
    status remove(std::uint32_t object, property_address address, std::uint64_t function,
                  std::uint64_t refcon, void *queue, void *block) {
        std::shared_ptr<listener> removed;
        {
            std::lock_guard lock(mutex_);
            const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto &item) {
                return same(*item, object, address, function, refcon, queue, block);
            });
            if (found == entries_.end()) return ok;
            removed = *found; removed->enabled.store(false, std::memory_order_release);
            entries_.erase(found);
        }
        return ok;
    }
    void notify(std::uint32_t object, property_address address) {
        std::vector<std::shared_ptr<listener>> recipients;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            for (const auto &item : entries_)
                if (item->object == object && matches(item->address, address)) recipients.push_back(item);
        }
        for (auto &item : recipients) {
            auto *queue = item->queue ? item->queue : queue_;
            queue_audio_notification(queue, new delivery{item, object, address}, deliver);
        }
    }
private:
    static bool same(const listener &item, std::uint32_t object, property_address address,
                     std::uint64_t function, std::uint64_t refcon, void *queue, void *block) {
        return item.object == object && item.address.selector == address.selector && item.address.scope == address.scope &&
            item.address.element == address.element && item.function == function && item.refcon == refcon &&
            item.queue == queue && item.original_block == block;
    }
    void monitor() {
        std::unique_lock lock(mutex_);
        while (!wake_.wait_for(lock, std::chrono::milliseconds(100), [this] { return stopping_; })) {
            lock.unlock();
            auto current = pipewire_devices();
            refresh_audio_units(current);
            auto contains = [](const auto &devices, std::uint32_t id) {
                return std::any_of(devices.begin(), devices.end(), [id](const auto &device) { return device.id == id; });
            };
            bool changed = current.outputs.size() != previous_.outputs.size();
            for (const auto &old : previous_.outputs)
                if (!contains(current.outputs, old.id)) {
                    changed = true;
                    notify(old.id, {fourcc('l','i','v','n'), global_scope, 0});
                }
            if (changed) {
                notify(1, {fourcc('d','e','v','#'), global_scope, 0});
                notify(1, {fourcc('o','w','n','d'), global_scope, 0});
            }
            if (current.default_output != previous_.default_output) {
                notify(1, {fourcc('d','O','u','t'), global_scope, 0});
                notify(1, {fourcc('s','O','u','t'), global_scope, 0});
            }
            for (const auto &output : current.outputs) {
                const auto device = find_hal_device(output.id);
                const bool running = device && device->active.load(std::memory_order_acquire);
                if (running_[output.id] != running) {
                    running_[output.id] = running;
                    notify(output.id, {fourcc('g','o','i','n'), global_scope, 0});
                    notify(output.id, {fourcc('g','o','n','e'), global_scope, 0});
                }
            }
            previous_ = std::move(current);
            lock.lock();
        }
    }
    device_snapshot previous_;
    void *queue_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::vector<std::shared_ptr<listener>> entries_;
    std::unordered_map<std::uint32_t, bool> running_;
    std::thread worker_;
};
std::atomic<listeners *> live_listeners{nullptr};
listeners &property_listeners() {
    static listeners instance;
    live_listeners.store(&instance, std::memory_order_release);
    return instance;
}
} // namespace
void notify_audio_property(std::uint32_t object, property_address address) { property_listeners().notify(object, address); }
void ensure_audio_device_monitor() { (void)property_listeners(); }
void quiesce_audio_listeners() {
    if (auto *instance = live_listeners.load(std::memory_order_acquire)) instance->shutdown();
}
} // namespace mm::audio

extern "C" {
using namespace mm::audio;
std::int32_t AudioObjectAddPropertyListener_(std::uint32_t object, const darwin::property_address *address,
    std::uint64_t function, std::uint64_t refcon) AUDIO_EXPORT(AudioObjectAddPropertyListener);
std::int32_t AudioObjectAddPropertyListener_(std::uint32_t object, const darwin::property_address *address,
    std::uint64_t function, std::uint64_t refcon) {
    if (!address || !function) return darwin::bad_parameter;
    if (object != 1 && !find_hal_device(object) && !(object & 1u && find_hal_device(object - 1))) return darwin::bad_object;
    return property_listeners().add(object, *address, function, refcon, nullptr, nullptr);
}
std::int32_t AudioObjectRemovePropertyListener_(std::uint32_t object, const darwin::property_address *address,
    std::uint64_t function, std::uint64_t refcon) AUDIO_EXPORT(AudioObjectRemovePropertyListener);
std::int32_t AudioObjectRemovePropertyListener_(std::uint32_t object, const darwin::property_address *address,
    std::uint64_t function, std::uint64_t refcon) {
    return address ? property_listeners().remove(object, *address, function, refcon, nullptr, nullptr) : darwin::bad_parameter;
}
std::int32_t AudioObjectAddPropertyListenerBlock_(std::uint32_t object, const darwin::property_address *address,
    void *queue, void *block) AUDIO_EXPORT(AudioObjectAddPropertyListenerBlock);
std::int32_t AudioObjectAddPropertyListenerBlock_(std::uint32_t object, const darwin::property_address *address,
    void *queue, void *block) {
    if (!address || !block) return darwin::bad_parameter;
    if (object != 1 && !find_hal_device(object) && !(object & 1u && find_hal_device(object - 1))) return darwin::bad_object;
    return property_listeners().add(object, *address, 0, 0, queue, block);
}
std::int32_t AudioObjectRemovePropertyListenerBlock_(std::uint32_t object, const darwin::property_address *address,
    void *queue, void *block) AUDIO_EXPORT(AudioObjectRemovePropertyListenerBlock);
std::int32_t AudioObjectRemovePropertyListenerBlock_(std::uint32_t object, const darwin::property_address *address,
    void *queue, void *block) {
    return address ? property_listeners().remove(object, *address, 0, 0, queue, block) : darwin::bad_parameter;
}
} // extern "C"

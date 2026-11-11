/* Manual KVO for object-valued properties. Host setters bracket their changes
 * with willChangeValueForKey:/didChangeValueForKey:, as guest setters can do.
 * Automatic setter interposition and compound key paths are not implemented. */
#include "foundation.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

extern "C" {
mm_id kvo_get_associated(mm_id, const void *) __asm__("_objc_getAssociatedObject");
void kvo_set_associated(mm_id, const void *, mm_id, std::uintptr_t) __asm__("_objc_setAssociatedObject");
mm_id kvo_init_weak(mm_id *, mm_id) __asm__("_objc_initWeak");
void kvo_destroy_weak(mm_id *) __asm__("_objc_destroyWeak");
mm_id kvo_load_weak(mm_id *) __asm__("_objc_loadWeakRetained");
void *kvo_get_method(mm_objc_class *, mm_SEL) __asm__("_class_getInstanceMethod");
const char *kvo_method_types(void *) __asm__("_method_getTypeEncoding");
}

namespace fnd {
FND_CLASS(__MMKeyValueObservers);
namespace {
struct Owned {
    id value;
    explicit Owned(id object) : value(retain(object)) {}
    ~Owned() { release(value); }
};
struct Observation {
    id observer = nullptr;
    std::shared_ptr<Owned> key;
    std::string path;
    std::uint64_t options;
    void *context;
    std::atomic<bool> active{true};
    Observation(id target, id name, std::uint64_t flags, void *ctx)
        : key(std::make_shared<Owned>(name)), path(to_utf8(name)), options(flags), context(ctx) {
        kvo_init_weak(&observer, target);
    }
    ~Observation() { kvo_destroy_weak(&observer); }
};
struct Pending {
    std::string path;
    std::thread::id thread;
    std::shared_ptr<Owned> old;
};
struct Observers {
    mm_objc_object base;
    std::mutex mutex;
    std::vector<std::shared_ptr<Observation>> entries;
    std::vector<Pending> pending;
};
Class observers_class;
const char association_key = 0;
std::mutex association_mutex;

std::shared_ptr<Owned> state_for(id object, bool create) {
    std::lock_guard lock(association_mutex);
    id state = kvo_get_associated(object, &association_key);
    if (!state && create) {
        state = reinterpret_cast<id>(make<Observers>(observers_class));
        kvo_set_associated(object, &association_key, state, 1);
        release(state);
    }
    return state ? std::make_shared<Owned>(state) : nullptr;
}

id object_value(id object, const std::string &key) {
    const SEL selector = mm_objc_sel(key.c_str());
    void *method = kvo_get_method(object->isa, selector);
    const char *type = method ? kvo_method_types(method) : nullptr;
    if (type && (*type == '@' || *type == '#')) return obj(send(object, key.c_str()));
    std::fprintf(stderr, "Foundation KVO: object-valued getter %s is unavailable\n", key.c_str());
    return nullptr;
}

std::vector<std::shared_ptr<Observation>> snapshot(Observers *state, const std::string &key) {
    std::vector<std::shared_ptr<Observation>> result;
    std::lock_guard lock(state->mutex);
    for (auto &entry : state->entries) if (entry->path == key) result.push_back(entry);
    return result;
}

void deliver(id object, const std::vector<std::shared_ptr<Observation>> &entries,
             id old_value, id new_value, bool prior, bool initial = false) {
    // Keep the observed object and each observer alive only during delivery.
    Owned observed(object);
    for (auto &entry : entries) {
        if (!entry->active || (prior && !(entry->options & 8))) continue;
        id observer = kvo_load_weak(&entry->observer);
        if (!observer) continue;
        id change = dictionary_create(false);
        id kind = number_from_int64(1);
        dictionary_set(change, constant_string("kind"), kind);
        release(kind);
        if (prior) {
            id flag = number_bool(true);
            dictionary_set(change, constant_string("notificationIsPrior"), flag);
        }
        if ((entry->options & 2) && !initial)
            dictionary_set(change, constant_string("old"), old_value ? old_value : null_object());
        if ((entry->options & 1) && !prior)
            dictionary_set(change, constant_string("new"), new_value ? new_value : null_object());
        const std::uint64_t args[] = {ret(entry->key->value), ret(object), ret(change), ret(entry->context)};
        mm_objc_send(observer, mm_objc_sel("observeValueForKeyPath:ofObject:change:context:"), 4, args);
        release(change);
        release(observer);
    }
}

std::uint64_t add_observer(id self, SEL, id observer, id key, std::uint64_t options, void *context) {
    if (!observer || !key) return 0;
    auto owner = state_for(self, true);
    auto entry = std::make_shared<Observation>(observer, key, options, context);
    {
        auto *state = as<Observers>(owner->value);
        std::lock_guard lock(state->mutex);
        state->entries.push_back(entry);
    }
    if (options & 4) {
        Owned value(object_value(self, entry->path));
        deliver(self, {entry}, nullptr, value.value, false, true);
    }
    return 0;
}
std::uint64_t remove_observer(id self, SEL, id observer, id key, void *context, bool match_context) {
    auto owner = state_for(self, false);
    if (!owner) return 0;
    const std::string path = to_utf8(key);
    auto *state = as<Observers>(owner->value);
    // Loading/releasing a weak observer may run guest dealloc. Do that outside
    // the state lock, and keep entries alive until after erasing them.
    auto entries = snapshot(state, path);
    std::vector<std::shared_ptr<Observation>> removed;
    for (auto &entry : entries) {
        id target = kvo_load_weak(&entry->observer);
        const bool matches = target == observer &&
                             (!match_context || entry->context == context);
        release(target);
        if (matches) { entry->active = false; removed.push_back(entry); }
    }
    {
        std::lock_guard lock(state->mutex);
        std::erase_if(state->entries, [&](const auto &entry) {
            return std::find(removed.begin(), removed.end(), entry) != removed.end();
        });
    }
    return 0;
}
std::uint64_t remove_plain(id self, SEL sel, id observer, id key) {
    return remove_observer(self, sel, observer, key, nullptr, false);
}
std::uint64_t remove_context(id self, SEL sel, id observer, id key, void *context) {
    return remove_observer(self, sel, observer, key, context, true);
}
std::uint64_t will_change(id self, SEL, id key) {
    auto owner = state_for(self, false);
    if (!owner) return 0;
    auto *state = as<Observers>(owner->value);
    const std::string path = to_utf8(key);
    auto entries = snapshot(state, path);
    if (entries.empty()) return 0;
    auto old = std::make_shared<Owned>(object_value(self, path));
    {
        std::lock_guard lock(state->mutex);
        state->pending.push_back({path, std::this_thread::get_id(), old});
    }
    deliver(self, entries, old->value, nullptr, true);
    return 0;
}
std::uint64_t did_change(id self, SEL, id key) {
    auto owner = state_for(self, false);
    if (!owner) return 0;
    auto *state = as<Observers>(owner->value);
    const std::string path = to_utf8(key);
    std::shared_ptr<Owned> old;
    {
        std::lock_guard lock(state->mutex);
        for (auto it = state->pending.end(); it != state->pending.begin();) {
            --it;
            if (it->path == path && it->thread == std::this_thread::get_id()) {
                old = it->old;
                state->pending.erase(it);
                break;
            }
        }
    }
    Owned value(object_value(self, path));
    deliver(self, snapshot(state, path), old ? old->value : nullptr, value.value, false);
    return 0;
}
std::uint64_t value_for_key(id self, SEL, id key) { return ret(object_value(self, to_utf8(key))); }
}

void register_key_value() {
    Class base = cls("NSObject");
    observers_class = mm_objc_define_class("__MMKeyValueObservers", base, sizeof(Observers),
        FND_CLASS_REF(__MMKeyValueObservers), FND_META_REF(__MMKeyValueObservers));
    register_lifecycle<Observers>(observers_class);
    FND_METHODS(base,
        {"addObserver:forKeyPath:options:context:", IMP(add_observer), "v48@0:8@16@24Q32^v40"},
        {"removeObserver:forKeyPath:", IMP(remove_plain), "v32@0:8@16@24"},
        {"removeObserver:forKeyPath:context:", IMP(remove_context), "v40@0:8@16@24^v32"},
        {"willChangeValueForKey:", IMP(will_change), "v24@0:8@16"},
        {"didChangeValueForKey:", IMP(did_change), "v24@0:8@16"},
        {"valueForKey:", IMP(value_for_key), "@24@0:8@16"});
}
}

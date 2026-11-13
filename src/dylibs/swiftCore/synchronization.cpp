#include "internal.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>

namespace {
struct once_state { std::mutex mutex; std::condition_variable changed; };
once_state &once_sync() { static once_state state; return state; }

struct alignas(void *) access_record {
    void *pointer;
    const void *pc;
    std::uintptr_t next_and_action;
    access_record *next() const { return reinterpret_cast<access_record *>(next_and_action & ~std::uintptr_t{1}); }
    bool modifies() const { return next_and_action & 1; }
    void set_next(access_record *record) {
        next_and_action = reinterpret_cast<std::uintptr_t>(record) | (next_and_action & 1);
    }
};
static_assert(sizeof(access_record) == 3 * sizeof(void *)); // Swift ValueBuffer
thread_local access_record *accesses = nullptr;

struct once_callback { std::uint64_t function; void *context; };
void invoke_once(void *opaque) {
    auto &value = *static_cast<once_callback *>(opaque);
    const auto *api = mm_swift_api();
    if (!api || !api->call_guest || !api->is_guest_code || !api->is_guest_code(value.function))
        mm_swift_fail("swift_once requires a guest initializer");
    const auto argument = reinterpret_cast<std::uintptr_t>(value.context);
    api->call_guest(value.function, &argument, 1);
}
} // namespace

void mm_swift_once(std::intptr_t *predicate, void (*function)(void *), void *context) {
    if (!predicate || !function) mm_swift_fail("invalid swift_once arguments");
    auto token = std::atomic_ref(*predicate);
    if (token.load(std::memory_order_acquire) == -1) return;
    auto &sync = once_sync();
    std::unique_lock lock(sync.mutex);
    while (token.load(std::memory_order_relaxed) == 1) sync.changed.wait(lock);
    if (token.load(std::memory_order_acquire) == -1) return;
    if (token.load(std::memory_order_relaxed) != 0) mm_swift_fail("invalid swift_once token");
    token.store(1, std::memory_order_relaxed);
    lock.unlock();
    function(context); // permit nested initializers for different predicates
    lock.lock();
    token.store(-1, std::memory_order_release);
    lock.unlock();
    sync.changed.notify_all();
}

extern "C" {
// Darwin's symbol adds a second underscore to the Swift spelling.
unsigned char darwin_swift_disableExclusivityChecking __asm__("__swift_disableExclusivityChecking") SWIFT_EXPORT = 0;

void darwin_swift_once(std::intptr_t *, const void *, void *) SWIFT_DARWIN(swift_once);
void darwin_swift_once(std::intptr_t *predicate, const void *function, void *context) {
    once_callback value{reinterpret_cast<std::uintptr_t>(function), context};
    mm_swift_once(predicate, invoke_once, &value);
}

void darwin_swift_beginAccess(void *, void *, std::uintptr_t, const void *) SWIFT_DARWIN(swift_beginAccess);
void darwin_swift_beginAccess(void *pointer, void *buffer, std::uintptr_t flags, const void *pc) {
    if (!pointer || !buffer || (reinterpret_cast<std::uintptr_t>(buffer) & (alignof(access_record) - 1)))
        mm_swift_fail("invalid exclusivity access buffer");
    auto *record = static_cast<access_record *>(buffer);
    record->pointer = nullptr; // disabled and untracked accesses can be ended
    if (darwin_swift_disableExclusivityChecking) return;
    if (!pc) {
        const auto *api = mm_swift_api();
        if (auto *ctx = api && api->current_context ? api->current_context() : nullptr)
            pc = reinterpret_cast<const void *>(ctx->x[30]);
    }
    for (auto *old = accesses; old; old = old->next()) {
        if (old == record) mm_swift_fail("exclusivity access buffer reused before ending");
        if (old->pointer == pointer && (old->modifies() || (flags & 1))) {
            std::fprintf(stderr, "Swift runtime: simultaneous accesses to %p, previous PC %p, current PC %p\n", pointer, old->pc, pc);
            mm_swift_fail("modification requires exclusive access");
        }
    }
    if (!(flags & 0x20)) return; // check this access without tracking its duration
    record->pointer = pointer;
    record->pc = pc;
    record->next_and_action = reinterpret_cast<std::uintptr_t>(accesses) | (flags & 1);
    accesses = record;
}
void darwin_swift_endAccess(void *) SWIFT_DARWIN(swift_endAccess);
void darwin_swift_endAccess(void *buffer) {
    if (!buffer) mm_swift_fail("null exclusivity access buffer");
    auto *record = static_cast<access_record *>(buffer);
    if (!record->pointer) return;
    access_record *previous = nullptr;
    for (auto *current = accesses; current; previous = current, current = current->next()) {
        if (current != record) continue;
        if (previous) previous->set_next(current->next());
        else accesses = current->next();
        record->pointer = nullptr;
        return;
    }
    mm_swift_fail("exclusivity access ended on the wrong thread");
}
}

/* libdispatch: queues, and the blocks a program puts on them.
 *
 * It lives beside the run loop rather than in libSystem because the main
 * queue *is* the main run loop: a block sent there has to run on the
 * thread that owns the event loop, between events, and that loop is the
 * one in Foundation/AppKit. A dispatch_async whose block never runs is
 * invisible — the work simply does not happen — and an app that schedules
 * showing its window that way (Ableton Live does) waits forever.
 *
 * Blocks are guest objects, so they are invoked through libobjc rather
 * than called as host code. Serial queues get a thread each and the
 * global queues share a small pool, which is what the API promises:
 * ordering within a serial queue, concurrency between queues. */
#include "foundation.h"
#include "machoman/objc_framework.h"
#include "machoman/dispatch_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fnd {
FND_CLASS(OS_dispatch_group);

namespace {

struct callback_scope;
struct queued_callback {
    std::shared_ptr<callback_scope> owner;
    void *context;
    void (*invoke)(void *), (*cancel)(void *);
};
struct callback_scope {
    std::mutex mutex;
    std::condition_variable idle;
    bool stopped = false;
    unsigned running = 0;
    std::vector<queued_callback *> pending;
};
using scope_handle = std::shared_ptr<callback_scope>;
struct dispatch_group;
void group_leave(dispatch_group *);

struct work_item {
    void *block = nullptr;                 /* retained guest block, or */
    void *function = nullptr;              /* a plain function... */
    void *context = nullptr;               /* ...with this context */
    std::shared_ptr<queued_callback> owned{};
    dispatch_group *group = nullptr;
};

void run_owned_callback(queued_callback &item) {
    auto &owner = *item.owner;
    void (*invoke)(void *);
    void *context;
    {
        std::lock_guard lock(owner.mutex);
        if (!item.invoke) return;
        std::erase(owner.pending, &item);
        ++owner.running;
        invoke = item.invoke; context = item.context;
        item.invoke = item.cancel = nullptr; item.context = nullptr;
    }
    struct completion {
        callback_scope &owner;
        ~completion() {
            std::lock_guard lock(owner.mutex);
            if (!--owner.running) owner.idle.notify_all();
        }
    } done{owner};
    mmfw::autorelease_pool pool;
    invoke(context);
}

/* The queue traffic is part of the window-system trace: a block that is
 * queued and never run is invisible otherwise. */
bool dispatch_trace() {
    static const bool on = std::getenv("MACHOMAN_TRACE") || std::getenv("MACHOMAN_APPKIT_TRACE");
    return on;
}

/* A function pointer from the guest has to go through the runtime. */
void call_function(void *fn, const std::uint64_t *args, int n) {
    if (!fn || !api()) return;
    const auto va = reinterpret_cast<std::uintptr_t>(fn);
    if (api()->is_guest_code(va)) api()->call_guest(va, args, n);
    else reinterpret_cast<void (*)(std::uint64_t)>(fn)(args && n ? args[0] : 0);
}

void run_item(const work_item &item) {
    if (item.owned) { run_owned_callback(*item.owned); return; }
    struct completion {
        dispatch_group *group;
        ~completion() { if (group) group_leave(group); }
    } done{item.group};
    mmfw::autorelease_pool pool;
    if (item.block) {
        mm_objc_block_invoke(item.block, 0, nullptr);
        release(reinterpret_cast<id>(item.block));
    } else if (item.function) {
        const std::uint64_t arg = reinterpret_cast<std::uintptr_t>(item.context);
        call_function(item.function, &arg, 1);
    }
}

/* A queue is either the main one (drained by the run loop), a serial one
 * with a thread of its own, or a global one served by a shared pool. */
struct dispatch_queue {
    std::string label;
    bool is_main = false;
    bool concurrent = false;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<work_item> items;
    std::vector<std::thread> workers;
    std::atomic<bool> stop{false};

    void push(const work_item &item) {
        {
            std::lock_guard lock(mu);
            items.push_back(item);
        }
        cv.notify_one();
    }
    bool pop(work_item &out) {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return stop || !items.empty(); });
        if (items.empty()) return false;
        out = items.front();
        items.pop_front();
        return true;
    }
    void serve() {
        for (;;) {
            work_item item;
            if (!pop(item)) return;
            run_item(item);
        }
    }
    void start(int threads) {
        for (int i = 0; i < threads; i++) workers.emplace_back([this] { serve(); });
    }
};

extern "C" EXPORT dispatch_queue mm_dispatch_main_q __asm__("__dispatch_main_q");

dispatch_queue &main_queue() {
    static auto *q = [] {
        auto *queue = &mm_dispatch_main_q;
        queue->label = "com.apple.main-thread";
        queue->is_main = true;
        return queue;
    }();
    return *q;
}

dispatch_queue &global_queue() {
    static auto *q = [] {
        auto *queue = new dispatch_queue;
        queue->label = "com.apple.root.default-qos";
        queue->concurrent = true;
        queue->start(4);
        return queue;
    }();
    return *q;
}

std::vector<std::unique_ptr<dispatch_queue>> &created_queues() {
    static auto *v = new std::vector<std::unique_ptr<dispatch_queue>>;
    return *v;
}
std::mutex &created_mutex() { static auto *m = new std::mutex; return *m; }

struct dispatch_group {
    mm_objc_object base;
    std::mutex mutex;
    std::condition_variable idle;
    std::uint64_t count = 0, generation = 0;
    std::vector<std::pair<dispatch_queue *, work_item>> notifications;
};
Class group_class() {
    static auto *c = [] {
        auto *result = mm_objc_define_class("OS_dispatch_group", cls("NSObject"), sizeof(dispatch_group),
            FND_CLASS_REF(OS_dispatch_group), FND_META_REF(OS_dispatch_group));
        register_lifecycle<dispatch_group>(result);
        return result;
    }();
    return c;
}
void group_enter(dispatch_group *group) {
    retain(reinterpret_cast<id>(group));
    std::lock_guard lock(group->mutex);
    ++group->count;
}
void group_leave(dispatch_group *group) {
    std::vector<std::pair<dispatch_queue *, work_item>> ready;
    {
        std::lock_guard lock(group->mutex);
        if (!group->count) {
            std::fputs("dispatch: unbalanced dispatch_group_leave\n", stderr);
            std::abort();
        }
        if (!--group->count) {
            ++group->generation;
            ready.swap(group->notifications);
            group->idle.notify_all();
        }
    }
    for (const auto &[queue, item] : ready) queue->push(item);
    release(reinterpret_cast<id>(group));
}
void group_notify(dispatch_group *group, dispatch_queue *queue, work_item item) {
    if (!queue) queue = &global_queue();
    {
        std::lock_guard lock(group->mutex);
        if (group->count) { group->notifications.emplace_back(queue, std::move(item)); return; }
    }
    queue->push(item);
}

} // namespace

/* Called by the run loop once per turn: the main queue's blocks run on
 * whichever thread is running the loop, as they do on macOS. */
void dispatch_drain_main_queue() {
    dispatch_queue &q = main_queue();
    for (;;) {
        work_item item;
        {
            std::lock_guard lock(q.mu);
            if (q.items.empty()) return;
            item = q.items.front();
            q.items.pop_front();
        }
        run_item(item);
    }
}

void dispatch_submit_owned_block(void *queue, id block) {
    auto *target = queue ? static_cast<dispatch_queue *>(queue) : &global_queue();
    target->push({block, nullptr, nullptr});
}
void *dispatch_main_queue_handle() { return &main_queue(); }
void dispatch_submit_callback(void *queue, void *function, void *context) {
    auto *target = queue ? static_cast<dispatch_queue *>(queue) : &global_queue();
    target->push({nullptr, function, context});
}

} // namespace fnd

using namespace fnd;

extern "C" {

EXPORT void *mm_dispatch_scope_create() { return new scope_handle(std::make_shared<callback_scope>()); }
EXPORT void mm_dispatch_scope_async(void *handle, void *queue, void *context,
                                    void (*invoke)(void *), void (*cancel)(void *)) {
    const auto owner = *static_cast<scope_handle *>(handle);
    auto callback = std::make_shared<queued_callback>(queued_callback{owner, context, invoke, cancel});
    {
        std::lock_guard lock(owner->mutex);
        if (!owner->stopped) {
            owner->pending.push_back(callback.get());
            work_item item; item.owned = std::move(callback);
            (queue ? *static_cast<dispatch_queue *>(queue) : global_queue()).push(item);
            return;
        }
    }
    if (cancel) cancel(context);
}
EXPORT void mm_dispatch_scope_stop(void *handle) {
    const auto owner = *static_cast<scope_handle *>(handle);
    std::vector<std::pair<void (*)(void *), void *>> cancelled;
    {
        std::lock_guard lock(owner->mutex);
        owner->stopped = true;
        for (auto *item : owner->pending) {
            if (item->cancel) cancelled.emplace_back(item->cancel, item->context);
            item->invoke = item->cancel = nullptr; item->context = nullptr;
        }
        owner->pending.clear();
    }
    // Releasing a captured guest block can call back into the framework.
    for (const auto &[cancel, context] : cancelled) cancel(context);
    std::unique_lock lock(owner->mutex);
    owner->idle.wait(lock, [&] { return owner->running == 0; });
}
EXPORT void mm_dispatch_scope_destroy(void *handle) {
    if (!handle) return;
    mm_dispatch_scope_stop(handle);
    delete static_cast<scope_handle *>(handle);
}

/* dispatch_get_main_queue() is a macro over this symbol, so the guest
 * binds to the object itself rather than calling anything. */
EXPORT dispatch_queue mm_dispatch_main_q __asm__("__dispatch_main_q");

dispatch_queue *darwin_dispatch_get_main_queue() DARWIN(dispatch_get_main_queue);
dispatch_queue *darwin_dispatch_get_main_queue() { return &main_queue(); }

dispatch_queue *darwin_dispatch_get_global_queue(long, unsigned long) DARWIN(dispatch_get_global_queue);
dispatch_queue *darwin_dispatch_get_global_queue(long, unsigned long) { return &global_queue(); }

dispatch_group *darwin_dispatch_group_create() DARWIN(dispatch_group_create);
dispatch_group *darwin_dispatch_group_create() { return make<dispatch_group>(group_class()); }
void darwin_dispatch_group_enter(dispatch_group *group) DARWIN(dispatch_group_enter);
void darwin_dispatch_group_enter(dispatch_group *group) { group_enter(group); }
void darwin_dispatch_group_leave(dispatch_group *group) DARWIN(dispatch_group_leave);
void darwin_dispatch_group_leave(dispatch_group *group) { group_leave(group); }
long darwin_dispatch_group_wait(dispatch_group *group, std::uint64_t timeout) DARWIN(dispatch_group_wait);
long darwin_dispatch_group_wait(dispatch_group *group, std::uint64_t timeout) {
    std::unique_lock lock(group->mutex);
    if (!group->count) return 0;
    if (!timeout) return 1;
    const auto generation = group->generation;
    auto done = [&] { return group->generation != generation; };
    if (timeout == ~0ull) { group->idle.wait(lock, done); return 0; }
    const auto deadline = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(
        static_cast<std::int64_t>(std::min(timeout, std::uint64_t(std::numeric_limits<std::int64_t>::max())))));
    return group->idle.wait_until(lock, deadline, done) ? 0 : 1;
}
void darwin_dispatch_group_async(dispatch_group *group, dispatch_queue *queue, void *block) DARWIN(dispatch_group_async);
void darwin_dispatch_group_async(dispatch_group *group, dispatch_queue *queue, void *block) {
    if (!block) return;
    work_item item{retain(static_cast<id>(block)), nullptr, nullptr};
    group_enter(group); item.group = group;
    (queue ? *queue : global_queue()).push(item);
}
void darwin_dispatch_group_async_f(dispatch_group *group, dispatch_queue *queue, void *context, void *function) DARWIN(dispatch_group_async_f);
void darwin_dispatch_group_async_f(dispatch_group *group, dispatch_queue *queue, void *context, void *function) {
    if (!function) return;
    work_item item{nullptr, function, context};
    group_enter(group); item.group = group;
    (queue ? *queue : global_queue()).push(item);
}
void darwin_dispatch_group_notify(dispatch_group *group, dispatch_queue *queue, void *block) DARWIN(dispatch_group_notify);
void darwin_dispatch_group_notify(dispatch_group *group, dispatch_queue *queue, void *block) {
    if (block) group_notify(group, queue, {retain(static_cast<id>(block)), nullptr, nullptr});
}
void darwin_dispatch_group_notify_f(dispatch_group *group, dispatch_queue *queue, void *context, void *function) DARWIN(dispatch_group_notify_f);
void darwin_dispatch_group_notify_f(dispatch_group *group, dispatch_queue *queue, void *context, void *function) {
    if (function) group_notify(group, queue, {nullptr, function, context});
}

dispatch_queue *darwin_dispatch_queue_create(const char *label, void *attr) DARWIN(dispatch_queue_create);
dispatch_queue *darwin_dispatch_queue_create(const char *label, void *attr) {
    auto queue = std::make_unique<dispatch_queue>();
    queue->label = label ? label : "";
    /* A null attribute means serial; anything else is treated as
     * concurrent, which only relaxes ordering the caller said it did not
     * need. */
    queue->concurrent = attr != nullptr;
    queue->start(queue->concurrent ? 2 : 1);
    dispatch_queue *raw = queue.get();
    std::lock_guard lock(created_mutex());
    created_queues().push_back(std::move(queue));
    return raw;
}

const char *darwin_dispatch_queue_get_label(dispatch_queue *queue) DARWIN(dispatch_queue_get_label);
const char *darwin_dispatch_queue_get_label(dispatch_queue *queue) {
    return queue ? queue->label.c_str() : "";
}

void darwin_dispatch_async(dispatch_queue *queue, void *block) DARWIN(dispatch_async);
void darwin_dispatch_async(dispatch_queue *queue, void *block) {
    if (!block) return;
    if (dispatch_trace())
        std::fprintf(stderr, "dispatch: async on %s\n", (queue ? *queue : main_queue()).label.c_str());
    work_item item{retain(reinterpret_cast<id>(block)), nullptr, nullptr};
    (queue ? *queue : main_queue()).push(item);
}

void darwin_dispatch_async_f(dispatch_queue *queue, void *context, void *function) DARWIN(dispatch_async_f);
void darwin_dispatch_async_f(dispatch_queue *queue, void *context, void *function) {
    if (!function) return;
    (queue ? *queue : main_queue()).push({nullptr, function, context});
}

/* dispatch_sync runs the block where the caller is: the work has to be
 * finished when it returns, and running it here is both the simplest way
 * to guarantee that and the one that cannot deadlock against the main
 * queue (which is drained by the caller's own run loop). */
void darwin_dispatch_sync(dispatch_queue *, void *block) DARWIN(dispatch_sync);
void darwin_dispatch_sync(dispatch_queue *, void *block) {
    if (block) mm_objc_block_invoke(block, 0, nullptr);
}

void darwin_dispatch_sync_f(dispatch_queue *, void *context, void *function) DARWIN(dispatch_sync_f);
void darwin_dispatch_sync_f(dispatch_queue *, void *context, void *function) {
    if (!function) return;
    const std::uint64_t arg = reinterpret_cast<std::uintptr_t>(context);
    call_function(function, &arg, 1);
}

/* dispatch_after: the delay is honoured by a thread that waits and then
 * hands the block to the queue, so ordering on that queue still holds. */
void darwin_dispatch_after(std::uint64_t when, dispatch_queue *queue, void *block) DARWIN(dispatch_after);
void darwin_dispatch_after(std::uint64_t when, dispatch_queue *queue, void *block) {
    if (!block) return;
    /* `when` is a dispatch_time_t: nanoseconds since boot, as
     * libSystem's dispatch_time builds it. */
    id retained = retain(reinterpret_cast<id>(block));
    dispatch_queue *target = queue ? queue : &main_queue();
    std::thread([when, target, retained] {
        const auto now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        if (when > now) std::this_thread::sleep_for(std::chrono::nanoseconds(when - now));
        target->push({retained, nullptr, nullptr});
    }).detach();
}

/* An assertion a program makes about which queue it is on; there is one
 * main thread here and the loop drains the main queue on it, so it holds. */
void darwin_dispatch_assert_queue(dispatch_queue *) DARWIN(dispatch_assert_queue$V2);
void darwin_dispatch_assert_queue(dispatch_queue *) {}

void darwin_dispatch_apply(std::size_t iterations, dispatch_queue *, void *block) DARWIN(dispatch_apply);
void darwin_dispatch_apply(std::size_t iterations, dispatch_queue *, void *block) {
    /* Run in order on this thread: dispatch_apply must not return before
     * every iteration has finished, and the iterations are independent. */
    for (std::size_t i = 0; i < iterations; i++) {
        const std::uint64_t index = i;
        if (block) mm_objc_block_invoke(block, 1, &index);
    }
}

} // extern "C"

#include "machoman/dispatch_host.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

extern "C" void *main_queue() __asm__("_dispatch_get_main_queue");
extern "C" void *serial_queue(const char *, void *) __asm__("_dispatch_queue_create");
static void check(bool ok) { if (!ok) std::abort(); }
struct state {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned invoked = 0, cancelled = 0;
    bool release = false;
};
static void cancel(void *opaque) {
    auto &s = *static_cast<state *>(opaque);
    std::lock_guard lock(s.mutex); ++s.cancelled; s.changed.notify_all();
}
static void invoke(void *opaque) {
    auto &s = *static_cast<state *>(opaque);
    std::unique_lock lock(s.mutex);
    ++s.invoked; s.changed.notify_all();
    s.changed.wait(lock, [&] { return s.release; });
}
int main() {
    state s;
    void *scope = mm_dispatch_scope_create();
    void *queue = serial_queue("scope-test", nullptr);
    mm_dispatch_scope_async(scope, queue, &s, invoke, cancel);
    {
        std::unique_lock lock(s.mutex);
        check(s.changed.wait_for(lock, std::chrono::seconds(2), [&] { return s.invoked == 1; }));
    }
    mm_dispatch_scope_async(scope, queue, &s, invoke, cancel);
    mm_dispatch_scope_async(scope, main_queue(), &s, invoke, cancel);
    std::atomic<bool> stopped{false};
    std::thread stop([&] { mm_dispatch_scope_stop(scope); stopped = true; });
    {
        std::unique_lock lock(s.mutex);
        check(s.changed.wait_for(lock, std::chrono::seconds(2), [&] { return s.cancelled == 2; }));
        check(!stopped); s.release = true; s.changed.notify_all();
    }
    stop.join();
    mm_dispatch_scope_async(scope, queue, &s, invoke, cancel);
    mm_dispatch_scope_destroy(scope);
    // The serial worker must pass the cancelled item before this sentinel.
    state sentinel; sentinel.release = true;
    scope = mm_dispatch_scope_create();
    mm_dispatch_scope_async(scope, queue, &sentinel, invoke, cancel);
    {
        std::unique_lock lock(sentinel.mutex);
        check(sentinel.changed.wait_for(lock, std::chrono::seconds(2), [&] { return sentinel.invoked == 1; }));
    }
    mm_dispatch_scope_destroy(scope);
    check(s.invoked == 1 && s.cancelled == 3);
    std::puts("Host dispatch cancellation and in-flight callback draining passed");
}

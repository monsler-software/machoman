#include "notification_dispatch.h"
#include "machoman/dispatch_host.h"
#include <atomic>

namespace mm::audio {
namespace {
std::atomic<void *> live_scope{nullptr};
struct notification_scope {
    void *handle = mm_dispatch_scope_create();
    notification_scope() { live_scope.store(handle, std::memory_order_release); }
    ~notification_scope() {
        live_scope.store(nullptr, std::memory_order_release);
        mm_dispatch_scope_destroy(handle);
    }
};
}
void dispatch_audio_notification(void *queue, void *context, void (*invoke)(void *), void (*cancel)(void *)) {
    static notification_scope scope;
    mm_dispatch_scope_async(scope.handle, queue, context, invoke, cancel);
}
void quiesce_audio_notifications() {
    if (auto *scope = live_scope.load(std::memory_order_acquire)) mm_dispatch_scope_stop(scope);
}
}

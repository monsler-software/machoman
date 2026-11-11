#include "machoman/objc_host.h"
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <thread>

namespace {
std::thread worker;
std::uint64_t wait_for_other_thread(mm_id self, mm_SEL) {
    auto completed = std::make_shared<std::promise<void>>();
    auto ready = completed->get_future();
    worker = std::thread([self, completed] {
        mm_objc_send0(self, "retain");
        mm_objc_send0(self, "release");
        completed->set_value();
    });
    /* The IMP must permit another thread to enter the Objective-C runtime.
     * A bounded wait lets the old implementation fail without hanging. */
    return ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
}
}

int main() {
    mm_objc_class storage{}, metaclass{};
    auto *cls = mm_objc_define_class("ConcurrencyProbe", mm_objc_class_named("NSObject"),
        sizeof(mm_objc_object), &storage, &metaclass);
    mm_objc_add_method(cls, "waitForOtherThread", reinterpret_cast<void *>(&wait_for_other_thread), "Q16@0:8");
    auto *object = mm_objc_alloc(cls, 0);
    const auto completed = mm_objc_send0(object, "waitForOtherThread");
    worker.join();
    mm_objc_release(object);
    if (!completed) {
        std::fputs("Objective-C method held the runtime lock while waiting for another thread\n", stderr);
        return 1;
    }
    std::puts("Objective-C methods allow concurrent runtime entry");
    return 0;
}

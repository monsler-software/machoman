#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "dispatch group check failed at %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static atomic_uint calls, notifications;
static dispatch_semaphore_t notification_ready;
static void work(void *context) { atomic_fetch_add(&calls, (unsigned)(unsigned long)context); }
static void notify(void *context) {
    atomic_fetch_add(&notifications, (unsigned)(unsigned long)context);
    dispatch_semaphore_signal(notification_ready);
}
static void notify_with_capture(dispatch_group_t group, dispatch_queue_t queue, unsigned value) {
    dispatch_group_notify(group, queue, ^{ notify((void *)(unsigned long)value); });
}
int main(void) {
    dispatch_queue_t queue = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
    dispatch_group_t group = dispatch_group_create(); CHECK(group);
    notification_ready = dispatch_semaphore_create(0);
    CHECK(!dispatch_group_wait(group, DISPATCH_TIME_NOW));
    dispatch_group_enter(group);
    CHECK(dispatch_group_wait(group, DISPATCH_TIME_NOW));
    CHECK(dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW, 10000000)));
    notify_with_capture(group, queue, 2);
    dispatch_group_notify_f(group, queue, (void *)3, notify);
    CHECK(!atomic_load(&notifications));
    for (unsigned i = 0; i < 24; ++i) {
        dispatch_group_async_f(group, queue, (void *)1, work);
        dispatch_group_async(group, queue, ^{ dispatch_assert_queue(queue); work((void *)2); });
    }
    dispatch_group_leave(group);
    CHECK(!dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW, 2000000000)));
    CHECK(atomic_load(&calls) == 72);
    CHECK(!dispatch_semaphore_wait(notification_ready, dispatch_time(DISPATCH_TIME_NOW, 2000000000)));
    CHECK(!dispatch_semaphore_wait(notification_ready, dispatch_time(DISPATCH_TIME_NOW, 2000000000)));
    CHECK(atomic_load(&notifications) == 5);
    // Reuse a completed group and notify an already-empty group.
    dispatch_group_enter(group); dispatch_group_leave(group);
    CHECK(!dispatch_group_wait(group, DISPATCH_TIME_NOW));
    notify_with_capture(group, queue, 7);
    CHECK(!dispatch_semaphore_wait(notification_ready, dispatch_time(DISPATCH_TIME_NOW, 2000000000)));
    CHECK(atomic_load(&notifications) == 12);
    dispatch_release(group);
    // Outstanding work retains the group after its external reference is gone.
    dispatch_semaphore_t gate = dispatch_semaphore_create(0);
    group = dispatch_group_create();
    dispatch_group_async(group, queue, ^{ dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER); });
    notify_with_capture(group, queue, 11);
    dispatch_release(group);
    dispatch_semaphore_signal(gate);
    CHECK(!dispatch_semaphore_wait(notification_ready, dispatch_time(DISPATCH_TIME_NOW, 2000000000)));
    CHECK(atomic_load(&notifications) == 23);
    dispatch_release(gate); dispatch_release(notification_ready);
    puts("Dispatch groups, deadlines, captured notifications and retention passed");
    return 0;
}

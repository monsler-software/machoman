#include <mach/mach.h>
#include <mach/notify.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mach_port_t endpoint;
struct packet { mach_msg_header_t header; unsigned payload; mach_msg_trailer_t trailer; };
static void check(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "Mach ports: %s\n", message); abort(); }
}
static void *receive(void *unused) {
    (void)unused;
    struct packet p = {0};
    check(mach_msg(&p.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof p, endpoint, 2000, MACH_PORT_NULL) == KERN_SUCCESS,
          "blocking receive from another thread");
    check(p.header.msgh_id == 42 && p.header.msgh_local_port == endpoint && p.payload == 123, "header and payload");
    return NULL;
}
int main(void) {
    check(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &endpoint) == KERN_SUCCESS && endpoint, "allocate receive right");
    mach_port_t send;
    mach_msg_type_name_t kind;
    check(mach_port_extract_right(mach_task_self(), endpoint, MACH_MSG_TYPE_MAKE_SEND, &send, &kind) == KERN_SUCCESS &&
          send == endpoint && kind == MACH_MSG_TYPE_PORT_SEND, "extract/coalesce send right");
    mach_port_urefs_t refs;
    check(mach_port_get_refs(mach_task_self(), send, MACH_PORT_RIGHT_SEND, &refs) == KERN_SUCCESS && refs == 1, "send references");
    mach_port_t once_a, once_b;
    check(mach_port_extract_right(mach_task_self(), endpoint, MACH_MSG_TYPE_MAKE_SEND_ONCE, &once_a, &kind) == KERN_SUCCESS &&
          kind == MACH_MSG_TYPE_PORT_SEND_ONCE, "extract send-once right");
    check(mach_port_extract_right(mach_task_self(), endpoint, MACH_MSG_TYPE_MAKE_SEND_ONCE, &once_b, &kind) == KERN_SUCCESS &&
          once_a != once_b && once_a != endpoint && once_b != endpoint, "send-once rights have distinct names");
    check(mach_port_deallocate(mach_task_self(), once_a) == KERN_SUCCESS &&
          mach_port_deallocate(mach_task_self(), once_b) == KERN_SUCCESS, "release send-once names");
    for (unsigned i = 0; i < 2; ++i) {
        mach_send_once_notification_t notification = {0};
        check(mach_msg(&notification.not_header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof notification,
                      endpoint, 1000, MACH_PORT_NULL) == KERN_SUCCESS &&
              notification.not_header.msgh_id == MACH_NOTIFY_SEND_ONCE, "released send-once right notification");
    }
    pthread_t thread;
    check(pthread_create(&thread, NULL, receive, NULL) == 0, "receiver thread");
    struct packet p = {0};
    p.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    p.header.msgh_size = sizeof p.header + sizeof p.payload;
    p.header.msgh_remote_port = send; p.header.msgh_id = 42; p.payload = 123;
    const mach_msg_size_t send_size = p.header.msgh_size;
    p.header.msgh_size = 0; /* MIG passes send_size without initializing this field. */
    check(mach_msg(&p.header, MACH_SEND_MSG, send_size, 0, 0, 0, 0) == KERN_SUCCESS, "inline send with authoritative send_size");
    check(pthread_join(thread, NULL) == 0, "receiver finished");
    check(mach_msg(&p.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof p, endpoint, 1, 0) == MACH_RCV_TIMED_OUT, "receive timeout");
    check(mach_port_mod_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_RECEIVE, -1) == KERN_SUCCESS, "destroy receive right");
    check(mach_port_get_refs(mach_task_self(), send, MACH_PORT_RIGHT_DEAD_NAME, &refs) == KERN_SUCCESS && refs == 1, "send becomes dead name");
    check(mach_port_deallocate(mach_task_self(), send) == KERN_SUCCESS, "deallocate dead name");
    check(mach_port_get_refs(mach_task_self(), send, MACH_PORT_RIGHT_DEAD_NAME, &refs) == KERN_INVALID_NAME, "name removed");
    puts("Mach local port rights, messages, timeout and destruction passed");
    return 0;
}

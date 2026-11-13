#include <mach/mach.h>
#include <mach/notify.h>
#include <mach/mach_error.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void success(kern_return_t status, const char *message) {
    if (status) { fprintf(stderr, "Mach notifications: %s (0x%x)\n", message, status); exit(1); }
}
static void check(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "Mach notifications: %s\n", message); exit(1); }
}
static mach_port_t allocate(void) {
    mach_port_t port;
    success(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port), "allocate receive");
    return port;
}
static void receive(void *buffer, unsigned size, mach_port_t port) {
    memset(buffer, 0, size);
    success(mach_msg(buffer, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, size, port, 1000, 0), "receive notification");
}
static mach_port_t request(mach_port_t port, int id, unsigned sync, mach_port_t destination) {
    mach_port_t previous;
    success(mach_port_request_notification(mach_task_self(), port, id, sync, destination,
        MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous), "request notification");
    return previous;
}
static void guarded_ports(void) {
    mach_port_options_t options = {0};
    options.flags = MPO_CONTEXT_AS_GUARD | MPO_STRICT | MPO_INSERT_SEND_RIGHT | MPO_QLIMIT;
    options.mpl.mpl_qlimit = 2;
    mach_port_t port;
    success(mach_port_construct(mach_task_self(), &options, 0, &port), "construct zero-valued strict guard");
    mach_port_status_t status;
    mach_msg_type_number_t count = MACH_PORT_RECEIVE_STATUS_COUNT;
    success(mach_port_get_attributes(mach_task_self(), port, MACH_PORT_RECEIVE_STATUS, (mach_port_info_t)&status, &count), "guarded status");
    check(status.mps_qlimit == 2 && status.mps_srights &&
          (status.mps_flags & (MACH_PORT_STATUS_FLAG_GUARDED | MACH_PORT_STATUS_FLAG_STRICT_GUARD)) ==
          (MACH_PORT_STATUS_FLAG_GUARDED | MACH_PORT_STATUS_FLAG_STRICT_GUARD), "SDK construct options and status flags");
    check(mach_port_set_context(mach_task_self(), port, 1) == KERN_INVALID_ARGUMENT, "strict context mutation rejected");
    check(mach_port_destruct(mach_task_self(), port, -1, 1) == KERN_INVALID_ARGUMENT, "wrong guard leaves rights intact");
    success(mach_port_destruct(mach_task_self(), port, -1, 0), "destruct matching zero guard and send reference");
    check(mach_port_type(mach_task_self(), port, &count) == KERN_INVALID_NAME, "destruct removed all rights");

    options.flags = MPO_CONTEXT_AS_GUARD;
    success(mach_port_construct(mach_task_self(), &options, 0x1020304050607080ull, &port), "construct guarded movable receive");
    mach_port_t destination = allocate();
    success(mach_port_insert_right(mach_task_self(), destination, destination, MACH_MSG_TYPE_MAKE_SEND), "descriptor destination");
    struct {
        mach_msg_header_t header;
        mach_msg_body_t body;
        mach_msg_guarded_port_descriptor_t descriptor;
        mach_msg_trailer_t trailer;
    } message = {0};
    message.header.msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    message.header.msgh_size = offsetof(__typeof__(message), trailer);
    message.header.msgh_remote_port = destination;
    message.body.msgh_descriptor_count = 1;
    message.descriptor.name = port;
    message.descriptor.disposition = MACH_MSG_TYPE_MOVE_RECEIVE;
    message.descriptor.type = MACH_MSG_GUARDED_PORT_DESCRIPTOR;
    message.descriptor.flags = MACH_MSG_GUARD_FLAGS_IMMOVABLE_RECEIVE;
    message.descriptor.context = 1;
    check(mach_msg_send(&message.header) == MACH_SEND_INVALID_CONTEXT, "wrong descriptor guard rejected atomically");
    message.descriptor.context = 0x1020304050607080ull;
    success(mach_msg_send(&message.header), "guarded receive right transfer");
    receive(&message, sizeof message, destination);
    check(message.descriptor.context == (mach_port_context_t)&message &&
          !(message.descriptor.flags & MACH_MSG_GUARD_FLAGS_UNGUARDED_ON_SEND), "receiver guard context is message address");
    count = MACH_PORT_RECEIVE_STATUS_COUNT;
    success(mach_port_get_attributes(mach_task_self(), message.descriptor.name, MACH_PORT_RECEIVE_STATUS,
        (mach_port_info_t)&status, &count), "transferred guard status");
    check(status.mps_flags & MACH_PORT_STATUS_FLAG_GUARD_IMMOVABLE_RECEIVE, "received immovable guard");
    const mach_port_t imported = message.descriptor.name;
    mach_msg_destroy(&message.header);
    check(mach_port_type(mach_task_self(), imported, &count) == KERN_INVALID_NAME, "destroy unguards and releases receive descriptor");
    success(mach_port_destroy(mach_task_self(), destination), "destroy descriptor destination");
}
static void exception_ports(void) {
    mach_port_t task, handler = allocate(), replacement = allocate(), notify = allocate();
    success(task_for_pid(mach_task_self(), getpid(), &task), "resolve current task by PID");
    check(task == mach_task_self(), "current task identity coalesces");
    success(mach_port_insert_right(mach_task_self(), handler, handler, MACH_MSG_TYPE_MAKE_SEND), "exception handler send right");
    success(mach_port_insert_right(mach_task_self(), replacement, replacement, MACH_MSG_TYPE_MAKE_SEND), "replacement handler send right");
    const exception_mask_t mask = EXC_MASK_BAD_ACCESS | EXC_MASK_BREAKPOINT;
    success(task_set_exception_ports(task, mask, handler, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, THREAD_STATE_NONE), "set task exception ports");
    request(handler, MACH_NOTIFY_NO_SENDERS, 1, notify);
    success(mach_port_deallocate(mach_task_self(), handler), "release user's handler send reference");
    mach_port_t thread = mach_thread_self(), thread_again = mach_thread_self();
    check(thread == thread_again, "thread port identity");
    exception_mask_t masks[8]; mach_port_t handlers[8]; exception_behavior_t behaviors[8]; thread_state_flavor_t flavors[8];
    mach_msg_type_number_t count = 8;
    success(thread_get_exception_ports(thread, mask, masks, &count, handlers, behaviors, flavors), "independent thread exception table");
    check(count == 1 && handlers[0] == MACH_PORT_NULL, "task handler does not overwrite thread table");
    count = 8;
    success(task_get_exception_ports(task, mask, masks, &count, handlers, behaviors, flavors), "get grouped task exceptions");
    check(count == 1 && masks[0] == mask && handlers[0] == handler &&
          behaviors[0] == (EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES) && flavors[0] == THREAD_STATE_NONE,
          "SDK exception arrays and combined masks");
    success(mach_port_deallocate(mach_task_self(), handlers[0]), "release get's imported handler right");
    count = 0;
    check(task_swap_exception_ports(task, mask, replacement, EXCEPTION_DEFAULT, THREAD_STATE_NONE,
        masks, &count, handlers, behaviors, flavors) == KERN_NO_SPACE, "small swap output leaves handler unchanged");
    count = 8;
    success(task_swap_exception_ports(task, mask, replacement, EXCEPTION_DEFAULT, THREAD_STATE_NONE,
        masks, &count, handlers, behaviors, flavors), "atomic exception swap with ten arguments");
    check(count == 1 && handlers[0] == handler, "swap returns original handler");
    success(mach_port_deallocate(mach_task_self(), handlers[0]), "release swap's old handler reference");
    mach_no_senders_notification_t empty;
    receive(&empty, sizeof empty, notify);
    check(empty.not_header.msgh_id == MACH_NOTIFY_NO_SENDERS, "exception table kept right alive until replacement");
    success(task_set_exception_ports(task, mask, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE), "clear task handlers");
    success(mach_port_destroy(mach_task_self(), handler), "destroy old handler");
    success(mach_port_destroy(mach_task_self(), replacement), "destroy replacement handler");
    success(mach_port_destroy(mach_task_self(), notify), "destroy handler notification endpoint");
    success(mach_port_deallocate(mach_task_self(), thread), "release thread right");
    success(mach_port_deallocate(mach_task_self(), thread_again), "release second thread right");
    success(mach_port_deallocate(mach_task_self(), task), "release task right");
}
int main(void) {
    check(vm_page_size && !(vm_page_size & (vm_page_size - 1)) && vm_page_mask == vm_page_size - 1,
          "exported Darwin VM page constants");
    check(strstr(mach_error_string(MACH_SEND_INVALID_RIGHT), "right") != NULL, "Mach diagnostic error string");
    check(NDR_record.int_rep == 1 && NDR_record.char_rep == 0 && NDR_record.float_rep == 0,
          "exported MIG NDR record matches SDK integer, character and float encoding");
    guarded_ports();
    exception_ports();
    mach_port_t port = allocate(), notify = allocate();
    check(request(port, MACH_NOTIFY_NO_SENDERS, 0, notify) == 0, "first subscription");
    mach_no_senders_notification_t empty;
    receive(&empty, sizeof empty, notify);
    check(empty.not_header.msgh_id == MACH_NOTIFY_NO_SENDERS && empty.not_count == 0, "immediate no-senders");

    success(mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND), "make send right");
    check(request(port, MACH_NOTIFY_NO_SENDERS, 1, notify) == 0, "subscribe while send right exists");
    mach_port_t previous = request(port, MACH_NOTIFY_NO_SENDERS, 1, notify);
    check(previous != 0, "replacement returns old send-once right");
    success(mach_port_deallocate(mach_task_self(), previous), "release replaced send-once subscription");
    mach_send_once_notification_t once;
    receive(&once, sizeof once, notify);
    check(once.not_header.msgh_id == MACH_NOTIFY_SEND_ONCE, "replaced request has real send-once ownership");
    success(mach_port_deallocate(mach_task_self(), port), "release last send right");
    receive(&empty, sizeof empty, notify);
    check(empty.not_header.msgh_id == MACH_NOTIFY_NO_SENDERS && empty.not_count == 1, "last sender notification");

    success(mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND), "make another send right");
    check(request(port, MACH_NOTIFY_DEAD_NAME, 0, notify) == 0, "subscribe to death");
    success(mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1), "destroy receive right");
    mach_dead_name_notification_t dead;
    receive(&dead, sizeof dead, notify);
    check(dead.not_header.msgh_id == MACH_NOTIFY_DEAD_NAME && dead.not_port == port && dead.NDR.int_rep == 1,
          "SDK death notification payload");
    mach_port_urefs_t refs;
    success(mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_DEAD_NAME, &refs), "death references");
    check(refs == 2, "notification holds additional dead-name reference");
    check(request(port, MACH_NOTIFY_DEAD_NAME, 1, notify) == 0, "immediate death subscription");
    receive(&dead, sizeof dead, notify);
    success(mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_DEAD_NAME, &refs), "immediate death references");
    check(refs == 3, "immediate death adds reference");
    success(mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_DEAD_NAME, -3), "release dead name");

    port = allocate();
    check(request(port, MACH_NOTIFY_DEAD_NAME, 0, notify) == 0, "subscribe to name deletion");
    success(mach_port_allocate_name(mach_task_self(), MACH_PORT_RIGHT_DEAD_NAME, 0x7003), "reserve unused name");
    success(mach_port_deallocate(mach_task_self(), 0x7003), "release reserved name");
    success(mach_port_rename(mach_task_self(), port, 0x7003), "rename subscribed right");
    success(mach_port_destroy(mach_task_self(), 0x7003), "destroy subscribed name");
    receive(&dead, sizeof dead, notify);
    check(dead.not_header.msgh_id == MACH_NOTIFY_DEAD_NAME && dead.not_port == 0x7003, "death follows renamed local name");

    /* A port-destroyed request backs up the receive right, preserving its
     * queued messages. It does not turn surviving send rights into dead names. */
    port = allocate();
    success(mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND), "backup send right");
    check(request(port, MACH_NOTIFY_PORT_DESTROYED, 0, notify) == 0, "register receive-right backup");
    struct { mach_msg_header_t header; unsigned value; mach_msg_trailer_t trailer; } message = {0};
    message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    message.header.msgh_size = sizeof message.header + sizeof message.value;
    message.header.msgh_remote_port = port; message.header.msgh_id = 77; message.value = 42;
    success(mach_msg_send(&message.header), "queue message before backing up receive right");
    success(mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1), "move receive into backup notification");
    mach_port_destroyed_notification_t backup;
    receive(&backup, sizeof backup, notify);
    check(backup.not_header.msgh_id == MACH_NOTIFY_PORT_DESTROYED && backup.not_port.disposition == MACH_MSG_TYPE_PORT_RECEIVE,
          "SDK backup descriptor layout");
    check(backup.not_port.name == port, "backup receive right coalesces with existing send right");
    receive(&message, sizeof message, backup.not_port.name);
    check(message.header.msgh_id == 77 && message.value == 42, "backup preserves queued data");
    mach_msg_destroy(&backup.not_header);
    success(mach_port_deallocate(mach_task_self(), port), "release backup dead name");

    /* Send-possible is armed on a full queue and wakes when space appears. */
    port = allocate();
    success(mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND), "queue send right");
    mach_port_limits_t limits = {1};
    success(mach_port_set_attributes(mach_task_self(), port, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits,
        MACH_PORT_LIMITS_INFO_COUNT), "set queue limit");
    memset(&message, 0, sizeof message);
    message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    message.header.msgh_size = sizeof message.header;
    message.header.msgh_remote_port = port; message.header.msgh_id = 78;
    success(mach_msg_send(&message.header), "fill queue");
    check(request(port, MACH_NOTIFY_SEND_POSSIBLE, 1, notify) == 0, "arm send-possible request");
    check(mach_msg(&message.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, message.header.msgh_size, 0, 0, 0, 0) == MACH_SEND_TIMED_OUT,
          "full queue send timeout");
    receive(&message, sizeof message, port);
    mach_send_possible_notification_t possible;
    receive(&possible, sizeof possible, notify);
    check(possible.not_header.msgh_id == MACH_NOTIFY_SEND_POSSIBLE && possible.not_port == port, "queue capacity notification");
    /* Send-once replies bypass the ordinary queue limit. */
    memset(&message, 0, sizeof message);
    message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    message.header.msgh_size = sizeof message.header; message.header.msgh_remote_port = port;
    success(mach_msg_send(&message.header), "refill queue");
    mach_msg_type_name_t kind;
    success(mach_port_extract_right(mach_task_self(), port, MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous, &kind), "extract one-shot reply");
    message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    message.header.msgh_remote_port = previous;
    success(mach_msg(&message.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, message.header.msgh_size, 0, 0, 0, 0),
        "send-once delivery to full queue");
    success(mach_port_destroy(mach_task_self(), port), "destroy queued messages");
    success(mach_port_destroy(mach_task_self(), notify), "destroy notification endpoint");
    puts("SDK Mach notifications, subscription replacement, dead references, backup rights and queue capacity passed");
    return 0;
}

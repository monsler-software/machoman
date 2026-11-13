#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/notify.h>
#include <servers/bootstrap.h>
#include <spawn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#pragma pack(push, 4)
struct request_message {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t memory;
    mach_msg_ool_descriptor_t bytes;
    mach_msg_ool_ports_descriptor_t ports;
    unsigned value;
};
struct reply_message {
    mach_msg_header_t header;
    unsigned value;
};
struct request_buffer { struct request_message message; mach_msg_context_trailer_t trailer; };
struct reply_buffer { struct reply_message message; mach_msg_trailer_t trailer; };
#pragma pack(pop)
_Static_assert(sizeof(mach_msg_header_t) == 24, "Mach header ABI");
_Static_assert(sizeof(mach_msg_port_descriptor_t) == 12, "Mach port descriptor ABI");
_Static_assert(sizeof(mach_msg_ool_descriptor_t) == 16, "Mach OOL descriptor ABI");
_Static_assert(offsetof(struct request_message, ports) == 56, "mixed descriptor layout");
_Static_assert(sizeof(mach_msg_context_trailer_t) == 60, "Mach context trailer ABI");

static void check(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "Mach exchange: %s\n", message); exit(1); }
}
static void success(kern_return_t status, const char *message) {
    if (status) { fprintf(stderr, "Mach exchange: %s (0x%x)\n", message, status); exit(1); }
}
static mach_port_t allocate(mach_port_right_t kind) {
    mach_port_t port;
    success(mach_port_allocate(mach_task_self(), kind, &port), "allocate right");
    return port;
}
static int child(char *service) {
    mach_port_t endpoint, set = allocate(MACH_PORT_RIGHT_PORT_SET);
    success(bootstrap_check_in(bootstrap_port, service, &endpoint), "check in new dynamic child service");
    success(mach_port_set_context(mach_task_self(), endpoint, 0x123456789abcdef0ull), "port context");
    success(mach_port_insert_member(mach_task_self(), endpoint, set), "port set membership");
    struct request_buffer buffer = {0};
    /* A small receive must report the SDK message size without consuming it. */
    kern_return_t status = mach_msg(&buffer.message.header,
        MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_LARGE | MACH_RCV_LARGE_IDENTITY,
        0, sizeof(mach_msg_header_t), set, 5000, MACH_PORT_NULL);
    check(status == MACH_RCV_TOO_LARGE && buffer.message.header.msgh_size == sizeof buffer.message &&
          buffer.message.header.msgh_local_port == endpoint, "large message preserves queue and member identity");
    success(mach_msg(&buffer.message.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT |
        MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) | MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_CTX),
        0, sizeof buffer, set, 5000, MACH_PORT_NULL), "receive mixed complex message");
    struct request_message *m = &buffer.message;
    check(m->header.msgh_id == 400 && m->body.msgh_descriptor_count == 3 && m->value == 41,
          "inline header and payload");
    check(m->memory.type == MACH_MSG_PORT_DESCRIPTOR && m->memory.disposition == MACH_MSG_TYPE_PORT_SEND,
          "memory-entry right descriptor");
    check(m->bytes.size == 4096 && !memcmp(m->bytes.address, "OOL payload", 12), "OOL memory bytes");
    check(m->ports.count == 2 && m->ports.disposition == MACH_MSG_TYPE_PORT_SEND, "OOL port array");
    mach_port_t *ports = m->ports.address;
    check(ports[0] == ports[1], "repeated send rights coalesce in receiver namespace");
    mach_port_urefs_t refs;
    success(mach_port_get_refs(mach_task_self(), ports[0], MACH_PORT_RIGHT_SEND, &refs), "imported send references");
    check(refs == 2, "OOL array imports both send references");
    check(buffer.trailer.msgh_trailer_size == sizeof buffer.trailer &&
          buffer.trailer.msgh_context == 0x123456789abcdef0ull &&
          buffer.trailer.msgh_audit.val[5] == (unsigned)getppid(), "SDK trailer layout, context and sender PID");
    mach_vm_address_t shared = 0;
    success(mach_vm_map(mach_task_self(), &shared, 4096, 0, VM_FLAGS_ANYWHERE, m->memory.name, 0, FALSE,
        VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_SHARE), "map transferred memory entry");
    check(*(unsigned *)shared == 41, "shared memory starts with parent bytes");
    *(unsigned *)shared = 43;
    struct reply_message reply = {0};
    reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    reply.header.msgh_size = sizeof reply;
    reply.header.msgh_remote_port = m->header.msgh_remote_port;
    reply.header.msgh_id = 401; reply.value = 43;
    success(mach_msg_send(&reply.header), "send-once reply");
    check(mach_msg_send(&reply.header) == MACH_SEND_INVALID_DEST, "reply right consumed exactly once");
    success(mach_vm_deallocate(mach_task_self(), shared, 4096), "unmap shared entry");
    /* Destroy releases the imported port references and both OOL mappings. */
    const mach_port_t imported = ports[0];
    mach_msg_destroy(&m->header);
    check(mach_port_type(mach_task_self(), imported, &refs) == KERN_INVALID_NAME,
          "destroy message released imported port array rights");
    success(mach_port_destroy(mach_task_self(), endpoint), "destroy service endpoint");
    success(mach_port_destroy(mach_task_self(), set), "destroy port set");
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--child")) return child(argv[2]);
    check(argc == 2, "child executable argument");
    char service[100]; snprintf(service, sizeof service, "org.machoman.exchange.%d", getpid());
    char *args[] = {argv[1], "--child", service, NULL};
    pid_t pid;
    success(posix_spawn(&pid, argv[1], NULL, NULL, args, environ), "spawn separate guest process");
    mach_port_t destination = MACH_PORT_NULL;
    for (unsigned i = 0; i < 5000; ++i) {
        kern_return_t status = bootstrap_look_up(bootstrap_port, service, &destination);
        if (!status) break;
        check(status == BOOTSTRAP_UNKNOWN_SERVICE, "lookup before service registration");
        usleep(1000);
    }
    check(destination != MACH_PORT_NULL, "service discovery deadline");
    mach_port_t death = allocate(MACH_PORT_RIGHT_RECEIVE), previous;
    success(mach_port_request_notification(mach_task_self(), destination, MACH_NOTIFY_DEAD_NAME, 0,
        death, MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous), "register service death notification");
    check(previous == MACH_PORT_NULL, "first death request has no previous right");
    mach_vm_address_t shared = 0, ool = 0;
    success(mach_vm_allocate(mach_task_self(), &shared, 4096, VM_FLAGS_ANYWHERE), "parent shared allocation");
    *(unsigned *)shared = 41;
    memory_object_size_t length = 4096;
    mach_port_t memory;
    success(mach_make_memory_entry_64(mach_task_self(), &length, shared, VM_PROT_READ | VM_PROT_WRITE,
        &memory, MACH_PORT_NULL), "create entry from parent memory");
    success(mach_vm_allocate(mach_task_self(), &ool, 4096, VM_FLAGS_ANYWHERE), "OOL source allocation");
    memcpy((void *)ool, "OOL payload", 12);
    mach_port_t array_source = allocate(MACH_PORT_RIGHT_RECEIVE), reply_port = allocate(MACH_PORT_RIGHT_RECEIVE);
    mach_port_t ports[] = {array_source, array_source};
    struct request_message m = {0};
    m.header.msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    m.header.msgh_size = sizeof m; m.header.msgh_remote_port = destination;
    m.header.msgh_local_port = reply_port; m.header.msgh_id = 400;
    m.body.msgh_descriptor_count = 3;
    m.memory.name = memory; m.memory.disposition = MACH_MSG_TYPE_MOVE_SEND; m.memory.type = MACH_MSG_PORT_DESCRIPTOR;
    m.bytes.address = (void *)ool; m.bytes.size = 4096; m.bytes.deallocate = TRUE;
    m.bytes.copy = MACH_MSG_VIRTUAL_COPY; m.bytes.type = MACH_MSG_OOL_DESCRIPTOR;
    m.ports.address = ports; m.ports.count = 2; m.ports.copy = MACH_MSG_PHYSICAL_COPY;
    m.ports.disposition = MACH_MSG_TYPE_MAKE_SEND; m.ports.type = MACH_MSG_OOL_PORTS_DESCRIPTOR;
    m.value = 41;
    success(mach_msg_send(&m.header), "send complex message across processes");
    mach_port_t no_senders = allocate(MACH_PORT_RIGHT_RECEIVE);
    success(mach_port_request_notification(mach_task_self(), array_source, MACH_NOTIFY_NO_SENDERS, 2,
        no_senders, MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous), "register no-senders including queued rights");
    mach_port_type_t type;
    check(mach_port_type(mach_task_self(), memory, &type) == KERN_INVALID_NAME, "MOVE_SEND consumed parent memory name");
    struct reply_buffer reply = {0};
    success(mach_msg(&reply.message.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof reply, reply_port, 5000, 0),
        "receive child reply");
    check(reply.message.header.msgh_id == 401 && reply.message.value == 43 && *(unsigned *)shared == 43,
          "reply payload and child shared-memory writes");
    int status;
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child completed exchange");
    mach_dead_name_notification_t dead = {0};
    success(mach_msg(&dead.not_header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof dead, death, 5000, 0),
        "receive service death notification");
    check(dead.not_header.msgh_id == MACH_NOTIFY_DEAD_NAME && dead.not_port == destination && dead.NDR.int_rep == 1,
          "SDK dead-name notification and task-local port name");
    mach_no_senders_notification_t empty = {0};
    success(mach_msg(&empty.not_header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof empty, no_senders, 5000, 0),
        "receive no-senders notification after child released OOL rights");
    check(empty.not_header.msgh_id == MACH_NOTIFY_NO_SENDERS && empty.not_count == 2,
          "SDK no-senders notification and make-send count");
    success(mach_vm_deallocate(mach_task_self(), shared, 4096), "unmap parent shared memory");
    success(mach_port_destroy(mach_task_self(), reply_port), "destroy parent reply port");
    success(mach_port_destroy(mach_task_self(), array_source), "destroy source receive port");
    success(mach_port_deallocate(mach_task_self(), destination), "release service dead name");
    success(mach_port_deallocate(mach_task_self(), destination), "release notification's extra dead-name reference");
    success(mach_port_destroy(mach_task_self(), death), "destroy death notification port");
    success(mach_port_destroy(mach_task_self(), no_senders), "destroy no-senders port");
    puts("SDK Mach cross-process descriptors, OOL memory, shared entries, port sets, trailers and replies passed");
    return 0;
}

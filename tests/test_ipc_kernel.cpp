#include "machoman/ipc.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>

using namespace mm_ipc;
static void check(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "IPC kernel: %s\n", message); std::abort(); }
}
static response control(kernel &k, task_id t, operation op, std::uint64_t n = 0,
                        std::uint64_t a = 0, std::uint64_t b = 0) {
    request r; r.op = op; r.args = {1, n, a, b}; return k.transact(t, r);
}
static name allocate(kernel &k, task_id t, unsigned kind = 1) {
    auto r = control(k, t, operation::allocate, kind);
    check(r.status == 0 && r.args[0], "allocate"); return static_cast<name>(r.args[0]);
}
static response bootstrap(kernel &k, task_id t, operation op, const char *service, name port = 0) {
    request r; r.op = op; r.args = {3, port}; r.data.assign(service, service + std::strlen(service));
    return k.transact(t, r);
}
template <typename T> static void put(std::vector<std::uint8_t> &bytes, std::size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof value);
}
template <typename T> static T get(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    T value; std::memcpy(&value, bytes.data() + offset, sizeof value); return value;
}
static request packet(name destination, name reply = 0) {
    request r; r.op = operation::send; r.data.resize(28);
    put(r.data, 0, 19u | (reply ? 21u << 8 : 0u)); put(r.data, 4, 28u);
    put(r.data, 8, destination); put(r.data, 12, reply); put(r.data, 20, 42u); put(r.data, 24, 123u);
    return r;
}
static response receive(kernel &k, task_id t, name port, unsigned capacity = 1024, unsigned flags = 0x100) {
    request r; r.op = operation::receive; r.args = {port, flags, 1, capacity}; return k.transact(t, r);
}
int main() {
    kernel k; k.add_task(10, 111, 1000); k.add_task(20, 222, 1000);
    const auto server = allocate(k, 10), reply = allocate(k, 20);
    auto sent = control(k, 10, operation::extract, server, 20);
    check(sent.status == 0 && sent.args[0] == server && sent.args[1] == 17, "send/receive coalescing");
    check(bootstrap(k, 10, operation::bootstrap_register, "org.example.ipc", server).status == 0, "register service");
    auto looked_up = bootstrap(k, 20, operation::bootstrap_lookup, "org.example.ipc");
    check(looked_up.status == 0, "other task lookup");
    const auto send = static_cast<name>(looked_up.args[0]);
    check(send != server, "destination names are task-local");
    auto request_message = packet(send, reply);
    check(k.transact(20, request_message).status == 0, "cross-task send with send-once reply");
    auto received = receive(k, 10, server);
    check(received.status == 0 && get<unsigned>(received.data, 24) == 123 &&
          get<name>(received.data, 12) == server && received.args[4] == 222, "request payload, receive name and sender identity");
    const auto once = get<name>(received.data, 8);
    check(once && once != server, "reply right imported");
    auto response_message = packet(once); put(response_message.data, 0, 18u);
    check(k.transact(10, response_message).status == 0, "consume send-once reply");
    check(k.transact(10, response_message).status == 0x10000003, "send-once cannot be reused");
    check(receive(k, 20, reply).status == 0, "client receives reply");

    const auto movable = allocate(k, 20);
    auto complex = packet(send); complex.data.resize(56);
    put(complex.data, 0, 0x80000000u | 19u); put(complex.data, 4, 56u);
    put(complex.data, 24, 2u);
    put(complex.data, 28, movable); complex.data[38] = 16;
    put(complex.data, 40, movable); complex.data[50] = 16;
    check(k.transact(20, complex).status == 0x1000000a, "invalid repeated move rejected atomically");
    check(control(k, 20, operation::get_refs, movable, 1).args[0] == 1, "failed send retained receive right");
    complex.data.resize(40); put(complex.data, 4, 40u); put(complex.data, 24, 1u);
    check(k.transact(20, complex).status == 0, "receive right enters message escrow");
    check(control(k, 20, operation::type, movable).status == 15, "move removed sender name");
    received = receive(k, 10, server);
    check(received.status == 0, "receive complex message");
    const auto moved = get<name>(received.data, 28);
    check(control(k, 10, operation::get_refs, moved, 1).args[0] == 1, "receiver owns transferred receive right");

    const auto set = allocate(k, 10, 3);
    check(control(k, 10, operation::insert_member, server, set).status == 0, "insert port set member");
    check(receive(k, 10, server).status == 0x1000400a, "direct receive on member refused");
    check(k.transact(20, packet(send)).status == 0, "send to member");
    received = receive(k, 10, set, 24, 0x104);
    check(received.status == 0x10004004 && received.args[0] == 28, "large receive reports size and preserves message");
    received = receive(k, 10, set);
    check(received.status == 0 && get<name>(received.data, 12) == server, "port set receives actual member name");
    check(receive(k, 10, set).status == 0x10004003, "receive timeout");

    check(bootstrap(k, 20, operation::bootstrap_create_service, "org.example.inactive").status == 0, "create inactive service");
    check(bootstrap(k, 10, operation::bootstrap_check_in, "org.example.inactive").status == 1100, "check-in ownership");
    check(bootstrap(k, 20, operation::bootstrap_check_in, "org.example.inactive").status == 0, "check in service");
    check(bootstrap(k, 20, operation::bootstrap_check_in, "org.example.inactive").status == 1103, "duplicate check-in");
    check(bootstrap(k, 10, operation::bootstrap_lookup, "org.example.dynamic").status == 1102,
          "unregistered dynamic service cannot be looked up");
    auto dynamic = bootstrap(k, 20, operation::bootstrap_check_in, "org.example.dynamic");
    check(dynamic.status == 0, "check-in creates unmanaged dynamic service");
    check(bootstrap(k, 10, operation::bootstrap_lookup, "org.example.dynamic").status == 0, "dynamic service is discoverable");
    check(bootstrap(k, 20, operation::bootstrap_check_in, "org.example.dynamic").status == 1103, "active dynamic service cannot check in twice");
    check(control(k, 20, operation::destroy, dynamic.args[0]).status == 0, "dynamic receive right destroyed");
    check(bootstrap(k, 10, operation::bootstrap_lookup, "org.example.dynamic").status == 1102, "destroyed dynamic service disappears");
    check(bootstrap(k, 20, operation::bootstrap_check_in, "org.example.dynamic").status == 0, "dynamic service can check in again");

    // A send right in message escrow keeps the sender alive even after
    // its original task has moved away the last user reference.
    const auto tracked = allocate(k, 20), notifications = allocate(k, 20);
    check(control(k, 20, operation::extract, tracked, 20).status == 0, "tracked send right");
    request subscription; subscription.op = operation::request_notification;
    subscription.args = {1, tracked, 70, 1, notifications, 21};
    check(k.transact(20, subscription).status == 0, "no-senders subscription");
    check(control(k, 10, operation::extract_member, server, set).status == 0, "remove service from set");
    auto escrow = packet(send); escrow.data.resize(40);
    put(escrow.data, 0, 0x80000000u | 19u); put(escrow.data, 4, 40u); put(escrow.data, 24, 1u);
    put(escrow.data, 28, tracked); escrow.data[38] = 17;
    check(k.transact(20, escrow).status == 0, "move last send right into queue");
    check(receive(k, 20, notifications).status == 0x10004003, "queued right prevents premature no-senders");
    received = receive(k, 10, server);
    check(received.status == 0, "receive escrow send right");
    check(control(k, 10, operation::deallocate, get<name>(received.data, 28)).status == 0, "release imported escrow right");
    received = receive(k, 20, notifications);
    check(received.status == 0 && get<unsigned>(received.data, 20) == 70 && get<unsigned>(received.data, 32) == 1,
          "last escrow sender produces one notification");

    // Losing a task-local send name cancels its death subscription through
    // a PORT_DELETED notification carrying that task's current name.
    auto remote_send = bootstrap(k, 20, operation::bootstrap_lookup, "org.example.ipc");
    const auto deleted_name = static_cast<name>(remote_send.args[0]);
    subscription.args = {1, deleted_name, 72, 0, notifications, 21};
    check(k.transact(20, subscription).status == 0, "death request on send name");
    check(control(k, 20, operation::mod_refs, deleted_name, 0, static_cast<std::uint64_t>(-2ll)).status == 0,
          "delete both coalesced send references");
    received = receive(k, 20, notifications);
    check(received.status == 0 && get<unsigned>(received.data, 20) == 65 && get<name>(received.data, 32) == deleted_name,
          "port-deleted reports task-local deleted name");

    // Discarding an undersized message releases receive-right escrow and
    // sends SEND_ONCE for an unconsumed reply right.
    const auto discard = allocate(k, 20), discarded_reply = allocate(k, 20);
    check(control(k, 20, operation::extract, discard, 20).status == 0, "discard destination send right");
    check(k.transact(20, packet(discard, discarded_reply)).status == 0, "queued one-shot reply");
    check(receive(k, 20, discard, 24).status == 0x10004004, "small receive discards without LARGE flag");
    received = receive(k, 20, discarded_reply);
    check(received.status == 0 && get<unsigned>(received.data, 20) == 71, "discard emits send-once notification");

    const auto waiting = allocate(k, 20);
    std::promise<void> started;
    auto waiter = std::async(std::launch::async, [&] {
        started.set_value(); return receive(k, 20, waiting, 1024, 0).status;
    });
    started.get_future().wait();
    k.remove_task(20);
    const auto death_status = waiter.get();
    check(death_status == 0x10004009 || death_status == 16, "task death wakes blocking receive");
    k.remove_task(10);
    check(k.transact(10, {}).status == 16, "dead task rejected");
    std::puts("IPC task namespaces, atomic rights transfer, send-once replies, bootstrap, port sets and task death passed");
}

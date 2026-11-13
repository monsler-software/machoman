#include "machoman/ipc.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <spawn.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;
using namespace mm_ipc;
static void check(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "IPC transport: %s (errno %d)\n", message, errno); std::abort(); }
}
static response control(operation op, std::uint64_t n, std::uint64_t value = 0) {
    request r; r.op = op; r.args = {1, n, value}; return exchange(r);
}
static response service(operation op, const char *text, name n = 0) {
    request r; r.op = op; r.args = {3, n}; r.data.assign(text, text + std::strlen(text)); return exchange(r);
}
static name allocate() {
    const auto r = control(operation::allocate, 1);
    check(r.status == 0 && r.args[0], "broker port allocation"); return static_cast<name>(r.args[0]);
}
template <typename T> static void put(std::vector<std::uint8_t> &b, std::size_t offset, T value) {
    std::memcpy(b.data() + offset, &value, sizeof value);
}
template <typename T> static T get(const std::vector<std::uint8_t> &b, std::size_t offset) {
    T value; std::memcpy(&value, b.data() + offset, sizeof value); return value;
}
static response send_message(name destination, unsigned value, name reply = 0, unsigned disposition = 19) {
    request r; r.op = operation::send; r.data.resize(28);
    put(r.data, 0, disposition | (reply ? 21u << 8 : 0u)); put(r.data, 4, 28u);
    put(r.data, 8, destination); put(r.data, 12, reply); put(r.data, 20, 42u); put(r.data, 24, value);
    return exchange(r);
}
static response receive_message(name port) {
    request r; r.op = operation::receive; r.args = {port, 0x100, 5000, 1024}; return exchange(r);
}
int main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--child") == 0) {
        const auto destination = service(operation::bootstrap_lookup, "org.example.transport");
        check(destination.status == 0, "child discovers parent's port");
        const auto reply = allocate();
        check(control(operation::extract, reply, 20).status == 0, "child send right");
        check(service(operation::bootstrap_register, "org.example.child", reply).status == 0, "register child service");
        check(send_message(static_cast<name>(destination.args[0]), 123, reply).status == 0, "child sends request");
        const auto response = receive_message(reply);
        check(response.status == 0 && get<unsigned>(response.data, 40) == 456, "child receives send-once reply");
        const auto memory_name = get<name>(response.data, 28);
        request memory; memory.op = operation::memory_get; memory.args[0] = memory_name;
        const auto entry = exchange(memory);
        check(entry.status == 0 && entry.descriptors.size() == 1 && entry.args[0] == 4096, "child resolves transferred memory right");
        auto *shared = static_cast<unsigned *>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, entry.descriptors[0].get(), 0));
        check(shared != MAP_FAILED && shared[0] == 321, "SCM_RIGHTS memory contents");
        shared[0] = 789; munmap(shared, 4096);
        return 0;
    }
    check(argc == 2, "machoman executable argument");
    char directory[] = "/tmp/machoman-ipc-test-XXXXXX";
    check(mkdtemp(directory) != nullptr, "temporary broker namespace");
    unsetenv("MACHOMAN_VENV"); unsetenv("MACHOMAN_EXE");
    request explicit_request; explicit_request.op = operation::allocate; explicit_request.args = {1, 1};
    const auto explicit_port = exchange(explicit_request, directory, argv[1]);
    check(explicit_port.status == 0 && explicit_port.args[0], "immutable context starts broker without environment variables");
    explicit_request.op = operation::destroy; explicit_request.args[1] = explicit_port.args[0];
    check(exchange(explicit_request, directory, argv[1]).status == 0, "immutable context reuses broker connection");
    setenv("MACHOMAN_VENV", directory, 1); setenv("MACHOMAN_EXE", argv[1], 1);
    setenv("MACHOMAN_IPC_IDLE_TIMEOUT_MS", "100", 1);
    const auto port = allocate();
    check(control(operation::extract, port, 20).status == 0, "server send right");
    check(service(operation::bootstrap_register, "org.example.transport", port).status == 0, "register server service");
    request watch; watch.op = operation::watch; watch.args[0] = port;
    const auto watched = exchange(watch);
    check(watched.status == 0 && watched.descriptors.size() == 1, "broker exports readiness descriptor");
    pollfd readiness{watched.descriptors[0].get(), POLLIN, 0};
    check(poll(&readiness, 1, 0) == 0, "empty port not readable");
    file_descriptor memory_fd(static_cast<int>(syscall(SYS_memfd_create, "machoman-ipc-test", MFD_CLOEXEC)));
    check(memory_fd.get() >= 0 && ftruncate(memory_fd.get(), 4096) == 0, "memory backing");
    auto *shared = static_cast<unsigned *>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, memory_fd.get(), 0));
    check(shared != MAP_FAILED, "parent memory mapping"); shared[0] = 321;
    request memory; memory.op = operation::memory_create; memory.args = {1, 4096, 3}; memory.descriptors.push_back(memory_fd);
    const auto memory_entry = exchange(memory);
    check(memory_entry.status == 0, "broker imports memory backing descriptor");
    auto other_thread = std::async(std::launch::async, [port] { return control(operation::get_refs, port, 1); });
    check(other_thread.get().args[0] == 1, "different connections share one Linux task namespace");
    pid_t child = -1;
    char *child_args[] = {argv[0], const_cast<char *>("--child"), nullptr};
    check(posix_spawn(&child, argv[0], nullptr, nullptr, child_args, environ) == 0, "spawn independent client");
    check(poll(&readiness, 1, 5000) == 1 && (readiness.revents & POLLIN), "cross-process message wakes eventfd");
    const auto request = receive_message(port);
    check(request.status == 0 && get<unsigned>(request.data, 24) == 123 && request.args[4] == static_cast<unsigned>(child),
          "broker routes message and authenticates sender pid");
    const auto child_service = service(operation::bootstrap_lookup, "org.example.child");
    check(child_service.status == 0, "discover child's service");
    const auto once = get<name>(request.data, 8);
    check(poll(&readiness, 1, 0) == 0, "drained queue clears readiness");
    mm_ipc::request response; response.op = operation::send; response.data.resize(44);
    put(response.data, 0, 0x80000000u | 18u); put(response.data, 4, 44u); put(response.data, 8, once);
    put(response.data, 24, 1u); put(response.data, 28, static_cast<name>(memory_entry.args[0]));
    response.data[38] = 19; put(response.data, 40, 456u);
    check(exchange(response).status == 0, "parent sends one-shot reply carrying memory right");
    int status = 0; check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "client completed");
    check(shared[0] == 789, "memory writes are shared between Linux processes"); munmap(shared, 4096);
    bool dead = false;
    for (int i = 0; i < 40; ++i) {
        const auto refs = control(operation::get_refs, child_service.args[0], 4);
        if (refs.status == 0 && refs.args[0] == 1) { dead = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check(dead, "pidfd process death converts remote send right to dead name");
    check(service(operation::bootstrap_lookup, "org.example.child").status == 1102, "dead service removed");
    check(control(operation::destroy, port).status == 0, "destroy server port");
    std::filesystem::remove_all(directory);
    std::puts("Linux IPC broker, authenticated tasks, cross-process bootstrap, replies and process death passed");
}

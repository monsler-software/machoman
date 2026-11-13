#include "machoman/ipc.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>

extern char **environ;
namespace mm_ipc {
namespace {
constexpr std::uint32_t magic = 0x4d4d4950, version = 3;
constexpr std::size_t max_packet = 64 * 1024 * 1024;
struct wire_header {
    std::uint32_t magic_, version_, code, bytes;
    std::uint32_t descriptor_count, reserved = 0;
    std::array<std::uint64_t, 12> args;
};
static_assert(sizeof(wire_header) == 120);
struct descriptor {
    int fd = -1;
    explicit descriptor(int value = -1) : fd(value) {}
    ~descriptor() { if (fd >= 0) close(fd); }
    descriptor(const descriptor &) = delete;
    descriptor &operator=(const descriptor &) = delete;
};
bool read_all(int fd, void *memory, std::size_t size) {
    auto *p = static_cast<std::uint8_t *>(memory);
    while (size) {
        const auto n = recv(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= static_cast<std::size_t>(n);
    }
    return true;
}
bool write_all(int fd, const void *memory, std::size_t size) {
    const auto *p = static_cast<const std::uint8_t *>(memory);
    while (size) {
        const auto n = send(fd, p, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= static_cast<std::size_t>(n);
    }
    return true;
}
bool read_header(int fd, wire_header &header, std::vector<file_descriptor> &descriptors) {
    alignas(cmsghdr) std::uint8_t ancillary[CMSG_SPACE(253 * sizeof(int))]{};
    iovec iov{&header, sizeof header}; msghdr message{};
    message.msg_iov = &iov; message.msg_iovlen = 1;
    message.msg_control = ancillary; message.msg_controllen = sizeof ancillary;
    ssize_t count;
    do { count = recvmsg(fd, &message, MSG_WAITALL | MSG_CMSG_CLOEXEC); } while (count < 0 && errno == EINTR);
    if (count <= 0) return false;
    for (auto *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS || c->cmsg_len < CMSG_LEN(0)) return false;
        const auto size = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const auto *fds = reinterpret_cast<const int *>(CMSG_DATA(c));
        for (std::size_t i = 0; i < size; ++i) descriptors.emplace_back(fds[i]);
    }
    if (message.msg_flags & MSG_CTRUNC) return false;
    if (static_cast<std::size_t>(count) < sizeof header &&
        !read_all(fd, reinterpret_cast<std::uint8_t *>(&header) + count, sizeof header - static_cast<std::size_t>(count))) return false;
    return header.descriptor_count == descriptors.size();
}
bool write_header(int fd, const wire_header &header, const std::vector<file_descriptor> &descriptors) {
    if (descriptors.size() > 253) return false;
    iovec iov{const_cast<wire_header *>(&header), sizeof header}; msghdr message{};
    message.msg_iov = &iov; message.msg_iovlen = 1;
    std::vector<std::uint8_t> ancillary;
    if (!descriptors.empty()) {
        ancillary.resize(CMSG_SPACE(descriptors.size() * sizeof(int)));
        message.msg_control = ancillary.data(); message.msg_controllen = ancillary.size();
        auto *c = CMSG_FIRSTHDR(&message); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(descriptors.size() * sizeof(int));
        auto *fds = reinterpret_cast<int *>(CMSG_DATA(c));
        for (std::size_t i = 0; i < descriptors.size(); ++i) {
            fds[i] = descriptors[i].get(); if (fds[i] < 0) return false;
        }
    }
    ssize_t count;
    do { count = sendmsg(fd, &message, MSG_NOSIGNAL); } while (count < 0 && errno == EINTR);
    return count > 0 && (static_cast<std::size_t>(count) == sizeof header ||
        write_all(fd, reinterpret_cast<const std::uint8_t *>(&header) + count, sizeof header - static_cast<std::size_t>(count)));
}
std::string root_path(const char *root) {
    std::error_code error;
    auto path = std::filesystem::weakly_canonical(root, error);
    return (error ? std::filesystem::absolute(root) : path).string();
}
sockaddr_un address_for(const std::string &root) {
    std::uint64_t hash = 14695981039346656037ull;
    for (char ch : root) { hash ^= static_cast<unsigned char>(ch); hash *= 1099511628211ull; }
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    /* Abstract Unix addresses disappear with the broker; stale socket
     * files cannot redirect a new venv to a dead server. */
    std::snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1, "machoman-ipc-v%u-%u-%016llx",
                  version, static_cast<unsigned>(getuid()), static_cast<unsigned long long>(hash));
    return address;
}
socklen_t address_size(const sockaddr_un &address) {
    return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + std::strlen(address.sun_path + 1));
}
int connect_to(const sockaddr_un &address) {
    descriptor fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (fd.fd < 0 || connect(fd.fd, reinterpret_cast<const sockaddr *>(&address), address_size(address)) < 0) return -1;
    const auto result = fd.fd; fd.fd = -1; return result;
}
int connect_or_start(const std::string &root, const char *configured) {
    const auto address = address_for(root);
    if (const int fd = connect_to(address); fd >= 0) return fd;
    const auto directory = std::filesystem::path(root) / ".machoman/ipc";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return -1;
    descriptor start_lock(open((directory / "start.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
    if (start_lock.fd < 0 || flock(start_lock.fd, LOCK_EX) < 0) return -1;
    if (const int fd = connect_to(address); fd >= 0) return fd;
    std::string executable;
    if (configured && *configured) executable = configured;
    else {
        executable = std::filesystem::read_symlink("/proc/self/exe", error).string();
        if (error) return -1;
    }
    char *argv[] = {executable.data(), const_cast<char *>("ipc-broker"), const_cast<char *>(root.c_str()), nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (posix_spawn_file_actions_init(&actions) != 0) return -1;
    if (posix_spawnattr_init(&attributes) != 0) { posix_spawn_file_actions_destroy(&actions); return -1; }
    int setup = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    if (!setup) setup = posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    if (!setup) setup = posix_spawn_file_actions_addopen(&actions, 2, (directory / "broker.log").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (!setup) setup = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    sigset_t mask; sigemptyset(&mask);
    if (!setup) setup = posix_spawnattr_setsigmask(&attributes, &mask);
    if (!setup) setup = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK);
    pid_t pid = -1;
    const int spawned = setup ? setup : posix_spawn(&pid, executable.c_str(), &actions, &attributes, argv, environ);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes);
    if (spawned) { errno = spawned; return -1; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (const int fd = connect_to(address); fd >= 0) return fd;
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    errno = ETIMEDOUT; return -1;
}
struct client_connection {
    int fd = -1;
    pid_t pid = 0;
    std::string root;
    ~client_connection() { if (fd >= 0) close(fd); }
};
thread_local client_connection connection;
struct broker_task { task_id id = 0; int pidfd = -1; };
struct broker_state {
    kernel model;
    std::mutex mutex;
    std::unordered_map<pid_t, broker_task> tasks;
    std::unordered_map<int, bool> connections;
    task_id next = 1;
};
void serve_connection(const std::shared_ptr<broker_state> &state, int socket_fd, task_id task) {
    descriptor fd(socket_fd);
    wire_header wire{};
    for (;;) {
        request message;
        if (!read_header(fd.fd, wire, message.descriptors)) break;
        if (wire.magic_ != magic || wire.version_ != version || wire.bytes > max_packet ||
            wire.code >= static_cast<std::uint32_t>(operation::_count)) break;
        message.op = static_cast<operation>(wire.code); message.args = wire.args;
        message.data.resize(wire.bytes);
        if (!read_all(fd.fd, message.data.data(), message.data.size())) break;
        response result;
        try { result = state->model.transact(task, message); }
        catch (const std::bad_alloc &) { result.status = 6; } /* KERN_RESOURCE_SHORTAGE */
        const wire_header reply{magic, version, result.status, static_cast<std::uint32_t>(result.data.size()),
            static_cast<std::uint32_t>(result.descriptors.size()), 0, result.args};
        if (!write_header(fd.fd, reply, result.descriptors) || !write_all(fd.fd, result.data.data(), result.data.size())) break;
    }
    std::lock_guard lock(state->mutex);
    state->connections.erase(fd.fd);
}
}

file_descriptor::file_descriptor(int fd) : fd_(std::shared_ptr<int>(new int(fd), [](int *p) { if (*p >= 0) close(*p); delete p; })) {}

response exchange(const request &message) {
    return exchange(message, std::getenv("MACHOMAN_VENV"), std::getenv("MACHOMAN_EXE"));
}

response exchange(const request &message, const char *root, const char *launcher) {
    response result;
    const auto failure = [&] { result.status = 5; return result; };
    if (!root || !*root || message.data.size() > max_packet) return failure();
    const auto canonical = root_path(root);
    if (connection.pid != getpid() || connection.root != canonical) {
        if (connection.fd >= 0) close(connection.fd);
        connection.fd = -1; connection.pid = getpid(); connection.root = canonical;
    }
    if (connection.fd < 0) connection.fd = connect_or_start(canonical, launcher);
    if (connection.fd < 0) return failure();
    const wire_header header{magic, version, static_cast<std::uint32_t>(message.op),
                            static_cast<std::uint32_t>(message.data.size()),
                            static_cast<std::uint32_t>(message.descriptors.size()), 0, message.args};
    wire_header reply{};
    if (!write_header(connection.fd, header, message.descriptors) || !write_all(connection.fd, message.data.data(), message.data.size()) ||
        !read_header(connection.fd, reply, result.descriptors) || reply.magic_ != magic || reply.version_ != version || reply.bytes > max_packet) {
        close(connection.fd); connection.fd = -1; return failure();
    }
    result.status = reply.code; result.args = reply.args; result.data.resize(reply.bytes);
    if (!read_all(connection.fd, result.data.data(), result.data.size())) {
        close(connection.fd); connection.fd = -1; return failure();
    }
    return result;
}

int serve(const char *venv_root) {
    const auto root = root_path(venv_root);
    const auto address = address_for(root);
    descriptor listener(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (listener.fd < 0 || bind(listener.fd, reinterpret_cast<const sockaddr *>(&address), address_size(address)) < 0 ||
        listen(listener.fd, 32) < 0) return 1;
    auto state = std::make_shared<broker_state>();
    auto last_activity = std::chrono::steady_clock::now();
    std::chrono::milliseconds idle_timeout(120000);
    if (const char *configured = std::getenv("MACHOMAN_IPC_IDLE_TIMEOUT_MS"))
        idle_timeout = std::chrono::milliseconds(std::max(100L, std::strtol(configured, nullptr, 10)));
    for (;;) {
        std::vector<pollfd> watched{{listener.fd, POLLIN, 0}};
        std::vector<pid_t> pids;
        {
            std::lock_guard lock(state->mutex);
            for (const auto &[pid, task] : state->tasks) { pids.push_back(pid); watched.push_back({task.pidfd, POLLIN, 0}); }
        }
        const int ready = poll(watched.data(), watched.size(), 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return 1;
        for (std::size_t i = 1; i < watched.size(); ++i) if (watched[i].revents) {
            task_id id = 0;
            {
                std::lock_guard lock(state->mutex);
                auto it = state->tasks.find(pids[i - 1]);
                if (it == state->tasks.end()) continue;
                id = it->second.id; close(it->second.pidfd); state->tasks.erase(it);
            }
            state->model.remove_task(id); last_activity = std::chrono::steady_clock::now();
        }
        if (watched[0].revents & POLLIN) {
            descriptor client(accept4(listener.fd, nullptr, nullptr, SOCK_CLOEXEC));
            if (client.fd < 0) continue;
            ucred credentials{}; socklen_t size = sizeof credentials;
            if (getsockopt(client.fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) < 0 || credentials.uid != getuid()) continue;
            task_id id;
            {
                std::lock_guard lock(state->mutex);
                auto it = state->tasks.find(credentials.pid);
                if (it == state->tasks.end()) {
                    const int pidfd = static_cast<int>(syscall(SYS_pidfd_open, credentials.pid, 0));
                    if (pidfd < 0) continue;
                    id = state->next++;
                    state->tasks.emplace(credentials.pid, broker_task{id, pidfd});
                    state->model.add_task(id, static_cast<std::uint32_t>(credentials.pid), credentials.uid);
                } else id = it->second.id;
                state->connections.emplace(client.fd, true);
            }
            const auto fd = client.fd; client.fd = -1;
            std::thread(serve_connection, state, fd, id).detach();
            last_activity = std::chrono::steady_clock::now();
        }
        {
            std::lock_guard lock(state->mutex);
            if (!state->tasks.empty()) last_activity = std::chrono::steady_clock::now();
            else if (std::chrono::steady_clock::now() - last_activity >= idle_timeout) break;
        }
    }
    return 0;
}
}

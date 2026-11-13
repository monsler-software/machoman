#define _GNU_SOURCE 1
#include "internal.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <deque>
#include <fcntl.h>
#include <new>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <vector>
#include <unistd.h>

extern char **environ;
extern "C" int darwin_sigprocmask(int, const std::uint32_t *, std::uint32_t *) __asm__("_sigprocmask");
namespace {
struct fork_handler { const void *prepare, *parent, *child; };
std::mutex fork_handlers_mutex;
std::vector<fork_handler> fork_handlers;
thread_local std::deque<std::vector<fork_handler>> fork_snapshots;
void prepare_fork() {
    auto &snapshot = fork_snapshots.emplace_back();
    {
        std::lock_guard lock(fork_handlers_mutex);
        snapshot = fork_handlers;
    }
    /* Prepare callbacks run in reverse registration order, with no host
     * registry lock held across guest code or the actual fork. */
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
        if (it->prepare) mm_call_callback(it->prepare, nullptr, 0);
}
void finish_fork(bool child) {
    if (fork_snapshots.empty()) return;
    const auto &snapshot = fork_snapshots.back();
    for (const auto &handler : snapshot) {
        const auto function = child ? handler.child : handler.parent;
        if (function) mm_call_callback(function, nullptr, 0);
    }
    fork_snapshots.pop_back();
}
void parent_fork() { finish_fork(false); }
void child_fork() { finish_fork(true); }

struct attributes { posix_spawnattr_t native; short flags = 0; std::uint32_t mask = 0; };
struct file_action { int kind, fd, target = 0, flags = 0; unsigned mode = 0; std::string path; };
struct actions { posix_spawn_file_actions_t native; std::vector<file_action> records; };
int apply(posix_spawn_file_actions_t &a, const file_action &record) {
    switch (record.kind) {
    case 0: return posix_spawn_file_actions_adddup2(&a, record.fd, record.target);
    case 1: return posix_spawn_file_actions_addclose(&a, record.fd);
    case 2: return posix_spawn_file_actions_addopen(&a, record.fd, record.path.c_str(), record.flags, record.mode);
    case 3: return posix_spawn_file_actions_addchdir_np(&a, record.path.c_str());
    }
    return EINVAL;
}
int append(actions *a, file_action record) {
    if (!a) return 22;
    const int error = apply(a->native, record);
    if (!error) a->records.push_back(std::move(record));
    return mm_darwin_errno(error);
}
int close_by_default(posix_spawn_file_actions_t &a, const actions *source) {
    std::vector<int> keep;
    if (source) for (const auto &record : source->records) {
        const int error = apply(a, record); if (error) return error;
        if (record.kind == 0) keep.push_back(record.target);
        if (record.kind == 2) keep.push_back(record.fd);
        if (record.kind == 1) std::erase(keep, record.fd);
    }
    std::sort(keep.begin(), keep.end());
    keep.erase(std::unique(keep.begin(), keep.end()), keep.end());
    if (keep.empty()) return posix_spawn_file_actions_addclosefrom_np(&a, 0);
    /* Save retained descriptors above their final destinations, close every
     * other descriptor (including descriptors opened by another parent
     * thread during spawn), then restore the retained numbers. */
    const int temporary = keep.back() + 1;
    for (std::size_t i = 0; i < keep.size(); ++i) {
        const int error = posix_spawn_file_actions_adddup2(&a, keep[i], temporary + static_cast<int>(i));
        if (error) return error;
    }
    for (int fd = 0; fd < temporary; ++fd) {
        const int error = posix_spawn_file_actions_addclose(&a, fd);
        if (error) return error;
    }
    int error = posix_spawn_file_actions_addclosefrom_np(&a, temporary + static_cast<int>(keep.size()));
    if (error) return error;
    for (std::size_t i = 0; i < keep.size(); ++i) {
        error = posix_spawn_file_actions_adddup2(&a, temporary + static_cast<int>(i), keep[i]);
        if (error) return error;
        error = posix_spawn_file_actions_addclose(&a, temporary + static_cast<int>(i));
        if (error) return error;
    }
    return 0;
}
bool macho(const char *path) {
    auto *file = path ? std::fopen(path, "rb") : nullptr;
    if (!file) return false;
    std::uint32_t magic = 0;
    const bool read = std::fread(&magic, 1, 4, file) == 4;
    std::fclose(file);
    return read && (magic == 0xFEEDFACFu || magic == 0xCFFAEDFEu || magic == 0xBEBAFECAu || magic == 0xBFBAFECAu);
}
template <typename T> T *object(const void *slot) { return slot ? *static_cast<T *const *>(slot) : nullptr; }
std::string find_program(const char *path) {
    if (!path) return {};
    if (std::string_view(path).find('/') != std::string_view::npos) return path;
    const char *search = mm_host_getenv(mm_libsystem_api(), "PATH");
    std::string_view dirs = search ? search : "/usr/bin:/bin";
    for (;;) {
        const auto end = dirs.find(':');
        auto dir = dirs.substr(0, end);
        const auto candidate = std::filesystem::path(dir.empty() ? "." : std::string(dir)) / path;
        if (access(candidate.c_str(), X_OK) == 0) return candidate.string();
        if (end == std::string_view::npos) break;
        dirs.remove_prefix(end + 1);
    }
    return path;
}
int spawn(int *pid, const char *path, const void *file_actions, const void *attrs,
          char *const *argv, char *const *envp, bool search) {
    if (!path || !argv || !argv[0]) return 22;
    const auto program = search ? find_program(path) : std::string(path);
    const auto *api=mm_libsystem_api();
    const char *launcher = mm_host_launcher(api), *venv = mm_host_venv(api);
    const bool nested = macho(program.c_str());
    if (nested && (!launcher || !venv)) return 2;
    if (nested && std::getenv("MACHOMAN_DISABLE_GUEST_SUBPROCESSES")) return 12;
    std::vector<std::string> args;
    if (nested) {
        std::uint32_t mask = 0;
        darwin_sigprocmask(3, nullptr, &mask);
        const auto *attr=object<attributes>(attrs);
        if (attr && (attr->flags & 8)) mask=attr->mask;
        args=mm_guest_launch_arguments(api, std::filesystem::absolute(program).string(), argv, mask);
    } else for (auto p = argv; *p; ++p) args.emplace_back(*p);
    std::vector<char *> raw;
    for (auto &a : args) raw.push_back(a.data());
    raw.push_back(nullptr);
    std::vector<std::string> environment;
    for (auto p = envp; p && *p; ++p) environment.emplace_back(*p);
    auto *attr = object<attributes>(attrs);
    std::vector<char *> env;
    for (auto &e : environment) env.push_back(e.data());
    env.push_back(nullptr);
    auto *act = object<actions>(file_actions);
    posix_spawn_file_actions_t filtered;
    const bool filter = attr && (attr->flags & 0x4000);
    if (filter) {
        int error = posix_spawn_file_actions_init(&filtered);
        if (error) return mm_darwin_errno(error);
        error = close_by_default(filtered, act);
        if (error) { posix_spawn_file_actions_destroy(&filtered); return mm_darwin_errno(error); }
    }
    pid_t result = 0;
    /* libc's spawn avoids executing C++ code after fork in a multithreaded
     * guest. File actions, exec errors and descriptor inheritance are real. */
    const int error = posix_spawn(&result, nested ? launcher : program.c_str(), filter ? &filtered : act ? &act->native : nullptr,
                                  attr ? &attr->native : nullptr, raw.data(), env.data());
    if (filter) posix_spawn_file_actions_destroy(&filtered);
    if (!error && pid) *pid = result;
    return mm_darwin_errno(error);
}
int exec(const char *path, char *const *argv, char *const *envp, bool search) {
    if (!path || !argv || !argv[0]) { errno = EINVAL; return -1; }
    const auto program = search ? find_program(path) : std::string(path);
    if (!macho(program.c_str())) return execve(program.c_str(), argv, envp);
    const auto *api=mm_libsystem_api();
    const char *launcher = mm_host_launcher(api), *venv = mm_host_venv(api);
    if (!launcher || !venv) { errno = ENOENT; return -1; }
    std::uint32_t mask = 0;
    darwin_sigprocmask(3, nullptr, &mask);
    auto args=mm_guest_launch_arguments(api, std::filesystem::absolute(program).string(), argv, mask);
    std::vector<char *> raw;
    for (auto &a : args) raw.push_back(a.data()); raw.push_back(nullptr);
    std::vector<std::string> environment;
    for (auto p = envp; p && *p; ++p) environment.emplace_back(*p);
    std::vector<char *> env;
    for (auto &e : environment) env.push_back(e.data()); env.push_back(nullptr);
    return execve(launcher, raw.data(), env.data());
}
}
extern "C" {
int darwin_pthread_atfork(const void *, const void *, const void *) DARWIN(pthread_atfork);
int darwin_pthread_atfork(const void *prepare, const void *parent, const void *child) {
    /* Register host thunks, never Mach-O addresses, with libc. This also
     * covers forks made by a host API while serving a guest callback. */
    static const int registration = ::pthread_atfork(prepare_fork, parent_fork, child_fork);
    if (registration) return mm_darwin_errno(registration);
    try {
        std::lock_guard lock(fork_handlers_mutex);
        fork_handlers.push_back({prepare, parent, child});
    } catch (const std::bad_alloc &) { return 12; }
    return 0;
}
int darwin_execve(const char *, char *const *, char *const *) DARWIN(execve);
int darwin_execve(const char *path, char *const *argv, char *const *envp) { return exec(path, argv, envp, false); }
int darwin_execv(const char *, char *const *) DARWIN(execv);
int darwin_execv(const char *path, char *const *argv) {
    const auto *api=mm_libsystem_api();
    return exec(path, argv, api ? mm_host_environment(api) : environ, false);
}
int darwin_execvp(const char *, char *const *) DARWIN(execvp);
int darwin_execvp(const char *path, char *const *argv) {
    const auto *api=mm_libsystem_api();
    return exec(path, argv, api ? mm_host_environment(api) : environ, true);
}
int darwin_posix_spawn(int *, const char *, const void *, const void *, char *const *, char *const *) DARWIN(posix_spawn);
int darwin_posix_spawn(int *pid, const char *path, const void *actions, const void *attr, char *const *args, char *const *env) {
    return spawn(pid, path, actions, attr, args, env, false);
}
int darwin_posix_spawnp(int *, const char *, const void *, const void *, char *const *, char *const *) DARWIN(posix_spawnp);
int darwin_posix_spawnp(int *pid, const char *path, const void *actions, const void *attr, char *const *args, char *const *env) {
    return spawn(pid, path, actions, attr, args, env, true);
}
int darwin_spawnattr_init(attributes **) DARWIN(posix_spawnattr_init);
int darwin_spawnattr_init(attributes **slot) {
    if (!slot) return 22;
    auto *attr = new (std::nothrow) attributes;
    if (!attr) return 12;
    const int error = posix_spawnattr_init(&attr->native);
    if (error) { delete attr; return mm_darwin_errno(error); }
    *slot = attr; return 0;
}
int darwin_spawnattr_destroy(attributes **) DARWIN(posix_spawnattr_destroy);
int darwin_spawnattr_destroy(attributes **slot) {
    auto *a = object<attributes>(slot); if (!a) return 22;
    const int error = posix_spawnattr_destroy(&a->native); delete a; *slot = nullptr; return mm_darwin_errno(error);
}
int darwin_spawnattr_setflags(attributes **, short) DARWIN(posix_spawnattr_setflags);
int darwin_spawnattr_setflags(attributes **slot, short flags) {
    auto *a = object<attributes>(slot); if (!a) return 22;
    if (flags & ~0x440f) return 45;
    short native = 0;
    if (flags & 1) native |= POSIX_SPAWN_RESETIDS;
    if (flags & 2) native |= POSIX_SPAWN_SETPGROUP;
    if (flags & 4) native |= POSIX_SPAWN_SETSIGDEF;
    if (flags & 0x400) native |= POSIX_SPAWN_SETSID;
    const int error = posix_spawnattr_setflags(&a->native, native);
    if (!error) a->flags = flags;
    return mm_darwin_errno(error);
}
int darwin_spawnattr_getflags(attributes *const *, short *) DARWIN(posix_spawnattr_getflags);
int darwin_spawnattr_getflags(attributes *const *slot, short *flags) {
    auto *a = object<attributes>(slot); if (!a || !flags) return 22; *flags = a->flags; return 0;
}
int darwin_spawnattr_setpgroup(attributes **, int) DARWIN(posix_spawnattr_setpgroup);
int darwin_spawnattr_setpgroup(attributes **slot, int group) {
    auto *a = object<attributes>(slot); return a ? mm_darwin_errno(posix_spawnattr_setpgroup(&a->native, group)) : 22;
}
int darwin_spawnattr_setsigmask(attributes **, const std::uint32_t *) DARWIN(posix_spawnattr_setsigmask);
int darwin_spawnattr_setsigmask(attributes **slot, const std::uint32_t *mask) {
    auto *a = object<attributes>(slot); if (!a || !mask) return 22; a->mask = *mask; return 0;
}
int darwin_spawnattr_setsigdefault(attributes **, const std::uint32_t *) DARWIN(posix_spawnattr_setsigdefault);
int darwin_spawnattr_setsigdefault(attributes **slot, const std::uint32_t *mask) {
    auto *a = object<attributes>(slot); if (!a || !mask) return 22;
    constexpr int signals[] = {0, SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT, 0, SIGFPE, SIGKILL,
        SIGBUS, SIGSEGV, SIGSYS, SIGPIPE, SIGALRM, SIGTERM, SIGURG, SIGSTOP, SIGTSTP, SIGCONT, SIGCHLD,
        SIGTTIN, SIGTTOU, SIGIO, SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF, SIGWINCH, 0, SIGUSR1, SIGUSR2};
    sigset_t host; sigemptyset(&host);
    for (unsigned i = 1; i < 32; ++i) if ((*mask & (1u << (i - 1))) && signals[i]) sigaddset(&host, signals[i]);
    return mm_darwin_errno(posix_spawnattr_setsigdefault(&a->native, &host));
}
int darwin_actions_init(actions **) DARWIN(posix_spawn_file_actions_init);
int darwin_actions_init(actions **slot) {
    if (!slot) return 22;
    auto *a = new (std::nothrow) actions; if (!a) return 12;
    const int error = posix_spawn_file_actions_init(&a->native);
    if (error) { delete a; return mm_darwin_errno(error); }
    *slot = a; return 0;
}
int darwin_actions_destroy(actions **) DARWIN(posix_spawn_file_actions_destroy);
int darwin_actions_destroy(actions **slot) {
    auto *a = object<actions>(slot); if (!a) return 22;
    const int error = posix_spawn_file_actions_destroy(&a->native); delete a; *slot = nullptr; return mm_darwin_errno(error);
}
int darwin_actions_dup2(actions **, int, int) DARWIN(posix_spawn_file_actions_adddup2);
int darwin_actions_dup2(actions **slot, int fd, int newfd) {
    return append(object<actions>(slot), {0, fd, newfd, 0, 0, {}});
}
int darwin_actions_close(actions **, int) DARWIN(posix_spawn_file_actions_addclose);
int darwin_actions_close(actions **slot, int fd) {
    return append(object<actions>(slot), {1, fd, 0, 0, 0, {}});
}
int darwin_actions_open(actions **, int, const char *, int, unsigned) DARWIN(posix_spawn_file_actions_addopen);
int darwin_actions_open(actions **slot, int fd, const char *path, int flags, unsigned mode) {
    if (!path) return 22;
    return append(object<actions>(slot), {2, fd, 0, mm_darwin_open_flags(flags), mode, path});
}
int darwin_actions_chdir(actions **, const char *) DARWIN(posix_spawn_file_actions_addchdir_np);
int darwin_actions_chdir(actions **slot, const char *path) {
    if (!path) return 22;
    return append(object<actions>(slot), {3, 0, 0, 0, 0, path});
}
int darwin_actions_inherit(actions **, int) DARWIN(posix_spawn_file_actions_addinherit_np);
int darwin_actions_inherit(actions **slot, int fd) { return append(object<actions>(slot), {0, fd, fd, 0, 0, {}}); }
}

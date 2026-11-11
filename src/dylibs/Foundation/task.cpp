/* NSTask and the pipes it writes through.
 *
 * An app spawns helpers: a plug-in scanner, an updater, a crash reporter.
 * The object model is the whole of it — a task is a program, its
 * arguments, its environment and its exit status — and launching goes
 * through the host's fork/exec, because a helper is a separate process
 * either way.
 *
 * A helper that is itself an arm64 Mach-O cannot run on the host: exec
 * refuses it, and machoman has no nested launcher yet. That is reported
 * as a launch failure (the status a shell gives a program it cannot run),
 * which is what a caller already handles — a missing helper is a feature
 * it does without, not a crash. */
#include "foundation.h"

#include <cerrno>
#include <cstdlib>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fnd {

FND_CLASS(NSTask);
FND_CLASS(NSPipe);
FND_CLASS(NSFileHandle);

namespace {

Class g_task, g_pipe, g_filehandle;

/* ---- NSFileHandle -------------------------------------------------------- */

struct filehandle_object {
    mm_objc_object base;
    int fd = -1;
    bool owns = false;
    ~filehandle_object() { if (owns && fd >= 0) ::close(fd); }
};
filehandle_object *fh(id o) { return as<filehandle_object>(o); }

id filehandle_create(int fd, bool owns) {
    auto *o = make<filehandle_object>(g_filehandle);
    o->fd = fd;
    o->owns = owns;
    return &o->base;
}

std::uint64_t fh_fileDescriptor(id self, SEL) { return static_cast<std::uint64_t>(fh(self)->fd); }
std::uint64_t fh_readDataToEndOfFile(id self, SEL) {
    std::string bytes;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fh(self)->fd, buf, sizeof buf);
        if (n <= 0) break;
        bytes.append(buf, static_cast<std::size_t>(n));
    }
    return ret(autorelease(data_create(bytes.data(), static_cast<CFIndex>(bytes.size()), false)));
}
std::uint64_t fh_availableData(id self, SEL) {
    char buf[4096];
    const ssize_t n = ::read(fh(self)->fd, buf, sizeof buf);
    return ret(autorelease(data_create(buf, n > 0 ? static_cast<CFIndex>(n) : 0, false)));
}
std::uint64_t fh_writeData(id self, SEL, id data) {
    if (!is_data(data)) return 0;
    const std::string &bytes = data_storage(data);
    ssize_t written = 0;
    while (written < static_cast<ssize_t>(bytes.size())) {
        const ssize_t n = ::write(fh(self)->fd, bytes.data() + written, bytes.size() - static_cast<std::size_t>(written));
        if (n <= 0) break;
        written += n;
    }
    return 0;
}
/* The process's own descriptors, which a task can hand to a child. */
std::uint64_t fh_standard_input(id, SEL) { return ret(autorelease(filehandle_create(0, false))); }
std::uint64_t fh_standard_output(id, SEL) { return ret(autorelease(filehandle_create(1, false))); }
std::uint64_t fh_standard_error(id, SEL) { return ret(autorelease(filehandle_create(2, false))); }

std::uint64_t fh_closeFile(id self, SEL) {
    filehandle_object *o = fh(self);
    if (o->fd >= 0 && o->owns) ::close(o->fd);
    o->fd = -1;
    return 0;
}

/* ---- NSPipe -------------------------------------------------------------- */

struct pipe_object {
    mm_objc_object base;
    id reader = nullptr, writer = nullptr;
    ~pipe_object() { release(reader); release(writer); }
};
pipe_object *pp(id o) { return as<pipe_object>(o); }

std::uint64_t pipec_pipe(id c, SEL) {
    auto *o = make<pipe_object>(reinterpret_cast<Class>(c));
    int fds[2] = {-1, -1};
    if (::pipe(fds) == 0) {
        o->reader = retain(filehandle_create(fds[0], true));
        o->writer = retain(filehandle_create(fds[1], true));
    }
    return ret(autorelease(&o->base));
}
std::uint64_t pipe_fileHandleForReading(id self, SEL) { return ret(pp(self)->reader); }
std::uint64_t pipe_fileHandleForWriting(id self, SEL) { return ret(pp(self)->writer); }

/* ---- NSTask -------------------------------------------------------------- */

struct task_object {
    mm_objc_object base;
    std::string path, directory;
    std::vector<std::string> arguments;
    id environment = nullptr;
    id standard_output = nullptr, standard_error = nullptr, standard_input = nullptr;
    id termination_handler = nullptr;
    pid_t pid = -1;
    int status = 0, launch_error = 0;
    bool running = false, launched = false;
    ~task_object() {
        release(environment);
        release(standard_output);
        release(standard_error);
        release(standard_input);
        release(termination_handler);
    }
};
task_object *tk(id o) { return as<task_object>(o); }

/* The descriptor a standardOutput/standardError value stands for: either
 * an NSFileHandle or an NSPipe's writing end. */
int descriptor_of(id o, bool for_writing) {
    if (!o) return -1;
    if (is_kind(o, g_filehandle)) return fh(o)->fd;
    if (is_kind(o, g_pipe)) {
        id end = for_writing ? pp(o)->writer : pp(o)->reader;
        return end ? fh(end)->fd : -1;
    }
    return -1;
}

bool is_macho(const std::string &path) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::uint32_t magic = 0;
    const bool read = std::fread(&magic, 1, 4, f) == 4;
    std::fclose(f);
    return read && (magic == 0xFEEDFACFu || magic == 0xCFFAEDFEu || magic == 0xBEBAFECAu || magic == 0xCAFEBABEu);
}

std::uint64_t task_setLaunchPath(id self, SEL, id path) { tk(self)->path = to_utf8(path); return 0; }
std::uint64_t task_launchPath(id self, SEL) { return ret(autorelease(string_from_utf8(tk(self)->path))); }
std::uint64_t task_setExecutableURL(id self, SEL, id url) { tk(self)->path = url_path(url); return 0; }
std::uint64_t task_executableURL(id self, SEL) { return ret(autorelease(url_from_path(tk(self)->path, false))); }
std::uint64_t task_setArguments(id self, SEL, id array) {
    task_object *t = tk(self);
    t->arguments.clear();
    if (!array) return 0;
    const auto n = static_cast<std::uint64_t>(send(array, "count"));
    for (std::uint64_t i = 0; i < n; i++)
        t->arguments.push_back(to_utf8(obj(send(array, "objectAtIndex:", i))));
    return 0;
}
std::uint64_t task_arguments(id self, SEL) {
    std::vector<id> items;
    for (const std::string &a : tk(self)->arguments) items.push_back(autorelease(string_from_utf8(a)));
    return ret(autorelease(array_create(items.data(), static_cast<CFIndex>(items.size()), false)));
}
std::uint64_t task_setCurrentDirectoryPath(id self, SEL, id path) { tk(self)->directory = to_utf8(path); return 0; }
std::uint64_t task_setEnvironment(id self, SEL, id env) {
    task_object *t = tk(self);
    if (t->environment) release(t->environment);
    t->environment = env ? retain(env) : nullptr;
    return 0;
}
std::uint64_t task_environment(id self, SEL) { return ret(tk(self)->environment); }

std::uint64_t task_set_output(id self, SEL, id handle) {
    task_object *t = tk(self);
    if (t->standard_output) release(t->standard_output);
    t->standard_output = handle ? retain(handle) : nullptr;
    return 0;
}
std::uint64_t task_set_error(id self, SEL, id handle) {
    task_object *t = tk(self);
    if (t->standard_error) release(t->standard_error);
    t->standard_error = handle ? retain(handle) : nullptr;
    return 0;
}
std::uint64_t task_set_input(id self, SEL, id handle) {
    task_object *t = tk(self);
    if (t->standard_input) release(t->standard_input);
    t->standard_input = handle ? retain(handle) : nullptr;
    return 0;
}
std::uint64_t task_standardOutput(id self, SEL) { return ret(tk(self)->standard_output); }
std::uint64_t task_standardError(id self, SEL) { return ret(tk(self)->standard_error); }
std::uint64_t task_standardInput(id self, SEL) { return ret(tk(self)->standard_input); }

int spawn(task_object *t) {
    std::vector<std::string> argv{t->path};
    argv.insert(argv.end(), t->arguments.begin(), t->arguments.end());
    std::vector<char *> raw;
    for (std::string &a : argv) raw.push_back(a.data());
    raw.push_back(nullptr);

    std::vector<std::string> env_strings;
    std::vector<char *> env_raw;
    if (t->environment) {
        std::vector<id> keys;
        dictionary_keys(t->environment, keys);
        for (id k : keys)
            env_strings.push_back(to_utf8(k) + "=" + to_utf8(dictionary_get(t->environment, k)));
        for (std::string &e : env_strings) env_raw.push_back(e.data());
        env_raw.push_back(nullptr);
    }

    const int out = descriptor_of(t->standard_output, true);
    const int err = descriptor_of(t->standard_error, true);
    const int in = descriptor_of(t->standard_input, false);

    const pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (!t->directory.empty() && ::chdir(t->directory.c_str()) != 0) ::_exit(127);
        if (in >= 0) ::dup2(in, 0);
        if (out >= 0) ::dup2(out, 1);
        if (err >= 0) ::dup2(err, 2);
        if (t->environment) ::execve(t->path.c_str(), raw.data(), env_raw.data());
        else ::execv(t->path.c_str(), raw.data());
        ::_exit(127);
    }
    t->pid = pid;
    t->running = true;
    t->launched = true;
    return 0;
}

void reap(task_object *t) {
    if (!t->running || t->pid <= 0) return;
    int status = 0;
    if (::waitpid(t->pid, &status, 0) == t->pid) {
        t->status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        t->running = false;
    }
}

std::uint64_t task_launch(id self, SEL) {
    task_object *t = tk(self);
    if (t->path.empty()) return 0;
    /* A helper that is itself a Mach-O is started the way this process was
     * started: through machoman, with the same venv. The runtime publishes
     * both in the environment (mm_runtime_launch). */
    if (is_macho(t->path)) {
        if (std::getenv("MACHOMAN_DISABLE_GUEST_SUBPROCESSES")) {
            std::fprintf(stderr, "Foundation: NSTask: guest helper disabled for this diagnostic run: %s\n", t->path.c_str());
            t->launch_error = ENOMEM;
            t->status = 127;
            return 0;
        }
        const char *launcher = std::getenv("MACHOMAN_EXE");
        const char *venv = std::getenv("MACHOMAN_VENV");
        if (!launcher || !venv) {
            std::fprintf(stderr, "Foundation: NSTask: %s is a Mach-O and there is no launcher to run it with\n",
                         t->path.c_str());
            t->launched = true;
            t->status = 127;
            return 0;
        }
        std::vector<std::string> rest = t->arguments;
        t->arguments.clear();
        t->arguments.push_back("run");
        t->arguments.push_back(venv);
        t->arguments.push_back(t->path);
        t->arguments.insert(t->arguments.end(), rest.begin(), rest.end());
        t->path = launcher;
        if (trace())
            std::fprintf(stderr, "Foundation: NSTask: launching %s through %s\n", t->arguments[2].c_str(), launcher);
    }
    if (spawn(t) != 0)
        std::fprintf(stderr, "Foundation: NSTask: cannot run %s: %s\n", t->path.c_str(), std::strerror(errno));
    return 0;
}
std::uint64_t task_launchAndReturnError(id self, SEL, id *error) {
    task_launch(self, nullptr);
    if (error) {
        *error = nullptr;
        if (tk(self)->launch_error)
            *error = obj(send(reinterpret_cast<id>(cls("NSError")), "errorWithDomain:code:userInfo:",
                              ret(autorelease(string_from_utf8("NSPOSIXErrorDomain"))), tk(self)->launch_error, 0));
    }
    return tk(self)->launched ? 1 : 0;
}
std::uint64_t task_waitUntilExit(id self, SEL) {
    reap(tk(self));
    id handler = tk(self)->termination_handler;
    if (handler) {
        const std::uint64_t arg = ret(self);
        mm_objc_block_invoke(handler, 1, &arg);
    }
    return 0;
}
std::uint64_t task_isRunning(id self, SEL) {
    task_object *t = tk(self);
    if (t->running && t->pid > 0) {
        int status = 0;
        if (::waitpid(t->pid, &status, WNOHANG) == t->pid) {
            t->status = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
            t->running = false;
        }
    }
    return t->running ? 1 : 0;
}
std::uint64_t task_terminate(id self, SEL) {
    task_object *t = tk(self);
    if (t->running && t->pid > 0) ::kill(t->pid, SIGTERM);
    return 0;
}
std::uint64_t task_interrupt(id self, SEL) {
    task_object *t = tk(self);
    if (t->running && t->pid > 0) ::kill(t->pid, SIGINT);
    return 0;
}
std::uint64_t task_terminationStatus(id self, SEL) { return static_cast<std::uint64_t>(tk(self)->status); }
/* NSTaskTerminationReasonExit = 1, ...UncaughtSignal = 2. */
std::uint64_t task_terminationReason(id self, SEL) { return tk(self)->status < 128 ? 1 : 2; }
std::uint64_t task_processIdentifier(id self, SEL) { return static_cast<std::uint64_t>(tk(self)->pid); }
std::uint64_t task_setTerminationHandler(id self, SEL, id block) {
    task_object *t = tk(self);
    if (t->termination_handler) release(t->termination_handler);
    t->termination_handler = block ? retain(block) : nullptr;
    return 0;
}
std::uint64_t taskc_launchedTaskWithLaunchPath(id c, SEL, id path, id arguments) {
    id task = obj(send(reinterpret_cast<id>(c), "alloc"));
    task = obj(send(task, "init"));
    send(task, "setLaunchPath:", ret(path));
    send(task, "setArguments:", ret(arguments));
    send(task, "launch");
    return ret(autorelease(task));
}

} // namespace

void register_task() {
    Class nsobject = cls("NSObject");

    g_filehandle = mm_objc_define_class("NSFileHandle", nsobject, sizeof(filehandle_object),
                                        FND_CLASS_REF(NSFileHandle), FND_META_REF(NSFileHandle));
    register_lifecycle<filehandle_object>(g_filehandle);
    FND_METHODS(g_filehandle,
        {"fileDescriptor", IMP(fh_fileDescriptor), "i16@0:8"},
        {"readDataToEndOfFile", IMP(fh_readDataToEndOfFile), "@16@0:8"},
        {"availableData", IMP(fh_availableData), "@16@0:8"},
        {"writeData:", IMP(fh_writeData), "v24@0:8@16"},
        {"closeFile", IMP(fh_closeFile), "v16@0:8"});
    FND_CLASS_METHODS(g_filehandle,
        {"fileHandleWithStandardInput", IMP(fh_standard_input), "@16@0:8"},
        {"fileHandleWithStandardOutput", IMP(fh_standard_output), "@16@0:8"},
        {"fileHandleWithStandardError", IMP(fh_standard_error), "@16@0:8"});

    g_pipe = mm_objc_define_class("NSPipe", nsobject, sizeof(pipe_object), FND_CLASS_REF(NSPipe), FND_META_REF(NSPipe));
    register_lifecycle<pipe_object>(g_pipe);
    FND_METHODS(g_pipe,
        {"fileHandleForReading", IMP(pipe_fileHandleForReading), "@16@0:8"},
        {"fileHandleForWriting", IMP(pipe_fileHandleForWriting), "@16@0:8"});
    FND_CLASS_METHODS(g_pipe, {"pipe", IMP(pipec_pipe), "@16@0:8"});

    g_task = mm_objc_define_class("NSTask", nsobject, sizeof(task_object), FND_CLASS_REF(NSTask), FND_META_REF(NSTask));
    register_lifecycle<task_object>(g_task);
    FND_METHODS(g_task,
        {"setLaunchPath:", IMP(task_setLaunchPath), "v24@0:8@16"},
        {"launchPath", IMP(task_launchPath), "@16@0:8"},
        {"setExecutableURL:", IMP(task_setExecutableURL), "v24@0:8@16"},
        {"executableURL", IMP(task_executableURL), "@16@0:8"},
        {"setArguments:", IMP(task_setArguments), "v24@0:8@16"},
        {"arguments", IMP(task_arguments), "@16@0:8"},
        {"setCurrentDirectoryPath:", IMP(task_setCurrentDirectoryPath), "v24@0:8@16"},
        {"setEnvironment:", IMP(task_setEnvironment), "v24@0:8@16"},
        {"environment", IMP(task_environment), "@16@0:8"},
        {"setStandardOutput:", IMP(task_set_output), "v24@0:8@16"},
        {"setStandardError:", IMP(task_set_error), "v24@0:8@16"},
        {"setStandardInput:", IMP(task_set_input), "v24@0:8@16"},
        {"standardOutput", IMP(task_standardOutput), "@16@0:8"},
        {"standardError", IMP(task_standardError), "@16@0:8"},
        {"standardInput", IMP(task_standardInput), "@16@0:8"},
        {"launch", IMP(task_launch), "v16@0:8"},
        {"launchAndReturnError:", IMP(task_launchAndReturnError), "B24@0:8^@16"},
        {"waitUntilExit", IMP(task_waitUntilExit), "v16@0:8"},
        {"isRunning", IMP(task_isRunning), "B16@0:8"},
        {"terminate", IMP(task_terminate), "v16@0:8"},
        {"interrupt", IMP(task_interrupt), "v16@0:8"},
        {"terminationStatus", IMP(task_terminationStatus), "i16@0:8"},
        {"terminationReason", IMP(task_terminationReason), "q16@0:8"},
        {"processIdentifier", IMP(task_processIdentifier), "i16@0:8"},
        {"setTerminationHandler:", IMP(task_setTerminationHandler), "v24@0:8@?16"});
    FND_CLASS_METHODS(g_task,
        {"launchedTaskWithLaunchPath:arguments:", IMP(taskc_launchedTaskWithLaunchPath), "@32@0:8@16@24"});
}

} // namespace fnd

/* kqueue/kevent on top of epoll.
 *
 * The filters a Darwin program actually uses on files and sockets map onto
 * epoll (READ/WRITE), timerfd (TIMER) and eventfd (USER); the rest of BSD's
 * filter set (VNODE, PROC, SIGNAL) has no equivalent and is
 * refused with EINVAL rather than silently never firing, so a program that
 * depends on one fails where it registers instead of deadlocking later.
 *
 * Only the fields a client can observe are modelled: ident, filter, flags,
 * fflags, data and udata. One kqueue is one epoll fd plus the registration
 * table below. */
#include "internal.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <mutex>
#include <memory>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

/* struct kevent / kevent64_s from <sys/event.h>. */
struct kevent_s {
    std::uint64_t ident;
    std::int16_t filter;
    std::uint16_t flags;
    std::uint32_t fflags;
    std::int64_t data;
    std::uint64_t udata;
};
struct kevent64_s {
    std::uint64_t ident;
    std::int16_t filter;
    std::uint16_t flags;
    std::uint32_t fflags;
    std::int64_t data;
    std::uint64_t udata;
    std::uint64_t ext[2];
};

constexpr std::int16_t EVFILT_READ = -1, EVFILT_WRITE = -2, EVFILT_TIMER = -7, EVFILT_USER = -10, EVFILT_MACHPORT = -8;
constexpr std::uint16_t EV_ADD = 0x0001, EV_DELETE = 0x0002, EV_ENABLE = 0x0004, EV_DISABLE = 0x0008,
                        EV_ONESHOT = 0x0010, EV_CLEAR = 0x0020, EV_RECEIPT = 0x0040, EV_ERROR = 0x4000,
                        EV_EOF = 0x8000;
/* EVFILT_USER fflags */
constexpr std::uint32_t NOTE_FFNOP = 0x00000000, NOTE_FFAND = 0x40000000, NOTE_FFOR = 0x80000000,
                        NOTE_FFCOPY = 0xc0000000, NOTE_FFCTRLMASK = 0xc0000000, NOTE_FFLAGSMASK = 0x00ffffff,
                        NOTE_TRIGGER = 0x01000000;
/* EVFILT_TIMER fflags: the unit of `data`. */
constexpr std::uint32_t NOTE_SECONDS = 0x00000001, NOTE_USECONDS = 0x00000002, NOTE_NSECONDS = 0x00000004,
                        NOTE_ABSOLUTE = 0x00000008;

struct registration {
    std::uint64_t ident = 0;
    std::int16_t filter = 0;
    std::uint16_t flags = 0;
    std::uint32_t fflags = 0;
    std::int64_t data = 0;
    std::uint64_t udata = 0;
    bool enabled = true;
    int helper_fd = -1;   /* timerfd/eventfd for TIMER and USER, else -1 */
    std::uint64_t mach_watch_token = 0;
    bool user_triggered = false;
};

struct kqueue_state {
    std::mutex mu;
    int epoll_fd = -1;
    /* Key: filter in the high 32 bits, ident in the low ones. */
    std::unordered_map<std::uint64_t, registration> registrations;
};

std::mutex g_queues_mu;
std::unordered_map<int, std::shared_ptr<kqueue_state>> g_queues;

bool trace_kqueue() {
    const char *value = std::getenv("MACHOMAN_KQUEUE_TRACE");
    return value && value[0] && std::strcmp(value, "0");
}

std::shared_ptr<kqueue_state> queue_for(int fd) {
    std::lock_guard lock(g_queues_mu);
    auto it = g_queues.find(fd);
    return it == g_queues.end() ? nullptr : it->second;
}

std::uint64_t key_of(std::uint64_t ident, std::int16_t filter) {
    return (static_cast<std::uint64_t>(static_cast<std::uint16_t>(filter)) << 48) | (ident & 0xFFFFFFFFFFFFull);
}

/* The fd epoll watches for a registration: the file itself for READ/WRITE,
 * the helper for TIMER/USER. */
int watched_fd(const registration &r) { return r.helper_fd >= 0 ? r.helper_fd : static_cast<int>(r.ident); }

bool arm_timer(registration &r) {
    if (r.helper_fd < 0) {
        r.helper_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (r.helper_fd < 0) return false;
    }
    std::int64_t ns = r.data;
    if (r.fflags & NOTE_SECONDS) ns *= 1000000000;
    else if (r.fflags & NOTE_USECONDS) ns *= 1000;
    else if (!(r.fflags & NOTE_NSECONDS)) ns *= 1000000; /* milliseconds by default */
    if (ns < 0) ns = 0;
    itimerspec its{};
    its.it_value.tv_sec = static_cast<time_t>(ns / 1000000000);
    its.it_value.tv_nsec = static_cast<long>(ns % 1000000000);
    if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0) its.it_value.tv_nsec = 1;
    if (!(r.flags & EV_ONESHOT)) its.it_interval = its.it_value;
    return timerfd_settime(r.helper_fd, (r.fflags & NOTE_ABSOLUTE) ? TFD_TIMER_ABSTIME : 0, &its, nullptr) == 0;
}

/* Applies one change; returns 0 or a Darwin errno. */
int apply_change(kqueue_state &q, const kevent64_s &change) {
    const std::uint64_t key = key_of(change.ident, change.filter);
    auto it = q.registrations.find(key);

    if (change.flags & EV_DELETE) {
        if (it == q.registrations.end()) return 2; /* ENOENT */
        epoll_ctl(q.epoll_fd, EPOLL_CTL_DEL, watched_fd(it->second), nullptr);
        if (it->second.filter == EVFILT_MACHPORT) mm_mach_unwatch_port(static_cast<std::uint32_t>(it->second.ident), it->second.mach_watch_token);
        if (it->second.helper_fd >= 0) close(it->second.helper_fd);
        q.registrations.erase(it);
        return 0;
    }
    if (change.filter != EVFILT_READ && change.filter != EVFILT_WRITE && change.filter != EVFILT_TIMER &&
        change.filter != EVFILT_USER && change.filter != EVFILT_MACHPORT)
        return 22; /* EINVAL: no Linux equivalent for this filter */

    if (it == q.registrations.end()) {
        if (!(change.flags & EV_ADD)) return 2;
        registration r;
        r.ident = change.ident;
        r.filter = change.filter;
        r.flags = change.flags;
        r.fflags = change.fflags;
        r.data = change.data;
        r.udata = change.udata;
        if (change.filter == EVFILT_TIMER) {
            if (!arm_timer(r)) return errno;
        } else if (change.filter == EVFILT_USER) {
            r.helper_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (r.helper_fd < 0) return errno;
        } else if (change.filter == EVFILT_MACHPORT) {
            r.helper_fd = mm_mach_watch_port(static_cast<std::uint32_t>(r.ident), &r.mach_watch_token);
            if (r.helper_fd < 0) return errno;
        }
        epoll_event ev{};
        ev.events = (change.filter == EVFILT_WRITE) ? EPOLLOUT : EPOLLIN;
        if (change.flags & EV_CLEAR) ev.events |= EPOLLET;
        ev.data.u64 = key;
        if (epoll_ctl(q.epoll_fd, EPOLL_CTL_ADD, watched_fd(r), &ev) != 0) {
            const int e = errno;
            if (r.filter == EVFILT_MACHPORT) mm_mach_unwatch_port(static_cast<std::uint32_t>(r.ident), r.mach_watch_token);
            if (r.helper_fd >= 0) close(r.helper_fd);
            return e;
        }
        q.registrations.emplace(key, r);
        return 0;
    }

    registration &r = it->second;
    r.udata = change.udata;
    if (change.flags & EV_ENABLE) r.enabled = true;
    if (change.flags & EV_DISABLE) r.enabled = false;
    if (change.filter == EVFILT_USER) {
        const std::uint32_t ctrl = change.fflags & NOTE_FFCTRLMASK;
        const std::uint32_t bits = change.fflags & NOTE_FFLAGSMASK;
        if (ctrl == NOTE_FFAND) r.fflags &= bits;
        else if (ctrl == NOTE_FFOR) r.fflags |= bits;
        else if (ctrl == NOTE_FFCOPY) r.fflags = bits;
        else if (ctrl == NOTE_FFNOP && bits) r.fflags |= bits;
        if (change.fflags & NOTE_TRIGGER) {
            r.user_triggered = true;
            const std::uint64_t one = 1;
            if (write(r.helper_fd, &one, 8) < 0) { /* already signalled */ }
        }
    } else if (change.filter == EVFILT_TIMER && (change.flags & EV_ADD)) {
        r.data = change.data;
        r.fflags = change.fflags;
        r.flags = change.flags;
        if (!arm_timer(r)) return errno;
    }
    return 0;
}

int wait_events(kqueue_state &q, kevent64_s *out, int nevents, const timespec *timeout) {
    std::vector<epoll_event> events(static_cast<std::size_t>(nevents));
    int ms = -1;
    if (timeout) {
        const std::int64_t total = static_cast<std::int64_t>(timeout->tv_sec) * 1000 + timeout->tv_nsec / 1000000;
        ms = static_cast<int>(total < 0 ? 0 : total);
    }
    const int n = epoll_wait(q.epoll_fd, events.data(), nevents, ms);
    if (n < 0) return -1;

    int produced = 0;
    for (int i = 0; i < n && produced < nevents; i++) {
        std::lock_guard lock(q.mu);
        auto it = q.registrations.find(events[i].data.u64);
        if (it == q.registrations.end()) continue;
        registration &r = it->second;
        if (!r.enabled) continue;

        kevent64_s &e = out[produced];
        e = {};
        e.ident = r.ident;
        e.filter = r.filter;
        e.flags = 0;
        e.fflags = r.fflags;
        e.udata = r.udata;
        if (events[i].events & EPOLLHUP) e.flags |= EV_EOF;

        if (r.filter == EVFILT_TIMER) {
            std::uint64_t expirations = 0;
            if (read(r.helper_fd, &expirations, 8) != 8) expirations = 1;
            e.data = static_cast<std::int64_t>(expirations);
        } else if (r.filter == EVFILT_USER) {
            std::uint64_t value = 0;
            if (read(r.helper_fd, &value, 8) != 8) value = 1;
            r.user_triggered = false;
            e.data = 0;
        } else if (r.filter == EVFILT_MACHPORT) {
            std::uint64_t count = 0; int alive = 1;
            if (mm_mach_watch_ready(static_cast<std::uint32_t>(r.ident), &count, &alive) < 0) { e.flags |= EV_ERROR; e.data = 5; }
            else { e.data = static_cast<std::int64_t>(count); if (!alive) e.flags |= EV_EOF; }
        } else {
            /* How much can be read/written, as BSD reports. */
            int available = 0;
            if (r.filter == EVFILT_READ && ioctl(static_cast<int>(r.ident), FIONREAD, &available) != 0) available = 1;
            e.data = available;
        }
        produced++;

        if (r.flags & EV_ONESHOT) {
            epoll_ctl(q.epoll_fd, EPOLL_CTL_DEL, watched_fd(r), nullptr);
            if (r.filter == EVFILT_MACHPORT) mm_mach_unwatch_port(static_cast<std::uint32_t>(r.ident), r.mach_watch_token);
            if (r.helper_fd >= 0) close(r.helper_fd);
            q.registrations.erase(it);
        }
    }
    return produced;
}

} // namespace

extern "C" {

int darwin_close(int) DARWIN(close);
int darwin_close(int fd) {
    const int saved_errno = errno;
    std::shared_ptr<kqueue_state> q;
    {
        std::lock_guard lock(g_queues_mu);
        if (const auto it = g_queues.find(fd); it != g_queues.end()) { q = it->second; g_queues.erase(it); }
    }
    if (q) {
        std::lock_guard lock(q->mu);
        for (auto &[key, registration] : q->registrations) {
            (void)key;
            if (registration.filter == EVFILT_MACHPORT)
                mm_mach_unwatch_port(static_cast<std::uint32_t>(registration.ident), registration.mach_watch_token);
            if (registration.helper_fd >= 0) close(registration.helper_fd);
        }
        q->registrations.clear();
    }
    errno = saved_errno;
    return close(fd);
}

int darwin_kqueue() DARWIN(kqueue);
int darwin_kqueue() {
    const int fd = epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0) return -1;
    auto q = std::make_shared<kqueue_state>();
    q->epoll_fd = fd;
    std::lock_guard lock(g_queues_mu);
    g_queues.emplace(fd, q);
    return fd;
}

int darwin_kevent64(int kq, const kevent64_s *changes, int nchanges, kevent64_s *events, int nevents,
                    unsigned flags, const timespec *timeout) DARWIN(kevent64);
int darwin_kevent64(int kq, const kevent64_s *changes, int nchanges, kevent64_s *events, int nevents,
                    unsigned flags, const timespec *timeout) {
    (void)flags;
    auto q = queue_for(kq);
    if (!q) { errno = EBADF; return -1; }
    int produced = 0;
    for (int i = 0; i < nchanges; i++) {
        int err;
        {
            std::lock_guard lock(q->mu);
            err = apply_change(*q, changes[i]);
        }
        if (trace_kqueue()) std::fprintf(stderr, "[kqueue] change queue=%d ident=%llu filter=%d flags=0x%x fflags=0x%x data=%lld error=%d\n",
            kq, static_cast<unsigned long long>(changes[i].ident), changes[i].filter, changes[i].flags, changes[i].fflags,
            static_cast<long long>(changes[i].data), err);
        /* BSD reports a failed change through the event list. */
        if ((err || (changes[i].flags & EV_RECEIPT)) && events && produced < nevents) {
            events[produced] = changes[i];
            events[produced].flags = EV_ERROR;
            events[produced].data = err;
            produced++;
        } else if (err) {
            errno = err;
            return -1;
        }
    }
    if (produced || !events || nevents == 0) return produced;
    return wait_events(*q, events, nevents, timeout);
}

int darwin_kevent(int kq, const kevent_s *changes, int nchanges, kevent_s *events, int nevents,
                  const timespec *timeout) DARWIN(kevent);
int darwin_kevent(int kq, const kevent_s *changes, int nchanges, kevent_s *events, int nevents,
                  const timespec *timeout) {
    std::vector<kevent64_s> in(static_cast<std::size_t>(nchanges > 0 ? nchanges : 0));
    for (int i = 0; i < nchanges; i++)
        in[static_cast<std::size_t>(i)] = {changes[i].ident, changes[i].filter, changes[i].flags, changes[i].fflags,
                                           changes[i].data,  changes[i].udata, {0, 0}};
    std::vector<kevent64_s> out(static_cast<std::size_t>(nevents > 0 ? nevents : 0));
    const int n = darwin_kevent64(kq, in.data(), nchanges, out.data(), nevents, 0, timeout);
    for (int i = 0; i < n; i++) {
        const kevent64_s &e = out[static_cast<std::size_t>(i)];
        events[i] = {e.ident, e.filter, e.flags, e.fflags, e.data, e.udata};
    }
    return n;
}

} // extern "C"

/* ---- the little bit of Mach a Darwin program needs ---------------------- */

/* Mach ports have no Linux equivalent; a program uses these to name itself
 * and to ask for memory, so the task port is a fixed non-zero token and the
 * vm_ calls go to mmap. Anything that would actually message another task
 * has no implementation and says so. */
extern "C" {

} // extern "C"

namespace {
std::mutex g_semaphores_mu;
std::unordered_map<unsigned, sem_t *> g_semaphores;
unsigned g_next_semaphore = 0x1000;

/* Darwin task_info counts 32-bit words and packs these records to four
 * bytes even on arm64. Linux values describe the complete host process,
 * including translations and native framework memory. */
#pragma pack(push, 4)
struct task_basic_record {
    std::uint64_t virtual_size, resident_size, resident_peak;
    std::int32_t user_seconds, user_microseconds, system_seconds, system_microseconds, policy, suspend_count;
};
struct task_legacy_record {
    std::int32_t suspend_count;
    std::uint64_t virtual_size, resident_size;
    std::int32_t user_seconds, user_microseconds, system_seconds, system_microseconds, policy;
};
struct task_vm_record {
    std::uint64_t virtual_size;
    std::int32_t region_count, page_size;
    std::uint64_t resident_size, resident_peak, device, device_peak, internal, internal_peak,
        external, external_peak, reusable, reusable_peak, purgeable_pmap, purgeable_resident,
        purgeable_virtual, compressed, compressed_peak, compressed_lifetime, phys_footprint,
        min_address, max_address;
};
#pragma pack(pop)
static_assert(sizeof(task_basic_record) == 48 && sizeof(task_legacy_record) == 40);
static_assert(sizeof(task_vm_record) == 168 && offsetof(task_vm_record, phys_footprint) == 144);

struct process_memory {
    std::uint64_t virtual_size = 0, resident = 0, peak = 0, anonymous = 0, file = 0, swap = 0;
};
bool read_process_memory(process_memory &memory) {
    FILE *status = std::fopen("/proc/self/status", "r");
    if (!status) return false;
    char line[256];
    while (std::fgets(line, sizeof line, status)) {
        char key[64];
        unsigned long long kb;
        if (std::sscanf(line, "%63s %llu kB", key, &kb) != 2) continue;
        const std::uint64_t bytes = kb * 1024;
        if (!std::strcmp(key, "VmSize:")) memory.virtual_size = bytes;
        else if (!std::strcmp(key, "VmRSS:")) memory.resident = bytes;
        else if (!std::strcmp(key, "VmHWM:")) memory.peak = bytes;
        else if (!std::strcmp(key, "RssAnon:")) memory.anonymous = bytes;
        else if (!std::strcmp(key, "RssFile:")) memory.file = bytes;
        else if (!std::strcmp(key, "VmSwap:")) memory.swap = bytes;
    }
    std::fclose(status);
    return memory.virtual_size != 0;
}

sem_t *semaphore_for(unsigned port) {
    std::lock_guard lock(g_semaphores_mu);
    auto it = g_semaphores.find(port);
    return it == g_semaphores.end() ? nullptr : it->second;
}
} // namespace

extern "C" {

extern __attribute__((visibility("default"))) unsigned mach_task_self_storage __asm__("_mach_task_self_");
unsigned mach_task_self_storage = 1;

unsigned darwin_mach_task_self() DARWIN(mach_task_self);
unsigned darwin_mach_task_self() { return mach_task_self_storage; }
unsigned darwin_mach_host_self() DARWIN(mach_host_self);
unsigned darwin_mach_host_self() { return 2; }

/* Thread scheduling policy. Real-time and precedence policies are a Mach
 * concept; the closest a Linux process can do without privileges is
 * nothing, so these succeed and change no scheduling. A program uses them
 * to ask for a better audio thread, not to depend on one. */
int darwin_thread_policy_set(unsigned, int, void *, unsigned) DARWIN(thread_policy_set);
int darwin_thread_policy_set(unsigned, int, void *, unsigned) { return 0; }
int darwin_thread_policy_get(unsigned, int, void *, unsigned *count, int *get_default) DARWIN(thread_policy_get);
int darwin_thread_policy_get(unsigned, int, void *, unsigned *count, int *get_default) {
    if (count) *count = 0;
    if (get_default) *get_default = 1;
    return 0;
}
int darwin_thread_info(unsigned, int, void *, unsigned *count) DARWIN(thread_info);
int darwin_thread_info(unsigned, int, void *, unsigned *count) {
    if (count) *count = 0;
    return 4; /* KERN_INVALID_ARGUMENT: no thread info to report */
}
int darwin_task_info(unsigned, int, void *, unsigned *count) DARWIN(task_info);
int darwin_task_info(unsigned task, int flavor, void *out, unsigned *count) {
    if (task != mach_task_self_storage || !out || !count) return 4;
    unsigned words = 0;
    if (flavor == 20) words = sizeof(task_basic_record) / 4;
    else if (flavor == 5 || flavor == 18) words = sizeof(task_legacy_record) / 4;
    else if (flavor == 22) {
        if (*count >= 42) words = 42; /* TASK_VM_INFO_REV2 */
        else if (*count >= 38) words = 38; /* REV1: footprint */
        else if (*count >= 36) words = 36; /* REV0 */
    }
    if (!words || *count < words) return 4;
    process_memory memory;
    if (!read_process_memory(memory)) return 5; /* KERN_FAILURE */
    if (flavor == 20) {
        const task_basic_record info{memory.virtual_size, memory.resident, memory.peak, 0, 0, 0, 0, 1, 0};
        std::memcpy(out, &info, sizeof info);
    } else if (flavor == 5 || flavor == 18) {
        const task_legacy_record info{0, memory.virtual_size, memory.resident, 0, 0, 0, 0, 1};
        std::memcpy(out, &info, sizeof info);
    } else {
        task_vm_record info{};
        info.virtual_size = memory.virtual_size;
        info.resident_size = memory.resident;
        info.resident_peak = memory.peak;
        info.page_size = static_cast<std::int32_t>(sysconf(_SC_PAGESIZE));
        info.internal = memory.anonymous;
        info.external = memory.file;
        /* Linux has no Mach footprint ledger; RSS plus swapped anonymous
         * pages approximates the process's physical memory obligation. */
        info.phys_footprint = memory.resident + memory.swap;
        FILE *maps = std::fopen("/proc/self/maps", "r");
        if (maps) {
            char line[1024];
            while (std::fgets(line, sizeof line, maps)) {
                unsigned long long start, end;
                if (std::sscanf(line, "%llx-%llx", &start, &end) != 2) continue;
                ++info.region_count;
                if (!info.min_address || start < info.min_address) info.min_address = start;
                info.max_address = std::max(info.max_address, static_cast<std::uint64_t>(end));
            }
            std::fclose(maps);
        }
        std::memcpy(out, &info, words * 4);
    }
    *count = words;
    return 0;
}

/* Mach semaphores: a port name standing for a host semaphore. Programs use
 * these where a pthread condition variable would do, so that is what they
 * are underneath. */
int darwin_semaphore_create(unsigned, unsigned *port, int policy, int value) DARWIN(semaphore_create);
int darwin_semaphore_create(unsigned, unsigned *port, int policy, int value) {
    (void)policy;
    auto *sem = new sem_t;
    if (sem_init(sem, 0, static_cast<unsigned>(value < 0 ? 0 : value)) != 0) { delete sem; return 5; }
    std::lock_guard lock(g_semaphores_mu);
    const unsigned name = g_next_semaphore++;
    g_semaphores.emplace(name, sem);
    if (port) *port = name;
    return 0;
}
int darwin_semaphore_destroy(unsigned, unsigned port) DARWIN(semaphore_destroy);
int darwin_semaphore_destroy(unsigned, unsigned port) {
    std::lock_guard lock(g_semaphores_mu);
    auto it = g_semaphores.find(port);
    if (it == g_semaphores.end()) return 4; /* KERN_INVALID_ARGUMENT */
    sem_destroy(it->second);
    delete it->second;
    g_semaphores.erase(it);
    return 0;
}
int darwin_semaphore_signal(unsigned port) DARWIN(semaphore_signal);
int darwin_semaphore_signal(unsigned port) {
    sem_t *sem = semaphore_for(port);
    if (!sem) return 4;
    return sem_post(sem) == 0 ? 0 : 5;
}
int darwin_semaphore_signal_all(unsigned port) DARWIN(semaphore_signal_all);
int darwin_semaphore_signal_all(unsigned port) {
    sem_t *sem = semaphore_for(port);
    if (!sem) return 4;
    /* Release every waiter: post until nobody is blocked. */
    int waiting = 0;
    sem_getvalue(sem, &waiting);
    for (int i = waiting; i <= 0; i++) sem_post(sem);
    return 0;
}
int darwin_semaphore_wait(unsigned port) DARWIN(semaphore_wait);
int darwin_semaphore_wait(unsigned port) {
    sem_t *sem = semaphore_for(port);
    if (!sem) return 4;
    while (sem_wait(sem) != 0)
        if (errno != EINTR) return 5;
    return 0;
}
/* mach_timespec_t is {unsigned tv_sec; int tv_nsec}, passed by value in
 * two registers. */
int darwin_semaphore_timedwait(unsigned port, unsigned sec, int nsec) DARWIN(semaphore_timedwait);
int darwin_semaphore_timedwait(unsigned port, unsigned sec, int nsec) {
    sem_t *sem = semaphore_for(port);
    if (!sem) return 4;
    timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += static_cast<time_t>(sec);
    deadline.tv_nsec += nsec;
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
    while (sem_timedwait(sem, &deadline) != 0) {
        if (errno == ETIMEDOUT) return 49; /* KERN_OPERATION_TIMED_OUT */
        if (errno != EINTR) return 5;
    }
    return 0;
}

} // extern "C"

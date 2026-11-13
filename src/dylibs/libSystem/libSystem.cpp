/* Host implementation of /usr/lib/libSystem.B.dylib.
 *
 * Only symbols whose Darwin ABI or semantics differ from the host libc are
 * implemented here; everything else (malloc, memcpy, write, ...) is
 * resolved by the runtime through this object's dependency on libc. Each
 * function is exported under its Darwin name (leading underscore kept) via
 * an asm label, so it never collides with — or shadows — the libc symbol
 * it wraps.
 *
 * Guest arguments arrive as plain integer registers, so pointers are guest
 * VAs that are directly dereferenceable (single address space). */
#include "internal.h"
#include "darwin_stat.h"
#include "machoman/macho.h"
#include "machoman/objc_framework.h"
#include "machoman/runtime.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <vector>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int __cxa_atexit(void (*)(void *), void *, void *);

/* ---- stdio ------------------------------------------------------------ */

/* Darwin's stdin/stdout/stderr are the pointer variables __stdinp & co.
 * The guest only ever passes them back to stdio functions, so the host
 * FILE objects serve as-is. */
extern FILE *darwin_stdinp __asm__("___stdinp");
extern FILE *darwin_stdoutp __asm__("___stdoutp");
extern FILE *darwin_stderrp __asm__("___stderrp");
__attribute__((visibility("default"))) FILE *darwin_stdinp = nullptr;
__attribute__((visibility("default"))) FILE *darwin_stdoutp = nullptr;
__attribute__((visibility("default"))) FILE *darwin_stderrp = nullptr;
void init_stdio() __attribute__((constructor));
void init_stdio() { darwin_stdinp = stdin; darwin_stdoutp = stdout; darwin_stderrp = stderr; }

namespace {

const mm_host_api *g_api = nullptr;
thread_local int guest_errno = 0;
thread_local int synchronized_guest_errno = 0;

/* ---- errno ------------------------------------------------------------ */

int to_darwin_errno(int e) {
    switch (e) {
    case EAGAIN:       return 35;
    case EDEADLK:      return 11;
    case ENAMETOOLONG: return 63;
    case ENOLCK:       return 77;
    case ENOSYS:       return 78;
    case ENOTEMPTY:    return 66;
    case ELOOP:        return 62;
    case EOVERFLOW:    return 84;
    case ENOTSUP:      return 45;
    case ETIMEDOUT:    return 60;
    case ENODATA:      return 93; /* ENOATTR */
    case EINPROGRESS:  return 36;
    case EALREADY:     return 37;
    case ENOTSOCK:     return 38;
    case EAFNOSUPPORT: return 47;
    case EADDRINUSE:   return 48;
    case ECONNRESET:   return 54;
    case ECONNREFUSED: return 61;
    case EILSEQ:       return 92;
    case ECANCELED:    return 89;
    default:           return e <= 34 ? e : 45;
    }
}

int from_darwin_errno(int e) {
    switch (e) {
    case 93: return ENODATA;
    case 35: return EAGAIN;
    case 11: return EDEADLK;
    case 63: return ENAMETOOLONG;
    case 77: return ENOLCK;
    case 78: return ENOSYS;
    case 66: return ENOTEMPTY;
    case 62: return ELOOP;
    case 84: return EOVERFLOW;
    case 45: return ENOTSUP;
    case 60: return ETIMEDOUT;
    case 36: return EINPROGRESS;
    case 37: return EALREADY;
    case 38: return ENOTSOCK;
    case 47: return EAFNOSUPPORT;
    case 48: return EADDRINUSE;
    case 54: return ECONNRESET;
    case 61: return ECONNREFUSED;
    case 92: return EILSEQ;
    case 89: return ECANCELED;
    default: return e;
    }
}

/* ---- open/fcntl flags ------------------------------------------------- */

constexpr int D_O_NONBLOCK = 0x4, D_O_APPEND = 0x8, D_O_NOFOLLOW = 0x100, D_O_CREAT = 0x200, D_O_TRUNC = 0x400,
              D_O_EXCL = 0x800, D_O_NOCTTY = 0x20000, D_O_DIRECTORY = 0x100000, D_O_CLOEXEC = 0x1000000;

int open_flags_from_darwin(int f) {
    int r = f & 0x3; /* access mode is the same */
    if (f & D_O_NONBLOCK)  r |= O_NONBLOCK;
    if (f & D_O_APPEND)    r |= O_APPEND;
    if (f & D_O_NOFOLLOW)  r |= O_NOFOLLOW;
    if (f & D_O_CREAT)     r |= O_CREAT;
    if (f & D_O_TRUNC)     r |= O_TRUNC;
    if (f & D_O_EXCL)      r |= O_EXCL;
    if (f & D_O_NOCTTY)    r |= O_NOCTTY;
    if (f & D_O_DIRECTORY) r |= O_DIRECTORY;
    if (f & D_O_CLOEXEC)   r |= O_CLOEXEC;
    return r;
}

int open_flags_to_darwin(int f) {
    int r = f & 0x3;
    if (f & O_NONBLOCK)  r |= D_O_NONBLOCK;
    if (f & O_APPEND)    r |= D_O_APPEND;
    if (f & O_NOFOLLOW)  r |= D_O_NOFOLLOW;
    if (f & O_CREAT)     r |= D_O_CREAT;
    if (f & O_TRUNC)     r |= D_O_TRUNC;
    if (f & O_EXCL)      r |= D_O_EXCL;
    if (f & O_NOCTTY)    r |= D_O_NOCTTY;
    if (f & O_DIRECTORY) r |= D_O_DIRECTORY;
    if (f & O_CLOEXEC)   r |= D_O_CLOEXEC;
    return r;
}

/* ---- struct stat (64-bit inode layout, the only one on arm64) --------- */

/* ---- dirent ------------------------------------------------------------ */

struct darwin_dirent {
    std::uint64_t d_ino;
    std::uint64_t d_seekoff;
    std::uint16_t d_reclen;
    std::uint16_t d_namlen;
    std::uint8_t  d_type;
    char          d_name[1024];
};

/* ---- pthread objects: Darwin layouts are bigger than glibc's, so the
 * host object lives inside the guest one after an 8-byte signature. ---- */

constexpr std::uint64_t MM_SIG = 0x4D4D5458'4F424A31ull; /* "MMTXOBJ1" */

template <typename Host>
Host *host_object(void *guest, std::size_t guest_size, void (*init)(Host *)) {
    static_assert(sizeof(Host) <= 56);
    if (guest_size < 8 + sizeof(Host))
        std::abort();
    auto *sig = static_cast<std::uint64_t *>(guest);
    auto *obj = reinterpret_cast<Host *>(static_cast<std::uint8_t *>(guest) + 8);
    if (*sig != MM_SIG) { /* static initializer or fresh memory: lazily initialise */
        init(obj);
        *sig = MM_SIG;
    }
    return obj;
}

pthread_mutex_t *mutex_of(void *m) {
    return host_object<pthread_mutex_t>(m, 64, [](pthread_mutex_t *x) { pthread_mutex_init(x, nullptr); });
}

/* ---- TLS: dyld's tlv_bootstrap ---------------------------------------- */

struct tlv_descriptor { void *thunk; std::uint64_t key; std::uint64_t offset; };

/* The image's thread-local template: __thread_data followed by
 * __thread_bss, as laid out in memory. One copy per host thread. */
struct tlv_image { const std::uint8_t *data = nullptr; std::size_t data_size = 0, total_size = 0; };

tlv_image find_tlv_image(const void *header, std::uint64_t slide) {
    tlv_image t;
    if (!header)
        return t;
    const auto *hdr = static_cast<const mm_mach_header_64 *>(header);
    std::span<const std::uint8_t> bytes(static_cast<const std::uint8_t *>(header),
                                        sizeof(mm_mach_header_64) + hdr->sizeofcmds);
    mm_macho_image img;
    if (mm_macho_parse(bytes, img) != mm_macho_status::ok)
        return t;
    std::uint64_t lo = ~0ull, hi = 0, data_end = 0;
    for (const auto *seg : img.segments)
        for (const auto &sect : img.sections(*seg)) {
            const auto name = mm_macho_name(sect.sectname);
            if (name != "__thread_data" && name != "__thread_bss")
                continue;
            lo = std::min(lo, sect.addr + slide);
            hi = std::max(hi, sect.addr + slide + sect.size);
            if (name == "__thread_data")
                data_end = std::max(data_end, sect.addr + slide + sect.size);
        }
    if (hi <= lo)
        return t;
    t.data = reinterpret_cast<const std::uint8_t *>(lo);
    t.total_size = hi - lo;
    t.data_size = data_end > lo ? data_end - lo : 0;
    return t;
}

/* Darwin pthread_key_t is unsigned long, and its values belong to the
 * guest, not glibc's key namespace. In particular, glibc must never call a
 * Mach-O destructor directly after the runtime has restored the host GS. */
constexpr std::size_t guest_key_limit = 512;
struct guest_key { bool used = false; std::uint64_t generation = 0; const void *destructor = nullptr; };
std::array<guest_key, guest_key_limit> g_keys;
std::mutex g_keys_mu;
struct guest_key_value { std::uint64_t generation = 0; const void *value = nullptr; };
struct thread_local_state {
    std::unordered_map<const void *, std::unique_ptr<std::uint8_t[]>> blocks;
    std::array<guest_key_value, guest_key_limit> values{};
    std::vector<std::pair<const void *, void *>> destructors;
    bool destroying = false;
};
/* Ownership is a private host pthread key, rather than a C++ TLS destructor:
 * guest destructors must run before the TLV backing storage is released.
 * Runtime-owned threads explicitly drain it while their identity is active;
 * native framework workers use the host-key destructor as a fallback. */
thread_local thread_local_state *t_local_state = nullptr;
void destroy_thread_local_state(void *value);
pthread_key_t local_state_key() {
    static const pthread_key_t key = [] {
        pthread_key_t created;
        if (pthread_key_create(&created, destroy_thread_local_state) != 0) std::abort();
        return created;
    }();
    return key;
}
thread_local_state &local_state() {
    if (!t_local_state) {
        auto state = std::make_unique<thread_local_state>();
        if (pthread_setspecific(local_state_key(), state.get()) != 0) std::abort();
        t_local_state = state.release();
    }
    return *t_local_state;
}
void destroy_thread_local_state(void *value) {
    auto *state = static_cast<thread_local_state *>(value);
    if (!state || state->destroying) return;
    state->destroying = true;
    auto drain_tlv = [&] {
        while (!state->destructors.empty()) {
            const auto [function, argument] = state->destructors.back();
            state->destructors.pop_back();
            const auto arg = reinterpret_cast<std::uintptr_t>(argument);
            mm_call_callback(function, &arg, 1);
        }
    };
    drain_tlv();
    /* POSIX clears a value before calling its destructor and permits a
     * destructor to re-arm the key, for PTHREAD_DESTRUCTOR_ITERATIONS passes. */
    for (int pass = 0; pass < 4; ++pass) {
        bool invoked = false;
        for (std::size_t i = 0; i < guest_key_limit; ++i) {
            const void *function = nullptr, *argument = nullptr;
            {
                std::lock_guard lock(g_keys_mu);
                auto &slot = state->values[i];
                const auto &key = g_keys[i];
                if (key.used && key.generation == slot.generation && slot.value && key.destructor) {
                    function = key.destructor;
                    argument = slot.value;
                    slot.value = nullptr;
                }
            }
            if (function) {
                invoked = true;
                const auto arg = reinterpret_cast<std::uintptr_t>(argument);
                mm_call_callback(function, &arg, 1);
            }
        }
        drain_tlv();
        if (!invoked) break;
    }
    pthread_setspecific(local_state_key(), nullptr);
    t_local_state = nullptr;
    delete state;
}

} // namespace

extern "C" {

__attribute__((visibility("default"))) void machoman_dylib_guest_enter() {
    errno = from_darwin_errno(guest_errno);
    synchronized_guest_errno = guest_errno;
}
__attribute__((visibility("default"))) void machoman_dylib_guest_leave() {
    /* Guest code may have written through the pointer returned by __error.
     * At an import trap those writes precede the upcoming host API call. */
    if (guest_errno != synchronized_guest_errno) errno = from_darwin_errno(guest_errno);
    else guest_errno = to_darwin_errno(errno);
    synchronized_guest_errno = guest_errno;
}

/* ---- runtime hookup ---------------------------------------------------- */

__attribute__((visibility("default"))) void machoman_dylib_init(const mm_host_api *api) {
    g_api = api;
    mm_libsystem_environment_init(api);
}

__attribute__((visibility("default"))) void machoman_dylib_thread_exit() {
    destroy_thread_local_state(t_local_state);
}

} /* extern "C" */

const mm_host_api *mm_libsystem_api() { return g_api; }
int mm_darwin_errno(int error) { return to_darwin_errno(error); }
int mm_darwin_open_flags(int flags) { return open_flags_from_darwin(flags); }

std::uint64_t mm_call_callback(const void *fn, const std::uint64_t *args, int n) {
    if (g_api && g_api->is_guest_code(reinterpret_cast<std::uintptr_t>(fn)))
        return g_api->call_guest(reinterpret_cast<std::uintptr_t>(fn), args, n);
    using fn8 = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                  std::uint64_t, std::uint64_t);
    std::uint64_t a[8] = {};
    for (int i = 0; i < n && i < 8; i++) a[i] = args[i];
    return reinterpret_cast<fn8>(const_cast<void *>(fn))(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}

extern "C" {

/* ---- errno ------------------------------------------------------------ */

int *darwin_error() DARWIN(_error);
int *darwin_error() {
    if (!g_api) guest_errno = to_darwin_errno(errno);
    return &guest_errno;
}

int darwin_strerror_r(int errnum, char *buf, std::size_t len) DARWIN(strerror_r);
int darwin_strerror_r(int errnum, char *buf, std::size_t len) {
    std::snprintf(buf, len, "%s", std::strerror(from_darwin_errno(errnum)));
    return 0;
}

/* ---- files ------------------------------------------------------------- */

/* Apple Libc string/NetBSD/memset_s.c: bounds errors still overwrite smax
 * bytes when the destination size itself is valid. Return Darwin errors
 * without changing errno. Volatile stores keep secret erasure observable. */
int darwin_memset_s(void *destination, std::size_t capacity, int byte, std::size_t count) DARWIN(memset_s);
int darwin_memset_s(void *destination, std::size_t capacity, int byte, std::size_t count) {
    constexpr auto rsize_max = std::numeric_limits<std::size_t>::max() >> 1;
    if (!destination) return 22; /* EINVAL */
    if (capacity > rsize_max) return 7; /* E2BIG */
    int error = 0;
    if (count > rsize_max) { error = 7; count = capacity; }
    if (count > capacity) { error = 84; count = capacity; } /* Darwin EOVERFLOW */
    auto *out = static_cast<volatile unsigned char *>(destination);
    for (std::size_t i = 0; i < count; ++i) out[i] = static_cast<unsigned char>(byte);
    return error;
}

/* open(path, flags, ...): the mode is a variadic argument, i.e. on the guest stack. */
int darwin_open(const char *path, int flags, ...) DARWIN(open);
int darwin_open(const char *path, int flags, ...) {
    const int mode = (flags & 0x200) ? mm_guest_va_list::at_call_site(2).next_i32() : 0; /* O_CREAT */
    return open(path, open_flags_from_darwin(flags), mode);
}

int darwin_fcntl(int fd, int cmd, ...) DARWIN(fcntl);
int darwin_fcntl(int fd, int cmd, ...) {
    /* GETFD/GETFL have no anonymous argument; do not read a guest stack
     * slot for a parameter the caller did not supply. */
    const long arg = cmd == 1 || cmd == 3 ? 0 : static_cast<long>(mm_guest_va_list::at_call_site(2).next_i64());
    switch (cmd) {
    case 0:  return fcntl(fd, F_DUPFD, arg);
    case 1:  return fcntl(fd, F_GETFD);
    case 2:  return fcntl(fd, F_SETFD, arg); /* FD_CLOEXEC is 1 on both */
    case 3: {
        const int r = fcntl(fd, F_GETFL);
        return r < 0 ? r : open_flags_to_darwin(r);
    }
    case 4:  return fcntl(fd, F_SETFL, open_flags_from_darwin(static_cast<int>(arg)));
    case 7: case 8: case 9: /* F_GETLK, F_SETLK, F_SETLKW */
    case 90: case 91: case 92: { /* open-file-description variants */
        /* SDK sys/fcntl.h: the offsets precede pid/type/whence on both
         * Darwin architectures. Linux puts type and whence first. */
        struct darwin_flock {
            std::int64_t start, length;
            std::int32_t pid;
            std::int16_t type, whence;
        };
        static_assert(sizeof(darwin_flock) == 24);
        auto *guest = reinterpret_cast<darwin_flock *>(arg);
        if (!guest) { errno = EFAULT; return -1; }
        struct flock host{};
        switch (guest->type) {
        case 1: host.l_type = F_RDLCK; break;
        case 2: host.l_type = F_UNLCK; break;
        case 3: host.l_type = F_WRLCK; break;
        default: errno = EINVAL; return -1;
        }
        host.l_whence = guest->whence;
        host.l_start = guest->start;
        host.l_len = guest->length;
        host.l_pid = guest->pid;
        int operation;
        switch (cmd) {
        case 7: operation = F_GETLK; break;
        case 8: operation = F_SETLK; break;
        case 9: operation = F_SETLKW; break;
        case 90: operation = F_OFD_SETLK; break;
        case 91: operation = F_OFD_SETLKW; break;
        default: operation = F_OFD_GETLK; break;
        }
        const int result = fcntl(fd, operation, &host);
        if (result == 0 && (cmd == 7 || cmd == 92)) {
            guest->type = host.l_type == F_RDLCK ? 1 : host.l_type == F_WRLCK ? 3 : 2;
            guest->whence = host.l_whence;
            guest->start = host.l_start;
            guest->length = host.l_len;
            guest->pid = host.l_pid;
        }
        return result;
    }
    case 67: return fcntl(fd, F_DUPFD_CLOEXEC, arg);
    default:
        errno = EINVAL;
        return -1;
    }
}

int darwin_fstat(int fd, darwin_stat_t *out) DARWIN(fstat);
int darwin_fstat(int fd, darwin_stat_t *out) {
    struct stat h;
    if (fstat(fd, &h) < 0) return -1;
    convert_stat(h, out);
    return 0;
}

int darwin_stat(const char *path, darwin_stat_t *out) DARWIN(stat);
int darwin_stat(const char *path, darwin_stat_t *out) {
    struct stat h;
    if (stat(path, &h) < 0) return -1;
    convert_stat(h, out);
    return 0;
}

int darwin_lstat(const char *path, darwin_stat_t *out) DARWIN(lstat);
int darwin_lstat(const char *path, darwin_stat_t *out) {
    struct stat h;
    if (lstat(path, &h) < 0) return -1;
    convert_stat(h, out);
    return 0;
}

int darwin_readdir_r(DIR *dir, darwin_dirent *entry, darwin_dirent **result) DARWIN(readdir_r);
int darwin_readdir_r(DIR *dir, darwin_dirent *entry, darwin_dirent **result) {
    errno = 0;
    const dirent *h = readdir(dir);
    if (!h) {
        *result = nullptr;
        return errno;
    }
    entry->d_ino = h->d_ino;
    entry->d_seekoff = static_cast<std::uint64_t>(h->d_off);
    entry->d_type = h->d_type;
    const std::size_t n = std::strlen(h->d_name);
    entry->d_namlen = static_cast<std::uint16_t>(n);
    std::memcpy(entry->d_name, h->d_name, n + 1);
    entry->d_reclen = static_cast<std::uint16_t>(sizeof(darwin_dirent) - sizeof(entry->d_name) + n + 1);
    *result = entry;
    return 0;
}

darwin_dirent *darwin_readdir(DIR *dir) DARWIN(readdir);
darwin_dirent *darwin_readdir(DIR *dir) {
    static thread_local darwin_dirent entry;
    darwin_dirent *result = nullptr;
    darwin_readdir_r(dir, &entry, &result);
    return result;
}

int darwin_ioctl(int fd, unsigned long req, ...) DARWIN(ioctl);
int darwin_ioctl(int fd, unsigned long req, ...) {
    void *arg = mm_guest_va_list::at_call_site(2).next_ptr<void>();
    switch (req) {
    case 0x40087468: return ioctl(fd, TIOCGWINSZ, arg);
    case 0x4004667F: return ioctl(fd, FIONREAD, arg);
    case 0x8004667E: return ioctl(fd, FIONBIO, arg);
    default:
        errno = ENOTTY;
        return -1;
    }
}

/* ---- memory ------------------------------------------------------------ */

void *darwin_mmap(void *addr, std::size_t len, int prot, int flags, int fd, std::int64_t off) DARWIN(mmap);
void *darwin_mmap(void *addr, std::size_t len, int prot, int flags, int fd, std::int64_t off) {
    int f = flags & 0x3; /* MAP_SHARED / MAP_PRIVATE agree */
    if (flags & 0x10)   f |= MAP_FIXED;
    if (flags & 0x1000) f |= MAP_ANONYMOUS;
    if (flags & 0x40)   f |= MAP_NORESERVE;
    return mmap(addr, len, prot, f, fd, off);
}

int darwin_madvise(void *addr, std::size_t len, int advice) DARWIN(madvise);
int darwin_madvise(void *addr, std::size_t len, int advice) {
    switch (advice) {
    case 0: case 1: case 2: case 3: case 4: return madvise(addr, len, advice);
    case 5: case 7: case 8: return madvise(addr, len, MADV_FREE);
    default: return 0;
    }
}

int darwin_posix_madvise(void *addr, std::size_t len, int advice) DARWIN(posix_madvise);
int darwin_posix_madvise(void *addr, std::size_t len, int advice) { return darwin_madvise(addr, len, advice); }

void darwin_memset_pattern16(void *b, const void *pattern, std::size_t len) DARWIN(memset_pattern16);
void darwin_memset_pattern16(void *b, const void *pattern, std::size_t len) {
    auto *p = static_cast<std::uint8_t *>(b);
    for (std::size_t i = 0; i < len; i++)
        p[i] = static_cast<const std::uint8_t *>(pattern)[i % 16];
}

/* ---- system information ---------------------------------------------- */

long darwin_sysconf(int name) DARWIN(sysconf);
long darwin_sysconf(int name) {
    switch (name) {
    case 1:  return sysconf(_SC_ARG_MAX);
    case 2:  return sysconf(_SC_CHILD_MAX);
    case 3:  return sysconf(_SC_CLK_TCK);
    case 4:  return sysconf(_SC_NGROUPS_MAX);
    case 5:  return sysconf(_SC_OPEN_MAX);
    case 29: return sysconf(_SC_PAGESIZE);
    case 57: return sysconf(_SC_NPROCESSORS_CONF);
    case 58: return sysconf(_SC_NPROCESSORS_ONLN);
    case 70: return sysconf(_SC_GETGR_R_SIZE_MAX);
    case 71: return sysconf(_SC_GETPW_R_SIZE_MAX);
    case 93: return sysconf(_SC_THREAD_STACK_MIN);
    case 200: return sysconf(_SC_PHYS_PAGES);
    default:
        errno = EINVAL;
        return -1;
    }
}

/* confstr: Darwin's per-user directories (_CS_DARWIN_USER_*) have no Linux
 * equivalent, so they are created under the user's runtime directory; a
 * program asking for its temp directory gets a real, writable one. */
std::size_t darwin_confstr(int name, char *buf, std::size_t len) DARWIN(confstr);
std::size_t darwin_confstr(int name, char *buf, std::size_t len) {
    std::string value;
    switch (name) {
    case 1: /* _CS_PATH */
        value = "/usr/bin:/bin:/usr/sbin:/sbin";
        break;
    case 65536:   /* _CS_DARWIN_USER_DIR */
    case 65537:   /* _CS_DARWIN_USER_TEMP_DIR */
    case 65538: { /* _CS_DARWIN_USER_CACHE_DIR */
        /* A display's runtime directory can be read-only to this process.
         * Respect an explicit TMPDIR and only report a directory that we
         * could create and can write to. */
        const char *bases[] = {mm_host_getenv(g_api, "TMPDIR"), std::getenv("XDG_RUNTIME_DIR"), "/tmp"};
        for (const char *base : bases) {
            if (!base || !*base) continue;
            std::string dir = std::string(base) + "/machoman-" + std::to_string(getuid());
            if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) continue;
            if (name == 65537) dir += "/T";
            else if (name == 65538) dir += "/C";
            if (name != 65536 && ::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) continue;
            if (::access(dir.c_str(), W_OK | X_OK) != 0) continue;
            value = dir + "/";
            break;
        }
        if (value.empty()) return 0;
        break;
    }
    default:
        errno = EINVAL;
        return 0;
    }
    const std::size_t need = value.size() + 1;
    if (buf && len) {
        const std::size_t n = std::min(len - 1, value.size());
        std::memcpy(buf, value.data(), n);
        buf[n] = 0;
    }
    return need;
}

/* sysctl: only the few names a Mach-O program actually asks for. Darwin's
 * MIB numbers differ from Linux's and most entries have no equivalent, so
 * unknown queries fail with ENOENT rather than returning something wrong. */
int darwin_sysctlbyname(const char *name, void *out, std::size_t *outlen, const void *, std::size_t) DARWIN(sysctlbyname);
int darwin_sysctlbyname(const char *name, void *out, std::size_t *outlen, const void *, std::size_t) {
    const std::string_view n(name ? name : "");
    auto give_u64 = [&](std::uint64_t v) {
        if (out && outlen && *outlen >= 8) std::memcpy(out, &v, 8);
        else if (out && outlen && *outlen >= 4) { const std::uint32_t v32 = static_cast<std::uint32_t>(v); std::memcpy(out, &v32, 4); }
        if (outlen) *outlen = 8;
        return 0;
    };
    auto give_str = [&](const char *v) {
        const std::size_t need = std::strlen(v) + 1;
        if (out && outlen && *outlen >= need) std::memcpy(out, v, need);
        if (outlen) *outlen = need;
        return 0;
    };
    if (n == "hw.ncpu" || n == "hw.activecpu" || n == "hw.logicalcpu" || n == "hw.logicalcpu_max")
        return give_u64(static_cast<std::uint64_t>(sysconf(_SC_NPROCESSORS_ONLN)));
    if (n == "hw.physicalcpu" || n == "hw.physicalcpu_max")
        return give_u64(static_cast<std::uint64_t>(sysconf(_SC_NPROCESSORS_ONLN)));
    if (n == "hw.memsize")
        return give_u64(static_cast<std::uint64_t>(sysconf(_SC_PHYS_PAGES)) * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE)));
    if (n == "hw.pagesize" || n == "vm.pagesize")
        return give_u64(static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE)));
    if (n == "hw.cachelinesize") return give_u64(64);
    if (n == "hw.cpufrequency") return give_u64(2400000000ull);
    if (n == "hw.model") return give_str("MacBookPro18,1");
    if (n == "hw.machine" || n == "hw.targettype") {
        const auto *api = mm_libsystem_api();
        const auto *ctx = api && api->current_context ? api->current_context() : nullptr;
        return give_str(ctx && ctx->native_mode ? "x86_64" : "arm64");
    }
    if (n == "kern.ostype") return give_str("Darwin");
    if (n == "kern.osrelease") return give_str("22.0.0");
    if (n == "kern.osversion") return give_str("22A380");
    if (n == "kern.version") return give_str("Darwin Kernel Version 22.0.0");
    if (n == "kern.hostname") { char buf[256] = {}; gethostname(buf, sizeof buf - 1); return give_str(buf); }
    if (n == "kern.boottime") {
        struct { std::int64_t sec, usec; } tv{0, 0};
        struct timespec ts;
        clock_gettime(CLOCK_BOOTTIME, &ts);
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        tv.sec = now.tv_sec - ts.tv_sec;
        if (out && outlen && *outlen >= sizeof tv) std::memcpy(out, &tv, sizeof tv);
        if (outlen) *outlen = sizeof tv;
        return 0;
    }
    if (n == "sysctl.proc_translated" || n == "kern.proc_translated") return give_u64(0);
    errno = ENOENT;
    return -1;
}

/* The numeric form: only KERN_PROC/HW_* MIBs the guest uses, mapped onto
 * the names above; anything else fails rather than guessing. */
int darwin_sysctl(int *mib, unsigned namelen, void *out, std::size_t *outlen, const void *newp, std::size_t newlen) DARWIN(sysctl);
int darwin_sysctl(int *mib, unsigned namelen, void *out, std::size_t *outlen, const void *newp, std::size_t newlen) {
    if (!mib || namelen < 2) { errno = EINVAL; return -1; }
    constexpr int CTL_KERN = 1, CTL_HW = 6;
    constexpr int KERN_OSTYPE = 1, KERN_OSRELEASE = 2, KERN_VERSION = 4, KERN_HOSTNAME = 10, KERN_BOOTTIME = 21, KERN_PROC = 14;
    constexpr int HW_NCPU = 3, HW_PAGESIZE = 7, HW_MEMSIZE = 24;
    const char *name = nullptr;
    if (mib[0] == CTL_KERN) {
        switch (mib[1]) {
        case KERN_OSTYPE: name = "kern.ostype"; break;
        case KERN_OSRELEASE: name = "kern.osrelease"; break;
        case KERN_VERSION: name = "kern.version"; break;
        case KERN_HOSTNAME: name = "kern.hostname"; break;
        case KERN_BOOTTIME: name = "kern.boottime"; break;
        case KERN_PROC:
            /* A process listing. kinfo_proc has no Linux equivalent worth
             * synthesising, so the list comes back empty: a program that
             * looks for another instance of itself finds none, which is
             * the truthful answer here. */
            if (outlen) *outlen = 0;
            return 0;
        default: break;
        }
    } else if (mib[0] == CTL_HW) {
        switch (mib[1]) {
        case HW_NCPU: name = "hw.ncpu"; break;
        case HW_PAGESIZE: name = "hw.pagesize"; break;
        case HW_MEMSIZE: name = "hw.memsize"; break;
        default: break;
        }
    }
    if (!name) { errno = ENOENT; return -1; }
    return darwin_sysctlbyname(name, out, outlen, newp, newlen);
}

/* Mach time: nanoseconds, so the timebase is 1/1. */
struct mach_timebase { std::uint32_t numer, denom; };
int darwin_mach_timebase_info(mach_timebase *info) DARWIN(mach_timebase_info);
int darwin_mach_timebase_info(mach_timebase *info) { if (info) *info = {1, 1}; return 0; }

std::uint64_t darwin_mach_absolute_time() DARWIN(mach_absolute_time);
std::uint64_t darwin_mach_absolute_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}
std::uint64_t darwin_mach_continuous_time() DARWIN(mach_continuous_time);
std::uint64_t darwin_mach_continuous_time() {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}
std::uint64_t darwin_clock_gettime_nsec_np(int) DARWIN(clock_gettime_nsec_np);
std::uint64_t darwin_clock_gettime_nsec_np(int) { return darwin_mach_absolute_time(); }

int darwin_clock_gettime(int id, struct timespec *ts) DARWIN(clock_gettime);
int darwin_clock_gettime(int id, struct timespec *ts) {
    switch (id) {
    case 0:  return clock_gettime(CLOCK_REALTIME, ts);
    case 4: case 5: return clock_gettime(CLOCK_MONOTONIC_RAW, ts);
    case 6:  return clock_gettime(CLOCK_MONOTONIC, ts);
    case 8: case 9: return clock_gettime(CLOCK_BOOTTIME, ts);
    case 12: return clock_gettime(CLOCK_PROCESS_CPUTIME_ID, ts);
    case 16: return clock_gettime(CLOCK_THREAD_CPUTIME_ID, ts);
    default:
        errno = EINVAL;
        return -1;
    }
}

int darwin_CCRandomGenerateBytes(void *buf, std::size_t n) DARWIN(CCRandomGenerateBytes);
int darwin_CCRandomGenerateBytes(void *buf, std::size_t n) {
    auto *p = static_cast<std::uint8_t *>(buf);
    while (n > 0) {
        const ssize_t r = getrandom(p, n, 0);
        if (r < 0) return -1;
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return 0;
}

/* ---- signals: guest dispositions and masks ---------------------------- */

/* Darwin struct sigaction: { handler; uint32 sa_mask; int sa_flags } = 16 bytes.
 * Handlers are guest functions; the runtime keeps and delivers them. */
struct darwin_sigaction_t { std::uint64_t handler; std::uint32_t mask; std::int32_t flags; };

std::uint64_t darwin_signal(int sig, std::uint64_t handler) DARWIN(signal);
std::uint64_t darwin_signal(int sig, std::uint64_t handler) {
    return g_api ? g_api->signal_install(sig, handler, 0, 0) : 0;
}

int darwin_sigaction(int sig, const darwin_sigaction_t *act, darwin_sigaction_t *old) DARWIN(sigaction);
int darwin_sigaction(int sig, const darwin_sigaction_t *act, darwin_sigaction_t *old) {
    if (!g_api) return -1;
    std::uint64_t prev;
    if (act) prev = g_api->signal_install(sig, act->handler, static_cast<std::uint32_t>(act->flags), act->mask);
    else { prev = g_api->signal_install(sig, 0, 0, 0); g_api->signal_install(sig, prev, 0, 0); }
    if (old) { old->handler = prev; old->mask = 0; old->flags = 0; }
    return 0;
}

int darwin_sigaltstack(const void *, void *) DARWIN(sigaltstack);
int darwin_sigaltstack(const void *, void *) { return 0; }

/* Darwin's sigset_t is a 32-bit mask, one bit per signal. */
int darwin_sigemptyset(std::uint32_t *set) DARWIN(sigemptyset);
int darwin_sigemptyset(std::uint32_t *set) { *set = 0; return 0; }

int darwin_sigfillset(std::uint32_t *set) DARWIN(sigfillset);
int darwin_sigfillset(std::uint32_t *set) { *set = 0xFFFFFFFFu; return 0; }

int darwin_sigaddset(std::uint32_t *set, int sig) DARWIN(sigaddset);
int darwin_sigaddset(std::uint32_t *set, int sig) { *set |= 1u << (sig - 1); return 0; }

int darwin_sigdelset(std::uint32_t *set, int sig) DARWIN(sigdelset);
int darwin_sigdelset(std::uint32_t *set, int sig) { *set &= ~(1u << (sig - 1)); return 0; }

int darwin_sigismember(const std::uint32_t *set, int sig) DARWIN(sigismember);
int darwin_sigismember(const std::uint32_t *set, int sig) { return set && (*set & (1u << (sig - 1))) ? 1 : 0; }

/* The blocked-signal mask is the guest's own: the runtime, not the host
 * kernel, decides what reaches guest code, and applying the mask to the
 * host thread would be actively harmful — a program that blocks every
 * signal around a thread creation (nanomsg does) would then block SIGSEGV,
 * which is what the runtime uses to catch a guest memory fault.
 *
 * Darwin's `how` values are not Linux's, either: SIG_BLOCK is 1 there and
 * 0 here, and passing Darwin's SIG_SETMASK (3) to the host libc fails with
 * EINVAL — which is exactly the error that stopped Live's helper. */
std::uint32_t initial_signal_mask() {
    const char *mask = std::getenv("MACHOMAN_INITIAL_SIGNAL_MASK");
    return mask ? static_cast<std::uint32_t>(std::strtoul(mask, nullptr, 10)) : 0;
}
thread_local std::uint32_t t_signal_mask = initial_signal_mask();

int darwin_set_signal_mask(int how, const std::uint32_t *set, std::uint32_t *old) {
    const std::uint32_t previous = t_signal_mask;
    if (set) {
        switch (how) {
        case 1: t_signal_mask |= *set; break;   /* SIG_BLOCK */
        case 2: t_signal_mask &= ~*set; break;  /* SIG_UNBLOCK */
        case 3: t_signal_mask = *set; break;    /* SIG_SETMASK */
        default: errno = EINVAL; return -1;
        }
    }
    if (old) *old = previous;
    return 0;
}

int darwin_sigprocmask(int how, const std::uint32_t *set, std::uint32_t *old) DARWIN(sigprocmask);
int darwin_sigprocmask(int how, const std::uint32_t *set, std::uint32_t *old) {
    return darwin_set_signal_mask(how, set, old);
}

/* pthread_sigmask reports the error rather than setting errno. */
int darwin_pthread_sigmask(int how, const std::uint32_t *set, std::uint32_t *old) DARWIN(pthread_sigmask);
int darwin_pthread_sigmask(int how, const std::uint32_t *set, std::uint32_t *old) {
    return darwin_set_signal_mask(how, set, old) == 0 ? 0 : EINVAL;
}

/* ---- process ----------------------------------------------------------- */

int darwin_fork() DARWIN(fork);
int darwin_fork() { return ::fork(); }

/* ---- dyld / process introspection ------------------------------------- */

int *darwin_NSGetArgc() DARWIN(_NSGetArgc);
int *darwin_NSGetArgc() { static int argc; argc = g_api ? g_api->argc : 0; return &argc; }

char ***darwin_NSGetArgv() DARWIN(_NSGetArgv);
char ***darwin_NSGetArgv() { static char **argv; argv = g_api ? g_api->argv : nullptr; return &argv; }


int darwin_NSGetExecutablePath(char *buf, std::uint32_t *size) DARWIN(_NSGetExecutablePath);
int darwin_NSGetExecutablePath(char *buf, std::uint32_t *size) {
    const char *path = g_api && g_api->version >= 6 ? g_api->executable_path
        : g_api && g_api->argc > 0 ? g_api->argv[0] : "";
    const std::size_t need = std::strlen(path) + 1;
    if (*size < need) { *size = static_cast<std::uint32_t>(need); return -1; }
    std::memcpy(buf, path, need);
    return 0;
}

std::uint32_t darwin_dyld_image_count() DARWIN(_dyld_image_count);
std::uint32_t darwin_dyld_image_count() { return g_api ? static_cast<std::uint32_t>(g_api->image_count()) : 0; }

const void *darwin_dyld_get_image_header(std::uint32_t i) DARWIN(_dyld_get_image_header);
const void *darwin_dyld_get_image_header(std::uint32_t i) { return g_api ? g_api->image_header(i) : nullptr; }

std::intptr_t darwin_dyld_get_image_vmaddr_slide(std::uint32_t i) DARWIN(_dyld_get_image_vmaddr_slide);
std::intptr_t darwin_dyld_get_image_vmaddr_slide(std::uint32_t i) { return g_api ? static_cast<std::intptr_t>(g_api->image_slide(i)) : 0; }

const char *darwin_dyld_get_image_name(std::uint32_t i) DARWIN(_dyld_get_image_name);
const char *darwin_dyld_get_image_name(std::uint32_t i) { return g_api ? g_api->image_path(i) : nullptr; }

/* Image notifications. Images are never unloaded here, so a remove
 * callback is remembered and never called; an add callback is invoked
 * immediately for every image already loaded, as dyld does. */
void darwin_dyld_register_func_for_add_image(void (*fn)(const void *, std::intptr_t)) DARWIN(_dyld_register_func_for_add_image);
void darwin_dyld_register_func_for_add_image(void (*fn)(const void *, std::intptr_t)) {
    if (!g_api || !fn) return;
    for (std::size_t i = 0; i < g_api->image_count(); i++) {
        const void *header = g_api->image_header(i);
        const auto slide = static_cast<std::intptr_t>(g_api->image_slide(i));
        const std::uint64_t args[2] = {reinterpret_cast<std::uintptr_t>(header), static_cast<std::uint64_t>(slide)};
        mm_call_callback(reinterpret_cast<const void *>(fn), args, 2);
    }
}
void darwin_dyld_register_func_for_remove_image(void (*)(const void *, std::intptr_t)) DARWIN(_dyld_register_func_for_remove_image);
void darwin_dyld_register_func_for_remove_image(void (*)(const void *, std::intptr_t)) {}

/* libunwind's lookup of the unwind tables of the image containing `addr`.
 * The mapped header's load commands carry unslid section addresses. */
struct dyld_unwind_sections {
    const void *mh;
    const void *dwarf_section;
    std::uintptr_t dwarf_section_length;
    const void *compact_unwind_section;
    std::uintptr_t compact_unwind_section_length;
};
/* Returns int, not bool: the guest reads w0 and x86-64 defines only al. */
int darwin_dyld_find_unwind_sections(void *addr, dyld_unwind_sections *info) DARWIN(_dyld_find_unwind_sections);
int darwin_dyld_find_unwind_sections(void *addr, dyld_unwind_sections *info) {
    if (!g_api) return 0;
    const std::ptrdiff_t i = g_api->image_for_address(reinterpret_cast<std::uintptr_t>(addr));
    if (i < 0) return 0;
    const auto *hdr = static_cast<const mm_mach_header_64 *>(g_api->image_header(static_cast<std::size_t>(i)));
    const std::uint64_t slide = static_cast<std::uint64_t>(g_api->image_slide(static_cast<std::size_t>(i)));
    std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t *>(hdr), sizeof(mm_mach_header_64) + hdr->sizeofcmds);
    mm_macho_image img;
    if (mm_macho_parse(bytes, img) != mm_macho_status::ok) return 0;
    *info = {};
    info->mh = hdr;
    for (const auto *seg : img.segments)
        for (const auto &sect : img.sections(*seg)) {
            const auto name = mm_macho_name(sect.sectname);
            if (name == "__eh_frame") {
                info->dwarf_section = reinterpret_cast<const void *>(sect.addr + slide);
                info->dwarf_section_length = sect.size;
            } else if (name == "__unwind_info") {
                info->compact_unwind_section = reinterpret_cast<const void *>(sect.addr + slide);
                info->compact_unwind_section_length = sect.size;
            }
        }
    if (std::getenv("MACHOMAN_TRACE"))
        std::fprintf(stderr, "dyld: unwind sections for %p: mh=%p eh_frame=%p/%zu unwind_info=%p/%zu\n", addr, info->mh,
                     info->dwarf_section, info->dwarf_section_length, info->compact_unwind_section, info->compact_unwind_section_length);
    return 1;
}

void *darwin_dlsym(void *, const char *) DARWIN(dlsym);
void *darwin_dlsym(void *, const char *name) {
    return g_api && g_api->version >= 4 && g_api->lookup_symbol
        ? reinterpret_cast<void *>(g_api->lookup_symbol(name)) : nullptr;
}

bool darwin_NSIsSymbolNameDefined(const char *name) DARWIN(NSIsSymbolNameDefined);
bool darwin_NSIsSymbolNameDefined(const char *name) {
    if (!name) return false;
    const char *plain = name[0] == '_' ? name + 1 : name;
    if (std::strstr(plain, "GREMEDY")) return false;
    if (dlsym(RTLD_DEFAULT, plain)) return true;
    static void *libgl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (libgl && dlsym(libgl, plain)) return true;
    using proc_fn = void (*)();
    static auto egl_get_proc = reinterpret_cast<proc_fn (*)(const char *)>(dlsym(RTLD_DEFAULT, "eglGetProcAddress"));
    if (egl_get_proc && egl_get_proc(plain)) return true;
    return false;
}

bool darwin_NSIsSymbolNameDefinedInImage(void *, const char *name) DARWIN(NSIsSymbolNameDefinedInImage);
bool darwin_NSIsSymbolNameDefinedInImage(void *, const char *name) {
    return darwin_NSIsSymbolNameDefined(name);
}

void *darwin_NSLookupAndBindSymbol(const char *name) DARWIN(NSLookupAndBindSymbol);
void *darwin_NSLookupAndBindSymbol(const char *name) {
    if (!name) return nullptr;
    const char *plain = name[0] == '_' ? name + 1 : name;
    if (std::strstr(plain, "GREMEDY")) return nullptr;
    if (void *sym = dlsym(RTLD_DEFAULT, plain)) return sym;
    static void *libgl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (libgl) {
        if (void *sym = dlsym(libgl, plain)) return sym;
    }
    using proc_fn = void (*)();
    static auto egl_get_proc = reinterpret_cast<proc_fn (*)(const char *)>(dlsym(RTLD_DEFAULT, "eglGetProcAddress"));
    if (egl_get_proc) {
        if (void *sym = reinterpret_cast<void *>(egl_get_proc(plain))) return sym;
    }
    return nullptr;
}

void *darwin_NSAddressOfSymbol(void *sym) DARWIN(NSAddressOfSymbol);
void *darwin_NSAddressOfSymbol(void *sym) { return sym; }

void darwin_dyld_stub_binder() DARWIN(dyld_stub_binder);
void darwin_dyld_stub_binder() {
    std::fprintf(stderr, "libSystem: dyld_stub_binder reached: a lazy slot was not bound eagerly\n");
    std::abort();
}

/* ---- fenv: Darwin arm64 fenv_t = { uint32 fpsr; uint32 fpcr } --------- */

struct darwin_fenv { std::uint32_t fpsr, fpcr; };
constexpr std::uint32_t FPCR_RMODE = 3u << 22;

int darwin_fegetenv(darwin_fenv *env) DARWIN(fegetenv);
int darwin_fegetenv(darwin_fenv *env) {
    env->fpsr = 0;
    env->fpcr = g_api ? static_cast<std::uint32_t>(g_api->fpcr_get()) : 0;
    return 0;
}
int darwin_fesetenv(const darwin_fenv *env) DARWIN(fesetenv);
int darwin_fesetenv(const darwin_fenv *env) {
    if (g_api) g_api->fpcr_set(env ? env->fpcr : 0); /* FE_DFL_ENV is a null-ish pointer on Darwin */
    return 0;
}
int darwin_feholdexcept(darwin_fenv *env) DARWIN(feholdexcept);
int darwin_feholdexcept(darwin_fenv *env) { return darwin_fegetenv(env); }
int darwin_feupdateenv(const darwin_fenv *env) DARWIN(feupdateenv);
int darwin_feupdateenv(const darwin_fenv *env) { return darwin_fesetenv(env); }
int darwin_fegetround() DARWIN(fegetround);
int darwin_fegetround() { return g_api ? static_cast<int>(g_api->fpcr_get() & FPCR_RMODE) : 0; }
int darwin_fesetround(int mode) DARWIN(fesetround);
int darwin_fesetround(int mode) {
    if (!g_api || (mode & ~FPCR_RMODE)) return -1;
    g_api->fpcr_set((g_api->fpcr_get() & ~static_cast<std::uint64_t>(FPCR_RMODE)) | static_cast<std::uint64_t>(mode));
    return 0;
}
int darwin_feclearexcept(int) DARWIN(feclearexcept);
int darwin_feclearexcept(int) { return 0; }
int darwin_fetestexcept(int) DARWIN(fetestexcept);
int darwin_fetestexcept(int) { return 0; }
int darwin_feraiseexcept(int) DARWIN(feraiseexcept);
int darwin_feraiseexcept(int) { return 0; }
int darwin_fegetexcept() DARWIN(fegetexcept);
int darwin_fegetexcept() { return 0; }
int darwin_fedisableexcept(int) DARWIN(fedisableexcept);
int darwin_fedisableexcept(int) { return 0; }
int darwin_feenableexcept(int) DARWIN(feenableexcept);
int darwin_feenableexcept(int) { return 0; }

/* Darwin SDK sizes: x86_64 jmp_buf = 148 bytes, arm64 = 192 bytes.
 * Match libplatform's register/mask offsets, including the unused arm64
 * word before d8. Buffers may have only int alignment, so use memcpy. */
struct arm64_jmp_state {
    std::uint64_t x[12], sp, reserved, d[8];
    std::uint32_t mask, mask_reserved, save_mask, onstack;
};
struct x86_jmp_state {
    std::uint64_t rbx, rbp, rsp, r12, r13, r14, r15, rip, flags;
    std::uint32_t mxcsr;
    std::uint16_t fpcontrol, fp_reserved;
    std::uint32_t mask, save_mask, onstack;
};
static_assert(sizeof(arm64_jmp_state) == 192 && offsetof(arm64_jmp_state, d) == 112);
static_assert(offsetof(arm64_jmp_state, mask) == 176 && offsetof(arm64_jmp_state, save_mask) == 184);
static_assert(offsetof(x86_jmp_state, mask) == 80 && offsetof(x86_jmp_state, save_mask) == 84);
static_assert(sizeof(x86_jmp_state) <= 148);

int guest_setjmp(void *buffer, bool save_mask) {
    mm_cpu_context *c = g_api ? g_api->current_context() : nullptr;
    if (!c) return 0;
    if (c->native_mode) {
        x86_jmp_state jb{};
        jb.rbx = c->native_x[MM_RBX]; jb.rbp = c->native_x[MM_RBP];
        jb.rsp = c->native_x[MM_RSP] + 8;
        jb.r12 = c->native_x[MM_R12]; jb.r13 = c->native_x[MM_R13];
        jb.r14 = c->native_x[MM_R14]; jb.r15 = c->native_x[MM_R15];
        jb.rip = c->x[30];
        std::memcpy(&jb.mxcsr, c->native_fp + 24, sizeof jb.mxcsr);
        std::memcpy(&jb.fpcontrol, c->native_fp, sizeof jb.fpcontrol);
        if (save_mask) jb.mask = t_signal_mask;
        jb.save_mask = save_mask ? 1u : 0u;
        std::memcpy(buffer, &jb, sizeof jb);
    } else {
        arm64_jmp_state jb{};
        std::memcpy(jb.x, c->x + 19, sizeof jb.x);
        jb.sp = c->x[31];
        for (int i = 0; i < 8; ++i) jb.d[i] = c->v[8 + i][0];
        if (save_mask) jb.mask = t_signal_mask;
        jb.save_mask = save_mask ? 1u : 0u;
        std::memcpy(buffer, &jb, sizeof jb);
    }
    return 0;
}

int guest_longjmp(const void *buffer, int val, bool restore_mask, bool sigjmp) {
    mm_cpu_context *c = g_api ? g_api->current_context() : nullptr;
    if (!c) {
        std::fprintf(stderr, "libSystem: longjmp outside guest execution\n");
        std::abort();
    }
    const int result = val ? val : 1;
    if (c->native_mode) {
        x86_jmp_state jb;
        std::memcpy(&jb, buffer, sizeof jb);
        c->native_x[MM_RBX] = jb.rbx; c->native_x[MM_RBP] = jb.rbp;
        c->native_x[MM_RSP] = jb.rsp;
        c->native_x[MM_R12] = jb.r12; c->native_x[MM_R13] = jb.r13;
        c->native_x[MM_R14] = jb.r14; c->native_x[MM_R15] = jb.r15;
        c->native_x[MM_RAX] = static_cast<std::uint32_t>(result);
        c->native_flags &= ~std::uint64_t{0x400}; // libplatform clears DF; arithmetic flags are volatile.
        // libplatform fninit clears the x87 stack/exceptions before restoring its control word.
        std::memset(c->native_fp, 0, 24);
        std::memcpy(c->native_fp, &jb.fpcontrol, sizeof jb.fpcontrol);
        std::memcpy(c->native_fp + 24, &jb.mxcsr, sizeof jb.mxcsr);
        c->x[29] = jb.rbp; c->x[30] = jb.rip; c->x[31] = jb.rsp;
        c->resume_va = jb.rip; // Saved RSP is already after RET; skip the ordinary host-return pop.
        if (restore_mask || (sigjmp && jb.save_mask)) t_signal_mask = jb.mask;
    } else {
        arm64_jmp_state jb;
        std::memcpy(&jb, buffer, sizeof jb);
        std::memcpy(c->x + 19, jb.x, sizeof jb.x);
        c->x[31] = jb.sp;
        for (int i = 0; i < 8; ++i) c->v[8 + i][0] = jb.d[i];
        if (restore_mask || (sigjmp && jb.save_mask)) t_signal_mask = jb.mask;
    }
    return result;
}

int darwin_setjmp(void *jb) DARWIN(setjmp);
int darwin_setjmp(void *jb) { return guest_setjmp(jb, true); }
int darwin__setjmp(void *jb) DARWIN(_setjmp);
int darwin__setjmp(void *jb) { return guest_setjmp(jb, false); }
int darwin_sigsetjmp(void *jb, int save_mask) DARWIN(sigsetjmp);
int darwin_sigsetjmp(void *jb, int save_mask) { return guest_setjmp(jb, save_mask != 0); }
int darwin_longjmp(const void *jb, int val) DARWIN(longjmp);
int darwin_longjmp(const void *jb, int val) { return guest_longjmp(jb, val, true, false); }
int darwin__longjmp(const void *jb, int val) DARWIN(_longjmp);
int darwin__longjmp(const void *jb, int val) { return guest_longjmp(jb, val, false, false); }
int darwin_siglongjmp(const void *jb, int val) DARWIN(siglongjmp);
int darwin_siglongjmp(const void *jb, int val) { return guest_longjmp(jb, val, false, true); }

/* ---- process exit: guest destructors must run as guest code ------------- */

int darwin_cxa_atexit(std::uint64_t fn, std::uint64_t arg, void *dso) DARWIN(__cxa_atexit);
int darwin_cxa_atexit(std::uint64_t fn, std::uint64_t arg, void *dso) {
    if (g_api && g_api->is_guest_code(fn)) { g_api->atexit_register(fn, arg); return 0; }
    return __cxa_atexit(reinterpret_cast<void (*)(void *)>(fn), reinterpret_cast<void *>(arg), dso);
}
int darwin_atexit(std::uint64_t fn) DARWIN(atexit);
int darwin_atexit(std::uint64_t fn) {
    if (g_api && g_api->is_guest_code(fn)) { g_api->atexit_register(fn, 0); return 0; }
    return atexit(reinterpret_cast<void (*)()>(fn));
}
[[noreturn]] void darwin_exit(int code) DARWIN(exit);
[[noreturn]] void darwin_exit(int code) {
    if (g_api) g_api->atexit_run();
    std::fflush(nullptr);
    std::exit(code);
}

/* ---- stack protector ---------------------------------------------------- */

__attribute__((visibility("default"))) std::uint64_t darwin_stack_chk_guard __asm__("___stack_chk_guard") = 0x2f3d5b7e9ac1d4e6ull;

void darwin_stack_chk_fail() DARWIN(__stack_chk_fail);
void darwin_stack_chk_fail() {
    std::fprintf(stderr, "libSystem: stack check failed in guest\n");
    std::abort();
}

void darwin_chkstk() DARWIN(__chkstk_darwin);
void darwin_chkstk() {} /* stack probe; the guest stack is fully committed */

/* Symbols whose ABI preserves every argument register (see runtime.h). */
__attribute__((visibility("default"), used)) extern const char machoman_preserve_regs[] = "___chkstk_darwin\0";

/* ---- TLS ---------------------------------------------------------------- */

void *darwin_tlv_bootstrap(tlv_descriptor *d) DARWIN(_tlv_bootstrap);
void *darwin_tlv_bootstrap(tlv_descriptor *d) {
    const auto index = g_api ? g_api->image_for_address(reinterpret_cast<std::uintptr_t>(d)) : -1;
    const void *header = index >= 0 ? g_api->image_header(static_cast<std::size_t>(index)) : nullptr;
    const auto slide = index >= 0 ? static_cast<std::uint64_t>(g_api->image_slide(static_cast<std::size_t>(index))) : 0;
    auto &block = local_state().blocks[header];
    if (!block) {
        const tlv_image image = find_tlv_image(header, slide);
        block = std::make_unique<std::uint8_t[]>(image.total_size ? image.total_size : 1);
        if (image.data_size)
            std::memcpy(block.get(), image.data, image.data_size);
    }
    return block.get() + d->offset;
}

void darwin_tlv_atexit(const void *, void *) DARWIN(_tlv_atexit);
void darwin_tlv_atexit(const void *function, void *argument) {
    local_state().destructors.emplace_back(function, argument);
}

/* ---- pthreads ----------------------------------------------------------- */

int darwin_pthread_key_create(std::uint64_t *, const void *) DARWIN(pthread_key_create);
int darwin_pthread_key_create(std::uint64_t *result, const void *destructor) {
    if (!result) return 22;
    std::lock_guard lock(g_keys_mu);
    for (std::size_t i = 0; i < guest_key_limit; ++i) {
        auto &key = g_keys[i];
        if (key.used) continue;
        key.used = true;
        ++key.generation;
        key.destructor = destructor;
        *result = i;
        return 0;
    }
    return 35; /* EAGAIN */
}
int darwin_pthread_key_delete(std::uint64_t) DARWIN(pthread_key_delete);
int darwin_pthread_key_delete(std::uint64_t index) {
    std::lock_guard lock(g_keys_mu);
    if (index >= guest_key_limit || !g_keys[index].used) return 22;
    g_keys[index].used = false;
    g_keys[index].destructor = nullptr;
    return 0;
}
void *darwin_pthread_getspecific(std::uint64_t) DARWIN(pthread_getspecific);
void *darwin_pthread_getspecific(std::uint64_t index) {
    std::lock_guard lock(g_keys_mu);
    if (index >= guest_key_limit || !g_keys[index].used || !t_local_state) return nullptr;
    const auto &slot = t_local_state->values[index];
    return slot.generation == g_keys[index].generation ? const_cast<void *>(slot.value) : nullptr;
}
int darwin_pthread_setspecific(std::uint64_t, const void *) DARWIN(pthread_setspecific);
int darwin_pthread_setspecific(std::uint64_t index, const void *value) {
    std::lock_guard lock(g_keys_mu);
    if (index >= guest_key_limit || !g_keys[index].used) return 22;
    local_state().values[index] = {g_keys[index].generation, value};
    return 0;
}

int darwin_pthread_mutex_init(void *m, const void *attr) DARWIN(pthread_mutex_init);
int darwin_pthread_mutex_init(void *m, const void *attr) {
    pthread_mutexattr_t ha;
    pthread_mutexattr_init(&ha);
    if (attr) {
        const int type = static_cast<const std::int32_t *>(attr)[2]; /* stored by mutexattr_settype below */
        pthread_mutexattr_settype(&ha, type == 2 ? PTHREAD_MUTEX_RECURSIVE : type == 1 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL);
    }
    auto *sig = static_cast<std::uint64_t *>(m);
    auto *hm = reinterpret_cast<pthread_mutex_t *>(static_cast<std::uint8_t *>(m) + 8);
    const int r = pthread_mutex_init(hm, &ha);
    pthread_mutexattr_destroy(&ha);
    *sig = MM_SIG;
    return r;
}
int darwin_pthread_mutex_lock(void *m) DARWIN(pthread_mutex_lock);
int darwin_pthread_mutex_lock(void *m) { return pthread_mutex_lock(mutex_of(m)); }
int darwin_pthread_mutex_trylock(void *m) DARWIN(pthread_mutex_trylock);
int darwin_pthread_mutex_trylock(void *m) { const int r = pthread_mutex_trylock(mutex_of(m)); return r == EBUSY ? 16 : r; }
int darwin_pthread_mutex_unlock(void *m) DARWIN(pthread_mutex_unlock);
int darwin_pthread_mutex_unlock(void *m) { return pthread_mutex_unlock(mutex_of(m)); }
int darwin_pthread_mutex_destroy(void *m) DARWIN(pthread_mutex_destroy);
int darwin_pthread_mutex_destroy(void *m) {
    auto *sig = static_cast<std::uint64_t *>(m);
    if (*sig == MM_SIG) { pthread_mutex_destroy(mutex_of(m)); *sig = 0; }
    return 0;
}

/* Darwin pthread_cond_t is 48 bytes: too small for glibc's plus a
 * signature, so the host object lives on the heap behind a pointer. */
pthread_cond_t *cond_of(void *c) {
    static std::mutex init_lock;
    auto *sig = static_cast<std::uint64_t *>(c);
    auto **slot = reinterpret_cast<pthread_cond_t **>(static_cast<std::uint8_t *>(c) + 8);
    if (*sig == MM_SIG) return *slot;
    std::lock_guard lock(init_lock);
    if (*sig != MM_SIG) {
        auto *hc = new pthread_cond_t;
        pthread_cond_init(hc, nullptr);
        *slot = hc;
        *sig = MM_SIG;
    }
    return *slot;
}

int darwin_pthread_cond_init(void *c, const void *) DARWIN(pthread_cond_init);
int darwin_pthread_cond_init(void *c, const void *) { *static_cast<std::uint64_t *>(c) = 0; cond_of(c); return 0; }
int darwin_pthread_cond_destroy(void *c) DARWIN(pthread_cond_destroy);
int darwin_pthread_cond_destroy(void *c) {
    auto *sig = static_cast<std::uint64_t *>(c);
    if (*sig == MM_SIG) { pthread_cond_t *hc = cond_of(c); pthread_cond_destroy(hc); delete hc; *sig = 0; }
    return 0;
}
int darwin_pthread_cond_wait(void *c, void *m) DARWIN(pthread_cond_wait);
int darwin_pthread_cond_wait(void *c, void *m) { return pthread_cond_wait(cond_of(c), mutex_of(m)); }
int darwin_pthread_cond_timedwait(void *c, void *m, const struct timespec *abs) DARWIN(pthread_cond_timedwait);
int darwin_pthread_cond_timedwait(void *c, void *m, const struct timespec *abs) {
    const int r = pthread_cond_timedwait(cond_of(c), mutex_of(m), abs);
    return r == ETIMEDOUT ? 60 : r;
}
int darwin_pthread_cond_signal(void *c) DARWIN(pthread_cond_signal);
int darwin_pthread_cond_signal(void *c) { return pthread_cond_signal(cond_of(c)); }
int darwin_pthread_cond_broadcast(void *c) DARWIN(pthread_cond_broadcast);
int darwin_pthread_cond_broadcast(void *c) { return pthread_cond_broadcast(cond_of(c)); }
int darwin_pthread_condattr_init(void *) DARWIN(pthread_condattr_init);
int darwin_pthread_condattr_init(void *) { return 0; }
int darwin_pthread_condattr_destroy(void *) DARWIN(pthread_condattr_destroy);
int darwin_pthread_condattr_destroy(void *) { return 0; }

int darwin_pthread_mutexattr_init(void *a) DARWIN(pthread_mutexattr_init);
int darwin_pthread_mutexattr_init(void *a) { std::memset(a, 0, 16); return 0; }
int darwin_pthread_mutexattr_destroy(void *) DARWIN(pthread_mutexattr_destroy);
int darwin_pthread_mutexattr_destroy(void *) { return 0; }
int darwin_pthread_mutexattr_settype(void *a, int type) DARWIN(pthread_mutexattr_settype);
int darwin_pthread_mutexattr_settype(void *a, int type) { static_cast<std::int32_t *>(a)[2] = type; return 0; }

int darwin_pthread_attr_init(void *a) DARWIN(pthread_attr_init);
int darwin_pthread_attr_init(void *a) { std::memset(a, 0, 64); return 0; }
int darwin_pthread_attr_destroy(void *) DARWIN(pthread_attr_destroy);
int darwin_pthread_attr_destroy(void *) { return 0; }
int darwin_pthread_attr_setstacksize(void *a, std::size_t size) DARWIN(pthread_attr_setstacksize);
int darwin_pthread_attr_setstacksize(void *a, std::size_t size) { static_cast<std::uint64_t *>(a)[1] = size; return 0; }

int darwin_pthread_create(std::uint64_t *thread, const void *attr, std::uint64_t start, std::uint64_t arg) DARWIN(pthread_create);
int darwin_pthread_create(std::uint64_t *thread, const void *attr, std::uint64_t start, std::uint64_t arg) {
    if (!g_api) return 78;
    const std::size_t stack = attr ? static_cast<const std::uint64_t *>(attr)[1] : 0;
    const int saved_errno = errno;
    const int result = g_api->thread_create(start, arg, stack, thread);
    errno = saved_errno; /* pthread functions return error numbers without changing errno. */
    return result;
}
int darwin_pthread_join(std::uint64_t thread, std::uint64_t *result) DARWIN(pthread_join);
int darwin_pthread_join(std::uint64_t thread, std::uint64_t *result) { return g_api ? g_api->thread_join(thread, result) : 78; }
int darwin_pthread_detach(std::uint64_t) DARWIN(pthread_detach);
int darwin_pthread_detach(std::uint64_t) { return 0; }
std::uint64_t darwin_pthread_self() DARWIN(pthread_self);
std::uint64_t darwin_pthread_self() { return g_api ? g_api->thread_self() : 0; }

/* Mach ports and pthread IDs have separate namespaces. */
std::uint32_t darwin_pthread_mach_thread_np(std::uint64_t t) DARWIN(pthread_mach_thread_np);
std::uint32_t darwin_pthread_mach_thread_np(std::uint64_t t) { return mm_mach_thread_port(t); }
std::uint32_t darwin_mach_thread_self() DARWIN(mach_thread_self);
std::uint32_t darwin_mach_thread_self() { return mm_mach_thread_port(darwin_pthread_self()); }
/* A guest pthread_t is the runtime's thread id, not a host pthread_t, so
 * every function taking one is answered here rather than reaching the host
 * libc (which would dereference the id as a pointer). Scheduling is the
 * host's: the values are reported as the defaults and setting them
 * succeeds without changing anything, which is also what happens on macOS
 * for a thread that may not raise its own priority. */
int darwin_pthread_equal(std::uint64_t a, std::uint64_t b) DARWIN(pthread_equal);
int darwin_pthread_equal(std::uint64_t a, std::uint64_t b) { return a == b; }

struct darwin_sched_param { int sched_priority; char opaque[4]; };
int darwin_pthread_getschedparam(std::uint64_t, int *policy, darwin_sched_param *param) DARWIN(pthread_getschedparam);
int darwin_pthread_getschedparam(std::uint64_t, int *policy, darwin_sched_param *param) {
    if (policy) *policy = 1; /* SCHED_OTHER on Darwin */
    if (param) *param = {31, {}}; /* the default priority of a Darwin thread */
    return 0;
}
int darwin_pthread_setschedparam(std::uint64_t, int, const darwin_sched_param *) DARWIN(pthread_setschedparam);
int darwin_pthread_setschedparam(std::uint64_t, int, const darwin_sched_param *) { return 0; }
int darwin_pthread_set_qos_class_self_np(int, int) DARWIN(pthread_set_qos_class_self_np);
int darwin_pthread_set_qos_class_self_np(int, int) { return 0; }
int darwin_pthread_kill(std::uint64_t, int) DARWIN(pthread_kill);
int darwin_pthread_kill(std::uint64_t, int) { return 78; /* ENOSYS: signals are per process here */ }
int darwin_pthread_cancel(std::uint64_t) DARWIN(pthread_cancel);
int darwin_pthread_cancel(std::uint64_t) { return 78; }
int darwin_pthread_main_np() DARWIN(pthread_main_np);
int darwin_pthread_main_np() { return g_api && g_api->thread_self() == 1; }

int darwin_pthread_setname_np(const char *) DARWIN(pthread_setname_np);
int darwin_pthread_setname_np(const char *) { return 0; }
int darwin_pthread_threadid_np(std::uint64_t thread, std::uint64_t *id) DARWIN(pthread_threadid_np);
int darwin_pthread_threadid_np(std::uint64_t thread, std::uint64_t *id) { *id = thread ? thread : (g_api ? g_api->thread_self() : 0); return 0; }
void *darwin_pthread_get_stackaddr_np(std::uint64_t) DARWIN(pthread_get_stackaddr_np);
void *darwin_pthread_get_stackaddr_np(std::uint64_t) {
    std::uint64_t base = 0, size = 0;
    if (g_api) g_api->thread_stack(&base, &size);
    return reinterpret_cast<void *>(base);
}
std::size_t darwin_pthread_get_stacksize_np(std::uint64_t) DARWIN(pthread_get_stacksize_np);
std::size_t darwin_pthread_get_stacksize_np(std::uint64_t) {
    std::uint64_t base = 0, size = 0;
    if (g_api) g_api->thread_stack(&base, &size);
    return size;
}
void darwin_pthread_jit_write_protect_np(int) DARWIN(pthread_jit_write_protect_np);
void darwin_pthread_jit_write_protect_np(int) {}
void darwin_sys_icache_invalidate(void *, std::size_t) DARWIN(sys_icache_invalidate);
void darwin_sys_icache_invalidate(void *, std::size_t) {}

/* ---- libdispatch semaphores ------------------------------------------ */

struct dispatch_semaphore_object {
    mm_objc_object header;
    std::mutex mutex;
    std::condition_variable available;
    std::uint64_t value = 0, waiters = 0;
};

static mm_objc_class *dispatch_semaphore_class() {
    static mm_objc_class storage{}, metaclass{};
    static auto *cls = [] {
        auto *c = mm_objc_define_class("OS_dispatch_semaphore", mm_objc_class_named("NSObject"),
            sizeof(dispatch_semaphore_object), &storage, &metaclass);
        mmfw::register_lifecycle<dispatch_semaphore_object>(c);
        return c;
    }();
    return cls;
}

dispatch_semaphore_object *darwin_dispatch_semaphore_create(long value) DARWIN(dispatch_semaphore_create);
dispatch_semaphore_object *darwin_dispatch_semaphore_create(long value) {
    if (value < 0) return nullptr;
    auto *s = mmfw::make<dispatch_semaphore_object>(dispatch_semaphore_class());
    s->value = static_cast<std::uint64_t>(value);
    return s;
}
long darwin_dispatch_semaphore_wait(dispatch_semaphore_object *s, std::uint64_t timeout) DARWIN(dispatch_semaphore_wait);
long darwin_dispatch_semaphore_wait(dispatch_semaphore_object *s, std::uint64_t timeout) {
    std::unique_lock lock(s->mutex);
    if (!s->value) {
        if (timeout == 0) return 49;
        ++s->waiters;
        bool acquired = true;
        if (timeout == ~0ull) {
            s->available.wait(lock, [&] { return s->value != 0; });
        } else {
            // dispatch_time_t is an absolute monotonic nanosecond deadline.
            auto deadline = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(
                static_cast<std::int64_t>(std::min(timeout,
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))));
            acquired = s->available.wait_until(lock, deadline, [&] { return s->value != 0; });
        }
        --s->waiters;
        if (!acquired) return 49;
    }
    --s->value;
    return 0;
}
long darwin_dispatch_semaphore_signal(dispatch_semaphore_object *s) DARWIN(dispatch_semaphore_signal);
long darwin_dispatch_semaphore_signal(dispatch_semaphore_object *s) {
    std::lock_guard lock(s->mutex);
    bool wakes_waiter = s->waiters > s->value;
    ++s->value;
    if (wakes_waiter) s->available.notify_one();
    return wakes_waiter ? 1 : 0;
}
void darwin_dispatch_retain(mm_id object) DARWIN(dispatch_retain);
void darwin_dispatch_retain(mm_id object) { mm_objc_retain(object); }
void darwin_dispatch_release(mm_id object) DARWIN(dispatch_release);
void darwin_dispatch_release(mm_id object) { mm_objc_release(object); }
std::uint64_t darwin_dispatch_time(std::uint64_t base, std::int64_t delta) DARWIN(dispatch_time);
std::uint64_t darwin_dispatch_time(std::uint64_t base, std::int64_t delta) {
    if (base == ~0ull) return base;
    if (base == 0) base = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (delta < 0) {
        auto magnitude = std::uint64_t{0} - static_cast<std::uint64_t>(delta);
        return magnitude >= base ? 1 : base - magnitude;
    }
    auto increment = static_cast<std::uint64_t>(delta);
    return increment >= ~0ull - base ? ~0ull : base + increment;
}

/* Unwinding is not defined here on purpose. Apple ships libunwind inside
 * libSystem, so a guest binds _Unwind_* against this library; leaving them
 * undefined makes the runtime look further and find the guest libunwind,
 * which can actually walk translated frames. A host definition would
 * shadow it.
 *
 */

} // extern "C"

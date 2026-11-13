#include "internal.h"
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <sys/resource.h>

namespace {
constexpr std::uint64_t darwin_rlim_infinity = INT64_MAX;
struct darwin_rlimit { std::uint64_t current, maximum; };
int host_resource(int resource) {
    switch (resource & ~0x1000) { // Darwin's strict POSIX flag
    case 0: return RLIMIT_CPU;
    case 1: return RLIMIT_FSIZE;
    case 2: return RLIMIT_DATA;
    case 3: return RLIMIT_STACK;
    case 4: return RLIMIT_CORE;
    case 5: return RLIMIT_AS;
    case 6: return RLIMIT_MEMLOCK;
    case 7: return RLIMIT_NPROC;
    case 8: return RLIMIT_NOFILE;
    default: return -1;
    }
}
}

/* macOS SDK sys/utsname.h uses five 256-byte fields; Linux uses 65-byte
 * fields and an additional domain name. Forwarding libc uname corrupts
 * the guest's view of every field after sysname. */
struct darwin_utsname {
    char sysname[256], nodename[256], release[256], version[256], machine[256];
};
static_assert(sizeof(darwin_utsname) == 1280 && offsetof(darwin_utsname, release) == 512);
extern "C" {
int darwin_getrlimit(int, darwin_rlimit *) DARWIN(getrlimit);
int darwin_getrlimit(int resource, darwin_rlimit *out) {
    const int host = host_resource(resource);
    if (host < 0) { errno = EINVAL; return -1; }
    if (!out) { errno = EFAULT; return -1; }
    rlimit limit{};
    if (::getrlimit(host, &limit)) return -1;
    darwin_rlimit result{
        limit.rlim_cur == RLIM_INFINITY ? darwin_rlim_infinity : limit.rlim_cur,
        limit.rlim_max == RLIM_INFINITY ? darwin_rlim_infinity : limit.rlim_max};
    const auto *api = mm_libsystem_api();
    if (!api || !api->copy_guest_memory || api->copy_guest_memory(out, &result, sizeof result)) {
        errno = EFAULT; return -1;
    }
    return 0;
}
int darwin_setrlimit(int, const darwin_rlimit *) DARWIN(setrlimit);
int darwin_setrlimit(int resource, const darwin_rlimit *in) {
    const int host = host_resource(resource);
    if (host < 0) { errno = EINVAL; return -1; }
    darwin_rlimit input{};
    const auto *api = mm_libsystem_api();
    if (!in || !api || !api->copy_guest_memory || api->copy_guest_memory(&input, in, sizeof input)) {
        errno = EFAULT; return -1;
    }
    rlimit limit{
        input.current == darwin_rlim_infinity ? RLIM_INFINITY : input.current,
        input.maximum == darwin_rlim_infinity ? RLIM_INFINITY : input.maximum};
    const int result = ::setrlimit(host, &limit);
    if (mm_host_getenv(api, "MACHOMAN_APPKIT_TRACE"))
        std::fprintf(stderr, "libSystem: setrlimit Darwin=%d host=%d cur=%llu max=%llu result=%d\n",
            resource, host, static_cast<unsigned long long>(input.current),
            static_cast<unsigned long long>(input.maximum), result);
    return result;
}
int darwin_sysctlbyname(const char *, void *, std::size_t *, const void *, std::size_t) DARWIN(sysctlbyname);
int darwin_uname(darwin_utsname *) DARWIN(uname);
int darwin_uname(darwin_utsname *out) {
    if (!out) { errno = EFAULT; return -1; }
    darwin_utsname info{};
    const char *names[] = {"kern.ostype", "kern.hostname", "kern.osrelease", "kern.version", "hw.machine"};
    char *fields[] = {info.sysname, info.nodename, info.release, info.version, info.machine};
    for (unsigned i = 0; i < 5; ++i) {
        std::size_t size = 256;
        if (darwin_sysctlbyname(names[i], fields[i], &size, nullptr, 0)) return -1;
    }
    const auto *api = mm_libsystem_api();
    if (!api || api->version < 5 || !api->copy_guest_memory ||
        api->copy_guest_memory(out, &info, sizeof info)) { errno = EFAULT; return -1; }
    return 0;
}
}

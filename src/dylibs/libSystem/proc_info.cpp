#include "internal.h"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
// SDK sys/proc_info.h: PROC_PIDLISTFDS records have the same layout on both guests.
struct darwin_proc_fdinfo { std::int32_t fd; std::uint32_t type; };
static_assert(sizeof(darwin_proc_fdinfo) == 8);
}

extern "C" int darwin_proc_pidinfo(int, int, std::uint64_t, void *, int) DARWIN(proc_pidinfo);
int darwin_proc_pidinfo(int pid, int flavor, std::uint64_t, void *buffer, int size) {
    if (pid <= 0 || size < 0) { errno = EINVAL; return 0; }
    if (flavor != 1) { errno = ENOTSUP; return 0; }
    if (buffer && size < static_cast<int>(sizeof(darwin_proc_fdinfo))) { errno = ENOMEM; return 0; }
    const auto path = "/proc/" + std::to_string(pid) + "/fd";
    auto *directory = ::opendir(path.c_str());
    if (!directory) { if (errno == ENOENT) errno = ESRCH; return 0; }
    std::vector<darwin_proc_fdinfo> descriptors;
    int error = 0;
    for (;;) {
        errno = 0;
        auto *entry = ::readdir(directory);
        if (!entry) { error = errno; break; }
        const std::string name(entry->d_name);
        int fd = 0;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), fd);
        if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size()) continue;
        // The directory opened for this query is not a descriptor owned by the guest.
        if (pid == ::getpid() && fd == ::dirfd(directory)) continue;
        struct stat info{};
        const auto target = path + "/" + name;
        if (::stat(target.c_str(), &info)) continue; // It may close during enumeration.
        std::uint32_t type = S_ISSOCK(info.st_mode) ? 2u : S_ISFIFO(info.st_mode) ? 6u : 1u;
        char link[128];
        const auto length = ::readlink(target.c_str(), link, sizeof link);
        if (length > 0 && std::string_view(link, static_cast<std::size_t>(length)) == "anon_inode:[eventpoll]") type = 5;
        descriptors.push_back({fd, type});
    }
    ::closedir(directory);
    if (error) { errno = error; return 0; }
    std::sort(descriptors.begin(), descriptors.end(), [](const auto &a, const auto &b) { return a.fd < b.fd; });
    const auto count = buffer ? std::min(descriptors.size(), static_cast<std::size_t>(size) / sizeof(darwin_proc_fdinfo)) : descriptors.size();
    const auto bytes = count * sizeof(darwin_proc_fdinfo);
    if (bytes > INT_MAX) { errno = EOVERFLOW; return 0; }
    if (buffer && bytes) {
        const auto *api = mm_libsystem_api();
        if (!api || !api->copy_guest_memory || api->copy_guest_memory(buffer, descriptors.data(), bytes)) {
            errno = EFAULT; return 0;
        }
    }
    return static_cast<int>(bytes);
}

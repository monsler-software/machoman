#include "internal.h"
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {
std::filesystem::path object_path(const char *name, bool create) {
    if (!name || name[0] != '/' || !name[1]) { errno = EINVAL; return {}; }
    std::string encoded;
    constexpr char digits[] = "0123456789abcdef";
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(name + 1); *p; ++p) {
        if (*p == '/') { errno = EINVAL; return {}; }
        encoded += digits[*p >> 4]; encoded += digits[*p & 15];
    }
    if (encoded.size() > 254) { errno = ENAMETOOLONG; return {}; }
    const char *root = mm_host_venv(mm_libsystem_api());
    if (!root) { errno = ENOENT; return {}; }
    const auto directory = std::filesystem::path(root) / ".machoman/ipc/shm";
    if (create) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) { errno = error.value(); return {}; }
    }
    return directory / encoded;
}
}
extern "C" {
/* Named objects belong to a venv. Independent guest processes map the same
 * backing file with MAP_SHARED; unlink removes the name while existing maps
 * retain their pages, matching POSIX shared-memory lifetime. */
int darwin_shm_open(const char *, int, unsigned) DARWIN(shm_open);
int darwin_shm_open(const char *name, int flags, unsigned mode) {
    const auto path = object_path(name, flags & 0x200);
    if (path.empty()) return -1;
    return open(path.c_str(), mm_darwin_open_flags(flags) | O_CLOEXEC | O_NOFOLLOW, mode);
}
int darwin_shm_unlink(const char *) DARWIN(shm_unlink);
int darwin_shm_unlink(const char *name) {
    const auto path = object_path(name, false);
    return path.empty() ? -1 : unlink(path.c_str());
}
}

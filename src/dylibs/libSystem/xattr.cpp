#include "internal.h"
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>
#include <sys/xattr.h>

namespace {
constexpr int nofollow = 1, create = 2, replace = 4, nodefault = 16, showcompression = 32;
bool validate(const char *name, std::uint32_t position, int options, bool setting, bool fd) {
    if (!name) { errno = EFAULT; return false; }
    const auto length = std::strlen(name);
    if (!length) { errno = EINVAL; return false; }
    if (length > 127) { errno = ENAMETOOLONG; return false; }
    if (position) { errno = ENOTSUP; return false; }
    const int allowed = nodefault | (fd ? 0 : nofollow) | (setting ? create | replace : showcompression);
    if (options & ~allowed) { errno = ENOTSUP; return false; }
    if (setting && (options & (create | replace)) == (create | replace)) { errno = EINVAL; return false; }
    return true;
}
std::string native_name(const char *name) { return std::string("user.") + name; }
int native_flags(int options) { return (options & create ? XATTR_CREATE : 0) | (options & replace ? XATTR_REPLACE : 0); }

template <typename List>
ssize_t list(char *output, std::size_t capacity, List get) {
    std::vector<char> native;
    ssize_t count = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        count = get(nullptr, 0);
        if (count < 0) return -1;
        native.resize(static_cast<std::size_t>(count));
        count = get(native.data(), native.size());
        if (count >= 0) break;
        if (errno != ERANGE) return -1;
    }
    if (count < 0) return -1;
    std::string names;
    for (std::size_t offset = 0; offset < static_cast<std::size_t>(count);) {
        const auto length = strnlen(native.data() + offset, static_cast<std::size_t>(count) - offset);
        if (length == static_cast<std::size_t>(count) - offset) { errno = EIO; return -1; }
        const std::string_view name(native.data() + offset, length);
        if (name.starts_with("user.")) { names.append(name.substr(5)); names.push_back('\0'); }
        offset += length + 1;
    }
    if (capacity == 0) return static_cast<ssize_t>(names.size());
    if (capacity < names.size()) { errno = ERANGE; return -1; }
    if (!output) { errno = EFAULT; return -1; }
    std::memcpy(output, names.data(), names.size());
    return static_cast<ssize_t>(names.size());
}
}

extern "C" {
ssize_t darwin_getxattr(const char *, const char *, void *, std::size_t, std::uint32_t, int) DARWIN(getxattr);
ssize_t darwin_getxattr(const char *path, const char *name, void *value, std::size_t size, std::uint32_t position, int options) {
    if (!validate(name, position, options, false, false)) return -1;
    const auto key = native_name(name);
    return options & nofollow ? lgetxattr(path, key.c_str(), value, size) : getxattr(path, key.c_str(), value, size);
}
ssize_t darwin_fgetxattr(int, const char *, void *, std::size_t, std::uint32_t, int) DARWIN(fgetxattr);
ssize_t darwin_fgetxattr(int fd, const char *name, void *value, std::size_t size, std::uint32_t position, int options) {
    if (!validate(name, position, options, false, true)) return -1;
    return fgetxattr(fd, native_name(name).c_str(), value, size);
}
int darwin_setxattr(const char *, const char *, const void *, std::size_t, std::uint32_t, int) DARWIN(setxattr);
int darwin_setxattr(const char *path, const char *name, const void *value, std::size_t size, std::uint32_t position, int options) {
    if (!validate(name, position, options, true, false)) return -1;
    const auto key = native_name(name);
    return options & nofollow ? lsetxattr(path, key.c_str(), value, size, native_flags(options)) : setxattr(path, key.c_str(), value, size, native_flags(options));
}
int darwin_fsetxattr(int, const char *, const void *, std::size_t, std::uint32_t, int) DARWIN(fsetxattr);
int darwin_fsetxattr(int fd, const char *name, const void *value, std::size_t size, std::uint32_t position, int options) {
    if (!validate(name, position, options, true, true)) return -1;
    return fsetxattr(fd, native_name(name).c_str(), value, size, native_flags(options));
}
int darwin_removexattr(const char *, const char *, int) DARWIN(removexattr);
int darwin_removexattr(const char *path, const char *name, int options) {
    if (!validate(name, 0, options, false, false)) return -1;
    const auto key = native_name(name);
    return options & nofollow ? lremovexattr(path, key.c_str()) : removexattr(path, key.c_str());
}
int darwin_fremovexattr(int, const char *, int) DARWIN(fremovexattr);
int darwin_fremovexattr(int fd, const char *name, int options) {
    if (!validate(name, 0, options, false, true)) return -1;
    return fremovexattr(fd, native_name(name).c_str());
}
ssize_t darwin_listxattr(const char *, char *, std::size_t, int) DARWIN(listxattr);
ssize_t darwin_listxattr(const char *path, char *output, std::size_t size, int options) {
    if (!validate("list", 0, options, false, false)) return -1;
    return list(output, size, [&](char *buffer, std::size_t length) {
        return options & nofollow ? llistxattr(path, buffer, length) : listxattr(path, buffer, length);
    });
}
ssize_t darwin_flistxattr(int, char *, std::size_t, int) DARWIN(flistxattr);
ssize_t darwin_flistxattr(int fd, char *output, std::size_t size, int options) {
    if (!validate("list", 0, options, false, true)) return -1;
    return list(output, size, [&](char *buffer, std::size_t length) { return flistxattr(fd, buffer, length); });
}
}

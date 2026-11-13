#include "internal.h"
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <pwd.h>
#include <vector>

namespace {
/* Darwin adds password-change time, class and expiration to Linux's
 * passwd record; even the home-directory field is at a different offset. */
struct darwin_passwd {
    char *name, *password;
    std::uint32_t uid, gid;
    std::int64_t change;
    char *class_, *gecos, *directory, *shell;
    std::int64_t expire;
};
static_assert(sizeof(darwin_passwd) == 72 && offsetof(darwin_passwd, directory) == 48);

template <typename Lookup>
int lookup(darwin_passwd *output, char *buffer, std::size_t size, darwin_passwd **result, Lookup get) {
    if (!result) return mm_darwin_errno(EINVAL);
    *result = nullptr;
    if (!output || !buffer) return mm_darwin_errno(EINVAL);
    passwd native{};
    passwd *found = nullptr;
    const int error = get(&native, buffer, size, &found);
    if (error) return mm_darwin_errno(error);
    if (!found) return 0;
    /* The empty class uses an existing string's terminator, so all string
     * pointers in a reentrant record remain in the caller's buffer. */
    char *empty = native.pw_name + std::strlen(native.pw_name);
    *output = {native.pw_name, native.pw_passwd, native.pw_uid, native.pw_gid, 0,
               empty, native.pw_gecos, native.pw_dir, native.pw_shell, 0};
    *result = output;
    return 0;
}

struct lookup_storage {
    darwin_passwd record{};
    std::vector<char> buffer = std::vector<char>(1024);
};
thread_local lookup_storage storage;

template <typename Lookup>
darwin_passwd *stored_lookup(Lookup get) {
    for (;;) {
        darwin_passwd *result = nullptr;
        int native_error = 0;
        const int error = lookup(&storage.record, storage.buffer.data(), storage.buffer.size(), &result,
            [&](passwd *p, char *b, std::size_t n, passwd **r) { native_error = get(p, b, n, r); return native_error; });
        if (error == mm_darwin_errno(ERANGE) && storage.buffer.size() < 1024 * 1024) {
            storage.buffer.resize(storage.buffer.size() * 2);
            continue;
        }
        if (error) errno = native_error ? native_error : EINVAL;
        return error ? nullptr : result;
    }
}
}

extern "C" {
int darwin_getpwuid_r(unsigned, darwin_passwd *, char *, std::size_t, darwin_passwd **) DARWIN(getpwuid_r);
int darwin_getpwuid_r(unsigned uid, darwin_passwd *output, char *buffer, std::size_t size, darwin_passwd **result) {
    return lookup(output, buffer, size, result,
        [uid](passwd *p, char *b, std::size_t n, passwd **r) { return getpwuid_r(uid, p, b, n, r); });
}
int darwin_getpwnam_r(const char *, darwin_passwd *, char *, std::size_t, darwin_passwd **) DARWIN(getpwnam_r);
int darwin_getpwnam_r(const char *name, darwin_passwd *output, char *buffer, std::size_t size, darwin_passwd **result) {
    if (!name) { if (result) *result = nullptr; return mm_darwin_errno(EINVAL); }
    return lookup(output, buffer, size, result,
        [name](passwd *p, char *b, std::size_t n, passwd **r) { return getpwnam_r(name, p, b, n, r); });
}
darwin_passwd *darwin_getpwuid(unsigned) DARWIN(getpwuid);
darwin_passwd *darwin_getpwuid(unsigned uid) {
    return stored_lookup([uid](passwd *p, char *b, std::size_t n, passwd **r) { return getpwuid_r(uid, p, b, n, r); });
}
darwin_passwd *darwin_getpwnam(const char *) DARWIN(getpwnam);
darwin_passwd *darwin_getpwnam(const char *name) {
    if (!name) { errno = EINVAL; return nullptr; }
    return stored_lookup([name](passwd *p, char *b, std::size_t n, passwd **r) { return getpwnam_r(name, p, b, n, r); });
}
darwin_passwd *darwin_getpwent() DARWIN(getpwent);
darwin_passwd *darwin_getpwent() {
    return stored_lookup([](passwd *p, char *b, std::size_t n, passwd **r) {
        const int error = getpwent_r(p, b, n, r);
        return error == ENOENT ? 0 : error;
    });
}
}

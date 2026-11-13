#include "internal.h"
#include <cerrno>
#include <cstdlib>

extern char **environ;
extern "C" {
/* glibc also exports a weak _environ alias. Protected visibility keeps our
 * internal references on this Darwin storage instead of preempting to libc. */
__attribute__((visibility("protected"))) char **darwin_environ __asm__("_environ") = nullptr;

char *darwin_getenv(const char *) DARWIN(getenv);
char *darwin_getenv(const char *name) { return mm_host_getenv(mm_libsystem_api(), name); }

int darwin_setenv(const char *, const char *, int) DARWIN(setenv);
int darwin_setenv(const char *name, const char *value, int overwrite) {
    const auto *api=mm_libsystem_api();
    if (!api || api->version < 6) return ::setenv(name, value, overwrite);
    const int error=api->environment_set(name, value, overwrite);
    if (error) { errno=error; return -1; } return 0;
}
int darwin_unsetenv(const char *) DARWIN(unsetenv);
int darwin_unsetenv(const char *name) {
    const auto *api=mm_libsystem_api();
    if (!api || api->version < 6) return ::unsetenv(name);
    const int error=api->environment_unset(name);
    if (error) { errno=error; return -1; } return 0;
}
int darwin_putenv(char *) DARWIN(putenv);
int darwin_putenv(char *entry) {
    const auto *api=mm_libsystem_api();
    if (!api || api->version < 6) return ::putenv(entry);
    const int error=api->environment_put(entry);
    if (error) { errno=error; return -1; } return 0;
}
int darwin_clearenv() DARWIN(clearenv);
int darwin_clearenv() {
    const auto *api=mm_libsystem_api();
    if (!api || api->version < 6) return ::clearenv();
    *api->environment()=nullptr; return 0;
}
char ***darwin_NSGetEnviron() DARWIN(_NSGetEnviron);
char ***darwin_NSGetEnviron() {
    const auto *api=mm_libsystem_api();
    return api && api->version >= 6 ? api->environment() : &darwin_environ;
}
}
void mm_libsystem_environment_init(const mm_host_api *api) {
    darwin_environ=api ? api->envp : environ;
    if (api && api->version >= 6) api->bind_environment(&darwin_environ);
}

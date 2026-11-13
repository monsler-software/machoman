#include <pwd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "passwd: %s\n", message); abort(); }
}
static int in_buffer(const char *p, const char *buffer, size_t size) {
    return (uintptr_t)p >= (uintptr_t)buffer && (uintptr_t)p < (uintptr_t)buffer + size;
}
int main(void) {
    struct passwd record, *result = NULL;
    char buffer[65536];
    check(getpwuid_r(getuid(), &record, buffer, sizeof buffer, &result) == 0 && result == &record,
          "lookup current uid");
    check(record.pw_uid == getuid() && record.pw_name[0] && record.pw_dir[0] == '/', "identity and home");
    check(record.pw_change == 0 && record.pw_expire == 0 && !record.pw_class[0], "Darwin extra fields");
    check(in_buffer(record.pw_name, buffer, sizeof buffer) && in_buffer(record.pw_passwd, buffer, sizeof buffer) &&
          in_buffer(record.pw_class, buffer, sizeof buffer) && in_buffer(record.pw_gecos, buffer, sizeof buffer) &&
          in_buffer(record.pw_dir, buffer, sizeof buffer) && in_buffer(record.pw_shell, buffer, sizeof buffer),
          "all reentrant strings belong to caller buffer");
    char *name = strdup(record.pw_name), *home = strdup(record.pw_dir), *shell = strdup(record.pw_shell);
    struct passwd *plain = getpwuid(getuid());
    check(plain && !strcmp(plain->pw_name, name) && !strcmp(plain->pw_dir, home) &&
          !strcmp(plain->pw_shell, shell), "ordinary lookup has Darwin layout");
    plain = getpwnam(name);
    check(plain && plain->pw_uid == getuid() && !strcmp(plain->pw_dir, home), "lookup by name");
    check(getpwnam_r(name, &record, buffer, sizeof buffer, &result) == 0 && result == &record &&
          !strcmp(record.pw_dir, home), "reentrant lookup by name");
    result = &record;
    check(getpwuid_r(getuid(), &record, buffer, 1, &result) == ERANGE && !result, "small buffer error");
    check(getpwnam_r("machoman-user-that-does-not-exist-192637", &record, buffer, sizeof buffer, &result) == 0 &&
          !result, "missing name");
    check(getpwuid_r((uid_t)-1, &record, buffer, sizeof buffer, &result) == 0 && !result, "missing uid");
    setpwent();
    plain = getpwent();
    check(plain && plain->pw_name[0] && plain->pw_dir[0] == '/' && plain->pw_class[0] == 0, "enumeration layout");
    endpwent();
    free(name); free(home); free(shell);
    puts("Darwin passwd layout, caller buffers, user lookup and enumeration passed");
    return 0;
}

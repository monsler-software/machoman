#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>

_Static_assert(sizeof(struct utsname) == 1280, "SDK uname size");
_Static_assert(offsetof(struct utsname, release) == 512, "SDK release offset");
static void check(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "Darwin uname: %s\n", message); exit(1); }
}
int main(void) {
    struct { unsigned char before[32]; struct utsname info; unsigned char after[32]; } buffer;
    memset(&buffer, 0xa5, sizeof buffer);
    check(uname(&buffer.info) == 0, "query system information");
    for (unsigned i = 0; i < 32; ++i)
        check(buffer.before[i] == 0xa5 && buffer.after[i] == 0xa5, "only SDK-sized output written");
    check(!strcmp(buffer.info.sysname, "Darwin") && buffer.info.nodename[0] && buffer.info.version[0], "system fields");
    check(strchr(buffer.info.release, '.') != NULL, "Darwin kernel release is a version string");
#if defined(__aarch64__)
    check(!strcmp(buffer.info.machine, "arm64"), "arm64 guest machine");
#else
    check(!strcmp(buffer.info.machine, "x86_64"), "Intel guest machine");
#endif
    char release[256], machine[256]; size_t size = sizeof release;
    check(sysctlbyname("kern.osrelease", release, &size, NULL, 0) == 0 &&
          !strcmp(release, buffer.info.release), "uname and sysctl kernel release agree");
    size = sizeof machine;
    check(sysctlbyname("hw.machine", machine, &size, NULL, 0) == 0 &&
          !strcmp(machine, buffer.info.machine), "uname and sysctl machine agree");
    check(uname(NULL) == -1 && errno == EFAULT, "invalid output pointer");
    puts("Darwin uname SDK layout, kernel release, machine identity and bounds passed");
    return 0;
}

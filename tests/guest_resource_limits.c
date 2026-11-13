#include <sys/resource.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void check(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "resource limits: %s\n", message); exit(1); }
}
int main(void) {
    struct rlimit files, locked, after, cpu;
    check(getrlimit(RLIMIT_CPU, &cpu) == 0 && cpu.rlim_cur <= RLIM_INFINITY &&
          cpu.rlim_max <= RLIM_INFINITY, "Darwin infinity representation");
    check(getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur >= 32,
          "read descriptor limit");
    check(getrlimit(RLIMIT_MEMLOCK, &locked) == 0, "read locked memory limit");
    struct rlimit reduced = files;
    reduced.rlim_cur = 32;
    check(setrlimit(RLIMIT_NOFILE, &reduced) == 0, "lower descriptor soft limit");
    int descriptors[64], count = 0;
    while (count < 64) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) break;
        descriptors[count++] = fd;
    }
    const int open_error = errno;
    for (int i = 0; i < count; ++i) close(descriptors[i]);
    check(setrlimit(RLIMIT_NOFILE, &files) == 0, "restore descriptor limit");
    check(count > 0 && count < 32 && open_error == EMFILE,
          "descriptor limit enforced by host open");
    check(getrlimit(RLIMIT_MEMLOCK, &after) == 0 && after.rlim_cur == locked.rlim_cur &&
          after.rlim_max == locked.rlim_max, "descriptor changes preserve locked memory limit");
    errno = 0;
    check(getrlimit(99, &after) == -1 && errno == EINVAL, "invalid resource rejected");
    check(getrlimit(RLIMIT_NOFILE, NULL) == -1 && errno == EFAULT,
          "invalid output pointer rejected");
    puts("Darwin resource numbers, infinity, descriptor enforcement and memory limit isolation passed");
    return 0;
}

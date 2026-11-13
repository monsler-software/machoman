#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "proc info: %s\n", message); exit(1); }
}
static int contains(struct proc_fdinfo *fds, int count, int fd, unsigned type) {
    for (int i = 0; i < count; ++i) if (fds[i].proc_fd == fd && fds[i].proc_fdtype == type) return 1;
    return 0;
}
int main(void) {
    int file = open("/dev/null", O_RDONLY), pipes[2], sockets[2];
    check(file >= 0 && pipe(pipes) == 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "open descriptors");
    int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, NULL, 0);
    check(bytes >= 5 * (int)sizeof(struct proc_fdinfo), "size query");
    struct proc_fdinfo fds[4096];
    bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, fds, sizeof fds);
    check(bytes > 0 && bytes % sizeof(struct proc_fdinfo) == 0, "SDK record layout");
    int count = bytes / sizeof(struct proc_fdinfo);
    check(contains(fds, count, file, PROX_FDTYPE_VNODE), "file type");
    check(contains(fds, count, pipes[0], PROX_FDTYPE_PIPE) && contains(fds, count, pipes[1], PROX_FDTYPE_PIPE), "pipe type");
    check(contains(fds, count, sockets[0], PROX_FDTYPE_SOCKET) && contains(fds, count, sockets[1], PROX_FDTYPE_SOCKET), "socket type");
    unsigned char truncated[sizeof(struct proc_fdinfo) + 1];
    memset(truncated, 0xa5, sizeof truncated);
    check(proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, truncated, sizeof truncated) == sizeof(struct proc_fdinfo) &&
          truncated[sizeof(struct proc_fdinfo)] == 0xa5, "truncate to whole records without overwrite");
    close(file); close(pipes[0]); close(pipes[1]); close(sockets[0]); close(sockets[1]);
    bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, fds, sizeof fds);
    check(bytes > 0 && !contains(fds, bytes / sizeof(struct proc_fdinfo), file, PROX_FDTYPE_VNODE), "closed descriptor removed");
    errno = 0;
    check(proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, (void *)1, sizeof fds) == 0 && errno == EFAULT, "bad output pointer");
    errno = 0;
    check(proc_pidinfo(getpid(), 999, 0, fds, sizeof fds) == 0 && errno == ENOTSUP, "unsupported flavor fails");
    puts("Darwin proc_pidinfo descriptor enumeration, types and bounds passed");
    return 0;
}

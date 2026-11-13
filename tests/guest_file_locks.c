#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

_Static_assert(sizeof(struct flock) == 24 && offsetof(struct flock, l_type) == 20, "SDK flock layout");
static void check(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "file locks: %s (errno=%d)\n", message, errno); _exit(1); }
}
static struct flock region(short type) {
    return (struct flock){.l_start = 17, .l_len = 23, .l_type = type, .l_whence = SEEK_SET};
}
int main(void) {
    char path[] = "/tmp/machoman-file-locks-XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "temporary file");
    struct flock lock = region(F_WRLCK);
    check(fcntl(fd, F_SETLK, &lock) == 0, "parent write lock");
    int ready[2], done[2];
    check(pipe(ready) == 0 && pipe(done) == 0, "handshake pipes");
    const pid_t parent = getpid();
    pid_t child = fork();
    check(child >= 0, "fork");
    if (!child) {
        close(ready[0]); close(done[0]);
        struct flock query = region(F_WRLCK);
        check(fcntl(fd, F_GETLK, &query) == 0, "query conflicting lock");
        check(query.l_type == F_WRLCK && query.l_pid == parent && query.l_start == 17 && query.l_len == 23,
              "translated lock type, owner and range");
        lock = region(F_RDLCK);
        check(fcntl(fd, F_SETLK, &lock) == -1 && (errno == EAGAIN || errno == EACCES), "nonblocking conflict");
        query = region(F_WRLCK); query.l_start = 50;
        check(fcntl(fd, F_GETLK, &query) == 0 && query.l_type == F_UNLCK, "nonoverlapping range");
        check(write(ready[1], "r", 1) == 1, "ready");
        lock = region(F_WRLCK);
        check(fcntl(fd, F_SETLKW, &lock) == 0, "blocking lock after parent unlock");
        check(write(done[1], "d", 1) == 1, "acquired");
        _exit(0);
    }
    close(ready[1]); close(done[1]);
    char byte;
    check(read(ready[0], &byte, 1) == 1, "child verified conflict");
    lock = region(F_UNLCK);
    check(fcntl(fd, F_SETLK, &lock) == 0, "parent unlock");
    check(read(done[0], &byte, 1) == 1, "child acquired after unlock");
    int status;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child completion");
    int other = open(path, O_RDWR);
    check(other >= 0, "independent open description");
    lock = region(F_WRLCK);
    check(fcntl(fd, F_OFD_SETLK, &lock) == 0, "OFD write lock");
    struct flock query = region(F_WRLCK);
    check(fcntl(other, F_OFD_GETLK, &query) == 0 && query.l_type == F_WRLCK && query.l_pid == -1,
          "OFD lock has no process owner");
    check(fcntl(other, F_OFD_SETLK, &lock) == -1 && (errno == EAGAIN || errno == EACCES), "OFD conflict");
    lock = region(F_UNLCK);
    check(fcntl(fd, F_OFD_SETLKW, &lock) == 0, "OFD unlock");
    check(fcntl(other, F_OFD_SETLKW, &lock) == 0, "OFD blocking command");
    lock = region(99);
    check(fcntl(fd, F_SETLK, &lock) == -1 && errno == EINVAL, "invalid lock type");
    check(fcntl(fd, F_GETLK, NULL) == -1 && errno == EFAULT, "null lock pointer");
    close(other); close(fd); unlink(path); close(ready[0]); close(done[0]);
    puts("SDK fcntl record and OFD locks, conflicts and cross-process wakeup passed");
    return 0;
}

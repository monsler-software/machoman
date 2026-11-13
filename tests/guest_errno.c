#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "errno: %s\n", message); abort(); }
}
static void *worker(void *argument) {
    check(errno == 0, "new thread errno starts at zero");
    errno = EAGAIN;
    check(errno == EAGAIN, "thread-local guest assignment");
    return argument;
}
int main(void) {
    errno = EDOM;
    int *saved_pointer = &errno;
    pthread_t thread;
    check(pthread_create(&thread, NULL, worker, NULL) == 0, "create thread");
    check(errno == EDOM && *saved_pointer == EDOM, "pthread_create preserves caller errno");
    check(pthread_join(thread, NULL) == 0 && errno == EDOM, "thread isolation and join");
    *saved_pointer = 0;
    check(close(-1) == -1 && *saved_pointer == EBADF, "cached pointer sees API error");
    char path[500];
    memset(path, 'x', sizeof path - 1); path[sizeof path - 1] = 0;
    errno = 0;
    check(open(path, O_RDONLY) == -1 && errno == ENAMETOOLONG, "Linux-to-Darwin error number");
    *saved_pointer = 0;
    check(strtol("9999999999999999999999999999999999999", NULL, 10) != 0 &&
          errno == ERANGE, "ordinary libc error captured");
    errno = EAGAIN;
    check(strtol("123", NULL, 10) == 123 && errno == EAGAIN, "guest assignment retained by successful libc call");
    puts("Guest errno assignments, cached pointers, native libc, number conversion and thread isolation passed");
    return 0;
}

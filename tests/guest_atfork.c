#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int state;
static pthread_t identity;
static uintptr_t native_identity;
static _Thread_local int local_value = 17;
static uintptr_t gs_identity(void) {
#if defined(__x86_64__)
    uintptr_t result;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(result));
    return result;
#else
    return 0;
#endif
}
static void require(int condition, const char *what) {
    if (!condition) { fprintf(stderr, "atfork failed: %s\n", what); _exit(1); }
}
static void append(int value) {
    require(pthread_equal(pthread_self(), identity) && gs_identity() == native_identity, "callback thread identity");
    require(getpid() > 0 && local_value == 17, "nested host call and live TLS");
    state = state * 10 + value;
}
static void prepare_a(void) { append(1); }
static void prepare_b(void) { append(2); }
static void parent_a(void) { append(3); }
static void parent_b(void) { append(4); }
static void child_a(void) { append(5); }
static void child_b(void) { append(6); }
int main(void) {
    identity = pthread_self();
    native_identity = gs_identity();
    errno = EDOM;
    require(pthread_atfork(prepare_a, parent_a, child_a) == 0, "register first handlers");
    require(pthread_atfork(NULL, NULL, NULL) == 0, "register null handlers");
    require(pthread_atfork(prepare_b, parent_b, child_b) == 0 && errno == EDOM, "registration preserves errno");
    int pipe_fds[2];
    require(pipe(pipe_fds) == 0, "child report pipe");
    const pid_t parent = getpid();
    const pid_t pid = fork();
    require(pid >= 0, "fork");
    if (!pid) {
        require(state == 2156 && getpid() != parent, "reverse prepare and forward child callbacks");
        close(pipe_fds[0]);
        require(write(pipe_fds[1], &state, sizeof state) == sizeof state, "child result write");
        _exit(0);
    }
    require(state == 2134 && getpid() == parent, "reverse prepare and forward parent callbacks");
    close(pipe_fds[1]);
    int child_state = 0, status = 0;
    require(read(pipe_fds[0], &child_state, sizeof child_state) == sizeof child_state && child_state == 2156,
            "independent fork callback effects");
    require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child completes");
    close(pipe_fds[0]);
    puts("SDK atfork guest callbacks, ordering, TLS identity and parent/child isolation passed");
    return 0;
}

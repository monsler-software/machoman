#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

struct shared { _Atomic unsigned stage; int parent_pid, child_pid, value; };
static int private_value;
static void check(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "guest processes: %s (errno %d)\n", message, errno); exit(1); }
}
static void await(struct shared *region, unsigned stage) {
    for (unsigned i = 0; i < 2000; ++i) {
        if (atomic_load_explicit(&region->stage, memory_order_acquire) == stage) return;
        usleep(1000);
    }
    check(0, "shared-memory synchronization deadline");
}
static int child(const char *name) {
    check(private_value == 0, "child has its own private globals");
    check(getenv("MM_PROCESS_TEST") && !strcmp(getenv("MM_PROCESS_TEST"), "child"), "explicit child environment");
    if (getenv("MM_PROCESS_EXPECT_CLOSED"))
        check(fcntl(199, F_GETFD) == -1 && errno == EBADF, "CLOEXEC_DEFAULT excludes unmentioned descriptors");
    sigset_t mask;
    check(sigprocmask(SIG_SETMASK, NULL, &mask) == 0 && sigismember(&mask, SIGUSR1), "guest spawn signal mask");
    int fd = shm_open(name, O_RDWR, 0);
    check(fd >= 0, "child opens shared name");
    struct shared *region = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(region != MAP_FAILED, "child shared mapping"); close(fd);
    check(region->parent_pid != getpid() && region->value == 41, "distinct process and parent data");
    region->child_pid = getpid(); region->value = 43;
    atomic_store_explicit(&region->stage, 1, memory_order_release);
    check(write(198, "R", 1) == 1, "spawn descriptor action");
    await(region, 2);
    region->value += 3;
    atomic_store_explicit(&region->stage, 3, memory_order_release);
    munmap(region, 4096); close(198);
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--child")) return child(argv[2]);
    char name[80]; snprintf(name, sizeof name, "/machoman-process-test-%d", getpid());
    shm_unlink(name);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    check(fd >= 0 && ftruncate(fd, 4096) == 0, "parent shared allocation");
    check(shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600) == -1 && errno == EEXIST, "exclusive shared name");
    struct shared *region = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(region != MAP_FAILED, "parent shared mapping"); close(fd);
    memset(region, 0, sizeof *region); region->parent_pid = getpid(); region->value = 41;
    private_value = 37;
    int pipes[2]; check(pipe(pipes) == 0, "notification pipe");
    check(dup2(pipes[0], 199) == 199, "unmentioned descriptor for close-by-default test");
    posix_spawn_file_actions_t actions;
    check(posix_spawn_file_actions_init(&actions) == 0, "initialize spawn actions");
    check(posix_spawn_file_actions_addinherit_np(&actions, STDOUT_FILENO) == 0 &&
          posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO) == 0, "explicit standard stream inheritance");
    check(posix_spawn_file_actions_adddup2(&actions, pipes[1], 198) == 0, "duplicate pipe to child");
    check(posix_spawn_file_actions_addclose(&actions, pipes[0]) == 0, "close child read descriptor");
    check(posix_spawn_file_actions_addclose(&actions, pipes[1]) == 0, "close child original write descriptor");
    posix_spawnattr_t attr; check(posix_spawnattr_init(&attr) == 0, "initialize attributes");
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGUSR1);
    check(posix_spawnattr_setsigmask(&attr, &mask) == 0 && posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT) == 0, "configure child guest mask and descriptor defaults");
    char *args[] = {argc > 1 ? argv[1] : argv[0], "--child", name, NULL};
    char *environment[] = {"MM_PROCESS_TEST=child", argc > 2 ? NULL : "MM_PROCESS_EXPECT_CLOSED=1", NULL};
    pid_t pid = 0;
    if (argc > 2 && !strcmp(argv[2], "--fork-parent")) {
        sigset_t old;
        sigprocmask(SIG_SETMASK, &mask, &old);
        pid = fork();
        check(pid >= 0, "fork creates separate process");
        if (pid == 0) {
            check(private_value == 37, "fork copies private state"); private_value = 99;
            check(dup2(pipes[1], 198) == 198, "child descriptor setup");
            close(pipes[0]); close(pipes[1]);
            execve(args[0], args, environment);
            _exit(97);
        }
        sigprocmask(SIG_SETMASK, &old, NULL);
    } else check(posix_spawn(&pid, args[0], &actions, &attr, args, environment) == 0 && pid != getpid(), "spawn guest as separate Linux process");
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attr); close(pipes[1]); close(199);
    char ready;
    check(read(pipes[0], &ready, 1) == 1 && ready == 'R', "child notification"); close(pipes[0]);
    await(region, 1);
    check(region->child_pid == pid && region->value == 43 && private_value == 37, "parent sees shared writes and retains private state");
    check(shm_unlink(name) == 0, "unlink shared name");
    check(shm_open(name, O_RDWR, 0) == -1 && errno == ENOENT, "unlinked name is gone");
    atomic_store_explicit(&region->stage, 2, memory_order_release);
    int status;
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "guest process exit status");
    check(atomic_load(&region->stage) == 3 && region->value == 46, "shared mapping survives unlink");
    munmap(region, 4096);
    pid = 99;
    check(posix_spawn(&pid, "/machoman-missing-program", NULL, NULL, args, environment) == ENOENT && pid == 99, "exec error returned without a false child PID");
    puts("Separate guest processes, shared memory, file actions and lifetime passed");
    return 0;
}

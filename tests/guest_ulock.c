#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern int __ulock_wait(uint32_t, void *, uint64_t, uint32_t);
extern int __ulock_wait2(uint32_t, void *, uint64_t, uint64_t, uint64_t);
extern int __ulock_wake(uint32_t, void *, uint64_t);
extern uint64_t mach_absolute_time(void);
enum { NO_ERRNO = 0x01000000, DEADLINE = 0x02000000, WAKE_ALL = 0x100 };
static _Atomic uint32_t word = 7;
static _Atomic unsigned ready, finished;
static void check(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "ulock: %s\n", message); abort(); }
}
static void *worker(void *unused) {
    (void)unused;
    atomic_fetch_add(&ready, 1);
    const int saved_errno = errno;
    int r = __ulock_wait(1 | NO_ERRNO, &word, 7, 2000000);
    check(r >= 0 && errno == saved_errno, "wait/wake and NO_ERRNO preservation");
    atomic_fetch_add(&finished, 1);
    return NULL;
}
int main(void) {
    check(__ulock_wait(1 | NO_ERRNO, &word, 8, 1000) == 0, "changed value returns success");
    check(__ulock_wait(1 | NO_ERRNO, &word, 7, 1000) == -60, "microsecond timeout");
    check(__ulock_wait(1, &word, 7, 1000) == -1 && errno == 60, "Darwin timeout errno");
    check(__ulock_wait2(1 | NO_ERRNO, &word, 7, 1000000, 0) == -60, "nanosecond timeout");
    check(__ulock_wait2(1 | NO_ERRNO | DEADLINE, &word, 7, mach_absolute_time() - 1, 0) == -60, "absolute deadline");
    check(__ulock_wait(99 | NO_ERRNO, &word, 7, 1000) == -22, "invalid opcode");
    pthread_t threads[3];
    for (unsigned i = 0; i < 3; ++i) check(pthread_create(&threads[i], NULL, worker, NULL) == 0, "create waiter");
    while (atomic_load(&ready) != 3) usleep(1000);
    /* Retry if a thread announced itself before actually entering the kernel. */
    for (unsigned attempt = 0; atomic_load(&finished) != 3 && attempt < 1000; ++attempt) {
        int r = __ulock_wake(1 | NO_ERRNO | WAKE_ALL, &word, 0);
        check(r == 0 || r == -2, "wake all");
        usleep(1000);
    }
    for (unsigned i = 0; i < 3; ++i) check(pthread_join(threads[i], NULL) == 0, "join waiter");
    check(atomic_load(&finished) == 3, "all waiters resumed");
    puts("Darwin ulock compare, deadlines, errno and multithreaded wake passed");
    return 0;
}

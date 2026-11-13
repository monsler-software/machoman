#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern void _tlv_atexit(void (*)(void *), void *);
static _Thread_local int tls_value = 17;
static pthread_key_t repeated_key, deleted_key;
static atomic_int ready, proceed;
struct worker_state { pthread_t identity; uintptr_t gs; int tlv_calls, key_calls; };
static void require(int condition, const char *what) {
    if (!condition) { fprintf(stderr, "thread destructor failed: %s\n", what); exit(1); }
}
static uintptr_t gs_identity(void) {
#if defined(__x86_64__)
    uintptr_t result;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(result));
    return result;
#else
    return 0;
#endif
}
static void check_identity(struct worker_state *state) {
    require(pthread_equal(pthread_self(), state->identity), "destructor keeps guest pthread identity");
    require(gs_identity() == state->gs, "destructor keeps native Darwin GS");
    require(getpid() > 0, "host call nested in guest destructor");
}
static void second_tlv_destructor(void *argument) {
    struct worker_state *state = argument;
    check_identity(state);
    require(state->tlv_calls == 1 && tls_value == 42, "reverse TLV destructor order and live storage");
    ++state->tlv_calls;
    tls_value = 43;
}
static void first_tlv_destructor(void *argument) {
    struct worker_state *state = argument;
    check_identity(state);
    require(state->tlv_calls == 0 && tls_value == 41, "TLV destructor on returning thread");
    ++state->tlv_calls;
    tls_value = 42;
    _tlv_atexit(second_tlv_destructor, argument);
}
static void repeated_destructor(void *argument) {
    struct worker_state *state = argument;
    check_identity(state);
    require(tls_value == 43 && state->tlv_calls == 2, "TLS storage survives pthread key destructors");
    require(pthread_getspecific(repeated_key) == NULL, "clear key before callback");
    ++state->key_calls;
    /* Even an always re-armed key must stop after the SDK iteration limit. */
    require(pthread_setspecific(repeated_key, argument) == 0, "re-arm key from destructor");
}
static void forbidden_destructor(void *argument) {
    (void)argument;
    require(0, "deleted key never calls its destructor");
}
static void *worker(void *argument) {
    struct worker_state *state = argument;
    state->identity = pthread_self();
    state->gs = gs_identity();
    require(tls_value == 17, "thread-local initial value");
    tls_value = 41;
    _tlv_atexit(first_tlv_destructor, state);
    require(pthread_setspecific(repeated_key, state) == 0, "set key value");
    require(pthread_setspecific(deleted_key, state) == 0, "set soon-deleted key");
    atomic_store(&ready, 1);
    while (!atomic_load(&proceed)) usleep(1000);
    require(pthread_getspecific(deleted_key) == NULL, "reused key does not expose another generation's value");
    return (void *)123;
}
int main(void) {
    require(sizeof(pthread_key_t) == 8, "Darwin SDK key width");
    repeated_key = deleted_key = (pthread_key_t)-1;
    errno = EDOM;
    require(pthread_key_create(&repeated_key, repeated_destructor) == 0, "create repeated key");
    require(repeated_key < PTHREAD_KEYS_MAX && errno == EDOM, "full-width key write and preserved errno");
    require(pthread_key_create(&deleted_key, forbidden_destructor) == 0, "create deleted key");
    const pthread_key_t original = deleted_key;
    struct worker_state state = {0};
    pthread_t thread;
    require(pthread_create(&thread, NULL, worker, &state) == 0, "create worker");
    while (!atomic_load(&ready)) usleep(1000);
    require(pthread_key_delete(deleted_key) == 0, "delete key with another thread's value");
    require(pthread_key_create(&deleted_key, forbidden_destructor) == 0 && deleted_key == original, "reuse deleted key");
    atomic_store(&proceed, 1);
    void *result = NULL;
    require(pthread_join(thread, &result) == 0 && result == (void *)123, "join preserves worker result after destructors");
    require(state.key_calls == PTHREAD_DESTRUCTOR_ITERATIONS && state.tlv_calls == 2, "SDK destructor iteration limit");
    require(tls_value == 17 && pthread_getspecific(repeated_key) == NULL, "main thread TLS isolation");
    require(pthread_key_delete(repeated_key) == 0 && pthread_key_delete(deleted_key) == 0, "delete keys");
    require(pthread_key_delete(deleted_key) == EINVAL, "reject already deleted key");
    require(pthread_setspecific((pthread_key_t)-1, &state) == EINVAL, "reject invalid key");
    puts("SDK pthread key width, thread identity, native GS, TLV lifetime and destructor iterations passed");
    return 0;
}

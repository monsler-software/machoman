#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Thread_local int tls_value = 17;
static int initialized;
__attribute__((constructor)) static void initialize(void) { initialized = getpid() > 0; }
static void require(int value, const char *what) {
    if (!value) { fprintf(stderr, "native smoke failed: %s\n", what); exit(1); }
}
static int format(char *out, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int result = vsnprintf(out, 128, fmt, ap);
    va_end(ap); return result;
}
static int compare(const void *a, const void *b) {
    char text[128];
    snprintf(text, sizeof text, "%d", *(const int *)a);
    require(text[0] != 0, "nested guest-to-host callback");
    return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b);
}
static void *worker(void *arg) {
    require(tls_value == 17, "thread TLS initial value");
    tls_value = 29;
    require(getpid() > 0, "host call from guest thread");
    return arg;
}
__attribute__((naked)) static void *on_stack(void *top, void *(*function)(void *), void *argument) {
    __asm__ volatile(
        "push %r12\n"
        "push %r13\n"
        "mov %rsp, %r12\n"
        "mov %rsi, %r13\n"
        "mov %rdi, %rsp\n"
        "and $-16, %rsp\n"
        "mov %rdx, %rdi\n"
        "call *%r13\n"
        "mov %r12, %rsp\n"
        "pop %r13\n"
        "pop %r12\n"
        "ret\n");
}
static void *fiber(void *argument) {
    int values[] = {5, 1, 3};
    qsort(values, 3, sizeof(int), compare);
    require(values[0] == 1 && values[2] == 5, "host call and nested callback on fiber stack");
    char text[128];
    format(text, "fiber %d", 42);
    require(strcmp(text, "fiber 42") == 0, "varargs on fiber stack");
    return argument;
}
int main(void) {
    require(initialized, "static initializer");
    char text[128];
    snprintf(text, sizeof text, "%s %d %.2f %d %d %d %d", "native", 7, 2.5, 3, 4, 5, 6);
    require(strcmp(text, "native 7 2.50 3 4 5 6") == 0, "register and stack variadic arguments");
    format(text, "%s %.1f %d", "va_list", 3.5, 11);
    require(strcmp(text, "va_list 3.5 11") == 0, "Darwin x86_64 va_list");
    int values[] = {9, 2, 7, 1};
    qsort(values, 4, sizeof(int), compare);
    require(values[0] == 1 && values[3] == 9, "guest callback and callee-saved registers");
    require(dlsym(RTLD_DEFAULT, "getpid") != NULL, "dynamic symbol lookup");
    int (*pid_function)(void) = dlsym(RTLD_DEFAULT, "getpid");
    require(pid_function() == getpid(), "dynamic host function bridge");
    require(tls_value == 17, "main thread TLS");
    char *fiber_stack = malloc(64 * 1024);
    require(fiber_stack != NULL, "allocate fiber stack");
    require(on_stack(fiber_stack + 64 * 1024 - 64, fiber, (void *)456) == (void *)456, "fiber return");
    free(fiber_stack);
    tls_value = 23;
    pthread_t thread; void *result = NULL;
    errno = EDOM;
    require(pthread_create(&thread, NULL, worker, (void *)123) == 0, "pthread_create");
    require(errno == EDOM, "pthread_create preserves errno");
    require(pthread_join(thread, &result) == 0 && result == (void *)123, "pthread_join");
    require(tls_value == 23, "TLS isolation");
    puts("native x86_64 calls, callbacks, varargs, dlsym and TLS passed");
    return 0;
}

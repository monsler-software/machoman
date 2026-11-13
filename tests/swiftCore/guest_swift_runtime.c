/* This is an ABI probe, not a Swift-language/standard-library test. Clang
 * emits the real Swift destructor calling convention on both guest ISAs. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

struct object { const void *metadata; uint64_t counts; uint64_t payload; };
extern struct object *swift_allocObject(const void *, size_t, size_t);
extern struct object *swift_initStackObject(const void *, struct object *);
extern struct object *swift_initStaticObject(const void *, struct object *);
extern struct object *swift_retain(struct object *);
extern struct object *swift_retain_n(struct object *, uint32_t);
extern void swift_release(struct object *);
extern void swift_release_n(struct object *, uint32_t);
extern struct object *swift_tryRetain(struct object *);
extern size_t swift_retainCount(struct object *);
extern int swift_isDeallocating(struct object *);
extern int swift_isUniquelyReferenced_nonNull_native(const void *);
extern void swift_deallocObject(struct object *, size_t, size_t);
extern void swift_deallocUninitializedObject(struct object *, size_t, size_t);
extern void swift_verifyEndOfLifetime(struct object *);
extern void swift_once(intptr_t *, void (*)(void *), void *);
extern void swift_beginAccess(void *, void *, uintptr_t, const void *);
extern void swift_endAccess(void *);

static unsigned destroyed;
static void check(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static void __attribute__((swiftcall)) destroy(struct object * __attribute__((swift_context)) self) {
    check(self && self->payload == 42, "destructor received swiftself and object payload");
    check(swift_isDeallocating(self) && !swift_tryRetain(self), "destructor cannot resurrect self");
    ++destroyed;
    swift_deallocObject(self, sizeof(*self), 7);
}
static struct { const void *destroy; const void *witnesses; uintptr_t kind; } metadata = {(const void *)&destroy, 0, 0x400};

static intptr_t inner_token;
static unsigned initializer_calls;
static void inner_initialize(void *context) { ++initializer_calls; *(int *)context = 42; }
static void initialize(void *context) {
    ++initializer_calls;
    swift_once(&inner_token, inner_initialize, context);
}
static intptr_t token;
static int once_value;
static void *worker(void *argument) {
    struct object *value = argument;
    for (unsigned n = 0; n < 2000; ++n) { swift_retain(value); swift_release(value); }
    swift_once(&token, initialize, &once_value);
    check(once_value == 42, "guest once initializer published its write");
    return 0;
}
int main(void) {
    struct object *value = swift_allocObject(&metadata.kind, sizeof(*value), 63);
    value->payload = 42;
    check(((uintptr_t)value & 63) == 0 && value->counts == 3, "Swift header and allocation alignment");
    check(swift_isUniquelyReferenced_nonNull_native(value), "new object is unique");
    swift_retain_n(value, 3);
    check(swift_retainCount(value) == 4 && !swift_isUniquelyReferenced_nonNull_native(value), "batch retain and uniqueness");
    swift_release_n(value, 3);
    pthread_t threads[4];
    for (unsigned n = 0; n < 4; ++n) check(pthread_create(&threads[n], 0, worker, value) == 0, "create guest thread");
    for (unsigned n = 0; n < 4; ++n) check(pthread_join(threads[n], 0) == 0, "join guest thread");
    check(swift_retainCount(value) == 1 && initializer_calls == 2 && token == -1, "concurrent ARC and nested guest once");
    swift_release(value);
    check(destroyed == 1, "final release runs guest destructor exactly once");

    struct object stack = {0};
    swift_initStackObject(&metadata.kind, &stack); stack.payload = 42;
    swift_release(&stack); swift_verifyEndOfLifetime(&stack);
    check(destroyed == 2 && stack.payload == 42, "stack storage survives guest destruction");
    static struct { intptr_t token; struct object value; } outlined;
    swift_initStaticObject(&metadata.kind, &outlined.value);
    const uint64_t immortal = outlined.value.counts;
    swift_retain_n(&outlined.value, 10); swift_release_n(&outlined.value, 11);
    swift_initStaticObject(&metadata.kind, &outlined.value);
    check(outlined.value.counts == immortal && destroyed == 2, "static object is immortal");
    swift_deallocUninitializedObject(swift_allocObject(&metadata.kind, sizeof(stack), 7), sizeof(stack), 7);

    uintptr_t a[3] = {0}, b[3] = {0}, c[3] = {0};
    int other = 0;
    swift_beginAccess(&once_value, a, 0x20, 0);
    swift_beginAccess(&once_value, b, 0x20, 0);
    swift_beginAccess(&other, c, 0x21, 0);
    swift_endAccess(a); swift_endAccess(c); swift_endAccess(b);
    swift_beginAccess(&once_value, a, 0x21, 0); once_value = 43; swift_endAccess(a);
    check(once_value == 43, "exclusivity permits reads and subsequent mutation");
    puts("Swift heap ABI, guest swiftself destructors, once and exclusivity passed");
    return 0;
}

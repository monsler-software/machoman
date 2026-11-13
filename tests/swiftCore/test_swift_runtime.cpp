#include "internal.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <vector>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using object = swift_heap_object;
struct heap_metadata { std::uint64_t destructor, witnesses, kind; } metadata{0x1234, 0, 0x400};
thread_local mm_cpu_context context;
std::atomic<unsigned> destroyed = 0;
void (*dealloc)(object *, std::size_t, std::size_t);
object *(*try_retain)(object *);
int (*is_deallocating)(object *);
int (*is_unique)(const void *);
std::size_t (*retain_count)(object *);
void (*once)(std::intptr_t *, const void *, void *);

void check(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
template <class T> T symbol(void *library, const char *name) {
    auto function = reinterpret_cast<T>(dlsym(library, name));
    check(function != nullptr, name);
    return function;
}
mm_cpu_context *current_context() { return &context; }
bool is_guest(std::uint64_t) { return true; }
std::uint64_t call_guest(std::uint64_t function, const std::uint64_t *args, int count) {
    if (function == metadata.destructor) {
        check(count == 0, "destructor has no ordinary integer argument");
        auto *value = reinterpret_cast<object *>(context.native_mode ? context.native_x[MM_R13] : context.x[20]);
        check(value && value->metadata == &metadata.kind, "swiftself points to the object");
        check(is_deallocating(value) && !is_unique(value) && !try_retain(value), "final release prevents resurrection");
        check(retain_count(value) == 0, "deallocating object has no live strong references");
        destroyed.fetch_add(1);
        dealloc(value, sizeof(object), 7);
    } else {
        check(count == 1, "once forwards its context argument");
        reinterpret_cast<void (*)(void *)>(function)(reinterpret_cast<void *>(args[0]));
    }
    return 0;
}

template <class F> void fails(F action, const char *expected) {
    int output[2];
    check(pipe(output) == 0, "create diagnostic pipe");
    const auto child = fork();
    check(child >= 0, "fork failure test");
    if (!child) {
        close(output[0]);
        dup2(output[1], STDERR_FILENO);
        close(output[1]);
        struct rlimit no_core{0, 0};
        setrlimit(RLIMIT_CORE, &no_core);
        action();
        _exit(0);
    }
    close(output[1]);
    std::string diagnostic;
    char buffer[512];
    ssize_t size;
    while ((size = read(output[0], buffer, sizeof(buffer))) > 0) diagnostic.append(buffer, static_cast<std::size_t>(size));
    close(output[0]);
    int status = 0;
    check(waitpid(child, &status, 0) == child, "collect failure process");
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "invalid Swift operation aborts");
    check(diagnostic.find(expected) != std::string::npos, expected);
}
struct once_payload { std::atomic<unsigned> calls = 0; std::intptr_t inner = 0; unsigned published = 0; };
void inner_init(void *opaque) { ++static_cast<once_payload *>(opaque)->calls; }
void outer_init(void *opaque) {
    auto &value = *static_cast<once_payload *>(opaque);
    ++value.calls;
    once(&value.inner, reinterpret_cast<const void *>(&inner_init), opaque);
    value.published = 42;
}
}

int main(int argc, char **argv) {
    check(argc == 2, "shared library path provided");
    auto *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    check(library != nullptr, "load Swift core dylib");
    auto init = symbol<void (*)(const mm_host_api *)>(library, "machoman_dylib_init");
    mm_host_api api{};
    api.version = 6;
    api.current_context = current_context;
    api.is_guest_code = is_guest;
    api.call_guest = call_guest;
    init(&api);
    auto alloc = symbol<object *(*)(const void *, std::size_t, std::size_t)>(library, "_swift_allocObject");
    auto retain = symbol<object *(*)(object *)>(library, "_swift_retain");
    auto retain_n = symbol<object *(*)(object *, std::uint32_t)>(library, "_swift_retain_n");
    auto release = symbol<void (*)(object *)>(library, "_swift_release");
    auto release_n = symbol<void (*)(object *, std::uint32_t)>(library, "_swift_release_n");
    auto non_retain = symbol<object *(*)(object *)>(library, "_swift_nonatomic_retain");
    auto non_release = symbol<void (*)(object *)>(library, "_swift_nonatomic_release");
    dealloc = symbol<decltype(dealloc)>(library, "_swift_deallocObject");
    try_retain = symbol<decltype(try_retain)>(library, "_swift_tryRetain");
    is_deallocating = symbol<decltype(is_deallocating)>(library, "_swift_isDeallocating");
    is_unique = symbol<decltype(is_unique)>(library, "_swift_isUniquelyReferenced_nonNull_native");
    retain_count = symbol<decltype(retain_count)>(library, "_swift_retainCount");
    once = symbol<decltype(once)>(library, "_swift_once");
    auto stack_init = symbol<object *(*)(const void *, object *)>(library, "_swift_initStackObject");
    auto static_init = symbol<object *(*)(const void *, object *)>(library, "_swift_initStaticObject");
    auto verify_lifetime = symbol<void (*)(object *)>(library, "_swift_verifyEndOfLifetime");
    auto uninitialized_dealloc = symbol<decltype(dealloc)>(library, "_swift_deallocUninitializedObject");
    auto slow_alloc = symbol<void *(*)(std::size_t, std::size_t)>(library, "_swift_slowAlloc");
    auto slow_dealloc = symbol<void (*)(void *, std::size_t, std::size_t)>(library, "_swift_slowDealloc");
    auto begin = symbol<void (*)(void *, void *, std::uintptr_t, const void *)>(library, "_swift_beginAccess");
    auto end = symbol<void (*)(void *)>(library, "_swift_endAccess");
    auto *disabled = symbol<unsigned char *>(library, "__swift_disableExclusivityChecking");

    auto *value = alloc(&metadata.kind, 64, 63);
    check(!(reinterpret_cast<std::uintptr_t>(value) & 63), "object allocation honors over-alignment");
    check(value->refcounts == 3 && retain_count(value) == 1 && is_unique(value), "stable ABI header and initial uniqueness");
    check(retain(value) == value && value->refcounts == 0x200000003ULL, "retain sets the Swift strong-extra field");
    retain_n(value, 4);
    check(retain_count(value) == 6 && !is_unique(value), "batch retain affects uniqueness");
    release_n(value, 5);
    check(is_unique(value), "batch release restores uniqueness");
    non_retain(value); non_release(value);
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&] {
        for (unsigned n = 0; n < 10000; ++n) { retain(value); release(value); }
    });
    for (auto &worker : workers) worker.join();
    check(retain_count(value) == 1 && destroyed == 0, "concurrent retains keep the root reference alive");
    context.x[20] = 0xabc;
    release(value);
    check(destroyed == 1 && context.x[20] == 0xabc, "one destruction and caller swiftself preserved");
    context.native_mode = 1; context.native_x[MM_R13] = 0xdef;
    release(alloc(&metadata.kind, sizeof(object), 7));
    check(destroyed == 2 && context.native_x[MM_R13] == 0xdef, "native Darwin swiftself preserved");
    context.native_mode = 0;

    object stack{};
    check(stack_init(&metadata.kind, &stack) == &stack, "initialize stack object");
    retain(&stack); release(&stack); release(&stack);
    verify_lifetime(&stack);
    check(stack.refcounts == 0x100000003ULL && destroyed == 3, "stack destructor runs without freeing storage");
    struct { std::intptr_t token = 0; object value{}; } outlined;
    static_init(&metadata.kind, &outlined.value);
    const auto immortal_header = outlined.value.refcounts;
    retain_n(&outlined.value, 100); release_n(&outlined.value, 101);
    static_init(&metadata.kind, &outlined.value);
    check(outlined.token == -1 && outlined.value.refcounts == immortal_header && !is_unique(&outlined.value), "static initialization is immortal and idempotent");
    uninitialized_dealloc(alloc(&metadata.kind, 16, 7), 16, 7);
    check(destroyed == 3, "uninitialized storage does not run a destructor");
    check(!retain(nullptr) && !try_retain(nullptr) && !is_unique(nullptr), "null reference operations");
    release(nullptr); release_n(nullptr, 10);
    auto *raw = slow_alloc(0, static_cast<std::size_t>(-1));
    check(raw != nullptr, "zero-size raw allocation");
    slow_dealloc(raw, 0, static_cast<std::size_t>(-1));

    once_payload payload;
    std::intptr_t predicate = 0;
    workers.clear();
    for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&] {
        once(&predicate, reinterpret_cast<const void *>(&outer_init), &payload);
        check(payload.published == 42, "once publishes initializer writes to all callers");
    });
    for (auto &worker : workers) worker.join();
    check(payload.calls == 2 && predicate == -1, "nested once callbacks execute exactly once");

    alignas(void *) std::uintptr_t a[3]{}, b[3]{}, c[3]{};
    int first = 0, second = 0;
    begin(&first, a, 0x20, nullptr); begin(&first, b, 0x20, nullptr);
    begin(&second, c, 0x21, nullptr);
    end(a); end(c); end(b); // removal need not be LIFO
    begin(&first, a, 0x21, nullptr);
    std::thread independent([&] { alignas(void *) std::uintptr_t local[3]{}; begin(&first, local, 0x21, nullptr); end(local); });
    independent.join(); end(a);
    begin(&first, a, 0x21, nullptr);
    *disabled = 1; begin(&first, b, 0x21, nullptr); *disabled = 0; end(b); end(a);
    begin(&first, a, 1, nullptr); begin(&first, b, 0x21, nullptr); end(b); end(a);
    fails([&] { begin(&first, a, 0x20, nullptr); begin(&first, b, 0x21, nullptr); }, "modification requires exclusive access");
    fails([&] { begin(&first, a, 0x21, nullptr); begin(&first, b, 0, nullptr); }, "modification requires exclusive access");
    fails([&] { object escaped{}; stack_init(&metadata.kind, &escaped); verify_lifetime(&escaped); }, "stack object escaped");
    fails([&] { slow_alloc(1, 6); }, "invalid allocation alignment mask");
    fails([&] { auto *o = alloc(&metadata.kind, 16, 7); release_n(o, 2); }, "reference count underflow");
    fails([&] { auto *o = alloc(&metadata.kind, 16, 7); retain_n(o, 0xffffffffu); }, "reference count overflow");
    fails([&] { object side{&metadata.kind, 0xc000000000000001ULL}; retain(&side); }, "side tables are not implemented");
    check(!dlsym(library, "_swift_dynamicCast") && !dlsym(library, "machoman_resolve_symbol"), "unsupported Swift symbols stay unresolved");
    std::puts("Swift runtime lifetime, concurrency, once, exclusivity and failure diagnostics passed");
}

#include "internal.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
const mm_host_api *host_api = nullptr;

bool immortal(std::uint64_t bits) {
    return (bits & swift_slow_rc) && (bits & 0xffffffffu) == 0xffffffffu;
}
void require_inline(std::uint64_t bits) {
    if (bits & swift_slow_rc) mm_swift_fail("reference-count side tables are not implemented");
    if (!(bits & 1)) mm_swift_fail("Objective-C object lifetime is not implemented");
}
std::atomic_ref<std::uint64_t> counts(swift_heap_object *object) {
    return std::atomic_ref(object->refcounts);
}
void *allocate(std::size_t size, std::size_t alignment_mask) {
    // SIZE_MAX asks the slow allocator for its default malloc alignment.
    if (alignment_mask == std::numeric_limits<std::size_t>::max()) alignment_mask = alignof(std::max_align_t) - 1;
    const auto alignment = alignment_mask + 1;
    if (!alignment || (alignment & alignment_mask)) mm_swift_fail("invalid allocation alignment mask");
    void *memory = nullptr;
    if (posix_memalign(&memory, std::max(alignment, sizeof(void *)), std::max(size, std::size_t{1})))
        mm_swift_fail("allocation failed");
    return memory;
}
swift_heap_object *retain(swift_heap_object *object, std::uint32_t n, bool try_retain = false) {
    if (!object || !n) return object;
    auto rc = counts(object);
    auto old = rc.load(std::memory_order_relaxed);
    for (;;) {
        if (immortal(old)) return object;
        require_inline(old);
        if (old & swift_deiniting) {
            if (try_retain) return nullptr;
            mm_swift_fail("retain of a deallocating object");
        }
        const auto extra = (old & swift_strong_mask) >> 33;
        if (n > ((std::uint64_t{1} << 30) - 1) - extra) mm_swift_fail("strong reference count overflow");
        if (rc.compare_exchange_weak(old, old + n * swift_strong_unit, std::memory_order_relaxed)) return object;
    }
}
void destroy(swift_heap_object *object) {
    if (!object->metadata) mm_swift_fail("object has no heap metadata");
    const auto function = static_cast<const std::uint64_t *>(object->metadata)[-2];
    auto *ctx = host_api && host_api->current_context ? host_api->current_context() : nullptr;
    if (!function || !ctx || !host_api->call_guest || !host_api->is_guest_code || !host_api->is_guest_code(function))
        mm_swift_fail("heap destructor requires a guest Swift function");
    // SWIFT_CONTEXT/swiftself is x20 on arm64 and r13 on Darwin x86_64.
    // call_guest preserves these special registers across the nested run.
    auto &self = ctx->native_mode ? ctx->native_x[MM_R13] : ctx->x[20];
    const auto saved_self = self;
    self = reinterpret_cast<std::uintptr_t>(object);
    host_api->call_guest(function, nullptr, 0);
    self = saved_self;
    // A deallocating destructor calls swift_deallocObject itself. Never touch
    // the object after the callback: it may already have been freed.
}
void release(swift_heap_object *object, std::uint32_t n) {
    if (!object || !n) return;
    auto rc = counts(object);
    auto old = rc.load(std::memory_order_relaxed);
    for (;;) {
        if (immortal(old)) return;
        require_inline(old);
        if (old & swift_deiniting) mm_swift_fail("release of a deallocating object");
        const auto count = ((old & swift_strong_mask) >> 33) + 1;
        if (n > count) mm_swift_fail("strong reference count underflow");
        const bool last = n == count;
        const auto next = last ? (old & ~swift_strong_mask) | swift_deiniting : old - n * swift_strong_unit;
        if (rc.compare_exchange_weak(old, next, std::memory_order_release, std::memory_order_relaxed)) {
            if (last) {
                std::atomic_thread_fence(std::memory_order_acquire);
                destroy(object);
            }
            return;
        }
    }
}
void set_deallocating(swift_heap_object *object) {
    if (!object) mm_swift_fail("null object in swift_setDeallocating");
    auto rc = counts(object);
    auto old = rc.load(std::memory_order_relaxed);
    for (;;) {
        if (immortal(old)) return;
        require_inline(old);
        if (old & (swift_strong_mask | swift_deiniting)) mm_swift_fail("deallocation requires a unique live object");
        if (rc.compare_exchange_weak(old, old | swift_deiniting, std::memory_order_relaxed)) return;
    }
}
void deallocate(swift_heap_object *object) {
    if (!object) mm_swift_fail("null object in swift_deallocObject");
    auto rc = counts(object);
    const auto bits = rc.load(std::memory_order_relaxed);
    require_inline(bits);
    if (!(bits & swift_deiniting) || (bits & swift_strong_mask)) mm_swift_fail("object deallocated before its final release");
    const auto unowned = (bits & 0xfffffffeu) >> 1;
    if (unowned == 1) std::free(object);
    else if (unowned == 2) rc.fetch_sub(2, std::memory_order_relaxed); // stack-promoted object; never free its storage
    else mm_swift_fail("unowned object lifetime is not implemented");
}
int unique(const void *pointer) {
    if (!pointer) return 0;
    const auto bits = counts(const_cast<swift_heap_object *>(static_cast<const swift_heap_object *>(pointer))).load(std::memory_order_relaxed);
    if (immortal(bits)) return 0;
    require_inline(bits);
    return !(bits & (swift_slow_rc | swift_deiniting | swift_strong_mask));
}
} // namespace

const mm_host_api *mm_swift_api() { return host_api; }
[[noreturn]] void mm_swift_fail(const char *message) {
    std::fprintf(stderr, "Swift runtime: %s\n", message);
    if (host_api && host_api->print_guest_backtrace) host_api->print_guest_backtrace();
    std::abort();
}

extern "C" {
SWIFT_EXPORT void machoman_dylib_init(const mm_host_api *api) { host_api = api; }

void *darwin_swift_slowAlloc(std::size_t, std::size_t) SWIFT_DARWIN(swift_slowAlloc);
void *darwin_swift_slowAlloc(std::size_t size, std::size_t mask) { return allocate(size, mask); }
void darwin_swift_slowDealloc(void *, std::size_t, std::size_t) SWIFT_DARWIN(swift_slowDealloc);
void darwin_swift_slowDealloc(void *memory, std::size_t, std::size_t) { std::free(memory); }

swift_heap_object *darwin_swift_allocObject(const void *, std::size_t, std::size_t) SWIFT_DARWIN(swift_allocObject);
swift_heap_object *darwin_swift_allocObject(const void *metadata, std::size_t size, std::size_t mask) {
    if (!metadata || size < sizeof(swift_heap_object)) mm_swift_fail("invalid heap object allocation");
    auto *object = static_cast<swift_heap_object *>(allocate(size, mask));
    object->metadata = metadata;
    object->refcounts = 3; // strong count 1, unowned count 1, pure Swift deallocation
    return object;
}
swift_heap_object *darwin_swift_initStackObject(const void *, swift_heap_object *) SWIFT_DARWIN(swift_initStackObject);
swift_heap_object *darwin_swift_initStackObject(const void *metadata, swift_heap_object *object) {
    if (!metadata || !object) mm_swift_fail("invalid stack object");
    object->metadata = metadata;
    object->refcounts = 5; // an extra unowned retain prevents freeing stack memory
    return object;
}
swift_heap_object *darwin_swift_initStaticObject(const void *, swift_heap_object *) SWIFT_DARWIN(swift_initStaticObject);
swift_heap_object *darwin_swift_initStaticObject(const void *metadata, swift_heap_object *object) {
    if (!metadata || !object) mm_swift_fail("invalid static object");
    struct context { const void *metadata; swift_heap_object *object; } init{metadata, object};
    mm_swift_once(reinterpret_cast<std::intptr_t *>(object) - 1, [](void *opaque) {
        auto &value = *static_cast<context *>(opaque);
        value.object->metadata = value.metadata;
        value.object->refcounts = swift_immortal;
    }, &init);
    return object;
}
void darwin_swift_verifyEndOfLifetime(swift_heap_object *) SWIFT_DARWIN(swift_verifyEndOfLifetime);
void darwin_swift_verifyEndOfLifetime(swift_heap_object *object) {
    if (!object) mm_swift_fail("null stack object");
    const auto bits = counts(object).load(std::memory_order_relaxed);
    require_inline(bits);
    if (!(bits & swift_deiniting) || (bits & swift_strong_mask)) mm_swift_fail("stack object escaped its lifetime");
}

#define RETAIN_ENTRY(name) \
    swift_heap_object *darwin_##name(swift_heap_object *) SWIFT_DARWIN(name); \
    swift_heap_object *darwin_##name(swift_heap_object *o) { return retain(o, 1); }
#define RETAIN_N_ENTRY(name) \
    swift_heap_object *darwin_##name(swift_heap_object *, std::uint32_t) SWIFT_DARWIN(name); \
    swift_heap_object *darwin_##name(swift_heap_object *o, std::uint32_t n) { return retain(o, n); }
#define RELEASE_ENTRY(name) \
    void darwin_##name(swift_heap_object *) SWIFT_DARWIN(name); \
    void darwin_##name(swift_heap_object *o) { release(o, 1); }
#define RELEASE_N_ENTRY(name) \
    void darwin_##name(swift_heap_object *, std::uint32_t) SWIFT_DARWIN(name); \
    void darwin_##name(swift_heap_object *o, std::uint32_t n) { release(o, n); }
RETAIN_ENTRY(swift_retain)
RETAIN_ENTRY(swift_nonatomic_retain)
RETAIN_N_ENTRY(swift_retain_n)
RETAIN_N_ENTRY(swift_nonatomic_retain_n)
RELEASE_ENTRY(swift_release)
RELEASE_ENTRY(swift_nonatomic_release)
RELEASE_N_ENTRY(swift_release_n)
RELEASE_N_ENTRY(swift_nonatomic_release_n)

swift_heap_object *darwin_swift_tryRetain(swift_heap_object *) SWIFT_DARWIN(swift_tryRetain);
swift_heap_object *darwin_swift_tryRetain(swift_heap_object *object) { return retain(object, 1, true); }
std::size_t darwin_swift_retainCount(swift_heap_object *) SWIFT_DARWIN(swift_retainCount);
std::size_t darwin_swift_retainCount(swift_heap_object *object) {
    if (!object) return 0;
    const auto bits = counts(object).load(std::memory_order_relaxed);
    if (immortal(bits)) return ((bits & swift_strong_mask) >> 33) + 1;
    require_inline(bits);
    return bits & swift_deiniting ? 0 : ((bits & swift_strong_mask) >> 33) + 1;
}
int darwin_swift_isDeallocating(swift_heap_object *) SWIFT_DARWIN(swift_isDeallocating);
int darwin_swift_isDeallocating(swift_heap_object *object) {
    if (!object) return 0;
    const auto bits = counts(object).load(std::memory_order_relaxed);
    if (immortal(bits)) return 0;
    require_inline(bits);
    return !!(bits & swift_deiniting);
}
void darwin_swift_setDeallocating(swift_heap_object *) SWIFT_DARWIN(swift_setDeallocating);
void darwin_swift_setDeallocating(swift_heap_object *object) { set_deallocating(object); }

#define UNIQUE_ENTRY(name) \
    int darwin_##name(const void *) SWIFT_DARWIN(name); \
    int darwin_##name(const void *object) { return unique(object); }
UNIQUE_ENTRY(swift_isUniquelyReferenced_native)
UNIQUE_ENTRY(swift_isUniquelyReferenced_nonNull_native)
UNIQUE_ENTRY(swift_isUniquelyReferencedNonObjC)
UNIQUE_ENTRY(swift_isUniquelyReferencedNonObjC_nonNull)

void darwin_swift_deallocObject(swift_heap_object *, std::size_t, std::size_t) SWIFT_DARWIN(swift_deallocObject);
void darwin_swift_deallocObject(swift_heap_object *object, std::size_t, std::size_t) { deallocate(object); }
void darwin_swift_deallocClassInstance(swift_heap_object *, std::size_t, std::size_t) SWIFT_DARWIN(swift_deallocClassInstance);
void darwin_swift_deallocClassInstance(swift_heap_object *object, std::size_t, std::size_t) { deallocate(object); }
void darwin_swift_deallocUninitializedObject(swift_heap_object *, std::size_t, std::size_t) SWIFT_DARWIN(swift_deallocUninitializedObject);
void darwin_swift_deallocUninitializedObject(swift_heap_object *object, std::size_t, std::size_t) {
    set_deallocating(object);
    deallocate(object);
}
}

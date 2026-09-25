/* Shared between the translation units of libSystem.B.dylib.so. */
#ifndef MACHOMAN_LIBSYSTEM_INTERNAL_H
#define MACHOMAN_LIBSYSTEM_INTERNAL_H

#include "machoman/runtime.h"

#include <cstdint>

/* Exports a definition under its Darwin symbol name (leading underscore). */
#define DARWIN(name) __attribute__((visibility("default"))) __asm__("_" #name)

/* The runtime's callbacks, set by machoman_dylib_init. */
const mm_host_api *mm_libsystem_api();

/* Calls `fn` with `n` integer arguments; guest function pointers (a
 * comparator handed to qsort, a callback handed to a dispatch API) go
 * through the runtime instead of being called as host code. */
std::uint64_t mm_call_callback(const void *fn, const std::uint64_t *args, int n);

/* Variadic arguments of the calling guest. Apple's arm64 ABI passes every
 * anonymous argument on the stack in 8-byte slots (16 for long double),
 * regardless of free registers, and a Darwin va_list is a plain pointer
 * into that area. So a host implementation of a variadic Darwin function
 * reads its "..." from the guest SP at call time, and a v*-variant reads
 * from the va_list it was handed. */
class mm_guest_va_list {
public:
    explicit mm_guest_va_list(std::uint64_t base) : p_(base) {}
    /* va_list at the guest's SP: what a "..." function sees. */
    static mm_guest_va_list at_call_site();

    std::uint64_t next_u64() { const std::uint64_t v = read(); p_ += 8; return v; }
    std::int64_t next_i64() { return static_cast<std::int64_t>(next_u64()); }
    std::int32_t next_i32() { return static_cast<std::int32_t>(next_u64()); }
    double next_double() { std::uint64_t bits = next_u64(); double d; __builtin_memcpy(&d, &bits, 8); return d; }
    /* Darwin arm64 long double is IEEE binary128 in a 16-byte aligned slot. */
    long double next_long_double() {
        p_ = (p_ + 15) & ~std::uint64_t{15};
        __float128 q; __builtin_memcpy(&q, reinterpret_cast<const void *>(p_), 16); p_ += 16;
        return static_cast<long double>(q);
    }
    template <typename T> T *next_ptr() { return reinterpret_cast<T *>(next_u64()); }
    std::uint64_t position() const { return p_; }

private:
    std::uint64_t read() const { std::uint64_t v; __builtin_memcpy(&v, reinterpret_cast<const void *>(p_), 8); return v; }
    std::uint64_t p_;
};

#endif

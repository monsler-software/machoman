/* Swift 5 stable, 64-bit heap ABI. Keep Swift functions in this dylib;
 * unsupported symbols must remain unresolved instead of being synthesised.
 * Layout references: swiftlang/swift's SwiftShims/RefCount.h, ABI/Metadata.h.
 * The metadata address point has the deallocating destructor at word -2
 * and its value-witness table at word -1. The destructor owns deallocation. */
#ifndef MACHOMAN_SWIFTCORE_INTERNAL_H
#define MACHOMAN_SWIFTCORE_INTERNAL_H

#include "machoman/runtime.h"
#include <cstddef>
#include <cstdint>

#define SWIFT_EXPORT __attribute__((visibility("default")))
#define SWIFT_DARWIN(name) SWIFT_EXPORT __asm__("_" #name)

struct swift_heap_object {
    const void *metadata;
    std::uint64_t refcounts;
};
static_assert(sizeof(swift_heap_object) == 16);

inline constexpr std::uint64_t swift_strong_unit = std::uint64_t{1} << 33;
inline constexpr std::uint64_t swift_strong_mask = ((std::uint64_t{1} << 30) - 1) << 33;
inline constexpr std::uint64_t swift_deiniting = std::uint64_t{1} << 32;
inline constexpr std::uint64_t swift_slow_rc = std::uint64_t{1} << 63;
inline constexpr std::uint64_t swift_immortal = swift_slow_rc | (2 * swift_strong_unit) | 0xffffffffu;

const mm_host_api *mm_swift_api();
[[noreturn]] void mm_swift_fail(const char *message);
void mm_swift_once(std::intptr_t *predicate, void (*function)(void *), void *context);

#endif

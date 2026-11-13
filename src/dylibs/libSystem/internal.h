/* Shared between the translation units of libSystem.B.dylib.so. */
#ifndef MACHOMAN_LIBSYSTEM_INTERNAL_H
#define MACHOMAN_LIBSYSTEM_INTERNAL_H

#include "machoman/runtime.h"
#include "machoman/guest_args.h"
#include "machoman/host_environment.h"

#include <cstdint>

/* Exports a definition under its Darwin symbol name (leading underscore). */
#define DARWIN(name) __attribute__((visibility("default"))) __asm__("_" #name)

/* The runtime's callbacks, set by machoman_dylib_init. */
const mm_host_api *mm_libsystem_api();
void mm_libsystem_environment_init(const mm_host_api *api);
int mm_darwin_errno(int error);
int mm_darwin_open_flags(int flags);
int mm_mach_watch_port(std::uint32_t port, std::uint64_t *token);
void mm_mach_unwatch_port(std::uint32_t port, std::uint64_t token);
int mm_mach_watch_ready(std::uint32_t port, std::uint64_t *count, int *alive);
std::uint32_t mm_mach_thread_port(std::uint64_t thread);

/* Calls `fn` with `n` integer arguments; guest function pointers (a
 * comparator handed to qsort, a callback handed to a dispatch API) go
 * through the runtime instead of being called as host code. */
std::uint64_t mm_call_callback(const void *fn, const std::uint64_t *args, int n);

/* Add the current libSystem guest context to the shared Darwin ABI reader. */
class mm_guest_va_list : public mm_darwin_va_list {
public:
    explicit mm_guest_va_list(std::uint64_t base)
        : mm_darwin_va_list(base, context()) {}
    static mm_guest_va_list at_call_site(unsigned named_integer_args = 0) {
        return mm_guest_va_list(mm_darwin_va_list::at_call_site(context(), named_integer_args));
    }
private:
    explicit mm_guest_va_list(const mm_darwin_va_list &args) : mm_darwin_va_list(args) {}
    static const mm_cpu_context *context() {
        const auto *api = mm_libsystem_api();
        return api && api->current_context ? api->current_context() : nullptr;
    }
};

#endif

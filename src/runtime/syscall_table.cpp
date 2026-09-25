#include "machoman/runtime.h"

namespace {

/* Darwin BSD syscall numbers (from xnu's syscalls.master), just the small
 * subset a CLI tool like ripgrep actually needs at the boundary: process
 * lifecycle, fd I/O, and mmap for reading input. Every handler is null for
 * now — wiring one up means translating Darwin's argument/struct ABI to the
 * equivalent Win32 (CreateFileW/ReadFile/...) or Linux syscall, which is
 * the next slice of runtime work once the translator can produce a full
 * program rather than isolated instructions. */
constexpr mm_syscall_entry g_table[] = {
    { 1,   "exit",            nullptr },
    { 3,   "read",            nullptr },
    { 4,   "write",           nullptr },
    { 5,   "open",            nullptr },
    { 6,   "close",           nullptr },
    { 197, "mmap",            nullptr },
    { 199, "lseek",           nullptr },
    { 202, "sysctl",          nullptr }, /* used heavily by libSystem for cpu/page info */
    { 338, "getcwd",          nullptr },
    { 339, "getdirentries64", nullptr },
};

} // namespace

std::span<const mm_syscall_entry> mm_syscall_table() {
    return g_table;
}

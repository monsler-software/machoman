#include "mach_ipc.h"
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <sys/syscall.h>

namespace {
struct region { std::uint64_t size, offset; mm_ipc::file_descriptor backing; int maximum = 7; };
std::mutex regions_mutex;
std::map<std::uint64_t, region> regions;
std::uint64_t page_size() { static const auto page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE)); return page; }
std::uint64_t rounded(std::uint64_t size) { const auto p = page_size(); return (size + p - 1) & ~(p - 1); }
int current_protection(std::uint64_t address, std::uint64_t size, std::uint64_t *inode = nullptr) {
    std::ifstream maps("/proc/self/maps"); std::string line;
    while (std::getline(maps, line)) {
        unsigned long long begin, end, offset, ino; unsigned major, minor; char modes[5];
        if (std::sscanf(line.c_str(), "%llx-%llx %4s %llx %x:%x %llu", &begin, &end, modes, &offset, &major, &minor, &ino) != 7) continue;
        if (begin <= address && address < end && size <= end - address) {
            if (inode) *inode = ino;
            return (modes[0] == 'r' ? 1 : 0) | (modes[1] == 'w' ? 2 : 0) | (modes[2] == 'x' ? 4 : 0);
        }
    }
    return -1;
}
void forget_regions(std::uint64_t address, std::uint64_t size) {
    for (auto it = regions.begin(); it != regions.end();) {
        if (it->first < address + size && address < it->first + it->second.size) it = regions.erase(it);
        else ++it;
    }
}
int map_memory(mm_mach_name task, std::uint64_t *address, std::uint64_t size, std::uint64_t mask,
               int flags, mm_mach_name object, std::uint64_t offset, int copy, int protection, int maximum, int) {
    if (task != 1) return 16;
    if (static_cast<unsigned>(flags) & ~(0xff000000u | 1u | 8u | 16u | 0x4000u)) return 46;
    if (!address || !size || size > UINT64_MAX - page_size() || (mask & (mask + 1)) ||
        (protection & ~7) || (maximum & ~7) || (protection & ~maximum)) return 4;
    size = rounded(size);
    mm_ipc::response entry;
    int fd = -1;
    std::uint64_t file_offset = 0;
    if (object) {
        mm_ipc::request request; request.op = mm_ipc::operation::memory_get; request.args[0] = object;
        entry = mm_mach_exchange(request);
        if (entry.status || entry.descriptors.size() != 1) return entry.status ? static_cast<int>(entry.status) : 4;
        if (offset > entry.args[0] || size > entry.args[0] - offset || (maximum & ~entry.args[1])) return 2;
        file_offset = entry.args[2] + offset;
        if (file_offset & (page_size() - 1)) return 4;
        fd = entry.descriptors[0].get();
    } else if (offset) return 4;
    const bool anywhere = flags & 1, overwrite = flags & 0x4000;
    if (!anywhere && (*address & (mask | (page_size() - 1)))) return 4;
    int native_flags = object ? (copy ? MAP_PRIVATE : MAP_SHARED) : MAP_PRIVATE | MAP_ANONYMOUS;
    void *want = anywhere ? nullptr : reinterpret_cast<void *>(*address);
    void *reservation = nullptr;
    std::uint64_t reserved_size = 0;
    if (anywhere && mask >= page_size()) {
        if (mask > UINT64_MAX - size) return 3;
        reserved_size = size + mask + 1;
        reservation = mmap(nullptr, reserved_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (reservation == MAP_FAILED) return 3;
        const auto start = reinterpret_cast<std::uintptr_t>(reservation);
        want = reinterpret_cast<void *>((start + mask) & ~mask);
        native_flags |= MAP_FIXED;
    } else if (!anywhere) native_flags |= overwrite ? MAP_FIXED : MAP_FIXED_NOREPLACE;
    void *mapped = mmap(want, size, protection, native_flags, fd, static_cast<off_t>(file_offset));
    if (reservation) {
        const auto begin = reinterpret_cast<std::uintptr_t>(reservation), aligned = reinterpret_cast<std::uintptr_t>(want);
        if (mapped == MAP_FAILED) munmap(reservation, reserved_size);
        else {
            if (aligned > begin) munmap(reservation, aligned - begin);
            if (begin + reserved_size > aligned + size) munmap(reinterpret_cast<void *>(aligned + size), begin + reserved_size - aligned - size);
        }
    }
    if (mapped == MAP_FAILED) return errno == ENOMEM || errno == EEXIST ? 3 : 1;
    *address = reinterpret_cast<std::uintptr_t>(mapped);
    std::lock_guard lock(regions_mutex);
    forget_regions(*address, size);
    if (object && !copy) regions.emplace(*address, region{size, file_offset, entry.descriptors[0], maximum});
    return 0;
}
int map_guest_memory(mm_mach_name task, std::uint64_t *address, std::uint64_t size, std::uint64_t mask,
                     int flags, mm_mach_name object, std::uint64_t offset, int copy,
                     int protection, int maximum, int inheritance) {
    const auto *api = mm_libsystem_api();
    const auto *ctx = api && api->current_context ? api->current_context() : nullptr;
    if (ctx && !ctx->native_mode) {
        mm_darwin_arm64_stack_args args(ctx);
        protection = args.next<int>();
        maximum = args.next<int>();
        inheritance = args.next<int>();
    }
    return map_memory(task, address, size, mask, flags, object, offset, copy, protection, maximum, inheritance);
}
}

extern "C" {
extern std::uint64_t darwin_vm_page_size __asm__("_vm_page_size");
extern std::uint64_t darwin_vm_page_mask __asm__("_vm_page_mask");
extern unsigned darwin_vm_page_shift __asm__("_vm_page_shift");
__attribute__((visibility("default"))) std::uint64_t darwin_vm_page_size = page_size();
__attribute__((visibility("default"))) std::uint64_t darwin_vm_page_mask = page_size() - 1;
__attribute__((visibility("default"))) unsigned darwin_vm_page_shift = [] {
    unsigned shift = 0;
    for (auto size = page_size(); size > 1; size >>= 1) ++shift;
    return shift;
}();
int darwin_mach_vm_map(mm_mach_name, std::uint64_t *, std::uint64_t, std::uint64_t, int, mm_mach_name, std::uint64_t, int, int, int, int) DARWIN(mach_vm_map);
int darwin_mach_vm_map(mm_mach_name task, std::uint64_t *address, std::uint64_t size, std::uint64_t mask, int flags,
                      mm_mach_name object, std::uint64_t offset, int copy, int protection, int maximum, int inheritance) {
    return map_guest_memory(task, address, size, mask, flags, object, offset, copy, protection, maximum, inheritance);
}
#define MAP_ALIAS(symbol) \
int darwin_##symbol(mm_mach_name, std::uint64_t *, std::uint64_t, std::uint64_t, int, mm_mach_name, std::uint64_t, int, int, int, int) DARWIN(symbol); \
int darwin_##symbol(mm_mach_name task, std::uint64_t *address, std::uint64_t size, std::uint64_t mask, int flags, mm_mach_name object, std::uint64_t offset, int copy, int protection, int maximum, int inheritance) { \
    return map_guest_memory(task, address, size, mask, flags, object, offset, copy, protection, maximum, inheritance); }
MAP_ALIAS(vm_map)
MAP_ALIAS(vm_map_64)
#undef MAP_ALIAS
#define ALLOCATE(symbol) \
int darwin_##symbol(mm_mach_name, std::uint64_t *, std::uint64_t, int) DARWIN(symbol); \
int darwin_##symbol(mm_mach_name task, std::uint64_t *address, std::uint64_t size, int flags) { \
    return map_memory(task, address, size, 0, flags, 0, 0, 0, 3, 7, 1); }
ALLOCATE(vm_allocate)
ALLOCATE(mach_vm_allocate)
#undef ALLOCATE
#define DEALLOCATE(symbol) \
int darwin_##symbol(mm_mach_name, std::uint64_t, std::uint64_t) DARWIN(symbol); \
int darwin_##symbol(mm_mach_name task, std::uint64_t address, std::uint64_t size) { \
    if (task != 1) return 16; \
    if (size > UINT64_MAX - address) return 4; \
    const int result = mm_mach_deallocate_memory(address, size); \
    if (!result) { std::lock_guard lock(regions_mutex); forget_regions(address, size); } \
    return result; }
DEALLOCATE(vm_deallocate)
DEALLOCATE(mach_vm_deallocate)
#undef DEALLOCATE
int darwin_mach_make_memory_entry_64(mm_mach_name, std::uint64_t *, std::uint64_t, int, mm_mach_name *, mm_mach_name) DARWIN(mach_make_memory_entry_64);
int darwin_mach_make_memory_entry_64(mm_mach_name task, std::uint64_t *size, std::uint64_t address, int permission, mm_mach_name *out, mm_mach_name parent) {
    if (task != 1) return 16;
    if (!size || !out || !*size || *size > UINT64_MAX - address || *size > UINT64_MAX - page_size()) return 4;
    const bool named = permission & 0x20000, private_copy = permission & 0x200000;
    const unsigned allowed = 7u | 0x20000u | 0x80000u | 0x200000u | 0x400000u;
    if (static_cast<unsigned>(permission) & ~allowed) return 46;
    const auto begin = named ? 0 : address & ~(page_size() - 1);
    const auto length = rounded(*size + (named ? 0 : address - begin));
    mm_ipc::file_descriptor backing;
    std::uint64_t offset = 0;
    std::lock_guard lock(regions_mutex);
    if (parent) {
        if (named) return 4;
        mm_ipc::request request; request.op = mm_ipc::operation::memory_get; request.args[0] = parent;
        const auto entry = mm_mach_exchange(request);
        if (entry.status || entry.descriptors.size() != 1) return static_cast<int>(entry.status ? entry.status : 4);
        if (begin > entry.args[0] || length > entry.args[0] - begin || (permission & 7u & ~entry.args[1])) return 2;
        backing = entry.descriptors[0]; offset = entry.args[2] + begin;
    } else {
        const int protection = named ? 3 : current_protection(begin, length);
        if (!named && (protection < 0 || !(protection & 1))) return 1;
        if (!named && !private_copy) {
            auto it = regions.upper_bound(begin);
            if (it != regions.begin()) {
                --it;
                std::uint64_t inode = 0; struct stat info{};
                if (begin >= it->first && begin - it->first <= it->second.size && length <= it->second.size - (begin - it->first) &&
                    current_protection(begin, length, &inode) >= 0 && fstat(it->second.backing.get(), &info) == 0 && inode == info.st_ino) {
                    backing = it->second.backing; offset = it->second.offset + begin - it->first;
                }
            }
        }
        if (backing.get() < 0) {
            backing = mm_ipc::file_descriptor(static_cast<int>(syscall(SYS_memfd_create, "machoman-memory-entry", MFD_CLOEXEC)));
            if (backing.get() < 0 || ftruncate(backing.get(), static_cast<off_t>(length)) < 0) return 6;
            if (!named) {
                void *temporary = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, backing.get(), 0);
                if (temporary == MAP_FAILED) return 6;
                const bool copied = mm_mach_copy(temporary, reinterpret_cast<const void *>(begin), length);
                munmap(temporary, length);
                if (!copied) return 1;
                if (!private_copy) {
                    void *mapped = mmap(reinterpret_cast<void *>(begin), length, protection, MAP_SHARED | MAP_FIXED, backing.get(), 0);
                    if (mapped == MAP_FAILED) return 6;
                    forget_regions(begin, length); regions.emplace(begin, region{length, 0, backing});
                }
            }
        }
    }
    mm_ipc::request request; request.op = mm_ipc::operation::memory_create;
    request.args = {1, length, static_cast<unsigned>(permission) & 7u, offset}; request.descriptors.push_back(backing);
    const auto result = mm_mach_exchange(request);
    if (!result.status) { *out = static_cast<mm_mach_name>(result.args[0]); *size = length; }
    return static_cast<int>(result.status);
}
}

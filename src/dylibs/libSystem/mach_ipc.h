#pragma once
#include "internal.h"
#include "machoman/ipc.h"
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

using mm_mach_name = std::uint32_t;
struct mm_mach_header { std::uint32_t bits, size, remote, local, voucher; std::int32_t id; };
static_assert(sizeof(mm_mach_header) == 24);
inline mm_ipc::response mm_mach_exchange(const mm_ipc::request &request) {
    const auto *api = mm_libsystem_api();
    return mm_ipc::exchange(request, mm_host_venv(api), mm_host_launcher(api));
}
inline mm_ipc::response mm_mach_control(mm_ipc::operation op, mm_mach_name task, mm_mach_name name = 0,
                                      std::uint64_t a = 0, std::uint64_t b = 0) {
    mm_ipc::request request; request.op = op; request.args = {task, name, a, b}; return mm_mach_exchange(request);
}
inline bool mm_mach_copy(void *destination, const void *source, std::size_t size) {
    const auto *api = mm_libsystem_api();
    return api && api->version >= 5 && api->copy_guest_memory && api->copy_guest_memory(destination, source, size) == 0;
}
template <typename T> T mm_mach_load(const std::vector<std::uint8_t> &data, std::size_t offset) {
    T value; std::memcpy(&value, data.data() + offset, sizeof value); return value;
}
template <typename T> void mm_mach_store(std::vector<std::uint8_t> &data, std::size_t offset, T value) {
    std::memcpy(data.data() + offset, &value, sizeof value);
}
inline void *mm_mach_allocate_memory(const void *source, std::size_t size) {
    if (!size) return nullptr;
    void *mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) return nullptr;
    if (source) std::memcpy(mapped, source, size);
    return mapped;
}
inline int mm_mach_deallocate_memory(std::uint64_t address, std::uint64_t size) {
    if (!size) return 0;
    const auto page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE)), begin = address & ~(page - 1);
    if (size > UINT64_MAX - address || address + size > UINT64_MAX - (page - 1)) return 4;
    const auto end = (address + size + page - 1) & ~(page - 1);
    return munmap(reinterpret_cast<void *>(begin), end - begin) == 0 ? 0 : 1;
}
template <typename Visit> bool mm_mach_descriptors(std::vector<std::uint8_t> &bytes, Visit visit) {
    if (bytes.size() < 24) return false;
    if (!(mm_mach_load<std::uint32_t>(bytes, 0) & 0x80000000u)) return true;
    const auto size = mm_mach_load<std::uint32_t>(bytes, 4);
    if (size < 28 || size > bytes.size()) return false;
    const auto count = mm_mach_load<std::uint32_t>(bytes, 24);
    std::size_t offset = 28;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (offset + 12 > size) return false;
        const auto type = bytes[offset + 11];
        if (type > 4) return false;
        const auto length = type == 0 ? 12u : 16u;
        if (offset + length > size || !visit(offset, type)) return false;
        offset += length;
    }
    return true;
}
extern "C" {
int darwin_mach_port_allocate(mm_mach_name, unsigned, mm_mach_name *) DARWIN(mach_port_allocate);
int darwin_mach_port_destroy(mm_mach_name, mm_mach_name) DARWIN(mach_port_destroy);
int darwin_mach_port_deallocate(mm_mach_name, mm_mach_name) DARWIN(mach_port_deallocate);
int darwin_mach_port_mod_refs(mm_mach_name, mm_mach_name, unsigned, int) DARWIN(mach_port_mod_refs);
int darwin_mach_port_extract_right(mm_mach_name, mm_mach_name, unsigned, mm_mach_name *, unsigned *) DARWIN(mach_port_extract_right);
int darwin_mach_port_insert_right(mm_mach_name, mm_mach_name, mm_mach_name, unsigned) DARWIN(mach_port_insert_right);
int darwin_mach_port_unguard(mm_mach_name, mm_mach_name, std::uint64_t) DARWIN(mach_port_unguard);
}

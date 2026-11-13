#include "mach_ipc.h"

namespace {
constexpr std::size_t max_message = 64 * 1024 * 1024;
int send_message(mm_mach_header *msg, unsigned options, unsigned size, unsigned timeout) {
    if (size < 24) return 0x10000008;
    if (size > max_message) return 0x1000000e;
    mm_ipc::request request; request.op = mm_ipc::operation::send; request.args = {options, timeout};
    request.data.resize(size);
    if (!mm_mach_copy(request.data.data(), msg, size)) return 0x10000002;
    /* mach_msg's send_size is authoritative. MIG request stubs often leave
     * msgh_size uninitialized; XNU fills it in the copied-in kernel header. */
    mm_mach_store(request.data, 4, size);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> deallocated;
    unsigned error = 0x1000000f;
    const bool valid = mm_mach_descriptors(request.data, [&](std::size_t offset, unsigned type) {
        if (type == 0 || type == 4) return true;
        const auto address = mm_mach_load<std::uint64_t>(request.data, offset);
        const auto count = mm_mach_load<std::uint32_t>(request.data, offset + 12);
        const auto bytes = static_cast<std::uint64_t>(count) * (type == 2 ? 4 : 1);
        if (bytes > max_message - request.data.size()) { error = 0x1000000e; return false; }
        if (request.data[offset + 8] && bytes) deallocated.emplace_back(address, bytes);
        const auto begin = request.data.size(); request.data.resize(begin + bytes);
        if (bytes && !mm_mach_copy(request.data.data() + begin, reinterpret_cast<const void *>(address), bytes)) {
            error = 0x1000000c; return false;
        }
        mm_mach_store<std::uint64_t>(request.data, offset, begin);
        return true;
    });
    if (!valid) return static_cast<int>(error);
    const auto result = mm_mach_exchange(request);
    if (!result.status) for (const auto &[address, bytes] : deallocated) mm_mach_deallocate_memory(address, bytes);
    return static_cast<int>(result.status);
}
int receive_message(mm_mach_header *msg, unsigned options, unsigned size, mm_mach_name name, unsigned timeout) {
    mm_mach_header original;
    if (size < sizeof original || !mm_mach_copy(&original, msg, sizeof original) || !mm_mach_copy(msg, &original, sizeof original)) return 0x10004008;
    mm_ipc::request request; request.op = mm_ipc::operation::receive;
    request.args = {name, options, timeout, size, reinterpret_cast<std::uintptr_t>(msg)};
    auto result = mm_mach_exchange(request);
    if (result.status == 0x10004004) {
        original.size = static_cast<std::uint32_t>(result.args[0]); original.local = static_cast<mm_mach_name>(result.args[1]);
        mm_mach_copy(msg, &original, sizeof original);
    }
    if (result.status) return static_cast<int>(result.status);
    std::vector<std::pair<void *, std::size_t>> mappings;
    const bool valid = mm_mach_descriptors(result.data, [&](std::size_t offset, unsigned type) {
        if (type == 0 || type == 4) return true;
        const auto begin = mm_mach_load<std::uint64_t>(result.data, offset);
        const auto count = mm_mach_load<std::uint32_t>(result.data, offset + 12);
        const auto bytes = static_cast<std::uint64_t>(count) * (type == 2 ? 4 : 1);
        if (begin > result.data.size() || bytes > result.data.size() - begin) return false;
        void *mapped = mm_mach_allocate_memory(result.data.data() + begin, bytes);
        if (bytes && !mapped) return false;
        if (mapped) mappings.emplace_back(mapped, bytes);
        mm_mach_store<std::uint64_t>(result.data, offset, reinterpret_cast<std::uintptr_t>(mapped));
        return true;
    });
    if (!valid || result.data.size() < result.args[0] || !mm_mach_copy(msg, result.data.data(), result.args[0])) {
        for (const auto &[mapped, bytes] : mappings) munmap(mapped, bytes);
        return 0x1000400c;
    }
    /* Darwin's format-0 trailer is packed to four bytes, including the
     * context field. OOL payload offsets remain in the transport only. */
    const auto elements = (options >> 24) & 15u;
    std::vector<std::uint8_t> trailer(elements >= 4 ? 60 : elements >= 3 ? 52 : elements >= 2 ? 20 : elements >= 1 ? 12 : 8);
    mm_mach_store(trailer, 0, 0u); mm_mach_store(trailer, 4, static_cast<unsigned>(trailer.size()));
    if (elements >= 1) mm_mach_store(trailer, 8, static_cast<unsigned>(result.args[2]));
    if (elements >= 2) { mm_mach_store(trailer, 12, static_cast<unsigned>(result.args[5])); mm_mach_store(trailer, 16, 0u); }
    if (elements >= 3) {
        mm_mach_store(trailer, 20, static_cast<unsigned>(result.args[5]));
        mm_mach_store(trailer, 24, static_cast<unsigned>(result.args[5]));
        mm_mach_store(trailer, 32, static_cast<unsigned>(result.args[5]));
        mm_mach_store(trailer, 40, static_cast<unsigned>(result.args[4]));
    }
    if (elements >= 4) mm_mach_store(trailer, 52, result.args[3]);
    const auto offset = (result.args[0] + 3) & ~std::uint64_t{3};
    if (offset <= size && trailer.size() <= size - offset)
        mm_mach_copy(reinterpret_cast<std::uint8_t *>(msg) + offset, trailer.data(), trailer.size());
    return 0;
}
thread_local mm_mach_name reply_port = 0;
}

extern "C" {
/* MIG generated requests copy this exported data record into their body. */
extern const std::uint8_t darwin_NDR_record[8] __asm__("_NDR_record");
__attribute__((visibility("default"))) const std::uint8_t darwin_NDR_record[8] = {0, 0, 0, 0, 1, 0, 0, 0};
int darwin_mach_msg(mm_mach_header *, unsigned, unsigned, unsigned, mm_mach_name, unsigned, mm_mach_name) DARWIN(mach_msg);
int darwin_mach_msg(mm_mach_header *msg, unsigned options, unsigned send_size, unsigned receive_size, mm_mach_name name, unsigned timeout, mm_mach_name) {
    if (options & 1) { const int error = send_message(msg, options, send_size, timeout); if (error) return error; }
    return options & 2 ? receive_message(msg, options, receive_size, name, timeout) : 0;
}
int darwin_mach_msg_overwrite(mm_mach_header *, unsigned, unsigned, unsigned, mm_mach_name, unsigned, mm_mach_name, mm_mach_header *, unsigned) DARWIN(mach_msg_overwrite);
int darwin_mach_msg_overwrite(mm_mach_header *msg, unsigned options, unsigned send_size, unsigned receive_size, mm_mach_name name, unsigned timeout, mm_mach_name, mm_mach_header *receive_msg, unsigned) {
    if (options & 1) { const int error = send_message(msg, options, send_size, timeout); if (error) return error; }
    return options & 2 ? receive_message(receive_msg ? receive_msg : msg, options, receive_size, name, timeout) : 0;
}
int darwin_mach_msg_send(mm_mach_header *) DARWIN(mach_msg_send);
int darwin_mach_msg_send(mm_mach_header *msg) { mm_mach_header h; return mm_mach_copy(&h, msg, sizeof h) ? send_message(msg, 1, h.size, 0) : 0x10000002; }
int darwin_mach_msg_receive(mm_mach_header *) DARWIN(mach_msg_receive);
int darwin_mach_msg_receive(mm_mach_header *msg) { mm_mach_header h; return mm_mach_copy(&h, msg, sizeof h) ? receive_message(msg, 2, h.size, h.local, 0) : 0x10004008; }
mm_mach_name darwin_mach_reply_port() DARWIN(mach_reply_port);
mm_mach_name darwin_mach_reply_port() { mm_mach_name name = 0; darwin_mach_port_allocate(1, 1, &name); return name; }
mm_mach_name darwin_mig_get_reply_port() DARWIN(mig_get_reply_port);
mm_mach_name darwin_mig_get_reply_port() { if (!reply_port) reply_port = darwin_mach_reply_port(); return reply_port; }
void darwin_mig_dealloc_reply_port(mm_mach_name) DARWIN(mig_dealloc_reply_port);
void darwin_mig_dealloc_reply_port(mm_mach_name name) { darwin_mach_port_destroy(1, name); if (reply_port == name) reply_port = 0; }
void darwin_mig_put_reply_port(mm_mach_name) DARWIN(mig_put_reply_port);
void darwin_mig_put_reply_port(mm_mach_name) {}
void darwin_mach_msg_destroy(mm_mach_header *) DARWIN(mach_msg_destroy);
void darwin_mach_msg_destroy(mm_mach_header *msg) {
    mm_mach_header h; if (!mm_mach_copy(&h, msg, sizeof h) || h.size < 24 || h.size > max_message) return;
    auto release = [&](mm_mach_name name, unsigned disposition) {
        if (!name || name == ~0u) return;
        if (disposition == 16) darwin_mach_port_mod_refs(1, name, 1, -1);
        else if (disposition == 17 || disposition == 18) darwin_mach_port_deallocate(1, name);
        else if (disposition == 20) {
            if (!darwin_mach_port_insert_right(1, name, name, 20)) darwin_mach_port_deallocate(1, name);
        } else if (disposition == 21) {
            mm_mach_name once; unsigned kind;
            if (!darwin_mach_port_extract_right(1, name, 21, &once, &kind)) darwin_mach_port_deallocate(1, once);
        }
    };
    release(h.remote, h.bits & 31u);
    release(h.voucher, (h.bits >> 16) & 31u);
    std::vector<std::uint8_t> bytes(h.size); if (!mm_mach_copy(bytes.data(), msg, bytes.size())) return;
    mm_mach_descriptors(bytes, [&](std::size_t offset, unsigned type) {
        if (type == 0) release(mm_mach_load<mm_mach_name>(bytes, offset), bytes[offset + 10]);
        else if (type == 4) {
            const auto name = mm_mach_load<mm_mach_name>(bytes, offset + 12);
            if (!(mm_mach_load<std::uint16_t>(bytes, offset + 8) & 2u))
                darwin_mach_port_unguard(1, name, mm_mach_load<std::uint64_t>(bytes, offset));
            release(name, bytes[offset + 10]);
        }
        else if (type == 3) return true; /* Volatile OOL memory is caller managed. */
        else {
            const auto address = mm_mach_load<std::uint64_t>(bytes, offset);
            const auto count = mm_mach_load<std::uint32_t>(bytes, offset + 12);
            const auto size = static_cast<std::uint64_t>(count) * (type == 2 ? 4 : 1);
            if (type == 2) for (std::uint32_t i = 0; i < count; ++i) {
                mm_mach_name name;
                if (!mm_mach_copy(&name, reinterpret_cast<const void *>(address + i * 4), 4)) break;
                release(name, bytes[offset + 10]);
            }
            if (bytes[offset + 8]) mm_mach_deallocate_memory(address, size);
        }
        return true;
    });
}
}

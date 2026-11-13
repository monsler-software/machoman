#include "mach_ipc.h"
#include <cerrno>
#include <fcntl.h>
using port_name = mm_mach_name;
using mm_ipc::operation;
int mm_mach_watch_port(port_name name, std::uint64_t *token) {
    mm_ipc::request r; r.op = operation::watch; r.args[0] = name;
    const auto result = mm_mach_exchange(r);
    if (result.status || result.descriptors.size() != 1) { errno = EINVAL; return -1; }
    const int fd = fcntl(result.descriptors[0].get(), F_DUPFD_CLOEXEC, 3);
    if (fd >= 0) *token = result.args[0];
    else mm_mach_unwatch_port(name, result.args[0]);
    return fd;
}
void mm_mach_unwatch_port(port_name name, std::uint64_t token) {
    mm_ipc::request r; r.op = operation::unwatch; r.args = {name, token}; mm_mach_exchange(r);
}
int mm_mach_watch_ready(port_name name, std::uint64_t *count, int *alive) {
    mm_ipc::request r; r.op = operation::watch_status; r.args[0] = name;
    const auto result = mm_mach_exchange(r);
    if (result.status == 15 || result.status == 17) { *count = 0; *alive = 0; return 0; }
    if (result.status) return -1;
    *count = result.args[0]; *alive = result.args[1] != 0; return 0;
}
port_name mm_mach_thread_port(std::uint64_t thread) {
    mm_ipc::request r; r.op = operation::thread_self; r.args[0] = thread;
    const auto result = mm_mach_exchange(r);
    return result.status ? 0 : static_cast<port_name>(result.args[0]);
}
namespace {
int export_array(const std::vector<std::uint8_t> &data, port_name **out, unsigned *count) {
    if (!out || !count) return 4;
    auto *memory = static_cast<port_name *>(mm_mach_allocate_memory(data.data(), data.size()));
    if (!data.empty() && !memory) return 6;
    *out = memory; *count = static_cast<unsigned>(data.size() / 4); return 0;
}
}
extern "C" {
int darwin_mach_port_allocate(port_name task, unsigned kind, port_name *out) {
    if (!out) return 4;
    const auto r = mm_mach_control(operation::allocate, task, kind);
    if (!r.status) *out = static_cast<port_name>(r.args[0]);
    return static_cast<int>(r.status);
}
int darwin_mach_port_allocate_name(port_name, unsigned, port_name) DARWIN(mach_port_allocate_name);
int darwin_mach_port_allocate_name(port_name task, unsigned kind, port_name name) {
    return static_cast<int>(mm_mach_control(operation::allocate, task, kind, name).status);
}
int darwin_mach_port_construct(port_name, const void *, std::uint64_t, port_name *) DARWIN(mach_port_construct);
int darwin_mach_port_construct(port_name task, const void *options, std::uint64_t context, port_name *out) {
    struct port_options { unsigned flags, limit; std::uint64_t reserved[2]; } info;
    if (!out || !mm_mach_copy(&info, options, sizeof info)) return 4;
    mm_ipc::request request; request.op = operation::construct; request.args = {task, info.flags, info.limit, context};
    const auto result = mm_mach_exchange(request);
    if (!result.status) *out = static_cast<port_name>(result.args[0]);
    return static_cast<int>(result.status);
}
int darwin_mach_port_destruct(port_name, port_name, int, std::uint64_t) DARWIN(mach_port_destruct);
int darwin_mach_port_destruct(port_name task, port_name name, int delta, std::uint64_t guard) {
    return static_cast<int>(mm_mach_control(operation::destruct, task, name,
        static_cast<std::uint64_t>(static_cast<std::int64_t>(delta)), guard).status);
}
int darwin_mach_port_get_refs(port_name, port_name, unsigned, unsigned *) DARWIN(mach_port_get_refs);
int darwin_mach_port_get_refs(port_name task, port_name name, unsigned kind, unsigned *out) {
    if (!out) return 4;
    const auto r = mm_mach_control(operation::get_refs, task, name, kind);
    if (!r.status) *out = static_cast<unsigned>(r.args[0]);
    return static_cast<int>(r.status);
}
int darwin_mach_port_mod_refs(port_name task, port_name name, unsigned kind, int delta) {
    return static_cast<int>(mm_mach_control(operation::mod_refs, task, name, kind, static_cast<std::uint64_t>(delta)).status);
}
#define SIMPLE(symbol, op) \
int darwin_##symbol(port_name, port_name) DARWIN(symbol); \
int darwin_##symbol(port_name task, port_name name) { return static_cast<int>(mm_mach_control(operation::op, task, name).status); }
SIMPLE(mach_port_destroy, destroy)
SIMPLE(mach_port_deallocate, deallocate)
#undef SIMPLE
#define THREE(symbol, op) \
int darwin_##symbol(port_name, port_name, port_name) DARWIN(symbol); \
int darwin_##symbol(port_name task, port_name name, port_name value) { return static_cast<int>(mm_mach_control(operation::op, task, name, value).status); }
THREE(mach_port_rename, rename)
THREE(mach_port_move_member, move_member)
THREE(mach_port_insert_member, insert_member)
THREE(mach_port_extract_member, extract_member)
THREE(mach_port_set_mscount, set_mscount)
THREE(mach_port_set_seqno, set_seqno)
#undef THREE
int darwin_mach_port_type(port_name, port_name, unsigned *) DARWIN(mach_port_type);
int darwin_mach_port_type(port_name task, port_name name, unsigned *out) {
    if (!out) return 4;
    const auto r = mm_mach_control(operation::type, task, name);
    if (!r.status) *out = static_cast<unsigned>(r.args[0]);
    return static_cast<int>(r.status);
}
int darwin_mach_port_extract_right(port_name, port_name, unsigned, port_name *, unsigned *) DARWIN(mach_port_extract_right);
int darwin_mach_port_extract_right(port_name task, port_name name, unsigned disposition, port_name *out, unsigned *kind) {
    if (!out || !kind) return 4;
    const auto r = mm_mach_control(operation::extract, task, name, disposition);
    if (!r.status) { *out = static_cast<port_name>(r.args[0]); *kind = static_cast<unsigned>(r.args[1]); }
    return static_cast<int>(r.status);
}
int darwin_mach_port_insert_right(port_name, port_name, port_name, unsigned) DARWIN(mach_port_insert_right);
int darwin_mach_port_insert_right(port_name task, port_name name, port_name source, unsigned disposition) {
    return static_cast<int>(mm_mach_control(operation::insert, task, name, source, disposition).status);
}
int darwin_mach_port_get_set_status(port_name, port_name, port_name **, unsigned *) DARWIN(mach_port_get_set_status);
int darwin_mach_port_get_set_status(port_name task, port_name set, port_name **out, unsigned *count) {
    const auto r = mm_mach_control(operation::members, task, set);
    return r.status ? static_cast<int>(r.status) : export_array(r.data, out, count);
}
int darwin_mach_port_names(port_name, port_name **, unsigned *, unsigned **, unsigned *) DARWIN(mach_port_names);
int darwin_mach_port_names(port_name task, port_name **names, unsigned *name_count, unsigned **types, unsigned *type_count) {
    if (!names || !name_count || !types || !type_count) return 4;
    const auto r = mm_mach_control(operation::names, task);
    if (r.status) return static_cast<int>(r.status);
    std::vector<std::uint8_t> ns(r.args[0] * 4), ts(ns.size());
    for (std::size_t i = 0; i < r.args[0]; ++i) {
        std::memcpy(ns.data() + i * 4, r.data.data() + i * 8, 4);
        std::memcpy(ts.data() + i * 4, r.data.data() + i * 8 + 4, 4);
    }
    const int first = export_array(ns, names, name_count);
    if (first) return first;
    const int second = export_array(ts, types, type_count);
    if (second && *names) munmap(*names, ns.size());
    return second;
}
int darwin_mach_port_get_attributes(port_name, port_name, int, void *, unsigned *) DARWIN(mach_port_get_attributes);
int darwin_mach_port_get_attributes(port_name task, port_name name, int flavor, void *out, unsigned *count) {
    if (!out || !count) return 4;
    const auto r = mm_mach_control(operation::get_attributes, task, name, static_cast<unsigned>(flavor), *count);
    if (!r.status) { std::memcpy(out, r.data.data(), r.data.size()); *count = static_cast<unsigned>(r.args[0]); }
    return static_cast<int>(r.status);
}
int darwin_mach_port_set_attributes(port_name, port_name, int, const void *, unsigned) DARWIN(mach_port_set_attributes);
int darwin_mach_port_set_attributes(port_name task, port_name name, int flavor, const void *info, unsigned count) {
    if (!info || count > 64) return 4;
    mm_ipc::request r; r.op = operation::set_attributes; r.args = {task, name, static_cast<unsigned>(flavor), count}; r.data.resize(count * 4);
    if (!mm_mach_copy(r.data.data(), info, r.data.size())) return 4;
    return static_cast<int>(mm_mach_exchange(r).status);
}
int darwin_mach_port_get_context(port_name, port_name, std::uint64_t *) DARWIN(mach_port_get_context);
int darwin_mach_port_get_context(port_name task, port_name name, std::uint64_t *out) {
    if (!out) return 4;
    const auto r = mm_mach_control(operation::get_context, task, name);
    if (!r.status) *out = r.args[0];
    return static_cast<int>(r.status);
}
int darwin_mach_port_set_context(port_name, port_name, std::uint64_t) DARWIN(mach_port_set_context);
int darwin_mach_port_set_context(port_name task, port_name name, std::uint64_t context) {
    return static_cast<int>(mm_mach_control(operation::set_context, task, name, context).status);
}
int darwin_mach_port_guard(port_name, port_name, std::uint64_t, int) DARWIN(mach_port_guard);
int darwin_mach_port_guard(port_name task, port_name name, std::uint64_t context, int strict) {
    return static_cast<int>(mm_mach_control(operation::guard, task, name, context, static_cast<unsigned>(strict)).status);
}
int darwin_mach_port_unguard(port_name, port_name, std::uint64_t) DARWIN(mach_port_unguard);
int darwin_mach_port_unguard(port_name task, port_name name, std::uint64_t context) {
    return static_cast<int>(mm_mach_control(operation::unguard, task, name, context).status);
}
int darwin_mach_port_request_notification(port_name, port_name, int, unsigned, port_name, unsigned, port_name *) DARWIN(mach_port_request_notification);
int darwin_mach_port_request_notification(port_name task, port_name name, int id, unsigned sync,
                                         port_name notify, unsigned disposition, port_name *previous) {
    if (!previous) return 4;
    mm_ipc::request request; request.op = operation::request_notification;
    request.args = {task, name, static_cast<unsigned>(id), sync, notify, disposition};
    const auto result = mm_mach_exchange(request);
    if (!result.status) *previous = static_cast<port_name>(result.args[0]);
    return static_cast<int>(result.status);
}
}

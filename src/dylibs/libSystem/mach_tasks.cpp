#include "mach_ipc.h"

namespace {
int exception_ports(mm_ipc::operation op, mm_mach_name target, unsigned mask, mm_mach_name replacement,
                    unsigned behavior, unsigned flavor, unsigned *masks, unsigned *count,
                    mm_mach_name *handlers, unsigned *behaviors, unsigned *flavors) {
    if (op != mm_ipc::operation::exception_set && (!count || !masks || !handlers || !behaviors || !flavors)) return 4;
    mm_ipc::request request; request.op = op;
    request.args = {target, mask, replacement, behavior, flavor, count ? *count : 0};
    const auto result = mm_mach_exchange(request);
    if (result.status) return static_cast<int>(result.status);
    if (op != mm_ipc::operation::exception_set) {
        for (unsigned i = 0; i < result.args[0]; ++i) {
            masks[i] = mm_mach_load<unsigned>(result.data, i * 16);
            handlers[i] = mm_mach_load<unsigned>(result.data, i * 16 + 4);
            behaviors[i] = mm_mach_load<unsigned>(result.data, i * 16 + 8);
            flavors[i] = mm_mach_load<unsigned>(result.data, i * 16 + 12);
        }
        *count = static_cast<unsigned>(result.args[0]);
    }
    return 0;
}
}
extern "C" {
const char *darwin_mach_error_string(int) DARWIN(mach_error_string);
const char *darwin_mach_error_string(int error) {
    switch (static_cast<unsigned>(error)) {
    case 0: return "success";
    case 1: return "invalid address";
    case 2: return "protection failure";
    case 3: return "no space available";
    case 4: return "invalid argument";
    case 5: return "failure";
    case 6: return "resource shortage";
    case 13: return "name already exists";
    case 15: return "invalid name";
    case 16: return "invalid task";
    case 17: return "invalid right";
    case 18: return "invalid value";
    case 19: return "user references overflow";
    case 20: return "invalid capability";
    case 21: return "right already exists";
    case 46: return "operation not supported";
    case 0x10000002: return "(ipc/send) invalid data";
    case 0x10000003: return "(ipc/send) invalid destination port";
    case 0x10000004: return "(ipc/send) timed out";
    case 0x10000005: return "(ipc/send) invalid voucher";
    case 0x10000008: return "(ipc/send) message too small";
    case 0x10000009: return "(ipc/send) invalid reply port";
    case 0x1000000a: return "(ipc/send) invalid right";
    case 0x1000000c: return "(ipc/send) invalid out-of-line memory";
    case 0x1000000e: return "(ipc/send) message too large";
    case 0x1000000f: return "(ipc/send) invalid descriptor type";
    case 0x10000010: return "(ipc/send) invalid header";
    case 0x10000012: return "(ipc/send) invalid guard context";
    case 0x10004002: return "(ipc/receive) invalid receive name";
    case 0x10004003: return "(ipc/receive) timed out";
    case 0x10004004: return "(ipc/receive) message too large";
    case 0x10004008: return "(ipc/receive) invalid data buffer";
    case 0x10004009: return "(ipc/receive) port changed or died";
    case 0x1000400a: return "(ipc/receive) port is in a set";
    case 0x1000400c: return "(ipc/receive) invalid message body";
    default: return "unknown Mach error";
    }
}
int darwin_task_for_pid(mm_mach_name, int, mm_mach_name *) DARWIN(task_for_pid);
int darwin_task_for_pid(mm_mach_name, int pid, mm_mach_name *out) {
    if (!out || pid <= 0) return 4;
    mm_ipc::request request; request.op = mm_ipc::operation::task_for_pid; request.args[0] = static_cast<unsigned>(pid);
    const auto result = mm_mach_exchange(request);
    if (!result.status) *out = static_cast<mm_mach_name>(result.args[0]);
    return static_cast<int>(result.status);
}
#define EXCEPTION_PORTS(prefix) \
int darwin_##prefix##_set_exception_ports(mm_mach_name, unsigned, mm_mach_name, unsigned, unsigned) DARWIN(prefix##_set_exception_ports); \
int darwin_##prefix##_set_exception_ports(mm_mach_name target, unsigned mask, mm_mach_name handler, unsigned behavior, unsigned flavor) { \
    return exception_ports(mm_ipc::operation::exception_set, target, mask, handler, behavior, flavor, nullptr, nullptr, nullptr, nullptr, nullptr); \
} \
int darwin_##prefix##_get_exception_ports(mm_mach_name, unsigned, unsigned *, unsigned *, mm_mach_name *, unsigned *, unsigned *) DARWIN(prefix##_get_exception_ports); \
int darwin_##prefix##_get_exception_ports(mm_mach_name target, unsigned mask, unsigned *masks, unsigned *count, mm_mach_name *handlers, unsigned *behaviors, unsigned *flavors) { \
    return exception_ports(mm_ipc::operation::exception_get, target, mask, 0, 0, 0, masks, count, handlers, behaviors, flavors); \
} \
int darwin_##prefix##_swap_exception_ports(mm_mach_name, unsigned, mm_mach_name, unsigned, unsigned, unsigned *, unsigned *, mm_mach_name *, unsigned *, unsigned *) DARWIN(prefix##_swap_exception_ports); \
int darwin_##prefix##_swap_exception_ports(mm_mach_name target, unsigned mask, mm_mach_name replacement, unsigned behavior, unsigned flavor, unsigned *masks, unsigned *count, mm_mach_name *handlers, unsigned *behaviors, unsigned *flavors) { \
    return exception_ports(mm_ipc::operation::exception_swap, target, mask, replacement, behavior, flavor, masks, count, handlers, behaviors, flavors); \
}
EXCEPTION_PORTS(task)
EXCEPTION_PORTS(thread)
#undef EXCEPTION_PORTS
}

#include "mach_ipc.h"

namespace {
mm_ipc::response bootstrap(mm_ipc::operation op, mm_mach_name bp, const char *service, mm_mach_name port = 0) {
    mm_ipc::response error; error.status = 1104;
    if (!service) return error;
    mm_ipc::request request; request.op = op; request.args = {bp, port};
    for (std::size_t i = 0; i < 128; ++i) {
        char ch;
        if (!mm_mach_copy(&ch, service + i, 1)) return error;
        if (!ch) return mm_mach_exchange(request);
        request.data.push_back(static_cast<std::uint8_t>(ch));
    }
    return error;
}
}
extern "C" {
extern mm_mach_name darwin_bootstrap_port __asm__("_bootstrap_port");
__attribute__((visibility("default"))) mm_mach_name darwin_bootstrap_port = 3;
int darwin_bootstrap_register(mm_mach_name, const char *, mm_mach_name) DARWIN(bootstrap_register);
int darwin_bootstrap_register(mm_mach_name bp, const char *name, mm_mach_name port) {
    return static_cast<int>(bootstrap(mm_ipc::operation::bootstrap_register, bp, name, port).status);
}
#define BOOTSTRAP_OUT(symbol, op) \
int darwin_##symbol(mm_mach_name, const char *, mm_mach_name *) DARWIN(symbol); \
int darwin_##symbol(mm_mach_name bp, const char *name, mm_mach_name *out) { \
    if (!out) return 4; \
    const auto r = bootstrap(mm_ipc::operation::op, bp, name); \
    if (!r.status) *out = static_cast<mm_mach_name>(r.args[0]); \
    return static_cast<int>(r.status); }
BOOTSTRAP_OUT(bootstrap_look_up, bootstrap_lookup)
BOOTSTRAP_OUT(bootstrap_check_in, bootstrap_check_in)
BOOTSTRAP_OUT(bootstrap_create_service, bootstrap_create_service)
BOOTSTRAP_OUT(bootstrap_status, bootstrap_status)
#undef BOOTSTRAP_OUT
const char *darwin_bootstrap_strerror(int) DARWIN(bootstrap_strerror);
const char *darwin_bootstrap_strerror(int status) {
    switch (status) {
    case 0: return "Success"; case 1100: return "Not privileged"; case 1101: return "Name already in use";
    case 1102: return "Unknown service"; case 1103: return "Service already active"; case 1104: return "Bad count";
    case 1105: return "No memory"; case 1106: return "No children"; default: return "Unknown bootstrap error";
    }
}
}

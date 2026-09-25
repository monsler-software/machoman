/* AppKit's entry point: registers every class the framework defines and
 * keeps the runtime's host API. The classes are registered from a
 * constructor, because the guest's bind records are resolved before any
 * guest code runs and they must already exist. */
#include "appkit.h"
#include "synthesise.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace ak {

namespace {
const mm_host_api *g_api = nullptr;
bool g_trace = false;
} // namespace

void set_api(const mm_host_api *a) { g_api = a; }

const mm_host_api *api() { return g_api; }
bool trace() {
    /* MACHOMAN_APPKIT_TRACE traces the window system alone; MACHOMAN_TRACE
     * also traces every host call, which is far too much to read. */
    static const bool on = std::getenv("MACHOMAN_TRACE") != nullptr ||
                           std::getenv("MACHOMAN_APPKIT_TRACE") != nullptr;
    g_trace = on;
    return on;
}

std::string to_utf8(id nsstring) {
    if (!nsstring) return {};
    const char *utf8 = reinterpret_cast<const char *>(send(nsstring, "UTF8String"));
    return utf8 ? std::string(utf8) : std::string();
}
id string(const std::string &utf8) {
    return obj(send(reinterpret_cast<id>(cls("NSString")), "stringWithUTF8String:", ret(utf8.c_str())));
}
id number(double v) {
    mm_cpu_context *ctx = g_api ? g_api->current_context() : nullptr;
    if (ctx) std::memcpy(&ctx->v[0][0], &v, 8);
    return obj(send(reinterpret_cast<id>(cls("NSNumber")), "numberWithDouble:"));
}

} // namespace ak

extern "C" {

__attribute__((visibility("default"))) void machoman_dylib_init(const mm_host_api *api) { ak::set_api(api); }

/* libobjc and Foundation are dependencies, so their constructors (and
 * therefore NSObject and NSString) have already run when this one does. */
static void appkit_ctor() __attribute__((constructor));
static void appkit_ctor() {
    ak::set_api(mm_objc_host_api());
    ak::register_responder();
    ak::register_views();
    ak::register_application();
    ak::register_opengl();
}

} // extern "C"

/* AppKit defines its classes but not yet every constant the guest binds
 * to; those are synthesised from their names (see symbol-synthesis). */
extern "C" __attribute__((visibility("default"))) void *machoman_resolve_symbol(const char *name, unsigned flags) {
    return mm_synthesise_symbol(name, flags);
}

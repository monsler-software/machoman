/* AppKit's entry point: registers every class the framework defines and
 * keeps the runtime's host API. The classes are registered from a
 * constructor, because the guest's bind records are resolved before any
 * guest code runs and they must already exist. */
#include "appkit.h"
#include "synthesise.h"

#include <cstdio>
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
    ak::register_file_panels();
    ak::register_window_controller();
    ak::register_opengl();
    ak::register_layers();
}

} // extern "C"

/* AppKit defines its classes but not yet every constant the guest binds
 * to; those are synthesised from their names (see symbol-synthesis). */
extern "C" __attribute__((visibility("default"))) void *machoman_resolve_symbol(const char *name, unsigned flags) {
    return mm_synthesise_symbol(name, flags);
}

/* The public application entry point belongs to AppKit and is re-exported
 * by the Cocoa and Quartz umbrellas. Bundle metadata selects the application
 * class and main nib; -run delivers the launch notifications and event loop. */
extern "C" int NSApplicationMain_(int, const char **) DARWIN(NSApplicationMain);
extern "C" int NSApplicationMain_(int, const char **) {
    using namespace ak;
    autorelease_pool pool;
    id bundle = obj(send(reinterpret_cast<id>(cls("NSBundle")), "mainBundle"));
    auto info = [&](const char *key) { return obj(send(bundle, "objectForInfoDictionaryKey:", ret(string(key)))); };
    auto principal = to_utf8(info("NSPrincipalClass"));
    Class application = principal.empty() ? cls("NSApplication") : cls(principal.c_str());
    if (!application) {
        std::fprintf(stderr, "AppKit: NSApplicationMain: unknown principal class %s\n", principal.c_str());
        return 1;
    }
    id app = obj(send(reinterpret_cast<id>(application), "sharedApplication"));
    const auto nib = to_utf8(info("NSMainNibFile"));
    std::vector<id> top_level;
    if (!nib.empty() && !load_nib(nib, app, &top_level)) {
        std::fprintf(stderr, "AppKit: NSApplicationMain: unable to load %s\n", nib.c_str());
        return 1;
    }
    // Top-level objects must outlive the autorelease pool of nib decoding.
    for (id object : top_level) retain(object);
    send(app, "run");
    for (id object : top_level) release(object);
    return 0;
}

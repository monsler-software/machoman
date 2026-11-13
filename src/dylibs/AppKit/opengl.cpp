/* NSOpenGLContext, NSOpenGLPixelFormat and NSOpenGLLayer.
 *
 * An NSOpenGLContext is an EGL context on the platform surface of the view
 * it is attached to; the GL entry points themselves are the host's, which
 * the guest reaches through the OpenGL framework stub resolving to
 * libGL/libGLESv2. Without a display the context is null and the drawing
 * calls are no-ops, which is what the headless backend reports. */
#include "appkit.h"

#include <cstdio>
#include <mutex>
#include <unordered_map>

MM_FW_CLASS(NSOpenGLContext);
MM_FW_CLASS(NSOpenGLPixelFormat);

namespace ak {

namespace {

struct gl_context_object;
std::mutex context_mutex;
std::unordered_map<void *, gl_context_object *> &contexts() {
    static auto *registry = new std::unordered_map<void *, gl_context_object *>;
    return *registry;
}

struct gl_context_object {
    mm_objc_object base;
    void *egl = nullptr;
    id view = nullptr;
    platform_window *window = nullptr;
    ~gl_context_object() {
        if (g_current_gl_context == reinterpret_cast<id>(this)) {
            g_current_gl_context = nullptr;
            platform::make_current(nullptr, nullptr);
        }
        std::lock_guard lock(context_mutex);
        if (auto it = contexts().find(egl); it != contexts().end() && it->second == this)
            contexts().erase(it);
    }
};
gl_context_object *gc(id o) { return as<gl_context_object>(o); }

platform_window *window_of_view(id view) {
    if (!view) return nullptr;
    id window = obj(send(view, "window"));
    if (window && !as<window_object>(window)->platform) {
        ensure_platform_window(window);
    }
    return window ? as<window_object>(window)->platform : nullptr;
}

std::uint64_t ctx_initWithFormatShareContext(id self, SEL, id, id share) {
    gc(self)->egl = platform::create_gl_context(nullptr, share ? gc(share)->egl : nullptr);
    if (trace()) std::fprintf(stderr, "AppKit: initWithFormat:shareContext: self=%p share=%p egl=%p\n", static_cast<void *>(self), static_cast<void *>(share), gc(self)->egl);
    if (gc(self)->egl) {
        std::lock_guard lock(context_mutex);
        contexts()[gc(self)->egl] = gc(self);
    }
    return ret(self);
}
std::uint64_t ctx_setView(id self, SEL, id view) {
    gl_context_object *o = gc(self);
    o->view = view;
    o->window = window_of_view(view);
    return 0;
}
std::uint64_t ctx_view(id self, SEL) { return ret(gc(self)->view); }
std::uint64_t ctx_makeCurrentContext(id self, SEL) {
    gl_context_object *o = gc(self);
    g_current_gl_context = self;
    if (!o->window) o->window = window_of_view(o->view);
    if (trace()) std::fprintf(stderr, "AppKit: ctx_makeCurrentContext: egl=%p window=%p\n", o->egl, o->window);
    if (trace() && !o->egl && api() && api()->print_guest_backtrace) api()->print_guest_backtrace();
    platform::make_current(o->egl, o->window);
    return 0;
}
std::uint64_t ctx_clearCurrentContext(id, SEL) {
    g_current_gl_context = nullptr;
    platform::make_current(nullptr, nullptr);
    return 0;
}
std::uint64_t ctx_currentContext(id, SEL) { return ret(g_current_gl_context); }
std::uint64_t ctx_flushBuffer(id self, SEL) {
    gl_context_object *o = gc(self);
    if (!o->window) o->window = window_of_view(o->view);
    if (trace()) std::fprintf(stderr, "AppKit: ctx_flushBuffer: egl=%p window=%p\n", o->egl, o->window);
    platform::swap_buffers(o->egl, o->window);
    return 0;
}
std::uint64_t ctx_update(id self, SEL) {
    gc(self)->window = window_of_view(gc(self)->view);
    return 0;
}
std::uint64_t ctx_setValuesForParameter(id self, SEL, const std::int32_t *values, std::uint64_t parameter) {
    if (parameter == 222 && values) /* NSOpenGLContextParameterSwapInterval */
        platform::set_swap_interval(gc(self)->egl, *values);
    return 0;
}
std::uint64_t ctx_CGLContextObj(id self, SEL) { return ret(gc(self)->egl); }
std::uint64_t gl_noop(id, SEL, ...) { return 0; }

std::uint64_t format_initWithAttributes(id self, SEL, const std::uint32_t *) { return ret(self); }

} // namespace

thread_local id g_current_gl_context = nullptr;

void register_opengl() {
    Class nsobject = cls("NSObject");

    Class format = mm_objc_define_class("NSOpenGLPixelFormat", nsobject, sizeof(mm_objc_object),
                                        MM_FW_CLASS_REF(NSOpenGLPixelFormat), MM_FW_META_REF(NSOpenGLPixelFormat));
    MM_METHODS(format, {"initWithAttributes:", MM_IMP(format_initWithAttributes), "@24@0:8^I16"});

    Class context = mm_objc_define_class("NSOpenGLContext", nsobject, sizeof(gl_context_object),
                                         MM_FW_CLASS_REF(NSOpenGLContext), MM_FW_META_REF(NSOpenGLContext));
    register_lifecycle<gl_context_object>(context);
    MM_METHODS(context,
        {"initWithFormat:shareContext:", MM_IMP(ctx_initWithFormatShareContext), "@32@0:8@16@24"},
        {"setView:", MM_IMP(ctx_setView), "v24@0:8@16"},
        {"view", MM_IMP(ctx_view), "@16@0:8"},
        {"makeCurrentContext", MM_IMP(ctx_makeCurrentContext), "v16@0:8"},
        {"flushBuffer", MM_IMP(ctx_flushBuffer), "v16@0:8"},
        {"update", MM_IMP(ctx_update), "v16@0:8"},
        {"setValues:forParameter:", MM_IMP(ctx_setValuesForParameter), "v32@0:8^i16q24"},
        {"CGLContextObj", MM_IMP(ctx_CGLContextObj), "^v16@0:8"},
        {"clearDrawable", MM_IMP(gl_noop), "v16@0:8"});
    MM_CLASS_METHODS(context,
        {"currentContext", MM_IMP(ctx_currentContext), "@16@0:8"},
        {"clearCurrentContext", MM_IMP(ctx_clearCurrentContext), "v16@0:8"});
}

extern "C" {

__attribute__((visibility("default"))) void *CGLGetCurrentContext(void) {
    return g_current_gl_context ? gc(g_current_gl_context)->egl : nullptr;
}

__attribute__((visibility("default"))) std::uint64_t CGLSetCurrentContext(void *ctx) {
    if (!ctx) {
        g_current_gl_context = nullptr;
        platform::make_current(nullptr, nullptr);
        return 0;
    }
    /* CGLContextObj is an opaque EGL handle, not an Objective-C object.
     * Find its owner without dereferencing the handle, and restore the
     * owner's drawable instead of using the previous context's window. */
    gl_context_object *owner;
    {
        std::lock_guard lock(context_mutex);
        const auto it = contexts().find(ctx);
        if (it == contexts().end()) return 10004; /* kCGLBadContext */
        owner = it->second;
    }
    return ctx_makeCurrentContext(reinterpret_cast<id>(owner), nullptr);
}

} // extern "C"

} // namespace ak

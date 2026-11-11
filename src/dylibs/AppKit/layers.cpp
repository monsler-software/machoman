/* CALayer and NSOpenGLLayer: the layer a view draws through.
 *
 * A modern Cocoa program does not draw in -drawRect: any more. It asks for
 * a layer-backed view, hands back its own CALayer subclass from
 * -makeBackingLayer, and renders into it; Ableton Live's view returns a
 * TOpenGLLayer, an NSOpenGLLayer subclass that overrides the four methods
 * AppKit calls every frame:
 *
 *   -openGLPixelFormatForDisplayMask:            which format to use
 *   -openGLContextForPixelFormat:                the context to draw into
 *   -canDrawInOpenGLContext:...forLayerTime:displayTime:
 *   -drawInOpenGLContext:...forLayerTime:displayTime:
 *
 * so this is where the program's own drawing actually happens. Without a
 * layer none of it is ever called and the window stays empty, which is
 * exactly what machoman showed before.
 *
 * The layer time is a double: it reaches a *guest* override through the
 * guest's own v0, which is why the sends below go through `send_layer_time`
 * rather than the plain integer-argument helpers. The host defaults in this
 * file therefore ignore it — they take only the integer arguments. */
#include "appkit.h"

#include <chrono>
#include <atomic>
#include <cstdio>
#include <dlfcn.h>
#include <cmath>
#ifdef MM_APPKIT_MEVK
#include "mevk/render.h"
extern "C" void machoman_mevk_register_layer();
#endif

MM_FW_CLASS(CALayer);
MM_FW_CLASS(CATransaction);
MM_FW_CLASS(NSOpenGLLayer);
MM_FW_CLASS(CAMetalLayer);

namespace ak {

view_object::~view_object() {
    for(id sub:subviews){auto *child=as<view_object>(sub);child->superview=nullptr;child->window=nullptr;release(sub);}
    if (layer) {
        if (is_kind(layer, cls("CALayer"))) {
            auto *lo = as<layer_object>(layer);
            lo->view = nullptr;
            platform::release_metal(lo);
        }
        mm_objc_release(layer);
    }
}

namespace {

layer_object *l(id o) { return as<layer_object>(o); }

Class g_layer = nullptr;

/* CoreVideo's timestamp, as a program reads it in -canDraw...: the SMPTE
 * member is 24 bytes, so `flags` is at offset 64 — Live tests bit 1
 * (kCVTimeStampHostTimeValid) there before it agrees to draw. */
struct cv_time_stamp {
    std::uint32_t version;
    std::int32_t video_time_scale;
    std::int64_t video_time;
    std::uint64_t host_time;
    double rate_scalar;
    std::int64_t video_refresh_period;
    struct { std::int16_t subframes, subframe_divisor; std::uint32_t counter, type, flags;
             std::int16_t hours, minutes, seconds, frames; } smpte_time;
    std::uint64_t flags;
    std::uint64_t reserved;
};
static_assert(offsetof(cv_time_stamp, flags) == 64, "CVTimeStamp.flags is at offset 64");

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

cv_time_stamp frame_timestamp(std::int64_t frame) {
    cv_time_stamp ts{};
    ts.version = 0;
    ts.video_time_scale = 60000;
    ts.video_time = frame * 1000;
    ts.host_time = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    ts.rate_scalar = 1.0;
    ts.video_refresh_period = 1000;
    ts.flags = 0x1 | 0x2; /* video time and host time are both valid */
    return ts;
}

/* A message whose first floating-point argument is the layer time. Only a
 * guest override reads it (from v0); a host implementation here is declared
 * without it. */
std::uint64_t send_layer_time(id receiver, const char *selector, double layer_time,
                              const std::uint64_t *args, int nargs) {
    if (mm_cpu_context *ctx = api()->current_context()) std::memcpy(&ctx->v[0][0], &layer_time, 8);
    return mm_objc_send(receiver, mm_objc_sel(selector), nargs, args);
}

/* ---- CALayer ---------------------------------------------------------------- */

std::uint64_t layer_init(id self, SEL) { return ret(self); }
std::uint64_t layerc_layer(id c, SEL) { return ret(autorelease(obj(send(obj(send(c, "alloc")), "init")))); }

mm_two_doubles layer_bounds(id self, SEL) {
    const rect &b = l(self)->bounds;
    mm_objc_return_doubles_hi(b.w, b.h);
    return {b.x, b.y};
}
std::uint64_t layer_setBounds(id self, SEL, double x, double y, double w, double h) {
    l(self)->bounds = {x, y, w, h};
    return 0;
}
mm_two_doubles layer_frame(id self, SEL) {
    const rect &f = l(self)->frame;
    mm_objc_return_doubles_hi(f.w, f.h);
    return {f.x, f.y};
}
std::uint64_t layer_setFrame(id self, SEL, double x, double y, double w, double h) {
    layer_object *o = l(self);
    o->frame = {x, y, w, h};
    o->bounds = {0, 0, w, h};
    o->position = {x + w / 2, y + h / 2};
    return 0;
}
mm_two_doubles layer_position(id self, SEL) { return {l(self)->position.x, l(self)->position.y}; }
std::uint64_t layer_setPosition(id self, SEL, double x, double y) { l(self)->position = {x, y}; return 0; }
mm_two_doubles layer_anchorPoint(id self, SEL) { return {l(self)->anchor.x, l(self)->anchor.y}; }
std::uint64_t layer_setAnchorPoint(id self, SEL, double x, double y) { l(self)->anchor = {x, y}; return 0; }
double layer_contentsScale(id self, SEL) { return l(self)->contents_scale; }
std::uint64_t layer_setContentsScale(id self, SEL, double s) { l(self)->contents_scale = std::isfinite(s) && s > 0 ? s : 1.0; return 0; }
constexpr const char *gravity_names[] = {"resize", "resizeAspect", "resizeAspectFill", "center", "top", "topRight",
    "right", "bottomRight", "bottom", "bottomLeft", "left", "topLeft"};
std::uint64_t layer_contentsGravity(id self, SEL) { return ret(string(gravity_names[l(self)->contents_placement])); }
std::uint64_t layer_setContentsGravity(id self, SEL, id gravity) {
    const auto name = to_utf8(gravity);
    for (unsigned i = 0; i < 12; ++i) if (name == gravity_names[i]) {
        l(self)->contents_placement = i;
        break;
    }
    return 0;
}

std::uint64_t layer_setNeedsDisplay(id self, SEL) {
    layer_object *o = l(self);
    o->needs_display = true;
    const rect &b = o->bounds;
    if (mm_cpu_context *ctx = api()->current_context()) {
        std::memcpy(&ctx->v[0][0], &b.x, 8);
        std::memcpy(&ctx->v[1][0], &b.y, 8);
        std::memcpy(&ctx->v[2][0], &b.w, 8);
        std::memcpy(&ctx->v[3][0], &b.h, 8);
    }
    send(self, "setNeedsDisplayInRect:");
    return 0;
}
std::uint64_t layer_setNeedsDisplayInRect(id self, SEL, double, double, double, double) {
    l(self)->needs_display = true;
    return 0;
}
std::uint64_t layer_needsDisplay(id self, SEL) { return l(self)->needs_display ? 1 : 0; }
std::uint64_t layer_display(id self, SEL) { l(self)->needs_display = false; return 0; }
std::uint64_t layer_displayIfNeeded(id self, SEL) {
    if (l(self)->needs_display) send(self, "display");
    return 0;
}

std::uint64_t layer_delegate(id self, SEL) { return ret(l(self)->delegate); }
std::uint64_t layer_setDelegate(id self, SEL, id d) { l(self)->delegate = d; return 0; }
std::uint64_t layer_superlayer(id self, SEL) { return ret(l(self)->superlayer); }
std::uint64_t layer_sublayers(id self, SEL) {
    id array = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
    for (id s : l(self)->sublayers) send(array, "addObject:", ret(s));
    return ret(array);
}
std::uint64_t layer_addSublayer(id self, SEL, id sub) {
    if (!sub) return 0;
    l(self)->sublayers.push_back(retain(sub));
    l(sub)->superlayer = self;
    return 0;
}
std::uint64_t layer_removeFromSuperlayer(id self, SEL) {
    platform::release_metal(l(self));
    if (id super = l(self)->superlayer) {
        l(self)->superlayer = nullptr;
        auto &subs = l(super)->sublayers;
        for (auto it = subs.begin(); it != subs.end(); ++it)
            if (*it == self) { subs.erase(it); release(self); break; }
    }
    return 0;
}

std::uint64_t layer_isOpaque(id self, SEL) { return l(self)->opaque ? 1 : 0; }
std::uint64_t layer_setOpaque(id self, SEL, std::uint64_t flag) { l(self)->opaque = (flag & 0xFF) != 0; return 0; }
std::uint64_t layer_isHidden(id self, SEL) { return l(self)->hidden ? 1 : 0; }
std::uint64_t layer_setHidden(id self, SEL, std::uint64_t flag) {
    l(self)->hidden = (flag & 0xFF) != 0;
    if (l(self)->hidden) platform::release_metal(l(self));
    return 0;
}
std::uint64_t layer_contents(id self, SEL) { return ret(l(self)->contents); }
std::uint64_t layer_setContents(id self, SEL, id c) {
    layer_object *o = l(self);
    if (o->contents != c) { release(o->contents); o->contents = retain(c); }
    return 0;
}
std::uint64_t layer_noop(id, SEL, ...) { return 0; }

/* ---- NSOpenGLLayer ---------------------------------------------------------- */

std::uint64_t gllayer_openGLContext(id self, SEL) { return ret(l(self)->gl_context); }
std::uint64_t gllayer_setOpenGLContext(id self, SEL, id context) {
    layer_object *o = l(self);
    if (o->gl_context != context) { release(o->gl_context); o->gl_context = retain(context); }
    return 0;
}
std::uint64_t gllayer_openGLPixelFormat(id self, SEL) { return ret(l(self)->gl_format); }
std::uint64_t gllayer_setOpenGLPixelFormat(id self, SEL, id format) {
    layer_object *o = l(self);
    if (o->gl_format != format) { release(o->gl_format); o->gl_format = retain(format); }
    return 0;
}
std::uint64_t gllayer_view(id self, SEL) { return ret(l(self)->view); }
std::uint64_t gllayer_setView(id self, SEL, id view) { l(self)->view = view; return 0; }
std::uint64_t gllayer_asynchronous(id self, SEL) { return l(self)->asynchronous ? 1 : 0; }
std::uint64_t gllayer_setAsynchronous(id self, SEL, std::uint64_t flag) {
    l(self)->asynchronous = (flag & 0xFF) != 0;
    return 0;
}

/* The defaults a subclass overrides; they are what AppKit itself would do. */
std::uint64_t gllayer_pixelFormatForDisplayMask(id, SEL, std::uint32_t) {
    id format = obj(send(reinterpret_cast<id>(cls("NSOpenGLPixelFormat")), "alloc"));
    return ret(autorelease(obj(send(format, "initWithAttributes:", 0))));
}
std::uint64_t gllayer_contextForPixelFormat(id, SEL, id format) {
    id context = obj(send(reinterpret_cast<id>(cls("NSOpenGLContext")), "alloc"));
    return ret(autorelease(obj(send(context, "initWithFormat:shareContext:", ret(format), 0))));
}
/* The layer time and display time are the guest's arguments; a host
 * implementation has nothing to decide, so it always agrees to draw and
 * draws nothing. */
std::uint64_t gllayer_canDraw(id, SEL, id, id, id) { return 1; }
std::uint64_t gllayer_draw(id, SEL, id, id, id) { return 0; }

} // namespace

/* ---- the frame ---------------------------------------------------------------
 *
 * One turn of the event loop is one frame: every layer-backed view of a
 * visible window is given its context, asked whether it wants to draw and,
 * if it does, told to. This is what a CVDisplayLink does on macOS for an
 * asynchronous layer. */
namespace {
thread_local std::uint64_t current_display_pass = 0, next_display_pass = 0;
}
layer_display_pass::layer_display_pass() : previous(current_display_pass) {
    current_display_pass = ++next_display_pass;
}
layer_display_pass::~layer_display_pass() { current_display_pass = previous; }

void draw_layer(id view, id layer) {
    layer_object *o = l(layer);
    static std::int64_t frame_number = 0;
    static const bool profile = std::getenv("MACHOMAN_APPKIT_FRAME_TIMING") != nullptr;
    if (current_display_pass && o->last_display_pass == current_display_pass) return;
    const double layer_time = now_seconds();
    if (layer_time - o->last_frame_time < 0.016) {
        return;
    }
    o->last_frame_time = layer_time;
    o->last_display_pass = current_display_pass;

    const rect &vf = as<view_object>(view)->frame;
    if (o->bounds.w == 0 && vf.w > 0) {
        o->bounds = {0, 0, vf.w, vf.h};
        o->frame = {0, 0, vf.w, vf.h};
        if (mm_cpu_context *ctx = api()->current_context()) {
            const double values[4] = {0, 0, vf.w, vf.h};
            for (int i = 0; i < 4; i++) std::memcpy(&ctx->v[i][0], &values[i], 8);
        }
        send(layer, "setBounds:");
    }

    if (!o->gl_format) {
        const std::uint64_t mask = 1;
        o->gl_format = retain(obj(mm_objc_send(layer, mm_objc_sel("openGLPixelFormatForDisplayMask:"), 1, &mask)));
        if (o->gl_format && !send(layer, "openGLPixelFormat")) send(layer, "setOpenGLPixelFormat:", ret(o->gl_format));
    }
    if (!o->gl_context) {
        const std::uint64_t format = ret(o->gl_format);
        id context = obj(mm_objc_send(layer, mm_objc_sel("openGLContextForPixelFormat:"), 1, &format));
        if (!context) return;
        send(layer, "setOpenGLContext:", ret(context));
        if (!o->gl_context) o->gl_context = retain(context); /* a subclass that keeps it elsewhere */
        if (trace()) std::fprintf(stderr, "AppKit: %s has an OpenGL layer, context %p\n",
                                  mm_objc_class_name(mm_objc_object_class(layer)), static_cast<void *>(context));
    }
    id context = o->gl_context;
    if (!context) return;

    send(context, "setView:", ret(view));
    send(context, "makeCurrentContext");

    if (responds(view, "ensureValidSize")) {
        send(view, "ensureValidSize");
    }
    if (responds(view, "updateContents")) {
        send(view, "updateContents");
    }

    cv_time_stamp ts = frame_timestamp(frame_number++);
    const std::uint64_t args[3] = {ret(context), ret(o->gl_format), ret(&ts)};
    uint64_t can_draw = send_layer_time(layer, "canDrawInOpenGLContext:pixelFormat:forLayerTime:displayTime:", layer_time, args, 3);
    if (trace()) std::fprintf(stderr, "AppKit: draw_layer: canDraw=%llu\n", (unsigned long long)can_draw);
    if (!can_draw)
        return;
    const double updated_at = profile ? now_seconds() : 0;
    send_layer_time(layer, "drawInOpenGLContext:pixelFormat:forLayerTime:displayTime:", layer_time, args, 3);
    const double drawn_at = profile ? now_seconds() : 0;
    if (trace()) std::fprintf(stderr, "AppKit: draw_layer: drawInOpenGLContext finished, flushing buffer\n");
    static int probes = 0;
    if (trace() && probes < 4) {
        ++probes;
        /* What state did the guest leave the context in? If it drew into an
         * FBO of its own, the default framebuffer we present stays black. */
        using intp_fn = void (*)(unsigned, int *);
        auto get_iv = reinterpret_cast<intp_fn>(dlsym(RTLD_DEFAULT, "glGetIntegerv"));
        auto get_err = reinterpret_cast<unsigned (*)()>(dlsym(RTLD_DEFAULT, "glGetError"));
        using wide_fn = unsigned (*)(void *, void *, int, int *);
        auto query_surface = reinterpret_cast<wide_fn>(dlsym(RTLD_DEFAULT, "eglQuerySurface"));
        if (get_iv && get_err && query_surface) {
            int fbo = 0, vp[4] = {}, sw = 0, sh = 0;
            get_err();
            get_iv(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &fbo);
            get_iv(0x0BA2 /* GL_VIEWPORT */, vp);
            window_object *wo = as<view_object>(view)->window ? as<window_object>(as<view_object>(view)->window) : nullptr;
            void *egl = wo && wo->platform ? platform::egl_surface_of(wo->platform) : nullptr;
            void *edpy = platform::egl_display_of();
            if (egl && edpy) {
                query_surface(edpy, egl, 0x3057 /* EGL_WIDTH */, &sw);
                query_surface(edpy, egl, 0x3056 /* EGL_HEIGHT */, &sh);
            }
            const unsigned err = get_err();
            using read_fn = void (*)(int, int, int, int, unsigned, unsigned, void *);
            auto read_px = reinterpret_cast<read_fn>(dlsym(RTLD_DEFAULT, "glReadPixels"));
            unsigned char center[4] = {}, corner[4] = {};
            if (read_px && sw > 0 && sh > 0) {
                read_px(sw / 2, sh / 2, 1, 1, 0x1907 /* GL_RGB */, 0x1401 /* GL_UNSIGNED_BYTE */, center);
                read_px(2, 2, 1, 1, 0x1907, 0x1401, corner);
            }
            std::fprintf(stderr, "AppKit: post-draw GL: fbo=%d viewport={%d,%d,%d,%d} surface=%dx%d center={%u,%u,%u} corner={%u,%u,%u} err=%u\n",
                             fbo, vp[0], vp[1], vp[2], vp[3], sw, sh, center[0], center[1], center[2],
                             corner[0], corner[1], corner[2], err);
        }
    }
    send(context, "flushBuffer");
    if (profile) {
        const double finished_at = now_seconds();
        ++o->profile_frames;
        o->profile_update_ms += (updated_at - layer_time) * 1000;
        o->profile_draw_ms += (drawn_at - updated_at) * 1000;
        o->profile_present_ms += (finished_at - drawn_at) * 1000;
        o->profile_max_ms = std::max(o->profile_max_ms, (finished_at - layer_time) * 1000);
        if (!o->profile_report_time) o->profile_report_time = layer_time;
        if (finished_at - o->profile_report_time >= 3) {
            std::fprintf(stderr, "AppKit frame timing: layer=%p size=%gx%g frames=%u update=%.2f draw=%.2f present=%.2f ms average, max=%.2f ms\n",
                         static_cast<void *>(layer), vf.w, vf.h, o->profile_frames,
                         o->profile_update_ms / o->profile_frames, o->profile_draw_ms / o->profile_frames,
                         o->profile_present_ms / o->profile_frames, o->profile_max_ms);
            o->profile_frames = 0;
            o->profile_update_ms = o->profile_draw_ms = o->profile_present_ms = o->profile_max_ms = 0;
            o->profile_report_time = finished_at;
        }
    }
    o->needs_display = false;
}

void draw_layers(id view) {
    if (!view) return;
    view_object *o = as<view_object>(view);
    if (trace()) {
        std::fprintf(stderr, "AppKit: draw_layers: view=%s (frame %gx%g) layer=%s (%zu subviews)\n",
                     mm_objc_class_name(mm_objc_object_class(view)), o->frame.w, o->frame.h,
                     o->layer ? mm_objc_class_name(mm_objc_object_class(o->layer)) : "(none)",
                     o->subviews.size());
    }
    if (o->layer && is_kind(o->layer, cls("NSOpenGLLayer"))) draw_layer(view, o->layer);
    for (id sub : o->subviews) draw_layers(sub);
}

/* The layer a view gets from -setWantsLayer:YES: its own, if it makes one. */
id make_backing_layer(id view) {
    id layer = responds(view, "makeBackingLayer") ? obj(send(view, "makeBackingLayer"))
                                                  : obj(send(reinterpret_cast<id>(g_layer), "layer"));
    if (!layer) return nullptr;
    retain(layer);
    layer_object *lo = l(layer);
    lo->view = view;
    const rect &f = as<view_object>(view)->frame;
    lo->bounds = {0, 0, f.w, f.h};
    lo->frame = {0, 0, f.w, f.h};
    lo->contents_placement = as<view_object>(view)->contents_placement;
    if (responds(layer, "setView:")) send(layer, "setView:", ret(view));
    if (mm_cpu_context *ctx = api()->current_context()) {
        const double values[4] = {0, 0, f.w, f.h};
        for (int i = 0; i < 4; i++) std::memcpy(&ctx->v[i][0], &values[i], 8);
    }
    send(layer, "setBounds:");
    if (mm_cpu_context *ctx = api()->current_context()) {
        const double values[4] = {0, 0, f.w, f.h};
        for (int i = 0; i < 4; i++) std::memcpy(&ctx->v[i][0], &values[i], 8);
    }
    send(layer, "setNeedsDisplayInRect:");
    if (as<view_object>(view)->window && responds(layer, "viewDidMoveToWindow"))
        send(layer, "viewDidMoveToWindow");
    if (trace())
        std::fprintf(stderr, "AppKit: %s is layer-backed by a %s\n", mm_objc_class_name(mm_objc_object_class(view)),
                     mm_objc_class_name(mm_objc_object_class(layer)));
    return layer;
}

void register_layers() {
    Class nsobject = cls("NSObject");

    g_layer = mm_objc_define_class("CALayer", nsobject, sizeof(layer_object), MM_FW_CLASS_REF(CALayer), MM_FW_META_REF(CALayer));
    register_lifecycle<layer_object>(g_layer);
    mm_objc_add_class_method(g_layer, "layer", MM_IMP(layerc_layer), "@16@0:8");
    MM_METHODS(g_layer,
        {"init", MM_IMP(layer_init), "@16@0:8"},
        {"bounds", MM_IMP(layer_bounds), "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8"},
        {"setBounds:", MM_IMP(layer_setBounds), "v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"frame", MM_IMP(layer_frame), "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8"},
        {"setFrame:", MM_IMP(layer_setFrame), "v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"position", MM_IMP(layer_position), "{CGPoint=dd}16@0:8"},
        {"setPosition:", MM_IMP(layer_setPosition), "v32@0:8{CGPoint=dd}16"},
        {"anchorPoint", MM_IMP(layer_anchorPoint), "{CGPoint=dd}16@0:8"},
        {"setAnchorPoint:", MM_IMP(layer_setAnchorPoint), "v32@0:8{CGPoint=dd}16"},
        {"contentsScale", MM_IMP(layer_contentsScale), "d16@0:8"},
        {"setContentsScale:", MM_IMP(layer_setContentsScale), "v24@0:8d16"},
        {"contentsGravity", MM_IMP(layer_contentsGravity), "@16@0:8"},
        {"setContentsGravity:", MM_IMP(layer_setContentsGravity), "v24@0:8@16"},
        {"setNeedsDisplay", MM_IMP(layer_setNeedsDisplay), "v16@0:8"},
        {"setNeedsDisplayInRect:", MM_IMP(layer_setNeedsDisplayInRect), "v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"needsDisplay", MM_IMP(layer_needsDisplay), "B16@0:8"},
        {"display", MM_IMP(layer_display), "v16@0:8"},
        {"displayIfNeeded", MM_IMP(layer_displayIfNeeded), "v16@0:8"},
        {"delegate", MM_IMP(layer_delegate), "@16@0:8"},
        {"setDelegate:", MM_IMP(layer_setDelegate), "v24@0:8@16"},
        {"superlayer", MM_IMP(layer_superlayer), "@16@0:8"},
        {"sublayers", MM_IMP(layer_sublayers), "@16@0:8"},
        {"addSublayer:", MM_IMP(layer_addSublayer), "v24@0:8@16"},
        {"removeFromSuperlayer", MM_IMP(layer_removeFromSuperlayer), "v16@0:8"},
        {"isOpaque", MM_IMP(layer_isOpaque), "B16@0:8"},
        {"setOpaque:", MM_IMP(layer_setOpaque), "v24@0:8B16"},
        {"isHidden", MM_IMP(layer_isHidden), "B16@0:8"},
        {"setHidden:", MM_IMP(layer_setHidden), "v24@0:8B16"},
        {"contents", MM_IMP(layer_contents), "@16@0:8"},
        {"setContents:", MM_IMP(layer_setContents), "v24@0:8@16"});
    /* Appearance and animation settings a layer accepts and this one has
     * nothing to do with; answering them keeps the program's setup code on
     * its normal path. */
    for (const char *sel : {"setBackgroundColor:", "setContentsFormat:", "setActions:",
                            "setMasksToBounds:", "setAutoresizingMask:", "setDrawsAsynchronously:", "setNeedsLayout",
                            "layoutIfNeeded", "setAllowsNextDrawableTimeout:", "setOpacity:", "setZPosition:",
                            "setEdgeAntialiasingMask:", "setGeometryFlipped:", "removeAllAnimations", "setMagnificationFilter:",
                            "setMinificationFilter:", "setShouldRasterize:", "setRasterizationScale:",
                            "setNeedsDisplayOnBoundsChange:", "needsDisplayOnBoundsChange"})
        mm_objc_add_method(g_layer, sel, MM_IMP(layer_noop), "v24@0:8@16");

    Class gl_layer = mm_objc_define_class("NSOpenGLLayer", g_layer, sizeof(layer_object),
                                          MM_FW_CLASS_REF(NSOpenGLLayer), MM_FW_META_REF(NSOpenGLLayer));
    MM_METHODS(gl_layer,
        {"openGLContext", MM_IMP(gllayer_openGLContext), "@16@0:8"},
        {"setOpenGLContext:", MM_IMP(gllayer_setOpenGLContext), "v24@0:8@16"},
        {"openGLPixelFormat", MM_IMP(gllayer_openGLPixelFormat), "@16@0:8"},
        {"setOpenGLPixelFormat:", MM_IMP(gllayer_setOpenGLPixelFormat), "v24@0:8@16"},
        {"view", MM_IMP(gllayer_view), "@16@0:8"},
        {"setView:", MM_IMP(gllayer_setView), "v24@0:8@16"},
        {"isAsynchronous", MM_IMP(gllayer_asynchronous), "B16@0:8"},
        {"setAsynchronous:", MM_IMP(gllayer_setAsynchronous), "v24@0:8B16"},
        {"openGLPixelFormatForDisplayMask:", MM_IMP(gllayer_pixelFormatForDisplayMask), "@24@0:8I16"},
        {"openGLContextForPixelFormat:", MM_IMP(gllayer_contextForPixelFormat), "@24@0:8@16"},
        {"canDrawInOpenGLContext:pixelFormat:forLayerTime:displayTime:", MM_IMP(gllayer_canDraw), "B48@0:8@16@24d32^v40"},
        {"drawInOpenGLContext:pixelFormat:forLayerTime:displayTime:", MM_IMP(gllayer_draw), "v48@0:8@16@24d32^v40"});

    /* CATransaction batches layer changes; there is no animation here, so
     * beginning and committing one is all it has to accept. */
    Class transaction = mm_objc_define_class("CATransaction", nsobject, sizeof(mm_objc_object),
                                             MM_FW_CLASS_REF(CATransaction), MM_FW_META_REF(CATransaction));
    for (const char *sel : {"begin", "commit", "flush", "setDisableActions:", "setAnimationDuration:",
                            "setValue:forKey:", "setCompletionBlock:"})
        mm_objc_add_class_method(transaction, sel, MM_IMP(layer_noop), "v24@0:8@16");

    Class metal_layer = mm_objc_define_class("CAMetalLayer", g_layer, sizeof(layer_object),
                                             MM_FW_CLASS_REF(CAMetalLayer), MM_FW_META_REF(CAMetalLayer));
    register_lifecycle<layer_object>(metal_layer);
    for (const char *sel : {"device", "setDevice:", "pixelFormat", "setPixelFormat:",
                            "framebufferOnly", "setFramebufferOnly:", "drawableSize", "setDrawableSize:",
                            "nextDrawable", "presentsWithTransaction", "setPresentsWithTransaction:"})
        mm_objc_add_method(metal_layer, sel, MM_IMP(layer_noop), "v24@0:8@16");
#ifdef MM_APPKIT_MEVK
    machoman_mevk_register_layer();
#endif
}

} // namespace ak

// Native bridge called after the Vulkan fence signals. Keep platform details
// in AppKit; MEVK only supplies a complete BGRA frame.
static int present_metal_layer(mm_id layer, const void *pixels,
        std::uint32_t width, std::uint32_t height, std::uint64_t stride, const void *gpu_texture) {
    using namespace ak;
    if (!layer || (!pixels && !gpu_texture) || !width || !height || stride < std::uint64_t{width} * 4 ||
        !is_kind(layer, cls("CAMetalLayer"))) return 0;
    auto *lo = as<layer_object>(layer);
    rect frame = lo->frame;
    id ancestor = layer;
    for (unsigned depth = 0; !as<layer_object>(ancestor)->view && depth < 128; ++depth) {
        id parent = as<layer_object>(ancestor)->superlayer;
        if (!parent) break;
        frame.x += as<layer_object>(parent)->frame.x - as<layer_object>(parent)->bounds.x;
        frame.y += as<layer_object>(parent)->frame.y - as<layer_object>(parent)->bounds.y;
        ancestor = parent;
    }
    id view = as<layer_object>(ancestor)->view;
    if (lo->hidden) { platform::release_metal(lo); return 1; }
    if (!view) return platform::available() ? 0 : 1;
    auto *vo = as<view_object>(view);
    id window = vo->window;
    if (!window) return platform::available() ? 0 : 1;
    if (as<window_object>(window)->alpha == 0) { platform::release_metal(lo); return 1; }
    for (unsigned depth = 0; view && depth < 128; ++depth) {
        auto *v = as<view_object>(view);
        if (v->hidden) { platform::release_metal(lo); return 1; }
        frame.x += v->frame.x - v->bounds.x;
        frame.y += v->frame.y - v->bounds.y;
        view = v->superview;
    }
    if (!std::isfinite(frame.x) || !std::isfinite(frame.y) || !std::isfinite(frame.w) || !std::isfinite(frame.h)) return 0;
    static std::atomic<unsigned> placement_traces{0};
    if (trace() && placement_traces.fetch_add(1, std::memory_order_relaxed) < 24)
        std::fprintf(stderr, "AppKit Metal: layer=%p pixels=%ux%u frame={%g,%g,%g,%g} window=%gx%g scale=%g gravity=%s\n",
            static_cast<void *>(layer), width, height, frame.x, frame.y, frame.w, frame.h,
            as<window_object>(window)->frame.w, as<window_object>(window)->frame.h,
            lo->contents_scale, gravity_names[lo->contents_placement]);
    ensure_platform_window(window);
#ifdef MM_APPKIT_MEVK
    if (gpu_texture) return platform::present_metal_texture(as<window_object>(window)->platform, lo, frame,
        *static_cast<const std::shared_ptr<mevk::Texture> *>(gpu_texture), lo->contents_scale, lo->contents_placement) ? 1 : -1;
#endif
    return platform::present_metal(as<window_object>(window)->platform, lo, frame, pixels, width, height, stride,
        lo->contents_scale, lo->contents_placement) ? 1 : 0;
}
extern "C" MM_FW_EXPORT int machoman_appkit_present_metal(mm_id layer, const void *pixels,
        std::uint32_t width, std::uint32_t height, std::uint64_t stride) {
    return present_metal_layer(layer, pixels, width, height, stride, nullptr);
}
extern "C" MM_FW_EXPORT double machoman_appkit_display_refresh_hz() { return ak::platform::screen_refresh(); }
#ifdef MM_APPKIT_MEVK
extern "C" MM_FW_EXPORT int machoman_appkit_present_metal_texture(mm_id layer, const void *texture) {
    if (!texture) return 0;
    const auto &t = *static_cast<const std::shared_ptr<mevk::Texture> *>(texture);
    if (!t) return 0;
    return present_metal_layer(layer, nullptr, t->width(), t->height(), std::uint64_t{t->width()} * 4, texture);
}
#endif

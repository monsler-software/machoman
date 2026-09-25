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
#include <cstdio>

MM_FW_CLASS(CALayer);
MM_FW_CLASS(CATransaction);

namespace ak {

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
std::uint64_t layer_setContentsScale(id self, SEL, double s) { l(self)->contents_scale = s > 0 ? s : 1.0; return 0; }

std::uint64_t layer_setNeedsDisplay(id self, SEL) { l(self)->needs_display = true; return 0; }
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
    if (id super = l(self)->superlayer) {
        auto &subs = l(super)->sublayers;
        for (auto it = subs.begin(); it != subs.end(); ++it)
            if (*it == self) { subs.erase(it); release(self); break; }
        l(self)->superlayer = nullptr;
    }
    return 0;
}

std::uint64_t layer_isOpaque(id self, SEL) { return l(self)->opaque ? 1 : 0; }
std::uint64_t layer_setOpaque(id self, SEL, std::uint64_t flag) { l(self)->opaque = (flag & 0xFF) != 0; return 0; }
std::uint64_t layer_isHidden(id self, SEL) { return l(self)->hidden ? 1 : 0; }
std::uint64_t layer_setHidden(id self, SEL, std::uint64_t flag) { l(self)->hidden = (flag & 0xFF) != 0; return 0; }
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
void draw_layer(id view, id layer) {
    layer_object *o = l(layer);
    static std::int64_t frame_number = 0;

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

    cv_time_stamp ts = frame_timestamp(frame_number++);
    const double layer_time = now_seconds();
    const std::uint64_t args[3] = {ret(context), ret(o->gl_format), ret(&ts)};
    if (!send_layer_time(layer, "canDrawInOpenGLContext:pixelFormat:forLayerTime:displayTime:", layer_time, args, 3))
        return;
    send_layer_time(layer, "drawInOpenGLContext:pixelFormat:forLayerTime:displayTime:", layer_time, args, 3);
    send(context, "flushBuffer");
    o->needs_display = false;
}

void draw_layers(id view) {
    if (!view) return;
    view_object *o = as<view_object>(view);
    if (o->layer && is_kind(o->layer, cls("NSOpenGLLayer"))) draw_layer(view, o->layer);
    for (id sub : o->subviews) draw_layers(sub);
}

/* The layer a view gets from -setWantsLayer:YES: its own, if it makes one. */
id make_backing_layer(id view) {
    id layer = responds(view, "makeBackingLayer") ? obj(send(view, "makeBackingLayer"))
                                                  : obj(send(reinterpret_cast<id>(g_layer), "layer"));
    if (!layer) return nullptr;
    retain(layer);
    if (responds(layer, "setView:")) send(layer, "setView:", ret(view));
    const rect &f = as<view_object>(view)->frame;
    if (mm_cpu_context *ctx = api()->current_context()) {
        const double values[4] = {0, 0, f.w, f.h};
        for (int i = 0; i < 4; i++) std::memcpy(&ctx->v[i][0], &values[i], 8);
    }
    send(layer, "setBounds:");
    if (trace())
        std::fprintf(stderr, "AppKit: %s is layer-backed by a %s\n", mm_objc_class_name(mm_objc_object_class(view)),
                     mm_objc_class_name(mm_objc_object_class(layer)));
    return layer;
}

void register_layers() {
    Class nsobject = cls("NSObject");

    g_layer = mm_objc_define_class("CALayer", nsobject, sizeof(layer_object), MM_FW_CLASS_REF(CALayer), MM_FW_META_REF(CALayer));
    register_lifecycle<layer_object>(g_layer);
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
    for (const char *sel : {"setBackgroundColor:", "setContentsGravity:", "setContentsFormat:", "setActions:",
                            "setMasksToBounds:", "setAutoresizingMask:", "setDrawsAsynchronously:", "setNeedsLayout",
                            "layoutIfNeeded", "setAllowsNextDrawableTimeout:", "setOpacity:", "setZPosition:",
                            "setEdgeAntialiasingMask:", "setGeometryFlipped:", "removeAllAnimations", "setMagnificationFilter:",
                            "setMinificationFilter:", "setShouldRasterize:", "setRasterizationScale:"})
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
}

} // namespace ak

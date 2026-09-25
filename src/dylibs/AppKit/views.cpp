/* NSView, NSWindow, NSViewController, NSGraphicsContext.
 *
 * Geometry follows AppKit: points, origin at the bottom left, a view's
 * frame in its superview's coordinates and its bounds in its own. A window
 * owns one platform window; its content view is the root of the view tree
 * and the next responder after the window. */
#include "appkit.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

MM_FW_CLASS(NSView);
MM_FW_CLASS(NSWindow);
MM_FW_CLASS(NSPanel);
MM_FW_CLASS(NSViewController);
MM_FW_CLASS(NSGraphicsContext);
MM_FW_CLASS(NSControl);
MM_FW_CLASS(NSButton);
MM_FW_CLASS(NSScroller);
MM_FW_CLASS(NSTextField);
MM_FW_CLASS(NSPopUpButton);

namespace ak {

namespace {

Class g_view, g_window, g_panel, g_view_controller, g_graphics_context;

view_object *v(id o) { return as<view_object>(o); }
window_object *w(id o) { return as<window_object>(o); }

/* ---- NSView -------------------------------------------------------------- */

std::uint64_t view_initWithFrame(id self, SEL, double x, double y, double width, double height) {
    v(self)->frame = {x, y, width, height};
    v(self)->bounds = {0, 0, width, height};
    return ret(self);
}
std::uint64_t view_init(id self, SEL) { return view_initWithFrame(self, nullptr, 0, 0, 0, 0); }

mm_two_doubles view_frame(id self, SEL) {
    const rect &r = v(self)->frame;
    mm_objc_return_doubles_hi(r.w, r.h);
    return {r.x, r.y};
}
mm_two_doubles view_bounds(id self, SEL) {
    const rect &r = v(self)->bounds;
    mm_objc_return_doubles_hi(r.w, r.h);
    return {r.x, r.y};
}
std::uint64_t view_setFrame(id self, SEL, double x, double y, double width, double height) {
    view_object *o = v(self);
    const bool resized = o->frame.w != width || o->frame.h != height;
    o->frame = {x, y, width, height};
    if (resized) {
        o->bounds.w = width;
        o->bounds.h = height;
        send(self, "setNeedsDisplay:", 1);
        if (responds(self, "frameDidChange")) send(self, "frameDidChange");
    }
    return 0;
}
std::uint64_t view_setFrameSize(id self, SEL, double width, double height) {
    const rect &r = v(self)->frame;
    return view_setFrame(self, nullptr, r.x, r.y, width, height);
}
std::uint64_t view_setFrameOrigin(id self, SEL, double x, double y) {
    const rect &r = v(self)->frame;
    return view_setFrame(self, nullptr, x, y, r.w, r.h);
}
std::uint64_t view_setBounds(id self, SEL, double x, double y, double width, double height) {
    v(self)->bounds = {x, y, width, height};
    return 0;
}

std::uint64_t view_superview(id self, SEL) { return ret(v(self)->superview); }
std::uint64_t view_window(id self, SEL) { return ret(v(self)->window); }
std::uint64_t view_subviews(id self, SEL) {
    id array = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
    for (id sub : v(self)->subviews) send(array, "addObject:", ret(sub));
    return ret(array);
}
/* A view's window is inherited by everything under it. */
void set_window_recursively(id view, id window) {
    v(view)->window = window;
    for (id sub : v(view)->subviews) set_window_recursively(sub, window);
}
std::uint64_t view_addSubview(id self, SEL, id sub) {
    if (!sub) return 0;
    view_object *o = v(self);
    if (std::find(o->subviews.begin(), o->subviews.end(), sub) != o->subviews.end()) return 0;
    o->subviews.push_back(retain(sub));
    v(sub)->superview = self;
    set_window_recursively(sub, o->window);
    send(sub, "setNextResponder:", ret(self));
    send(sub, "viewDidMoveToWindow");
    return 0;
}
std::uint64_t view_addSubviewPositioned(id self, SEL, id sub, std::uint64_t, id) { return view_addSubview(self, nullptr, sub); }
std::uint64_t view_removeFromSuperview(id self, SEL) {
    id super = v(self)->superview;
    if (!super) return 0;
    auto &subs = v(super)->subviews;
    subs.erase(std::remove(subs.begin(), subs.end(), self), subs.end());
    v(self)->superview = nullptr;
    set_window_recursively(self, nullptr);
    release(self);
    return 0;
}
std::uint64_t view_isDescendantOf(id self, SEL, id other) {
    for (id c = self; c; c = v(c)->superview)
        if (c == other) return 1;
    return 0;
}

std::uint64_t view_setNeedsDisplay(id self, SEL, std::uint64_t flag) {
    v(self)->needs_display = (flag & 0xFF) != 0;
    if (v(self)->needs_display && v(self)->window) w(v(self)->window)->needs_display = true;
    return 0;
}
std::uint64_t view_setNeedsDisplayInRect(id self, SEL, double, double, double, double) {
    return view_setNeedsDisplay(self, nullptr, 1);
}
std::uint64_t view_needsDisplay(id self, SEL) { return v(self)->needs_display; }
std::uint64_t view_drawRect(id, SEL, double, double, double, double) { return 0; }
std::uint64_t view_display(id self, SEL) {
    const rect &b = v(self)->bounds;
    const std::uint64_t args[4] = {0, 0, 0, 0};
    (void)args;
    v(self)->needs_display = false;
    /* -drawRect: takes an NSRect: four doubles in v0-v3. */
    mm_cpu_context *ctx = api()->current_context();
    if (ctx) {
        std::memcpy(&ctx->v[0][0], &b.x, 8);
        std::memcpy(&ctx->v[1][0], &b.y, 8);
        std::memcpy(&ctx->v[2][0], &b.w, 8);
        std::memcpy(&ctx->v[3][0], &b.h, 8);
    }
    send(self, "drawRect:");
    for (id sub : v(self)->subviews) send(sub, "display");
    return 0;
}
std::uint64_t view_setHidden(id self, SEL, std::uint64_t flag) { v(self)->hidden = (flag & 0xFF) != 0; return 0; }
std::uint64_t view_isHidden(id self, SEL) { return v(self)->hidden; }
std::uint64_t view_setWantsLayer(id self, SEL, std::uint64_t flag) { v(self)->wants_layer = (flag & 0xFF) != 0; return 0; }
std::uint64_t view_wantsLayer(id self, SEL) { return v(self)->wants_layer; }
std::uint64_t view_layer(id self, SEL) { return ret(v(self)->layer); }
std::uint64_t view_setLayer(id self, SEL, id layer) { v(self)->layer = layer; return 0; }
std::uint64_t view_isFlipped(id, SEL) { return 0; }
std::uint64_t view_isOpaque(id, SEL) { return 1; }
std::uint64_t view_acceptsFirstResponder(id, SEL) { return 1; }
std::uint64_t view_viewDidMoveToWindow(id, SEL) { return 0; }
std::uint64_t view_noop(id, SEL, ...) { return 0; }

/* Coordinate conversion: only translations are involved so far, because
 * nothing scales or rotates a view yet. */
point view_origin_in_window(id view) {
    point p{0, 0};
    for (id c = view; c; c = v(c)->superview) {
        p.x += v(c)->frame.x;
        p.y += v(c)->frame.y;
    }
    return p;
}
mm_two_doubles view_convertPointFromView(id self, SEL, double x, double y, id from) {
    const point a = from ? view_origin_in_window(from) : point{0, 0};
    const point b = view_origin_in_window(self);
    return {x + a.x - b.x, y + a.y - b.y};
}
mm_two_doubles view_convertPointToView(id self, SEL, double x, double y, id to) {
    const point a = view_origin_in_window(self);
    const point b = to ? view_origin_in_window(to) : point{0, 0};
    return {x + a.x - b.x, y + a.y - b.y};
}
mm_two_doubles view_convertRectFromView(id self, SEL, double x, double y, double width, double height, id from) {
    const mm_two_doubles o = view_convertPointFromView(self, nullptr, x, y, from);
    mm_objc_return_doubles_hi(width, height);
    return o;
}
mm_two_doubles view_convertRectToView(id self, SEL, double x, double y, double width, double height, id to) {
    const mm_two_doubles o = view_convertPointToView(self, nullptr, x, y, to);
    mm_objc_return_doubles_hi(width, height);
    return o;
}

/* Hit testing walks the subviews back to front, like AppKit. */
std::uint64_t view_hitTest(id self, SEL, double x, double y) {
    view_object *o = v(self);
    if (o->hidden) return 0;
    const double lx = x - o->frame.x, ly = y - o->frame.y;
    if (lx < 0 || ly < 0 || lx >= o->frame.w || ly >= o->frame.h) return 0;
    for (auto it = o->subviews.rbegin(); it != o->subviews.rend(); ++it) {
        const std::uint64_t args[2] = {0, 0};
        (void)args;
        mm_cpu_context *ctx = api()->current_context();
        if (ctx) { std::memcpy(&ctx->v[0][0], &lx, 8); std::memcpy(&ctx->v[1][0], &ly, 8); }
        if (const std::uint64_t hit = send(*it, "hitTest:")) return hit;
    }
    return ret(self);
}

std::uint64_t view_tag(id self, SEL) { return static_cast<std::uint64_t>(v(self)->tag); }
std::uint64_t view_setTag(id self, SEL, std::int64_t tag) { v(self)->tag = tag; return 0; }
/* -viewWithTag: searches the subtree, as AppKit's does. */
std::uint64_t view_viewWithTag(id self, SEL, std::int64_t tag) {
    if (v(self)->tag == tag) return ret(self);
    for (id sub : v(self)->subviews)
        if (std::uint64_t found = send(sub, "viewWithTag:", static_cast<std::uint64_t>(tag))) return found;
    return 0;
}

} // namespace

id make_window(const rect &frame, std::uint64_t style) {
    id window = obj(send(reinterpret_cast<id>(g_window), "alloc"));
    if (!window) return nullptr;
    window_object *o = w(window);
    o->frame = frame;
    o->style = style;
    id content = obj(send(reinterpret_cast<id>(g_view), "alloc"));
    v(content)->frame = {0, 0, frame.w, frame.h};
    v(content)->bounds = {0, 0, frame.w, frame.h};
    o->content_view = content;
    v(content)->window = window;
    send(content, "setNextResponder:", ret(window));
    application_add_window(window);
    return window;
}

namespace {

/* ---- NSControl ----------------------------------------------------------- */

/* A control is a view plus what a control adds: a title or string value,
 * a target/action pair and an on/off state. */
struct control_object {
    view_object view;
    id title = nullptr, key_equivalent = nullptr, target = nullptr;
    SEL action = nullptr;
    std::int64_t state = 0;
    bool enabled = true;
};
control_object *control_of(id o) { return as<control_object>(o); }

std::uint64_t control_title(id self, SEL) { return ret(control_of(self)->title); }
std::uint64_t control_setTitle(id self, SEL, id title) {
    control_object *c = control_of(self);
    if (c->title) release(c->title);
    c->title = title ? retain(title) : nullptr;
    return 0;
}
std::uint64_t control_target(id self, SEL) { return ret(control_of(self)->target); }
std::uint64_t control_setTarget(id self, SEL, id target) { control_of(self)->target = target; return 0; }
std::uint64_t control_action(id self, SEL) { return reinterpret_cast<std::uintptr_t>(control_of(self)->action); }
std::uint64_t control_setAction(id self, SEL, SEL action) { control_of(self)->action = action; return 0; }
std::uint64_t control_state(id self, SEL) { return static_cast<std::uint64_t>(control_of(self)->state); }
std::uint64_t control_setState(id self, SEL, std::int64_t state) { control_of(self)->state = state; return 0; }
std::uint64_t control_isEnabled(id self, SEL) { return control_of(self)->enabled ? 1 : 0; }
std::uint64_t control_setEnabled(id self, SEL, std::uint64_t enabled) { control_of(self)->enabled = (enabled & 0xFF) != 0; return 0; }
std::uint64_t control_keyEquivalent(id self, SEL) { return ret(control_of(self)->key_equivalent); }
std::uint64_t control_setKeyEquivalent(id self, SEL, id key) {
    control_object *c = control_of(self);
    if (c->key_equivalent) release(c->key_equivalent);
    c->key_equivalent = key ? retain(key) : nullptr;
    return 0;
}
/* Appearance the platform backend does not draw: accepted and ignored,
 * because a program configures a control before it uses it. */
std::uint64_t control_ignore(id, SEL, std::uint64_t) { return 0; }

/* Clicking sends the action to the target, which is what a control does. */
std::uint64_t control_performClick(id self, SEL, id) {
    control_object *c = control_of(self);
    if (c->target && c->action) {
        const std::uint64_t arg = ret(self);
        mm_objc_send(c->target, c->action, 1, &arg);
    }
    return 0;
}

/* ---- NSWindow ------------------------------------------------------------ */

std::uint64_t window_initWithContentRect(id self, SEL, double x, double y, double width, double height,
                                         std::uint64_t style, std::uint64_t, std::uint64_t) {
    window_object *o = w(self);
    o->frame = {x, y, width, height};
    o->style = style;
    if (trace())
        std::fprintf(stderr, "AppKit: -[NSWindow initWithContentRect:] %gx%g at %g,%g style 0x%llx\n",
                     width, height, x, y, static_cast<unsigned long long>(style));
    /* The content view exists from the start, as it does on macOS. */
    id content = obj(send(reinterpret_cast<id>(g_view), "alloc"));
    mm_cpu_context *ctx = api()->current_context();
    if (ctx) {
        const double zero = 0;
        std::memcpy(&ctx->v[0][0], &zero, 8);
        std::memcpy(&ctx->v[1][0], &zero, 8);
        std::memcpy(&ctx->v[2][0], &width, 8);
        std::memcpy(&ctx->v[3][0], &height, 8);
    }
    content = obj(send(content, "initWithFrame:"));
    o->content_view = content;
    v(content)->window = self;
    send(content, "setNextResponder:", ret(self));
    application_add_window(self);
    return ret(self);
}

std::uint64_t window_contentView(id self, SEL) { return ret(w(self)->content_view); }
std::uint64_t window_setContentView(id self, SEL, id view) {
    window_object *o = w(self);
    if (o->content_view == view) return 0;
    if (o->content_view) release(o->content_view);
    o->content_view = view ? retain(view) : nullptr;
    if (view) {
        set_window_recursively(view, self);
        send(view, "setNextResponder:", ret(self));
        const rect &f = o->frame;
        view_setFrame(view, nullptr, 0, 0, f.w, f.h);
    }
    return 0;
}
mm_two_doubles window_frame(id self, SEL) {
    const rect &r = w(self)->frame;
    mm_objc_return_doubles_hi(r.w, r.h);
    return {r.x, r.y};
}
std::uint64_t window_setFrame(id self, SEL, double x, double y, double width, double height, std::uint64_t display) {
    window_object *o = w(self);
    o->frame = {x, y, width, height};
    if (o->platform) platform::set_frame(o->platform, o->frame);
    if (o->content_view) view_setFrame(o->content_view, nullptr, 0, 0, width, height);
    if (display & 0xFF) send(self, "display");
    return 0;
}
std::uint64_t window_setFrameDisplayAnimate(id self, SEL, double x, double y, double width, double height,
                                            std::uint64_t display, std::uint64_t) {
    return window_setFrame(self, nullptr, x, y, width, height, display);
}
mm_two_doubles window_contentRectForFrameRect(id, SEL, double x, double y, double width, double height) {
    mm_objc_return_doubles_hi(width, height);
    return {x, y};
}
std::uint64_t window_title(id self, SEL) { return ret(string(w(self)->title)); }
std::uint64_t window_setTitle(id self, SEL, id title) {
    window_object *o = w(self);
    o->title = to_utf8(title);
    if (o->platform) platform::set_title(o->platform, o->title);
    return 0;
}

/* Showing a window is when the platform window appears. */
void ensure_platform_window(id self) {
    window_object *o = w(self);
    if (o->platform || !platform::available()) return;
    const bool decorated = (o->style & 1) != 0; /* NSWindowStyleMaskTitled */
    o->platform = platform::create_window(self, o->frame, o->title, decorated);
}
std::uint64_t window_makeKeyAndOrderFront(id self, SEL, id) {
    ensure_platform_window(self);
    window_object *o = w(self);
    o->visible = true;
    if (o->platform) platform::show_window(o->platform, true);
    send(self, "becomeKeyWindow");
    return 0;
}
std::uint64_t window_orderFront(id self, SEL, id) { return window_makeKeyAndOrderFront(self, nullptr, nullptr); }
std::uint64_t window_orderOut(id self, SEL, id) {
    window_object *o = w(self);
    o->visible = false;
    if (o->platform) platform::show_window(o->platform, false);
    return 0;
}
std::uint64_t window_close(id self, SEL) {
    window_object *o = w(self);
    o->visible = false;
    if (o->platform) { platform::destroy_window(o->platform); o->platform = nullptr; }
    id delegate = o->delegate;
    if (responds(delegate, "windowWillClose:")) send(delegate, "windowWillClose:", ret(self));
    application_remove_window(self);
    return 0;
}
std::uint64_t window_isVisible(id self, SEL) { return w(self)->visible; }
std::uint64_t window_isKeyWindow(id self, SEL) { return w(self)->key; }
std::uint64_t window_isMainWindow(id self, SEL) { return w(self)->key; }
std::uint64_t window_becomeKeyWindow(id self, SEL) { w(self)->key = true; return 0; }
std::uint64_t window_resignKeyWindow(id self, SEL) { w(self)->key = false; return 0; }
std::uint64_t window_makeFirstResponder(id self, SEL, id responder) {
    window_object *o = w(self);
    if (o->first_responder == responder) return 1;
    if (o->first_responder) send(o->first_responder, "resignFirstResponder");
    o->first_responder = responder;
    if (responder) send(responder, "becomeFirstResponder");
    return 1;
}
std::uint64_t window_firstResponder(id self, SEL) { return ret(w(self)->first_responder); }
std::uint64_t window_delegate(id self, SEL) { return ret(w(self)->delegate); }
std::uint64_t window_setDelegate(id self, SEL, id d) { w(self)->delegate = d; return 0; }
std::uint64_t window_display(id self, SEL) {
    window_object *o = w(self);
    o->needs_display = false;
    if (o->content_view) send(o->content_view, "display");
    return 0;
}
std::uint64_t window_setAcceptsMouseMovedEvents(id self, SEL, std::uint64_t flag) {
    w(self)->accepts_mouse_moved = (flag & 0xFF) != 0;
    return 0;
}
std::uint64_t window_styleMask(id self, SEL) { return w(self)->style; }
std::uint64_t window_setStyleMask(id self, SEL, std::uint64_t mask) { w(self)->style = mask; return 0; }
double window_backingScaleFactor(id, SEL) { return platform::screen_scale(); }
mm_two_doubles window_convertPointToScreen(id self, SEL, double x, double y) {
    const rect &f = w(self)->frame;
    return {x + f.x, y + f.y};
}
mm_two_doubles window_convertPointFromScreen(id self, SEL, double x, double y) {
    const rect &f = w(self)->frame;
    return {x - f.x, y - f.y};
}
std::uint64_t window_screen(id, SEL) { return send(reinterpret_cast<id>(cls("NSScreen")), "mainScreen"); }
std::uint64_t window_sendEvent(id self, SEL, id event) {
    /* Route to the view under the pointer, or the first responder. */
    window_object *o = w(self);
    const std::uint64_t type = send(event, "type");
    id target = o->first_responder;
    if (o->content_view && (type == ev_left_mouse_down || type == ev_right_mouse_down || type == ev_other_mouse_down ||
                            type == ev_scroll_wheel || type == ev_mouse_moved)) {
        auto *e = as<event_object>(event);
        mm_cpu_context *ctx = api()->current_context();
        if (ctx) { std::memcpy(&ctx->v[0][0], &e->where.x, 8); std::memcpy(&ctx->v[1][0], &e->where.y, 8); }
        if (const std::uint64_t hit = send(o->content_view, "hitTest:")) target = obj(hit);
    }
    if (!target) return 0;
    static const struct { std::uint64_t type; const char *sel; } routes[] = {
        {ev_left_mouse_down, "mouseDown:"},      {ev_left_mouse_up, "mouseUp:"},
        {ev_left_mouse_dragged, "mouseDragged:"}, {ev_right_mouse_down, "rightMouseDown:"},
        {ev_right_mouse_up, "rightMouseUp:"},     {ev_right_mouse_dragged, "rightMouseDragged:"},
        {ev_other_mouse_down, "otherMouseDown:"}, {ev_other_mouse_up, "otherMouseUp:"},
        {ev_other_mouse_dragged, "otherMouseDragged:"}, {ev_mouse_moved, "mouseMoved:"},
        {ev_scroll_wheel, "scrollWheel:"},        {ev_key_down, "keyDown:"},
        {ev_key_up, "keyUp:"},                    {ev_flags_changed, "flagsChanged:"},
    };
    for (const auto &r : routes)
        if (r.type == type) return send(target, r.sel, ret(event));
    return 0;
}

/* ---- NSViewController ----------------------------------------------------- */

std::uint64_t vc_init(id self, SEL) { return ret(self); }
std::uint64_t vc_view(id self, SEL) {
    auto *o = as<view_controller_object>(self);
    if (!o->view) {
        send(self, "loadView");
        if (!o->view) {
            id view = obj(send(reinterpret_cast<id>(g_view), "alloc"));
            o->view = obj(send(view, "init"));
        }
    }
    return ret(o->view);
}
std::uint64_t vc_setView(id self, SEL, id view) { as<view_controller_object>(self)->view = view ? retain(view) : nullptr; return 0; }
std::uint64_t vc_loadView(id, SEL) { return 0; }

/* ---- NSGraphicsContext ----------------------------------------------------- */

std::uint64_t gc_currentContext(id, SEL) { return ret(g_current_graphics_context); }
std::uint64_t gc_setCurrentContext(id, SEL, id ctx) { g_current_graphics_context = ctx; return 0; }
std::uint64_t gc_saveGraphicsState(id, SEL) { return 0; }
std::uint64_t gc_restoreGraphicsState(id, SEL) { return 0; }
std::uint64_t gc_flushGraphics(id, SEL) { return 0; }
std::uint64_t gc_CGContext(id, SEL) { return 0; }

} // namespace

id g_current_graphics_context = nullptr;

void register_views() {
    Class responder = cls("NSResponder");
    Class nsobject = cls("NSObject");

    g_view = mm_objc_define_class("NSView", responder, sizeof(view_object), MM_FW_CLASS_REF(NSView), MM_FW_META_REF(NSView));
    register_lifecycle<view_object>(g_view);
    MM_METHODS(g_view,
        {"init", MM_IMP(view_init), "@16@0:8"},
        {"initWithFrame:", MM_IMP(view_initWithFrame), "@48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"frame", MM_IMP(view_frame), "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8"},
        {"bounds", MM_IMP(view_bounds), "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8"},
        {"setFrame:", MM_IMP(view_setFrame), "v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"setFrameSize:", MM_IMP(view_setFrameSize), "v32@0:8{CGSize=dd}16"},
        {"setFrameOrigin:", MM_IMP(view_setFrameOrigin), "v32@0:8{CGPoint=dd}16"},
        {"setBounds:", MM_IMP(view_setBounds), "v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"superview", MM_IMP(view_superview), "@16@0:8"},
        {"window", MM_IMP(view_window), "@16@0:8"},
        {"subviews", MM_IMP(view_subviews), "@16@0:8"},
        {"addSubview:", MM_IMP(view_addSubview), "v24@0:8@16"},
        {"addSubview:positioned:relativeTo:", MM_IMP(view_addSubviewPositioned), "v40@0:8@16q24@32"},
        {"removeFromSuperview", MM_IMP(view_removeFromSuperview), "v16@0:8"},
        {"removeFromSuperviewWithoutNeedingDisplay", MM_IMP(view_removeFromSuperview), "v16@0:8"},
        {"isDescendantOf:", MM_IMP(view_isDescendantOf), "B24@0:8@16"},
        {"setNeedsDisplay:", MM_IMP(view_setNeedsDisplay), "v24@0:8B16"},
        {"tag", MM_IMP(view_tag), "q16@0:8"},
        {"setNeedsLayout:", MM_IMP(view_setNeedsDisplay), "v24@0:8B16"},
        {"setTag:", MM_IMP(view_setTag), "v24@0:8q16"},
        {"viewWithTag:", MM_IMP(view_viewWithTag), "@24@0:8q16"},
        {"setNeedsDisplayInRect:", MM_IMP(view_setNeedsDisplayInRect), "v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"needsDisplay", MM_IMP(view_needsDisplay), "B16@0:8"},
        {"drawRect:", MM_IMP(view_drawRect), "v48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"display", MM_IMP(view_display), "v16@0:8"},
        {"displayIfNeeded", MM_IMP(view_display), "v16@0:8"},
        {"setHidden:", MM_IMP(view_setHidden), "v24@0:8B16"},
        {"isHidden", MM_IMP(view_isHidden), "B16@0:8"},
        {"setWantsLayer:", MM_IMP(view_setWantsLayer), "v24@0:8B16"},
        {"wantsLayer", MM_IMP(view_wantsLayer), "B16@0:8"},
        {"layer", MM_IMP(view_layer), "@16@0:8"},
        {"setLayer:", MM_IMP(view_setLayer), "v24@0:8@16"},
        {"isFlipped", MM_IMP(view_isFlipped), "B16@0:8"},
        {"isOpaque", MM_IMP(view_isOpaque), "B16@0:8"},
        {"acceptsFirstResponder", MM_IMP(view_acceptsFirstResponder), "B16@0:8"},
        {"viewDidMoveToWindow", MM_IMP(view_viewDidMoveToWindow), "v16@0:8"},
        {"hitTest:", MM_IMP(view_hitTest), "@32@0:8{CGPoint=dd}16"},
        {"convertPoint:fromView:", MM_IMP(view_convertPointFromView), "{CGPoint=dd}40@0:8{CGPoint=dd}16@32"},
        {"convertPoint:toView:", MM_IMP(view_convertPointToView), "{CGPoint=dd}40@0:8{CGPoint=dd}16@32"},
        {"convertRect:fromView:", MM_IMP(view_convertRectFromView), "{CGRect={CGPoint=dd}{CGSize=dd}}56@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16@48"},
        {"convertRect:toView:", MM_IMP(view_convertRectToView), "{CGRect={CGPoint=dd}{CGSize=dd}}56@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16@48"});
    for (const char *sel : {"setAutoresizingMask:", "setAutoresizesSubviews:", "setTranslatesAutoresizingMaskIntoConstraints:",
                            "updateTrackingAreas", "addTrackingArea:", "removeTrackingArea:", "setToolTip:",
                            "registerForDraggedTypes:", "unregisterDraggedTypes", "setAlphaValue:", "invalidateIntrinsicContentSize",
                            "layout", "setPostsFrameChangedNotifications:", "setPostsBoundsChangedNotifications:",
                            "setCanDrawConcurrently:", "lockFocus", "unlockFocus", "setAcceptsTouchEvents:"})
        mm_objc_add_method(g_view, sel, MM_IMP(view_noop), "v24@0:8@16");

    g_window = mm_objc_define_class("NSWindow", responder, sizeof(window_object), MM_FW_CLASS_REF(NSWindow), MM_FW_META_REF(NSWindow));
    register_lifecycle<window_object>(g_window);
    MM_METHODS(g_window,
        {"initWithContentRect:styleMask:backing:defer:", MM_IMP(window_initWithContentRect), "@72@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16Q48Q56B64"},
        {"contentView", MM_IMP(window_contentView), "@16@0:8"},
        {"setContentView:", MM_IMP(window_setContentView), "v24@0:8@16"},
        {"frame", MM_IMP(window_frame), "{CGRect={CGPoint=dd}{CGSize=dd}}16@0:8"},
        {"setFrame:display:", MM_IMP(window_setFrame), "v56@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16B48"},
        {"setFrame:display:animate:", MM_IMP(window_setFrameDisplayAnimate), "v64@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16B48B56"},
        {"contentRectForFrameRect:", MM_IMP(window_contentRectForFrameRect), "{CGRect={CGPoint=dd}{CGSize=dd}}48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"frameRectForContentRect:", MM_IMP(window_contentRectForFrameRect), "{CGRect={CGPoint=dd}{CGSize=dd}}48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"title", MM_IMP(window_title), "@16@0:8"},
        {"setTitle:", MM_IMP(window_setTitle), "v24@0:8@16"},
        {"makeKeyAndOrderFront:", MM_IMP(window_makeKeyAndOrderFront), "v24@0:8@16"},
        {"makeKeyWindow", MM_IMP(window_becomeKeyWindow), "v16@0:8"},
        {"orderFront:", MM_IMP(window_orderFront), "v24@0:8@16"},
        {"orderFrontRegardless", MM_IMP(window_orderFront), "v16@0:8"},
        {"orderOut:", MM_IMP(window_orderOut), "v24@0:8@16"},
        {"close", MM_IMP(window_close), "v16@0:8"},
        {"performClose:", MM_IMP(window_close), "v24@0:8@16"},
        {"isVisible", MM_IMP(window_isVisible), "B16@0:8"},
        {"isKeyWindow", MM_IMP(window_isKeyWindow), "B16@0:8"},
        {"isMainWindow", MM_IMP(window_isMainWindow), "B16@0:8"},
        {"becomeKeyWindow", MM_IMP(window_becomeKeyWindow), "v16@0:8"},
        {"resignKeyWindow", MM_IMP(window_resignKeyWindow), "v16@0:8"},
        {"makeFirstResponder:", MM_IMP(window_makeFirstResponder), "B24@0:8@16"},
        {"firstResponder", MM_IMP(window_firstResponder), "@16@0:8"},
        {"delegate", MM_IMP(window_delegate), "@16@0:8"},
        {"setDelegate:", MM_IMP(window_setDelegate), "v24@0:8@16"},
        {"display", MM_IMP(window_display), "v16@0:8"},
        {"displayIfNeeded", MM_IMP(window_display), "v16@0:8"},
        {"setAcceptsMouseMovedEvents:", MM_IMP(window_setAcceptsMouseMovedEvents), "v24@0:8B16"},
        {"styleMask", MM_IMP(window_styleMask), "Q16@0:8"},
        {"setStyleMask:", MM_IMP(window_setStyleMask), "v24@0:8Q16"},
        {"backingScaleFactor", MM_IMP(window_backingScaleFactor), "d16@0:8"},
        {"convertPointToScreen:", MM_IMP(window_convertPointToScreen), "{CGPoint=dd}32@0:8{CGPoint=dd}16"},
        {"convertPointFromScreen:", MM_IMP(window_convertPointFromScreen), "{CGPoint=dd}32@0:8{CGPoint=dd}16"},
        {"screen", MM_IMP(window_screen), "@16@0:8"},
        {"sendEvent:", MM_IMP(window_sendEvent), "v24@0:8@16"});
    /* Layer-backing and high-DPI knobs: this draws at one scale into one
     * surface, so they are accepted and change nothing. */
    for (const char *sel : {"setLayerContentsPlacement:", "setLayerContentsRedrawPolicy:",
                            "setWantsBestResolutionOpenGLSurface:", "setCanDrawConcurrently:",
                            "setAutoresizingMask:", "setAutoresizesSubviews:", "setToolTip:",
                            "addTrackingArea:", "removeTrackingArea:", "updateTrackingAreas",
                            "setPostsFrameChangedNotifications:", "setPostsBoundsChangedNotifications:"})
        mm_objc_add_method(g_view, sel, MM_IMP(view_noop), "v24@0:8@16");

    for (const char *sel : {"setOpaque:", "setBackgroundColor:", "setReleasedWhenClosed:", "setLevel:", "setMinSize:",
                            "setMaxSize:", "setContentMinSize:", "setContentMaxSize:", "center", "setRestorable:",
                            "setCollectionBehavior:", "setTitlebarAppearsTransparent:", "setMovableByWindowBackground:",
                            "invalidateShadow", "setViewsNeedDisplay:", "setAllowsConcurrentViewDrawing:",
                            "toggleFullScreen:", "zoom:", "miniaturize:", "deminiaturize:", "setAppearance:",
                            /* Appearance and behaviour the Wayland backend does not model: a
                             * window is a surface with a size here, so these are accepted and
                             * have no effect, the way they do on a system without shadows or
                             * window levels. */
                            "setHasShadow:", "setAlphaValue:", "setIgnoresMouseEvents:", "setCanHide:",
                            "setHidesOnDeactivate:", "setAnimationBehavior:", "setFrameAutosaveName:",
                            "setExcludedFromWindowsMenu:", "setShowsResizeIndicator:", "setAutodisplay:",
                            "setPreservesContentDuringLiveResize:", "setDisplaysWhenScreenProfileChanges:",
                            "useOptimizedDrawing:", "setTabbingMode:", "setTitleVisibility:", "setToolbar:",
                            "setInitialFirstResponder:", "setContentSize:", "setAspectRatio:",
                            "setResizeIncrements:", "setDocumentEdited:", "setRepresentedFilename:",
                            "disableCursorRects", "enableCursorRects", "resetCursorRects",
                            "setAcceptsMouseMovedEvents:", "setOneShot:", "setDepthLimit:",
                            "registerForDraggedTypes:", "unregisterDraggedTypes", "setAllowsToolTips:"})
        mm_objc_add_method(g_window, sel, MM_IMP(view_noop), "v24@0:8@16");
    /* The matching getters report the defaults a plain window has. */
    static auto window_yes = [](id, SEL) -> std::uint64_t { return 1; };
    static auto window_no = [](id, SEL) -> std::uint64_t { return 0; };
    static auto window_alpha = [](id, SEL) -> std::uint64_t { const double one = 1.0; mm_objc_return_doubles_hi(0, 0); std::uint64_t bits; std::memcpy(&bits, &one, 8); return bits; };
    (void)window_alpha;
    MM_METHODS(g_window,
        {"hasShadow", MM_IMP(+window_yes), "B16@0:8"},
        {"isOpaque", MM_IMP(+window_yes), "B16@0:8"},
        {"isZoomed", MM_IMP(+window_no), "B16@0:8"},
        {"isMiniaturized", MM_IMP(+window_no), "B16@0:8"},
        {"isSheet", MM_IMP(+window_no), "B16@0:8"},
        {"isDocumentEdited", MM_IMP(+window_no), "B16@0:8"},
        {"ignoresMouseEvents", MM_IMP(+window_no), "B16@0:8"},
        {"isMovableByWindowBackground", MM_IMP(+window_no), "B16@0:8"},
        {"canBecomeKeyWindow", MM_IMP(+window_yes), "B16@0:8"},
        {"canBecomeMainWindow", MM_IMP(+window_yes), "B16@0:8"},
        {"level", MM_IMP(+window_no), "q16@0:8"},
        {"collectionBehavior", MM_IMP(+window_no), "Q16@0:8"});
    for (const char *sel : {"setColorSpace:", "setSharingType:", "setBackingType:"})
        mm_objc_add_method(g_window, sel, MM_IMP(view_noop), "v24@0:8@16");

    g_panel = mm_objc_define_class("NSPanel", g_window, sizeof(window_object), MM_FW_CLASS_REF(NSPanel), MM_FW_META_REF(NSPanel));

    g_view_controller = mm_objc_define_class("NSViewController", responder, sizeof(view_controller_object),
                                             MM_FW_CLASS_REF(NSViewController), MM_FW_META_REF(NSViewController));
    register_lifecycle<view_controller_object>(g_view_controller);
    MM_METHODS(g_view_controller,
        {"init", MM_IMP(vc_init), "@16@0:8"},
        {"view", MM_IMP(vc_view), "@16@0:8"},
        {"setView:", MM_IMP(vc_setView), "v24@0:8@16"},
        {"loadView", MM_IMP(vc_loadView), "v16@0:8"});

    g_graphics_context = mm_objc_define_class("NSGraphicsContext", nsobject, sizeof(mm_objc_object),
                                              MM_FW_CLASS_REF(NSGraphicsContext), MM_FW_META_REF(NSGraphicsContext));
    MM_METHODS(g_graphics_context,
        {"saveGraphicsState", MM_IMP(gc_saveGraphicsState), "v16@0:8"},
        {"restoreGraphicsState", MM_IMP(gc_restoreGraphicsState), "v16@0:8"},
        {"flushGraphics", MM_IMP(gc_flushGraphics), "v16@0:8"},
        {"CGContext", MM_IMP(gc_CGContext), "^v16@0:8"},
        {"graphicsPort", MM_IMP(gc_CGContext), "^v16@0:8"});
    MM_CLASS_METHODS(g_graphics_context,
        {"currentContext", MM_IMP(gc_currentContext), "@16@0:8"},
        {"setCurrentContext:", MM_IMP(gc_setCurrentContext), "v24@0:8@16"},
        {"saveGraphicsState", MM_IMP(gc_saveGraphicsState), "v16@0:8"},
        {"restoreGraphicsState", MM_IMP(gc_restoreGraphicsState), "v16@0:8"});

    /* NSControl is a view with a value, a title and a target/action pair;
     * NSButton and NSTextField are the two an alert or a dialog builds
     * from. They keep the view payload, so every NSView method still
     * works on them. */
    Class control = mm_objc_define_class("NSControl", g_view, sizeof(control_object),
                                         MM_FW_CLASS_REF(NSControl), MM_FW_META_REF(NSControl));
    register_lifecycle<control_object>(control);
    MM_METHODS(control,
        {"title", MM_IMP(control_title), "@16@0:8"},
        {"setTitle:", MM_IMP(control_setTitle), "v24@0:8@16"},
        {"stringValue", MM_IMP(control_title), "@16@0:8"},
        {"setStringValue:", MM_IMP(control_setTitle), "v24@0:8@16"},
        {"target", MM_IMP(control_target), "@16@0:8"},
        {"setTarget:", MM_IMP(control_setTarget), "v24@0:8@16"},
        {"action", MM_IMP(control_action), ":16@0:8"},
        {"setAction:", MM_IMP(control_setAction), "v24@0:8:16"},
        {"state", MM_IMP(control_state), "q16@0:8"},
        {"setState:", MM_IMP(control_setState), "v24@0:8q16"},
        {"isEnabled", MM_IMP(control_isEnabled), "B16@0:8"},
        {"setEnabled:", MM_IMP(control_setEnabled), "v24@0:8B16"},
        {"keyEquivalent", MM_IMP(control_keyEquivalent), "@16@0:8"},
        {"setKeyEquivalent:", MM_IMP(control_setKeyEquivalent), "v24@0:8@16"},
        {"performClick:", MM_IMP(control_performClick), "v24@0:8@16"});
    for (const char *sel : {"setBezelStyle:", "setButtonType:", "setBordered:", "setImagePosition:",
                            "setFont:", "setAlignment:", "setEditable:", "setSelectable:", "setBezeled:",
                            "setDrawsBackground:", "sizeToFit", "setContinuous:", "setRefusesFirstResponder:"})
        mm_objc_add_method(control, sel, MM_IMP(control_ignore), "v24@0:8q16");
    mm_objc_define_class("NSButton", control, sizeof(control_object), MM_FW_CLASS_REF(NSButton), MM_FW_META_REF(NSButton));
    mm_objc_define_class("NSTextField", control, sizeof(control_object), MM_FW_CLASS_REF(NSTextField), MM_FW_META_REF(NSTextField));
    mm_objc_define_class("NSPopUpButton", control, sizeof(control_object), MM_FW_CLASS_REF(NSPopUpButton), MM_FW_META_REF(NSPopUpButton));
    mm_objc_define_class("NSScroller", control, sizeof(control_object), MM_FW_CLASS_REF(NSScroller), MM_FW_META_REF(NSScroller));
}

} // namespace ak

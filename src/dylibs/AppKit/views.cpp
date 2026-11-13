/* NSView, NSWindow, NSViewController, NSGraphicsContext.
 *
 * Geometry follows AppKit: points, origin at the bottom left, a view's
 * frame in its superview's coordinates and its bounds in its own. A window
 * owns one platform window; its content view is the root of the view tree
 * and the next responder after the window. */
#include "appkit.h"
#include "cocoa_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
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

extern "C" {
mm_id appkit_get_association(mm_id, const void *) __asm__("_objc_getAssociatedObject");
void appkit_set_association(mm_id, const void *, mm_id, std::uintptr_t) __asm__("_objc_setAssociatedObject");
}

namespace ak {

namespace { void set_window_recursively(id view, id window); }

window_object::~window_object() {
    const id self = reinterpret_cast<id>(this);
    application_remove_window(self);
    if (platform) platform::destroy_window(platform);
    for (id child : child_windows) {
        auto *object = as<window_object>(child);
        object->parent_window = nullptr;
        if (object->platform) platform::set_parent(object->platform, nullptr);
        release(child);
    }
    if (content_view) {
        first_responder = nullptr;
        set_window_recursively(content_view, nullptr);
        release(content_view);
    }
    for (id target : mouse_targets) release(target);
}

namespace {

Class g_view, g_window, g_panel, g_view_controller, g_graphics_context;

view_object *v(id o) { return as<view_object>(o); }
window_object *w(id o) { return as<window_object>(o); }

void post_view_notification(id self, const char *name) {
    id center = obj(send(reinterpret_cast<id>(cls("NSNotificationCenter")), "defaultCenter"));
    if (center) send(center, "postNotificationName:object:", ret(string(name)), ret(self));
}

bool same_rect(const rect &a, const rect &b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/* ---- NSView -------------------------------------------------------------- */

std::uint64_t view_initWithFrame(id self, SEL, double x, double y, double width, double height) {
    if (trace())
        std::fprintf(stderr, "AppKit: -[%s initWithFrame:] {%g, %g, %g, %g}\n",
                     mm_objc_class_name(mm_objc_object_class(self)), x, y, width, height);
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
    if (x != x || y != y || width != width || height != height) {
        if (trace())
            std::fprintf(stderr, "AppKit: -[%s setFrame:] NaN {%g, %g, %g, %g} ignored\n",
                         mm_objc_class_name(mm_objc_object_class(self)), x, y, width, height);
        static bool once = false;
        if (!once) {
            once = true;
            std::fprintf(stderr, "AppKit: NaN setFrame raw registers:\n");
            if (mm_cpu_context *ctx = api()->current_context()) {
                for (int i = 0; i < 8; i++)
                    std::fprintf(stderr, "  x%d = %016llx\n", i, static_cast<unsigned long long>(ctx->x[i]));
                for (int i = 0; i < 4; i++)
                    std::fprintf(stderr, "  v%d = %016llx %016llx\n", i,
                                 static_cast<unsigned long long>(ctx->v[i][0]), static_cast<unsigned long long>(ctx->v[i][1]));
            }
        }
        return 0;
    }
    const rect previous = o->frame;
    if (trace() && !same_rect(previous, {x,y,width,height}))
        std::fprintf(stderr, "AppKit: -[%s setFrame:] {%g, %g, %g, %g} -> {%g, %g, %g, %g}\n",
                     mm_objc_class_name(mm_objc_object_class(self)), previous.x, previous.y, previous.w,
                     previous.h, x, y, width, height);
    const bool resized = previous.w != width || previous.h != height;
    o->frame = {x, y, width, height};
    if (resized) {
        if (o->autoresizes_subviews) {
            const auto children = o->subviews;
            for (id child : children) {
                auto *cv = v(child); auto f = cv->frame;
                if (!cv->translates_mask) continue;
                layout::resize_axis(f.x, f.w, previous.w, width-previous.w, cv->autoresizing_mask & 7);
                layout::resize_axis(f.y, f.h, previous.h, height-previous.h, (cv->autoresizing_mask >> 3) & 7);
                auto aligned = layout::align_edges({f.x, f.y, f.w, f.h});
                f = {aligned.x, aligned.y, aligned.w, aligned.h};
                view_setFrame(child, nullptr, f.x, f.y, f.w, f.h);
            }
        }
        o->bounds.w = width;
        o->bounds.h = height;
        if (o->layer) {
            if (is_kind(o->layer, cls("CALayer"))) {
                // AppKit owns a backing layer's frame in the view's local
                // coordinates. Presentation uses frame, not just bounds.
                auto *layer = as<layer_object>(o->layer);
                layer->frame = {0, 0, width, height};
                layer->position = {width / 2, height / 2};
            }
            if (mm_cpu_context *ctx = api()->current_context()) {
                const double values[4] = {0, 0, width, height};
                for (int i = 0; i < 4; i++) std::memcpy(&ctx->v[i][0], &values[i], 8);
            }
            send(o->layer, "setBounds:");
            if (mm_cpu_context *ctx = api()->current_context()) {
                const double values[4] = {0, 0, width, height};
                for (int i = 0; i < 4; i++) std::memcpy(&ctx->v[i][0], &values[i], 8);
            }
            send(o->layer, "setNeedsDisplayInRect:");
        }
        send(self, "setNeedsDisplay:", 1);
        if (responds(self, "frameDidChange")) send(self, "frameDidChange");
    }
    if (!same_rect(previous, o->frame) && o->posts_frame_notifications)
        post_view_notification(self, "NSViewFrameDidChangeNotification");
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
    view_object *o = v(self);
    const rect next{x, y, width, height};
    if (same_rect(o->bounds, next)) return 0;
    o->bounds = next;
    if (o->posts_bounds_notifications) post_view_notification(self, "NSViewBoundsDidChangeNotification");
    return 0;
}
std::uint64_t view_postsFrameChangedNotifications(id self, SEL) { return v(self)->posts_frame_notifications; }
std::uint64_t view_setPostsFrameChangedNotifications(id self, SEL, std::uint64_t value) {
    v(self)->posts_frame_notifications = (value & 0xff) != 0;
    return 0;
}
std::uint64_t view_postsBoundsChangedNotifications(id self, SEL) { return v(self)->posts_bounds_notifications; }
std::uint64_t view_setPostsBoundsChangedNotifications(id self, SEL, std::uint64_t value) {
    v(self)->posts_bounds_notifications = (value & 0xff) != 0;
    return 0;
}

std::uint64_t view_superview(id self, SEL) { return ret(v(self)->superview); }
std::uint64_t view_window(id self, SEL) {
    return ret(v(self)->window);
}
std::uint64_t view_subviews(id self, SEL) {
    id array = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
    for (id sub : v(self)->subviews) send(array, "addObject:", ret(sub));
    return ret(array);
}
/* A view's window is inherited by everything under it. */
void set_window_recursively(id view, id window) {
    id previous = v(view)->window;
    if (previous != window) send(view, "willChangeValueForKey:", ret(string("window")));
    if (previous && previous != window && w(previous)->first_responder == view) {
        /* A detached view cannot remain a window's weak first responder,
         * even if it refuses resignation while being removed. */
        w(previous)->first_responder = nullptr;
    }
    v(view)->window = window;
    for (id sub : v(view)->subviews) set_window_recursively(sub, window);
    if (previous != window) send(view, "didChangeValueForKey:", ret(string("window")));
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
    view_object *o = v(self);
    const bool needs = (flag & 0xFF) != 0;
    o->needs_display = needs;
    id win = obj(view_window(self, nullptr));
    if (needs && win) w(win)->needs_display = true;
    if (needs && o->layer) {
        const rect &b = o->bounds;
        if (mm_cpu_context *ctx = api()->current_context()) {
            std::memcpy(&ctx->v[0][0], &b.x, 8);
            std::memcpy(&ctx->v[1][0], &b.y, 8);
            std::memcpy(&ctx->v[2][0], &b.w, 8);
            std::memcpy(&ctx->v[3][0], &b.h, 8);
        }
        send(o->layer, "setNeedsDisplayInRect:");
    }
    return 0;
}
std::uint64_t view_setNeedsDisplayInRect(id self, SEL, double x, double y, double width, double height) {
    view_object *o = v(self);
    o->needs_display = true;
    id win = obj(view_window(self, nullptr));
    if (win) w(win)->needs_display = true;
    if (o->layer) {
        if (mm_cpu_context *ctx = api()->current_context()) {
            std::memcpy(&ctx->v[0][0], &x, 8);
            std::memcpy(&ctx->v[1][0], &y, 8);
            std::memcpy(&ctx->v[2][0], &width, 8);
            std::memcpy(&ctx->v[3][0], &height, 8);
        }
        send(o->layer, "setNeedsDisplayInRect:");
    }
    return 0;
}
std::uint64_t view_needsDisplay(id self, SEL) { return v(self)->needs_display; }
std::uint64_t view_drawRect(id, SEL, double, double, double, double) { return 0; }
std::uint64_t view_display(id self, SEL) {
    view_object *o = v(self);
    const rect &b = o->bounds;
    o->needs_display = false;
    /* -drawRect: takes an NSRect: four doubles in v0-v3. */
    mm_cpu_context *ctx = api()->current_context();
    if (ctx) {
        std::memcpy(&ctx->v[0][0], &b.x, 8);
        std::memcpy(&ctx->v[1][0], &b.y, 8);
        std::memcpy(&ctx->v[2][0], &b.w, 8);
        std::memcpy(&ctx->v[3][0], &b.h, 8);
    }
    id previous_drawing = begin_bitmap_drawing(self);
    send(self, "drawRect:");
    end_bitmap_drawing(previous_drawing);
    for (id sub : o->subviews) send(sub, "display");
    if (o->layer && is_kind(o->layer, cls("NSOpenGLLayer"))) draw_layers(self);
    return 0;
}
std::uint64_t view_displayIfNeeded(id self, SEL) {
    view_object *o = v(self);
    if (o->needs_display) send(self, "display");
    for (id sub : o->subviews) send(sub, "displayIfNeeded");
    return 0;
}
std::uint64_t view_setHidden(id self, SEL, std::uint64_t flag) { v(self)->hidden = (flag & 0xFF) != 0; return 0; }
std::uint64_t view_isHidden(id self, SEL) { return v(self)->hidden; }
std::uint64_t view_setWantsLayer(id self, SEL, std::uint64_t flag) {
    view_object *o = v(self);
    const bool wants = (flag & 0xFF) != 0;
    o->wants_layer = wants;
    if (wants && !o->layer) {
        o->layer = make_backing_layer(self);
    }
    return 0;
}
std::uint64_t view_wantsLayer(id self, SEL) { return v(self)->wants_layer; }
std::uint64_t view_layerContentsPlacement(id self, SEL) { return v(self)->contents_placement; }
std::uint64_t view_setLayerContentsPlacement(id self, SEL, std::uint64_t placement) {
    if (placement > 11) return 0;
    auto *o = v(self);
    o->contents_placement = static_cast<unsigned>(placement);
    if (o->layer && is_kind(o->layer, cls("CALayer")))
        as<layer_object>(o->layer)->contents_placement = o->contents_placement;
    return 0;
}
std::uint64_t view_layer(id self, SEL) {
    view_object *o = v(self);
    if (!o->layer && o->wants_layer) {
        o->layer = make_backing_layer(self);
    }
    return ret(o->layer);
}
std::uint64_t view_setLayer(id self, SEL, id layer) {
    view_object *o = v(self);
    if (o->layer != layer) {
        if (o->layer && is_kind(o->layer, cls("CALayer"))) {
            auto *old = as<layer_object>(o->layer);
            platform::release_metal(old);
            old->view = nullptr;
        }
        release(o->layer);
        o->layer = retain(layer);
    }
    o->wants_layer = (layer != nullptr);
    if (layer) {
        const rect &f = o->frame;
        if (is_kind(layer, cls("CALayer"))) {
            auto *lo = as<layer_object>(layer);
            lo->view = self;
            lo->frame = {0, 0, f.w, f.h};
            lo->contents_placement = o->contents_placement;
        }
        if (mm_cpu_context *ctx = api()->current_context()) {
            const double values[4] = {0, 0, f.w, f.h};
            for (int i = 0; i < 4; i++) std::memcpy(&ctx->v[i][0], &values[i], 8);
        }
        send(layer, "setBounds:");
    }
    return 0;
}
std::uint64_t view_makeBackingLayer(id, SEL) {
    return ret(obj(send(reinterpret_cast<id>(cls("CALayer")), "layer")));
}
std::uint64_t view_isFlipped(id, SEL) { return 0; }
std::uint64_t view_isOpaque(id, SEL) { return 1; }
std::uint64_t view_acceptsFirstResponder(id, SEL) { return 1; }
std::uint64_t view_viewDidMoveToWindow(id, SEL) { return 0; }
std::uint64_t view_noop(id, SEL, ...) { return 0; }

/* Frames live in the superview's coordinates; bounds live in the view's.
 * A flipped view changes the direction of y at that boundary. The window
 * base coordinate system is always bottom-left, even for a flipped root. */
bool flip_at_boundary(id view) {
    id parent = v(view)->superview;
    return (send(view, "isFlipped") != 0) != (parent && send(parent, "isFlipped") != 0);
}
point point_to_parent(id view, point p) {
    const auto *o = v(view);
    const rect frame = o->frame, bounds = o->bounds;
    const double sx = bounds.w ? frame.w / bounds.w : 1;
    const double sy = bounds.h ? frame.h / bounds.h : 1;
    const bool flipped = flip_at_boundary(view);
    return {frame.x + (p.x - bounds.x) * sx,
            flipped ? frame.y + frame.h - (p.y - bounds.y) * sy : frame.y + (p.y - bounds.y) * sy};
}
point point_from_parent(id view, point p) {
    const auto *o = v(view);
    const rect frame = o->frame, bounds = o->bounds;
    const double sx = frame.w ? bounds.w / frame.w : 1;
    const double sy = frame.h ? bounds.h / frame.h : 1;
    const bool flipped = flip_at_boundary(view);
    return {bounds.x + (p.x - frame.x) * sx,
            bounds.y + (flipped ? frame.y + frame.h - p.y : p.y - frame.y) * sy};
}
point point_to_window(id view, point p) {
    for (id c = view; c; c = v(c)->superview) p = point_to_parent(c, p);
    return p;
}
point point_from_window(id view, point p) {
    std::vector<id> path;
    for (id c = view; c; c = v(c)->superview) path.push_back(c);
    for (auto it = path.rbegin(); it != path.rend(); ++it) p = point_from_parent(*it, p);
    return p;
}
mm_two_doubles view_convertPointFromView(id self, SEL, double x, double y, id from) {
    const point p = point_from_window(self, point_to_window(from, {x, y}));
    return {p.x, p.y};
}
mm_two_doubles view_convertPointToView(id self, SEL, double x, double y, id to) {
    const point p = point_from_window(to, point_to_window(self, {x, y}));
    return {p.x, p.y};
}
mm_two_doubles view_convertRectFromView(id self, SEL, double x, double y, double width, double height, id from) {
    const auto a = view_convertPointFromView(self, nullptr, x, y, from);
    const auto b = view_convertPointFromView(self, nullptr, x + width, y + height, from);
    mm_objc_return_doubles_hi(std::abs(b.a - a.a), std::abs(b.b - a.b));
    return {std::min(a.a, b.a), std::min(a.b, b.b)};
}
mm_two_doubles view_convertRectToView(id self, SEL, double x, double y, double width, double height, id to) {
    const auto a = view_convertPointToView(self, nullptr, x, y, to);
    const auto b = view_convertPointToView(self, nullptr, x + width, y + height, to);
    mm_objc_return_doubles_hi(std::abs(b.a - a.a), std::abs(b.b - a.b));
    return {std::min(a.a, b.a), std::min(a.b, b.b)};
}
mm_two_doubles view_convertSizeToBacking(id, SEL, double w, double h) {
    const double scale = platform::screen_scale();
    return {w * scale, h * scale};
}
mm_two_doubles view_convertSizeFromBacking(id, SEL, double w, double h) {
    const double scale = platform::screen_scale();
    const double inv = scale > 0 ? 1.0 / scale : 1.0;
    return {w * inv, h * inv};
}
mm_two_doubles view_convertPointToBacking(id, SEL, double x, double y) {
    const double scale = platform::screen_scale();
    return {x * scale, y * scale};
}
mm_two_doubles view_convertPointFromBacking(id, SEL, double x, double y) {
    const double scale = platform::screen_scale();
    const double inv = scale > 0 ? 1.0 / scale : 1.0;
    return {x * inv, y * inv};
}
mm_two_doubles view_convertRectToBacking(id, SEL, double x, double y, double width, double height) {
    const double scale = platform::screen_scale();
    mm_objc_return_doubles_hi(width * scale, height * scale);
    return {x * scale, y * scale};
}
mm_two_doubles view_convertRectFromBacking(id, SEL, double x, double y, double width, double height) {
    const double scale = platform::screen_scale();
    const double inv = scale > 0 ? 1.0 / scale : 1.0;
    mm_objc_return_doubles_hi(width * inv, height * inv);
    return {x * inv, y * inv};
}
double view_backingScaleFactor(id, SEL) {
    return platform::screen_scale();
}

/* Hit testing walks the subviews back to front, like AppKit. */
std::uint64_t view_hitTest(id self, SEL, double x, double y) {
    view_object *o = v(self);
    if (o->hidden) return 0;
    if (x < o->frame.x || y < o->frame.y || x >= o->frame.x + o->frame.w || y >= o->frame.y + o->frame.h) return 0;
    const point local = point_from_parent(self, {x, y});
    const double lx = local.x, ly = local.y;
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

control_object *control_of(id o) { return as<control_object>(o); }

std::uint64_t control_title(id self, SEL) { return ret(control_of(self)->title); }
std::uint64_t control_setTitle(id self, SEL, id title) {
    control_object *c = control_of(self);
    id replacement = retain(title);
    release(c->title); c->title = replacement;
    release(c->attributed_title); c->attributed_title = nullptr;
    view_setNeedsDisplay(self, nullptr, 1);
    return 0;
}
std::uint64_t control_setAttributedTitle(id self, SEL, id title) {
    retain(title);
    control_setTitle(self,nullptr,title?obj(send(title,"string")):nullptr);
    control_of(self)->attributed_title = title;
    return 0;
}
std::uint64_t control_attributedTitle(id self, SEL) { return ret(control_of(self)->attributed_title); }
std::uint64_t control_target(id self, SEL) { return ret(control_of(self)->target); }
std::uint64_t control_setTarget(id self, SEL, id target) { control_of(self)->target = target; return 0; }
std::uint64_t control_action(id self, SEL) { return reinterpret_cast<std::uintptr_t>(control_of(self)->action); }
std::uint64_t control_setAction(id self, SEL, SEL action) { control_of(self)->action = action; return 0; }
std::uint64_t control_state(id self, SEL) { return static_cast<std::uint64_t>(control_of(self)->state); }
std::uint64_t control_setState(id self, SEL, std::int64_t state) { control_of(self)->state = state; view_setNeedsDisplay(self, nullptr, 1); return 0; }
std::uint64_t control_isEnabled(id self, SEL) { return control_of(self)->enabled ? 1 : 0; }
std::uint64_t control_setEnabled(id self, SEL, std::uint64_t enabled) { control_of(self)->enabled = (enabled & 0xFF) != 0; view_setNeedsDisplay(self, nullptr, 1); return 0; }
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

std::uint64_t scroller_preferredStyle(id, SEL) { return 0; /* NSScrollerStyleLegacy */ }

/* Clicking sends the action to the target, which is what a control does. */
std::uint64_t control_performClick(id self, SEL, id) {
    auto *c = control_of(self);
    if (!c->enabled) return 0;
    if (c->button_type == 3 || c->button_type == 1 || c->button_type == 2) c->state = c->state ? 0 : 1;
    if (c->button_type == 4) c->state = 1;
    view_setNeedsDisplay(self, nullptr, 1);
    if (!c->action) return 0;
    if (g_NSApp) send(g_NSApp, "sendAction:to:from:", ret(c->action), ret(c->target), ret(self));
    else if (c->target) { const auto arg = ret(self); mm_objc_send(c->target, c->action, 1, &arg); }
    return 0;
}
std::uint64_t control_isHighlighted(id self, SEL) { return control_of(self)->highlighted; }
std::uint64_t control_mouseDown(id self, SEL, id) {
    auto *c = control_of(self); c->highlighted = c->enabled; c->view.needs_display = true; return 0;
}
std::uint64_t control_mouseUp(id self, SEL, id event) {
    auto *c = control_of(self); const bool pressed = c->highlighted;
    c->highlighted = false; c->view.needs_display = true;
    auto point = as<event_object>(event)->where;
    std::vector<id> parents;
    for (id view = self; view; view = as<view_object>(view)->superview) parents.push_back(view);
    for (auto it = parents.rbegin(); it != parents.rend(); ++it) point = point_from_parent(*it, point);
    const auto b = c->view.bounds;
    if (pressed && point.x >= b.x && point.x < b.x+b.w && point.y >= b.y && point.y < b.y+b.h)
        send(self,"performClick:",0);
    return 0;
}

/* ---- NSWindow ------------------------------------------------------------ */

std::uint64_t window_initWithContentRect(id self, SEL, double x, double y, double width, double height,
                                         std::uint64_t style, std::uint64_t, std::uint64_t) {
    window_object *o = w(self);
    /* Live's first run computes the window origin from a saved-geometry value
     * that comes back as NSNotFound (9.22e18 as a double); every layout
     * operation downstream then produces NaN and the UI draws nothing but
     * the clear. Pull an absurd origin back onto the screen. */
    if (!(x > -1e9 && x < 1e9)) x = 40;
    if (!(y > -1e9 && y < 1e9)) y = 40;
    if (!(width > 0 && width < 1e6)) width = 1280;
    if (!(height > 0 && height < 1e6)) height = 800;
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
    std::unique_ptr<mm_objc_object, decltype(&mm_objc_release)> receiver(retain(self), mm_objc_release);
    window_object *o = w(self);
    if (o->content_view == view) return 0;
    id replacement = retain(view);
    id previous = o->content_view;
    o->content_view = replacement;
    if (previous) {
        set_window_recursively(previous, nullptr);
        send(previous, "setNextResponder:", 0);
        release(previous);
    }
    if (view) {
        set_window_recursively(view, self);
        send(view, "setNextResponder:", ret(self));
        const rect &f = o->frame;
        view_setFrame(view, nullptr, 0, 0, f.w, f.h);
        send(view, "viewDidMoveToWindow");
        view_object *vo = v(view);
        if (vo->layer && responds(vo->layer, "viewDidMoveToWindow"))
            send(vo->layer, "viewDidMoveToWindow");
    }
    return 0;
}
mm_two_doubles window_frame(id self, SEL) {
    const rect &r = w(self)->frame;
    mm_objc_return_doubles_hi(r.w, r.h);
    static int frames = 0;
    if (trace() && frames++ < 12)
        std::fprintf(stderr, "AppKit: -[NSWindow frame] -> {%g, %g, %g, %g}\n", r.x, r.y, r.w, r.h);
    return {r.x, r.y};
}
std::uint64_t window_setFrame(id self, SEL, double x, double y, double width, double height, std::uint64_t display) {
    window_object *o = w(self);
    /* Live's layout hands down NaN on first run (some metric it computes
     * from comes back as garbage); a NaN frame would resize the EGL window
     * with an int cast of NaN and the surface stops presenting. */
    if (x != x || y != y || width != width || height != height) {
        if (trace())
            std::fprintf(stderr, "AppKit: -[NSWindow setFrame:] NaN {%g, %g, %g, %g} ignored\n", x, y, width, height);
        return 0;
    }
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
std::uint64_t window_setContentSize(id self, SEL, double width, double height) {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) return 0;
    const auto f = w(self)->frame;
    // Preserve the top edge, as AppKit does when changing content height.
    return window_setFrame(self, nullptr, f.x, f.y + f.h - height, width, height, 0);
}
std::uint64_t window_setMinimumSize(id self, SEL, double width, double height) {
    auto *o = w(self);
    if (!std::isfinite(width) || !std::isfinite(height) || width < 0 || height < 0) return 0;
    o->minimum_size = {width, height};
    if (o->platform) platform::set_size_limits(o->platform, o->minimum_size, o->maximum_size);
    return 0;
}
std::uint64_t window_setMaximumSize(id self, SEL, double width, double height) {
    auto *o = w(self);
    if (!std::isfinite(width) || !std::isfinite(height) || width < 0 || height < 0) return 0;
    o->maximum_size = {width, height};
    if (o->platform) platform::set_size_limits(o->platform, o->minimum_size, o->maximum_size);
    return 0;
}
mm_two_doubles window_minimumSize(id self, SEL) { auto s = w(self)->minimum_size; return {s.w, s.h}; }
mm_two_doubles window_maximumSize(id self, SEL) { auto s = w(self)->maximum_size; return {s.w, s.h}; }
mm_two_doubles window_contentRectForFrameRect(id, SEL, double x, double y, double width, double height) {
    static int crs = 0;
    if (trace() && crs++ < 12)
        std::fprintf(stderr, "AppKit: contentRectForFrameRect({%g, %g, %g, %g}) -> same\n", x, y, width, height);
    mm_objc_return_doubles_hi(width, height);
    return {x, y};
}
std::uint64_t window_title(id self, SEL) { return ret(string(w(self)->title)); }
const char accessibility_subrole_key = 0;
const char represented_url_key = 0;
std::uint64_t window_representedURL(id self, SEL) {
    return ret(appkit_get_association(self, &represented_url_key));
}
std::uint64_t window_setRepresentedURL(id self, SEL, id value) {
    appkit_set_association(self, &represented_url_key, value, 1);
    return 0;
}
std::uint64_t window_accessibilitySubrole(id self, SEL) {
    return ret(appkit_get_association(self, &accessibility_subrole_key));
}
std::uint64_t window_setAccessibilitySubrole(id self, SEL, id value) {
    id copy = value ? string(to_utf8(value)) : nullptr;
    appkit_set_association(self, &accessibility_subrole_key, copy, 1);
    return 0;
}
std::uint64_t window_setTitle(id self, SEL, id title) {
    window_object *o = w(self);
    o->title = to_utf8(title);
    if (trace()) std::fprintf(stderr, "AppKit: window %p title=\"%s\"\n", static_cast<void *>(self), o->title.c_str());
    if (o->platform) platform::set_title(o->platform, o->title);
    return 0;
}

} // namespace

/* Showing a window is when the platform window appears. */
void ensure_platform_window(id self) {
    // Presentation can arrive on the Metal completion worker while Cocoa is
    // ordering the same window. Creation includes a Wayland roundtrip, so the
    // check and publication must be serialized (including parent recursion).
    static std::recursive_mutex creation_mutex;
    std::lock_guard creation_lock(creation_mutex);
    window_object *o = as<window_object>(self);
    // Fully transparent companion windows have no pixels to map. Keep their
    // Cocoa ownership/ordering without creating an opaque Wayland toplevel.
    if (o->platform || o->alpha == 0 || !platform::available()) return;
    if (o->parent_window) ensure_platform_window(o->parent_window);
    const bool decorated = (o->style & 1) != 0; /* NSWindowStyleMaskTitled */
    o->platform = platform::create_window(self, o->frame, o->title, decorated, (o->style & (1u << 3)) != 0,
                                         o->popup && o->parent_window ? w(o->parent_window)->platform : nullptr,
                                         o->popup_anchor, o->popup_serial);
    if (o->platform) {
        if (o->parent_window) platform::set_parent(o->platform, as<window_object>(o->parent_window)->platform);
        platform::set_size_limits(o->platform, o->minimum_size, o->maximum_size);
        /* The initial Wayland configure can negotiate a different size
         * before NSApplication gets back to dispatching queued events.
         * Expose that geometry immediately, so launch-time drawing uses
         * the same dimensions as the EGL surface. */
        const rect actual = platform::window_frame(o->platform);
        std::fprintf(stderr, "AppKit layout: platform-window=%p title=\"%s\" requested=%gx%g configured=%gx%g\n",
                     static_cast<void *>(self),o->title.c_str(),o->frame.w,o->frame.h,actual.w,actual.h);
        if (actual.w != o->frame.w || actual.h != o->frame.h) {
            /* Publish geometry immediately for launch-time GL drawing,
             * then deliver the compositor's resize after the window is
             * visible. The initial synchronous update can precede the
             * guest view's readiness to resize its application layout. */
            o->pending_configure_notification = true;
            o->frame.w = actual.w;
            o->frame.h = actual.h;
            if (o->content_view) view_setFrame(o->content_view, nullptr, 0, 0, actual.w, actual.h);
        }
    }
}

namespace {
void notify_occlusion_change(id window, bool previously_visible) {
    if (previously_visible == w(window)->visible) return;
    id center = obj(send(reinterpret_cast<id>(cls("NSNotificationCenter")), "defaultCenter"));
    send(center, "postNotificationName:object:", ret(string("NSWindowDidChangeOcclusionStateNotification")), ret(window));
}
void order_children(id self, bool visible) {
    const auto children = w(self)->child_windows;
    for (id child : children) retain(child);
    for (id child : children) {
        if (w(child)->parent_window == self) send(child, visible ? "orderFront:" : "orderOut:", 0);
        release(child);
    }
}
std::uint64_t window_makeKeyAndOrderFront(id self, SEL, id) {
    std::unique_ptr<mm_objc_object, decltype(&mm_objc_release)> receiver(retain(self), mm_objc_release);
    ensure_platform_window(self);
    window_object *o = w(self);
    const bool previously_visible = o->visible;
    o->visible = true;
    application_order_front(self);
    if (o->platform) platform::show_window(o->platform, o->alpha > 0);
    send(self, "becomeKeyWindow");
    notify_occlusion_change(self, previously_visible);
    order_children(self, true);
    return 0;
}
std::uint64_t window_orderFront(id self, SEL, id) { return window_makeKeyAndOrderFront(self, nullptr, nullptr); }
std::uint64_t window_orderOut(id self, SEL, id) {
    std::unique_ptr<mm_objc_object, decltype(&mm_objc_release)> receiver(retain(self), mm_objc_release);
    window_object *o = w(self);
    const bool previously_visible = o->visible;
    o->visible = false;
    application_order_out(self);
    if (o->platform) platform::show_window(o->platform, false);
    notify_occlusion_change(self, previously_visible);
    order_children(self, false);
    return 0;
}
std::uint64_t window_close(id self, SEL) {
    std::unique_ptr<mm_objc_object, decltype(&mm_objc_release)> receiver(retain(self), mm_objc_release);
    cancel_dragging_session(self);
    window_object *o = w(self);
    const bool previously_visible = o->visible;
    o->visible = false;
    if (o->platform) { platform::destroy_window(o->platform); o->platform = nullptr; }
    order_children(self, false);
    notify_occlusion_change(self, previously_visible);
    id delegate = o->delegate;
    if (responds(delegate, "windowWillClose:")) send(delegate, "windowWillClose:", ret(self));
    application_remove_window(self);
    application_window_closed();
    return 0;
}
std::uint64_t window_isVisible(id self, SEL) { return w(self)->visible; }
// Wayland does not expose overlap occlusion. An ordered-in window is visible.
std::uint64_t window_occlusionState(id self, SEL) { return w(self)->visible ? 2 : 0; }
std::uint64_t window_parentWindow(id self, SEL) { return ret(w(self)->parent_window); }
std::uint64_t window_childWindows(id self, SEL) {
    id result = obj(send(reinterpret_cast<id>(cls("NSMutableArray")), "array"));
    for (id child : w(self)->child_windows) send(result, "addObject:", ret(child));
    return ret(result);
}
std::uint64_t window_removeChildWindow(id self, SEL, id child) {
    auto &children = w(self)->child_windows;
    auto it = std::find(children.begin(), children.end(), child);
    if (it == children.end()) return 0;
    children.erase(it);
    w(child)->parent_window = nullptr;
    if (w(child)->platform) platform::set_parent(w(child)->platform, nullptr);
    release(child);
    return 0;
}
std::uint64_t window_addChildWindow(id self, SEL, id child, std::int64_t order) {
    if (!child || !is_kind(child, g_window) || child == self) return 0;
    for (id ancestor = self; ancestor; ancestor = w(ancestor)->parent_window)
        if (ancestor == child) return 0; // Reject a cycle.
    std::unique_ptr<mm_objc_object, decltype(&mm_objc_release)> owned(retain(child), mm_objc_release);
    if (id old_parent = w(child)->parent_window) window_removeChildWindow(old_parent, nullptr, child);
    w(child)->parent_window = self;
    auto &children = w(self)->child_windows;
    if (order < 0) children.insert(children.begin(), retain(child));
    else children.push_back(retain(child));
    if (w(child)->platform) {
        ensure_platform_window(self);
        platform::set_parent(w(child)->platform, w(self)->platform);
    }
    if (w(self)->visible) send(child, "orderFront:", 0);
    return 0;
}
double window_alphaValue(id self, SEL) { return w(self)->alpha; }
std::uint64_t window_setAlphaValue(id self, SEL, double value) {
    if (!std::isfinite(value)) return 0;
    auto *window = w(self);
    window->alpha = std::clamp(value, 0.0, 1.0);
    if (window->visible) {
        if (window->alpha > 0) ensure_platform_window(self);
        if (window->platform) platform::show_window(window->platform, window->alpha > 0);
    }
    return 0;
}
std::uint64_t window_isKeyWindow(id self, SEL) { return w(self)->key; }
std::uint64_t window_isMainWindow(id self, SEL) { return w(self)->key; }
std::uint64_t window_becomeKeyWindow(id self, SEL) { w(self)->key = true; return 0; }
std::uint64_t window_resignKeyWindow(id self, SEL) { w(self)->key = false; return 0; }
std::uint64_t window_makeFirstResponder(id self, SEL, id responder) {
    std::unique_ptr<mm_objc_object, decltype(&mm_objc_release)> receiver(retain(self), mm_objc_release);
    window_object *o = w(self);
    if (o->first_responder == responder) return 1;
    /* A rejected candidate can be requested again by the old responder's
     * restoration callback. Reject that pending request before resigning
     * the old responder again, while permitting a callback to select a
     * different candidate. The outer transaction owns candidate lifetime. */
    if (std::find(o->first_responder_requests.begin(), o->first_responder_requests.end(), responder) != o->first_responder_requests.end()) return 0;
    o->first_responder_requests.push_back(responder);
    struct focus_request_scope {
        window_object *window;
        ~focus_request_scope() { window->first_responder_requests.pop_back(); }
    } request{o};
    std::unique_ptr<mm_objc_object, decltype(&mm_objc_release)> previous(retain(o->first_responder), mm_objc_release);
    std::unique_ptr<mm_objc_object, decltype(&mm_objc_release)> candidate(retain(responder), mm_objc_release);
    if (previous && !send(previous.get(), "resignFirstResponder")) return 0;
    /* A guest focus callback can itself choose another responder. */
    if (o->first_responder != previous.get()) return o->first_responder == responder;
    o->first_responder = responder;
    if (responder && !send(responder, "becomeFirstResponder")) {
        if (o->first_responder == responder) {
            o->first_responder = previous.get();
            if (previous) send(previous.get(), "becomeFirstResponder");
        }
        return 0;
    }
    return o->first_responder == responder;
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
std::uint64_t window_displayIfNeeded(id self, SEL) {
    window_object *o = w(self);
    if (o->needs_display) {
        o->needs_display = false;
        if (o->content_view) send(o->content_view, "displayIfNeeded");
    }
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
mm_two_doubles window_mouseLocationOutsideOfEventStream(id self, SEL) {
    const rect &f = w(self)->frame;
    return {g_mouse.x - f.x, g_mouse.y - f.y};
}
mm_two_doubles window_convertRectToBacking(id self, SEL, double x, double y, double width, double height) {
    const double scale = platform::screen_scale();
    mm_objc_return_doubles_hi(width * scale, height * scale);
    return {x * scale, y * scale};
}
mm_two_doubles window_convertRectFromBacking(id self, SEL, double x, double y, double width, double height) {
    const double scale = platform::screen_scale();
    const double inv = scale > 0 ? 1.0 / scale : 1.0;
    mm_objc_return_doubles_hi(width * inv, height * inv);
    return {x * inv, y * inv};
}
std::uint64_t window_screen(id, SEL) { return send(reinterpret_cast<id>(cls("NSScreen")), "mainScreen"); }
std::uint64_t window_sendEvent(id self, SEL, id event) {
    /* Route to the view under the pointer, or the first responder. */
    std::unique_ptr<mm_objc_object, decltype(&mm_objc_release)> receiver(retain(self), mm_objc_release);
    window_object *o = w(self);
    const std::uint64_t type = send(event, "type");
    if (type == ev_key_down && o->content_view && !text_input_has_priority(self, event)) {
        std::unique_ptr<mm_objc_object, decltype(&mm_objc_release)> content(retain(o->content_view), mm_objc_release);
        if (send(content.get(), "performKeyEquivalent:", ret(event))) return 0;
    }
    id target = o->first_responder ? o->first_responder : o->content_view;
    int button = -1;
    bool down = false, up = false, drag = false;
    if (type == ev_left_mouse_down || type == ev_left_mouse_up || type == ev_left_mouse_dragged) button = 0;
    if (type == ev_right_mouse_down || type == ev_right_mouse_up || type == ev_right_mouse_dragged) button = 1;
    if (type == ev_other_mouse_down || type == ev_other_mouse_up || type == ev_other_mouse_dragged) button = 2;
    down = type == ev_left_mouse_down || type == ev_right_mouse_down || type == ev_other_mouse_down;
    up = type == ev_left_mouse_up || type == ev_right_mouse_up || type == ev_other_mouse_up;
    drag = type == ev_left_mouse_dragged || type == ev_right_mouse_dragged || type == ev_other_mouse_dragged;
    if ((up || drag) && o->mouse_targets[button]) target = o->mouse_targets[button];
    if (o->content_view && (type == ev_left_mouse_down || type == ev_right_mouse_down || type == ev_other_mouse_down ||
                            type == ev_scroll_wheel || type == ev_mouse_moved)) {
        auto *e = as<event_object>(event);
        mm_cpu_context *ctx = api()->current_context();
        if (ctx) { std::memcpy(&ctx->v[0][0], &e->where.x, 8); std::memcpy(&ctx->v[1][0], &e->where.y, 8); }
        if (const std::uint64_t hit = send(o->content_view, "hitTest:")) target = obj(hit);
    }
    if (down) {
        release(o->mouse_targets[button]);
        o->mouse_targets[button] = retain(target);
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
    for (const auto &r : routes) {
        if (r.type != type) continue;
        if ((trace() && (down || up || type == ev_key_down || type == ev_key_up || type == ev_flags_changed)) ||
            (std::getenv("MACHOMAN_APPKIT_POINTER_TRACE") && (down || up)))
            std::fprintf(stderr, "AppKit: %s -> %s %p\n", r.sel, mm_objc_class_name(mm_objc_object_class(target)), static_cast<void *>(target));
        /* The handler can close the window or replace its content view. */
        std::unique_ptr<mm_objc_object, decltype(&mm_objc_release)> responder(retain(target), mm_objc_release);
        const auto result = send(target, r.sel, ret(event));
        if (up) { release(o->mouse_targets[button]); o->mouse_targets[button] = nullptr; }
        return result;
    }
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
        {"postsFrameChangedNotifications", MM_IMP(view_postsFrameChangedNotifications), "B16@0:8"},
        {"setPostsFrameChangedNotifications:", MM_IMP(view_setPostsFrameChangedNotifications), "v24@0:8B16"},
        {"postsBoundsChangedNotifications", MM_IMP(view_postsBoundsChangedNotifications), "B16@0:8"},
        {"setPostsBoundsChangedNotifications:", MM_IMP(view_setPostsBoundsChangedNotifications), "v24@0:8B16"},
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
        {"displayIfNeeded", MM_IMP(view_displayIfNeeded), "v16@0:8"},
        {"setHidden:", MM_IMP(view_setHidden), "v24@0:8B16"},
        {"isHidden", MM_IMP(view_isHidden), "B16@0:8"},
        {"setWantsLayer:", MM_IMP(view_setWantsLayer), "v24@0:8B16"},
        {"wantsLayer", MM_IMP(view_wantsLayer), "B16@0:8"},
        {"layer", MM_IMP(view_layer), "@16@0:8"},
        {"setLayer:", MM_IMP(view_setLayer), "v24@0:8@16"},
        {"makeBackingLayer", MM_IMP(view_makeBackingLayer), "@16@0:8"},
        {"isFlipped", MM_IMP(view_isFlipped), "B16@0:8"},
        {"isOpaque", MM_IMP(view_isOpaque), "B16@0:8"},
        {"acceptsFirstResponder", MM_IMP(view_acceptsFirstResponder), "B16@0:8"},
        {"viewDidMoveToWindow", MM_IMP(view_viewDidMoveToWindow), "v16@0:8"},
        {"hitTest:", MM_IMP(view_hitTest), "@32@0:8{CGPoint=dd}16"},
        {"convertPoint:fromView:", MM_IMP(view_convertPointFromView), "{CGPoint=dd}40@0:8{CGPoint=dd}16@32"},
        {"convertPoint:toView:", MM_IMP(view_convertPointToView), "{CGPoint=dd}40@0:8{CGPoint=dd}16@32"},
        {"convertRect:fromView:", MM_IMP(view_convertRectFromView), "{CGRect={CGPoint=dd}{CGSize=dd}}56@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16@48"},
        {"convertRect:toView:", MM_IMP(view_convertRectToView), "{CGRect={CGPoint=dd}{CGSize=dd}}56@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16@48"},
        {"convertSizeToBacking:", MM_IMP(view_convertSizeToBacking), "{CGSize=dd}32@0:8{CGSize=dd}16"},
        {"convertSizeFromBacking:", MM_IMP(view_convertSizeFromBacking), "{CGSize=dd}32@0:8{CGSize=dd}16"},
        {"convertPointToBacking:", MM_IMP(view_convertPointToBacking), "{CGPoint=dd}32@0:8{CGPoint=dd}16"},
        {"convertPointFromBacking:", MM_IMP(view_convertPointFromBacking), "{CGPoint=dd}32@0:8{CGPoint=dd}16"},
        {"convertRectToBacking:", MM_IMP(view_convertRectToBacking), "{CGRect={CGPoint=dd}{CGSize=dd}}48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"convertRectFromBacking:", MM_IMP(view_convertRectFromBacking), "{CGRect={CGPoint=dd}{CGSize=dd}}48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"backingScaleFactor", MM_IMP(view_backingScaleFactor), "d16@0:8"});
    for (const char *sel : {"updateTrackingAreas", "addTrackingArea:", "removeTrackingArea:", "setToolTip:",
                            "registerForDraggedTypes:", "unregisterDraggedTypes", "setAlphaValue:", "invalidateIntrinsicContentSize",
                            "layout",
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
        {"representedURL", MM_IMP(window_representedURL), "@16@0:8"},
        {"setRepresentedURL:", MM_IMP(window_setRepresentedURL), "v24@0:8@16"},
        {"accessibilitySubrole", MM_IMP(window_accessibilitySubrole), "@16@0:8"},
        {"setAccessibilitySubrole:", MM_IMP(window_setAccessibilitySubrole), "v24@0:8@16"},
        {"makeKeyAndOrderFront:", MM_IMP(window_makeKeyAndOrderFront), "v24@0:8@16"},
        {"makeKeyWindow", MM_IMP(window_becomeKeyWindow), "v16@0:8"},
        {"orderFront:", MM_IMP(window_orderFront), "v24@0:8@16"},
        {"orderFrontRegardless", MM_IMP(window_orderFront), "v16@0:8"},
        {"orderOut:", MM_IMP(window_orderOut), "v24@0:8@16"},
        {"close", MM_IMP(window_close), "v16@0:8"},
        {"performClose:", MM_IMP(window_close), "v24@0:8@16"},
        {"isVisible", MM_IMP(window_isVisible), "B16@0:8"},
        {"occlusionState", MM_IMP(window_occlusionState), "Q16@0:8"},
        {"parentWindow", MM_IMP(window_parentWindow), "@16@0:8"},
        {"childWindows", MM_IMP(window_childWindows), "@16@0:8"},
        {"addChildWindow:ordered:", MM_IMP(window_addChildWindow), "v32@0:8@16q24"},
        {"removeChildWindow:", MM_IMP(window_removeChildWindow), "v24@0:8@16"},
        {"alphaValue", MM_IMP(window_alphaValue), "d16@0:8"},
        {"setAlphaValue:", MM_IMP(window_setAlphaValue), "v24@0:8d16"},
        {"isKeyWindow", MM_IMP(window_isKeyWindow), "B16@0:8"},
        {"isMainWindow", MM_IMP(window_isMainWindow), "B16@0:8"},
        {"becomeKeyWindow", MM_IMP(window_becomeKeyWindow), "v16@0:8"},
        {"resignKeyWindow", MM_IMP(window_resignKeyWindow), "v16@0:8"},
        {"makeFirstResponder:", MM_IMP(window_makeFirstResponder), "B24@0:8@16"},
        {"firstResponder", MM_IMP(window_firstResponder), "@16@0:8"},
        {"delegate", MM_IMP(window_delegate), "@16@0:8"},
        {"setDelegate:", MM_IMP(window_setDelegate), "v24@0:8@16"},
        {"display", MM_IMP(window_display), "v16@0:8"},
        {"displayIfNeeded", MM_IMP(window_displayIfNeeded), "v16@0:8"},
        {"setAcceptsMouseMovedEvents:", MM_IMP(window_setAcceptsMouseMovedEvents), "v24@0:8B16"},
        {"styleMask", MM_IMP(window_styleMask), "Q16@0:8"},
        {"setStyleMask:", MM_IMP(window_setStyleMask), "v24@0:8Q16"},
        {"backingScaleFactor", MM_IMP(window_backingScaleFactor), "d16@0:8"},
        {"convertPointToScreen:", MM_IMP(window_convertPointToScreen), "{CGPoint=dd}32@0:8{CGPoint=dd}16"},
        {"convertPointFromScreen:", MM_IMP(window_convertPointFromScreen), "{CGPoint=dd}32@0:8{CGPoint=dd}16"},
        {"convertBaseToScreen:", MM_IMP(window_convertPointToScreen), "{CGPoint=dd}32@0:8{CGPoint=dd}16"},
        {"convertScreenToBase:", MM_IMP(window_convertPointFromScreen), "{CGPoint=dd}32@0:8{CGPoint=dd}16"},
        {"mouseLocationOutsideOfEventStream", MM_IMP(window_mouseLocationOutsideOfEventStream), "{CGPoint=dd}16@0:8"},
        {"convertRectToBacking:", MM_IMP(window_convertRectToBacking), "{CGRect={CGPoint=dd}{CGSize=dd}}48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"convertRectFromBacking:", MM_IMP(window_convertRectFromBacking), "{CGRect={CGPoint=dd}{CGSize=dd}}48@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16"},
        {"screen", MM_IMP(window_screen), "@16@0:8"},
        {"sendEvent:", MM_IMP(window_sendEvent), "v24@0:8@16"});
    /* Layer-backing and high-DPI knobs: this draws at one scale into one
     * surface, so they are accepted and change nothing. */
    MM_METHODS(g_view,
        {"layerContentsPlacement", MM_IMP(view_layerContentsPlacement), "q16@0:8"},
        {"setLayerContentsPlacement:", MM_IMP(view_setLayerContentsPlacement), "v24@0:8q16"});
    for (const char *sel : {"setLayerContentsRedrawPolicy:",
                            "setWantsBestResolutionOpenGLSurface:", "setCanDrawConcurrently:",
                            "setAutoresizingMask:", "setAutoresizesSubviews:", "setToolTip:",
                            "addTrackingArea:", "removeTrackingArea:", "updateTrackingAreas"})
        mm_objc_add_method(g_view, sel, MM_IMP(view_noop), "v24@0:8@16");

    mm_objc_add_method(g_view, "beginDraggingSessionWithItems:event:source:", MM_IMP(begin_dragging_session), "@40@0:8@16@24@32");
    MM_METHODS(g_window,
        {"setContentSize:", MM_IMP(window_setContentSize), "v32@0:8{CGSize=dd}16"},
        {"setMinSize:", MM_IMP(window_setMinimumSize), "v32@0:8{CGSize=dd}16"},
        {"setContentMinSize:", MM_IMP(window_setMinimumSize), "v32@0:8{CGSize=dd}16"},
        {"setMaxSize:", MM_IMP(window_setMaximumSize), "v32@0:8{CGSize=dd}16"},
        {"setContentMaxSize:", MM_IMP(window_setMaximumSize), "v32@0:8{CGSize=dd}16"},
        {"minSize", MM_IMP(window_minimumSize), "{CGSize=dd}16@0:8"},
        {"contentMinSize", MM_IMP(window_minimumSize), "{CGSize=dd}16@0:8"},
        {"maxSize", MM_IMP(window_maximumSize), "{CGSize=dd}16@0:8"},
        {"contentMaxSize", MM_IMP(window_maximumSize), "{CGSize=dd}16@0:8"});
    for (const char *sel : {"setOpaque:", "setBackgroundColor:", "setReleasedWhenClosed:", "setLevel:",
                            "center", "setRestorable:",
                            "setCollectionBehavior:", "setTitlebarAppearsTransparent:", "setMovableByWindowBackground:",
                            "invalidateShadow", "setViewsNeedDisplay:", "setAllowsConcurrentViewDrawing:",
                            "toggleFullScreen:", "zoom:", "miniaturize:", "deminiaturize:", "setAppearance:",
                            /* Appearance and behaviour the Wayland backend does not model: a
                             * window is a surface with a size here, so these are accepted and
                             * have no effect, the way they do on a system without shadows or
                             * window levels. */
                            "setHasShadow:", "setIgnoresMouseEvents:", "setCanHide:",
                            "setHidesOnDeactivate:", "setAnimationBehavior:", "setFrameAutosaveName:",
                            "setExcludedFromWindowsMenu:", "setShowsResizeIndicator:", "setAutodisplay:",
                            "setPreservesContentDuringLiveResize:", "setDisplaysWhenScreenProfileChanges:",
                            "useOptimizedDrawing:", "setTabbingMode:", "setTitleVisibility:", "setToolbar:",
                            "setInitialFirstResponder:", "setAspectRatio:",
                            "setResizeIncrements:", "setDocumentEdited:", "setRepresentedFilename:",
                            "disableCursorRects", "enableCursorRects", "resetCursorRects",
                            "setOneShot:", "setDepthLimit:",
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
        {"setAttributedStringValue:", MM_IMP(control_setAttributedTitle), "v24@0:8@16"},
        {"attributedStringValue", MM_IMP(control_attributedTitle), "@16@0:8"},
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
    Class button = mm_objc_define_class("NSButton", control, sizeof(control_object), MM_FW_CLASS_REF(NSButton), MM_FW_META_REF(NSButton));
    MM_METHODS(button, {"mouseDown:", MM_IMP(control_mouseDown), "v24@0:8@16"},
        {"mouseUp:", MM_IMP(control_mouseUp), "v24@0:8@16"}, {"isHighlighted", MM_IMP(control_isHighlighted), "B16@0:8"});
    mm_objc_add_method(control, "isHighlighted", MM_IMP(control_isHighlighted), "B16@0:8");
    mm_objc_define_class("NSTextField", control, sizeof(control_object), MM_FW_CLASS_REF(NSTextField), MM_FW_META_REF(NSTextField));
    mm_objc_define_class("NSPopUpButton", control, sizeof(control_object), MM_FW_CLASS_REF(NSPopUpButton), MM_FW_META_REF(NSPopUpButton));
    Class scroller = mm_objc_define_class("NSScroller", control, sizeof(control_object),
        MM_FW_CLASS_REF(NSScroller), MM_FW_META_REF(NSScroller));
    MM_CLASS_METHODS(scroller,
        {"preferredScrollerStyle", MM_IMP(scroller_preferredStyle), "q16@0:8"});
    register_cocoa_controls(g_view, control);
}

} // namespace ak

/* Internal header of the host AppKit.
 *
 * The object model is real AppKit: NSResponder -> NSView/NSWindow/
 * NSApplication, geometry in points with the origin at the bottom left,
 * `-drawRect:` and the responder chain. The platform underneath is
 * Wayland: one xdg-toplevel per NSWindow, EGL for the OpenGL contexts the
 * app draws with, wl_seat for events.
 *
 * Only the Wayland backend (`platform.cpp`) talks to the compositor; the
 * AppKit classes call it through the `platform` interface below, so the
 * object model stays testable without a display. */
#ifndef MACHOMAN_APPKIT_H
#define MACHOMAN_APPKIT_H

#include "machoman/objc_framework.h"
#include "machoman/runtime.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#ifdef MM_APPKIT_EVILCOCOA
#include <evilcocoa/renderer.h>
#endif
#ifdef MM_APPKIT_MEVK
#include <memory>
namespace mevk { class Texture; }
#endif

#define DARWIN(name) __attribute__((visibility("default"))) __asm__("_" #name)

namespace ak {

using namespace mmfw;

struct rect { double x = 0, y = 0, w = 0, h = 0; };
struct point { double x = 0, y = 0; };
struct size { double w = 0, h = 0; };

/* ---- the platform window a guest NSWindow is shown in ------------------- */

struct platform_window;
struct native_menu_item;
struct cursor_spec {
    std::string shape = "default";
    std::vector<std::uint32_t> pixels;
    int width = 0, height = 0, hotspot_x = 0, hotspot_y = 0;
    bool hidden = false;
    bool operator==(const cursor_spec &) const = default;
};

/* Events the backend delivers; translated into NSEvent objects by app.cpp. */
enum class event_kind { none, mouse_down, mouse_up, mouse_moved, mouse_dragged, scroll, key_down, key_up, flags_changed,
                        resized, close_requested, exposed, focus_in, focus_out, menu_action, menu_open };
struct platform_event {
    event_kind kind = event_kind::none;
    platform_window *window = nullptr;
    point where;            /* in window coordinates, bottom-left origin */
    double dx = 0, dy = 0;  /* scroll deltas */
    int button = 0;         /* 0 left, 1 right, 2 other */
    std::uint32_t keycode = 0, modifiers = 0;
    std::string characters, characters_ignoring_modifiers;
    size new_size;
    std::uintptr_t menu_token = 0;
};

/* The display connection. Everything is a no-op when there is none, so an
 * app can run headless (MACHOMAN_APPKIT_HEADLESS=1 or no compositor). */
struct platform {
    static bool available();
    static platform_window *create_window(id ns_window, rect frame, const std::string &title, bool decorated, bool resizable);
    static void destroy_window(platform_window *w);
    static void show_window(platform_window *w, bool show);
    static void set_title(platform_window *w, const std::string &title);
    static std::string dialog_parent(platform_window *w);
    static void set_parent(platform_window *w, platform_window *parent);
    static void set_frame(platform_window *w, rect frame);
    static void set_size_limits(platform_window *w, size minimum, size maximum);
    static rect window_frame(platform_window *w);
    static void set_menu(platform_window *w, const std::vector<native_menu_item> &items);
    static void set_cursor(const cursor_spec &cursor);
    static bool present_metal(platform_window *w, const void *layer, rect frame,
                              const void *bgra, std::uint32_t width, std::uint32_t height, std::uint64_t stride,
                              double contents_scale, unsigned placement);
    static void release_metal(const void *layer);
#ifdef MM_APPKIT_EVILCOCOA
    static void present_cocoa(platform_window *w, const std::vector<evilcocoa::item> &items);
#endif
#ifdef MM_APPKIT_MEVK
    static bool present_metal_texture(platform_window *w, const void *layer, rect frame,
        const std::shared_ptr<mevk::Texture> &texture, double contents_scale, unsigned placement);
#endif
    /* Waits up to `timeout_seconds` (negative: forever) for the next event;
     * false when nothing arrived. */
    static bool next_event(platform_event &out, double timeout_seconds);
    /* Wakes a blocked next_event from another thread. */
    static void wake();
    static size screen_size();
    static double screen_scale();
    static double screen_refresh();
    /* An EGL context for an NSOpenGLContext attached to `w`. */
    static void *create_gl_context(platform_window *w, void *share);
    static void destroy_gl_context(void *ctx);
    static bool make_current(void *ctx, platform_window *w);
    static void swap_buffers(void *ctx, platform_window *w);
    /* Introspection for the GL drawing probes. */
    static void *egl_display_of();
    static void *egl_surface_of(platform_window *w);
    static void set_swap_interval(void *ctx, int interval);
};

/* ---- object payloads ------------------------------------------------------ */

/* Every AppKit object here starts with the objc header, so the struct can
 * be placement-new'd over the instance (see objc_framework.h). */
struct responder_object { mm_objc_object base; id next = nullptr; };

/* Printable/editing keys belong to a focused text client before window
 * and application key equivalents. Command/Control shortcuts still run. */
bool text_input_has_priority(id window, id event);

struct event_object {
    mm_objc_object base;
    std::uint64_t type = 0, modifiers = 0, click_count = 0, button = 0, keycode = 0;
    double timestamp = 0, dx = 0, dy = 0;
    bool repeat = false;
    point where;
    id window = nullptr;
    std::string characters, characters_ignoring_modifiers;
};

struct color_object { mm_objc_object base; double r = 0, g = 0, b = 0, a = 1; };

struct view_object {
    mm_objc_object base;
    id next = nullptr;          /* NSResponder's field: views are responders */
    rect frame, bounds;
    id superview = nullptr, window = nullptr, layer = nullptr;
    std::vector<id> subviews;   /* retained, back to front */
    std::int64_t tag = 0;       /* how a program finds a view it built */
    unsigned contents_placement = 0;
    unsigned autoresizing_mask = 0;
    bool autoresizes_subviews = true, translates_mask = true;
    struct layout_constraint {
        id first = nullptr, second = nullptr;
        int attribute = 0, second_attribute = 0, relation = 0;
        double multiplier = 1, constant = 0, priority = 1000;
    };
    std::vector<layout_constraint> constraints;
    struct tooltip_rect { rect frame; id owner=nullptr; void *data=nullptr; std::int64_t tag=0; };
    std::vector<tooltip_rect> tooltips;
    std::int64_t next_tooltip_tag=1;
    bool needs_display = true, hidden = false, wants_layer = false;
    bool posts_frame_notifications = true, posts_bounds_notifications = false;
    ~view_object();
};

struct control_object {
    view_object view;
    id title = nullptr, key_equivalent = nullptr, target = nullptr, image = nullptr, font = nullptr;
    id placeholder = nullptr, identifier = nullptr, text_field = nullptr, image_view = nullptr;
    SEL action = nullptr, double_action = nullptr;
    std::int64_t state = 0, button_type = 0, bezel_style = 1, alignment = 0, image_scaling = 0;
    std::string font_family="Sans";
    double value = 0, minimum = 0, maximum = 1, knob = .25, font_size = 13;
    bool enabled = true, highlighted = false, bordered = true, bezeled = true;
    bool editable = false, selectable = false, draws_background = false, bold = false, wraps = false;
    bool indeterminate = false, animating = false, vertical = false;
    std::vector<id> choices;
    std::int64_t selected = -1;
    ~control_object();
};

struct table_column_object {
    mm_objc_object base;
    id identifier = nullptr, title = nullptr;
    double width = 120;
    ~table_column_object() { if (identifier) release(identifier); if (title) release(title); }
};
struct table_object {
    control_object control;
    id delegate = nullptr, data_source = nullptr;
    std::vector<id> columns, reusable_views;
    double row_height = 20;
    std::int64_t rows = 0, selected_row = -1;
    bool alternating = false;
    ~table_object();
};

void layout_cocoa_view(id view);
void register_cocoa_controls(Class view, Class control);

struct window_object {
    mm_objc_object base;
    id next = nullptr;
    rect frame;
    size minimum_size{}, maximum_size{};
    std::uint64_t style = 0;
    std::string title;
    id content_view = nullptr, delegate = nullptr, first_responder = nullptr;
    id parent_window = nullptr; /* weak; a parent retains its child windows */
    std::vector<id> child_windows;
    double alpha = 1;
    id mouse_targets[3] = {}; /* the view that received each button's down */
    platform_window *platform = nullptr;
    bool visible = false, key = false, needs_display = true, accepts_mouse_moved = false;
    bool pending_configure_notification = false;
    ~window_object();
};

struct layer_object {
    mm_objc_object base;
    rect bounds{}, frame{};
    point position{}, anchor{0.5, 0.5};
    double contents_scale = 1.0;
    unsigned contents_placement = 0; /* NSViewLayerContentsPlacement / CALayer.contentsGravity */
    id delegate = nullptr;
    id superlayer = nullptr;
    std::vector<id> sublayers{};
    id contents = nullptr;
    id gl_context = nullptr;
    id gl_format = nullptr;
    id view = nullptr;
    bool needs_display = true;
    std::uint64_t last_display_pass = 0;
    unsigned profile_frames = 0;
    double profile_update_ms = 0, profile_draw_ms = 0, profile_present_ms = 0;
    double profile_max_ms = 0, profile_report_time = 0;
    bool opaque = true;
    bool hidden = false;
    bool asynchronous = false;
    double last_frame_time = 0;

    ~layer_object() {
        platform::release_metal(this);
        for (id s : sublayers) {
            auto *child = as<layer_object>(s);
            child->superlayer = nullptr;
            platform::release_metal(child);
            mm_objc_release(s);
        }
        if (contents) mm_objc_release(contents);
        if (gl_context) mm_objc_release(gl_context);
        if (gl_format) mm_objc_release(gl_format);
    }
};

struct view_controller_object { mm_objc_object base; id next = nullptr; id view = nullptr; };

struct image_object {
    mm_objc_object base; std::string name; size size;
#ifdef MM_APPKIT_EVILCOCOA
    std::shared_ptr<const evilcocoa::bitmap> pixels;
    bool decoded = false;
#endif
};
#ifdef MM_APPKIT_EVILCOCOA
void decode_cocoa_image(id image);
#endif

/* NSGraphicsContext's current context, per process (AppKit's is per thread,
 * but nothing here draws from more than one thread yet). */
extern id g_current_graphics_context;
/* NSOpenGLContext's +currentContext. */
extern thread_local id g_current_gl_context;

/* Input state the class methods of NSEvent report. */
extern std::uint64_t g_pressed_buttons, g_modifiers;
extern point g_mouse;

/* NSEventType values (AppKit's enum). */
enum : std::uint64_t {
    ev_left_mouse_down = 1, ev_left_mouse_up = 2, ev_right_mouse_down = 3, ev_right_mouse_up = 4,
    ev_mouse_moved = 5, ev_left_mouse_dragged = 6, ev_right_mouse_dragged = 7, ev_mouse_entered = 8,
    ev_mouse_exited = 9, ev_key_down = 10, ev_key_up = 11, ev_flags_changed = 12, ev_app_defined = 15,
    ev_scroll_wheel = 22, ev_other_mouse_down = 25, ev_other_mouse_up = 26, ev_other_mouse_dragged = 27,
};

/* Builds an autoreleased NSEvent. */
id make_event(std::uint64_t type, id window, point where, std::uint64_t modifiers);

/* The shared NSApplication; this is the exported `NSApp` variable itself,
 * which the guest reads directly. */
extern id &g_NSApp;
void application_add_window(id window);
void register_file_panels();
void register_window_controller();
void poll_file_panels();
bool file_panel_blocks_window(id window);
void application_dialog_tick(id owner);
#ifdef MM_APPKIT_EVILCOCOA
void render_cocoa_window(id window);
#endif
void application_remove_window(id window);
void application_window_closed();
void application_order_front(id window);
void application_order_out(id window);
/* The NSWindow a platform window belongs to. */
id platform_window_owner(platform_window *w);

/* ---- helpers ------------------------------------------------------------- */

const mm_host_api *api();
bool trace();
std::string to_utf8(id nsstring);          /* through NSString's own methods */
id string(const std::string &utf8);        /* autoreleased NSString */
id number(double v);                       /* autoreleased NSNumber */

/* Builds an NSWindow from host code. A host-to-host message cannot carry
 * the doubles of an NSRect — those travel in the guest's registers — so
 * anything in AppKit that needs a window asks for one here. */
id make_window(const rect &frame, std::uint64_t style);

/* Loads a nib from the main bundle (see nib.cpp). */
bool load_nib(const std::string &name, id owner, std::vector<id> *top_level);

void ensure_platform_window(id window);
id make_backing_layer(id view);
void draw_layers(id view);
/* A layer drawn by displayIfNeeded must not be drawn again by the
 * asynchronous-layer traversal in the same window redraw pass. */
struct layer_display_pass {
    std::uint64_t previous;
    layer_display_pass();
    ~layer_display_pass();
};

/* Registration, one per translation unit. */
void register_responder();   /* NSResponder, NSEvent, NSCursor, NSColor, ... */
void application_cursor_moved();
std::uint64_t begin_dragging_session(id view, SEL, id items, id event, id source);
bool dragging_session_event(id event);
void cancel_dragging_session(id window);
void register_views();       /* NSView, NSWindow, NSViewController, NSGraphicsContext */
void register_application(); /* NSApplication, NSScreen, NSMenu, NSWorkspace, ... */
void register_opengl();      /* NSOpenGLContext, NSOpenGLPixelFormat */
void register_layers();      /* CALayer, NSOpenGLLayer, CATransaction */

} // namespace ak

#endif

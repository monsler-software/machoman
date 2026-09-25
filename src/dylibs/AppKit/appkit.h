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

#define DARWIN(name) __attribute__((visibility("default"))) __asm__("_" #name)

namespace ak {

using namespace mmfw;

struct rect { double x = 0, y = 0, w = 0, h = 0; };
struct point { double x = 0, y = 0; };
struct size { double w = 0, h = 0; };

/* ---- the platform window a guest NSWindow is shown in ------------------- */

struct platform_window;

/* Events the backend delivers; translated into NSEvent objects by app.cpp. */
enum class event_kind { none, mouse_down, mouse_up, mouse_moved, mouse_dragged, scroll, key_down, key_up, flags_changed,
                        resized, close_requested, exposed, focus_in, focus_out };
struct platform_event {
    event_kind kind = event_kind::none;
    platform_window *window = nullptr;
    point where;            /* in window coordinates, bottom-left origin */
    double dx = 0, dy = 0;  /* scroll deltas */
    int button = 0;         /* 0 left, 1 right, 2 other */
    std::uint32_t keycode = 0, modifiers = 0;
    std::string characters;
    size new_size;
};

/* The display connection. Everything is a no-op when there is none, so an
 * app can run headless (MACHOMAN_APPKIT_HEADLESS=1 or no compositor). */
struct platform {
    static bool available();
    static platform_window *create_window(id ns_window, rect frame, const std::string &title, bool decorated);
    static void destroy_window(platform_window *w);
    static void show_window(platform_window *w, bool show);
    static void set_title(platform_window *w, const std::string &title);
    static void set_frame(platform_window *w, rect frame);
    static rect window_frame(platform_window *w);
    /* Waits up to `timeout_seconds` (negative: forever) for the next event;
     * false when nothing arrived. */
    static bool next_event(platform_event &out, double timeout_seconds);
    /* Wakes a blocked next_event from another thread. */
    static void wake();
    static size screen_size();
    static double screen_scale();
    /* An EGL context for an NSOpenGLContext attached to `w`. */
    static void *create_gl_context(platform_window *w, void *share);
    static void destroy_gl_context(void *ctx);
    static bool make_current(void *ctx, platform_window *w);
    static void swap_buffers(void *ctx, platform_window *w);
    static void set_swap_interval(void *ctx, int interval);
};

/* ---- object payloads ------------------------------------------------------ */

/* Every AppKit object here starts with the objc header, so the struct can
 * be placement-new'd over the instance (see objc_framework.h). */
struct responder_object { mm_objc_object base; id next = nullptr; };

struct event_object {
    mm_objc_object base;
    std::uint64_t type = 0, modifiers = 0, click_count = 0, button = 0, keycode = 0;
    double timestamp = 0, dx = 0, dy = 0;
    bool repeat = false;
    point where;
    id window = nullptr;
    std::string characters;
};

struct color_object { mm_objc_object base; double r = 0, g = 0, b = 0, a = 1; };

struct view_object {
    mm_objc_object base;
    id next = nullptr;          /* NSResponder's field: views are responders */
    rect frame, bounds;
    id superview = nullptr, window = nullptr, layer = nullptr;
    std::vector<id> subviews;   /* retained, back to front */
    std::int64_t tag = 0;       /* how a program finds a view it built */
    bool needs_display = true, hidden = false, wants_layer = false;
};

struct window_object {
    mm_objc_object base;
    id next = nullptr;
    rect frame;
    std::uint64_t style = 0;
    std::string title;
    id content_view = nullptr, delegate = nullptr, first_responder = nullptr;
    platform_window *platform = nullptr;
    bool visible = false, key = false, needs_display = true, accepts_mouse_moved = false;
};

struct view_controller_object { mm_objc_object base; id next = nullptr; id view = nullptr; };

struct image_object { mm_objc_object base; std::string name; size size; };

/* NSGraphicsContext's current context, per process (AppKit's is per thread,
 * but nothing here draws from more than one thread yet). */
extern id g_current_graphics_context;
/* NSOpenGLContext's +currentContext. */
extern id g_current_gl_context;

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
void application_remove_window(id window);
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

/* Registration, one per translation unit. */
void register_responder();   /* NSResponder, NSEvent, NSCursor, NSColor, ... */
void register_views();       /* NSView, NSWindow, NSViewController, NSGraphicsContext */
void register_application(); /* NSApplication, NSScreen, NSMenu, NSWorkspace, ... */
void register_opengl();      /* NSOpenGLContext, NSOpenGLPixelFormat, NSOpenGLLayer */

} // namespace ak

#endif

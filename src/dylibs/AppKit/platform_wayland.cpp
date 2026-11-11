/* The Wayland backend: one xdg-toplevel per NSWindow, EGL for the OpenGL
 * contexts the app draws with, wl_seat for input.
 *
 * Coordinates are the compositor's (origin at the top left, surface-local),
 * AppKit's are bottom-left in points, so every position crossing this
 * boundary is flipped against the window height here rather than in the
 * AppKit classes.
 *
 * The connection is opened lazily on the first window and everything
 * degrades to "no display" if that fails, which is what `available()`
 * reports. */
#include "appkit.h"
#ifdef MM_APPKIT_XDG_FOREIGN
#include "xdg-foreign-v2-client-protocol.h"
#endif
#include "platform_events.h"
#include "app_icon.h"
#include "native_menu.h"
#include "metal_presentation.h"
#ifdef MM_APPKIT_MEVK
#include "mevk/present.h"
#endif

#include <algorithm>
#include <chrono>
#include <atomic>
#include "keyboard.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <deque>
#include <mutex>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <EGL/egl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"
#ifdef MM_APPKIT_XDG_ICON
#include "xdg-toplevel-icon-v1-client-protocol.h"
#endif

namespace ak {

struct metal_shm_buffer {
    wl_buffer *buffer = nullptr;
    void *pixels = nullptr;
    std::size_t bytes = 0;
    int width = 0, height = 0;
    bool busy = false;
    void reset() {
        if (buffer) wl_buffer_destroy(buffer);
        if (pixels) munmap(pixels, bytes);
        buffer = nullptr; pixels = nullptr; bytes = 0; busy = false;
    }
    ~metal_shm_buffer() { reset(); }
};
struct metal_surface {
    wl_surface *surface = nullptr;
    wl_subsurface *subsurface = nullptr;
    std::array<metal_shm_buffer, 3> buffers;
#ifdef MM_APPKIT_MEVK
    std::unique_ptr<mevk::WaylandPresenter> presenter;
    bool gpu_failed = false;
    std::chrono::steady_clock::time_point last_report{};
#endif
    ~metal_surface() {
#ifdef MM_APPKIT_MEVK
        presenter.reset(); // WSI must release its borrowed wl_surface first.
#endif
        if (subsurface) wl_subsurface_destroy(subsurface);
        if (surface) wl_surface_destroy(surface);
    }
};

struct platform_window {
    metal_shm_buffer icon_buffer;
    std::array<metal_shm_buffer, 3> shell_buffers;
    std::unique_ptr<metal_surface> menu_popup;
    native_menu menu;
    bool menu_eligible = false;
    int menu_height = 0;
    wl_surface *content_surface = nullptr;
    wl_subsurface *content_subsurface = nullptr;
    id owner = nullptr;
    wl_surface *surface = nullptr;
    xdg_surface *shell_surface = nullptr;
    xdg_toplevel *toplevel = nullptr;
#ifdef MM_APPKIT_XDG_FOREIGN
    zxdg_exported_v2 *exported = nullptr;
    std::string exported_handle;
#endif
    wl_egl_window *egl_window = nullptr;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    /* Software backing store, for a window whose content is not drawn by
     * an OpenGL context: without a buffer attached the compositor never
     * maps the surface, so the window would not appear at all. */
    wl_buffer *shm_buffer = nullptr;
    std::uint32_t *shm_pixels = nullptr;
    std::size_t shm_bytes = 0;
    bool gl_attached = false;
#ifdef MM_APPKIT_EVILCOCOA
    EGLContext cocoa_context = EGL_NO_CONTEXT;
    std::unique_ptr<evilcocoa::renderer> cocoa_renderer;
    std::vector<evilcocoa::item> cocoa_items;
    int cocoa_width = 0, cocoa_height = 0;
    bool guest_gl = false;
#endif
    std::unordered_map<const void *, std::unique_ptr<metal_surface>> metal_layers;
    rect frame;                 /* AppKit points */
    size minimum_size{}, maximum_size{};
    std::string title;
    bool visible = false, configured = false;
    bool resizable = true;
    size pending_size;          /* from the last xdg_toplevel configure */
};

namespace {

/* ---- the connection ------------------------------------------------------ */

struct display_state {
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_compositor *compositor = nullptr;
    wl_subcompositor *subcompositor = nullptr;
    xdg_wm_base *wm_base = nullptr;
#ifdef MM_APPKIT_XDG_FOREIGN
    zxdg_exporter_v2 *exporter = nullptr;
#endif
#ifdef MM_APPKIT_XDG_ICON
    xdg_toplevel_icon_manager_v1 *icon_manager = nullptr;
#endif
    wl_seat *seat = nullptr;
    wl_shm *shm = nullptr;
    wl_event_queue *metal_queue = nullptr;
    wl_pointer *pointer = nullptr;
    wl_keyboard *keyboard = nullptr;
    wl_cursor_theme *cursor_theme = nullptr;
    wl_surface *cursor_surface = nullptr;
    cursor_spec cursor;
    std::array<metal_shm_buffer, 3> cursor_buffers;
    std::uint32_t pointer_enter_serial = 0;
    std::uint64_t cursor_generation = 1, cursor_applied_generation = 0;
    std::uint32_t cursor_applied_serial = 0;
    bool cursor_applied_chrome = false;
    wl_output *output = nullptr;

    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLConfig egl_config = nullptr;

    xkb_context *xkb = nullptr;
    xkb_keymap *keymap = nullptr;
    xkb_state *xkb_state_ = nullptr;

    size screen{1920, 1080};
    double scale = 1.0;
    std::atomic<double> refresh{60.0};

    std::vector<platform_window *> windows;
    std::deque<platform_event> queue;
    std::mutex queue_mu;
    // Metal buffer release events have their own queue, dispatched under this
    // lock. AppKit can keep reading input while a guest render thread presents.
    std::recursive_mutex surface_mu;

    /* Input state, so an event carries what AppKit expects to read from it. */
    platform_window *pointer_window = nullptr;
    platform_window *keyboard_window = nullptr;
    point pointer_at;
    std::uint64_t modifiers = 0;
    std::uint32_t modifier_keycode = 0;
    int buttons_down = 0;

    bool failed = false;
};

display_state &state() {
    static display_state s;
    return s;
}

/* A protocol error kills the connection silently otherwise, and every
 * later call turns into a no-op with no explanation. */
void check_protocol_error() {
    display_state &s = state();
    if (!s.display || s.failed) return;
    const int err = wl_display_get_error(s.display);
    if (!err) return;
    std::uint32_t code = 0;
    const wl_interface *iface = nullptr;
    std::uint32_t id = 0;
    if (err == EPROTO) code = wl_display_get_protocol_error(s.display, &iface, &id);
    std::fprintf(stderr, "AppKit: the Wayland connection failed: %s%s%s (code %u, object %u)\n", std::strerror(err),
                 iface ? ", interface " : "", iface ? iface->name : "", code, id);
    s.failed = true;
}

void push(const platform_event &e) {
    display_state &s = state();
    std::lock_guard lock(s.queue_mu);
    enqueue_platform_event(s.queue, e);
}

platform_window *window_of_surface(wl_surface *surface) {
    if (!surface) return nullptr;
    for (platform_window *w : state().windows)
        if (w->surface == surface) return w;
    return nullptr;
}

/* Wayland's y grows downwards; AppKit's does not. */
double flip_y(const platform_window *w, double y) { return w ? w->frame.h + w->menu_height - y : y; }

/* ---- NSEvent modifier flags ---------------------------------------------- */

constexpr std::uint64_t mod_shift = 1ull << 17, mod_control = 1ull << 18, mod_option = 1ull << 19,
                        mod_command = 1ull << 20, mod_caps = 1ull << 16;

std::uint64_t modifiers_from_xkb() {
    display_state &s = state();
    if (!s.xkb_state_) return 0;
    std::uint64_t m = 0;
    auto active = [&](const char *name) {
        return xkb_state_mod_name_is_active(s.xkb_state_, name, XKB_STATE_MODS_EFFECTIVE) > 0;
    };
    if (active(XKB_MOD_NAME_SHIFT)) m |= mod_shift;
    if (active(XKB_MOD_NAME_CTRL)) m |= mod_control;
    if (active(XKB_MOD_NAME_ALT)) m |= mod_option;
    if (active(XKB_MOD_NAME_LOGO)) m |= mod_command;
    if (active(XKB_MOD_NAME_CAPS)) m |= mod_caps;
    const char *mapping = std::getenv("MACHOMAN_NATIVE_SHORTCUTS");
    return native_shortcut_modifiers(m, !mapping || std::strcmp(mapping, "0") != 0);
}

/* Defined below, next to the rest of the software backing store. */
void update_shm_buffer(platform_window *w);
void update_menu_popup(platform_window *w);
void apply_platform_cursor();

/* ---- listeners ------------------------------------------------------------ */

void pointer_enter(void *, wl_pointer *, std::uint32_t serial, wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    display_state &s = state();
    s.pointer_window = window_of_surface(surface);
    s.pointer_enter_serial = serial;
    s.pointer_at = {wl_fixed_to_double(x), flip_y(s.pointer_window, wl_fixed_to_double(y))};
    apply_platform_cursor();
}
void pointer_leave(void *, wl_pointer *, std::uint32_t, wl_surface *) { state().pointer_window = nullptr; }
void pointer_motion(void *, wl_pointer *, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
    display_state &s = state();
    s.pointer_at = {wl_fixed_to_double(x), flip_y(s.pointer_window, wl_fixed_to_double(y))};
    apply_platform_cursor();
    platform_event e;
    e.kind = s.buttons_down ? event_kind::mouse_dragged : event_kind::mouse_moved;
    e.window = s.pointer_window;
    e.where = s.pointer_at;
    e.modifiers = s.modifiers;
    push(e);
}
void pointer_button(void *, wl_pointer *, std::uint32_t, std::uint32_t, std::uint32_t button, std::uint32_t pressed) {
    display_state &s = state();
    /* linux/input-event-codes.h: BTN_LEFT 0x110, BTN_RIGHT 0x111, BTN_MIDDLE 0x112 */
    const int index = button == 0x111 ? 1 : button == 0x112 ? 2 : 0;
    platform_event e;
    e.kind = pressed ? event_kind::mouse_down : event_kind::mouse_up;
    e.window = s.pointer_window;
    e.where = s.pointer_at;
    e.button = index;
    e.modifiers = s.modifiers;
    if (pressed) s.buttons_down++;
    else if (s.buttons_down) s.buttons_down--;
    push(e);
}
void pointer_axis(void *, wl_pointer *, std::uint32_t, std::uint32_t axis, wl_fixed_t value) {
    display_state &s = state();
    platform_event e;
    e.kind = event_kind::scroll;
    e.window = s.pointer_window;
    e.where = s.pointer_at;
    e.modifiers = s.modifiers;
    /* Wayland's axis value grows the way the content moves; AppKit's
     * scrollingDelta grows the opposite way. */
    if (axis == 0) e.dy = -wl_fixed_to_double(value);
    else e.dx = -wl_fixed_to_double(value);
    push(e);
}
void pointer_frame(void *, wl_pointer *) {}
void pointer_axis_source(void *, wl_pointer *, std::uint32_t) {}
void pointer_axis_stop(void *, wl_pointer *, std::uint32_t, std::uint32_t) {}
void pointer_axis_discrete(void *, wl_pointer *, std::uint32_t, std::int32_t) {}
void pointer_axis_value120(void *, wl_pointer *, std::uint32_t, std::int32_t) {}
void pointer_axis_relative_direction(void *, wl_pointer *, std::uint32_t, std::uint32_t) {}
void pointer_warp(void *, wl_pointer *, wl_fixed_t, wl_fixed_t) {}

/* Designated initializers, so a newer wl_pointer_listener (which keeps
 * gaining events) still compiles with the handlers we implement. */
const wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
    .axis_relative_direction = pointer_axis_relative_direction,
    .warp = pointer_warp,
};

void keyboard_keymap(void *, wl_keyboard *, std::uint32_t format, std::int32_t fd, std::uint32_t size) {
    display_state &s = state();
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *map = static_cast<char *>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (map != MAP_FAILED) {
        if (s.keymap) xkb_keymap_unref(s.keymap);
        if (s.xkb_state_) xkb_state_unref(s.xkb_state_);
        s.keymap = xkb_keymap_new_from_string(s.xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
        s.xkb_state_ = s.keymap ? xkb_state_new(s.keymap) : nullptr;
        munmap(map, size);
    }
    close(fd);
}
void keyboard_enter(void *, wl_keyboard *, std::uint32_t, wl_surface *surface, wl_array *) {
    display_state &s = state();
    s.keyboard_window = window_of_surface(surface);
    platform_event e;
    e.kind = event_kind::focus_in;
    e.window = s.keyboard_window;
    push(e);
}
void keyboard_leave(void *, wl_keyboard *, std::uint32_t, wl_surface *surface) {
    platform_event e;
    e.kind = event_kind::focus_out;
    e.window = window_of_surface(surface);
    state().keyboard_window = nullptr;
    push(e);
}
void keyboard_key(void *, wl_keyboard *, std::uint32_t, std::uint32_t, std::uint32_t key, std::uint32_t pressed) {
    display_state &s = state();
    const std::uint32_t code = key + 8; /* evdev -> xkb */
    platform_event e;
    e.kind = pressed ? event_kind::key_down : event_kind::key_up;
    e.window = s.keyboard_window;
    e.where = s.pointer_at;
    const char *mapping = std::getenv("MACHOMAN_NATIVE_SHORTCUTS");
    e.keycode = native_shortcut_keycode(mac_keycode_from_evdev(key), !mapping || std::strcmp(mapping, "0") != 0);
    if (e.keycode >= 54 && e.keycode <= 62) {
        s.modifier_keycode = e.keycode;
        return; // modifier transitions are delivered by keyboard_modifiers
    }
    e.modifiers = s.modifiers;
    if (s.xkb_state_) {
        char buf[64];
        const int n = xkb_state_key_get_utf8(s.xkb_state_, code, buf, sizeof buf);
        if (n > 0) e.characters.assign(buf, static_cast<std::size_t>(n));
        /* The keysym keeps Shift/Caps and layout, without XKB's Control
         * transformation of e.g. Ctrl+A into U+0001. */
        const auto sym = xkb_state_key_get_one_sym(s.xkb_state_, code);
        const int plain = xkb_keysym_to_utf8(sym, buf, sizeof buf);
        if (plain > 1) e.characters_ignoring_modifiers.assign(buf, static_cast<std::size_t>(plain - 1));
        // XKB applies host Control before the Cmd swap; remove its control
        // character conversion when that key now represents Darwin Command.
        if ((e.modifiers & mod_command) && !(e.modifiers & mod_control)) e.characters = e.characters_ignoring_modifiers;
        std::uint32_t special = 0;
        switch (sym) {
        case XKB_KEY_BackSpace: special = 0x7f; break;
        case XKB_KEY_Up: special = 0xf700; break;
        case XKB_KEY_Down: special = 0xf701; break;
        case XKB_KEY_Left: special = 0xf702; break;
        case XKB_KEY_Right: special = 0xf703; break;
        case XKB_KEY_Delete: special = 0xf728; break;
        case XKB_KEY_Home: special = 0xf729; break;
        case XKB_KEY_End: special = 0xf72b; break;
        case XKB_KEY_Page_Up: special = 0xf72c; break;
        case XKB_KEY_Page_Down: special = 0xf72d; break;
        default: if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F35) special = 0xf704 + sym - XKB_KEY_F1;
        }
        if (special) {
            std::string text;
            if (special < 0x80) text += static_cast<char>(special);
            else {
                text += static_cast<char>(0xe0 | (special >> 12));
                text += static_cast<char>(0x80 | ((special >> 6) & 0x3f));
                text += static_cast<char>(0x80 | (special & 0x3f));
            }
            e.characters = e.characters_ignoring_modifiers = text;
        }
    }
    push(e);
}
void keyboard_modifiers(void *, wl_keyboard *, std::uint32_t, std::uint32_t depressed, std::uint32_t latched,
                        std::uint32_t locked, std::uint32_t group) {
    display_state &s = state();
    if (s.xkb_state_) xkb_state_update_mask(s.xkb_state_, depressed, latched, locked, 0, 0, group);
    s.modifiers = modifiers_from_xkb();
    platform_event e;
    e.kind = event_kind::flags_changed;
    e.window = s.keyboard_window;
    e.keycode = s.modifier_keycode;
    e.modifiers = s.modifiers;
    push(e);
}
void keyboard_repeat_info(void *, wl_keyboard *, std::int32_t, std::int32_t) {}

const wl_keyboard_listener keyboard_listener = {
    keyboard_keymap, keyboard_enter, keyboard_leave, keyboard_key, keyboard_modifiers, keyboard_repeat_info,
};

void seat_capabilities(void *, wl_seat *seat, std::uint32_t caps) {
    display_state &s = state();
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !s.pointer) {
        s.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(s.pointer, &pointer_listener, nullptr);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !s.keyboard) {
        s.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(s.keyboard, &keyboard_listener, nullptr);
    }
}
void seat_name(void *, wl_seat *, const char *) {}
const wl_seat_listener seat_listener = {seat_capabilities, seat_name};

void output_geometry(void *, wl_output *, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                     const char *, const char *, std::int32_t) {}
void output_mode(void *, wl_output *, std::uint32_t flags, std::int32_t width, std::int32_t height, std::int32_t refresh) {
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        state().screen = {static_cast<double>(width), static_cast<double>(height)};
        if (refresh > 0) state().refresh = static_cast<double>(refresh) / 1000;
    }
}
void output_done(void *, wl_output *) {}
void output_scale(void *, wl_output *, std::int32_t factor) { state().scale = factor > 0 ? factor : 1; }
void output_name(void *, wl_output *, const char *) {}
void output_description(void *, wl_output *, const char *) {}
const wl_output_listener output_listener = {output_geometry, output_mode, output_done, output_scale,
                                            output_name, output_description};

void wm_base_ping(void *, xdg_wm_base *base, std::uint32_t serial) { xdg_wm_base_pong(base, serial); }
const xdg_wm_base_listener wm_base_listener = {wm_base_ping};
#ifdef MM_APPKIT_XDG_ICON
void icon_size(void *, xdg_toplevel_icon_manager_v1 *, std::int32_t) {}
void icon_done(void *, xdg_toplevel_icon_manager_v1 *) {}
const xdg_toplevel_icon_manager_v1_listener icon_listener = {icon_size, icon_done};
#endif

void registry_global(void *, wl_registry *registry, std::uint32_t name, const char *interface, std::uint32_t version) {
    display_state &s = state();
    const std::string_view i(interface);
    if (i == wl_compositor_interface.name)
        s.compositor = static_cast<wl_compositor *>(wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4u)));
    else if (i == wl_subcompositor_interface.name)
        s.subcompositor = static_cast<wl_subcompositor *>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
#ifdef MM_APPKIT_XDG_ICON
    else if (i == xdg_toplevel_icon_manager_v1_interface.name) {
        s.icon_manager = static_cast<xdg_toplevel_icon_manager_v1 *>(wl_registry_bind(registry, name, &xdg_toplevel_icon_manager_v1_interface, 1));
        xdg_toplevel_icon_manager_v1_add_listener(s.icon_manager, &icon_listener, nullptr);
    }
#endif
    else if (i == xdg_wm_base_interface.name) {
        s.wm_base = static_cast<xdg_wm_base *>(wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 3u)));
        xdg_wm_base_add_listener(s.wm_base, &wm_base_listener, nullptr);
    }
#ifdef MM_APPKIT_XDG_FOREIGN
    else if (i == zxdg_exporter_v2_interface.name) {
        s.exporter = static_cast<zxdg_exporter_v2 *>(wl_registry_bind(registry, name, &zxdg_exporter_v2_interface, 1));
    }
#endif
    else if (i == wl_seat_interface.name) {
        s.seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
        wl_seat_add_listener(s.seat, &seat_listener, nullptr);
    } else if (i == wl_shm_interface.name) {
        s.shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (i == wl_output_interface.name && !s.output) {
        s.output = static_cast<wl_output *>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 2u)));
        wl_output_add_listener(s.output, &output_listener, nullptr);
    }
}
void registry_global_remove(void *, wl_registry *, std::uint32_t) {}
const wl_registry_listener registry_listener = {registry_global, registry_global_remove};

void surface_configure(void *data, xdg_surface *surface, std::uint32_t serial) {
    xdg_surface_ack_configure(surface, serial);
    auto *w = static_cast<platform_window *>(data);
    w->configured = true;
    if (w->pending_size.w > 0 && w->pending_size.h > 0 &&
        (w->pending_size.w != w->frame.w || w->pending_size.h != w->frame.h)) {
        w->frame.w = w->pending_size.w;
        w->frame.h = w->pending_size.h;
        if (w->egl_window)
            wl_egl_window_resize(w->egl_window, static_cast<int>(w->frame.w), static_cast<int>(w->frame.h), 0, 0);
        if (w->visible) update_shm_buffer(w);
        platform_event e;
        e.kind = event_kind::resized;
        e.window = w;
        e.new_size = {w->frame.w, w->frame.h};
        push(e);
    }
    if (w->visible) update_shm_buffer(w);
    w->pending_size = {}; /* a later state-only configure has no old size */
    platform_event e;
    e.kind = event_kind::exposed;
    e.window = w;
    push(e);
}
const xdg_surface_listener shell_surface_listener = {surface_configure};

void toplevel_configure(void *data, xdg_toplevel *, std::int32_t width, std::int32_t height, wl_array *) {
    auto *w = static_cast<platform_window *>(data);
    w->pending_size = {width > 0 ? static_cast<double>(width) : w->frame.w,
                       height > 0 ? std::max(1., static_cast<double>(height) - w->menu_height) : w->frame.h};
}
void toplevel_close(void *data, xdg_toplevel *) {
    platform_event e;
    e.kind = event_kind::close_requested;
    e.window = static_cast<platform_window *>(data);
    push(e);
}
void toplevel_configure_bounds(void *, xdg_toplevel *, std::int32_t, std::int32_t) {}
void toplevel_wm_capabilities(void *, xdg_toplevel *, wl_array *) {}
const xdg_toplevel_listener toplevel_listener = {toplevel_configure, toplevel_close, toplevel_configure_bounds,
                                                 toplevel_wm_capabilities};

/* An EGL surface is created only when a GL context is actually attached to
 * the window: a surface that both EGL and our shm code attach buffers to is
 * a protocol error, and most windows never draw with OpenGL. */
bool ensure_egl_surface(platform_window *w) {
    display_state &s = state();
    if (!w || !s.display) return false;
    if (w->egl_surface != EGL_NO_SURFACE) return true;
    if (!w->content_surface) {
        if (w->shm_buffer) { wl_buffer_destroy(w->shm_buffer); w->shm_buffer = nullptr; }
        if (w->shm_pixels) { munmap(w->shm_pixels, w->shm_bytes); w->shm_pixels = nullptr; w->shm_bytes = 0; }
    }
    w->gl_attached = true;
    w->egl_window = wl_egl_window_create(w->content_surface ? w->content_surface : w->surface, std::max(1, static_cast<int>(w->frame.w)),
                                         std::max(1, static_cast<int>(w->frame.h)));
    if (!w->egl_window) return false;
    w->egl_surface = eglCreateWindowSurface(s.egl_display, s.egl_config,
                                            reinterpret_cast<EGLNativeWindowType>(w->egl_window), nullptr);
    if (w->egl_surface == EGL_NO_SURFACE) return false;
    if (s.compositor && w->surface) {
        wl_region *region = wl_compositor_create_region(s.compositor);
        if (region) {
            const int width = std::max(1, static_cast<int>(w->frame.w));
            const int height = std::max(1, static_cast<int>(w->frame.h));
            wl_region_add(region, 0, 0, width, height);
            wl_surface_set_opaque_region(w->content_surface ? w->content_surface : w->surface, region);
            wl_region_destroy(region);
        }
    }
    return true;
}

/* ---- software backing store ------------------------------------------------ */

/* Allocates (or reallocates) the shm buffer for a window's current size and
 * fills it with the window background, then attaches it. Views that draw
 * through OpenGL never take this path. */
metal_shm_buffer *cpu_buffer(std::array<metal_shm_buffer, 3> &buffers, int width, int height) {
    auto &s = state();
    if (width < 1 || height < 1 || width > 16384 || height > 16384) return nullptr;
    const auto bytes = std::size_t(width) * height * 4;
    if (bytes > 256 * 1024 * 1024) return nullptr;
    metal_shm_buffer *slot = nullptr;
    for (auto &b : buffers) if (!b.busy) { slot = &b; break; }
    if (!slot) return nullptr;
    if (slot->width != width || slot->height != height || !slot->buffer) {
        slot->reset(); const int fd = memfd_create("machoman-window-chrome", MFD_CLOEXEC);
        if (fd < 0) return nullptr;
        if (ftruncate(fd, static_cast<off_t>(bytes))) { close(fd); return nullptr; }
        void *pixels = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (pixels == MAP_FAILED) { close(fd); return nullptr; }
        auto *pool = wl_shm_create_pool(s.shm, fd, static_cast<int>(bytes));
        slot->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool); close(fd);
        slot->pixels = pixels; slot->bytes = bytes; slot->width = width; slot->height = height;
        static const wl_buffer_listener release = {[](void *data, wl_buffer *) { static_cast<metal_shm_buffer *>(data)->busy = false; }};
        wl_buffer_add_listener(slot->buffer, &release, slot);
    }
    return slot;
}
void commit_cpu(wl_surface *surface, metal_shm_buffer *slot) {
    slot->busy = true; wl_surface_attach(surface, slot->buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, slot->width, slot->height); wl_surface_commit(surface);
}
void apply_platform_cursor() {
    auto &s = state();
    if (!s.pointer || !s.pointer_window || !s.pointer_enter_serial || !s.compositor || !s.shm) return;
    const auto *w = s.pointer_window;
    const bool chrome = w->menu_height && (s.pointer_at.y >= w->frame.h || w->menu.opened());
    if (s.cursor_applied_serial == s.pointer_enter_serial && s.cursor_applied_generation == s.cursor_generation && s.cursor_applied_chrome == chrome) return;
    auto applied = [&] { s.cursor_applied_serial = s.pointer_enter_serial; s.cursor_applied_generation = s.cursor_generation; s.cursor_applied_chrome = chrome; };
    if (s.cursor.hidden && !chrome) { wl_pointer_set_cursor(s.pointer, s.pointer_enter_serial, nullptr, 0, 0); wl_display_flush(s.display); applied(); return; }
    if (!s.cursor_surface) s.cursor_surface = wl_compositor_create_surface(s.compositor);
    if (!chrome && !s.cursor.pixels.empty() && s.cursor.width > 0 && s.cursor.height > 0 &&
        s.cursor.pixels.size() == std::size_t(s.cursor.width) * s.cursor.height) {
        auto *slot = cpu_buffer(s.cursor_buffers, s.cursor.width, s.cursor.height);
        if (slot) {
            std::memcpy(slot->pixels, s.cursor.pixels.data(), slot->bytes);
            wl_pointer_set_cursor(s.pointer, s.pointer_enter_serial, s.cursor_surface, s.cursor.hotspot_x, s.cursor.hotspot_y);
            commit_cpu(s.cursor_surface, slot); wl_display_flush(s.display); applied();
        }
        return;
    }
    if (!s.cursor_theme) s.cursor_theme = wl_cursor_theme_load(std::getenv("XCURSOR_THEME"), 24, s.shm);
    if (!s.cursor_theme) return;
    const auto name = chrome ? "default" : s.cursor.shape.c_str();
    auto *cursor = wl_cursor_theme_get_cursor(s.cursor_theme, name);
    if (!cursor) cursor = wl_cursor_theme_get_cursor(s.cursor_theme, "left_ptr");
    if (!cursor || !cursor->image_count) return;
    auto *image = cursor->images[0]; auto *buffer = wl_cursor_image_get_buffer(image);
    if (!buffer) return;
    wl_pointer_set_cursor(s.pointer, s.pointer_enter_serial, s.cursor_surface, image->hotspot_x, image->hotspot_y);
    wl_surface_attach(s.cursor_surface, buffer, 0, 0);
    wl_surface_damage_buffer(s.cursor_surface, 0, 0, image->width, image->height); wl_surface_commit(s.cursor_surface); wl_display_flush(s.display);
    applied();
}
void update_menu_popup(platform_window *w) {
    auto &s = state();
    if (!w->menu.opened()) {
        if (w->menu_popup) { wl_surface_attach(w->menu_popup->surface, nullptr, 0, 0); wl_surface_commit(w->menu_popup->surface); }
        return;
    }
    if (!w->menu_popup) {
        w->menu_popup = std::make_unique<metal_surface>();
        auto &popup = *w->menu_popup;
        popup.surface = wl_compositor_create_surface(s.compositor);
        popup.subsurface = wl_subcompositor_get_subsurface(s.subcompositor, popup.surface, w->surface);
        wl_subsurface_set_desync(popup.subsurface);
        auto *empty = wl_compositor_create_region(s.compositor);
        wl_surface_set_input_region(popup.surface, empty); wl_region_destroy(empty);
    }
    auto *slot = cpu_buffer(w->menu_popup->buffers, std::max(1, int(w->frame.w)), std::max(1, int(w->frame.h) + w->menu_height));
    if (!slot) { w->menu.dirty = true; return; }
    w->menu.paint({static_cast<std::uint32_t *>(slot->pixels), slot->bytes / 4}, true);
    commit_cpu(w->menu_popup->surface, slot);
}
void update_shm_buffer(platform_window *w) {
    display_state &s = state();
    if (w->content_surface && s.shm) {
        const int width = std::max(1, int(w->frame.w)), height = std::max(1, int(w->frame.h) + w->menu_height);
        w->menu.resize(width, height);
        auto *slot = cpu_buffer(w->shell_buffers, width, height);
        if (!slot) { w->menu.dirty = true; return; }
        if (w->menu_height) w->menu.paint({static_cast<std::uint32_t *>(slot->pixels), slot->bytes / 4}, false);
        else std::fill_n(static_cast<std::uint32_t *>(slot->pixels), slot->bytes / 4, 0xff323232u);
        wl_subsurface_set_position(w->content_subsurface, 0, w->menu_height);
        xdg_surface_set_window_geometry(w->shell_surface, 0, 0, width, height);
        commit_cpu(w->surface, slot);
        w->menu.dirty = false; update_menu_popup(w);
        wl_display_flush(s.display); check_protocol_error(); return;
    }
    if (!s.shm || w->gl_attached) {
        if (trace() && !s.shm) std::fprintf(stderr, "AppKit: the compositor offers no wl_shm; the window cannot be shown\n");
        return;
    }
    const int width = std::max(1, static_cast<int>(w->frame.w));
    const int height = std::max(1, static_cast<int>(w->frame.h));
    const std::size_t stride = static_cast<std::size_t>(width) * 4;
    const std::size_t bytes = stride * static_cast<std::size_t>(height);

    if (w->shm_bytes != bytes) {
        if (w->shm_buffer) { wl_buffer_destroy(w->shm_buffer); w->shm_buffer = nullptr; }
        if (w->shm_pixels) { munmap(w->shm_pixels, w->shm_bytes); w->shm_pixels = nullptr; }
        const int fd = memfd_create("machoman-window", MFD_CLOEXEC);
        if (fd < 0) return;
        if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) { close(fd); return; }
        void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { close(fd); return; }
        wl_shm_pool *pool = wl_shm_create_pool(s.shm, fd, static_cast<std::int32_t>(bytes));
        w->shm_buffer = wl_shm_pool_create_buffer(pool, 0, width, height, static_cast<std::int32_t>(stride),
                                                  WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        w->shm_pixels = static_cast<std::uint32_t *>(p);
        w->shm_bytes = bytes;
    }
    if (!w->shm_pixels || !w->shm_buffer) return;
    /* AppKit's default window background, until views paint for themselves. */
    std::fill_n(w->shm_pixels, bytes / 4, 0x00323232u);
    wl_surface_attach(w->surface, w->shm_buffer, 0, 0);
    wl_surface_damage_buffer(w->surface, 0, 0, width, height);
    wl_surface_commit(w->surface);
    wl_display_flush(s.display);
    check_protocol_error();
    if (trace()) std::fprintf(stderr, "AppKit: attached a %dx%d buffer to \"%s\"\n", width, height, w->title.c_str());
}

/* ---- setup ---------------------------------------------------------------- */

bool init_egl(display_state &s) {
    s.egl_display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(s.display));
    if (s.egl_display == EGL_NO_DISPLAY) return false;
    EGLint major = 0, minor = 0;
    if (!eglInitialize(s.egl_display, &major, &minor)) return false;
    eglBindAPI(EGL_OPENGL_API);
    const EGLint attribs_no_alpha[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                       EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0,
                                       EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
                                       EGL_NONE};
    /* EGL takes ALPHA_SIZE 0 as "don't care", so Mesa happily hands back an
     * RGBA config; the compositor then treats the window as translucent
     * wherever the app leaves alpha < 1 (KWin drew its blur over Live).
     * Pick through every config for one that is genuinely opaque. */
    EGLint count = 0;
    if (!eglChooseConfig(s.egl_display, attribs_no_alpha, nullptr, 0, &count) || count <= 0) return false;
    std::vector<EGLConfig> configs(static_cast<std::size_t>(count));
    if (!eglChooseConfig(s.egl_display, attribs_no_alpha, configs.data(), count, &count) || count <= 0) return false;
    for (EGLint i = 0; i < count; i++) {
        EGLint alpha = 0;
        eglGetConfigAttrib(s.egl_display, configs[static_cast<std::size_t>(i)], EGL_ALPHA_SIZE, &alpha);
        if (alpha == 0) {
            s.egl_config = configs[static_cast<std::size_t>(i)];
            return true;
        }
    }
    const EGLint attribs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                              EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                              EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                              EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
                              EGL_NONE};
    if (!eglChooseConfig(s.egl_display, attribs, &s.egl_config, 1, &count) || count == 0) return false;
    return true;
}

bool connect() {
    display_state &s = state();
    if (s.display) return true;
    if (s.failed) return false;
    if (std::getenv("MACHOMAN_APPKIT_HEADLESS")) { s.failed = true; return false; }

    s.display = wl_display_connect(nullptr);
    if (!s.display) { s.failed = true; return false; }
    s.metal_queue = wl_display_create_queue(s.display);
    s.registry = wl_display_get_registry(s.display);
    wl_registry_add_listener(s.registry, &registry_listener, nullptr);
    wl_display_roundtrip(s.display); /* globals */
    wl_display_roundtrip(s.display); /* seat capabilities, output mode */

    if (!s.compositor || !s.wm_base) {
        std::fprintf(stderr, "AppKit: the compositor has no %s; running without a display\n",
                     s.compositor ? "xdg_shell" : "wl_compositor");
        s.failed = true;
        return false;
    }
    s.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!init_egl(s)) {
        std::fprintf(stderr, "AppKit: no EGL configuration for this display; running without a display\n");
        s.failed = true;
        return false;
    }
    if (trace()) std::fprintf(stderr, "AppKit: connected to the Wayland display\n");
    return true;
}

} // namespace

id platform_window_owner(platform_window *w) { return w ? w->owner : nullptr; }

bool platform::available() { return connect(); }

platform_window *platform::create_window(id ns_window, rect frame, const std::string &title, bool decorated, bool resizable) {
    if (!connect()) return nullptr;
    display_state &s = state();
    auto *w = new platform_window;
    w->owner = ns_window;
    w->frame = frame;
    w->title = title;
    w->resizable = resizable;
#ifdef MM_APPKIT_PANGO
    w->menu_eligible = decorated && resizable;
    if (w->menu_eligible && s.subcompositor) {
        w->menu.replace(application_menu_snapshot());
        w->menu_height = w->menu.empty() ? 0 : native_menu_bar_height;
    }
#endif
    if (w->frame.w < 1) w->frame.w = 640;
    if (w->frame.h < 1) w->frame.h = 480;

    w->surface = wl_compositor_create_surface(s.compositor);
    if (s.subcompositor) {
        w->content_surface = wl_compositor_create_surface(s.compositor);
        w->content_subsurface = wl_subcompositor_get_subsurface(s.subcompositor, w->content_surface, w->surface);
        wl_subsurface_set_desync(w->content_subsurface);
        wl_subsurface_set_position(w->content_subsurface, 0, w->menu_height);
        auto *empty = wl_compositor_create_region(s.compositor);
        wl_surface_set_input_region(w->content_surface, empty); wl_region_destroy(empty);
    }
    w->shell_surface = xdg_wm_base_get_xdg_surface(s.wm_base, w->surface);
    xdg_surface_add_listener(w->shell_surface, &shell_surface_listener, w);
    w->toplevel = xdg_surface_get_toplevel(w->shell_surface);
    xdg_toplevel_add_listener(w->toplevel, &toplevel_listener, w);
#ifdef MM_APPKIT_XDG_FOREIGN
    if (s.exporter) {
        w->exported = zxdg_exporter_v2_export_toplevel(s.exporter, w->surface);
        static const zxdg_exported_v2_listener listener = {
            +[](void *data, zxdg_exported_v2 *, const char *handle) {
                static_cast<platform_window *>(data)->exported_handle = handle;
            }};
        zxdg_exported_v2_add_listener(w->exported, &listener, w);
    }
#endif
    xdg_toplevel_set_title(w->toplevel, title.c_str());
    const auto &identity = main_application_identity();
    static const bool icon_installed = [&] {
        const char *data = std::getenv("XDG_DATA_HOME"), *home = std::getenv("HOME");
        const auto root = data && *data ? std::filesystem::path(data) : home && *home ? std::filesystem::path(home) / ".local/share" : std::filesystem::path{};
        return install_application_icon(identity, root);
    }();
    (void)icon_installed;
    if (std::getenv("MACHOMAN_DIAGNOSTICS"))
        std::fprintf(stderr, "AppKit application identity: %s icon=%ux%u desktop=%s\n", identity.app_id.c_str(),
                     identity.icon.width, identity.icon.height, icon_installed ? "installed" : "unavailable");
    xdg_toplevel_set_app_id(w->toplevel, identity.app_id.c_str());
#ifdef MM_APPKIT_XDG_ICON
    if (s.icon_manager && s.shm && !identity.icon.argb.empty()) {
        auto &buffer = w->icon_buffer;
        buffer.width = static_cast<int>(identity.icon.width);
        buffer.height = static_cast<int>(identity.icon.height);
        buffer.bytes = identity.icon.argb.size() * 4;
        const int fd = memfd_create("machoman-app-icon", MFD_CLOEXEC);
        if (fd >= 0) {
            if (ftruncate(fd, static_cast<off_t>(buffer.bytes)) == 0) {
                buffer.pixels = mmap(nullptr, buffer.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (buffer.pixels == MAP_FAILED) buffer.pixels = nullptr;
                if (buffer.pixels) {
                    std::memcpy(buffer.pixels, identity.icon.argb.data(), buffer.bytes);
                    wl_shm_pool *pool = wl_shm_create_pool(s.shm, fd, static_cast<int>(buffer.bytes));
                    buffer.buffer = wl_shm_pool_create_buffer(pool, 0, buffer.width, buffer.height, buffer.width * 4, WL_SHM_FORMAT_ARGB8888);
                    wl_shm_pool_destroy(pool);
                    auto *icon = xdg_toplevel_icon_manager_v1_create_icon(s.icon_manager);
                    xdg_toplevel_icon_v1_add_buffer(icon, buffer.buffer, 1);
                    xdg_toplevel_icon_manager_v1_set_icon(s.icon_manager, w->toplevel, icon);
                    xdg_toplevel_icon_v1_destroy(icon);
                }
            }
            close(fd);
        }
    }
#endif
    if (!resizable) {
        const int width = std::max(1, static_cast<int>(w->frame.w));
        const int height = std::max(1, static_cast<int>(w->frame.h));
        xdg_toplevel_set_min_size(w->toplevel, width, height + w->menu_height);
        xdg_toplevel_set_max_size(w->toplevel, width, height + w->menu_height);
    }
    if (!decorated) {
        /* Without xdg-decoration the compositor draws what it draws; an
         * undecorated window is a request we cannot make, so it is noted
         * and ignored rather than faked. */
        if (trace()) std::fprintf(stderr, "AppKit: undecorated windows are not supported by this compositor\n");
    }
    wl_surface_commit(w->surface);
    wl_display_roundtrip(s.display); /* wait for the first configure */
    check_protocol_error();

    { std::lock_guard lock(s.surface_mu); s.windows.push_back(w); }
    if (trace())
        std::fprintf(stderr, "AppKit: window \"%s\" %gx%g\n", title.c_str(), w->frame.w, w->frame.h);
    return w;
}

std::string platform::dialog_parent(platform_window *w) {
#ifdef MM_APPKIT_XDG_FOREIGN
    if (w && !w->exported_handle.empty()) return "wayland:" + w->exported_handle;
#else
    (void)w;
#endif
    return {};
}

void platform::destroy_window(platform_window *w) {
    if (!w) return;
    display_state &s = state();
    std::lock_guard lock(s.surface_mu);
    s.windows.erase(std::remove(s.windows.begin(), s.windows.end(), w), s.windows.end());
    {
        std::lock_guard queue_lock(s.queue_mu);
        std::erase_if(s.queue, [w](const platform_event &event) { return event.window == w; });
    }
    if (s.pointer_window == w) s.pointer_window = nullptr;
    if (s.keyboard_window == w) s.keyboard_window = nullptr;
    w->metal_layers.clear();
    w->menu_popup.reset();
    if (w->shm_buffer) wl_buffer_destroy(w->shm_buffer);
    if (w->shm_pixels) munmap(w->shm_pixels, w->shm_bytes);
#ifdef MM_APPKIT_EVILCOCOA
    if (w->cocoa_context != EGL_NO_CONTEXT) {
        const auto previous = eglGetCurrentContext();
        const auto draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
        eglMakeCurrent(s.egl_display, w->egl_surface, w->egl_surface, w->cocoa_context);
        w->cocoa_renderer.reset();
        if (previous == w->cocoa_context) eglMakeCurrent(s.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        else eglMakeCurrent(s.egl_display, draw, read, previous);
        eglDestroyContext(s.egl_display, w->cocoa_context);
    }
#endif
    if (w->egl_surface != EGL_NO_SURFACE) eglDestroySurface(s.egl_display, w->egl_surface);
    if (w->egl_window) wl_egl_window_destroy(w->egl_window);
    if (w->content_subsurface) wl_subsurface_destroy(w->content_subsurface);
    if (w->content_surface) wl_surface_destroy(w->content_surface);
#ifdef MM_APPKIT_XDG_FOREIGN
    if (w->exported) zxdg_exported_v2_destroy(w->exported);
#endif
    if (w->toplevel) xdg_toplevel_destroy(w->toplevel);
    if (w->shell_surface) xdg_surface_destroy(w->shell_surface);
    if (w->surface) wl_surface_destroy(w->surface);
    delete w;
}

void platform::show_window(platform_window *w, bool show) {
    std::lock_guard lock(state().surface_mu);
    if (!w) return;
    w->visible = show;
    if (show) {
        update_shm_buffer(w);
        wl_display_flush(state().display);
        return;
    }
    if (!show) {
        /* A surface with no buffer is not mapped, which is how a window is
         * hidden on Wayland. */
        wl_surface_attach(w->surface, nullptr, 0, 0);
        wl_surface_commit(w->surface);
    }
    wl_display_flush(state().display);
}

void platform::set_title(platform_window *w, const std::string &title) {
    if (!w || !w->toplevel) return;
    w->title = title;
    xdg_toplevel_set_title(w->toplevel, title.c_str());
    wl_display_flush(state().display);
}

void platform::set_parent(platform_window *w, platform_window *parent) {
    if (!w || !w->toplevel) return;
    std::lock_guard lock(state().surface_mu);
    xdg_toplevel_set_parent(w->toplevel, parent ? parent->toplevel : nullptr);
    wl_display_flush(state().display);
}

void platform::set_frame(platform_window *w, rect frame) {
    std::lock_guard lock(state().surface_mu);
    if (!w) return;
    /* A Wayland client cannot place its own window; only the size is ours. */
    w->frame.w = frame.w;
    w->frame.h = frame.h;
    const int width = std::max(1, static_cast<int>(frame.w));
    const int height = std::max(1, static_cast<int>(frame.h));
    if (!w->resizable && w->toplevel) {
        xdg_toplevel_set_min_size(w->toplevel, width, height + w->menu_height);
        xdg_toplevel_set_max_size(w->toplevel, width, height + w->menu_height);
    }
    if (w->egl_window) {
        wl_egl_window_resize(w->egl_window, width, height, 0, 0);
        display_state &s = state();
        if (s.compositor && w->surface) {
            wl_region *region = wl_compositor_create_region(s.compositor);
            if (region) {
                wl_region_add(region, 0, 0, width, height);
                wl_surface_set_opaque_region(w->content_surface ? w->content_surface : w->surface, region);
                wl_region_destroy(region);
            }
        }
    }
    if (w->visible) update_shm_buffer(w);
    wl_display_flush(state().display);
}

rect platform::window_frame(platform_window *w) { return w ? w->frame : rect{}; }

void platform::set_size_limits(platform_window *w, size minimum, size maximum) {
    if (!w || !w->toplevel || !w->resizable) return;
    std::lock_guard lock(state().surface_mu);
    w->minimum_size = minimum; w->maximum_size = maximum;
    const auto lo = outer_size_limit(minimum, w->menu_height);
    auto hi = outer_size_limit(maximum, w->menu_height);
    if (hi.w) hi.w = std::max(hi.w, lo.w);
    if (hi.h) hi.h = std::max(hi.h, lo.h);
    xdg_toplevel_set_min_size(w->toplevel, int(lo.w), int(lo.h));
    xdg_toplevel_set_max_size(w->toplevel, int(hi.w), int(hi.h));
    wl_surface_commit(w->surface);
    wl_display_flush(state().display);
}

void platform::set_menu(platform_window *w, const std::vector<native_menu_item> &items) {
    std::lock_guard lock(state().surface_mu);
    if (!w || !w->menu_eligible || !w->content_surface) return;
    const int before = w->menu_height;
    w->menu.replace(items); w->menu_height = w->menu.empty() ? 0 : native_menu_bar_height;
    if (before != w->menu_height) {
        set_size_limits(w, w->minimum_size, w->maximum_size);
        w->menu.close();
        // Keep the negotiated Linux outer size; only guest content loses/gains the bar.
        w->frame.h = std::max(1., w->frame.h + before - w->menu_height);
        if (w->egl_window) wl_egl_window_resize(w->egl_window, int(w->frame.w), int(w->frame.h), 0, 0);
        platform_event event; event.kind = event_kind::resized; event.window = w;
        event.new_size = {w->frame.w, w->frame.h}; push(event);
    }
    if (w->visible && w->menu.dirty) update_shm_buffer(w);
}

void platform::set_cursor(const cursor_spec &cursor) {
    auto &s = state(); std::lock_guard lock(s.surface_mu);
    if (s.cursor != cursor) { s.cursor = cursor; ++s.cursor_generation; }
    apply_platform_cursor();
}

void platform::release_metal(const void *layer) {
    std::lock_guard lock(state().surface_mu);
    for (auto *w : state().windows) w->metal_layers.erase(layer);
}

#ifdef MM_APPKIT_MEVK
bool platform::present_metal_texture(platform_window *w, const void *layer, rect frame,
    const std::shared_ptr<mevk::Texture> &texture, double scale, unsigned placement) {
    auto &s = state();
    std::lock_guard lock(s.surface_mu);
    if (!w) return !available();
    if (std::find(s.windows.begin(), s.windows.end(), w) == s.windows.end()) return false;
    if (s.failed || !s.subcompositor || !texture || !layer || !std::isfinite(scale) || scale <= 0 || placement > 11) return false;
    if (frame.w <= 0 || frame.h <= 0) { release_metal(layer); return true; }
    if (frame.w > 16384 || frame.h > 16384 || std::abs(frame.x) > 1e8 || std::abs(frame.y) > 1e8) return false;
    for (auto *other : s.windows) if (other != w) other->metal_layers.erase(layer);
    auto &entry = w->metal_layers[layer];
    if (!entry) {
        entry = std::make_unique<metal_surface>();
        entry->surface = wl_compositor_create_surface(s.compositor);
        entry->subsurface = wl_subcompositor_get_subsurface(s.subcompositor, entry->surface, w->surface);
        wl_subsurface_set_desync(entry->subsurface);
        auto *empty = wl_compositor_create_region(s.compositor);
        wl_surface_set_input_region(entry->surface, empty); wl_region_destroy(empty);
    }
    if (entry->gpu_failed) return false;
    std::string error;
    if (!entry->presenter) {
        entry->presenter = mevk::WaylandPresenter::create(texture, s.display, entry->surface, error);
        if (entry->presenter) std::fprintf(stderr, "AppKit Metal: direct Vulkan Wayland presentation layer=%p\n", layer);
    }
    if (!entry->presenter || !entry->presenter->present(texture, static_cast<unsigned>(std::ceil(frame.w)),
        static_cast<unsigned>(std::ceil(frame.h)), frame.w, frame.h, scale, placement, error)) {
        entry->presenter.reset(); entry->gpu_failed = true;
        std::fprintf(stderr, "AppKit Metal: Vulkan presentation unavailable: %s\n", error.c_str());
        return false;
    }
    wl_subsurface_set_position(entry->subsurface, static_cast<int>(std::lround(frame.x)),
        static_cast<int>(std::lround(w->frame.h + w->menu_height - frame.y - frame.h)));
    if (w->menu_popup) wl_subsurface_place_above(w->menu_popup->subsurface, entry->surface);
    wl_surface_commit(w->surface);
    wl_display_flush(s.display);
    const auto now = std::chrono::steady_clock::now();
    if (now - entry->last_report > std::chrono::seconds(3) && std::getenv("MACHOMAN_APPKIT_FRAME_TIMING")) {
        const auto stats = entry->presenter->stats();
        std::fprintf(stderr, "AppKit Metal WSI: layer=%p size=%gx%g presented=%llu skipped=%llu swapchains=%llu readback=0\n",
            layer, frame.w, frame.h, static_cast<unsigned long long>(stats.presented),
            static_cast<unsigned long long>(stats.skipped), static_cast<unsigned long long>(stats.recreations));
        entry->last_report = now;
    }
    check_protocol_error();
    return !s.failed;
}
#endif

bool platform::present_metal(platform_window *w, const void *layer, rect frame,
        const void *bgra, std::uint32_t width, std::uint32_t height, std::uint64_t stride,
        double contents_scale, unsigned placement) {
    auto &s = state();
    std::lock_guard lock(s.surface_mu);
    if (!w) return !available();
    if (std::find(s.windows.begin(), s.windows.end(), w) == s.windows.end()) return false;
    if (s.failed || !s.shm || !s.subcompositor || !layer || !bgra || !width || !height ||
        stride < std::uint64_t{width} * 4 || !std::isfinite(contents_scale) || contents_scale <= 0 || placement > 11) return false;
    wl_display_dispatch_queue_pending(s.display, s.metal_queue);
    if (frame.w <= 0 || frame.h <= 0) { release_metal(layer); return true; }
    // The view's placement controls scaling versus cropping allocation padding.
    // Produce logical points until fractional-scale/viewport support lands.
    if (frame.w > 16384 || frame.h > 16384 || std::abs(frame.x) > 1e8 || std::abs(frame.y) > 1e8) return false;
    const int out_width = static_cast<int>(std::ceil(frame.w));
    const int out_height = static_cast<int>(std::ceil(frame.h));
    const auto bytes = static_cast<std::size_t>(out_width) * static_cast<std::size_t>(out_height) * 4;
    // Reparenting a layer must also reparent its native surface.
    for (auto *other : s.windows) if (other != w) other->metal_layers.erase(layer);
    auto &entry = w->metal_layers[layer];
    if (!entry) {
        entry = std::make_unique<metal_surface>();
        entry->surface = wl_compositor_create_surface(s.compositor);
        entry->subsurface = wl_subcompositor_get_subsurface(s.subcompositor, entry->surface, w->surface);
        wl_subsurface_set_desync(entry->subsurface);
        // Let the parent window continue receiving input, including OpenGL views.
        auto *empty = wl_compositor_create_region(s.compositor);
        wl_surface_set_input_region(entry->surface, empty);
        wl_region_destroy(empty);
    }
    metal_shm_buffer *slot = nullptr;
    for (auto &b : entry->buffers) if (!b.busy) { slot = &b; break; }
    // The compositor still owns all three frames. Skip this frame; never rewrite
    // memory that is being displayed. The event loop processes wl_buffer.release.
    if (!slot) return true;
    if (slot->width != out_width || slot->height != out_height || !slot->buffer) {
        slot->reset();
        const int fd = memfd_create("machoman-metal", MFD_CLOEXEC);
        if (fd < 0) return false;
        if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) { close(fd); return false; }
        auto *mapped = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mapped == MAP_FAILED) { close(fd); return false; }
        auto *pool = wl_shm_create_pool(s.shm, fd, static_cast<std::int32_t>(bytes));
        slot->buffer = wl_shm_pool_create_buffer(pool, 0, out_width, out_height, out_width * 4, WL_SHM_FORMAT_ARGB8888);
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(slot->buffer), s.metal_queue);
        wl_shm_pool_destroy(pool); close(fd);
        slot->pixels = mapped; slot->bytes = bytes; slot->width = out_width; slot->height = out_height;
        static const wl_buffer_listener listener = {[](void *data, wl_buffer *) {
            static_cast<metal_shm_buffer *>(data)->busy = false;
        }};
        wl_buffer_add_listener(slot->buffer, &listener, slot);
    }
    copy_metal_contents(slot->pixels, out_width, out_height, frame.w, frame.h,
        bgra, width, height, stride, contents_scale, placement);
    wl_subsurface_set_position(entry->subsurface, static_cast<int>(std::lround(frame.x)),
                              static_cast<int>(std::lround(w->frame.h + w->menu_height - frame.y - frame.h)));
    if (w->menu_popup) wl_subsurface_place_above(w->menu_popup->subsurface, entry->surface);
    slot->busy = true;
    wl_surface_attach(entry->surface, slot->buffer, 0, 0);
    wl_surface_damage(entry->surface, 0, 0, out_width, out_height);
    wl_surface_commit(entry->surface);
    wl_surface_commit(w->surface); // subsurface position takes effect with parent commit
    wl_display_flush(s.display);
    check_protocol_error();
    return !s.failed;
}

bool platform::next_event(platform_event &out, double timeout_seconds) {
    display_state &s = state();
    if (!s.display) {
        if (timeout_seconds > 0) usleep(static_cast<unsigned>(timeout_seconds * 1e6));
        return false;
    }
    bool queued = false;
    {
        std::lock_guard lock(s.queue_mu);
        queued = !s.queue.empty();
    }
    /* Wayland's read cycle: prepare, poll, read, dispatch. */
    while (wl_display_prepare_read(s.display) != 0) wl_display_dispatch_pending(s.display);
    wl_display_flush(s.display);
    pollfd pfd{wl_display_get_fd(s.display), POLLIN, 0};
    /* Service protocol traffic (including compositor pings) even when
     * input is buffered; a slow guest redraw can otherwise starve it. */
    const int ms = queued ? 0 : timeout_seconds < 0 ? -1 : static_cast<int>(timeout_seconds * 1000);
    if (poll(&pfd, 1, ms) > 0 && (pfd.revents & POLLIN)) wl_display_read_events(s.display);
    else wl_display_cancel_read(s.display);
    wl_display_dispatch_pending(s.display);
    check_protocol_error();

    { std::lock_guard lock(s.surface_mu);
      for (auto *w : s.windows) if (w->visible && w->content_surface && w->menu.dirty) update_shm_buffer(w); }

    for (;;) {
        { std::lock_guard lock(s.queue_mu);
          if (s.queue.empty()) return false;
          out = std::move(s.queue.front()); s.queue.pop_front(); }
        auto *w = out.window;
        if (!w || !w->menu_height) return true;
        const auto result = w->menu.handle(out, {out.where.x, w->frame.h + w->menu_height - out.where.y});
        if (w->menu.dirty && w->visible) update_shm_buffer(w);
        if (result.action || result.opening) {
            out.kind = result.action ? event_kind::menu_action : event_kind::menu_open;
            out.menu_token = result.action ? result.action : result.opening; return true;
        }
        if (!result.consumed) return true;
    }
}

void platform::wake() {
    display_state &s = state();
    if (s.display) wl_display_flush(s.display);
}

size platform::screen_size() { connect(); return state().screen; }
double platform::screen_scale() { connect(); return state().scale; }
double platform::screen_refresh() { connect(); return state().refresh.load(); }

void *platform::create_gl_context(platform_window *, void *share) {
    if (!connect()) return nullptr;
    display_state &s = state();
    /* An app built against legacy OpenGL (which Live uses, relying on glBegin/glMatrixMode)
     * needs a compatibility / default context. */
    EGLContext ctx = eglCreateContext(s.egl_display, s.egl_config,
                                      share ? static_cast<EGLContext>(share) : EGL_NO_CONTEXT, nullptr);
    if (ctx == EGL_NO_CONTEXT) {
        const EGLint attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2,
                                  EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT, EGL_NONE};
        ctx = eglCreateContext(s.egl_display, s.egl_config,
                               share ? static_cast<EGLContext>(share) : EGL_NO_CONTEXT, attribs);
    }
    if (ctx == EGL_NO_CONTEXT) {
        const EGLint attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2,
                                  EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
        ctx = eglCreateContext(s.egl_display, s.egl_config,
                               share ? static_cast<EGLContext>(share) : EGL_NO_CONTEXT, attribs);
    }
    return ctx == EGL_NO_CONTEXT ? nullptr : ctx;
}

void platform::destroy_gl_context(void *ctx) {
    if (ctx) eglDestroyContext(state().egl_display, static_cast<EGLContext>(ctx));
}

bool platform::make_current(void *ctx, platform_window *w) {
    display_state &s = state();
    if (!s.display) return false;
    if (!ctx) return eglMakeCurrent(s.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (w) ensure_egl_surface(w);
#ifdef MM_APPKIT_EVILCOCOA
    if (w && ctx != w->cocoa_context) w->guest_gl = true;
#endif
    const EGLSurface surface = w ? w->egl_surface : EGL_NO_SURFACE;
    return eglMakeCurrent(s.egl_display, surface, surface, static_cast<EGLContext>(ctx));
}

#ifdef MM_APPKIT_EVILCOCOA
void platform::present_cocoa(platform_window *w, const std::vector<evilcocoa::item> &items) {
    auto &s = state();
    if (!w || !s.display || w->guest_gl || !w->metal_layers.empty() || !w->visible) return;
    const int width = std::max(1, int(w->frame.w)), height = std::max(1, int(w->frame.h));
    if (w->cocoa_renderer && w->cocoa_items == items && w->cocoa_width == width && w->cocoa_height == height) return;
    if (!ensure_egl_surface(w)) return;
    const auto previous = eglGetCurrentContext();
    const auto draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
    if (w->cocoa_context == EGL_NO_CONTEXT) {
        const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION,3,EGL_CONTEXT_MINOR_VERSION,3,
            EGL_CONTEXT_OPENGL_PROFILE_MASK,EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,EGL_NONE};
        w->cocoa_context = eglCreateContext(s.egl_display, s.egl_config, EGL_NO_CONTEXT, attributes);
    }
    if (w->cocoa_context == EGL_NO_CONTEXT || !eglMakeCurrent(s.egl_display, w->egl_surface, w->egl_surface, w->cocoa_context)) return;
    try {
        if (!w->cocoa_renderer) { w->cocoa_renderer = std::make_unique<evilcocoa::renderer>(); eglSwapInterval(s.egl_display, 0); }
        w->cocoa_renderer->draw(items, width, height);
        if (eglSwapBuffers(s.egl_display, w->egl_surface)) {
            w->cocoa_items = items; w->cocoa_width = width; w->cocoa_height = height;
        }
    } catch (const std::exception &error) { std::fprintf(stderr, "AppKit: evilcocoa: %s\n", error.what()); }
    eglMakeCurrent(s.egl_display, draw, read, previous);
    wl_display_flush(s.display);
}
#endif

void *platform::egl_display_of() { return state().egl_display; }
void *platform::egl_surface_of(platform_window *w) { return w ? w->egl_surface : nullptr; }

void platform::swap_buffers(void *ctx, platform_window *w) {
    display_state &s = state();
    (void)ctx;
    if (trace()) std::fprintf(stderr, "AppKit: platform::swap_buffers w=%p\n", w);
    if (!s.display || !w || !ensure_egl_surface(w)) {
        if (trace()) std::fprintf(stderr, "AppKit: platform::swap_buffers skipped (display=%p w=%p)\n", s.display, w);
        return;
    }
    eglSwapBuffers(s.egl_display, w->egl_surface);
    wl_display_flush(s.display);
}

void platform::set_swap_interval(void *, int interval) {
    display_state &s = state();
    if (s.egl_display != EGL_NO_DISPLAY) eglSwapInterval(s.egl_display, interval);
}

} // namespace ak

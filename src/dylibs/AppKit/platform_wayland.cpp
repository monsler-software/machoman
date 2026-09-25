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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <EGL/egl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"

namespace ak {

struct platform_window {
    id owner = nullptr;
    wl_surface *surface = nullptr;
    xdg_surface *shell_surface = nullptr;
    xdg_toplevel *toplevel = nullptr;
    wl_egl_window *egl_window = nullptr;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    /* Software backing store, for a window whose content is not drawn by
     * an OpenGL context: without a buffer attached the compositor never
     * maps the surface, so the window would not appear at all. */
    wl_buffer *shm_buffer = nullptr;
    std::uint32_t *shm_pixels = nullptr;
    std::size_t shm_bytes = 0;
    bool gl_attached = false;
    rect frame;                 /* AppKit points */
    std::string title;
    bool visible = false, configured = false;
    size pending_size;          /* from the last xdg_toplevel configure */
};

namespace {

/* ---- the connection ------------------------------------------------------ */

struct display_state {
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_compositor *compositor = nullptr;
    xdg_wm_base *wm_base = nullptr;
    wl_seat *seat = nullptr;
    wl_shm *shm = nullptr;
    wl_pointer *pointer = nullptr;
    wl_keyboard *keyboard = nullptr;
    wl_output *output = nullptr;

    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLConfig egl_config = nullptr;

    xkb_context *xkb = nullptr;
    xkb_keymap *keymap = nullptr;
    xkb_state *xkb_state_ = nullptr;

    size screen{1920, 1080};
    double scale = 1.0;

    std::vector<platform_window *> windows;
    std::deque<platform_event> queue;
    std::mutex queue_mu;

    /* Input state, so an event carries what AppKit expects to read from it. */
    platform_window *pointer_window = nullptr;
    platform_window *keyboard_window = nullptr;
    point pointer_at;
    std::uint64_t modifiers = 0;
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
    s.queue.push_back(e);
}

platform_window *window_of_surface(wl_surface *surface) {
    if (!surface) return nullptr;
    for (platform_window *w : state().windows)
        if (w->surface == surface) return w;
    return nullptr;
}

/* Wayland's y grows downwards; AppKit's does not. */
double flip_y(const platform_window *w, double y) { return w ? w->frame.h - y : y; }

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
    return m;
}

/* Defined below, next to the rest of the software backing store. */
void update_shm_buffer(platform_window *w);

/* ---- listeners ------------------------------------------------------------ */

void pointer_enter(void *, wl_pointer *, std::uint32_t, wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    display_state &s = state();
    s.pointer_window = window_of_surface(surface);
    s.pointer_at = {wl_fixed_to_double(x), flip_y(s.pointer_window, wl_fixed_to_double(y))};
}
void pointer_leave(void *, wl_pointer *, std::uint32_t, wl_surface *) { state().pointer_window = nullptr; }
void pointer_motion(void *, wl_pointer *, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
    display_state &s = state();
    s.pointer_at = {wl_fixed_to_double(x), flip_y(s.pointer_window, wl_fixed_to_double(y))};
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
    e.keycode = code;
    e.modifiers = s.modifiers;
    if (s.xkb_state_) {
        char buf[64];
        const int n = xkb_state_key_get_utf8(s.xkb_state_, code, buf, sizeof buf);
        if (n > 0) e.characters.assign(buf, static_cast<std::size_t>(n));
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
void output_mode(void *, wl_output *, std::uint32_t flags, std::int32_t width, std::int32_t height, std::int32_t) {
    if (flags & WL_OUTPUT_MODE_CURRENT) state().screen = {static_cast<double>(width), static_cast<double>(height)};
}
void output_done(void *, wl_output *) {}
void output_scale(void *, wl_output *, std::int32_t factor) { state().scale = factor > 0 ? factor : 1; }
void output_name(void *, wl_output *, const char *) {}
void output_description(void *, wl_output *, const char *) {}
const wl_output_listener output_listener = {output_geometry, output_mode, output_done, output_scale,
                                            output_name, output_description};

void wm_base_ping(void *, xdg_wm_base *base, std::uint32_t serial) { xdg_wm_base_pong(base, serial); }
const xdg_wm_base_listener wm_base_listener = {wm_base_ping};

void registry_global(void *, wl_registry *registry, std::uint32_t name, const char *interface, std::uint32_t version) {
    display_state &s = state();
    const std::string_view i(interface);
    if (i == wl_compositor_interface.name)
        s.compositor = static_cast<wl_compositor *>(wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4u)));
    else if (i == xdg_wm_base_interface.name) {
        s.wm_base = static_cast<xdg_wm_base *>(wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 3u)));
        xdg_wm_base_add_listener(s.wm_base, &wm_base_listener, nullptr);
    } else if (i == wl_seat_interface.name) {
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
    platform_event e;
    e.kind = event_kind::exposed;
    e.window = w;
    push(e);
}
const xdg_surface_listener shell_surface_listener = {surface_configure};

void toplevel_configure(void *data, xdg_toplevel *, std::int32_t width, std::int32_t height, wl_array *) {
    auto *w = static_cast<platform_window *>(data);
    if (width > 0 && height > 0) w->pending_size = {static_cast<double>(width), static_cast<double>(height)};
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
    if (w->shm_buffer) { wl_buffer_destroy(w->shm_buffer); w->shm_buffer = nullptr; }
    if (w->shm_pixels) { munmap(w->shm_pixels, w->shm_bytes); w->shm_pixels = nullptr; w->shm_bytes = 0; }
    w->gl_attached = true;
    w->egl_window = wl_egl_window_create(w->surface, std::max(1, static_cast<int>(w->frame.w)),
                                         std::max(1, static_cast<int>(w->frame.h)));
    if (!w->egl_window) return false;
    w->egl_surface = eglCreateWindowSurface(s.egl_display, s.egl_config,
                                            reinterpret_cast<EGLNativeWindowType>(w->egl_window), nullptr);
    return w->egl_surface != EGL_NO_SURFACE;
}

/* ---- software backing store ------------------------------------------------ */

/* Allocates (or reallocates) the shm buffer for a window's current size and
 * fills it with the window background, then attaches it. Views that draw
 * through OpenGL never take this path. */
void update_shm_buffer(platform_window *w) {
    display_state &s = state();
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
    const EGLint attribs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                              EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                              EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                              EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
                              EGL_NONE};
    EGLint count = 0;
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

platform_window *platform::create_window(id ns_window, rect frame, const std::string &title, bool decorated) {
    if (!connect()) return nullptr;
    display_state &s = state();
    auto *w = new platform_window;
    w->owner = ns_window;
    w->frame = frame;
    w->title = title;
    if (w->frame.w < 1) w->frame.w = 640;
    if (w->frame.h < 1) w->frame.h = 480;

    w->surface = wl_compositor_create_surface(s.compositor);
    w->shell_surface = xdg_wm_base_get_xdg_surface(s.wm_base, w->surface);
    xdg_surface_add_listener(w->shell_surface, &shell_surface_listener, w);
    w->toplevel = xdg_surface_get_toplevel(w->shell_surface);
    xdg_toplevel_add_listener(w->toplevel, &toplevel_listener, w);
    xdg_toplevel_set_title(w->toplevel, title.c_str());
    xdg_toplevel_set_app_id(w->toplevel, "machoman");
    if (!decorated) {
        /* Without xdg-decoration the compositor draws what it draws; an
         * undecorated window is a request we cannot make, so it is noted
         * and ignored rather than faked. */
        if (trace()) std::fprintf(stderr, "AppKit: undecorated windows are not supported by this compositor\n");
    }
    wl_surface_commit(w->surface);
    wl_display_roundtrip(s.display); /* wait for the first configure */
    check_protocol_error();

    s.windows.push_back(w);
    if (trace())
        std::fprintf(stderr, "AppKit: window \"%s\" %gx%g\n", title.c_str(), w->frame.w, w->frame.h);
    return w;
}

void platform::destroy_window(platform_window *w) {
    if (!w) return;
    display_state &s = state();
    s.windows.erase(std::remove(s.windows.begin(), s.windows.end(), w), s.windows.end());
    if (s.pointer_window == w) s.pointer_window = nullptr;
    if (s.keyboard_window == w) s.keyboard_window = nullptr;
    if (w->shm_buffer) wl_buffer_destroy(w->shm_buffer);
    if (w->shm_pixels) munmap(w->shm_pixels, w->shm_bytes);
    if (w->egl_surface != EGL_NO_SURFACE) eglDestroySurface(s.egl_display, w->egl_surface);
    if (w->egl_window) wl_egl_window_destroy(w->egl_window);
    if (w->toplevel) xdg_toplevel_destroy(w->toplevel);
    if (w->shell_surface) xdg_surface_destroy(w->shell_surface);
    if (w->surface) wl_surface_destroy(w->surface);
    delete w;
}

void platform::show_window(platform_window *w, bool show) {
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

void platform::set_frame(platform_window *w, rect frame) {
    if (!w) return;
    /* A Wayland client cannot place its own window; only the size is ours. */
    w->frame.w = frame.w;
    w->frame.h = frame.h;
    if (w->egl_window) wl_egl_window_resize(w->egl_window, static_cast<int>(frame.w), static_cast<int>(frame.h), 0, 0);
    if (w->visible) update_shm_buffer(w);
    wl_display_flush(state().display);
}

rect platform::window_frame(platform_window *w) { return w ? w->frame : rect{}; }

bool platform::next_event(platform_event &out, double timeout_seconds) {
    display_state &s = state();
    if (!s.display) {
        if (timeout_seconds > 0) usleep(static_cast<unsigned>(timeout_seconds * 1e6));
        return false;
    }
    {
        std::lock_guard lock(s.queue_mu);
        if (!s.queue.empty()) {
            out = s.queue.front();
            s.queue.pop_front();
            return true;
        }
    }
    /* Wayland's read cycle: prepare, poll, read, dispatch. */
    while (wl_display_prepare_read(s.display) != 0) wl_display_dispatch_pending(s.display);
    wl_display_flush(s.display);
    pollfd pfd{wl_display_get_fd(s.display), POLLIN, 0};
    const int ms = timeout_seconds < 0 ? -1 : static_cast<int>(timeout_seconds * 1000);
    if (poll(&pfd, 1, ms) > 0 && (pfd.revents & POLLIN)) wl_display_read_events(s.display);
    else wl_display_cancel_read(s.display);
    wl_display_dispatch_pending(s.display);
    check_protocol_error();

    std::lock_guard lock(s.queue_mu);
    if (s.queue.empty()) return false;
    out = s.queue.front();
    s.queue.pop_front();
    return true;
}

void platform::wake() {
    display_state &s = state();
    if (s.display) wl_display_flush(s.display);
}

size platform::screen_size() { connect(); return state().screen; }
double platform::screen_scale() { connect(); return state().scale; }

void *platform::create_gl_context(platform_window *, void *share) {
    if (!connect()) return nullptr;
    display_state &s = state();
    const EGLint attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2,
                              EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext ctx = eglCreateContext(s.egl_display, s.egl_config,
                                      share ? static_cast<EGLContext>(share) : EGL_NO_CONTEXT, attribs);
    if (ctx == EGL_NO_CONTEXT) {
        /* An app built against OpenGL 2.1 (which is all NSOpenGLContext
         * offered before core profiles) gets a compatibility context. */
        ctx = eglCreateContext(s.egl_display, s.egl_config, share ? static_cast<EGLContext>(share) : EGL_NO_CONTEXT, nullptr);
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
    const EGLSurface surface = w ? w->egl_surface : EGL_NO_SURFACE;
    return eglMakeCurrent(s.egl_display, surface, surface, static_cast<EGLContext>(ctx));
}

void platform::swap_buffers(void *ctx, platform_window *w) {
    display_state &s = state();
    (void)ctx;
    if (!s.display || !w || !ensure_egl_surface(w)) return;
    eglSwapBuffers(s.egl_display, w->egl_surface);
}

void platform::set_swap_interval(void *, int interval) {
    display_state &s = state();
    if (s.egl_display != EGL_NO_DISPLAY) eglSwapInterval(s.egl_display, interval);
}

} // namespace ak

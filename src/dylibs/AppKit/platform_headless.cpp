/* The platform backend used when there is no display: no windows appear
 * and no events arrive, but every AppKit call still behaves, so an app can
 * be driven to the point where it would draw. The Wayland backend
 * (platform_wayland.cpp) replaces this one when the xdg-shell protocol
 * definitions are available at build time. */
#include "appkit.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace ak {

/* A window exists as an object even with no compositor, so that geometry,
 * hit testing and the responder chain work. */
struct platform_window {
    id owner = nullptr;
    rect frame;
    std::string title;
    bool visible = false;
};

namespace {
std::vector<platform_window *> g_windows;
}

id platform_window_owner(platform_window *w) { return w ? w->owner : nullptr; }

std::string platform::dialog_parent(platform_window *) { return {}; }

#ifdef MM_APPKIT_EVILCOCOA
void platform::present_cocoa(platform_window *, const std::vector<evilcocoa::item> &) {}
#endif

bool platform::available() { return false; }

platform_window *platform::create_window(id ns_window, rect frame, const std::string &title, bool, bool, platform_window *, rect, std::uint32_t) {
    auto *w = new platform_window{ns_window, frame, title, false};
    g_windows.push_back(w);
    return w;
}
void platform::destroy_window(platform_window *w) {
    if (!w) return;
    g_windows.erase(std::remove(g_windows.begin(), g_windows.end(), w), g_windows.end());
    delete w;
}
void platform::show_window(platform_window *w, bool show) { if (w) w->visible = show; }
void platform::set_title(platform_window *w, const std::string &title) { if (w) w->title = title; }
void platform::set_parent(platform_window *, platform_window *) {}
void platform::set_frame(platform_window *w, rect frame) { if (w) w->frame = frame; }
void platform::set_size_limits(platform_window *, size, size) {}
rect platform::window_frame(platform_window *w) { return w ? w->frame : rect{}; }
void platform::set_menu(platform_window *, const std::vector<native_menu_item> &) {}
void platform::set_cursor(const cursor_spec &) {}
clipboard_snapshot platform::clipboard_types() { return {}; }
bool platform::clipboard_read(const std::string &, std::string &) { return false; }
void platform::clipboard_write(const clipboard_data &) {}
bool platform::present_metal(platform_window *, const void *, rect, const void *, std::uint32_t, std::uint32_t, std::uint64_t,
                            double, unsigned) { return true; }
void platform::release_metal(const void *) {}
#ifdef MM_APPKIT_MEVK
bool platform::present_metal_texture(platform_window *, const void *, rect,
    const std::shared_ptr<mevk::Texture> &, double, unsigned) { return true; }
#endif

bool platform::next_event(platform_event &, double timeout_seconds) {
    if (timeout_seconds > 0)
        std::this_thread::sleep_for(std::chrono::duration<double>(timeout_seconds));
    return false;
}
void platform::wake() {}

size platform::screen_size() { return {1920, 1080}; }
double platform::screen_scale() { return 1.0; }
double platform::screen_refresh() { return 60.0; }

void *platform::create_gl_context(platform_window *, void *) { return nullptr; }
void platform::destroy_gl_context(void *) {}
bool platform::make_current(void *, platform_window *) { return false; }
void platform::swap_buffers(void *, platform_window *) {}
void platform::set_swap_interval(void *, int) {}

} // namespace ak

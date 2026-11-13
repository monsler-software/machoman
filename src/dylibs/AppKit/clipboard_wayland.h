#ifndef MACHOMAN_CLIPBOARD_WAYLAND_H
#define MACHOMAN_CLIPBOARD_WAYLAND_H
#include "clipboard.h"
#include <memory>
struct wl_display;
struct wl_registry;
struct wl_seat;
namespace ak {
class wayland_clipboard {
    struct impl;
    std::unique_ptr<impl> p;
public:
    wayland_clipboard();
    ~wayland_clipboard();
    void global(wl_registry *, std::uint32_t name, const char *interface, std::uint32_t version);
    void setup(wl_display *, wl_seat *);
    void input(std::uint32_t serial);
    void unfocus();
    clipboard_snapshot snapshot() const;
    void write(const clipboard_data &);
    bool read(const std::string &type, std::string &bytes);
};
// Service protocol traffic without consuming AppKit input. Also watches a
// transfer pipe, so a clipboard owner in this process cannot deadlock itself.
bool dispatch_wayland(wl_display *, int timeout_ms, int transfer_fd = -1);
}
#endif

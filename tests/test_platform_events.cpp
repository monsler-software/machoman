#include "../src/dylibs/AppKit/platform_events.h"
#include "../src/dylibs/AppKit/keyboard.h"
#include <cstdio>
#include <cstdlib>

using namespace ak;
static void require(bool condition) { if (!condition) std::abort(); }
static platform_event event(event_kind kind, platform_window *window, double value = 0) {
    platform_event e;
    e.kind = kind; e.window = window;
    e.where = {value, value}; e.new_size = {value, value};
    return e;
}

int main() {
    auto limit = outer_size_limit({640.25, 480.25}, 28);
    require(limit.w == 641 && limit.h == 509);
    limit = outer_size_limit({0, 0}, 28);
    require(limit.w == 0 && limit.h == 0);
    limit = outer_size_limit({640, 480}, 0);
    require(limit.w == 640 && limit.h == 480);
    constexpr auto ctrl = 1ull << 18, cmd = 1ull << 20, shift = 1ull << 17, alt = 1ull << 19;
    require(native_shortcut_modifiers(ctrl | shift) == (cmd | shift));
    require(native_shortcut_modifiers(cmd | alt) == (ctrl | alt));
    require(native_shortcut_modifiers(ctrl | cmd | shift | alt) == (ctrl | cmd | shift | alt));
    require(native_shortcut_modifiers(ctrl, false) == ctrl);
    require(native_shortcut_keycode(mac_keycode_from_evdev(29)) == 55);
    require(native_shortcut_keycode(mac_keycode_from_evdev(97)) == 54);
    require(native_shortcut_keycode(mac_keycode_from_evdev(125)) == 59);
    require(native_shortcut_keycode(mac_keycode_from_evdev(30)) == 0);
    require(native_shortcut_keycode(59, false) == 59);
    int a, b;
    auto *first = reinterpret_cast<platform_window *>(&a);
    auto *second = reinterpret_cast<platform_window *>(&b);
    std::deque<platform_event> queue;
    for (int i = 1; i <= 10000; ++i) {
        enqueue_platform_event(queue, event(event_kind::resized, first, i));
        enqueue_platform_event(queue, event(event_kind::exposed, first));
        enqueue_platform_event(queue, event(event_kind::resized, second, i + 1));
        enqueue_platform_event(queue, event(event_kind::exposed, second));
    }
    require(queue.size() == 4 && queue[0].new_size.w == 10000 && queue[2].new_size.w == 10001);
    enqueue_platform_event(queue, event(event_kind::mouse_down, first));
    enqueue_platform_event(queue, event(event_kind::resized, first, 20000));
    require(queue.size() == 6 && queue[0].new_size.w == 10000 && queue[4].kind == event_kind::mouse_down);
    queue.clear();
    for (int i = 0; i < 10000; ++i) enqueue_platform_event(queue, event(event_kind::mouse_moved, first, i));
    require(queue.size() == 1 && queue[0].where.x == 9999);
    enqueue_platform_event(queue, event(event_kind::mouse_down, first));
    enqueue_platform_event(queue, event(event_kind::mouse_moved, first, 10001));
    enqueue_platform_event(queue, event(event_kind::mouse_up, first));
    require(queue.size() == 4 && queue[1].kind == event_kind::mouse_down && queue[3].kind == event_kind::mouse_up);
    enqueue_platform_event(queue, event(event_kind::mouse_moved, second));
    auto moved = event(event_kind::mouse_moved, second); moved.modifiers = 1;
    enqueue_platform_event(queue, moved);
    require(queue.size() == 6);
    enqueue_platform_event(queue, event(event_kind::mouse_dragged, first, 1));
    enqueue_platform_event(queue, event(event_kind::mouse_dragged, first, 2));
    require(queue.size() == 8 && queue[6].where.x == 1 && queue[7].where.x == 2);
    for (auto kind : {event_kind::key_down, event_kind::focus_out, event_kind::close_requested, event_kind::scroll}) {
        queue.clear();
        enqueue_platform_event(queue, event(event_kind::resized, first, 1));
        enqueue_platform_event(queue, event(kind, first));
        enqueue_platform_event(queue, event(event_kind::resized, first, 2));
        require(queue.size() == 3 && queue.front().new_size.w == 1);
    }
    puts("platform events keep latest geometry/hover, preserve input ordering and drag paths");
}

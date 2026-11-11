#ifndef MACHOMAN_APPKIT_PLATFORM_EVENTS_H
#define MACHOMAN_APPKIT_PLATFORM_EVENTS_H

#include "appkit.h"
#include <deque>
#include <cmath>

namespace ak {

inline size outer_size_limit(size content, int menu_height) {
    auto bound = [](double value) { return value > 0 && value < 1000000 ? std::ceil(value) : 0.; };
    return {bound(content.w), content.h > 0 && content.h < 1000000 ? bound(content.h) + menu_height : 0.};
}

/* Keep the latest compositor geometry in a burst. Never move it across
 * an input/focus event: those events must see the geometry at their time. */
inline void enqueue_platform_event(std::deque<platform_event> &queue, const platform_event &event) {
    if (event.kind == event_kind::resized || event.kind == event_kind::exposed) {
        for (auto it = queue.end(); it != queue.begin();) {
            --it;
            if (it->kind != event_kind::resized && it->kind != event_kind::exposed) break;
            if (it->kind == event.kind && it->window == event.window) {
                queue.erase(it);
                break;
            }
        }
    } else if (event.kind == event_kind::mouse_moved && !queue.empty()) {
        const auto &last = queue.back();
        if (last.kind == event.kind && last.window == event.window && last.modifiers == event.modifiers)
            queue.pop_back();
    }
    queue.push_back(event);
}

} // namespace ak
#endif

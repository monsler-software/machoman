#pragma once
#include "appkit.h"
#include <span>

namespace ak {
inline constexpr int native_menu_bar_height = 28;
struct native_menu_item {
    std::uintptr_t token = 0;
    std::string title, shortcut;
    bool enabled = true, separator = false;
    std::int64_t state = 0;
    std::vector<native_menu_item> children;
    bool operator==(const native_menu_item &) const = default;
};
struct native_menu_result {
    bool consumed = false;
    std::uintptr_t action = 0, opening = 0;
};
/* CPU menu layout, navigation and pixels: no EGL, GL or Metal objects. */
class native_menu {
public:
    void replace(const std::vector<native_menu_item> &items);
    void resize(int width, int height);
    bool empty() const { return items_.empty(); }
    bool opened() const { return !selection_.empty(); }
    bool dirty = true;
    void close();
    native_menu_result handle(const platform_event &event, point top_left);
    void paint(std::span<std::uint32_t> pixels, bool popup) const;
    const std::vector<rect> &bar_items() const { return bar_; }
private:
    struct panel { rect frame; std::vector<rect> rows; const std::vector<native_menu_item> *items; };
    std::vector<native_menu_item> items_;
    std::vector<rect> bar_;
    std::vector<panel> panels_;
    std::vector<std::size_t> selection_;
    std::vector<double> scroll_;
    int width_ = 1, height_ = 1;
    bool swallow_release_ = false;
    void layout();
    void select(std::size_t depth, std::size_t index);
    const native_menu_item *selected() const;
};
std::vector<native_menu_item> application_menu_snapshot();
void application_menu_activate(std::uintptr_t token);
void application_menu_open(std::uintptr_t token);
} // namespace ak

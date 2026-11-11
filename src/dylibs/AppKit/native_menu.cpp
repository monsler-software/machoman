#include "native_menu.h"
#include <algorithm>
#include <cmath>
#ifdef MM_APPKIT_PANGO
#include <pango/pangocairo.h>
#endif

namespace ak {
namespace {
bool contains(rect r, point p) { return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h; }
double text_width(const std::string &text) {
#ifdef MM_APPKIT_PANGO
    auto *map = pango_cairo_font_map_get_default();
    auto *context = pango_font_map_create_context(map);
    auto *layout = pango_layout_new(context);
    auto *font = pango_font_description_from_string("Sans 10");
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text.c_str(), -1);
    int width = 0; pango_layout_get_pixel_size(layout, &width, nullptr);
    pango_font_description_free(font); g_object_unref(layout); g_object_unref(context);
    return width;
#else
    return text.size() * 8.;
#endif
}
}
void native_menu::replace(const std::vector<native_menu_item> &items) {
    if (items == items_) return;
    /* Panel pointers refer into the model; rebuild before reading them. */
    items_ = items;
    const auto *list = &items_;
    for (std::size_t d = 0; d < selection_.size(); ++d) {
        if (selection_[d] >= list->size()) { selection_.resize(d); break; }
        list = &(*list)[selection_[d]].children;
    }
    layout(); dirty = true;
}
void native_menu::resize(int width, int height) {
    if (width_ == width && height_ == height) return;
    width_ = std::max(1, width); height_ = std::max(1, height);
    layout(); dirty = true;
}
void native_menu::close() { if (opened()) { selection_.clear(); scroll_.clear(); layout(); dirty = true; } }
void native_menu::layout() {
    bar_.clear(); panels_.clear();
    double x = 4;
    for (const auto &item : items_) {
        const double width = text_width(item.title) + 24;
        bar_.push_back({x, 0, width, native_menu_bar_height}); x += width;
    }
    const auto *list = &items_;
    rect anchor{};
    for (std::size_t depth = 0; depth < selection_.size(); ++depth) {
        const auto index = selection_[depth];
        if (index >= list->size()) break;
        const auto &item = (*list)[index];
        anchor = depth == 0 ? bar_[index] : panels_.back().rows[index];
        if (item.children.empty()) break;
        list = &item.children;
        double width = 160, height = 8;
        for (const auto &row : *list) {
            width = std::max(width, text_width(row.title) + text_width(row.shortcut) + 72);
            height += row.separator ? 9 : 25;
        }
        width = std::min(width, static_cast<double>(width_));
        const double visible = std::min(height, std::max(1., height_ - double(native_menu_bar_height)));
        double px = depth ? anchor.x + anchor.w - 2 : anchor.x;
        double py = depth ? anchor.y - 4 : native_menu_bar_height;
        if (px + width > width_) px = depth ? anchor.x - width + 2 : width_ - width;
        px = std::clamp(px, 0., std::max(0., width_ - width));
        py = std::clamp(py, double(native_menu_bar_height), std::max(double(native_menu_bar_height), height_ - visible));
        if (scroll_.size() <= depth) scroll_.resize(depth + 1);
        scroll_[depth] = std::clamp(scroll_[depth], 0., height - visible);
        panel p{{px, py, width, visible}, {}, list};
        double y = py + 4 - scroll_[depth];
        for (const auto &row : *list) { const double h = row.separator ? 9 : 25; p.rows.push_back({px, y, width, h}); y += h; }
        panels_.push_back(std::move(p));
    }
}
const native_menu_item *native_menu::selected() const {
    const auto *list = &items_; const native_menu_item *out = nullptr;
    for (auto i : selection_) { if (i >= list->size()) return nullptr; out = &(*list)[i]; list = &out->children; }
    return out;
}
void native_menu::select(std::size_t depth, std::size_t index) {
    selection_.resize(depth); selection_.push_back(index); scroll_.resize(depth + 1); layout(); dirty = true;
}
native_menu_result native_menu::handle(const platform_event &e, point p) {
    native_menu_result result;
    if (e.kind == event_kind::focus_out || e.kind == event_kind::close_requested) { close(); return result; }
    if (empty()) return result;
    if (e.kind == event_kind::mouse_up && swallow_release_) { swallow_release_ = false; result.consumed = true; return result; }
    if (e.kind == event_kind::scroll && opened()) {
        for (std::size_t i = panels_.size(); i-- > 0;) if (contains(panels_[i].frame, p)) {
            scroll_[i] -= e.dy * 3; layout(); dirty = true; break;
        }
        result.consumed = true; return result;
    }
    if (e.kind == event_kind::mouse_down || e.kind == event_kind::mouse_moved || e.kind == event_kind::mouse_dragged) {
        const bool down = e.kind == event_kind::mouse_down;
        if (p.y < native_menu_bar_height && p.y >= 0) {
            result.consumed = true;
            for (std::size_t i = 0; i < bar_.size(); ++i) if (contains(bar_[i], p) && items_[i].enabled) {
                if (down && opened() && selection_[0] == i) close();
                else if (down || opened()) { select(0, i); result.opening = items_[i].token; }
                if (down) swallow_release_ = true;
                break;
            }
            return result;
        }
        if (!opened()) return result;
        result.consumed = true;
        for (std::size_t depth = panels_.size(); depth-- > 0;) {
            auto &panel = panels_[depth];
            if (!contains(panel.frame, p)) continue;
            for (std::size_t i = 0; i < panel.rows.size(); ++i) if (contains(panel.rows[i], p)) {
                const auto item = (*panel.items)[i];
                if (!item.enabled || item.separator) return result;
                const bool changed = selection_.size() <= depth + 1 || selection_[depth + 1] != i;
                if (changed) select(depth + 1, i);
                if (!item.children.empty() && changed) result.opening = item.token;
                if (down && item.children.empty()) { result.action = item.token; close(); swallow_release_ = true; }
                return result;
            }
            return result;
        }
        if (down) { close(); swallow_release_ = true; }
        return result;
    }
    if (e.kind == event_kind::key_up && opened()) { result.consumed = true; return result; }
    if (e.kind != event_kind::key_down) return result;
    if (!opened()) {
        if (e.keycode != 109) return result; // F10
        for (std::size_t i = 0; i < items_.size(); ++i) if (items_[i].enabled) { select(0, i); result.opening = items_[i].token; break; }
        result.consumed = opened(); return result;
    }
    result.consumed = true;
    if (e.keycode == 53) { close(); return result; } // Escape
    if (e.keycode == 36 || e.keycode == 76) {
        if (const auto *item = selected(); item && item->children.empty() && item->enabled) { result.action = item->token; close(); }
        else if (!panels_.empty()) {
            const auto &rows = *panels_.back().items;
            for (std::size_t i = 0; i < rows.size(); ++i) if (rows[i].enabled && !rows[i].separator) { select(panels_.size(), i); break; }
        }
        return result;
    }
    if (e.keycode == 123 || e.keycode == 124) { // left / right
        if (e.keycode == 123 && selection_.size() > 2) { selection_.pop_back(); layout(); dirty = true; return result; }
        if (e.keycode == 124 && selected() && !selected()->children.empty() && selection_.size() > 1) {
            const auto &rows = selected()->children;
            for (std::size_t i = 0; i < rows.size(); ++i) if (rows[i].enabled && !rows[i].separator) { select(selection_.size(), i); break; }
        } else {
            const auto next = (selection_[0] + items_.size() + (e.keycode == 124 ? 1 : -1)) % items_.size();
            select(0, next); result.opening = items_[next].token;
        }
        return result;
    }
    if ((e.keycode == 125 || e.keycode == 126) && !panels_.empty()) {
        const std::size_t depth = panels_.size();
        const auto &rows = *panels_.back().items;
        if (rows.empty()) return result;
        std::size_t index = selection_.size() > depth ? selection_[depth] : (e.keycode == 125 ? rows.size() - 1 : 0);
        for (std::size_t n = 0; n < rows.size(); ++n) {
            index = (index + rows.size() + (e.keycode == 125 ? 1 : -1)) % rows.size();
            if (rows[index].enabled && !rows[index].separator) {
                const auto frame = panels_.back().frame, row = panels_.back().rows[index];
                if (row.y < frame.y + 4) scroll_[depth - 1] -= frame.y + 4 - row.y;
                if (row.y + row.h > frame.y + frame.h - 4) scroll_[depth - 1] += row.y + row.h - frame.y - frame.h + 4;
                select(depth, index); break;
            }
        }
    }
    return result;
}
void native_menu::paint(std::span<std::uint32_t> pixels, bool popup) const {
    if (pixels.size() < std::size_t(width_) * height_) return;
    std::fill(pixels.begin(), pixels.end(), popup ? 0u : 0xff323232u);
#ifdef MM_APPKIT_PANGO
    auto *surface = cairo_image_surface_create_for_data(reinterpret_cast<unsigned char *>(pixels.data()), CAIRO_FORMAT_ARGB32, width_, height_, width_ * 4);
    auto *cr = cairo_create(surface);
    auto fill = [&](rect r, double v) { cairo_set_source_rgb(cr, v, v, v); cairo_rectangle(cr, r.x, r.y, r.w, r.h); cairo_fill(cr); };
    auto text = [&](const std::string &s, double x, double y, double brightness) {
        auto *layout = pango_cairo_create_layout(cr); auto *font = pango_font_description_from_string("Sans 10");
        pango_layout_set_font_description(layout, font); pango_layout_set_text(layout, s.c_str(), -1);
        int h = 0; pango_layout_get_pixel_size(layout, nullptr, &h);
        cairo_set_source_rgb(cr, brightness, brightness, brightness); cairo_move_to(cr, x, y - h / 2.); pango_cairo_show_layout(cr, layout);
        pango_font_description_free(font); g_object_unref(layout);
    };
    if (!popup) {
        fill({0, 0, double(width_), native_menu_bar_height}, .16);
        for (std::size_t i = 0; i < bar_.size(); ++i) {
            if (opened() && selection_[0] == i) fill(bar_[i], .32);
            text(items_[i].title, bar_[i].x + 12, native_menu_bar_height / 2., items_[i].enabled ? .94 : .5);
        }
    } else for (std::size_t d = 0; d < panels_.size(); ++d) {
        const auto &p = panels_[d]; fill(p.frame, .18);
        cairo_save(cr); cairo_rectangle(cr, p.frame.x + 1, p.frame.y + 1, p.frame.w - 2, p.frame.h - 2); cairo_clip(cr);
        for (std::size_t i = 0; i < p.rows.size(); ++i) {
            const auto &r = p.rows[i]; const auto &item = (*p.items)[i];
            if (item.separator) { fill({r.x + 8, r.y + 4, r.w - 16, 1}, .35); continue; }
            if (selection_.size() > d + 1 && selection_[d + 1] == i) fill(r, .33);
            const double color = item.enabled ? .94 : .48;
            if (item.state) text(item.state < 0 ? "−" : "✓", r.x + 7, r.y + r.h / 2, color);
            text(item.title, r.x + 26, r.y + r.h / 2, color);
            if (!item.children.empty()) text("›", r.x + r.w - 17, r.y + r.h / 2, color);
            else text(item.shortcut, r.x + r.w - text_width(item.shortcut) - 16, r.y + r.h / 2, color);
        }
        cairo_restore(cr);
    }
    cairo_destroy(cr); cairo_surface_destroy(surface);
#endif
}
} // namespace ak

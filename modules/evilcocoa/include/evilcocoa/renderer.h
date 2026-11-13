#ifndef EVILCOCOA_RENDERER_H
#define EVILCOCOA_RENDERER_H
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace evilcocoa {
struct rect {
    double x = 0, y = 0, w = 0, h = 0;
    bool operator==(const rect &) const = default;
};
enum class kind {
    label,
    button,
    menu_item,
    checkbox,
    radio,
    text_field,
    popup,
    combo_box,
    slider,
    progress,
    image,
    box,
    separator,
    scroll_view,
    scroller,
    table,
    table_row,
    segments
};
enum class alignment { left, center, right };
enum class image_scaling { fit, fill, stretch, center };
struct bitmap {
    int width = 0, height = 0;
    std::vector<std::uint32_t> argb; // Premultiplied ARGB, top row first.
};
struct item {
    kind type = kind::label;
    rect frame, clip;
    std::string text, placeholder, font_family="Sans";
    std::optional<std::array<float,4>> text_color;
    double font_size = 13, value = 0, knob = .25;
    alignment align = alignment::left;
    image_scaling scaling = image_scaling::fit;
    std::shared_ptr<const bitmap> pixels;
    std::vector<std::string> segments;
    int selected = -1;
    bool enabled = true, pressed = false, checked = false, focused = false;
    bool bordered = true, background = false, bold = false, wrap = false, vertical = false;
    bool indeterminate = false, default_button = false, help_button = false;
    bool operator==(const item &) const = default;
};
/* Coordinates are logical points, top-left origin. The caller owns the
 * OpenGL 3.3 context and framebuffer. Destroy while that context is current. */
class renderer {
    struct impl;
    std::unique_ptr<impl> impl_;

  public:
    renderer();
    ~renderer();
    renderer(const renderer &) = delete;
    renderer &operator=(const renderer &) = delete;
    void draw(std::span<const item> items, int width, int height, double scale = 1);
    bool animating() const;
};
} // namespace evilcocoa
#endif

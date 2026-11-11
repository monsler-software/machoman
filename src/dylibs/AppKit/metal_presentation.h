#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ak {

// Source pixels have a top-left origin. Placement values match NSView's enum.
// Keep allocation padding outside the visible region for unscaled placement.
inline void copy_metal_contents(void *output, unsigned out_width, unsigned out_height,
        double logical_width, double logical_height, const void *pixels,
        unsigned width, unsigned height, std::uint64_t stride, double scale,
        unsigned placement) {
    double draw_width = width / scale, draw_height = height / scale;
    double left = 0, top = 0;
    if (placement == 0) {
        draw_width = logical_width; draw_height = logical_height;
    } else if (placement == 1 || placement == 2) {
        const double sx = logical_width / draw_width, sy = logical_height / draw_height;
        const double factor = placement == 1 ? std::min(sx, sy) : std::max(sx, sy);
        draw_width *= factor; draw_height *= factor;
        left = (logical_width - draw_width) / 2;
        top = (logical_height - draw_height) / 2;
    } else {
        if (placement == 3 || placement == 4 || placement == 8)
            left = (logical_width - draw_width) / 2;
        else if (placement == 5 || placement == 6 || placement == 7)
            left = logical_width - draw_width;
        if (placement == 3 || placement == 6 || placement == 10)
            top = (logical_height - draw_height) / 2;
        else if (placement == 7 || placement == 8 || placement == 9)
            top = logical_height - draw_height;
    }
    const double source_x_step = logical_width / out_width * width / draw_width;
    const double source_y_step = logical_height / out_height * height / draw_height;
    const double source_x_start = -left * width / draw_width;
    const double source_y_start = -top * height / draw_height;
    for (unsigned y = 0; y < out_height; ++y) {
        auto *dst = static_cast<std::uint8_t *>(output) + std::uint64_t{y} * out_width * 4;
        const double sy = std::floor(source_y_start + y * source_y_step);
        if (sy < 0 || sy >= height) { std::memset(dst, 0, std::uint64_t{out_width} * 4); continue; }
        const auto *src = static_cast<const std::uint8_t *>(pixels) + static_cast<std::uint64_t>(sy) * stride;
        if (source_x_start == 0 && source_x_step == 1 && out_width <= width) {
            std::memcpy(dst, src, std::uint64_t{out_width} * 4);
            continue;
        }
        for (unsigned x = 0; x < out_width; ++x) {
            const double sx = std::floor(source_x_start + x * source_x_step);
            if (sx < 0 || sx >= width) std::memset(dst + x * 4, 0, 4);
            else std::memcpy(dst + x * 4, src + static_cast<std::uint64_t>(sx) * 4, 4);
        }
    }
}

} // namespace ak

#include "evilcocoa/renderer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <epoxy/gl.h>
#include <pango/pangocairo.h>
#include <stdexcept>
#include <unordered_map>

namespace evilcocoa {
namespace {
GLuint shader(GLenum type, const char *source) {
    auto value = glCreateShader(type);
    glShaderSource(value, 1, &source, nullptr);
    glCompileShader(value);
    GLint ok = 0;
    glGetShaderiv(value, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        std::array<char, 2048> log{};
        glGetShaderInfoLog(value, log.size(), nullptr, log.data());
        glDeleteShader(value);
        throw std::runtime_error(std::string("evilcocoa shader: ") + log.data());
    }
    return value;
}
constexpr const char *vertex = R"(#version 330 core
uniform vec2 viewport;
uniform vec4 frame;
out vec2 uv;
const vec2 corners[6] = vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),vec2(0,1),vec2(1,0),vec2(1,1));
void main() {
    uv = corners[gl_VertexID];
    vec2 p = frame.xy + uv * frame.zw;
    gl_Position = vec4(p.x / viewport.x * 2.0 - 1.0, 1.0 - p.y / viewport.y * 2.0, 0, 1);
})";
constexpr const char *fragment = R"(#version 330 core
in vec2 uv;
out vec4 color;
uniform vec2 extent;
uniform float radius;
uniform vec4 ink;
uniform int texture_mode;
uniform sampler2D image;
void main() {
    float coverage;
    if (texture_mode == 2) { color = texture(image, uv) * ink.a; return; }
    if (texture_mode == 1) coverage = texture(image, uv).a;
    else {
        vec2 q = abs((uv - 0.5) * extent) - (extent * 0.5 - radius);
        float distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
        coverage = 1.0 - smoothstep(-0.5, 0.5, distance);
    }
    float a = ink.a * coverage;
    color = vec4(ink.rgb * a, a);
})";
struct texture {
    GLuint name = 0;
    int w = 0, h = 0;
};
} // namespace
struct renderer::impl {
    GLuint program = 0, vao = 0;
    GLint viewport, frame, extent, radius, ink, textured;
    struct cached_image {
        std::shared_ptr<const bitmap> pixels;
        texture gl;
    };
    std::vector<cached_image> images;
    std::unordered_map<std::string, texture> text;
    impl() {
        auto vs = shader(GL_VERTEX_SHADER, vertex), fs = shader(GL_FRAGMENT_SHADER, fragment);
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            glDeleteProgram(program);
            throw std::runtime_error("evilcocoa program link failed");
        }
        glGenVertexArrays(1, &vao);
        viewport = glGetUniformLocation(program, "viewport");
        frame = glGetUniformLocation(program, "frame");
        extent = glGetUniformLocation(program, "extent");
        radius = glGetUniformLocation(program, "radius");
        ink = glGetUniformLocation(program, "ink");
        textured = glGetUniformLocation(program, "texture_mode");
    }
    ~impl() {
        for (auto &i : images)
            glDeleteTextures(1, &i.gl.name);
        clear_text();
        glDeleteVertexArrays(1, &vao);
        glDeleteProgram(program);
    }
    void clear_text() {
        for (auto &[key, t] : text)
            glDeleteTextures(1, &t.name);
        text.clear();
    }
    void quad(rect r, std::array<float, 4> color, float corner, GLuint image = 0, bool rgba = false) {
        if (r.w <= 0 || r.h <= 0)
            return;
        glUniform4f(frame, float(r.x), float(r.y), float(r.w), float(r.h));
        glUniform2f(extent, float(r.w), float(r.h));
        glUniform1f(radius, corner);
        glUniform4fv(ink, 1, color.data());
        glUniform1i(textured, image ? (rgba ? 2 : 1) : 0);
        glBindTexture(GL_TEXTURE_2D, image);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    texture bitmap_texture(const std::shared_ptr<const bitmap> &pixels) {
        if (!pixels || pixels->width <= 0 || pixels->height <= 0 || pixels->width > 8192 || pixels->height > 8192 ||
            pixels->argb.size() != std::size_t(pixels->width) * pixels->height)
            return {};
        for (auto &i : images)
            if (i.pixels == pixels)
                return i.gl;
        // Dirty custom views publish fresh bitmap snapshots. Drop textures
        // whose snapshots are no longer referenced by any current view/item.
        std::erase_if(images, [](const auto &i) {
            if (i.pixels.use_count() != 1) return false;
            glDeleteTextures(1, &i.gl.name);
            return true;
        });
        if (images.size() >= 128) {
            for (auto &i : images)
                glDeleteTextures(1, &i.gl.name);
            images.clear();
        }
        texture t;
        t.w = pixels->width;
        t.h = pixels->height;
        std::vector<unsigned char> rgba(pixels->argb.size() * 4);
        for (std::size_t i = 0; i < pixels->argb.size(); ++i) {
            auto c = pixels->argb[i];
            rgba[i * 4] = c >> 16;
            rgba[i * 4 + 1] = c >> 8;
            rgba[i * 4 + 2] = c;
            rgba[i * 4 + 3] = c >> 24;
        }
        glGenTextures(1, &t.name);
        glBindTexture(GL_TEXTURE_2D, t.name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        images.push_back({pixels, t});
        return t;
    }
    texture label(const item &i, int width, double scale) {
        const auto key = i.text + '\0' + std::to_string(width) + ':' + std::to_string(i.font_size * scale) + ':' +
                         std::to_string(i.bold) + ':' + std::to_string(i.wrap) + ':' + std::to_string(int(i.align)) + ':' + i.font_family;
        if (auto it = text.find(key); it != text.end())
            return it->second;
        if (text.size() >= 256)
            clear_text();
        auto *measure = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        auto *cr = cairo_create(measure);
        auto *layout = pango_cairo_create_layout(cr);
        auto *font = pango_font_description_new();
        pango_font_description_set_family(font, i.font_family.c_str());
        pango_font_description_set_weight(font, i.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
        pango_font_description_set_absolute_size(font, i.font_size * scale * PANGO_SCALE);
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        pango_layout_set_text(layout, i.text.c_str(), static_cast<int>(i.text.size()));
        pango_layout_set_width(layout, std::max(1, width) * PANGO_SCALE);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        if (!i.wrap) {
            pango_layout_set_single_paragraph_mode(layout, true);
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        }
        pango_layout_set_alignment(layout, i.align == alignment::center  ? PANGO_ALIGN_CENTER
                                           : i.align == alignment::right ? PANGO_ALIGN_RIGHT
                                                                         : PANGO_ALIGN_LEFT);
        int w = 0, h = 0;
        pango_layout_get_pixel_size(layout, &w, &h);
        w = std::max(1, width); // Alignment offsets belong inside the full layout width.
        auto *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, std::max(1, w), std::max(1, h));
        auto *paint = cairo_create(surface);
        cairo_set_source_rgba(paint, 1, 1, 1, 1);
        pango_cairo_update_layout(paint, layout);
        pango_cairo_show_layout(paint, layout);
        cairo_surface_flush(surface);
        texture t;
        t.w = w;
        t.h = h;
        glGenTextures(1, &t.name);
        glBindTexture(GL_TEXTURE_2D, t.name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, std::max(1, w), std::max(1, h), 0, GL_BGRA, GL_UNSIGNED_BYTE,
                     cairo_image_surface_get_data(surface));
        cairo_destroy(paint);
        cairo_surface_destroy(surface);
        g_object_unref(layout);
        cairo_destroy(cr);
        cairo_surface_destroy(measure);
        text.emplace(key, t);
        return t;
    }
};
renderer::renderer() : impl_(std::make_unique<impl>()) {}
renderer::~renderer() = default;
void renderer::draw(std::span<const item> items, int width, int height, double scale) {
    if (width <= 0 || height <= 0 || !std::isfinite(scale) || scale <= 0)
        return;
    auto &p = *impl_;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glClearColor(.18f, .18f, .20f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(p.program);
    glBindVertexArray(p.vao);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(p.program, "image"), 0);
    glUniform2f(p.viewport, float(width), float(height));
    glEnable(GL_SCISSOR_TEST);
    auto set_clip = [&](rect clip) {
        const int left = std::clamp(int(std::floor(clip.x)), 0, width),
                  top = std::clamp(int(std::floor(clip.y)), 0, height);
        const int right = std::clamp(int(std::ceil(clip.x + clip.w)), 0, width),
                  bottom = std::clamp(int(std::ceil(clip.y + clip.h)), 0, height);
        glScissor(left, height - bottom, std::max(0, right - left), std::max(0, bottom - top));
    };
    const std::array<float, 4> edge{.46f, .46f, .50f, 1}, accent{.18f, .48f, .92f, 1}, panel{.12f, .12f, .14f, 1};
    for (const auto &i : items) {
        const rect r{i.frame.x * scale, i.frame.y * scale, i.frame.w * scale, i.frame.h * scale};
        rect clip{i.clip.x * scale, i.clip.y * scale, i.clip.w * scale, i.clip.h * scale};
        if (r.w <= 0 || r.h <= 0 || clip.w <= 0 || clip.h <= 0)
            continue;
        // The view clip also constrains glyphs and aspect-fill images.
        const double cx = std::max(r.x, clip.x), cy = std::max(r.y, clip.y);
        clip = {cx, cy, std::max(0., std::min(r.x + r.w, clip.x + clip.w) - cx),
                std::max(0., std::min(r.y + r.h, clip.y + clip.h) - cy)};
        set_clip(clip);
        auto fg = i.enabled ? std::array<float, 4>{.94f, .94f, .96f, 1} : std::array<float, 4>{.50f, .50f, .54f, 1};
        const double v = std::clamp(std::isfinite(i.value) ? i.value : 0., 0., 1.);
        auto bezel = [&](rect box, bool active = false, float radius = 5) {
            p.quad(box, i.focused ? accent : edge, float(radius * scale));
            p.quad({box.x + scale, box.y + scale, box.w - 2 * scale, box.h - 2 * scale},
                   active      ? accent
                   : i.pressed ? std::array<float, 4>{.22f, .36f, .58f, 1}
                               : std::array<float, 4>{.31f, .31f, .34f, 1},
                   float(std::max(0.f, radius - 1) * scale));
        };
        auto draw_text = [&](std::string text, rect box, alignment align, bool middle = true) {
            if (text.empty() || box.w <= 0 || box.h <= 0)
                return;
            item label = i;
            label.text = std::move(text);
            label.align = align;
            auto t = p.label(label, std::max(1, int(std::floor(box.w))), scale);
            p.quad({box.x, box.y + (middle ? (box.h - t.h) / 2 : 0), double(t.w), double(t.h)}, fg, 0, t.name);
        };
        double inset = 0, right_inset = 0;
        bool middle = i.type != kind::label;
        if (i.type == kind::menu_item) {
            inset = 24 * scale;
            if (i.pressed) p.quad(r, accent, float(3 * scale));
            if (i.checked) draw_text("✓", {r.x + 3 * scale, r.y, 18 * scale, r.h}, alignment::center);
        } else if (i.type == kind::button || i.type == kind::popup || i.type == kind::combo_box ||
            i.type == kind::text_field) {
            if (i.bordered)
                bezel(r, i.default_button, i.help_button ? float(std::min(r.w, r.h) / scale / 2) : 5);
            if (i.type == kind::text_field || i.type == kind::combo_box) {
                if (i.background || i.bordered)
                    p.quad({r.x + scale, r.y + scale, r.w - 2 * scale, r.h - 2 * scale}, panel, float(4 * scale));
            }
            inset = 6 * scale;
            right_inset = inset;
            if (i.help_button) {
                draw_text("?", r, alignment::center);
                continue;
            }
            if (i.type == kind::popup || i.type == kind::combo_box) {
                right_inset = 24 * scale;
                const double cx = r.x + r.w - right_inset / 2, cy = r.y + r.h / 2;
                // A geometric chevron stays centered regardless of font metrics.
                for (int step = 0; step < 4; ++step) {
                    const double offset = step * scale;
                    p.quad({cx - 4 * scale + offset, cy - 2 * scale + offset, 2 * scale, 2 * scale}, fg, float(scale));
                    p.quad({cx + 3 * scale - offset, cy - 2 * scale + offset, 2 * scale, 2 * scale}, fg, float(scale));
                }
            }
        } else if (i.type == kind::checkbox || i.type == kind::radio) {
            double side = std::min(r.h, 16 * scale), y = r.y + (r.h - side) / 2;
            bezel({r.x, y, side, side}, i.checked, i.type == kind::radio ? 8 : 3);
            if (i.checked) {
                if (i.type == kind::radio)
                    p.quad({r.x + 5 * scale, y + 5 * scale, side - 10 * scale, side - 10 * scale}, fg,
                           float(4 * scale));
                else
                    draw_text(i.selected == -1 ? "✓" : "−", {r.x, y, side, side}, alignment::center);
            }
            inset = side + 6 * scale;
        } else if (i.type == kind::slider) {
            if (i.vertical) {
                p.quad({r.x + r.w / 2 - 2 * scale, r.y + 7 * scale, 4 * scale, r.h - 14 * scale}, edge,
                       float(2 * scale));
                p.quad({r.x + r.w / 2 - 7 * scale, r.y + (1 - v) * (r.h - 14 * scale), 14 * scale, 14 * scale}, fg,
                       float(7 * scale));
            } else {
                p.quad({r.x + 7 * scale, r.y + r.h / 2 - 2 * scale, r.w - 14 * scale, 4 * scale}, edge,
                       float(2 * scale));
                p.quad({r.x + 7 * scale, r.y + r.h / 2 - 2 * scale, (r.w - 14 * scale) * v, 4 * scale}, accent,
                       float(2 * scale));
                p.quad({r.x + (r.w - 14 * scale) * v, r.y + r.h / 2 - 7 * scale, 14 * scale, 14 * scale}, fg,
                       float(7 * scale));
            }
            continue;
        } else if (i.type == kind::progress) {
            p.quad(r, panel, float(std::min(r.h / 2, 4 * scale)));
            if (i.indeterminate)
                p.quad({r.x + (r.w * 1.35 * v - r.w * .35), r.y, r.w * .35, r.h}, accent, float(4 * scale));
            else
                p.quad({r.x, r.y, r.w * v, r.h}, accent, float(std::min(r.h / 2, 4 * scale)));
            continue;
        } else if (i.type == kind::scroller) {
            if (i.background)
                p.quad(r, panel, 0);
            const double fraction = std::clamp(i.knob, .05, 1.);
            rect thumb = i.vertical
                             ? rect{r.x + 3 * scale, r.y + (r.h - r.h * fraction) * v, r.w - 6 * scale, r.h * fraction}
                             : rect{r.x + (r.w - r.w * fraction) * v, r.y + 3 * scale, r.w * fraction, r.h - 6 * scale};
            p.quad(thumb, edge, float(4 * scale));
            continue;
        } else if (i.type == kind::box || i.type == kind::scroll_view || i.type == kind::table) {
            if (i.background || i.type != kind::box)
                p.quad(r, panel, float(i.type == kind::box ? 4 * scale : 0));
            if (i.bordered) {
                p.quad({r.x, r.y, r.w, scale}, edge, 0);
                p.quad({r.x, r.y + r.h - scale, r.w, scale}, edge, 0);
                p.quad({r.x, r.y, scale, r.h}, edge, 0);
                p.quad({r.x + r.w - scale, r.y, scale, r.h}, edge, 0);
            }
            inset = 8 * scale;
            middle = false;
        } else if (i.type == kind::separator) {
            p.quad(i.vertical ? rect{r.x + r.w / 2, r.y, scale, r.h} : rect{r.x, r.y + r.h / 2, r.w, scale}, edge, 0);
            continue;
        } else if (i.type == kind::table_row) {
            if (i.checked)
                p.quad(r, accent, 0);
            else if (i.background)
                p.quad(r, {.16f, .16f, .18f, 1}, 0);
            inset = 6 * scale;
        } else if (i.type == kind::segments) {
            bezel(r);
            double w = r.w / std::max(std::size_t(1), i.segments.size());
            for (std::size_t n = 0; n < i.segments.size(); ++n) {
                rect segment{r.x + n * w, r.y, w, r.h};
                if (int(n) == i.selected)
                    p.quad({segment.x + scale, segment.y + scale, segment.w - 2 * scale, segment.h - 2 * scale}, accent,
                           float(4 * scale));
                if (n)
                    p.quad({segment.x, segment.y + 3 * scale, scale, segment.h - 6 * scale}, edge, 0);
                draw_text(i.segments[n], {segment.x + 4 * scale, segment.y, segment.w - 8 * scale, segment.h},
                          alignment::center);
            }
            continue;
        }
        if (i.pixels) {
            auto t = p.bitmap_texture(i.pixels);
            if (t.name) {
                rect box = r;
                if (i.type != kind::image) {
                    double side = std::min(r.h - 4 * scale, 20 * scale);
                    box = {r.x + inset, r.y + (r.h - side) / 2, side, side};
                    inset += side + 4 * scale;
                }
                double ratio = i.scaling == image_scaling::fill ? std::max(box.w / t.w, box.h / t.h)
                                                                : std::min(box.w / t.w, box.h / t.h);
                double w = i.scaling == image_scaling::stretch  ? box.w
                           : i.scaling == image_scaling::center ? t.w * scale
                                                                : t.w * ratio;
                double h = i.scaling == image_scaling::stretch  ? box.h
                           : i.scaling == image_scaling::center ? t.h * scale
                                                                : t.h * ratio;
                p.quad({box.x + (box.w - w) / 2, box.y + (box.h - h) / 2, w, h}, {1, 1, 1, i.enabled ? 1.f : .45f}, 0,
                       t.name, true);
            }
        }
        if (i.type == kind::image)
            continue;
        std::string title = i.text;
        if (title.empty() && !i.placeholder.empty()) {
            title = i.placeholder;
            fg = {.52f, .52f, .56f, 1};
        }
        draw_text(title, {r.x + inset, r.y, r.w - inset - right_inset, r.h},
                  i.type == kind::button ? alignment::center : i.align, middle);
    }
    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(0);
    glUseProgram(0);
}
} // namespace evilcocoa

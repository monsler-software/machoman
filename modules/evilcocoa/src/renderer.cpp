#include "evilcocoa/renderer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
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
uniform float padding;
uniform float softness;
uniform vec4 ink;
uniform vec4 shade;
uniform vec4 segment;
uniform int texture_mode;
uniform sampler2D image;
void main() {
    float coverage;
    if (texture_mode == 2) { color = texture(image, uv) * ink.a; return; }
    if (texture_mode == 1) coverage = texture(image, uv).a;
    else if (texture_mode == 3) {
        vec2 a = segment.xy, b = segment.zw, p = uv * extent;
        vec2 delta = b - a;
        float along = clamp(dot(p - a, delta) / max(dot(delta, delta), 0.001), 0.0, 1.0);
        float distance = length(p - a - along * delta) - radius;
        coverage = 1.0 - smoothstep(-0.5, 0.5, distance);
    }
    else {
        vec2 q = abs((uv - 0.5) * extent) - (extent * 0.5 - padding - radius);
        float distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
        coverage = 1.0 - smoothstep(-softness, softness, distance);
    }
    vec4 fill = mix(ink, shade, uv.y);
    float a = fill.a * coverage;
    color = vec4(fill.rgb * a, a);
})";
struct texture {
    GLuint name = 0;
    int w = 0, h = 0, content_w = 0;
};
} // namespace
struct renderer::impl {
    GLuint program = 0, vao = 0;
    GLint viewport, frame, extent, radius, padding, softness, ink, shade, segment, textured;
    struct cached_image {
        std::shared_ptr<const bitmap> pixels;
        texture gl;
    };
    std::vector<cached_image> images;
    std::unordered_map<std::string, texture> text;
    struct highlight_animation {
        rect frame;
        std::string text;
        float from = 0, target = 0;
        double changed = 0;
    };
    std::unordered_map<std::size_t,highlight_animation> menu_highlights;
    bool animations_running = false;
    float menu_highlight(std::size_t index, const item &i, double now) {
        const float target = i.pressed && i.enabled ? 1.f : 0.f;
        auto [it,inserted] = menu_highlights.try_emplace(index,highlight_animation{i.frame,i.text,target,target,now});
        auto &a = it->second;
        if (!inserted && (a.frame != i.frame || a.text != i.text)) a = {i.frame,i.text,target,target,now};
        auto amount = [&] {
            const float t = float(std::clamp((now-a.changed)/.12,0.,1.));
            return a.from + (a.target-a.from)*t*t*(3-2*t);
        };
        if (a.target != target) { a.from = amount(); a.target = target; a.changed = now; }
        if (now-a.changed < .12 && a.from != a.target) animations_running = true;
        return amount();
    }
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
        padding = glGetUniformLocation(program, "padding");
        softness = glGetUniformLocation(program, "softness");
        ink = glGetUniformLocation(program, "ink");
        shade = glGetUniformLocation(program, "shade");
        segment = glGetUniformLocation(program, "segment");
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
    void quad(rect r, std::array<float, 4> color, float corner, GLuint image = 0, bool rgba = false,
              const std::array<float, 4> *bottom = nullptr, float pad = 0, float blur = .5f, int shape = 0) {
        if (r.w <= 0 || r.h <= 0)
            return;
        glUniform4f(frame, float(r.x), float(r.y), float(r.w), float(r.h));
        glUniform2f(extent, float(r.w), float(r.h));
        glUniform1f(radius, corner);
        glUniform1f(padding, pad);
        glUniform1f(softness, blur);
        glUniform4fv(ink, 1, color.data());
        glUniform4fv(shade, 1, bottom ? bottom->data() : color.data());
        glUniform1i(textured, image ? (rgba ? 2 : 1) : shape);
        glBindTexture(GL_TEXTURE_2D, image);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    void gradient(rect r, std::array<float, 4> top, std::array<float, 4> bottom, float corner) {
        quad(r, top, corner, 0, false, &bottom);
    }
    void stroke(double ax, double ay, double bx, double by, float width, std::array<float,4> color) {
        const double margin = width / 2 + 1;
        rect r{std::min(ax,bx)-margin,std::min(ay,by)-margin,
               std::abs(bx-ax)+2*margin,std::abs(by-ay)+2*margin};
        glUniform4f(segment,float(ax-r.x),float(ay-r.y),float(bx-r.x),float(by-r.y));
        quad(r,color,width/2,0,false,nullptr,0,.5f,3);
    }
    void shadow(rect r, float corner, double scale, float opacity = .28f) {
        const double blur = 2 * scale;
        quad({r.x-blur,r.y-blur+scale,r.w+2*blur,r.h+2*blur}, {0,0,0,opacity}, corner,
             0,false,nullptr,float(blur),float(blur));
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
        int natural_width = 0;
        pango_layout_get_pixel_size(layout, &natural_width, nullptr);
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
        t.content_w = std::min(width, natural_width + 1);
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
bool renderer::animating() const { return impl_->animations_running; }
void renderer::draw(std::span<const item> items, int width, int height, double scale) {
    if (width <= 0 || height <= 0 || !std::isfinite(scale) || scale <= 0)
        return;
    auto &p = *impl_;
    p.animations_running = false;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (p.menu_highlights.size() > items.size()) p.menu_highlights.clear();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glClearColor(.17f, .17f, .18f, 1);
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
    const std::array<float, 4> edge{.38f, .38f, .40f, 1}, accent{.16f, .46f, .94f, 1}, panel{.12f, .12f, .13f, 1};
    std::size_t item_index = 0;
    for (const auto &i : items) {
        const std::size_t index = item_index++;
        const rect r{i.frame.x * scale, i.frame.y * scale, i.frame.w * scale, i.frame.h * scale};
        rect clip{i.clip.x * scale, i.clip.y * scale, i.clip.w * scale, i.clip.h * scale};
        if (r.w <= 0 || r.h <= 0 || clip.w <= 0 || clip.h <= 0)
            continue;
        // Shadows belong outside a control's bounds, but remain clipped by
        // its containing view. Glyphs and images are clipped below.
        set_clip(clip);
        if (i.bordered && (i.type == kind::button || i.type == kind::popup || i.type == kind::combo_box))
            p.shadow(r,float((i.help_button ? std::min(r.w,r.h)/scale/2 : 6)*scale),scale,i.enabled ? .30f : .12f);
        if (i.type == kind::checkbox || i.type == kind::radio) {
            double side = std::min(r.h,16*scale);
            p.shadow({r.x,r.y+(r.h-side)/2,side,side},float((i.type==kind::radio ? 8 : 3)*scale),scale,.22f);
        }
        // The view clip also constrains glyphs and aspect-fill images.
        const double cx = std::max(r.x, clip.x), cy = std::max(r.y, clip.y);
        clip = {cx, cy, std::max(0., std::min(r.x + r.w, clip.x + clip.w) - cx),
                std::max(0., std::min(r.y + r.h, clip.y + clip.h) - cy)};
        set_clip(clip);
        auto fg = i.enabled ? i.text_color.value_or(std::array<float, 4>{.90f, .90f, .92f, 1})
                            : std::array<float, 4>{.53f, .53f, .55f, 1};
        const double v = std::clamp(std::isfinite(i.value) ? i.value : 0., 0., 1.);
        auto bezel = [&](rect box, bool active = false, float radius = 6) {
            active = active && i.enabled;
            auto border_top = active ? std::array<float, 4>{.14f, .40f, .82f, 1}
                                          : std::array<float, 4>{.43f, .43f, .45f, 1};
            auto border_bottom = active ? std::array<float, 4>{.08f, .29f, .67f, 1} : edge;
            if (!i.enabled) { border_top = {.30f,.30f,.32f,1}; border_bottom = {.24f,.24f,.26f,1}; }
            p.gradient(box, i.focused ? accent : border_top, i.focused ? accent : border_bottom,
                       float(radius * scale));
            auto top = active ? std::array<float, 4>{.23f, .54f, 1.f, 1}
                             : std::array<float, 4>{.40f, .40f, .42f, 1};
            auto bottom = active ? accent : std::array<float, 4>{.34f, .34f, .36f, 1};
            if (i.pressed) {
                for (int channel = 0; channel < 3; ++channel) { top[channel] *= .78f; bottom[channel] *= .78f; }
            }
            if (!i.enabled) { top = {.25f, .25f, .27f, 1}; bottom = {.23f, .23f, .25f, 1}; }
            const double line = i.focused ? 2 * scale : scale;
            // A single subtle rim provides the highlight without stacking
            // bright rounded contours above the gradient face.
            p.gradient({box.x + line, box.y + line, box.w - 2 * line, box.h - 2 * line}, top, bottom,
                       float(std::max(0., radius * scale - line)));
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
        bool middle = !i.wrap;
        if (i.type == kind::menu_item) {
            inset = 24 * scale;
            auto highlight = accent;
            highlight[3] = p.menu_highlight(index,i,now);
            if (highlight[3] > 0) p.quad(r, highlight, float(3 * scale));
            if (i.checked) draw_text("✓", {r.x + 3 * scale, r.y, 18 * scale, r.h}, alignment::center);
        } else if (i.type == kind::button || i.type == kind::popup || i.type == kind::combo_box ||
            i.type == kind::text_field) {
            if (i.bordered)
                bezel(r, i.default_button, i.help_button ? float(std::min(r.w, r.h) / scale / 2) : 6);
            if (i.type == kind::text_field || i.type == kind::combo_box) {
                if (i.background || i.bordered)
                    p.quad({r.x + scale, r.y + scale, r.w - 2 * scale, r.h - 2 * scale}, panel, float(4 * scale));
            }
            inset = (i.type == kind::text_field && !i.bordered ? 2 : 6) * scale;
            right_inset = inset;
            if (i.help_button) {
                draw_text("?", r, alignment::center);
                continue;
            }
            if (i.type == kind::popup || i.type == kind::combo_box) {
                right_inset = 24 * scale;
                const double cx = r.x + r.w - 11 * scale, cy = r.y + r.h / 2;
                auto chevron = [&](double center_y, bool up) {
                    const double wing = center_y + (up ? 2 : -2) * scale;
                    p.stroke(cx-2.5*scale,wing,cx,center_y,float(1.25*scale),fg);
                    p.stroke(cx,center_y,cx+2.5*scale,wing,float(1.25*scale),fg);
                };
                if (i.type == kind::popup) {
                    const double height = std::min(16*scale,r.h-4*scale);
                    rect badge{cx-8*scale,cy-height/2,16*scale,height};
                    auto top = i.enabled ? std::array<float,4>{.16f,.56f,1,1} : std::array<float,4>{.31f,.31f,.33f,1};
                    auto bottom = i.enabled ? accent : std::array<float,4>{.27f,.27f,.29f,1};
                    if (i.pressed) { for (int c=0;c<3;++c) { top[c]*=.78f; bottom[c]*=.78f; } }
                    p.gradient(badge,top,bottom,float(4*scale));
                    chevron(cy-3.5*scale,true);
                    chevron(cy+3.5*scale,false);
                } else chevron(cy+scale,false);
            }
        } else if (i.type == kind::checkbox || i.type == kind::radio) {
            double side = std::min(r.h, 16 * scale), y = r.y + (r.h - side) / 2;
            bezel({r.x, y, side, side}, i.checked, i.type == kind::radio ? 8 : 3);
            if (i.checked) {
                if (i.type == kind::radio)
                    p.quad({r.x + 5 * scale, y + 5 * scale, side - 10 * scale, side - 10 * scale}, fg,
                           float(4 * scale));
                else {
                    const double unit = side / 16;
                    if (i.selected == -1) {
                        p.stroke(r.x+3.5*unit,y+8*unit,r.x+6.5*unit,y+11*unit,float(1.5*unit),fg);
                        p.stroke(r.x+6.5*unit,y+11*unit,r.x+12*unit,y+5*unit,float(1.5*unit),fg);
                    } else p.stroke(r.x+4*unit,y+8*unit,r.x+12*unit,y+8*unit,float(1.5*unit),fg);
                }
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
            if (!i.enabled) continue;
            const double fraction = std::clamp(i.knob, .05, 1.);
            rect thumb = i.vertical
                             ? rect{r.x + 3 * scale, r.y + (r.h - r.h * fraction) * v, r.w - 6 * scale, r.h * fraction}
                             : rect{r.x + (r.w - r.w * fraction) * v, r.y + 3 * scale, r.w * fraction, r.h - 6 * scale};
            p.quad(thumb, edge, float(4 * scale));
            continue;
        } else if (i.type == kind::box) {
            const bool titled = !i.text.empty();
            const double offset = titled ? 8 * scale : 0;
            rect box{r.x, r.y + offset, r.w, r.h - offset};
            if (i.bordered) {
                p.quad(box, {.34f, .34f, .36f, 1}, float(5 * scale));
                p.quad({box.x + scale, box.y + scale, box.w - 2 * scale, box.h - 2 * scale},
                       {.17f, .17f, .18f, 1}, float(4 * scale));
            }
            if (titled) {
                item title_label = i;
                title_label.align = alignment::left;
                auto title_texture = p.label(title_label, std::max(1, int(r.w - 32 * scale)), scale);
                const double title_width = std::min(r.w - 24 * scale, title_texture.content_w + 8 * scale);
                rect title{r.x + 12 * scale, r.y, title_width, 18 * scale};
                p.quad(title, {.17f, .17f, .18f, 1}, 0);
                draw_text(i.text, {title.x + 4 * scale, title.y, title.w - 8 * scale, title.h}, alignment::left);
            }
            continue;
        } else if (i.type == kind::scroll_view || i.type == kind::table) {
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

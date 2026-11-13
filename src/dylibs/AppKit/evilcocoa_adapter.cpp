#include "app_icon.h"
#include "appkit.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <evilcocoa/asset_catalog.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#ifdef MM_APPKIT_PIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

extern "C" void *cocoa_method(mm_objc_class *, mm_SEL) __asm__("_class_getMethodImplementation");
namespace ak {
void decode_cocoa_image(id image) {
    if (!image || !is_kind(image, cls("NSImage")))
        return;
    auto *o = as<image_object>(image);
    if (o->decoded)
        return;
    o->decoded = true;
    auto pixels = std::make_shared<evilcocoa::bitmap>();
    if (o->name == "NSApplicationIcon") {
        const auto &icon = main_application_identity().icon;
        pixels->width = icon.width;
        pixels->height = icon.height;
        pixels->argb = icon.argb;
    } else {
        namespace fs = std::filesystem;
        std::vector<fs::path> paths;
        const fs::path name(o->name);
        if (o->name == "NSFolder" || o->name == "NSMultipleDocuments") {
            const bool folder = o->name == "NSFolder";
            const std::string icon = folder ? "folder" : "text-x-generic";
            const std::string category = folder ? "places" : "mimetypes";
            paths.push_back(fs::path("/usr/share/icons/breeze") / category / "64" / (icon + ".svg"));
            paths.push_back(fs::path("/usr/share/icons/Adwaita/scalable") / category / (icon + ".svg"));
            paths.push_back(fs::path("/usr/share/icons/AdwaitaLegacy/48x48") / category / (icon + ".png"));
        } else if (name.is_absolute())
            paths.push_back(name);
        else {
            id bundle = obj(send(reinterpret_cast<id>(cls("NSBundle")), "mainBundle"));
            fs::path root(to_utf8(obj(send(bundle, "resourcePath"))));
            paths.push_back(root / name);
            if (!name.has_extension())
                for (auto extension : {".png", ".icns", ".tiff", ".tif", ".jpg", ".jpeg"})
                    paths.push_back(root / (o->name + extension));
        }
        if (!name.is_absolute()) {
            id bundle = obj(send(reinterpret_cast<id>(cls("NSBundle")), "mainBundle"));
            fs::path root(to_utf8(obj(send(bundle, "resourcePath"))));
            std::ifstream stream(root / "Assets.car", std::ios::binary | std::ios::ate);
            if (stream && stream.tellg() > 0 && stream.tellg() < 256 * 1024 * 1024) {
                std::vector<std::uint8_t> bytes(std::size_t(stream.tellg()));
                stream.seekg(0);
                stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
                if (stream)
                    *pixels = evilcocoa::read_asset_catalog(bytes, o->name);
            }
        }
        for (auto &path : paths) {
            if (pixels->width)
                break;
            std::error_code error;
            if (!fs::is_regular_file(path, error))
                continue;
            auto icon = read_application_icon(path);
            if (icon.width) {
                pixels->width = icon.width;
                pixels->height = icon.height;
                pixels->argb = std::move(icon.argb);
                break;
            }
#ifdef MM_APPKIT_PIXBUF
            GError *failure = nullptr;
            auto *p = gdk_pixbuf_new_from_file(path.c_str(), &failure);
            if (failure)
                g_error_free(failure);
            if (!p)
                continue;
            const int width = gdk_pixbuf_get_width(p), height = gdk_pixbuf_get_height(p),
                      stride = gdk_pixbuf_get_rowstride(p), channels = gdk_pixbuf_get_n_channels(p);
            if (width > 0 && height > 0 && width <= 8192 && height <= 8192 && gdk_pixbuf_get_bits_per_sample(p) == 8 &&
                (channels == 3 || channels == 4)) {
                pixels->width = width;
                pixels->height = height;
                pixels->argb.resize(std::size_t(width) * height);
                const auto *data = gdk_pixbuf_read_pixels(p);
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x) {
                        const auto *c = data + y * stride + x * channels;
                        unsigned a = channels == 4 ? c[3] : 255;
                        pixels->argb[std::size_t(y) * width + x] =
                            (a << 24) | ((c[0] * a / 255) << 16) | ((c[1] * a / 255) << 8) | (c[2] * a / 255);
                    }
            }
            g_object_unref(p);
            if (pixels->width)
                break;
#endif
        }
    }
    if (pixels->width) {
        if (o->size.w <= 0 || o->size.h <= 0)
            o->size = {double(pixels->width), double(pixels->height)};
        o->pixels = std::move(pixels);
    }
}
namespace {
void text_style(const control_object &control, evilcocoa::item &label, double scale) {
    label.font_size = control.font_size*std::abs(scale);
    label.font_family = control.font_family;
    label.bold = control.bold;
    if (!control.attributed_title || !send(control.attributed_title,"length")) return;
    id color = obj(send(control.attributed_title,"attribute:atIndex:effectiveRange:",ret(string("NSForegroundColorAttributeName")),0,0));
    if (!color) color = obj(send(control.attributed_title,"attribute:atIndex:effectiveRange:",ret(string("NSColor")),0,0));
    if (color && is_kind(color,cls("NSColor"))) {
        const auto *rgba = as<color_object>(color);
        label.text_color = {float(rgba->r),float(rgba->g),float(rgba->b),float(rgba->a)};
    }
}
evilcocoa::rect intersect(evilcocoa::rect a, evilcocoa::rect b) {
    const auto x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0., std::min(a.x + a.w, b.x + b.w) - x), std::max(0., std::min(a.y + a.h, b.y + b.h) - y)};
}
bool collect(id view, evilcocoa::rect frame, evilcocoa::rect clip, std::vector<evilcocoa::item> &out, int depth = 0) {
    if (!view || !is_kind(view, cls("NSView")) || depth > 128)
        return true;
    auto *v = as<view_object>(view);
    if (v->hidden)
        return true;
    const auto method = cocoa_method(mm_objc_object_class(view), mm_objc_sel("drawRect:"));
    if (v->layer)
        return false;
    const auto containing_clip = clip;
    clip = intersect(clip, frame);
    if (clip.w <= 0 || clip.h <= 0)
        return true;
    const bool flipped = send(view, "isFlipped") != 0;
    const double sx = v->bounds.w ? frame.w / v->bounds.w : 1, sy = v->bounds.h ? frame.h / v->bounds.h : 1;
    auto child_frame = [&](rect f) {
        return evilcocoa::rect{frame.x + (f.x - v->bounds.x) * sx,
                               frame.y + (flipped ? f.y - v->bounds.y : v->bounds.h - (f.y - v->bounds.y) - f.h) * sy,
                               f.w * sx, f.h * sy};
    };
    evilcocoa::item i;
    i.frame = frame;
    i.clip = clip;
    bool supported = false;
    if (is_kind(view, cls("NSControl"))) {
        auto *c = as<control_object>(view);
        supported = true;
        i.clip = containing_clip;
        i.text = c->title ? to_utf8(c->title) : "";
        i.placeholder = c->placeholder ? to_utf8(c->placeholder) : "";
        i.enabled = c->enabled;
        i.pressed = c->highlighted;
        i.checked = c->state != 0;
        i.selected = c->state == -1 ? 0 : -1;
        text_style(*c,i,sy);
        i.wrap = c->wraps;
        i.bordered = c->bordered;
        i.background = c->draws_background;
        i.align = c->alignment == 1   ? evilcocoa::alignment::center
                  : c->alignment == 2 ? evilcocoa::alignment::right
                                      : evilcocoa::alignment::left;
        i.focused = v->window && as<window_object>(v->window)->first_responder == view;
        i.value = c->maximum > c->minimum ? (c->value - c->minimum) / (c->maximum - c->minimum) : 0;
        i.knob = c->knob;
        i.vertical = frame.h > frame.w;
        if (c->image) {
            decode_cocoa_image(c->image);
            i.pixels = as<image_object>(c->image)->pixels;
        }
        i.scaling = c->image_scaling == 1   ? evilcocoa::image_scaling::stretch
                    : c->image_scaling == 2 ? evilcocoa::image_scaling::center
                                            : evilcocoa::image_scaling::fit;
        if (is_kind(view, cls("NSImageView")))
            i.type = evilcocoa::kind::image;
        else if (is_kind(view, cls("NSButton"))) {
            i.type = c->menu_row ? evilcocoa::kind::menu_item
                     : c->button_type == 3   ? evilcocoa::kind::checkbox
                     : c->button_type == 4 ? evilcocoa::kind::radio
                                           : evilcocoa::kind::button;
            i.help_button = c->bezel_style == 9;
            i.default_button = c->key_equivalent && to_utf8(c->key_equivalent) == "\r";
        } else if (is_kind(view, cls("NSPopUpButton"))) {
            i.type = evilcocoa::kind::popup;
            i.text = to_utf8(obj(send(view, "titleOfSelectedItem")));
            i.bordered = true;
        } else if (is_kind(view, cls("NSComboBox")))
            i.type = evilcocoa::kind::combo_box;
        else if (is_kind(view, cls("NSSlider")))
            i.type = evilcocoa::kind::slider;
        else if (is_kind(view, cls("NSProgressIndicator"))) {
            i.type = evilcocoa::kind::progress;
            i.indeterminate = c->indeterminate;
            if(c->indeterminate&&c->animating)i.value=std::fmod(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()/1.5,1.);
        } else if (is_kind(view, cls("NSScroller")))
            i.type = evilcocoa::kind::scroller;
        else if (is_kind(view, cls("NSBox"))) {
            i.type = c->box_type == 2 ? evilcocoa::kind::separator : evilcocoa::kind::box;
            if (c->title_position == 0) i.text.clear();
        }
        else if (is_kind(view, cls("NSTableView"))) {
            i.type = evilcocoa::kind::table;
            i.bordered = false;
        } else if (is_kind(view, cls("NSSegmentedControl"))) {
            i.type = evilcocoa::kind::segments;
            i.selected = c->selected;
            for (id choice : c->choices)
                i.segments.push_back(choice ? to_utf8(choice) : "");
        } else if (is_kind(view, cls("NSTextField"))) {
            i.type =
                c->editable || c->bezeled || c->draws_background ? evilcocoa::kind::text_field : evilcocoa::kind::label;
            i.bordered = c->bezeled || c->bordered;
            if (is_kind(view, cls("NSSecureTextField"))) {
                std::string masked;
                for (unsigned char byte : i.text)
                    if ((byte & 0xc0) != 0x80)
                        masked += "•";
                i.text = std::move(masked);
            }
        } else
            supported = false;
    } else if (is_kind(view, cls("NSScrollView"))) {
        i.type = evilcocoa::kind::scroll_view;
        supported = true;
    }
    if (supported)
        out.push_back(i);
    if (api() && api()->is_guest_code(reinterpret_cast<std::uintptr_t>(method)) && v->drawing) {
        evilcocoa::item drawing;
        drawing.type = evilcocoa::kind::image;
        drawing.frame = frame; drawing.clip = clip;
        drawing.scaling = evilcocoa::image_scaling::stretch;
        drawing.pixels = v->drawing;
        out.push_back(std::move(drawing));
    }
    if (is_kind(view, cls("NSTableView"))) {
        auto *table = as<table_object>(view);
        const double h = std::max(1., table->row_height);
        const auto first = std::max(std::int64_t(0), std::int64_t((clip.y - frame.y) / std::max(.001, h * sy)));
        const auto last = std::min(table->rows, first + std::int64_t(std::ceil(clip.h / std::max(.001, h * sy))) + 2);
        for (auto row = first; row < last; ++row) {
            evilcocoa::item background;
            background.type = evilcocoa::kind::table_row;
            background.frame = child_frame({0, double(row) * h, v->bounds.w, h});
            background.clip = clip;
            background.checked = table->selected_rows.contains(row);
            background.background = table->alternating && (row % 2);
            out.push_back(background);
            double x = 0;
            for (id column : table->columns) {
                auto *c = as<table_column_object>(column);
                id cell = table_cell_view(view, column, row);
                if (cell && is_kind(cell, cls("NSView"))) {
                    layout_cocoa_view(cell);
                    auto f = child_frame({x, double(row) * h, c->width, h});
                    if (!collect(cell, f, clip, out, depth + 1))
                        return false;
                } else {
                    id data_cell = prepare_table_data_cell(view,column,row);
                    auto label = background;
                    label.type = evilcocoa::kind::label;
                    label.frame = child_frame({x + 6, double(row) * h + 2, c->width - 12, h - 4});
                    label.text = to_utf8(obj(send(data_cell,"stringValue")));
                    if (is_kind(data_cell,cls("NSControl"))) {
                        auto *control = as<control_object>(data_cell);
                        text_style(*control,label,sy);
                    }
                    out.push_back(std::move(label));
                }
                x += c->width;
            }
        }
    }
    const auto children = v->subviews;
    for (id sub : children)
        if (!collect(sub, child_frame(as<view_object>(sub)->frame), clip, out, depth + 1))
            return false;
    return true;
}
} // namespace
void render_cocoa_window(id window) {
    auto *w = as<window_object>(window);
    layout_cocoa_view(w->content_view);
    std::vector<evilcocoa::item> items;
    if (collect(w->content_view, {0, 0, w->frame.w, w->frame.h}, {0, 0, w->frame.w, w->frame.h}, items))
        platform::present_cocoa(w->platform, items);
}
} // namespace ak

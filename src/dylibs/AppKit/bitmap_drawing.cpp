#include "appkit.h"
#include <algorithm>
#include <cmath>
#include <limits>

MM_FW_CLASS(NSImageRep);
MM_FW_CLASS(NSBitmapImageRep);
extern "C" void *bitmap_method(mm_objc_class *, mm_SEL) __asm__("_class_getMethodImplementation");

namespace ak {
namespace {
struct bitmap_rep {
    mm_objc_object base;
    std::int64_t width = 0, height = 0, samples = 0, stride = 0, bits = 0;
    std::uint64_t format = 0;
    bool alpha = false;
    size logical;
    std::vector<unsigned char> owned;
    unsigned char *data = nullptr;
};
thread_local id drawing_view = nullptr;
thread_local std::uint32_t fill_color = 0xff000000;

std::uint64_t bitmap_init_format(id self, SEL, unsigned char **planes, std::int64_t width,
    std::int64_t height, std::int64_t sample_bits, std::int64_t samples,
    std::uint64_t alpha, std::uint64_t planar, id, std::uint64_t format,
    std::int64_t stride, std::int64_t pixel_bits) {
    // Packed, 8-bit RGB/RGBA (and grayscale) are the formats used by Cocoa
    // raster clients. Reject other layouts instead of exposing a short buffer.
    const auto invalid = [&] { release(self); return std::uint64_t{0}; };
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
        sample_bits != 8 || planar || samples < 1 || samples > 4 ||
        (alpha && samples != 2 && samples != 4) || (format & ~std::uint64_t{3})) return invalid();
    if (!pixel_bits) pixel_bits = samples * 8;
    if (pixel_bits < samples * 8 || pixel_bits > 32 || pixel_bits % 8) return invalid();
    if (!stride) stride = width * (pixel_bits / 8);
    if (stride < width * (pixel_bits / 8) || stride > 256 * 1024 * 1024 / height) return invalid();
    auto *b = as<bitmap_rep>(self);
    b->width = width; b->height = height; b->samples = samples; b->stride = stride;
    b->bits = pixel_bits; b->alpha = alpha != 0; b->format = format;
    b->logical = {double(width), double(height)};
    b->owned.clear();
    if (planes && planes[0]) b->data = planes[0];
    else { b->owned.resize(std::size_t(stride) * height); b->data = b->owned.data(); }
    return ret(self);
}
std::uint64_t bitmap_init(id self, SEL sel, unsigned char **planes, std::int64_t width,
    std::int64_t height, std::int64_t sample_bits, std::int64_t samples,
    std::uint64_t alpha, std::uint64_t planar, id space, std::int64_t stride, std::int64_t bits) {
    return bitmap_init_format(self, sel, planes, width, height, sample_bits, samples,
                              alpha, planar, space, 0, stride, bits);
}
std::uint32_t pixel(const bitmap_rep &b, int x, int y) {
    const unsigned char *p = b.data + std::size_t(y) * b.stride + std::size_t(x) * (b.bits / 8);
    const unsigned a = b.alpha ? p[(b.format & 1) ? 0 : b.samples - 1] : 255;
    const unsigned offset = b.alpha && (b.format & 1) ? 1 : 0;
    unsigned r = p[offset], g = r, blue = r;
    if (b.samples >= 3) { g = p[offset + 1]; blue = p[offset + 2]; }
    if (b.alpha && (b.format & 2)) { r = (r * a + 127) / 255; g = (g * a + 127) / 255; blue = (blue * a + 127) / 255; }
    return (a << 24) | (r << 16) | (g << 8) | blue;
}
std::uint64_t bitmap_get_pixel(id self, SEL, std::uint64_t *out, std::int64_t x, std::int64_t y) {
    auto *b = as<bitmap_rep>(self);
    if (out && b->data && x >= 0 && y >= 0 && x < b->width && y < b->height) {
        const auto *p = b->data + y * b->stride + x * (b->bits / 8);
        for (int i = 0; i < b->samples; ++i) out[i] = p[i];
    }
    return 0;
}
std::uint64_t bitmap_set_pixel(id self, SEL, const std::uint64_t *in, std::int64_t x, std::int64_t y) {
    auto *b = as<bitmap_rep>(self);
    if (in && b->data && x >= 0 && y >= 0 && x < b->width && y < b->height) {
        auto *p = b->data + y * b->stride + x * (b->bits / 8);
        for (int i = 0; i < b->samples; ++i) p[i] = std::min<std::uint64_t>(255, in[i]);
    }
    return 0;
}
std::uint64_t image_add(id self, SEL, id rep) {
    if (!rep || !is_kind(rep, cls("NSImageRep"))) return 0;
    auto *im = as<image_object>(self);
    if (std::find(im->representations.begin(), im->representations.end(), rep) == im->representations.end())
        im->representations.push_back(retain(rep));
#ifdef MM_APPKIT_EVILCOCOA
    im->pixels.reset(); im->decoded = true;
#endif
    return 0;
}
std::uint64_t image_remove(id self, SEL, id rep) {
    auto &reps = as<image_object>(self)->representations;
    if (auto it = std::find(reps.begin(), reps.end(), rep); it != reps.end()) { release(*it); reps.erase(it); }
    return 0;
}
std::uint64_t image_reps(id self, SEL) {
    auto &reps = as<image_object>(self)->representations;
    return send(reinterpret_cast<id>(cls("NSArray")), "arrayWithObjects:count:", ret(reps.data()), reps.size());
}
bool valid_rect(rect r) { return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) && std::isfinite(r.h) && r.w > 0 && r.h > 0; }
#ifdef MM_APPKIT_EVILCOCOA
std::shared_ptr<evilcocoa::bitmap> canvas() {
    if (!drawing_view) return {};
    auto *view = as<view_object>(drawing_view);
    if (!view->drawing && valid_rect(view->bounds) && view->bounds.w <= 8192 && view->bounds.h <= 8192) {
        view->drawing = std::make_shared<evilcocoa::bitmap>();
        view->drawing->width = int(std::ceil(view->bounds.w));
        view->drawing->height = int(std::ceil(view->bounds.h));
        view->drawing->argb.resize(std::size_t(view->drawing->width) * view->drawing->height);
    }
    return view->drawing;
}
std::uint32_t composite(std::uint32_t src, std::uint32_t dst, unsigned operation, double fraction) {
    if (operation == 0) return 0; // NSCompositingOperationClear
    const unsigned f = unsigned(std::clamp(fraction, 0., 1.) * 255 + .5);
    const unsigned a = ((src >> 24) * f + 127) / 255;
    const unsigned inv = operation == 1 ? 0 : 255 - a; // Copy or SourceOver
    std::uint32_t result = 0;
    for (unsigned shift : {0u, 8u, 16u, 24u}) {
        const unsigned value = (((src >> shift) & 255) * f + ((dst >> shift) & 255) * inv + 127) / 255;
        result |= std::min(255u, value) << shift;
    }
    return result;
}
template<class Fetch> void raster(rect dest, unsigned operation, double fraction, Fetch fetch) {
    if (!valid_rect(dest) || !std::isfinite(fraction)) return;
    auto c = canvas(); if (!c) return;
    const rect bounds = as<view_object>(drawing_view)->bounds;
    const bool flipped = send(drawing_view, "isFlipped");
    const double left = dest.x - bounds.x, top = flipped ? dest.y - bounds.y : bounds.h - (dest.y - bounds.y) - dest.h;
    const int x0 = int(std::clamp(std::floor(left), 0., double(c->width)));
    const int x1 = int(std::clamp(std::ceil(left + dest.w), 0., double(c->width)));
    const int y0 = int(std::clamp(std::floor(top), 0., double(c->height)));
    const int y1 = int(std::clamp(std::ceil(top + dest.h), 0., double(c->height)));
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
        auto &dst = c->argb[std::size_t(y) * c->width + x];
        dst = composite(fetch((x + .5 - left) / dest.w, (y + .5 - top) / dest.h), dst, operation, fraction);
    }
}
#endif
void draw_image(id self, rect dest, rect source, unsigned op, double fraction) {
#ifdef MM_APPKIT_EVILCOCOA
    auto *im = as<image_object>(self);
    bitmap_rep *rep = nullptr;
    for (id r : im->representations) if (is_kind(r, cls("NSBitmapImageRep")) && as<bitmap_rep>(r)->data) { rep = as<bitmap_rep>(r); break; }
    if (!rep) decode_cocoa_image(self);
    if (!rep && !im->pixels) return;
    const int width = rep ? rep->width : im->pixels->width, height = rep ? rep->height : im->pixels->height;
    const double w = im->size.w > 0 ? im->size.w : width, h = im->size.h > 0 ? im->size.h : height;
    if (source.w == 0 && source.h == 0) source = {0, 0, w, h};
    if (!valid_rect(source)) return;
    raster(dest, op, fraction, [&](double u, double v) {
        const double sx = (source.x + u * source.w) * width / w;
        const double sy = (h - source.y - source.h + v * source.h) * height / h;
        if (sx < 0 || sy < 0 || sx >= width || sy >= height) return std::uint32_t{0};
        return rep ? pixel(*rep, int(sx), int(sy)) : im->pixels->argb[std::size_t(int(sy)) * width + int(sx)];
    });
#endif
}
std::uint64_t image_draw_rect(id self, SEL, std::uint64_t op, double x, double y, double w, double h,
                             double sx, double sy, double sw, double sh) {
    // The ninth FP argument is spilled at guest SP, while the host bridge
    // inserts x6/x7 before stack arguments. Read the original ABI slot.
    double fraction = 1;
    if (auto *ctx = api()->current_context()) std::memcpy(&fraction, reinterpret_cast<void *>(ctx->x[31]), 8);
    draw_image(self, {x,y,w,h}, {sx,sy,sw,sh}, op, fraction); return 0;
}
std::uint64_t image_draw_at(id self, SEL, std::uint64_t op, double x, double y,
                           double sx, double sy, double sw, double sh, double fraction) {
    auto *im = as<image_object>(self);
    draw_image(self, {x,y,sw ? sw : im->size.w,sh ? sh : im->size.h}, {sx,sy,sw,sh}, op, fraction); return 0;
}
}

id begin_bitmap_drawing(id view) {
    id previous = drawing_view;
    drawing_view = view;
#ifdef MM_APPKIT_EVILCOCOA
    const auto method = bitmap_method(mm_objc_object_class(view), mm_objc_sel("drawRect:"));
    // A snapshot per dirty draw lets presentation detect changes without
    // hashing pixels every frame. Ordinary controls never allocate a canvas.
    if (api() && api()->is_guest_code(reinterpret_cast<std::uintptr_t>(method)))
        as<view_object>(view)->drawing.reset();
#endif
    return previous;
}
void end_bitmap_drawing(id previous) { drawing_view = previous; }
void set_bitmap_fill_color(const color_object &c) {
    auto byte = [](double v) { return unsigned(std::clamp(v, 0., 1.) * 255 + .5); };
    fill_color = (byte(c.a) << 24) | (byte(c.r * c.a) << 16) | (byte(c.g * c.a) << 8) | byte(c.b * c.a);
}
void fill_bitmap_rect(rect r) {
#ifdef MM_APPKIT_EVILCOCOA
    raster(r, 2, 1., [](double, double) { return fill_color; });
#endif
}
void register_bitmap_drawing() {
    Class base = mm_objc_define_class("NSImageRep", cls("NSObject"), sizeof(bitmap_rep), MM_FW_CLASS_REF(NSImageRep), MM_FW_META_REF(NSImageRep));
    register_lifecycle<bitmap_rep>(base);
    Class bitmap = mm_objc_define_class("NSBitmapImageRep", base, sizeof(bitmap_rep), MM_FW_CLASS_REF(NSBitmapImageRep), MM_FW_META_REF(NSBitmapImageRep));
    register_lifecycle<bitmap_rep>(bitmap);
    static auto data = [](id s, SEL) { return ret(as<bitmap_rep>(s)->data); };
    static auto planes = [](id s, SEL, unsigned char **out) -> std::uint64_t { if (out) { out[0] = as<bitmap_rep>(s)->data; for (int i=1;i<5;++i) out[i]=nullptr; } return 0; };
    static auto size_get = [](id s, SEL) -> mm_two_doubles { auto sz=as<bitmap_rep>(s)->logical; return {sz.w,sz.h}; };
    static auto size_set = [](id s, SEL, double w, double h) -> std::uint64_t { as<bitmap_rep>(s)->logical={w,h}; return 0; };
    MM_METHODS(bitmap,
        {"initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bytesPerRow:bitsPerPixel:", MM_IMP(bitmap_init), "@96@0:8^^C16q24q32q40q48B56B60@64q72q80"},
        {"initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bitmapFormat:bytesPerRow:bitsPerPixel:", MM_IMP(bitmap_init_format), "@104@0:8^^C16q24q32q40q48B56B60@64Q72q80q88"},
        {"bitmapData", MM_IMP(+data), "^C16@0:8"}, {"getBitmapDataPlanes:", MM_IMP(+planes), "v24@0:8^^C16"},
        {"getPixel:atX:y:", MM_IMP(bitmap_get_pixel), "v40@0:8^Q16q24q32"},
        {"setPixel:atX:y:", MM_IMP(bitmap_set_pixel), "v40@0:8^Q16q24q32"},
        {"size", MM_IMP(+size_get), "{CGSize=dd}16@0:8"}, {"setSize:", MM_IMP(+size_set), "v32@0:8{CGSize=dd}16"});
    struct property { const char *name; std::int64_t bitmap_rep::*member; };
    // Separate functions keep the hot bitmapData path a plain native getter.
    static auto width=[](id s,SEL){return as<bitmap_rep>(s)->width;};
    static auto height=[](id s,SEL){return as<bitmap_rep>(s)->height;};
    static auto stride=[](id s,SEL){return as<bitmap_rep>(s)->stride;};
    static auto samples=[](id s,SEL){return as<bitmap_rep>(s)->samples;};
    static auto bits=[](id s,SEL){return as<bitmap_rep>(s)->bits;};
    static auto sample_bits=[](id,SEL){return 8ull;};
    static auto alpha=[](id s,SEL){return std::uint64_t(as<bitmap_rep>(s)->alpha);};
    static auto planar=[](id,SEL){return 0ull;};
    static auto format=[](id s,SEL){return as<bitmap_rep>(s)->format;};
    static auto plane_size=[](id s,SEL){auto *b=as<bitmap_rep>(s);return b->stride*b->height;};
    MM_METHODS(bitmap,
        {"pixelsWide",MM_IMP(+width),"q16@0:8"}, {"pixelsHigh",MM_IMP(+height),"q16@0:8"},
        {"bytesPerRow",MM_IMP(+stride),"q16@0:8"}, {"samplesPerPixel",MM_IMP(+samples),"q16@0:8"},
        {"bitsPerPixel",MM_IMP(+bits),"q16@0:8"}, {"bitsPerSample",MM_IMP(+sample_bits),"q16@0:8"},
        {"hasAlpha",MM_IMP(+alpha),"B16@0:8"}, {"isPlanar",MM_IMP(+planar),"B16@0:8"},
        {"bitmapFormat",MM_IMP(+format),"Q16@0:8"}, {"bytesPerPlane",MM_IMP(+plane_size),"q16@0:8"});
    MM_METHODS(cls("NSImage"),
        {"addRepresentation:",MM_IMP(image_add),"v24@0:8@16"}, {"removeRepresentation:",MM_IMP(image_remove),"v24@0:8@16"},
        {"representations",MM_IMP(image_reps),"@16@0:8"},
        {"drawInRect:fromRect:operation:fraction:",MM_IMP(image_draw_rect),"v96@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16{CGRect={CGPoint=dd}{CGSize=dd}}48Q80d88"},
        {"drawAtPoint:fromRect:operation:fraction:",MM_IMP(image_draw_at),"v80@0:8{CGPoint=dd}16{CGRect={CGPoint=dd}{CGSize=dd}}32Q64d72"});
}
} // namespace ak

extern "C" MM_FW_EXPORT void mm_NSRectFill(double, double, double, double) __asm__("_NSRectFill");
extern "C" void mm_NSRectFill(double x, double y, double w, double h) { ak::fill_bitmap_rect({x,y,w,h}); }

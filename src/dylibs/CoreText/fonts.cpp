/* Font lookup and descriptor attributes. Live uses the URL attribute to
 * open font data in its own renderer, so it must name a real font file. */
#include "machoman/objc_framework.h"
#include "synthesise.h"
#include <fontconfig/fontconfig.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

#define DARWIN(name) __attribute__((visibility("default"))) __asm__("_" #name)
using namespace mmfw;

extern "C" {
id cf_string(void *, const char *, std::uint32_t) __asm__("_CFStringCreateWithCString");
id cf_number(void *, std::int64_t, const void *) __asm__("_CFNumberCreate");
id cf_data(void *, const std::uint8_t *, std::int64_t) __asm__("_CFDataCreate");
#define FONT_KEY(name) __attribute__((visibility("default"))) id name __asm__("_" #name) = nullptr
FONT_KEY(kCTFontURLAttribute);
FONT_KEY(kCTFontNameAttribute);
FONT_KEY(kCTFontFamilyNameAttribute);
FONT_KEY(kCTFontStyleNameAttribute);
FONT_KEY(kCTFontDisplayNameAttribute);
FONT_KEY(kCTFontSizeAttribute);
FONT_KEY(kCTFontMatrixAttribute);
FONT_KEY(kCTFontTraitsAttribute);
FONT_KEY(kCTFontSymbolicTrait);
FONT_KEY(kCTFontWeightTrait);
FONT_KEY(kCTFontWidthTrait);
FONT_KEY(kCTFontSlantTrait);
}

namespace {
mm_objc_class font_storage{}, font_meta{};
Class font_class;
struct matrix { double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0; };
struct font_object {
    mm_objc_object base;
    std::string name, family, style, display, path;
    double size = 12;
    matrix transform;
    int weight = FC_WEIGHT_REGULAR, width = FC_WIDTH_NORMAL, slant = FC_SLANT_ROMAN;
};
id string_copy(const std::string &value) { return cf_string(nullptr, value.c_str(), 0x08000100); }
std::string text(id value) {
    const char *bytes = reinterpret_cast<const char *>(send(value, "UTF8String"));
    return bytes ? bytes : "";
}
id number_copy(double value) { return cf_number(nullptr, 13 /* double */, &value); }
void initialize() {
    static std::once_flag once;
    std::call_once(once, [] {
        font_class = mm_objc_define_class("__MMCTFont", cls("NSObject"), sizeof(font_object), &font_storage, &font_meta);
        register_lifecycle<font_object>(font_class);
        kCTFontURLAttribute = string_copy("NSFontURLAttribute");
        kCTFontNameAttribute = string_copy("NSFontNameAttribute");
        kCTFontFamilyNameAttribute = string_copy("NSFontFamilyAttribute");
        kCTFontStyleNameAttribute = string_copy("NSFontStyleAttribute");
        kCTFontDisplayNameAttribute = string_copy("NSFontVisibleNameAttribute");
        kCTFontSizeAttribute = string_copy("NSFontSizeAttribute");
        kCTFontMatrixAttribute = string_copy("NSFontMatrixAttribute");
        kCTFontTraitsAttribute = string_copy("NSFontTraitsAttribute");
        kCTFontSymbolicTrait = string_copy("CTFontSymbolicTrait");
        kCTFontWeightTrait = string_copy("CTFontWeightTrait");
        kCTFontWidthTrait = string_copy("CTFontWidthTrait");
        kCTFontSlantTrait = string_copy("CTFontSlantTrait");
    });
}
font_object *font(id value) {
    initialize();
    return value && is_kind(value, font_class) ? as<font_object>(value) : nullptr;
}
std::string pattern_text(FcPattern *pattern, const char *key) {
    FcChar8 *value = nullptr;
    return FcPatternGetString(pattern, key, 0, &value) == FcResultMatch
        ? reinterpret_cast<const char *>(value) : "";
}
id traits_copy(const font_object &f) {
    id result = retain(obj(send(reinterpret_cast<id>(cls("NSMutableDictionary")), "dictionary")));
    const auto add = [&](id key, double value) {
        id number = number_copy(value);
        send(result, "setObject:forKey:", ret(number), ret(key));
        release(number);
    };
    unsigned symbolic = 0;
    if (f.slant != FC_SLANT_ROMAN) symbolic |= 1; /* italic */
    if (f.weight >= FC_WEIGHT_DEMIBOLD) symbolic |= 2; /* bold */
    if (f.width < FC_WIDTH_NORMAL) symbolic |= 1u << 6; /* condensed */
    if (f.width > FC_WIDTH_NORMAL) symbolic |= 1u << 5; /* expanded */
    add(kCTFontSymbolicTrait, symbolic);
    add(kCTFontWeightTrait, std::clamp((f.weight - FC_WEIGHT_REGULAR) / 120.0, -1.0, 1.0));
    add(kCTFontWidthTrait, std::clamp((f.width - FC_WIDTH_NORMAL) / 100.0, -1.0, 1.0));
    add(kCTFontSlantTrait, f.slant == FC_SLANT_ROMAN ? 0 : 0.2);
    return result;
}
}

extern "C" {
id CTFontCreateWithName_(id name, double size, const matrix *transform) DARWIN(CTFontCreateWithName);
id CTFontCreateWithName_(id name, double size, const matrix *transform) {
    initialize();
    const std::string requested = text(name);
    if (requested.empty() || !std::isfinite(size) || size < 0) return nullptr;
    std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> pattern(FcPatternCreate(), FcPatternDestroy);
    if (!pattern) return nullptr;
    FcPatternAddString(pattern.get(), FC_FAMILY, reinterpret_cast<const FcChar8 *>(requested.c_str()));
    FcPatternAddString(pattern.get(), FC_POSTSCRIPT_NAME, reinterpret_cast<const FcChar8 *>(requested.c_str()));
    FcPatternAddDouble(pattern.get(), FC_SIZE, size ? size : 12);
    FcConfigSubstitute(nullptr, pattern.get(), FcMatchPattern);
    FcDefaultSubstitute(pattern.get());
    FcResult status;
    std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> match(FcFontMatch(nullptr, pattern.get(), &status), FcPatternDestroy);
    if (!match || pattern_text(match.get(), FC_FILE).empty()) return nullptr;
    auto *f = make<font_object>(font_class);
    f->name = pattern_text(match.get(), FC_POSTSCRIPT_NAME);
    f->family = pattern_text(match.get(), FC_FAMILY);
    f->style = pattern_text(match.get(), FC_STYLE);
    f->display = pattern_text(match.get(), FC_FULLNAME);
    f->path = pattern_text(match.get(), FC_FILE);
    if (f->name.empty()) f->name = f->family;
    if (f->display.empty()) f->display = f->family;
    f->size = size ? size : 12;
    if (transform) f->transform = *transform;
    FcPatternGetInteger(match.get(), FC_WEIGHT, 0, &f->weight);
    FcPatternGetInteger(match.get(), FC_WIDTH, 0, &f->width);
    FcPatternGetInteger(match.get(), FC_SLANT, 0, &f->slant);
    if (std::getenv("MACHOMAN_APPKIT_TRACE"))
        std::fprintf(stderr, "CoreText: font \"%s\" -> \"%s\" size=%g file=%s\n", requested.c_str(), f->name.c_str(), f->size, f->path.c_str());
    return reinterpret_cast<id>(f);
}
id CTFontCopyAttribute_(id value, id attribute) DARWIN(CTFontCopyAttribute);
id CTFontCopyAttribute_(id value, id attribute) {
    const auto *f = font(value);
    if (!f || !attribute) return nullptr;
    const std::string key = text(attribute);
    if (std::getenv("MACHOMAN_APPKIT_TRACE")) std::fprintf(stderr, "CoreText: font attribute %s\n", key.c_str());
    if (key == "NSFontNameAttribute") return string_copy(f->name);
    if (key == "NSFontFamilyAttribute") return string_copy(f->family);
    if (key == "NSFontStyleAttribute") return string_copy(f->style);
    if (key == "NSFontVisibleNameAttribute") return string_copy(f->display);
    if (key == "NSFontSizeAttribute") return number_copy(f->size);
    if (key == "NSFontMatrixAttribute") return cf_data(nullptr, reinterpret_cast<const std::uint8_t *>(&f->transform), sizeof(matrix));
    if (key == "NSFontTraitsAttribute") return traits_copy(*f);
    if (key == "NSFontURLAttribute") {
        id path = string_copy(f->path);
        id url = retain(obj(send(reinterpret_cast<id>(cls("NSURL")), "fileURLWithPath:", ret(path))));
        release(path);
        return url;
    }
    return nullptr;
}
std::uint64_t CTFontGetTypeID_() DARWIN(CTFontGetTypeID);
std::uint64_t CTFontGetTypeID_() { initialize(); return ret(font_class); }
double CTFontGetSize_(id value) DARWIN(CTFontGetSize);
double CTFontGetSize_(id value) { const auto *f = font(value); return f ? f->size : 0; }
__attribute__((visibility("default"))) void *machoman_resolve_symbol(const char *name, unsigned flags) {
    return mm_synthesise_symbol(name, flags);
}
__attribute__((constructor)) static void coretext_init() { initialize(); }
}

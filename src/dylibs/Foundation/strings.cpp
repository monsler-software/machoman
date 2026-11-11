/* NSString / NSMutableString / CFString.
 *
 * Storage is UTF-16 (CFString's native unit, so length and indices match
 * Apple's). Three representations share one method table:
 *   __NSCFConstantString  the guest's CFSTR()/@"" literals: Apple's fixed
 *                         layout {isa, flags, bytes, length}, 8-bit or
 *                         UTF-16 depending on bit 4 of flags
 *   __NSCFString          host-created strings, mutable or not
 *   guest NSString subclasses, reached through -length/-getCharacters:range:
 * Every method reads characters through string_chars(). */
#include "foundation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace fnd {

/* Class storage the guest binds to: external linkage, so not inside the
 * anonymous namespace below. */
FND_CLASS(NSString);
FND_CLASS(NSMutableString);
FND_CLASS(__NSCFString);

namespace {

struct const_string_layout { mm_objc_object base; std::uint64_t flags; const void *bytes; std::int64_t length; };

struct string_object {
    mm_objc_object base;
    std::u16string chars;
    std::string utf8;       /* cache for -UTF8String / CFStringGetCStringPtr */
    bool utf8_valid = false;
    bool mutable_ = false;
    std::uint64_t hash_cache = 0;
    bool hash_valid = false;
    void touch() { utf8_valid = false; hash_valid = false; }
};

/* The guest's __cfstring literals store the *address* of this symbol in
 * their isa field, so it must be the class object itself, not a pointer. */
extern "C" EXPORT mm_objc_class cf_constant_string_class __asm__("___CFConstantStringClassReference");
mm_objc_class cf_constant_string_class;
/* Only the metaclass storage is needed here; the class itself is the
 * exported ___CFConstantStringClassReference symbol above. */
__attribute__((visibility("hidden"))) mm_objc_class cf_constant_string_metaclass;

Class g_string, g_mutable, g_cf, g_constant;

/* Constant 8-bit strings are decoded once and cached by address. */
std::mutex g_const_mu;
std::unordered_map<const void *, std::u16string> g_const_cache;

bool is_constant(id s) { return s->isa == g_constant; }
bool is_host(id s) { return s->isa == g_cf || (is_kind(s, g_cf) && !is_constant(s)); }

} // namespace

/* ---- UTF conversions ---------------------------------------------------- */

std::u16string to_utf16(std::string_view in) {
    std::u16string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        std::uint32_t cp;
        std::size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else { out.push_back(0xFFFD); i++; continue; }
        if (i + n > in.size()) { out.push_back(0xFFFD); break; }
        bool ok = true;
        for (std::size_t k = 1; k < n; k++) {
            const unsigned char cc = static_cast<unsigned char>(in[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); i++; continue; }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string utf16_to_utf8(std::u16string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); i++) {
        std::uint32_t cp = in[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < in.size() && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else { out.push_back(static_cast<char>(0xF0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    }
    return out;
}

/* ---- encodings ---------------------------------------------------------- */

namespace {
constexpr std::uint32_t kCFStringEncodingMacRoman = 0, kCFStringEncodingWindowsLatin1 = 0x500, kCFStringEncodingISOLatin1 = 0x201,
                        kCFStringEncodingASCII = 0x600, kCFStringEncodingUnicode = 0x100, kCFStringEncodingUTF8 = 0x8000100,
                        kCFStringEncodingNonLossyASCII = 0xBFF, kCFStringEncodingUTF16BE = 0x10000100, kCFStringEncodingUTF16LE = 0x14000100,
                        kCFStringEncodingUTF32 = 0x0c000100, kCFStringEncodingUTF32BE = 0x18000100, kCFStringEncodingUTF32LE = 0x1c000100;

/* MacRoman 0x80-0xFF -> Unicode. */
const char16_t mac_roman[128] = {
    0x00C4,0x00C5,0x00C7,0x00C9,0x00D1,0x00D6,0x00DC,0x00E1,0x00E0,0x00E2,0x00E4,0x00E3,0x00E5,0x00E7,0x00E9,0x00E8,
    0x00EA,0x00EB,0x00ED,0x00EC,0x00EE,0x00EF,0x00F1,0x00F3,0x00F2,0x00F4,0x00F6,0x00F5,0x00FA,0x00F9,0x00FB,0x00FC,
    0x2020,0x00B0,0x00A2,0x00A3,0x00A7,0x2022,0x00B6,0x00DF,0x00AE,0x00A9,0x2122,0x00B4,0x00A8,0x2260,0x00C6,0x00D8,
    0x221E,0x00B1,0x2264,0x2265,0x00A5,0x00B5,0x2202,0x2211,0x220F,0x03C0,0x222B,0x00AA,0x00BA,0x03A9,0x00E6,0x00F8,
    0x00BF,0x00A1,0x00AC,0x221A,0x0192,0x2248,0x2206,0x00AB,0x00BB,0x2026,0x00A0,0x00C0,0x00C3,0x00D5,0x0152,0x0153,
    0x2013,0x2014,0x201C,0x201D,0x2018,0x2019,0x00F7,0x25CA,0x00FF,0x0178,0x2044,0x20AC,0x2039,0x203A,0xFB01,0xFB02,
    0x2021,0x00B7,0x201A,0x201E,0x2030,0x00C2,0x00CA,0x00C1,0x00CB,0x00C8,0x00CD,0x00CE,0x00CF,0x00CC,0x00D3,0x00D4,
    0xF8FF,0x00D2,0x00DA,0x00DB,0x00D9,0x0131,0x02C6,0x02DC,0x00AF,0x02D8,0x02D9,0x02DA,0x00B8,0x02DD,0x02DB,0x02C7,
};
} // namespace

std::uint32_t ns_to_cf_encoding(std::uint64_t ns) {
    switch (ns) {
    case 1:  return kCFStringEncodingASCII;
    case 4:  return kCFStringEncodingUTF8;
    case 5:  return kCFStringEncodingISOLatin1;
    case 7:  return kCFStringEncodingNonLossyASCII;
    case 10: return kCFStringEncodingUnicode;
    case 12: return kCFStringEncodingWindowsLatin1;
    case 30: return kCFStringEncodingMacRoman;
    case 0x90000100: return kCFStringEncodingUTF16BE;
    case 0x94000100: return kCFStringEncodingUTF16LE;
    case 0x8c000100: return kCFStringEncodingUTF32;
    case 0x98000100: return kCFStringEncodingUTF32BE;
    case 0x9c000100: return kCFStringEncodingUTF32LE;
    default: return static_cast<std::uint32_t>(ns);
    }
}

bool encode(std::u16string_view s, std::uint32_t enc, std::string &out, bool lossy) {
    out.clear();
    switch (enc) {
    case kCFStringEncodingUTF8:
        out = utf16_to_utf8(s);
        return true;
    case kCFStringEncodingUnicode: case kCFStringEncodingUTF16LE:
        out.resize(s.size() * 2);
        std::memcpy(out.data(), s.data(), s.size() * 2);
        return true;
    case kCFStringEncodingUTF16BE:
        for (char16_t c : s) { out.push_back(static_cast<char>(c >> 8)); out.push_back(static_cast<char>(c & 0xFF)); }
        return true;
    case kCFStringEncodingASCII: case kCFStringEncodingISOLatin1: case kCFStringEncodingWindowsLatin1:
    case kCFStringEncodingMacRoman: case kCFStringEncodingNonLossyASCII: {
        const std::uint32_t limit = enc == kCFStringEncodingASCII || enc == kCFStringEncodingNonLossyASCII ? 0x80 : 0x100;
        for (std::size_t i = 0; i < s.size(); i++) {
            std::uint32_t c = s[i];
            if (enc == kCFStringEncodingMacRoman && c >= 0x80) {
                bool found = false;
                for (int k = 0; k < 128; k++) if (mac_roman[k] == c) { out.push_back(static_cast<char>(0x80 + k)); found = true; break; }
                if (found) continue;
                if (!lossy) return false;
                out.push_back('?');
                continue;
            }
            if (c >= limit) {
                if (!lossy) return false;
                out.push_back('?');
                continue;
            }
            out.push_back(static_cast<char>(c));
        }
        return true;
    }
    case kCFStringEncodingUTF32: case kCFStringEncodingUTF32LE: case kCFStringEncodingUTF32BE: {
        for (std::size_t i = 0; i < s.size(); i++) {
            std::uint32_t cp = s[i];
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size()) { cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00); i++; }
            if (enc == kCFStringEncodingUTF32BE) for (int k = 3; k >= 0; k--) out.push_back(static_cast<char>((cp >> (k * 8)) & 0xFF));
            else for (int k = 0; k < 4; k++) out.push_back(static_cast<char>((cp >> (k * 8)) & 0xFF));
        }
        return true;
    }
    default:
        return false;
    }
}

bool decode(std::string_view b, std::uint32_t enc, std::u16string &out) {
    out.clear();
    switch (enc) {
    case kCFStringEncodingUTF8:
        out = to_utf16(b);
        return true;
    case kCFStringEncodingUnicode: case kCFStringEncodingUTF16LE: case kCFStringEncodingUTF16BE: {
        bool be = enc == kCFStringEncodingUTF16BE;
        std::size_t i = 0;
        if (enc == kCFStringEncodingUnicode && b.size() >= 2) {
            const unsigned char b0 = static_cast<unsigned char>(b[0]), b1 = static_cast<unsigned char>(b[1]);
            if (b0 == 0xFE && b1 == 0xFF) { be = true; i = 2; }
            else if (b0 == 0xFF && b1 == 0xFE) { be = false; i = 2; }
        }
        for (; i + 1 < b.size(); i += 2) {
            const unsigned char x = static_cast<unsigned char>(b[i]), y = static_cast<unsigned char>(b[i + 1]);
            out.push_back(static_cast<char16_t>(be ? (x << 8) | y : (y << 8) | x));
        }
        return true;
    }
    case kCFStringEncodingASCII: case kCFStringEncodingNonLossyASCII:
        for (char c : b) { if (static_cast<unsigned char>(c) >= 0x80) return false; out.push_back(static_cast<char16_t>(c)); }
        return true;
    case kCFStringEncodingISOLatin1: case kCFStringEncodingWindowsLatin1:
        for (char c : b) out.push_back(static_cast<char16_t>(static_cast<unsigned char>(c)));
        return true;
    case kCFStringEncodingMacRoman:
        for (char c : b) { const unsigned char u = static_cast<unsigned char>(c); out.push_back(u < 0x80 ? static_cast<char16_t>(u) : mac_roman[u - 0x80]); }
        return true;
    case kCFStringEncodingUTF32: case kCFStringEncodingUTF32LE: case kCFStringEncodingUTF32BE:
        for (std::size_t i = 0; i + 3 < b.size(); i += 4) {
            std::uint32_t cp = 0;
            for (int k = 0; k < 4; k++) {
                const std::uint32_t byte = static_cast<unsigned char>(b[i + static_cast<std::size_t>(k)]);
                cp |= enc == kCFStringEncodingUTF32BE ? byte << ((3 - k) * 8) : byte << (k * 8);
            }
            if (cp >= 0x10000) { cp -= 0x10000; out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10))); out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF))); }
            else out.push_back(static_cast<char16_t>(cp));
        }
        return true;
    default:
        return false;
    }
}

/* ---- access ------------------------------------------------------------- */

chars string_chars(id s) {
    if (!s) return {nullptr, 0};
    if (is_constant(s)) {
        auto *c = as<const_string_layout>(s);
        if (c->flags & 0x10) return {static_cast<const UniChar *>(c->bytes), c->length};
        std::lock_guard lock(g_const_mu);
        auto it = g_const_cache.find(c->bytes);
        if (it == g_const_cache.end()) {
            std::u16string u;
            for (std::int64_t i = 0; i < c->length; i++) u.push_back(static_cast<char16_t>(static_cast<const unsigned char *>(c->bytes)[i]));
            it = g_const_cache.emplace(c->bytes, std::move(u)).first;
        }
        return {it->second.data(), static_cast<CFIndex>(it->second.size())};
    }
    if (is_host(s)) {
        auto *o = as<string_object>(s);
        return {o->chars.data(), static_cast<CFIndex>(o->chars.size())};
    }
    /* A guest subclass: go through its primitive methods into a per-thread buffer. */
    static thread_local std::u16string buf;
    const CFIndex n = static_cast<CFIndex>(send(s, "length"));
    buf.assign(static_cast<std::size_t>(n), u'\0');
    if (n) send(s, "getCharacters:range:", ret(buf.data()), 0, static_cast<std::uint64_t>(n));
    return {buf.data(), n};
}

std::string to_utf8(id s) {
    if (!s) return {};
    if (is_host(s)) {
        auto *o = as<string_object>(s);
        if (!o->utf8_valid) { o->utf8 = utf16_to_utf8(o->chars); o->utf8_valid = true; }
        return o->utf8;
    }
    const chars c = string_chars(s);
    return utf16_to_utf8(std::u16string_view(c.data, static_cast<std::size_t>(c.length)));
}

/* Stable UTF-8 pointer (valid until the string is mutated or freed). */
static const char *utf8_ptr(id s) {
    if (is_constant(s)) {
        auto *c = as<const_string_layout>(s);
        if (!(c->flags & 0x10)) return static_cast<const char *>(c->bytes);
    }
    if (is_host(s)) {
        auto *o = as<string_object>(s);
        if (!o->utf8_valid) { o->utf8 = utf16_to_utf8(o->chars); o->utf8_valid = true; }
        return o->utf8.c_str();
    }
    /* Guest subclass: cache on the autorelease-ish thread buffer. */
    static thread_local std::string tmp;
    tmp = to_utf8(s);
    return tmp.c_str();
}

id string_from_utf16(const UniChar *p, CFIndex n) {
    auto *o = make<string_object>(g_cf);
    o->chars.assign(p, static_cast<std::size_t>(n));
    return reinterpret_cast<id>(o);
}
id string_from_utf16(std::u16string_view s) { return string_from_utf16(s.data(), static_cast<CFIndex>(s.size())); }
id string_from_utf8(std::string_view s) { return string_from_utf16(to_utf16(s)); }

id constant_string(const char *utf8) {
    static std::mutex mu;
    static std::unordered_map<std::string, id> table;
    std::lock_guard lock(mu);
    auto it = table.find(utf8);
    if (it != table.end()) return it->second;
    id s = string_from_utf8(utf8);
    table.emplace(utf8, s);
    return s;
}

bool is_string(id o) { return o && is_kind(o, g_string); }
bool is_mutable_string(id o) { return o && is_host(o) && as<string_object>(o)->mutable_; }

namespace {

std::u16string_view view(id s) { const chars c = string_chars(s); return {c.data, static_cast<std::size_t>(c.length)}; }

string_object *mutable_of(id s, const char *what) {
    if (!is_mutable_string(s)) {
        std::fprintf(stderr, "Foundation: %s sent to an immutable string\n", what);
        std::abort();
    }
    auto *o = as<string_object>(s);
    o->touch();
    return o;
}

/* ---- formatting (%@ and friends), arguments from the guest's stack ---- */

struct guest_args {
    std::uint64_t p;
    std::uint64_t u64() { std::uint64_t v; std::memcpy(&v, reinterpret_cast<const void *>(p), 8); p += 8; return v; }
    double f64() { std::uint64_t v = u64(); double d; std::memcpy(&d, &v, 8); return d; }
};

std::u16string format_guest(std::u16string_view fmt, guest_args &a) {
    std::u16string out;
    for (std::size_t i = 0; i < fmt.size();) {
        if (fmt[i] != u'%') { out.push_back(fmt[i++]); continue; }
        if (i + 1 < fmt.size() && fmt[i + 1] == u'%') { out.push_back(u'%'); i += 2; continue; }
        std::string spec = "%";
        i++;
        /* positional n$ */
        {
            std::size_t j = i;
            while (j < fmt.size() && fmt[j] >= u'0' && fmt[j] <= u'9') j++;
            if (j > i && j < fmt.size() && fmt[j] == u'$') i = j + 1;
        }
        while (i < fmt.size() && (fmt[i] == u'-' || fmt[i] == u'+' || fmt[i] == u' ' || fmt[i] == u'#' || fmt[i] == u'0' || fmt[i] == u'\'')) spec += static_cast<char>(fmt[i++]);
        if (i < fmt.size() && fmt[i] == u'*') { spec += std::to_string(static_cast<int>(a.u64())); i++; }
        else while (i < fmt.size() && fmt[i] >= u'0' && fmt[i] <= u'9') spec += static_cast<char>(fmt[i++]);
        if (i < fmt.size() && fmt[i] == u'.') {
            spec += '.'; i++;
            if (i < fmt.size() && fmt[i] == u'*') { spec += std::to_string(static_cast<int>(a.u64())); i++; }
            else while (i < fmt.size() && fmt[i] >= u'0' && fmt[i] <= u'9') spec += static_cast<char>(fmt[i++]);
        }
        int len = 0; /* 0 int, 1 long/long long/size_t, -1 short, -2 char, 2 long double */
        while (i < fmt.size()) {
            const char16_t c = fmt[i];
            if (c == u'l' || c == u'q' || c == u'j' || c == u'z' || c == u't') { len = 1; i++; }
            else if (c == u'h') { len = len == -1 ? -2 : -1; i++; }
            else if (c == u'L') { len = 2; i++; }
            else break;
        }
        if (i >= fmt.size()) break;
        const char16_t conv = fmt[i++];
        char buf[512];
        switch (conv) {
        case u'@': {
            id o = obj(a.u64());
            std::u16string d = o ? to_utf16(describe(o)) : u"(null)";
            out += d;
            break;
        }
        case u'd': case u'i': {
            std::int64_t v = static_cast<std::int64_t>(a.u64());
            if (len == 0) v = static_cast<std::int32_t>(v);
            else if (len == -1) v = static_cast<short>(v);
            else if (len == -2) v = static_cast<signed char>(v);
            std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), static_cast<long long>(v));
            out += to_utf16(buf);
            break;
        }
        case u'u': case u'x': case u'X': case u'o': {
            std::uint64_t v = a.u64();
            if (len == 0) v = static_cast<std::uint32_t>(v);
            else if (len == -1) v = static_cast<unsigned short>(v);
            else if (len == -2) v = static_cast<unsigned char>(v);
            std::snprintf(buf, sizeof buf, (spec + "ll" + static_cast<char>(conv)).c_str(), static_cast<unsigned long long>(v));
            out += to_utf16(buf);
            break;
        }
        case u'f': case u'F': case u'e': case u'E': case u'g': case u'G': case u'a': case u'A':
            std::snprintf(buf, sizeof buf, (spec + static_cast<char>(conv)).c_str(), a.f64());
            out += to_utf16(buf);
            break;
        case u'c': out.push_back(static_cast<char16_t>(a.u64() & 0xFF)); break;
        case u'C': out.push_back(static_cast<char16_t>(a.u64() & 0xFFFF)); break;
        case u's': {
            const char *s = reinterpret_cast<const char *>(a.u64());
            std::snprintf(buf, sizeof buf, (spec + "s").c_str(), s ? s : "(null)");
            out += to_utf16(buf);
            break;
        }
        case u'S': {
            const UniChar *s = reinterpret_cast<const UniChar *>(a.u64());
            if (s) while (*s) out.push_back(*s++);
            break;
        }
        case u'p': std::snprintf(buf, sizeof buf, "%p", reinterpret_cast<void *>(a.u64())); out += to_utf16(buf); break;
        default: out.push_back(u'%'); out.push_back(conv); break;
        }
    }
    return out;
}

/* The anonymous arguments of an objc method call start at the guest SP. */
guest_args args_after_registers() {
    const mm_cpu_context *ctx = api()->current_context();
    return {ctx ? ctx->x[31] : 0};
}

/* ---- comparison helpers -------------------------------------------------- */

/* NSStringCompareOptions; literal search is the only mode we implement, so
 * kCFCompareNonliteral (2) needs no flag of its own. */
constexpr std::uint64_t NSCaseInsensitiveSearch = 1, NSBackwardsSearch = 4, NSAnchoredSearch = 8, NSNumericSearch = 64;

char16_t fold(char16_t c) { return (c >= u'A' && c <= u'Z') ? static_cast<char16_t>(c + 32) : c; }

std::int64_t compare_views(std::u16string_view a, std::u16string_view b, std::uint64_t opts) {
    const bool ci = opts & NSCaseInsensitiveSearch, numeric = opts & NSNumericSearch;
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (numeric && a[i] >= u'0' && a[i] <= u'9' && b[j] >= u'0' && b[j] <= u'9') {
            std::uint64_t x = 0, y = 0;
            while (i < a.size() && a[i] >= u'0' && a[i] <= u'9') x = x * 10 + (a[i++] - u'0');
            while (j < b.size() && b[j] >= u'0' && b[j] <= u'9') y = y * 10 + (b[j++] - u'0');
            if (x != y) return x < y ? -1 : 1;
            continue;
        }
        const char16_t x = ci ? fold(a[i]) : a[i], y = ci ? fold(b[j]) : b[j];
        if (x != y) return x < y ? -1 : 1;
        i++; j++;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

CFRange find(std::u16string_view hay, std::u16string_view needle, std::uint64_t opts, CFRange range) {
    if (range.location < 0 || range.location > static_cast<CFIndex>(hay.size())) return {kCFNotFound, 0};
    if (range.location + range.length > static_cast<CFIndex>(hay.size())) range.length = static_cast<CFIndex>(hay.size()) - range.location;
    if (needle.empty()) return {kCFNotFound, 0};
    const bool ci = opts & NSCaseInsensitiveSearch;
    auto eq_at = [&](std::size_t pos) {
        for (std::size_t k = 0; k < needle.size(); k++) {
            const char16_t x = ci ? fold(hay[pos + k]) : hay[pos + k], y = ci ? fold(needle[k]) : needle[k];
            if (x != y) return false;
        }
        return true;
    };
    const std::size_t lo = static_cast<std::size_t>(range.location), hi = static_cast<std::size_t>(range.location + range.length);
    if (needle.size() > hi - lo) return {kCFNotFound, 0};
    if (opts & NSAnchoredSearch) {
        const std::size_t pos = (opts & NSBackwardsSearch) ? hi - needle.size() : lo;
        return eq_at(pos) ? CFRange{static_cast<CFIndex>(pos), static_cast<CFIndex>(needle.size())} : CFRange{kCFNotFound, 0};
    }
    if (opts & NSBackwardsSearch) {
        for (std::size_t pos = hi - needle.size() + 1; pos-- > lo;)
            if (eq_at(pos)) return {static_cast<CFIndex>(pos), static_cast<CFIndex>(needle.size())};
    } else {
        for (std::size_t pos = lo; pos + needle.size() <= hi; pos++)
            if (eq_at(pos)) return {static_cast<CFIndex>(pos), static_cast<CFIndex>(needle.size())};
    }
    return {kCFNotFound, 0};
}

std::uint64_t hash_chars(std::u16string_view s) {
    std::uint64_t h = 1469598103934665603ull;
    for (char16_t c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

/* ---- path helpers --------------------------------------------------------- */

std::u16string_view last_component(std::u16string_view p) {
    while (p.size() > 1 && p.back() == u'/') p.remove_suffix(1);
    const auto slash = p.rfind(u'/');
    if (slash == std::u16string_view::npos) return p;
    if (p.size() == 1) return p; /* "/" */
    return p.substr(slash + 1);
}

std::u16string delete_last_component(std::u16string_view p) {
    while (p.size() > 1 && p.back() == u'/') p.remove_suffix(1);
    const auto slash = p.rfind(u'/');
    if (slash == std::u16string_view::npos) return u"";
    if (slash == 0) return u"/";
    return std::u16string(p.substr(0, slash));
}

std::u16string_view extension(std::u16string_view p) {
    const auto last = last_component(p);
    const auto dot = last.rfind(u'.');
    if (dot == std::u16string_view::npos || dot == 0) return {};
    return last.substr(dot + 1);
}

/* ---- NSString methods --------------------------------------------------- */

#define SELF_VIEW std::u16string_view sv = view(self)

std::uint64_t m_length(id self, SEL) { return static_cast<std::uint64_t>(string_chars(self).length); }
std::uint64_t m_characterAtIndex(id self, SEL, std::uint64_t i) {
    const chars c = string_chars(self);
    if (static_cast<CFIndex>(i) >= c.length) { std::fprintf(stderr, "Foundation: -characterAtIndex: %llu out of range %lld\n", static_cast<unsigned long long>(i), static_cast<long long>(c.length)); std::abort(); }
    return c.data[i];
}
std::uint64_t m_getCharactersRange(id self, SEL, UniChar *buf, std::uint64_t loc, std::uint64_t len) {
    const chars c = string_chars(self);
    if (static_cast<CFIndex>(loc + len) > c.length) len = static_cast<std::uint64_t>(c.length) - loc;
    std::memcpy(buf, c.data + loc, len * 2);
    return 0;
}
std::uint64_t m_getCharacters(id self, SEL, UniChar *buf) { const chars c = string_chars(self); std::memcpy(buf, c.data, static_cast<std::size_t>(c.length) * 2); return 0; }
std::uint64_t m_UTF8String(id self, SEL) { return ret(utf8_ptr(self)); }
std::uint64_t m_cStringUsingEncoding(id self, SEL, std::uint64_t enc) {
    if (ns_to_cf_encoding(enc) == kCFStringEncodingUTF8) return ret(utf8_ptr(self));
    static thread_local std::string tmp;
    if (!encode(view(self), ns_to_cf_encoding(enc), tmp, false)) return 0;
    return ret(tmp.c_str());
}
std::uint64_t m_lengthOfBytes(id self, SEL, std::uint64_t enc) {
    std::string tmp;
    if (!encode(view(self), ns_to_cf_encoding(enc), tmp, false)) return 0;
    return tmp.size();
}
std::uint64_t m_maximumLengthOfBytes(id self, SEL, std::uint64_t enc) {
    const std::uint64_t n = static_cast<std::uint64_t>(string_chars(self).length);
    switch (ns_to_cf_encoding(enc)) {
    case kCFStringEncodingUTF8: return n * 3;
    case kCFStringEncodingUnicode: case kCFStringEncodingUTF16BE: case kCFStringEncodingUTF16LE: return n * 2;
    case kCFStringEncodingUTF32: case kCFStringEncodingUTF32BE: case kCFStringEncodingUTF32LE: return n * 4;
    default: return n;
    }
}
std::uint64_t m_getCString(id self, SEL, char *buf, std::uint64_t max, std::uint64_t enc) {
    std::string tmp;
    if (!encode(view(self), ns_to_cf_encoding(enc), tmp, false) || tmp.size() + 1 > max) return 0;
    std::memcpy(buf, tmp.c_str(), tmp.size() + 1);
    return 1;
}
std::uint64_t m_fileSystemRepresentation(id self, SEL) { return ret(utf8_ptr(self)); }
std::uint64_t m_getFileSystemRepresentation(id self, SEL, char *buf, std::uint64_t max) {
    const std::string s = to_utf8(self);
    if (s.size() + 1 > max) return 0;
    std::memcpy(buf, s.c_str(), s.size() + 1);
    return 1;
}
std::uint64_t m_isEqualToString(id self, SEL, id other) { return other && is_string(other) && view(self) == view(other); }
std::uint64_t m_isEqual(id self, SEL, id other) { return other && is_string(other) && view(self) == view(other); }
std::uint64_t m_hash(id self, SEL) {
    if (is_host(self)) {
        auto *o = as<string_object>(self);
        if (!o->hash_valid) { o->hash_cache = hash_chars(o->chars); o->hash_valid = true; }
        return o->hash_cache;
    }
    return hash_chars(view(self));
}
std::uint64_t m_compare(id self, SEL, id other) { return static_cast<std::uint64_t>(compare_views(view(self), view(other), 0)); }
std::uint64_t m_compareOptions(id self, SEL, id other, std::uint64_t opts) { return static_cast<std::uint64_t>(compare_views(view(self), view(other), opts)); }
std::uint64_t m_compareOptionsRange(id self, SEL, id other, std::uint64_t opts, std::uint64_t loc, std::uint64_t len) {
    return static_cast<std::uint64_t>(compare_views(view(self).substr(loc, len), view(other), opts));
}
std::uint64_t m_caseInsensitiveCompare(id self, SEL, id other) { return static_cast<std::uint64_t>(compare_views(view(self), view(other), NSCaseInsensitiveSearch)); }
std::uint64_t m_localizedCompare(id self, SEL, id other) { return static_cast<std::uint64_t>(compare_views(view(self), view(other), 0)); }
std::uint64_t m_hasPrefix(id self, SEL, id p) { SELF_VIEW; const auto pv = view(p); return !pv.empty() && sv.substr(0, pv.size()) == pv; }
std::uint64_t m_hasSuffix(id self, SEL, id p) { SELF_VIEW; const auto pv = view(p); return !pv.empty() && sv.size() >= pv.size() && sv.substr(sv.size() - pv.size()) == pv; }
std::uint64_t m_containsString(id self, SEL, id p) { SELF_VIEW; return find(sv, view(p), 0, {0, static_cast<CFIndex>(sv.size())}).location != kCFNotFound; }
std::uint64_t m_rangeOfString(id self, SEL, id p) { SELF_VIEW; const CFRange r = find(sv, view(p), 0, {0, static_cast<CFIndex>(sv.size())});
    mm_objc_return_words_hi(static_cast<std::uint64_t>(r.length));
    return static_cast<std::uint64_t>(r.location); }
std::uint64_t m_rangeOfStringOptions(id self, SEL, id p, std::uint64_t opts) { SELF_VIEW; const CFRange r = find(sv, view(p), opts, {0, static_cast<CFIndex>(sv.size())}); mm_objc_return_words_hi(static_cast<std::uint64_t>(r.length));
    return static_cast<std::uint64_t>(r.location); }
std::uint64_t m_rangeOfStringOptionsRange(id self, SEL, id p, std::uint64_t opts, std::uint64_t loc, std::uint64_t len) {
    SELF_VIEW; const CFRange r = find(sv, view(p), opts, {static_cast<CFIndex>(loc), static_cast<CFIndex>(len)}); mm_objc_return_words_hi(static_cast<std::uint64_t>(r.length));
    return static_cast<std::uint64_t>(r.location);
}
std::uint64_t m_substringFromIndex(id self, SEL, std::uint64_t i) { SELF_VIEW; return ret(autorelease(string_from_utf16(sv.substr(std::min(i, sv.size()))))); }
std::uint64_t m_substringToIndex(id self, SEL, std::uint64_t i) { SELF_VIEW; return ret(autorelease(string_from_utf16(sv.substr(0, i)))); }
std::uint64_t m_substringWithRange(id self, SEL, std::uint64_t loc, std::uint64_t len) { SELF_VIEW; return ret(autorelease(string_from_utf16(sv.substr(std::min(loc, sv.size()), len)))); }
std::uint64_t m_componentsSeparatedByString(id self, SEL, id sep) {
    SELF_VIEW;
    const auto sepv = view(sep);
    std::vector<id> parts;
    std::size_t start = 0;
    while (true) {
        const auto pos = sepv.empty() ? std::u16string_view::npos : sv.find(sepv, start);
        parts.push_back(string_from_utf16(sv.substr(start, pos == std::u16string_view::npos ? std::u16string_view::npos : pos - start)));
        if (pos == std::u16string_view::npos) break;
        start = pos + sepv.size();
    }
    id arr = array_create(parts.data(), static_cast<CFIndex>(parts.size()), false);
    for (id p : parts) release(p);
    return ret(autorelease(arr));
}
std::uint64_t m_stringByAppendingString(id self, SEL, id o) { std::u16string s(view(self)); s += view(o); return ret(autorelease(string_from_utf16(s))); }
std::uint64_t m_stringByAppendingPathComponent(id self, SEL, id o) {
    std::u16string s(view(self));
    const auto c = view(o);
    if (!s.empty() && s.back() != u'/' && !(c.size() && c[0] == u'/')) s += u'/';
    s += c;
    return ret(autorelease(string_from_utf16(s)));
}
std::uint64_t m_stringByAppendingPathExtension(id self, SEL, id o) { std::u16string s(view(self)); s += u'.'; s += view(o); return ret(autorelease(string_from_utf16(s))); }
std::uint64_t m_lastPathComponent(id self, SEL) { return ret(autorelease(string_from_utf16(last_component(view(self))))); }
std::uint64_t m_pathExtension(id self, SEL) { return ret(autorelease(string_from_utf16(extension(view(self))))); }
std::uint64_t m_stringByDeletingLastPathComponent(id self, SEL) { return ret(autorelease(string_from_utf16(delete_last_component(view(self))))); }
std::uint64_t m_stringByDeletingPathExtension(id self, SEL) {
    SELF_VIEW;
    const auto ext = extension(sv);
    return ret(autorelease(string_from_utf16(ext.empty() ? sv : sv.substr(0, sv.size() - ext.size() - 1))));
}
std::uint64_t m_pathComponents(id self, SEL) {
    SELF_VIEW;
    std::vector<id> parts;
    if (!sv.empty() && sv[0] == u'/') parts.push_back(string_from_utf8("/"));
    std::size_t i = 0;
    while (i < sv.size()) {
        while (i < sv.size() && sv[i] == u'/') i++;
        const std::size_t start = i;
        while (i < sv.size() && sv[i] != u'/') i++;
        if (i > start) parts.push_back(string_from_utf16(sv.substr(start, i - start)));
    }
    if (sv.size() > 1 && sv.back() == u'/') parts.push_back(string_from_utf8("/"));
    id arr = array_create(parts.data(), static_cast<CFIndex>(parts.size()), false);
    for (id p : parts) release(p);
    return ret(autorelease(arr));
}
std::u16string replace_all(std::u16string_view s, std::u16string_view from, std::u16string_view to, std::uint64_t opts, CFRange range) {
    std::u16string out(s.substr(0, static_cast<std::size_t>(range.location)));
    CFIndex pos = range.location;
    const CFIndex end = range.location + range.length;
    while (true) {
        const CFRange r = find(s, from, opts & ~NSBackwardsSearch, {pos, end - pos});
        if (r.location == kCFNotFound) break;
        out += s.substr(static_cast<std::size_t>(pos), static_cast<std::size_t>(r.location - pos));
        out += to;
        pos = r.location + r.length;
    }
    out += s.substr(static_cast<std::size_t>(pos));
    return out;
}
std::uint64_t m_stringByReplacingOccurrences(id self, SEL, id from, id to) {
    SELF_VIEW; return ret(autorelease(string_from_utf16(replace_all(sv, view(from), view(to), 0, {0, static_cast<CFIndex>(sv.size())}))));
}
std::uint64_t m_stringByReplacingOccurrencesOptionsRange(id self, SEL, id from, id to, std::uint64_t opts, std::uint64_t loc, std::uint64_t len) {
    SELF_VIEW; return ret(autorelease(string_from_utf16(replace_all(sv, view(from), view(to), opts, {static_cast<CFIndex>(loc), static_cast<CFIndex>(len)}))));
}
std::uint64_t m_stringByReplacingCharactersInRange(id self, SEL, std::uint64_t loc, std::uint64_t len, id to) {
    std::u16string s(view(self)); s.replace(loc, len, view(to)); return ret(autorelease(string_from_utf16(s)));
}
std::uint64_t m_uppercaseString(id self, SEL) { std::u16string s(view(self)); for (auto &c : s) if (c >= u'a' && c <= u'z') c = static_cast<char16_t>(c - 32); return ret(autorelease(string_from_utf16(s))); }
std::uint64_t m_lowercaseString(id self, SEL) { std::u16string s(view(self)); for (auto &c : s) c = fold(c); return ret(autorelease(string_from_utf16(s))); }
std::uint64_t m_capitalizedString(id self, SEL) {
    std::u16string s(view(self)); bool start = true;
    for (auto &c : s) { if (c == u' ') { start = true; continue; } if (start && c >= u'a' && c <= u'z') c = static_cast<char16_t>(c - 32); else if (!start) c = fold(c); start = false; }
    return ret(autorelease(string_from_utf16(s)));
}
std::uint64_t m_stringByTrimmingWhitespace(id self, SEL, id) {
    SELF_VIEW;
    std::size_t a = 0, b = sv.size();
    auto ws = [](char16_t c) { return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r'; };
    while (a < b && ws(sv[a])) a++;
    while (b > a && ws(sv[b - 1])) b--;
    return ret(autorelease(string_from_utf16(sv.substr(a, b - a))));
}
std::uint64_t m_intValue(id self, SEL) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::atoi(to_utf8(self).c_str()))); }
std::uint64_t m_integerValue(id self, SEL) { return static_cast<std::uint64_t>(std::strtoll(to_utf8(self).c_str(), nullptr, 10)); }
double m_doubleValue(id self, SEL) { return std::strtod(to_utf8(self).c_str(), nullptr); }
double m_floatValue(id self, SEL) { return static_cast<float>(std::strtod(to_utf8(self).c_str(), nullptr)); }
std::uint64_t m_boolValue(id self, SEL) {
    std::string s = to_utf8(self);
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < s.size() && (s[i] == 'Y' || s[i] == 'y' || s[i] == 'T' || s[i] == 't')) return 1;
    return std::strtoll(s.c_str() + i, nullptr, 10) != 0;
}
std::uint64_t m_description(id self, SEL) { return ret(self); }
std::uint64_t m_copy(id self, SEL) {
    if (is_constant(self) || (is_host(self) && !as<string_object>(self)->mutable_)) return ret(retain(self));
    return ret(string_from_utf16(view(self)));
}
std::uint64_t m_copyWithZone(id self, SEL, void *) { return m_copy(self, nullptr); }
std::uint64_t m_mutableCopy(id self, SEL) { id s = string_from_utf16(view(self)); as<string_object>(s)->mutable_ = true; return ret(s); }
std::uint64_t m_mutableCopyWithZone(id self, SEL, void *) { return m_mutableCopy(self, nullptr); }
std::uint64_t m_dataUsingEncoding(id self, SEL, std::uint64_t enc) {
    std::string bytes;
    if (!encode(view(self), ns_to_cf_encoding(enc), bytes, false)) return 0;
    return ret(autorelease(data_create(bytes.data(), static_cast<CFIndex>(bytes.size()), false)));
}
std::uint64_t m_dataUsingEncodingLossy(id self, SEL, std::uint64_t enc, std::uint64_t lossy) {
    std::string bytes;
    if (!encode(view(self), ns_to_cf_encoding(enc), bytes, lossy != 0)) return 0;
    return ret(autorelease(data_create(bytes.data(), static_cast<CFIndex>(bytes.size()), false)));
}
std::uint64_t m_canBeConvertedToEncoding(id self, SEL, std::uint64_t enc) { std::string b; return encode(view(self), ns_to_cf_encoding(enc), b, false); }
std::uint64_t m_stringByExpandingTilde(id self, SEL) {
    SELF_VIEW;
    if (sv.empty() || sv[0] != u'~') return ret(self);
    std::u16string s = to_utf16(home_directory()); s += sv.substr(1);
    return ret(autorelease(string_from_utf16(s)));
}
std::uint64_t m_stringByStandardizingPath(id self, SEL) {
    std::string p = to_utf8(self);
    if (!p.empty() && p[0] == '~') p = home_directory() + p.substr(1);
    std::vector<std::string> parts;
    const bool abs = !p.empty() && p[0] == '/';
    std::size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && p[i] == '/') i++;
        const std::size_t start = i;
        while (i < p.size() && p[i] != '/') i++;
        const std::string part = p.substr(start, i - start);
        if (part.empty() || part == ".") continue;
        if (part == ".." && !parts.empty() && parts.back() != "..") parts.pop_back();
        else parts.push_back(part);
    }
    std::string out = abs ? "/" : "";
    for (std::size_t k = 0; k < parts.size(); k++) { if (k) out += '/'; out += parts[k]; }
    if (out.empty()) out = abs ? "/" : ".";
    return ret(autorelease(string_from_utf8(out)));
}
std::uint64_t m_isAbsolutePath(id self, SEL) { SELF_VIEW; return !sv.empty() && sv[0] == u'/'; }
std::uint64_t m_writeToFile(id self, SEL, id path, std::uint64_t, std::uint64_t enc, id *error) {
    std::string bytes;
    if (!encode(view(self), ns_to_cf_encoding(enc), bytes, false)) return 0;
    FILE *f = std::fopen(to_utf8(path).c_str(), "wb");
    if (!f) { if (error) *error = autorelease(error_create(constant_string("NSCocoaErrorDomain"), 4, nullptr)); return 0; }
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return 1;
}

/* init family: `self` is the allocated (constructed) instance. */
string_object *init_target(id self) {
    if (!is_host(self)) { std::fprintf(stderr, "Foundation: NSString init on a non-host instance\n"); std::abort(); }
    return as<string_object>(self);
}
std::uint64_t m_init(id self, SEL) { init_target(self); return ret(self); }
std::uint64_t m_initWithUTF8String(id self, SEL, const char *s) { if (!s) { release(self); return 0; } init_target(self)->chars = to_utf16(s); return ret(self); }
std::uint64_t m_initWithString(id self, SEL, id s) { init_target(self)->chars = view(s); return ret(self); }
std::uint64_t m_initWithCStringEncoding(id self, SEL, const char *s, std::uint64_t enc) {
    if (!s || !decode(s, ns_to_cf_encoding(enc), init_target(self)->chars)) { release(self); return 0; }
    return ret(self);
}
std::uint64_t m_initWithCString(id self, SEL, const char *s) { return m_initWithCStringEncoding(self, nullptr, s, 4); }
std::uint64_t m_initWithBytesLengthEncoding(id self, SEL, const char *b, std::uint64_t n, std::uint64_t enc) {
    if (!decode(std::string_view(b, n), ns_to_cf_encoding(enc), init_target(self)->chars)) { release(self); return 0; }
    return ret(self);
}
std::uint64_t m_initWithBytesNoCopy(id self, SEL, const char *b, std::uint64_t n, std::uint64_t enc, std::uint64_t free_when_done) {
    const std::uint64_t r = m_initWithBytesLengthEncoding(self, nullptr, b, n, enc);
    if (free_when_done) std::free(const_cast<char *>(b));
    return r;
}
std::uint64_t m_initWithCharactersLength(id self, SEL, const UniChar *p, std::uint64_t n) { init_target(self)->chars.assign(p, n); return ret(self); }
std::uint64_t m_initWithCharactersNoCopy(id self, SEL, const UniChar *p, std::uint64_t n, std::uint64_t free_when_done) {
    init_target(self)->chars.assign(p, n);
    if (free_when_done) std::free(const_cast<UniChar *>(p));
    return ret(self);
}
std::uint64_t m_initWithDataEncoding(id self, SEL, id data, std::uint64_t enc) {
    if (!data || !decode(data_storage(data), ns_to_cf_encoding(enc), init_target(self)->chars)) { release(self); return 0; }
    return ret(self);
}
std::uint64_t m_initWithFormat(id self, SEL, id fmt) {
    guest_args a = args_after_registers();
    init_target(self)->chars = format_guest(view(fmt), a);
    return ret(self);
}
std::uint64_t m_initWithFormatArguments(id self, SEL, id fmt, std::uint64_t va) {
    guest_args a{va};
    init_target(self)->chars = format_guest(view(fmt), a);
    return ret(self);
}
std::uint64_t m_initWithFormatLocale(id self, SEL, id fmt, id) {
    guest_args a = args_after_registers();
    init_target(self)->chars = format_guest(view(fmt), a);
    return ret(self);
}
std::uint64_t m_initWithFormatLocaleArguments(id self, SEL, id fmt, id, std::uint64_t va) { return m_initWithFormatArguments(self, nullptr, fmt, va); }
std::uint64_t m_initWithContentsOfFile(id self, SEL, id path, std::uint64_t enc, id *error) {
    FILE *f = std::fopen(to_utf8(path).c_str(), "rb");
    if (!f) { if (error) *error = autorelease(error_create(constant_string("NSCocoaErrorDomain"), 260, nullptr)); release(self); return 0; }
    std::string bytes;
    char buf[65536];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.append(buf, n);
    std::fclose(f);
    if (!decode(bytes, ns_to_cf_encoding(enc), init_target(self)->chars)) { release(self); return 0; }
    return ret(self);
}
std::uint64_t m_initWithCapacity(id self, SEL, std::uint64_t n) { init_target(self)->chars.reserve(n); return ret(self); }

/* class methods */
id new_instance(id cls) { return obj(send(cls, "alloc")); }
std::uint64_t c_string(id cls, SEL) { return ret(autorelease(obj(send(new_instance(cls), "init")))); }
std::uint64_t c_stringWithUTF8String(id cls, SEL, const char *s) { return ret(autorelease(obj(send(new_instance(cls), "initWithUTF8String:", ret(s))))); }
std::uint64_t c_stringWithString(id cls, SEL, id s) { return ret(autorelease(obj(send(new_instance(cls), "initWithString:", ret(s))))); }
std::uint64_t c_stringWithCStringEncoding(id cls, SEL, const char *s, std::uint64_t enc) { return ret(autorelease(obj(send(new_instance(cls), "initWithCString:encoding:", ret(s), enc)))); }
std::uint64_t c_stringWithCharactersLength(id cls, SEL, const UniChar *p, std::uint64_t n) { return ret(autorelease(obj(send(new_instance(cls), "initWithCharacters:length:", ret(p), n)))); }
std::uint64_t c_stringWithFormat(id cls, SEL, id fmt) {
    guest_args a = args_after_registers();
    id s = new_instance(cls);
    init_target(s)->chars = format_guest(view(fmt), a);
    return ret(autorelease(s));
}
std::uint64_t c_stringWithContentsOfFile(id cls, SEL, id path, std::uint64_t enc, id *error) { return ret(autorelease(obj(send(new_instance(cls), "initWithContentsOfFile:encoding:error:", ret(path), enc, ret(error))))); }
std::uint64_t c_stringWithCapacity(id cls, SEL, std::uint64_t n) { return ret(autorelease(obj(send(new_instance(cls), "initWithCapacity:", n)))); }
std::uint64_t c_localizedStringWithFormat(id cls, SEL, id fmt) { return c_stringWithFormat(cls, nullptr, fmt); }
std::uint64_t c_defaultCStringEncoding(id, SEL) { return 4; }
std::uint64_t c_pathWithComponents(id, SEL, id comps) {
    std::u16string out;
    for (id c : array_storage(comps)) {
        const auto v = view(c);
        if (v == u"/") { if (out.empty()) out = u"/"; continue; }
        if (!out.empty() && out.back() != u'/') out += u'/';
        out += v;
    }
    return ret(autorelease(string_from_utf16(out)));
}

/* NSMutableString */
std::uint64_t mm_appendString(id self, SEL, id s) { mutable_of(self, "appendString:")->chars += view(s); return 0; }
std::uint64_t mm_appendFormat(id self, SEL, id fmt) { guest_args a = args_after_registers(); mutable_of(self, "appendFormat:")->chars += format_guest(view(fmt), a); return 0; }
std::uint64_t mm_setString(id self, SEL, id s) { mutable_of(self, "setString:")->chars = view(s); return 0; }
std::uint64_t mm_insertStringAtIndex(id self, SEL, id s, std::uint64_t i) { mutable_of(self, "insertString:atIndex:")->chars.insert(i, view(s)); return 0; }
std::uint64_t mm_deleteCharactersInRange(id self, SEL, std::uint64_t loc, std::uint64_t len) { mutable_of(self, "deleteCharactersInRange:")->chars.erase(loc, len); return 0; }
std::uint64_t mm_replaceCharactersInRange(id self, SEL, std::uint64_t loc, std::uint64_t len, id s) { mutable_of(self, "replaceCharactersInRange:withString:")->chars.replace(loc, len, view(s)); return 0; }
std::uint64_t mm_replaceOccurrences(id self, SEL, id from, id to, std::uint64_t opts, std::uint64_t loc, std::uint64_t len) {
    string_object *o = mutable_of(self, "replaceOccurrencesOfString:withString:options:range:");
    const std::u16string before = o->chars;
    o->chars = replace_all(before, view(from), view(to), opts, {static_cast<CFIndex>(loc), static_cast<CFIndex>(len)});
    std::uint64_t count = 0;
    for (CFRange r{static_cast<CFIndex>(loc), static_cast<CFIndex>(len)}; ; ) {
        const CFRange f = find(before, view(from), opts & ~NSBackwardsSearch, r);
        if (f.location == kCFNotFound) break;
        count++;
        r = {f.location + f.length, static_cast<CFIndex>(loc + len) - (f.location + f.length)};
    }
    return count;
}

} // namespace

std::string describe(id o) {
    if (!o) return "(null)";
    if (is_string(o)) return to_utf8(o);
    id d = obj(send(o, "description"));
    if (!d) return "(null)";
    if (is_string(d)) return to_utf8(d);
    return reinterpret_cast<const char *>(d); /* NSObject's C-string fallback */
}

void register_strings() {
    Class nsobject = cls("NSObject");
    g_string = mm_objc_define_class("NSString", nsobject, sizeof(mm_objc_object), FND_CLASS_REF(NSString), FND_META_REF(NSString));
    g_mutable = mm_objc_define_class("NSMutableString", g_string, sizeof(mm_objc_object), FND_CLASS_REF(NSMutableString), FND_META_REF(NSMutableString));
    g_cf = mm_objc_define_class("__NSCFString", g_mutable, sizeof(string_object), FND_CLASS_REF(__NSCFString), FND_META_REF(__NSCFString));
    g_constant = mm_objc_define_class("__NSCFConstantString", g_cf, sizeof(const_string_layout), &cf_constant_string_class,
                                      &cf_constant_string_metaclass);
    register_lifecycle<string_object>(g_cf);
    /* NSString/NSMutableString are class clusters: +alloc yields __NSCFString. */
    register_cluster<string_object>(g_string, g_cf, false);
    register_cluster<string_object>(g_mutable, g_cf, true);
    /* Constant strings are immortal. */
    static auto noop = [](id self, SEL) -> std::uint64_t { return ret(self); };
    static auto noop_v = [](id, SEL) -> std::uint64_t { return 0; };
    mm_objc_add_method(g_constant, "retain", IMP(+noop), "@16@0:8");
    mm_objc_add_method(g_constant, "release", IMP(+noop_v), "v16@0:8");
    mm_objc_add_method(g_constant, "autorelease", IMP(+noop), "@16@0:8");
    mm_objc_add_method(g_constant, "dealloc", IMP(+noop_v), "v16@0:8");

    FND_METHODS(g_string,
        {"length", IMP(m_length), "Q16@0:8"},
        {"characterAtIndex:", IMP(m_characterAtIndex), "S24@0:8Q16"},
        {"getCharacters:range:", IMP(m_getCharactersRange), "v40@0:8^S16{_NSRange=QQ}24"},
        {"getCharacters:", IMP(m_getCharacters), "v24@0:8^S16"},
        {"UTF8String", IMP(m_UTF8String), "r*16@0:8"},
        {"cStringUsingEncoding:", IMP(m_cStringUsingEncoding), "r*24@0:8Q16"},
        {"cString", IMP(m_UTF8String), "r*16@0:8"},
        {"lengthOfBytesUsingEncoding:", IMP(m_lengthOfBytes), "Q24@0:8Q16"},
        {"maximumLengthOfBytesUsingEncoding:", IMP(m_maximumLengthOfBytes), "Q24@0:8Q16"},
        {"getCString:maxLength:encoding:", IMP(m_getCString), "B40@0:8*16Q24Q32"},
        {"fileSystemRepresentation", IMP(m_fileSystemRepresentation), "r*16@0:8"},
        {"getFileSystemRepresentation:maxLength:", IMP(m_getFileSystemRepresentation), "B32@0:8*16Q24"},
        {"isEqualToString:", IMP(m_isEqualToString), "B24@0:8@16"},
        {"isEqual:", IMP(m_isEqual), "B24@0:8@16"},
        {"hash", IMP(m_hash), "Q16@0:8"},
        {"compare:", IMP(m_compare), "q24@0:8@16"},
        {"compare:options:", IMP(m_compareOptions), "q32@0:8@16Q24"},
        {"compare:options:range:", IMP(m_compareOptionsRange), "q48@0:8@16Q24{_NSRange=QQ}32"},
        {"caseInsensitiveCompare:", IMP(m_caseInsensitiveCompare), "q24@0:8@16"},
        {"localizedCompare:", IMP(m_localizedCompare), "q24@0:8@16"},
        {"localizedCaseInsensitiveCompare:", IMP(m_caseInsensitiveCompare), "q24@0:8@16"},
        {"localizedStandardCompare:", IMP(m_localizedCompare), "q24@0:8@16"},
        {"hasPrefix:", IMP(m_hasPrefix), "B24@0:8@16"},
        {"hasSuffix:", IMP(m_hasSuffix), "B24@0:8@16"},
        {"containsString:", IMP(m_containsString), "B24@0:8@16"},
        {"rangeOfString:", IMP(m_rangeOfString), "{_NSRange=QQ}24@0:8@16"},
        {"rangeOfString:options:", IMP(m_rangeOfStringOptions), "{_NSRange=QQ}32@0:8@16Q24"},
        {"rangeOfString:options:range:", IMP(m_rangeOfStringOptionsRange), "{_NSRange=QQ}48@0:8@16Q24{_NSRange=QQ}32"},
        {"substringFromIndex:", IMP(m_substringFromIndex), "@24@0:8Q16"},
        {"substringToIndex:", IMP(m_substringToIndex), "@24@0:8Q16"},
        {"substringWithRange:", IMP(m_substringWithRange), "@32@0:8{_NSRange=QQ}16"},
        {"componentsSeparatedByString:", IMP(m_componentsSeparatedByString), "@24@0:8@16"},
        {"stringByAppendingString:", IMP(m_stringByAppendingString), "@24@0:8@16"},
        {"stringByAppendingPathComponent:", IMP(m_stringByAppendingPathComponent), "@24@0:8@16"},
        {"stringByAppendingPathExtension:", IMP(m_stringByAppendingPathExtension), "@24@0:8@16"},
        {"lastPathComponent", IMP(m_lastPathComponent), "@16@0:8"},
        {"pathExtension", IMP(m_pathExtension), "@16@0:8"},
        {"stringByDeletingLastPathComponent", IMP(m_stringByDeletingLastPathComponent), "@16@0:8"},
        {"stringByDeletingPathExtension", IMP(m_stringByDeletingPathExtension), "@16@0:8"},
        {"pathComponents", IMP(m_pathComponents), "@16@0:8"},
        {"stringByReplacingOccurrencesOfString:withString:", IMP(m_stringByReplacingOccurrences), "@32@0:8@16@24"},
        {"stringByReplacingOccurrencesOfString:withString:options:range:", IMP(m_stringByReplacingOccurrencesOptionsRange), "@56@0:8@16@24Q32{_NSRange=QQ}40"},
        {"stringByReplacingCharactersInRange:withString:", IMP(m_stringByReplacingCharactersInRange), "@40@0:8{_NSRange=QQ}16@32"},
        {"uppercaseString", IMP(m_uppercaseString), "@16@0:8"},
        {"lowercaseString", IMP(m_lowercaseString), "@16@0:8"},
        {"capitalizedString", IMP(m_capitalizedString), "@16@0:8"},
        {"stringByTrimmingCharactersInSet:", IMP(m_stringByTrimmingWhitespace), "@24@0:8@16"},
        {"intValue", IMP(m_intValue), "i16@0:8"},
        {"integerValue", IMP(m_integerValue), "q16@0:8"},
        {"longLongValue", IMP(m_integerValue), "q16@0:8"},
        {"doubleValue", IMP(m_doubleValue), "d16@0:8"},
        {"floatValue", IMP(m_floatValue), "f16@0:8"},
        {"boolValue", IMP(m_boolValue), "B16@0:8"},
        {"description", IMP(m_description), "@16@0:8"},
        {"copy", IMP(m_copy), "@16@0:8"},
        {"copyWithZone:", IMP(m_copyWithZone), "@24@0:8^v16"},
        {"mutableCopy", IMP(m_mutableCopy), "@16@0:8"},
        {"mutableCopyWithZone:", IMP(m_mutableCopyWithZone), "@24@0:8^v16"},
        {"dataUsingEncoding:", IMP(m_dataUsingEncoding), "@24@0:8Q16"},
        {"dataUsingEncoding:allowLossyConversion:", IMP(m_dataUsingEncodingLossy), "@32@0:8Q16B24"},
        {"canBeConvertedToEncoding:", IMP(m_canBeConvertedToEncoding), "B24@0:8Q16"},
        {"stringByExpandingTildeInPath", IMP(m_stringByExpandingTilde), "@16@0:8"},
        {"stringByStandardizingPath", IMP(m_stringByStandardizingPath), "@16@0:8"},
        {"stringByResolvingSymlinksInPath", IMP(m_stringByStandardizingPath), "@16@0:8"},
        {"isAbsolutePath", IMP(m_isAbsolutePath), "B16@0:8"},
        {"writeToFile:atomically:encoding:error:", IMP(m_writeToFile), "B48@0:8@16B24Q32^@40"},
        {"init", IMP(m_init), "@16@0:8"},
        {"initWithUTF8String:", IMP(m_initWithUTF8String), "@24@0:8r*16"},
        {"initWithString:", IMP(m_initWithString), "@24@0:8@16"},
        {"initWithCString:", IMP(m_initWithCString), "@24@0:8r*16"},
        {"initWithCString:encoding:", IMP(m_initWithCStringEncoding), "@32@0:8r*16Q24"},
        {"initWithBytes:length:encoding:", IMP(m_initWithBytesLengthEncoding), "@40@0:8r^v16Q24Q32"},
        {"initWithBytesNoCopy:length:encoding:freeWhenDone:", IMP(m_initWithBytesNoCopy), "@48@0:8^v16Q24Q32B40"},
        {"initWithCharacters:length:", IMP(m_initWithCharactersLength), "@32@0:8r^S16Q24"},
        {"initWithCharactersNoCopy:length:freeWhenDone:", IMP(m_initWithCharactersNoCopy), "@40@0:8^S16Q24B32"},
        {"initWithData:encoding:", IMP(m_initWithDataEncoding), "@32@0:8@16Q24"},
        {"initWithFormat:", IMP(m_initWithFormat), "@24@0:8@16"},
        {"initWithFormat:arguments:", IMP(m_initWithFormatArguments), "@32@0:8@16^v24"},
        {"initWithFormat:locale:", IMP(m_initWithFormatLocale), "@32@0:8@16@24"},
        {"initWithFormat:locale:arguments:", IMP(m_initWithFormatLocaleArguments), "@40@0:8@16@24^v32"},
        {"initWithContentsOfFile:encoding:error:", IMP(m_initWithContentsOfFile), "@40@0:8@16Q24^@32"},
        {"initWithCapacity:", IMP(m_initWithCapacity), "@24@0:8Q16"});
    FND_CLASS_METHODS(g_string,
        {"string", IMP(c_string), "@16@0:8"},
        {"stringWithUTF8String:", IMP(c_stringWithUTF8String), "@24@0:8r*16"},
        {"stringWithString:", IMP(c_stringWithString), "@24@0:8@16"},
        {"stringWithCString:encoding:", IMP(c_stringWithCStringEncoding), "@32@0:8r*16Q24"},
        {"stringWithCharacters:length:", IMP(c_stringWithCharactersLength), "@32@0:8r^S16Q24"},
        {"stringWithFormat:", IMP(c_stringWithFormat), "@24@0:8@16"},
        {"localizedStringWithFormat:", IMP(c_localizedStringWithFormat), "@24@0:8@16"},
        {"stringWithContentsOfFile:encoding:error:", IMP(c_stringWithContentsOfFile), "@40@0:8@16Q24^@32"},
        {"stringWithCapacity:", IMP(c_stringWithCapacity), "@24@0:8Q16"},
        {"defaultCStringEncoding", IMP(c_defaultCStringEncoding), "Q16@0:8"},
        {"pathWithComponents:", IMP(c_pathWithComponents), "@24@0:8@16"});
    FND_METHODS(g_mutable,
        {"appendString:", IMP(mm_appendString), "v24@0:8@16"},
        {"appendFormat:", IMP(mm_appendFormat), "v24@0:8@16"},
        {"setString:", IMP(mm_setString), "v24@0:8@16"},
        {"insertString:atIndex:", IMP(mm_insertStringAtIndex), "v32@0:8@16Q24"},
        {"deleteCharactersInRange:", IMP(mm_deleteCharactersInRange), "v32@0:8{_NSRange=QQ}16"},
        {"replaceCharactersInRange:withString:", IMP(mm_replaceCharactersInRange), "v40@0:8{_NSRange=QQ}16@32"},
        {"replaceOccurrencesOfString:withString:options:range:", IMP(mm_replaceOccurrences), "Q56@0:8@16@24Q32{_NSRange=QQ}40"});
}

/* ---- CFString C API ------------------------------------------------------ */

namespace {
id cfstr_new(bool mutable_) { id s = string_from_utf16(u""); as<string_object>(s)->mutable_ = mutable_; return s; }
}

extern "C" {

std::uint64_t CFStringGetTypeID_() DARWIN(CFStringGetTypeID);
std::uint64_t CFStringGetTypeID_() { return ret(g_string); }

CFIndex CFStringGetLength_(id s) DARWIN(CFStringGetLength);
CFIndex CFStringGetLength_(id s) { return string_chars(s).length; }

UniChar CFStringGetCharacterAtIndex_(id s, CFIndex i) DARWIN(CFStringGetCharacterAtIndex);
UniChar CFStringGetCharacterAtIndex_(id s, CFIndex i) { return string_chars(s).data[i]; }

void CFStringGetCharacters_(id s, CFIndex loc, CFIndex len, UniChar *buf) DARWIN(CFStringGetCharacters);
void CFStringGetCharacters_(id s, CFIndex loc, CFIndex len, UniChar *buf) { const chars c = string_chars(s); std::memcpy(buf, c.data + loc, static_cast<std::size_t>(len) * 2); }

const UniChar *CFStringGetCharactersPtr_(id s) DARWIN(CFStringGetCharactersPtr);
const UniChar *CFStringGetCharactersPtr_(id s) {
    if (is_constant(s)) return (as<const_string_layout>(s)->flags & 0x10) ? static_cast<const UniChar *>(as<const_string_layout>(s)->bytes) : nullptr;
    if (is_host(s)) return as<string_object>(s)->chars.data();
    return nullptr;
}

const char *CFStringGetCStringPtr_(id s, std::uint32_t enc) DARWIN(CFStringGetCStringPtr);
const char *CFStringGetCStringPtr_(id s, std::uint32_t enc) {
    if (enc != kCFStringEncodingUTF8 && enc != kCFStringEncodingASCII && enc != kCFStringEncodingMacRoman && enc != kCFStringEncodingISOLatin1) return nullptr;
    if (is_constant(s)) return (as<const_string_layout>(s)->flags & 0x10) ? nullptr : static_cast<const char *>(as<const_string_layout>(s)->bytes);
    if (is_host(s)) {
        const chars c = string_chars(s);
        for (CFIndex i = 0; i < c.length; i++) if (c.data[i] >= 0x80) return enc == kCFStringEncodingUTF8 ? utf8_ptr(s) : nullptr;
        return utf8_ptr(s);
    }
    return nullptr;
}

int CFStringGetCString_(id s, char *buf, CFIndex size, std::uint32_t enc) DARWIN(CFStringGetCString);
int CFStringGetCString_(id s, char *buf, CFIndex size, std::uint32_t enc) {
    std::string bytes;
    if (!encode(view(s), enc, bytes, false) || static_cast<CFIndex>(bytes.size()) + 1 > size) return 0;
    std::memcpy(buf, bytes.c_str(), bytes.size() + 1);
    return 1;
}

int CFStringGetFileSystemRepresentation_(id s, char *buf, CFIndex size) DARWIN(CFStringGetFileSystemRepresentation);
int CFStringGetFileSystemRepresentation_(id s, char *buf, CFIndex size) { return CFStringGetCString_(s, buf, size, kCFStringEncodingUTF8); }

CFIndex CFStringGetBytes_(id s, CFIndex loc, CFIndex len, std::uint32_t enc, std::uint8_t loss_byte, int external, std::uint8_t *buf, CFIndex max, CFIndex *used) DARWIN(CFStringGetBytes);
CFIndex CFStringGetBytes_(id s, CFIndex loc, CFIndex len, std::uint32_t enc, std::uint8_t loss_byte, int external, std::uint8_t *buf, CFIndex max, CFIndex *used) {
    (void)external;
    const auto v = view(s).substr(static_cast<std::size_t>(loc), static_cast<std::size_t>(len));
    /* Convert character by character so partial buffers stop at a boundary. */
    CFIndex converted = 0, written = 0;
    std::string tmp;
    for (std::size_t i = 0; i < v.size();) {
        std::size_t n = (v[i] >= 0xD800 && v[i] < 0xDC00 && i + 1 < v.size()) ? 2 : 1;
        if (!encode(v.substr(i, n), enc, tmp, loss_byte != 0)) break;
        if (loss_byte && tmp == "?") tmp = std::string(1, static_cast<char>(loss_byte));
        if (buf && written + static_cast<CFIndex>(tmp.size()) > max) break;
        if (buf) std::memcpy(buf + written, tmp.data(), tmp.size());
        written += static_cast<CFIndex>(tmp.size());
        converted += static_cast<CFIndex>(n);
        i += n;
    }
    if (used) *used = written;
    return converted;
}

CFIndex CFStringGetMaximumSizeForEncoding_(CFIndex n, std::uint32_t enc) DARWIN(CFStringGetMaximumSizeForEncoding);
CFIndex CFStringGetMaximumSizeForEncoding_(CFIndex n, std::uint32_t enc) {
    switch (enc) {
    case kCFStringEncodingUTF8: return n * 3;
    case kCFStringEncodingUnicode: case kCFStringEncodingUTF16BE: case kCFStringEncodingUTF16LE: return n * 2;
    case kCFStringEncodingUTF32: case kCFStringEncodingUTF32BE: case kCFStringEncodingUTF32LE: return n * 4;
    default: return n;
    }
}
CFIndex CFStringGetMaximumSizeOfFileSystemRepresentation_(id s) DARWIN(CFStringGetMaximumSizeOfFileSystemRepresentation);
CFIndex CFStringGetMaximumSizeOfFileSystemRepresentation_(id s) { return string_chars(s).length * 3 + 1; }

std::uint32_t CFStringGetSystemEncoding_() DARWIN(CFStringGetSystemEncoding);
std::uint32_t CFStringGetSystemEncoding_() { return kCFStringEncodingMacRoman; }
std::uint32_t CFStringGetFastestEncoding_(id) DARWIN(CFStringGetFastestEncoding);
std::uint32_t CFStringGetFastestEncoding_(id) { return kCFStringEncodingUnicode; }
std::uint32_t CFStringGetSmallestEncoding_(id) DARWIN(CFStringGetSmallestEncoding);
std::uint32_t CFStringGetSmallestEncoding_(id) { return kCFStringEncodingUTF8; }
std::uint64_t CFStringConvertEncodingToNSStringEncoding_(std::uint32_t enc) DARWIN(CFStringConvertEncodingToNSStringEncoding);
std::uint64_t CFStringConvertEncodingToNSStringEncoding_(std::uint32_t enc) {
    switch (enc) {
    case kCFStringEncodingASCII: return 1;
    case kCFStringEncodingUTF8: return 4;
    case kCFStringEncodingISOLatin1: return 5;
    case kCFStringEncodingUnicode: return 10;
    case kCFStringEncodingWindowsLatin1: return 12;
    case kCFStringEncodingMacRoman: return 30;
    case kCFStringEncodingUTF16BE: return 0x90000100;
    case kCFStringEncodingUTF16LE: return 0x94000100;
    default: return enc;
    }
}
std::uint32_t CFStringConvertNSStringEncodingToEncoding_(std::uint64_t ns) DARWIN(CFStringConvertNSStringEncodingToEncoding);
std::uint32_t CFStringConvertNSStringEncodingToEncoding_(std::uint64_t ns) { return ns_to_cf_encoding(ns); }
int CFStringIsEncodingAvailable_(std::uint32_t enc) DARWIN(CFStringIsEncodingAvailable);
int CFStringIsEncodingAvailable_(std::uint32_t enc) { std::string t; return encode(u"", enc, t, false); }

id CFStringCreateWithCString_(void *, const char *s, std::uint32_t enc) DARWIN(CFStringCreateWithCString);
id CFStringCreateWithCString_(void *, const char *s, std::uint32_t enc) {
    if (!s) return nullptr;
    id r = cfstr_new(false);
    if (!decode(s, enc, as<string_object>(r)->chars)) { release(r); return nullptr; }
    return r;
}
id CFStringCreateWithCStringNoCopy_(void *a, const char *s, std::uint32_t enc, void *dealloc) DARWIN(CFStringCreateWithCStringNoCopy);
id CFStringCreateWithCStringNoCopy_(void *a, const char *s, std::uint32_t enc, void *dealloc) {
    id r = CFStringCreateWithCString_(a, s, enc);
    if (dealloc != reinterpret_cast<void *>(-1) /* kCFAllocatorNull marker handled by caller */ && dealloc) std::free(const_cast<char *>(s));
    return r;
}
id CFStringCreateWithBytes_(void *, const std::uint8_t *b, CFIndex n, std::uint32_t enc, int external) DARWIN(CFStringCreateWithBytes);
id CFStringCreateWithBytes_(void *, const std::uint8_t *b, CFIndex n, std::uint32_t enc, int external) {
    (void)external;
    id r = cfstr_new(false);
    if (!decode(std::string_view(reinterpret_cast<const char *>(b), static_cast<std::size_t>(n)), enc, as<string_object>(r)->chars)) { release(r); return nullptr; }
    return r;
}
id CFStringCreateWithBytesNoCopy_(void *a, const std::uint8_t *b, CFIndex n, std::uint32_t enc, int external, void *) DARWIN(CFStringCreateWithBytesNoCopy);
id CFStringCreateWithBytesNoCopy_(void *a, const std::uint8_t *b, CFIndex n, std::uint32_t enc, int external, void *) { return CFStringCreateWithBytes_(a, b, n, enc, external); }
id CFStringCreateWithCharacters_(void *, const UniChar *p, CFIndex n) DARWIN(CFStringCreateWithCharacters);
id CFStringCreateWithCharacters_(void *, const UniChar *p, CFIndex n) { return string_from_utf16(p, n); }
id CFStringCreateWithCharactersNoCopy_(void *, const UniChar *p, CFIndex n, void *) DARWIN(CFStringCreateWithCharactersNoCopy);
id CFStringCreateWithCharactersNoCopy_(void *, const UniChar *p, CFIndex n, void *) { return string_from_utf16(p, n); }
id CFStringCreateWithSubstring_(void *, id s, CFIndex loc, CFIndex len) DARWIN(CFStringCreateWithSubstring);
id CFStringCreateWithSubstring_(void *, id s, CFIndex loc, CFIndex len) { return string_from_utf16(view(s).substr(static_cast<std::size_t>(loc), static_cast<std::size_t>(len))); }
id CFStringCreateCopy_(void *, id s) DARWIN(CFStringCreateCopy);
id CFStringCreateCopy_(void *, id s) { return obj(m_copy(s, nullptr)); }
id CFStringCreateMutable_(void *, CFIndex) DARWIN(CFStringCreateMutable);
id CFStringCreateMutable_(void *, CFIndex) { return cfstr_new(true); }
id CFStringCreateMutableCopy_(void *, CFIndex, id s) DARWIN(CFStringCreateMutableCopy);
id CFStringCreateMutableCopy_(void *, CFIndex, id s) { return obj(m_mutableCopy(s, nullptr)); }
id CFStringCreateMutableWithExternalCharactersNoCopy_(void *, UniChar *p, CFIndex n, CFIndex, void *) DARWIN(CFStringCreateMutableWithExternalCharactersNoCopy);
id CFStringCreateMutableWithExternalCharactersNoCopy_(void *, UniChar *p, CFIndex n, CFIndex, void *) {
    id s = string_from_utf16(p, n);
    as<string_object>(s)->mutable_ = true;
    return s;
}
void CFStringSetExternalCharactersNoCopy_(id s, UniChar *p, CFIndex n, CFIndex) DARWIN(CFStringSetExternalCharactersNoCopy);
void CFStringSetExternalCharactersNoCopy_(id s, UniChar *p, CFIndex n, CFIndex) { mutable_of(s, "CFStringSetExternalCharactersNoCopy")->chars.assign(p, static_cast<std::size_t>(n)); }
id CFStringCreateWithFormatAndArguments_(void *, id, id fmt, std::uint64_t va) DARWIN(CFStringCreateWithFormatAndArguments);
id CFStringCreateWithFormatAndArguments_(void *, id, id fmt, std::uint64_t va) { guest_args a{va}; return string_from_utf16(format_guest(view(fmt), a)); }
id CFStringCreateWithFormat_(void *, id, id fmt, ...) DARWIN(CFStringCreateWithFormat);
id CFStringCreateWithFormat_(void *, id, id fmt, ...) { guest_args a = args_after_registers(); return string_from_utf16(format_guest(view(fmt), a)); }
void CFStringAppendFormat_(id s, id, id fmt, ...) DARWIN(CFStringAppendFormat);
void CFStringAppendFormat_(id s, id, id fmt, ...) { guest_args a = args_after_registers(); mutable_of(s, "CFStringAppendFormat")->chars += format_guest(view(fmt), a); }
void CFStringAppend_(id s, id o) DARWIN(CFStringAppend);
void CFStringAppend_(id s, id o) { mutable_of(s, "CFStringAppend")->chars += view(o); }
void CFStringAppendCharacters_(id s, const UniChar *p, CFIndex n) DARWIN(CFStringAppendCharacters);
void CFStringAppendCharacters_(id s, const UniChar *p, CFIndex n) { mutable_of(s, "CFStringAppendCharacters")->chars.append(p, static_cast<std::size_t>(n)); }
void CFStringAppendCString_(id s, const char *p, std::uint32_t enc) DARWIN(CFStringAppendCString);
void CFStringAppendCString_(id s, const char *p, std::uint32_t enc) { std::u16string t; decode(p, enc, t); mutable_of(s, "CFStringAppendCString")->chars += t; }
void CFStringInsert_(id s, CFIndex i, id o) DARWIN(CFStringInsert);
void CFStringInsert_(id s, CFIndex i, id o) { mutable_of(s, "CFStringInsert")->chars.insert(static_cast<std::size_t>(i), view(o)); }
void CFStringDelete_(id s, CFIndex loc, CFIndex len) DARWIN(CFStringDelete);
void CFStringDelete_(id s, CFIndex loc, CFIndex len) { mutable_of(s, "CFStringDelete")->chars.erase(static_cast<std::size_t>(loc), static_cast<std::size_t>(len)); }
void CFStringReplace_(id s, CFIndex loc, CFIndex len, id o) DARWIN(CFStringReplace);
void CFStringReplace_(id s, CFIndex loc, CFIndex len, id o) { mutable_of(s, "CFStringReplace")->chars.replace(static_cast<std::size_t>(loc), static_cast<std::size_t>(len), view(o)); }
void CFStringReplaceAll_(id s, id o) DARWIN(CFStringReplaceAll);
void CFStringReplaceAll_(id s, id o) { mutable_of(s, "CFStringReplaceAll")->chars = view(o); }
CFIndex CFStringFindAndReplace_(id s, id from, id to, CFIndex loc, CFIndex len, std::uint64_t opts) DARWIN(CFStringFindAndReplace);
CFIndex CFStringFindAndReplace_(id s, id from, id to, CFIndex loc, CFIndex len, std::uint64_t opts) { return static_cast<CFIndex>(mm_replaceOccurrences(s, nullptr, from, to, opts, static_cast<std::uint64_t>(loc), static_cast<std::uint64_t>(len))); }
void CFStringUppercase_(id s, id) DARWIN(CFStringUppercase);
void CFStringUppercase_(id s, id) { for (auto &c : mutable_of(s, "CFStringUppercase")->chars) if (c >= u'a' && c <= u'z') c = static_cast<char16_t>(c - 32); }
void CFStringLowercase_(id s, id) DARWIN(CFStringLowercase);
void CFStringLowercase_(id s, id) { for (auto &c : mutable_of(s, "CFStringLowercase")->chars) c = fold(c); }
void CFStringNormalize_(id s, std::uint64_t) DARWIN(CFStringNormalize);
void CFStringNormalize_(id s, std::uint64_t) { mutable_of(s, "CFStringNormalize"); /* ASCII-range strings are already normalized */ }
void CFStringTrimWhitespace_(id s) DARWIN(CFStringTrimWhitespace);
void CFStringTrimWhitespace_(id s) {
    auto &c = mutable_of(s, "CFStringTrimWhitespace")->chars;
    std::size_t a = 0, b = c.size();
    while (a < b && (c[a] == u' ' || c[a] == u'\t' || c[a] == u'\n' || c[a] == u'\r')) a++;
    while (b > a && (c[b - 1] == u' ' || c[b - 1] == u'\t' || c[b - 1] == u'\n' || c[b - 1] == u'\r')) b--;
    c = c.substr(a, b - a);
}
std::int64_t CFStringCompare_(id a, id b, std::uint64_t opts) DARWIN(CFStringCompare);
std::int64_t CFStringCompare_(id a, id b, std::uint64_t opts) { return compare_views(view(a), view(b), opts); }
std::int64_t CFStringCompareWithOptions_(id a, id b, CFIndex loc, CFIndex len, std::uint64_t opts) DARWIN(CFStringCompareWithOptions);
std::int64_t CFStringCompareWithOptions_(id a, id b, CFIndex loc, CFIndex len, std::uint64_t opts) { return compare_views(view(a).substr(static_cast<std::size_t>(loc), static_cast<std::size_t>(len)), view(b), opts); }
std::int64_t CFStringCompareWithOptionsAndLocale_(id a, id b, CFIndex loc, CFIndex len, std::uint64_t opts, id) DARWIN(CFStringCompareWithOptionsAndLocale);
std::int64_t CFStringCompareWithOptionsAndLocale_(id a, id b, CFIndex loc, CFIndex len, std::uint64_t opts, id) { return CFStringCompareWithOptions_(a, b, loc, len, opts); }
int CFStringHasPrefix_(id s, id p) DARWIN(CFStringHasPrefix);
int CFStringHasPrefix_(id s, id p) { return static_cast<int>(m_hasPrefix(s, nullptr, p)); }
int CFStringHasSuffix_(id s, id p) DARWIN(CFStringHasSuffix);
int CFStringHasSuffix_(id s, id p) { return static_cast<int>(m_hasSuffix(s, nullptr, p)); }
int CFStringFindWithOptions_(id s, id p, CFIndex loc, CFIndex len, std::uint64_t opts, CFRange *result) DARWIN(CFStringFindWithOptions);
int CFStringFindWithOptions_(id s, id p, CFIndex loc, CFIndex len, std::uint64_t opts, CFRange *result) {
    const CFRange r = find(view(s), view(p), opts, {loc, len});
    if (result) *result = r;
    return r.location != kCFNotFound;
}
std::uint64_t CFStringFind_(id s, id p, std::uint64_t opts) DARWIN(CFStringFind);
std::uint64_t CFStringFind_(id s, id p, std::uint64_t opts) { const auto v = view(s); const CFRange r = find(v, view(p), opts, {0, static_cast<CFIndex>(v.size())}); mm_objc_return_words_hi(static_cast<std::uint64_t>(r.length));
    return static_cast<std::uint64_t>(r.location); }
std::uint64_t CFStringHash_(id s) DARWIN(CFStringHash);
std::uint64_t CFStringHash_(id s) { return m_hash(s, nullptr); }
id CFStringCreateArrayBySeparatingStrings_(void *, id s, id sep) DARWIN(CFStringCreateArrayBySeparatingStrings);
id CFStringCreateArrayBySeparatingStrings_(void *, id s, id sep) { return retain(obj(m_componentsSeparatedByString(s, nullptr, sep))); }
id CFStringCreateByCombiningStrings_(void *, id arr, id sep) DARWIN(CFStringCreateByCombiningStrings);
id CFStringCreateByCombiningStrings_(void *, id arr, id sep) {
    std::u16string out;
    bool first = true;
    for (id e : array_storage(arr)) { if (!first) out += view(sep); first = false; out += view(e); }
    return string_from_utf16(out);
}
std::int32_t CFStringGetIntValue_(id s) DARWIN(CFStringGetIntValue);
std::int32_t CFStringGetIntValue_(id s) { return std::atoi(to_utf8(s).c_str()); }
double CFStringGetDoubleValue_(id s) DARWIN(CFStringGetDoubleValue);
double CFStringGetDoubleValue_(id s) { return std::strtod(to_utf8(s).c_str(), nullptr); }
id CFStringCreateWithFileSystemRepresentation_(void *, const char *p) DARWIN(CFStringCreateWithFileSystemRepresentation);
id CFStringCreateWithFileSystemRepresentation_(void *, const char *p) { return string_from_utf8(p); }
id CFStringCreateExternalRepresentation_(void *, id s, std::uint32_t enc, std::uint8_t loss) DARWIN(CFStringCreateExternalRepresentation);
id CFStringCreateExternalRepresentation_(void *, id s, std::uint32_t enc, std::uint8_t loss) {
    std::string bytes;
    if (!encode(view(s), enc, bytes, loss != 0)) return nullptr;
    return data_create(bytes.data(), static_cast<CFIndex>(bytes.size()), false);
}
id CFStringCreateFromExternalRepresentation_(void *, id data, std::uint32_t enc) DARWIN(CFStringCreateFromExternalRepresentation);
id CFStringCreateFromExternalRepresentation_(void *, id data, std::uint32_t enc) {
    id r = cfstr_new(false);
    if (!decode(data_storage(data), enc, as<string_object>(r)->chars)) { release(r); return nullptr; }
    return r;
}
void CFShow_(id o) DARWIN(CFShow);
void CFShow_(id o) { std::fprintf(stderr, "%s\n", describe(o).c_str()); }

/* Localization with no tables: the key is the value. */
id CFCopyLocalizedString_(id key, const char *) DARWIN(CFCopyLocalizedString);
id CFCopyLocalizedString_(id key, const char *) { return retain(key); }

} // extern "C"

} // namespace fnd

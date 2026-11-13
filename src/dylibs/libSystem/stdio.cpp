/* printf/scanf family with Darwin arm64 variadic argument passing (see
 * mm_guest_va_list in internal.h). Each conversion is re-issued to the host
 * snprintf/sscanf one at a time with the argument fetched from the guest's
 * stack, so flags, widths and precisions keep the host's exact semantics. */
#include "internal.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {

enum class length { none, hh, h, l, ll, j, z, t, L };

struct directive {
    std::string spec;         /* "%-08.3" flags/width/precision, without length/conversion */
    length len = length::none;
    char conv = 0;
    bool width_arg = false, prec_arg = false, suppress = false;
    int position = 0;         /* n$ (1-based), 0 if sequential */
    std::string set;          /* scanf %[...] */
};

/* Parses the directive starting at fmt[i] == '%'; returns the index after it. */
std::size_t parse_directive(std::string_view fmt, std::size_t i, directive &d, bool scanf) {
    d = {};
    d.spec = "%";
    i++;
    /* n$ positional */
    {
        std::size_t j = i;
        int n = 0;
        while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9') n = n * 10 + (fmt[j++] - '0');
        if (j > i && j < fmt.size() && fmt[j] == '$') { d.position = n; i = j + 1; }
    }
    if (scanf && i < fmt.size() && fmt[i] == '*') { d.suppress = true; i++; }
    while (i < fmt.size() && std::strchr("-+ #0'", fmt[i])) d.spec += fmt[i++];
    if (i < fmt.size() && fmt[i] == '*') { d.width_arg = true; d.spec += '*'; i++; }
    else while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') d.spec += fmt[i++];
    if (i < fmt.size() && fmt[i] == '.') {
        d.spec += fmt[i++];
        if (i < fmt.size() && fmt[i] == '*') { d.prec_arg = true; d.spec += '*'; i++; }
        else while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') d.spec += fmt[i++];
    }
    if (i < fmt.size()) {
        switch (fmt[i]) {
        case 'h': d.len = (i + 1 < fmt.size() && fmt[i + 1] == 'h') ? (i++, length::hh) : length::h; i++; break;
        case 'l': d.len = (i + 1 < fmt.size() && fmt[i + 1] == 'l') ? (i++, length::ll) : length::l; i++; break;
        case 'q': d.len = length::ll; i++; break;
        case 'j': d.len = length::j; i++; break;
        case 'z': d.len = length::z; i++; break;
        case 't': d.len = length::t; i++; break;
        case 'L': d.len = length::L; i++; break;
        default: break;
        }
    }
    if (i < fmt.size()) d.conv = fmt[i++];
    if (d.conv == '[' && scanf) {
        d.set = "[";
        if (i < fmt.size() && fmt[i] == '^') d.set += fmt[i++];
        if (i < fmt.size() && fmt[i] == ']') d.set += fmt[i++];
        while (i < fmt.size() && fmt[i] != ']') d.set += fmt[i++];
        if (i < fmt.size()) d.set += fmt[i++];
    }
    return i;
}

const char *length_str(length l) {
    switch (l) {
    case length::hh: return "hh";
    case length::h:  return "h";
    case length::l:  return "l";
    case length::ll: return "ll";
    case length::j:  return "j";
    case length::z:  return "z";
    case length::t:  return "t";
    case length::L:  return "L";
    default:         return "";
    }
}

/* One argument fetch honoring positional directives (each slot is 8 bytes). */
mm_guest_va_list arg_source(mm_guest_va_list &seq, mm_guest_va_list base, int position) {
    if (position > 0) {
        for (int i = 1; i < position; ++i) base.next_u64();
        return base;
    }
    return seq;
}

/* Appends one host-formatted conversion, retrying when it exceeds the stack buffer. */
template <typename T> void put(std::string &out, const std::string &spec, T v) {
    char buf[512];
    const int n = std::snprintf(buf, sizeof buf, spec.c_str(), v);
    if (n < 0) return;
    if (static_cast<std::size_t>(n) < sizeof buf) { out.append(buf, static_cast<std::size_t>(n)); return; }
    std::string tmp(static_cast<std::size_t>(n) + 1, '\0');
    std::snprintf(tmp.data(), tmp.size(), spec.c_str(), v);
    out.append(tmp.data(), static_cast<std::size_t>(n));
}

/* Formats `fmt` with guest arguments starting at `args` into `out`. */
std::string format_guest(std::string_view fmt, mm_guest_va_list args) {
    std::string out;
    const mm_guest_va_list base = args;
    std::size_t i = 0;
    while (i < fmt.size()) {
        const char c = fmt[i];
        if (c != '%') { out += c; i++; continue; }
        if (i + 1 < fmt.size() && fmt[i + 1] == '%') { out += '%'; i += 2; continue; }
        directive d;
        i = parse_directive(fmt, i, d, false);
        mm_guest_va_list src = arg_source(args, base, d.position);
        mm_guest_va_list &a = d.position ? src : args;
        int width = 0, prec = 0;
        if (d.width_arg) width = a.next_i32();
        if (d.prec_arg) prec = a.next_i32();
        std::string spec = d.spec;
        /* Substitute the '*'s so the host sees literal numbers. */
        auto subst = [&](int value) {
            const std::size_t star = spec.find('*');
            if (star != std::string::npos) spec.replace(star, 1, std::to_string(value));
        };
        if (d.width_arg) subst(width);
        if (d.prec_arg) subst(prec);
        /* Integer conversions are fetched as 64-bit slots, narrowed to the
         * declared length and printed with "ll". */
        switch (d.conv) {
        case 'd': case 'i': {
            const std::int64_t v = a.next_i64();
            std::int64_t narrowed = v;
            if (d.len == length::none) narrowed = static_cast<std::int32_t>(v);
            else if (d.len == length::h) narrowed = static_cast<short>(v);
            else if (d.len == length::hh) narrowed = static_cast<signed char>(v);
            put(out, spec + "lld", static_cast<long long>(narrowed));
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            const std::uint64_t v = a.next_u64();
            std::uint64_t narrowed = v;
            if (d.len == length::none) narrowed = static_cast<std::uint32_t>(v);
            else if (d.len == length::h) narrowed = static_cast<unsigned short>(v);
            else if (d.len == length::hh) narrowed = static_cast<unsigned char>(v);
            put(out, spec + "ll" + d.conv, static_cast<unsigned long long>(narrowed));
            break;
        }
        case 'c':
            if (d.len == length::l) put(out, spec + "lc", static_cast<wint_t>(a.next_u64()));
            else put(out, spec + "c", static_cast<int>(a.next_i32()));
            break;
        case 'C': put(out, spec + "lc", static_cast<wint_t>(a.next_u64())); break;
        case 's': case 'S': {
            const bool wide = d.conv == 'S' || d.len == length::l;
            const void *p = a.next_ptr<void>();
            if (wide) put(out, spec + "ls", p ? static_cast<const wchar_t *>(p) : L"(null)");
            else put(out, spec + "s", p ? static_cast<const char *>(p) : "(null)");
            break;
        }
        case 'p': put(out, spec + "p", a.next_ptr<void>()); break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            if (d.len == length::L) put(out, spec + "L" + d.conv, a.next_long_double());
            else put(out, spec + d.conv, a.next_double());
            break;
        case 'n': {
            void *p = a.next_ptr<void>();
            const std::size_t count = out.size();
            switch (d.len) {
            case length::hh: *static_cast<signed char *>(p) = static_cast<signed char>(count); break;
            case length::h:  *static_cast<short *>(p) = static_cast<short>(count); break;
            case length::none: *static_cast<int *>(p) = static_cast<int>(count); break;
            default: *static_cast<long long *>(p) = static_cast<long long>(count); break;
            }
            break;
        }
        default:
            /* Unknown conversion: emit it verbatim like most libcs do. */
            out += d.spec;
            out += d.conv;
            break;
        }
    }
    return out;
}

int emit_to_file(FILE *f, const std::string &s) {
    if (std::fwrite(s.data(), 1, s.size(), f) != s.size()) return -1;
    return static_cast<int>(s.size());
}

int copy_out(char *dst, std::size_t size, const std::string &s) {
    if (size) {
        const std::size_t n = s.size() < size - 1 ? s.size() : size - 1;
        std::memcpy(dst, s.data(), n);
        dst[n] = 0;
    }
    return static_cast<int>(s.size());
}

/* scanf: split at each directive and run host sscanf on "literal-prefix
 * + directive + %n" so the host does all the parsing. */
int scan_guest(const char *input, std::string_view fmt, mm_guest_va_list args) {
    int assigned = 0;
    const char *in = input;
    std::size_t i = 0;
    std::string prefix;
    while (i < fmt.size()) {
        const char c = fmt[i];
        if (c != '%') { prefix += c; i++; continue; }
        if (i + 1 < fmt.size() && fmt[i + 1] == '%') { prefix += "%%"; i += 2; continue; }
        directive d;
        i = parse_directive(fmt, i, d, true);
        std::string spec = prefix + d.spec;
        prefix.clear();
        if (d.suppress) spec.insert(spec.find('%') + 1, "*");
        int consumed = -1;
        int r;
        if (d.conv == 'n') {
            if (!d.suppress) {
                void *p = args.next_ptr<void>();
                const long long count = in - input;
                if (d.len == length::none) *static_cast<int *>(p) = static_cast<int>(count);
                else *static_cast<long long *>(p) = count;
            }
            continue;
        }
        if (d.conv == '[') spec += d.set;
        else { spec += length_str(d.len); spec += d.conv; }
        spec += "%n";
        if (d.suppress) {
            r = std::sscanf(in, spec.c_str(), &consumed);
        } else {
            void *p = args.next_ptr<void>();
            r = std::sscanf(in, spec.c_str(), p, &consumed);
            if (r >= 1) assigned++;
        }
        if (r == EOF) return assigned ? assigned : EOF;
        if (consumed < 0) return assigned; /* matching failure */
        in += consumed;
    }
    if (!prefix.empty()) {
        std::string spec = prefix + "%n";
        int consumed = -1;
        std::sscanf(in, spec.c_str(), &consumed);
    }
    return assigned;
}

} // namespace

/* Darwin symbol names; the "_l" and "_chk" variants take the same
 * anonymous arguments (locale/flags are ignored: formatting is locale-
 * independent in the C locale the guest runs in). */
extern "C" {

int darwin_printf(const char *fmt, ...) DARWIN(printf);
int darwin_printf(const char *fmt, ...) { return emit_to_file(stdout, format_guest(fmt, mm_guest_va_list::at_call_site(1))); }

int darwin_fprintf(FILE *f, const char *fmt, ...) DARWIN(fprintf);
int darwin_fprintf(FILE *f, const char *fmt, ...) { return emit_to_file(f, format_guest(fmt, mm_guest_va_list::at_call_site(2))); }

int darwin_sprintf(char *dst, const char *fmt, ...) DARWIN(sprintf);
int darwin_sprintf(char *dst, const char *fmt, ...) {
    const std::string s = format_guest(fmt, mm_guest_va_list::at_call_site(2));
    std::memcpy(dst, s.c_str(), s.size() + 1);
    return static_cast<int>(s.size());
}

int darwin_snprintf(char *dst, std::size_t size, const char *fmt, ...) DARWIN(snprintf);
int darwin_snprintf(char *dst, std::size_t size, const char *fmt, ...) {
    return copy_out(dst, size, format_guest(fmt, mm_guest_va_list::at_call_site(3)));
}

int darwin_asprintf(char **out, const char *fmt, ...) DARWIN(asprintf);
int darwin_asprintf(char **out, const char *fmt, ...) {
    const std::string s = format_guest(fmt, mm_guest_va_list::at_call_site(2));
    *out = static_cast<char *>(std::malloc(s.size() + 1));
    if (!*out) return -1;
    std::memcpy(*out, s.c_str(), s.size() + 1);
    return static_cast<int>(s.size());
}

int darwin_snprintf_l(char *dst, std::size_t size, void *, const char *fmt, ...) DARWIN(snprintf_l);
int darwin_snprintf_l(char *dst, std::size_t size, void *, const char *fmt, ...) {
    return copy_out(dst, size, format_guest(fmt, mm_guest_va_list::at_call_site(4)));
}

int darwin_asprintf_l(char **out, void *, const char *fmt, ...) DARWIN(asprintf_l);
int darwin_asprintf_l(char **out, void *, const char *fmt, ...) {
    const std::string s = format_guest(fmt, mm_guest_va_list::at_call_site(3));
    *out = static_cast<char *>(std::malloc(s.size() + 1));
    if (!*out) return -1;
    std::memcpy(*out, s.c_str(), s.size() + 1);
    return static_cast<int>(s.size());
}

int darwin_snprintf_chk(char *dst, std::size_t size, int, std::size_t capacity, const char *fmt, ...) DARWIN(__snprintf_chk);
int darwin_snprintf_chk(char *dst, std::size_t size, int, std::size_t capacity, const char *fmt, ...) {
    if (capacity != static_cast<std::size_t>(-1) && size > capacity) std::abort();
    return copy_out(dst, size, format_guest(fmt, mm_guest_va_list::at_call_site(5)));
}

int darwin_sprintf_chk(char *dst, int, std::size_t size, const char *fmt, ...) DARWIN(__sprintf_chk);
int darwin_sprintf_chk(char *dst, int, std::size_t size, const char *fmt, ...) {
    const std::string s = format_guest(fmt, mm_guest_va_list::at_call_site(4));
    if (size != static_cast<std::size_t>(-1) && s.size() + 1 > size) std::abort();
    std::memcpy(dst, s.c_str(), s.size() + 1);
    return static_cast<int>(s.size());
}

/* v* variants: the Darwin va_list is a pointer into the caller's argument area. */
int darwin_vprintf(const char *fmt, std::uint64_t ap) DARWIN(vprintf);
int darwin_vprintf(const char *fmt, std::uint64_t ap) { return emit_to_file(stdout, format_guest(fmt, mm_guest_va_list(ap))); }

int darwin_vfprintf(FILE *f, const char *fmt, std::uint64_t ap) DARWIN(vfprintf);
int darwin_vfprintf(FILE *f, const char *fmt, std::uint64_t ap) { return emit_to_file(f, format_guest(fmt, mm_guest_va_list(ap))); }

int darwin_vsprintf(char *dst, const char *fmt, std::uint64_t ap) DARWIN(vsprintf);
int darwin_vsprintf(char *dst, const char *fmt, std::uint64_t ap) {
    const std::string s = format_guest(fmt, mm_guest_va_list(ap));
    std::memcpy(dst, s.c_str(), s.size() + 1);
    return static_cast<int>(s.size());
}

int darwin_vsnprintf(char *dst, std::size_t size, const char *fmt, std::uint64_t ap) DARWIN(vsnprintf);
int darwin_vsnprintf(char *dst, std::size_t size, const char *fmt, std::uint64_t ap) {
    return copy_out(dst, size, format_guest(fmt, mm_guest_va_list(ap)));
}

int darwin_vsnprintf_l(char *dst, std::size_t size, void *, const char *fmt, std::uint64_t ap) DARWIN(vsnprintf_l);
int darwin_vsnprintf_l(char *dst, std::size_t size, void *, const char *fmt, std::uint64_t ap) {
    return copy_out(dst, size, format_guest(fmt, mm_guest_va_list(ap)));
}

int darwin_vsnprintf_chk(char *dst, std::size_t size, int, std::size_t, const char *fmt, std::uint64_t ap) DARWIN(__vsnprintf_chk);
int darwin_vsnprintf_chk(char *dst, std::size_t size, int, std::size_t, const char *fmt, std::uint64_t ap) {
    return copy_out(dst, size, format_guest(fmt, mm_guest_va_list(ap)));
}

int darwin_vsprintf_chk(char *dst, int, std::size_t, const char *fmt, std::uint64_t ap) DARWIN(__vsprintf_chk);
int darwin_vsprintf_chk(char *dst, int, std::size_t, const char *fmt, std::uint64_t ap) {
    const std::string s = format_guest(fmt, mm_guest_va_list(ap));
    std::memcpy(dst, s.c_str(), s.size() + 1);
    return static_cast<int>(s.size());
}

int darwin_vasprintf(char **out, const char *fmt, std::uint64_t ap) DARWIN(vasprintf);
int darwin_vasprintf(char **out, const char *fmt, std::uint64_t ap) {
    const std::string s = format_guest(fmt, mm_guest_va_list(ap));
    *out = static_cast<char *>(std::malloc(s.size() + 1));
    if (!*out) return -1;
    std::memcpy(*out, s.c_str(), s.size() + 1);
    return static_cast<int>(s.size());
}

int darwin_vasprintf_l(char **out, void *, const char *fmt, std::uint64_t ap) DARWIN(vasprintf_l);
int darwin_vasprintf_l(char **out, void *, const char *fmt, std::uint64_t ap) { return darwin_vasprintf(out, fmt, ap); }

int darwin_sscanf(const char *in, const char *fmt, ...) DARWIN(sscanf);
int darwin_sscanf(const char *in, const char *fmt, ...) { return scan_guest(in, fmt, mm_guest_va_list::at_call_site(2)); }

int darwin_sscanf_l(const char *in, void *, const char *fmt, ...) DARWIN(sscanf_l);
int darwin_sscanf_l(const char *in, void *, const char *fmt, ...) { return scan_guest(in, fmt, mm_guest_va_list::at_call_site(3)); }

int darwin_vsscanf(const char *in, const char *fmt, std::uint64_t ap) DARWIN(vsscanf);
int darwin_vsscanf(const char *in, const char *fmt, std::uint64_t ap) { return scan_guest(in, fmt, mm_guest_va_list(ap)); }

int darwin_vsscanf_l(const char *in, void *, const char *fmt, std::uint64_t ap) DARWIN(vsscanf_l);
int darwin_vsscanf_l(const char *in, void *, const char *fmt, std::uint64_t ap) { return scan_guest(in, fmt, mm_guest_va_list(ap)); }

} // extern "C"

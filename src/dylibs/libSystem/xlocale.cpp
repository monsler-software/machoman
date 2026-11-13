/* <locale.h>/<xlocale.h>/<langinfo.h>: Darwin's category numbers, mask
 * bits, nl_langinfo items and struct lconv differ from glibc's, and glibc
 * lacks the BSD "_l" variants of the multibyte functions. locale_t values
 * are glibc's; everything below maps constants and routes the "_l" forms
 * through uselocale(). */
#include "internal.h"
#include "machoman/objc_host.h"

#include <clocale>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <cwchar>
#include <langinfo.h>
#include <locale.h>
#include <new>

namespace {

/* Darwin LC_* category -> glibc. */
int category_from_darwin(int c) {
    switch (c) {
    case 0: return LC_ALL;
    case 1: return LC_COLLATE;
    case 2: return LC_CTYPE;
    case 3: return LC_MONETARY;
    case 4: return LC_NUMERIC;
    case 5: return LC_TIME;
    case 6: return LC_MESSAGES;
    default: return -1;
    }
}

/* Darwin LC_*_MASK bits: COLLATE=1, CTYPE=2, MESSAGES=4, MONETARY=8, NUMERIC=16, TIME=32. */
int mask_from_darwin(int m) {
    int out = 0;
    if (m & 1)  out |= LC_COLLATE_MASK;
    if (m & 2)  out |= LC_CTYPE_MASK;
    if (m & 4)  out |= LC_MESSAGES_MASK;
    if (m & 8)  out |= LC_MONETARY_MASK;
    if (m & 16) out |= LC_NUMERIC_MASK;
    if (m & 32) out |= LC_TIME_MASK;
    return out;
}

/* Runs `fn` with `loc` as the calling thread's locale. */
template <typename F> auto with_locale(locale_t loc, F fn) {
    /* Darwin accepts NULL as the current thread locale. glibc's *_l
     * functions dereference it, and also reject LC_GLOBAL_LOCALE. Calling
     * the ordinary function under the selected thread locale handles both. */
    if (!loc) return fn();
    const locale_t old = uselocale(loc);
    auto r = fn();
    const int result_errno = errno;
    uselocale(old);
    errno = result_errno;
    return r;
}

/* Darwin struct lconv (int_* fields ordered differently from glibc). */
struct darwin_lconv {
    char *decimal_point, *thousands_sep, *grouping, *int_curr_symbol, *currency_symbol, *mon_decimal_point,
        *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space, p_sign_posn,
        n_sign_posn, int_p_cs_precedes, int_n_cs_precedes, int_p_sep_by_space, int_n_sep_by_space, int_p_sign_posn,
        int_n_sign_posn;
};

thread_local darwin_lconv t_lconv;

darwin_lconv *convert_lconv(const lconv *h) {
    darwin_lconv &d = t_lconv;
    d.decimal_point = h->decimal_point; d.thousands_sep = h->thousands_sep; d.grouping = h->grouping;
    d.int_curr_symbol = h->int_curr_symbol; d.currency_symbol = h->currency_symbol;
    d.mon_decimal_point = h->mon_decimal_point; d.mon_thousands_sep = h->mon_thousands_sep;
    d.mon_grouping = h->mon_grouping; d.positive_sign = h->positive_sign; d.negative_sign = h->negative_sign;
    d.int_frac_digits = h->int_frac_digits; d.frac_digits = h->frac_digits;
    d.p_cs_precedes = h->p_cs_precedes; d.p_sep_by_space = h->p_sep_by_space;
    d.n_cs_precedes = h->n_cs_precedes; d.n_sep_by_space = h->n_sep_by_space;
    d.p_sign_posn = h->p_sign_posn; d.n_sign_posn = h->n_sign_posn;
    d.int_p_cs_precedes = h->int_p_cs_precedes; d.int_n_cs_precedes = h->int_n_cs_precedes;
    d.int_p_sep_by_space = h->int_p_sep_by_space; d.int_n_sep_by_space = h->int_n_sep_by_space;
    d.int_p_sign_posn = h->int_p_sign_posn; d.int_n_sign_posn = h->int_n_sign_posn;
    return &d;
}

/* Darwin nl_langinfo items (<langinfo.h>, sequential) -> glibc. */
nl_item langinfo_item_from_darwin(int item) {
    static const nl_item table[] = {
        CODESET, D_T_FMT, D_FMT, T_FMT, T_FMT_AMPM, AM_STR, PM_STR,
        DAY_1, DAY_2, DAY_3, DAY_4, DAY_5, DAY_6, DAY_7,
        ABDAY_1, ABDAY_2, ABDAY_3, ABDAY_4, ABDAY_5, ABDAY_6, ABDAY_7,
        MON_1, MON_2, MON_3, MON_4, MON_5, MON_6, MON_7, MON_8, MON_9, MON_10, MON_11, MON_12,
        ABMON_1, ABMON_2, ABMON_3, ABMON_4, ABMON_5, ABMON_6, ABMON_7, ABMON_8, ABMON_9, ABMON_10, ABMON_11, ABMON_12,
        ERA, ERA_D_FMT, ERA_D_T_FMT, ERA_T_FMT, ALT_DIGITS, RADIXCHAR, THOUSEP, YESEXPR, NOEXPR, YESSTR, NOSTR, CRNCYSTR,
        D_FMT, /* D_MD_ORDER: BSD-only; the date format tells the order */
    };
    if (item < 0 || static_cast<std::size_t>(item) >= sizeof table / sizeof table[0]) return CODESET;
    return table[item];
}

} // namespace

extern "C" {

char *darwin_setlocale(int category, const char *name) DARWIN(setlocale);
char *darwin_setlocale(int category, const char *name) {
    const int c = category_from_darwin(category);
    if (c < 0) return nullptr;
    /* Darwin's default locale is "C" with UTF-8 ctype, and Live only ever
     * asks for "" or "C": use C.UTF-8 so mbrtowc & co. speak UTF-8. */
    if (name && (*name == 0 || std::strcmp(name, "en_US.UTF-8") == 0)) name = "C.UTF-8";
    return std::setlocale(c, name);
}

locale_t darwin_newlocale(int mask, const char *name, locale_t base) DARWIN(newlocale);
locale_t darwin_newlocale(int mask, const char *name, locale_t base) {
    if (name && (*name == 0 || std::strcmp(name, "en_US.UTF-8") == 0)) name = "C.UTF-8";
    return newlocale(mask_from_darwin(mask), name, base);
}

/* duplocale/freelocale/uselocale/querylocale: glibc's are ABI-compatible (opaque pointers). */

int darwin_mb_cur_max() DARWIN(___mb_cur_max);
int darwin_mb_cur_max() { return static_cast<int>(MB_CUR_MAX); }
int darwin_mb_cur_max_l(locale_t loc) DARWIN(___mb_cur_max_l);
int darwin_mb_cur_max_l(locale_t loc) { return with_locale(loc, [] { return static_cast<int>(MB_CUR_MAX); }); }

darwin_lconv *darwin_localeconv() DARWIN(localeconv);
darwin_lconv *darwin_localeconv() { return convert_lconv(std::localeconv()); }
darwin_lconv *darwin_localeconv_l(locale_t loc) DARWIN(localeconv_l);
darwin_lconv *darwin_localeconv_l(locale_t loc) { return with_locale(loc, [] { return convert_lconv(std::localeconv()); }); }

char *darwin_nl_langinfo(int item) DARWIN(nl_langinfo);
char *darwin_nl_langinfo(int item) { return nl_langinfo(langinfo_item_from_darwin(item)); }
char *darwin_nl_langinfo_l(int item, locale_t loc) DARWIN(nl_langinfo_l);
char *darwin_nl_langinfo_l(int item, locale_t loc) {
    return with_locale(loc, [&] { return nl_langinfo(langinfo_item_from_darwin(item)); });
}

/* libc++ uses a NULL locale for its classic time facets. Darwin treats
 * that as the current locale, while glibc's explicit *_l entry points
 * dereference it. struct tm has the same layout on these 64-bit targets. */
std::size_t darwin_strftime_l(char *, std::size_t, const char *, const std::tm *, locale_t) DARWIN(strftime_l);
std::size_t darwin_strftime_l(char *text, std::size_t size, const char *format, const std::tm *time, locale_t loc) {
    return with_locale(loc, [&] { return std::strftime(text, size, format, time); });
}
std::size_t darwin_wcsftime_l(wchar_t *, std::size_t, const wchar_t *, const std::tm *, locale_t) DARWIN(wcsftime_l);
std::size_t darwin_wcsftime_l(wchar_t *text, std::size_t size, const wchar_t *format, const std::tm *time, locale_t loc) {
    return with_locale(loc, [&] { return std::wcsftime(text, size, format, time); });
}
char *darwin_strptime_l(const char *, const char *, std::tm *, locale_t) DARWIN(strptime_l);
char *darwin_strptime_l(const char *text, const char *format, std::tm *time, locale_t loc) {
    return with_locale(loc, [&] { return ::strptime(text, format, time); });
}

/* Numeric conversions: NULL locales are used by Live's preferences parser. */
double darwin_strtod_l(const char *s, char **end, locale_t loc) DARWIN(strtod_l);
double darwin_strtod_l(const char *s, char **end, locale_t loc) {
    return with_locale(loc, [&] { return std::strtod(s, end); });
}
float darwin_strtof_l(const char *s, char **end, locale_t loc) DARWIN(strtof_l);
float darwin_strtof_l(const char *s, char **end, locale_t loc) {
    return with_locale(loc, [&] { return std::strtof(s, end); });
}
long darwin_strtol_l(const char *s, char **end, int base, locale_t loc) DARWIN(strtol_l);
long darwin_strtol_l(const char *s, char **end, int base, locale_t loc) {
    return with_locale(loc, [&] { return std::strtol(s, end, base); });
}
unsigned long darwin_strtoul_l(const char *s, char **end, int base, locale_t loc) DARWIN(strtoul_l);
unsigned long darwin_strtoul_l(const char *s, char **end, int base, locale_t loc) {
    return with_locale(loc, [&] { return std::strtoul(s, end, base); });
}
long long darwin_strtoll_l(const char *s, char **end, int base, locale_t loc) DARWIN(strtoll_l);
long long darwin_strtoll_l(const char *s, char **end, int base, locale_t loc) {
    return with_locale(loc, [&] { return std::strtoll(s, end, base); });
}
unsigned long long darwin_strtoull_l(const char *s, char **end, int base, locale_t loc) DARWIN(strtoull_l);
unsigned long long darwin_strtoull_l(const char *s, char **end, int base, locale_t loc) {
    return with_locale(loc, [&] { return std::strtoull(s, end, base); });
}

/* Multibyte "_l" forms glibc does not have. */
int darwin_mbtowc_l(wchar_t *pwc, const char *s, std::size_t n, locale_t loc) DARWIN(mbtowc_l);
int darwin_mbtowc_l(wchar_t *pwc, const char *s, std::size_t n, locale_t loc) { return with_locale(loc, [&] { return std::mbtowc(pwc, s, n); }); }
int darwin_wctomb_l(char *s, wchar_t wc, locale_t loc) DARWIN(wctomb_l);
int darwin_wctomb_l(char *s, wchar_t wc, locale_t loc) { return with_locale(loc, [&] { return std::wctomb(s, wc); }); }
int darwin_mblen_l(const char *s, std::size_t n, locale_t loc) DARWIN(mblen_l);
int darwin_mblen_l(const char *s, std::size_t n, locale_t loc) { return with_locale(loc, [&] { return std::mblen(s, n); }); }
std::size_t darwin_mbrtowc_l(wchar_t *pwc, const char *s, std::size_t n, mbstate_t *ps, locale_t loc) DARWIN(mbrtowc_l);
std::size_t darwin_mbrtowc_l(wchar_t *pwc, const char *s, std::size_t n, mbstate_t *ps, locale_t loc) {
    return with_locale(loc, [&] { return std::mbrtowc(pwc, s, n, ps); });
}
std::size_t darwin_wcrtomb_l(char *s, wchar_t wc, mbstate_t *ps, locale_t loc) DARWIN(wcrtomb_l);
std::size_t darwin_wcrtomb_l(char *s, wchar_t wc, mbstate_t *ps, locale_t loc) { return with_locale(loc, [&] { return std::wcrtomb(s, wc, ps); }); }
std::size_t darwin_mbrlen_l(const char *s, std::size_t n, mbstate_t *ps, locale_t loc) DARWIN(mbrlen_l);
std::size_t darwin_mbrlen_l(const char *s, std::size_t n, mbstate_t *ps, locale_t loc) { return with_locale(loc, [&] { return std::mbrlen(s, n, ps); }); }
std::size_t darwin_mbsrtowcs_l(wchar_t *dst, const char **src, std::size_t len, mbstate_t *ps, locale_t loc) DARWIN(mbsrtowcs_l);
std::size_t darwin_mbsrtowcs_l(wchar_t *dst, const char **src, std::size_t len, mbstate_t *ps, locale_t loc) {
    return with_locale(loc, [&] { return std::mbsrtowcs(dst, src, len, ps); });
}
std::size_t darwin_mbsnrtowcs_l(wchar_t *dst, const char **src, std::size_t nms, std::size_t len, mbstate_t *ps, locale_t loc) DARWIN(mbsnrtowcs_l);
std::size_t darwin_mbsnrtowcs_l(wchar_t *dst, const char **src, std::size_t nms, std::size_t len, mbstate_t *ps, locale_t loc) {
    return with_locale(loc, [&] { return mbsnrtowcs(dst, src, nms, len, ps); });
}
std::size_t darwin_wcsrtombs_l(char *dst, const wchar_t **src, std::size_t len, mbstate_t *ps, locale_t loc) DARWIN(wcsrtombs_l);
std::size_t darwin_wcsrtombs_l(char *dst, const wchar_t **src, std::size_t len, mbstate_t *ps, locale_t loc) {
    return with_locale(loc, [&] { return std::wcsrtombs(dst, src, len, ps); });
}
std::size_t darwin_wcsnrtombs_l(char *dst, const wchar_t **src, std::size_t nwc, std::size_t len, mbstate_t *ps, locale_t loc) DARWIN(wcsnrtombs_l);
std::size_t darwin_wcsnrtombs_l(char *dst, const wchar_t **src, std::size_t nwc, std::size_t len, mbstate_t *ps, locale_t loc) {
    return with_locale(loc, [&] { return wcsnrtombs(dst, src, nwc, len, ps); });
}
wint_t darwin_btowc_l(int c, locale_t loc) DARWIN(btowc_l);
wint_t darwin_btowc_l(int c, locale_t loc) { return with_locale(loc, [&] { return std::btowc(c); }); }
int darwin_wctob_l(wint_t c, locale_t loc) DARWIN(wctob_l);
int darwin_wctob_l(wint_t c, locale_t loc) { return with_locale(loc, [&] { return std::wctob(c); }); }
std::size_t darwin_mbstowcs_l(wchar_t *dst, const char *src, std::size_t n, locale_t loc) DARWIN(mbstowcs_l);
std::size_t darwin_mbstowcs_l(wchar_t *dst, const char *src, std::size_t n, locale_t loc) { return with_locale(loc, [&] { return std::mbstowcs(dst, src, n); }); }
std::size_t darwin_wcstombs_l(char *dst, const wchar_t *src, std::size_t n, locale_t loc) DARWIN(wcstombs_l);
std::size_t darwin_wcstombs_l(char *dst, const wchar_t *src, std::size_t n, locale_t loc) { return with_locale(loc, [&] { return std::wcstombs(dst, src, n); }); }

} // extern "C"

/* ---- <ctype.h>: Darwin's inline is*() read _DefaultRuneLocale directly ---- */

/* struct _RuneLocale from <runetype.h> (namespace scope: a type with
 * internal linkage would make the exported variables internal too). */
struct rune_entry { std::int32_t min, max, map; std::uint32_t *types; };
struct rune_range { int nranges; rune_entry *ranges; };
struct rune_locale {
    char magic[8];
    char encoding[32];
    void *sgetrune, *sputrune;
    std::int32_t invalid_rune;
    std::uint32_t runetype[256];
    std::int32_t maplower[256];
    std::int32_t mapupper[256];
    rune_range runetype_ext, maplower_ext, mapupper_ext;
    void *variable;
    int variable_len;
    int ncharclasses;
    void *charclasses;
};
static_assert(offsetof(rune_locale, runetype) == 60, "_RuneLocale layout");

namespace {

constexpr std::uint32_t CT_A = 0x100, CT_C = 0x200, CT_D = 0x400, CT_G = 0x800, CT_L = 0x1000, CT_P = 0x2000,
                        CT_S = 0x4000, CT_U = 0x8000, CT_X = 0x10000, CT_B = 0x20000, CT_R = 0x40000;

std::uint32_t c_runetype(int c) {
    std::uint32_t t = 0;
    if (c >= 'A' && c <= 'Z') t |= CT_A | CT_U | CT_G | CT_R;
    if (c >= 'a' && c <= 'z') t |= CT_A | CT_L | CT_G | CT_R;
    if (c >= '0' && c <= '9') t |= CT_D | CT_G | CT_R;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) t |= CT_X;
    if (c == ' ') t |= CT_S | CT_B | CT_R;
    if (c == '\t') t |= CT_S | CT_B | CT_C;
    if (c == '\n' || c == '\v' || c == '\f' || c == '\r') t |= CT_S | CT_C;
    if ((c >= 0 && c < 0x20 && !(t & CT_S)) || c == 0x7f) t |= CT_C;
    if (c > 0x20 && c < 0x7f && !(t & (CT_A | CT_D))) t |= CT_P | CT_G | CT_R;
    /* screen width 1 for printable */
    if (t & CT_R) t |= 1u << 30;
    return t;
}

rune_locale make_default_rune_locale() {
    rune_locale r{};
    std::memcpy(r.magic, "RuneMagA", 8);
    std::memcpy(r.encoding, "NONE", 5);
    r.invalid_rune = 0xFFFD;
    for (int c = 0; c < 256; c++) {
        r.runetype[c] = c < 128 ? c_runetype(c) : 0;
        r.maplower[c] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        r.mapupper[c] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    return r;
}

} // namespace

extern rune_locale darwin_default_rune_locale __asm__("__DefaultRuneLocale");
__attribute__((visibility("default"))) rune_locale darwin_default_rune_locale = make_default_rune_locale();
extern rune_locale *darwin_current_rune_locale __asm__("__CurrentRuneLocale");
__attribute__((visibility("default"))) rune_locale *darwin_current_rune_locale = &darwin_default_rune_locale;

extern "C" {

/* Out-of-line forms for c >= 128 or the locale-aware variants. */
int darwin_maskrune(int c, unsigned long f) DARWIN(__maskrune);
int darwin_maskrune(int c, unsigned long f) {
    return static_cast<int>((c < 0 || c >= 256 ? 0 : darwin_default_rune_locale.runetype[c]) & f);
}
int darwin_maskrune_l(int c, unsigned long f, locale_t) DARWIN(__maskrune_l);
int darwin_maskrune_l(int c, unsigned long f, locale_t) { return darwin_maskrune(c, f); }
int darwin_tolower_rune(int c) DARWIN(__tolower);
int darwin_tolower_rune(int c) { return c >= 0 && c < 256 ? darwin_default_rune_locale.maplower[c] : c; }
int darwin_toupper_rune(int c) DARWIN(__toupper);
int darwin_toupper_rune(int c) { return c >= 0 && c < 256 ? darwin_default_rune_locale.mapupper[c] : c; }
int darwin_tolower_rune_l(int c, locale_t) DARWIN(__tolower_l);
int darwin_tolower_rune_l(int c, locale_t) { return darwin_tolower_rune(c); }
int darwin_toupper_rune_l(int c, locale_t) DARWIN(__toupper_l);
int darwin_toupper_rune_l(int c, locale_t) { return darwin_toupper_rune(c); }
int darwin_istype_l(int c, unsigned long f, locale_t) DARWIN(__istype_l);
int darwin_istype_l(int c, unsigned long f, locale_t) { return darwin_maskrune(c, f) != 0; }

} // extern "C"

/* ---- os_log: messages go to stderr when MACHOMAN_OSLOG is set ----------- */

namespace {
struct os_log_object {
    mm_objc_object object;
    std::string subsystem;
    std::string category;
};
void os_log_dealloc(mm_id self, mm_SEL) {
    reinterpret_cast<os_log_object *>(self)->~os_log_object();
    mm_objc_free_instance(self);
}
mm_objc_class *os_log_class() {
    static mm_objc_class storage{}, metaclass{};
    static auto *cls = [] {
        auto *c = mm_objc_define_class("OS_os_log", mm_objc_class_named("NSObject"),
            sizeof(os_log_object), &storage, &metaclass);
        mm_objc_add_method(c, "dealloc", reinterpret_cast<void *>(&os_log_dealloc), "v16@0:8");
        return c;
    }();
    return cls;
}
/* Allocated handles inherit NSObject's ARC methods and destroy their owned
 * strings on the final release. The exported static handles remain alive. */
bool is_log_object(const void *p) {
    if (!p) return false;
    mm_objc_class *isa;
    std::memcpy(&isa, p, sizeof isa);
    return isa == os_log_class();
}
bool oslog_enabled() { static const bool on = std::getenv("MACHOMAN_OSLOG") != nullptr; return on; }
} // namespace

extern "C" {

/* Darwin exports the objects themselves: OS_LOG_DEFAULT is their address. */
__attribute__((visibility("default"))) os_log_object darwin_os_log_default __asm__("__os_log_default"){{os_log_class()}, "default", ""};
__attribute__((visibility("default"))) os_log_object darwin_os_log_disabled __asm__("__os_log_disabled"){{os_log_class()}, "disabled", ""};

os_log_object *darwin_os_log_create(const char *subsystem, const char *category) DARWIN(os_log_create);
os_log_object *darwin_os_log_create(const char *subsystem, const char *category) {
    auto *cls = os_log_class();
    auto *storage = mm_objc_alloc(cls, 0);
    return new (storage) os_log_object{{cls}, subsystem ? subsystem : "", category ? category : ""};
}
int darwin_os_log_type_enabled(os_log_object *, std::uint8_t) DARWIN(os_log_type_enabled);
int darwin_os_log_type_enabled(os_log_object *log, std::uint8_t) { return log != &darwin_os_log_disabled && oslog_enabled(); }

/* _os_log_impl(dso, log, type, format, buf, size). The buffer holds the
 * arguments Apple's encoder produced: a summary byte, a count byte, then
 * for each argument a descriptor byte, a size byte and its bytes (for %s
 * and %@ the bytes are a pointer). Decoding it is what makes the message
 * readable, since the format alone is just "%{private}s". */
void darwin_os_log_impl(void *, os_log_object *log, std::uint8_t type, const char *format, std::uint8_t *buf,
                        std::uint32_t buflen) DARWIN(_os_log_impl);
void darwin_os_log_impl(void *, os_log_object *log, std::uint8_t type, const char *format, std::uint8_t *buf,
                        std::uint32_t buflen) {
    if (!oslog_enabled() || log == &darwin_os_log_disabled) return;
    std::string out;
    const char *f = format ? format : "";
    std::uint32_t at = buf && buflen >= 2 ? 2u : buflen; /* skip summary and count */
    auto next_arg = [&](std::uint64_t &value, std::uint8_t &size) {
        if (!buf || at + 2 > buflen) return false;
        at++;                       /* descriptor */
        size = buf[at++];
        if (at + size > buflen || size > 8) return false;
        value = 0;
        std::memcpy(&value, buf + at, size);
        at += size;
        return true;
    };
    for (std::size_t i = 0; f[i];) {
        if (f[i] != '%') { out += f[i++]; continue; }
        if (f[i + 1] == '%') { out += '%'; i += 2; continue; }
        std::size_t j = i + 1;
        if (f[j] == '{') { while (f[j] && f[j] != '}') j++; if (f[j]) j++; } /* %{private}, %{public}, ... */
        while (f[j] && !std::strchr("diuoxXeEfgGcsSpP@", f[j])) j++;
        const char conv = f[j];
        std::uint64_t value = 0;
        std::uint8_t size = 0;
        char tmp[64];
        if (!conv || !next_arg(value, size)) { out += "<?>"; i = conv ? j + 1 : i + 1; continue; }
        switch (conv) {
        case 's': case 'S':
            out += value ? reinterpret_cast<const char *>(value) : "(null)";
            break;
        case '@': case 'p': case 'P':
            std::snprintf(tmp, sizeof tmp, "%p", reinterpret_cast<void *>(value));
            out += tmp;
            break;
        case 'f': case 'e': case 'E': case 'g': case 'G': {
            double d = 0;
            std::memcpy(&d, &value, 8);
            std::snprintf(tmp, sizeof tmp, "%g", d);
            out += tmp;
            break;
        }
        case 'c':
            out += static_cast<char>(value);
            break;
        default: {
            std::int64_t v = static_cast<std::int64_t>(value);
            if (size == 4) v = static_cast<std::int32_t>(value);
            std::snprintf(tmp, sizeof tmp, conv == 'u' ? "%llu" : conv == 'x' ? "%llx" : "%lld",
                          static_cast<long long>(v));
            out += tmp;
            break;
        }
        }
        i = j + 1;
    }
    std::fprintf(stderr, "os_log[%s%s%s type=%u]: %s\n", log ? log->subsystem.c_str() : "",
                 log && !log->category.empty() ? ":" : "", log ? log->category.c_str() : "",
                 type, out.c_str());
}
void darwin_os_log_error_impl(void *dso, os_log_object *log, std::uint8_t type, const char *format, std::uint8_t *buf, std::uint32_t n) DARWIN(_os_log_error_impl);
void darwin_os_log_error_impl(void *dso, os_log_object *log, std::uint8_t type, const char *format, std::uint8_t *buf, std::uint32_t n) {
    darwin_os_log_impl(dso, log, type, format, buf, n);
}
void darwin_os_log_fault_impl(void *dso, os_log_object *log, std::uint8_t type, const char *format, std::uint8_t *buf, std::uint32_t n) DARWIN(_os_log_fault_impl);
void darwin_os_log_fault_impl(void *dso, os_log_object *log, std::uint8_t type, const char *format, std::uint8_t *buf, std::uint32_t n) {
    darwin_os_log_impl(dso, log, type, format, buf, n);
}
void darwin_os_log_debug_impl(void *dso, os_log_object *log, std::uint8_t type, const char *format, std::uint8_t *buf, std::uint32_t n) DARWIN(_os_log_debug_impl);
void darwin_os_log_debug_impl(void *dso, os_log_object *log, std::uint8_t type, const char *format, std::uint8_t *buf, std::uint32_t n) {
    darwin_os_log_impl(dso, log, type, format, buf, n);
}
void darwin_os_release(void *p) DARWIN(os_release);
void darwin_os_release(void *p) {
    if (is_log_object(p)) mm_objc_release(reinterpret_cast<mm_id>(p));
}
void *darwin_os_retain(void *p) DARWIN(os_retain);
void *darwin_os_retain(void *p) {
    if (is_log_object(p)) mm_objc_retain(reinterpret_cast<mm_id>(p));
    return p;
}
std::uint64_t darwin_os_signpost_id_generate(os_log_object *) DARWIN(os_signpost_id_generate);
std::uint64_t darwin_os_signpost_id_generate(os_log_object *) { return 1; }
int darwin_os_signpost_enabled(os_log_object *) DARWIN(os_signpost_enabled);
int darwin_os_signpost_enabled(os_log_object *) { return 0; }
void darwin_os_signpost_emit_with_name_impl(void *, os_log_object *, std::uint8_t, std::uint64_t, const char *, const char *, std::uint8_t *, std::uint32_t) DARWIN(_os_signpost_emit_with_name_impl);
void darwin_os_signpost_emit_with_name_impl(void *, os_log_object *, std::uint8_t, std::uint64_t, const char *, const char *, std::uint8_t *, std::uint32_t) {}

} // extern "C"

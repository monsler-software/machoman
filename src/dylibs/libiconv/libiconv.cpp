/* /usr/lib/libiconv.2.dylib.
 *
 * Apple ships GNU libiconv under two sets of names: the plain POSIX ones
 * and `libiconv_`-prefixed aliases. glibc has iconv built in, and its
 * conversion descriptors and error conventions are the same, so every
 * entry point here is that one call under the other name.
 *
 * The one thing that differs is the descriptor's error value: both return
 * (iconv_t)-1, so nothing has to be translated. */
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <iconv.h>
#include <langinfo.h>
#include <locale.h>

#define DARWIN(name) __attribute__((visibility("default"))) __asm__("_" #name)

extern "C" {

iconv_t mm_iconv_open(const char *to, const char *from) DARWIN(iconv_open);
iconv_t mm_iconv_open(const char *to, const char *from) { return ::iconv_open(to, from); }

std::size_t mm_iconv(iconv_t cd, char **in, std::size_t *in_left, char **out, std::size_t *out_left) DARWIN(iconv);
std::size_t mm_iconv(iconv_t cd, char **in, std::size_t *in_left, char **out, std::size_t *out_left) {
    return ::iconv(cd, in, in_left, out, out_left);
}

int mm_iconv_close(iconv_t cd) DARWIN(iconv_close);
int mm_iconv_close(iconv_t cd) { return ::iconv_close(cd); }

/* The libiconv_-prefixed aliases are the same functions. */
iconv_t mm_libiconv_open(const char *to, const char *from) DARWIN(libiconv_open);
iconv_t mm_libiconv_open(const char *to, const char *from) { return ::iconv_open(to, from); }
std::size_t mm_libiconv(iconv_t cd, char **in, std::size_t *in_left, char **out, std::size_t *out_left) DARWIN(libiconv);
std::size_t mm_libiconv(iconv_t cd, char **in, std::size_t *in_left, char **out, std::size_t *out_left) {
    return ::iconv(cd, in, in_left, out, out_left);
}
int mm_libiconv_close(iconv_t cd) DARWIN(libiconv_close);
int mm_libiconv_close(iconv_t cd) { return ::iconv_close(cd); }

/* Relocation is a no-op: there is no installation prefix to move. */
void mm_libiconv_set_relocation_prefix(const char *, const char *) DARWIN(libiconv_set_relocation_prefix);
void mm_libiconv_set_relocation_prefix(const char *, const char *) {}

/* glibc has no canonicalisation call; the name a program passed already
 * works with iconv_open, so it is its own canonical form. */
const char *mm_iconv_canonicalize(const char *name) DARWIN(iconv_canonicalize);
const char *mm_iconv_canonicalize(const char *name) { return name; }

const char *mm_locale_charset() DARWIN(locale_charset);
const char *mm_locale_charset() {
    const char *cs = nl_langinfo(CODESET);
    return cs && *cs ? cs : "UTF-8";
}

/* iconvlist walks the installed converters; glibc does not publish that
 * list through an API, so the walk visits nothing and the callback is
 * simply not called — which is what a caller handles when a build has no
 * converters to report. */
void mm_iconvlist(int (*)(unsigned int, const char *const *, void *), void *) DARWIN(iconvlist);
void mm_iconvlist(int (*)(unsigned int, const char *const *, void *), void *) {}

} // extern "C"

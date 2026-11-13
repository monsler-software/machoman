#include "internal.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/random.h>

namespace {
void generate(unsigned char *out) {
    std::size_t offset = 0;
    while (offset < 16) {
        const auto count = getrandom(out + offset, 16 - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { std::perror("uuid_generate: getrandom"); std::abort(); }
        offset += static_cast<std::size_t>(count);
    }
    out[6] = static_cast<unsigned char>((out[6] & 0x0f) | 0x40);
    out[8] = static_cast<unsigned char>((out[8] & 0x3f) | 0x80);
}
void unparse(const unsigned char *uuid, char *out, bool upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    unsigned pos = 0;
    for (unsigned i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[pos++] = '-';
        out[pos++] = digits[uuid[i] >> 4];
        out[pos++] = digits[uuid[i] & 15];
    }
    out[pos] = 0;
}
int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}
extern "C" {
void darwin_uuid_generate(unsigned char *) DARWIN(uuid_generate);
void darwin_uuid_generate(unsigned char *out) { generate(out); }
void darwin_uuid_generate_random(unsigned char *) DARWIN(uuid_generate_random);
void darwin_uuid_generate_random(unsigned char *out) { generate(out); }
void darwin_uuid_clear(unsigned char *) DARWIN(uuid_clear);
void darwin_uuid_clear(unsigned char *out) { std::memset(out, 0, 16); }
void darwin_uuid_copy(unsigned char *, const unsigned char *) DARWIN(uuid_copy);
void darwin_uuid_copy(unsigned char *out, const unsigned char *in) { std::memcpy(out, in, 16); }
int darwin_uuid_compare(const unsigned char *, const unsigned char *) DARWIN(uuid_compare);
int darwin_uuid_compare(const unsigned char *a, const unsigned char *b) { return std::memcmp(a, b, 16); }
int darwin_uuid_is_null(const unsigned char *) DARWIN(uuid_is_null);
int darwin_uuid_is_null(const unsigned char *uuid) {
    unsigned bits = 0;
    for (unsigned i = 0; i < 16; ++i) bits |= uuid[i];
    return bits == 0;
}
int darwin_uuid_parse(const char *, unsigned char *) DARWIN(uuid_parse);
int darwin_uuid_parse(const char *text, unsigned char *out) {
    if (!text || std::strlen(text) != 36) return -1;
    unsigned char value[16];
    unsigned pos = 0;
    for (unsigned i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) if (text[pos++] != '-') return -1;
        const int hi = hex(text[pos++]), lo = hex(text[pos++]);
        if (hi < 0 || lo < 0) return -1;
        value[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    std::memcpy(out, value, 16);
    return 0;
}
void darwin_uuid_unparse(const unsigned char *, char *) DARWIN(uuid_unparse);
void darwin_uuid_unparse(const unsigned char *uuid, char *out) { unparse(uuid, out, true); }
void darwin_uuid_unparse_upper(const unsigned char *, char *) DARWIN(uuid_unparse_upper);
void darwin_uuid_unparse_upper(const unsigned char *uuid, char *out) { unparse(uuid, out, true); }
void darwin_uuid_unparse_lower(const unsigned char *, char *) DARWIN(uuid_unparse_lower);
void darwin_uuid_unparse_lower(const unsigned char *uuid, char *out) { unparse(uuid, out, false); }
}

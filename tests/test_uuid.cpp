#include <cstdio>
#include <cstring>

extern "C" {
void generate(unsigned char *) __asm__("_uuid_generate");
void generate_random(unsigned char *) __asm__("_uuid_generate_random");
void clear(unsigned char *) __asm__("_uuid_clear");
void copy(unsigned char *, const unsigned char *) __asm__("_uuid_copy");
int compare(const unsigned char *, const unsigned char *) __asm__("_uuid_compare");
int is_null(const unsigned char *) __asm__("_uuid_is_null");
int parse(const char *, unsigned char *) __asm__("_uuid_parse");
void upper(const unsigned char *, char *) __asm__("_uuid_unparse");
void lower(const unsigned char *, char *) __asm__("_uuid_unparse_lower");
}
namespace {
int failures = 0;
void check(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
}
int main() {
    unsigned char a[16], b[16], c[16];
    generate(a); generate_random(b);
    check((a[6] >> 4) == 4 && (a[8] & 0xc0) == 0x80, "RFC 4122 version and variant");
    check(compare(a, b) != 0 && !is_null(a), "random UUIDs differ");
    copy(c, a); check(compare(a, c) == 0, "copy preserves bytes");
    clear(c); check(is_null(c), "clear creates null UUID");
    const unsigned char expected[16] = {0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x46, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    check(parse("00112233-4455-4677-8899-aAbBcCdDeEfF", c) == 0 && !std::memcmp(c, expected, 16), "mixed-case UUID parsing");
    char text[37];
    upper(c, text); check(!std::strcmp(text, "00112233-4455-4677-8899-AABBCCDDEEFF"), "Darwin default uppercase spelling");
    lower(c, text); check(!std::strcmp(text, "00112233-4455-4677-8899-aabbccddeeff"), "lowercase spelling");
    check(parse("00112233_4455-4677-8899-aabbccddeeff", c) == -1 && !std::memcmp(c, expected, 16), "invalid separators leave destination unchanged");
    check(parse("invalid", c) == -1, "invalid length rejected");
    check(parse("00112233-4455-4677-8899-aabbccddeefz", c) == -1, "invalid hex rejected");
    upper(a, text); check(parse(text, c) == 0 && compare(a, c) == 0, "generated UUID round trip");
    return failures ? 1 : 0;
}

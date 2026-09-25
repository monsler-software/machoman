/* CommonCrypto (src/dylibs/libSystem/commoncrypto.cpp) against the
 * published test vectors: FIPS 197 for AES, FIPS 180-4 for SHA-256, plus a
 * CBC round trip with PKCS#7 padding, which is the shape an application
 * actually uses. The symbols carry their Darwin names, so they are
 * declared here the way a guest binds to them. */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
unsigned char *mm_CC_SHA256(const void *data, std::uint32_t len, unsigned char *md) __asm__("_CC_SHA256");
int mm_CC_SHA256_Init(void *ctx) __asm__("_CC_SHA256_Init");
int mm_CC_SHA256_Update(void *ctx, const void *data, std::uint32_t len) __asm__("_CC_SHA256_Update");
int mm_CC_SHA256_Final(unsigned char *md, void *ctx) __asm__("_CC_SHA256_Final");
std::int32_t mm_CCCrypt(std::uint32_t op, std::uint32_t alg, std::uint32_t options,
                        const void *key, std::size_t key_length, const void *iv,
                        const void *data_in, std::size_t data_in_length,
                        void *data_out, std::size_t data_out_available,
                        std::size_t *data_out_moved) __asm__("_CCCrypt");
}

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

std::string hex(const std::uint8_t *p, std::size_t n) {
    static const char *digits = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; i++) {
        s.push_back(digits[p[i] >> 4]);
        s.push_back(digits[p[i] & 15]);
    }
    return s;
}

void from_hex(const char *text, std::uint8_t *out) {
    auto nibble = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    for (std::size_t i = 0; text[i * 2]; i++)
        out[i] = static_cast<std::uint8_t>(nibble(text[i * 2]) << 4 | nibble(text[i * 2 + 1]));
}

void test_sha256() {
    std::uint8_t md[32];
    mm_CC_SHA256("abc", 3, md);
    check(hex(md, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256(\"abc\")");

    mm_CC_SHA256("", 0, md);
    check(hex(md, 32) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256(\"\")");

    /* The streaming form must agree with the one-shot one, including a
     * message that spans several blocks. */
    std::string long_message(1000, 'a');
    std::uint8_t streamed[32], oneshot[32];
    std::uint8_t ctx[128] = {};
    mm_CC_SHA256_Init(ctx);
    mm_CC_SHA256_Update(ctx, long_message.data(), 500);
    mm_CC_SHA256_Update(ctx, long_message.data() + 500, 500);
    mm_CC_SHA256_Final(streamed, ctx);
    mm_CC_SHA256(long_message.data(), 1000, oneshot);
    check(std::memcmp(streamed, oneshot, 32) == 0, "SHA-256 streamed == one-shot");
}

void test_aes_vectors() {
    /* FIPS 197 appendix C: one block, no padding, ECB. */
    struct { const char *key, *plain, *cipher; std::size_t key_len; } cases[] = {
        {"000102030405060708090a0b0c0d0e0f", "00112233445566778899aabbccddeeff", "69c4e0d86a7b0430d8cdb78070b4c55a", 16},
        {"000102030405060708090a0b0c0d0e0f1011121314151617", "00112233445566778899aabbccddeeff",
         "dda97ca4864cdfe06eaf70a0ec0d7191", 24},
        {"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", "00112233445566778899aabbccddeeff",
         "8ea2b7ca516745bfeafc49904b496089", 32},
    };
    for (const auto &c : cases) {
        std::uint8_t key[32], plain[16], expected[16], out[16];
        from_hex(c.key, key);
        from_hex(c.plain, plain);
        from_hex(c.cipher, expected);
        std::size_t moved = 0;
        const std::int32_t status = mm_CCCrypt(0 /* encrypt */, 0 /* AES */, 2 /* ECB */, key, c.key_len, nullptr,
                                               plain, sizeof plain, out, sizeof out, &moved);
        check(status == 0 && moved == 16, "AES ECB encrypt returns success");
        check(std::memcmp(out, expected, 16) == 0, "AES ECB encrypt matches FIPS 197");

        std::uint8_t back[16];
        check(mm_CCCrypt(1 /* decrypt */, 0, 2, key, c.key_len, nullptr, expected, 16, back, sizeof back, &moved) == 0,
              "AES ECB decrypt returns success");
        check(std::memcmp(back, plain, 16) == 0, "AES ECB decrypt matches FIPS 197");
    }
}

void test_cbc_round_trip() {
    const std::uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const std::uint8_t iv[16] = {9, 9, 9, 9, 8, 8, 8, 8, 7, 7, 7, 7, 6, 6, 6, 6};
    const std::string message = "an interface description, 37 bytes...";
    std::uint8_t cipher[64] = {}, plain[64] = {};
    std::size_t encrypted = 0, decrypted = 0;

    check(mm_CCCrypt(0, 0, 1 /* PKCS#7 */, key, sizeof key, iv, message.data(), message.size(),
                     cipher, sizeof cipher, &encrypted) == 0, "AES CBC encrypt returns success");
    check(encrypted == 48, "PKCS#7 pads 37 bytes to three blocks");
    check(mm_CCCrypt(1, 0, 1, key, sizeof key, iv, cipher, encrypted, plain, sizeof plain, &decrypted) == 0,
          "AES CBC decrypt returns success");
    check(decrypted == message.size(), "padding is stripped on decrypt");
    check(std::memcmp(plain, message.data(), message.size()) == 0, "AES CBC round trip");

    /* A short output buffer is reported, not overrun. */
    std::uint8_t small[16];
    std::size_t needed = 0;
    check(mm_CCCrypt(0, 0, 1, key, sizeof key, iv, message.data(), message.size(), small, sizeof small, &needed) == -4301,
          "a short buffer reports kCCBufferTooSmall");
    check(needed == 48, "and says how much room it needs");

    /* An algorithm that is not implemented says so rather than returning
     * the input unchanged. */
    check(mm_CCCrypt(0, 1 /* DES */, 0, key, sizeof key, iv, message.data(), 16, cipher, sizeof cipher, &needed) == -4305,
          "an unimplemented algorithm reports kCCUnimplemented");
}

} // namespace

int main() {
    test_sha256();
    test_aes_vectors();
    test_cbc_round_trip();
    if (failures == 0) std::printf("all CommonCrypto tests passed\n");
    return failures == 0 ? 0 : 1;
}

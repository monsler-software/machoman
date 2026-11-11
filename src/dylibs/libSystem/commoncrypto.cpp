/* CommonCrypto, the part of libSystem a macOS program uses to hash and
 * encrypt: CC_SHA* and CCCrypt.
 *
 * It lives in libSystem on Darwin, so a guest binds to it as it binds to
 * malloc, and a stub that returns success without touching the data is
 * worse than none: the caller then works on garbage. Ableton Live keeps
 * its interface description in an AES-encrypted resource, so it builds an
 * empty window if this lies to it.
 *
 * Everything here is implemented in this file rather than against a host
 * crypto library, because machoman takes no external dependencies. The
 * algorithms are the published standards (FIPS 197 for AES, FIPS 180-4 for
 * SHA); this is not a hardened implementation — it makes no attempt at
 * constant-time behaviour — and it is not for protecting secrets, only for
 * reading what an application wrote. */
#include "internal.h"

#include <cstdint>
#include <algorithm>
#include <cstring>

namespace {

/* ---- SHA-2 --------------------------------------------------------------- */

struct sha256_state {
    std::uint32_t h[8];
    std::uint64_t length;   /* bits */
    std::uint32_t curlen;
    std::uint8_t buf[64];
};

constexpr std::uint32_t k256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

std::uint32_t ror32(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256_block(sha256_state &s, const std::uint8_t *p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = static_cast<std::uint32_t>(p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3]);
    for (int i = 16; i < 64; i++) {
        const std::uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t v[8];
    std::memcpy(v, s.h, sizeof v);
    for (int i = 0; i < 64; i++) {
        const std::uint32_t s1 = ror32(v[4], 6) ^ ror32(v[4], 11) ^ ror32(v[4], 25);
        const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        const std::uint32_t t0 = v[7] + s1 + ch + k256[i] + w[i];
        const std::uint32_t s0 = ror32(v[0], 2) ^ ror32(v[0], 13) ^ ror32(v[0], 22);
        const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        const std::uint32_t t1 = s0 + maj;
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t0;
        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t0 + t1;
    }
    for (int i = 0; i < 8; i++) s.h[i] += v[i];
}

void sha256_init(sha256_state &s) {
    static const std::uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(s.h, iv, sizeof iv);
    s.length = 0;
    s.curlen = 0;
}

void sha256_update(sha256_state &s, const std::uint8_t *in, std::size_t n) {
    while (n) {
        if (s.curlen == 0 && n >= 64) {
            sha256_block(s, in);
            s.length += 512;
            in += 64;
            n -= 64;
            continue;
        }
        const std::size_t take = std::min<std::size_t>(64 - s.curlen, n);
        std::memcpy(s.buf + s.curlen, in, take);
        s.curlen += static_cast<std::uint32_t>(take);
        in += take;
        n -= take;
        if (s.curlen == 64) {
            sha256_block(s, s.buf);
            s.length += 512;
            s.curlen = 0;
        }
    }
}

void sha256_final(sha256_state &s, std::uint8_t *out) {
    s.length += s.curlen * 8ull;
    s.buf[s.curlen++] = 0x80;
    if (s.curlen > 56) {
        while (s.curlen < 64) s.buf[s.curlen++] = 0;
        sha256_block(s, s.buf);
        s.curlen = 0;
    }
    while (s.curlen < 56) s.buf[s.curlen++] = 0;
    for (int i = 7; i >= 0; i--) s.buf[56 + (7 - i)] = static_cast<std::uint8_t>(s.length >> (i * 8));
    sha256_block(s, s.buf);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = static_cast<std::uint8_t>(s.h[i] >> (24 - j * 8));
}

/* ---- AES (FIPS 197) ------------------------------------------------------- */

const std::uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

std::uint8_t inv_sbox[256];

void build_inverse() {
    static bool done = false;
    if (done) return;
    for (int i = 0; i < 256; i++) inv_sbox[sbox[i]] = static_cast<std::uint8_t>(i);
    done = true;
}

std::uint8_t xtime(std::uint8_t x) { return static_cast<std::uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

std::uint8_t gmul(std::uint8_t a, std::uint8_t b) {
    std::uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b = static_cast<std::uint8_t>(b >> 1);
    }
    return r;
}

struct aes_key {
    std::uint8_t round_key[240];
    int rounds = 0;
};

bool aes_expand(aes_key &k, const std::uint8_t *key, std::size_t key_bytes) {
    const int nk = static_cast<int>(key_bytes / 4);
    if (nk != 4 && nk != 6 && nk != 8) return false;
    k.rounds = nk + 6;
    std::memcpy(k.round_key, key, key_bytes);
    std::uint8_t rcon = 1;
    for (int i = nk; i < 4 * (k.rounds + 1); i++) {
        std::uint8_t t[4];
        std::memcpy(t, k.round_key + (i - 1) * 4, 4);
        if (i % nk == 0) {
            const std::uint8_t tmp = t[0];
            t[0] = static_cast<std::uint8_t>(sbox[t[1]] ^ rcon);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[tmp];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int j = 0; j < 4; j++) t[j] = sbox[t[j]];
        }
        for (int j = 0; j < 4; j++) k.round_key[i * 4 + j] = static_cast<std::uint8_t>(k.round_key[(i - nk) * 4 + j] ^ t[j]);
    }
    return true;
}

void add_round_key(std::uint8_t *s, const std::uint8_t *rk) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
}

void aes_encrypt_block(const aes_key &k, const std::uint8_t *in, std::uint8_t *out) {
    std::uint8_t s[16];
    std::memcpy(s, in, 16);
    add_round_key(s, k.round_key);
    for (int round = 1; round <= k.rounds; round++) {
        for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
        /* ShiftRows on the column-major state. */
        std::uint8_t t[16];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t[c * 4 + r] = s[((c + r) % 4) * 4 + r];
        std::memcpy(s, t, 16);
        if (round != k.rounds) {
            for (int c = 0; c < 4; c++) {
                std::uint8_t *p = s + c * 4;
                const std::uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                p[0] = static_cast<std::uint8_t>(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                p[1] = static_cast<std::uint8_t>(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                p[2] = static_cast<std::uint8_t>(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                p[3] = static_cast<std::uint8_t>((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
            }
        }
        add_round_key(s, k.round_key + round * 16);
    }
    std::memcpy(out, s, 16);
}

void aes_decrypt_block(const aes_key &k, const std::uint8_t *in, std::uint8_t *out) {
    build_inverse();
    std::uint8_t s[16];
    std::memcpy(s, in, 16);
    add_round_key(s, k.round_key + k.rounds * 16);
    for (int round = k.rounds - 1; round >= 0; round--) {
        std::uint8_t t[16];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t[((c + r) % 4) * 4 + r] = s[c * 4 + r];
        for (int i = 0; i < 16; i++) s[i] = inv_sbox[t[i]];
        add_round_key(s, k.round_key + round * 16);
        if (round != 0) {
            for (int c = 0; c < 4; c++) {
                std::uint8_t *p = s + c * 4;
                const std::uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                p[0] = static_cast<std::uint8_t>(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                p[1] = static_cast<std::uint8_t>(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                p[2] = static_cast<std::uint8_t>(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                p[3] = static_cast<std::uint8_t>(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
            }
        }
    }
    std::memcpy(out, s, 16);
}

/* CCCrypt's constants. */
enum : std::uint32_t { op_encrypt = 0, op_decrypt = 1 };
enum : std::uint32_t { alg_aes = 0 };
enum : std::uint32_t { opt_pkcs7 = 1, opt_ecb = 2 };
enum : std::int32_t { cc_success = 0, cc_param_error = -4300, cc_buffer_too_small = -4301,
                      cc_align_error = -4303, cc_decode_error = -4304, cc_unimplemented = -4305 };

} // namespace

extern "C" {

/* ---- SHA-256 ------------------------------------------------------------- */

/* Apple's CC_SHA256_CTX is 104 bytes; the state kept here is smaller, so it
 * fits in the caller's buffer. */
int darwin_CC_SHA256_Init(void *ctx) DARWIN(CC_SHA256_Init);
int darwin_CC_SHA256_Init(void *ctx) {
    if (!ctx) return 0;
    sha256_init(*static_cast<sha256_state *>(ctx));
    return 1;
}

int darwin_CC_SHA256_Update(void *ctx, const void *data, std::uint32_t len) DARWIN(CC_SHA256_Update);
int darwin_CC_SHA256_Update(void *ctx, const void *data, std::uint32_t len) {
    if (!ctx || (!data && len)) return 0;
    sha256_update(*static_cast<sha256_state *>(ctx), static_cast<const std::uint8_t *>(data), len);
    return 1;
}

int darwin_CC_SHA256_Final(unsigned char *md, void *ctx) DARWIN(CC_SHA256_Final);
int darwin_CC_SHA256_Final(unsigned char *md, void *ctx) {
    if (!ctx || !md) return 0;
    sha256_final(*static_cast<sha256_state *>(ctx), md);
    return 1;
}

unsigned char *darwin_CC_SHA256(const void *data, std::uint32_t len, unsigned char *md) DARWIN(CC_SHA256);
unsigned char *darwin_CC_SHA256(const void *data, std::uint32_t len, unsigned char *md) {
    if (!md) return nullptr;
    sha256_state s;
    sha256_init(s);
    if (data && len) sha256_update(s, static_cast<const std::uint8_t *>(data), len);
    sha256_final(s, md);
    return md;
}

/* ---- CCCrypt -------------------------------------------------------------- */

/* One-shot encrypt or decrypt, the call a program makes when it has the
 * whole message in memory. AES is the only algorithm implemented; anything
 * else reports kCCUnimplemented rather than pretending to have worked. */
std::int32_t darwin_CCCrypt(std::uint32_t op, std::uint32_t alg, std::uint32_t options,
                            const void *key, std::size_t key_length, const void *iv,
                            const void *data_in, std::size_t data_in_length,
                            void *data_out, std::size_t data_out_available,
                            std::size_t *data_out_moved) DARWIN(CCCrypt);
std::int32_t darwin_CCCrypt(std::uint32_t op, std::uint32_t alg, std::uint32_t options,
                            const void *key, std::size_t key_length, const void *iv,
                            const void *data_in, std::size_t data_in_length,
                            void *data_out, std::size_t data_out_available,
                            std::size_t *data_out_moved) {
    if (alg != alg_aes) return cc_unimplemented;
    if (!key || (!data_in && data_in_length) || (!data_out && data_out_available)) return cc_param_error;

    aes_key k;
    if (!aes_expand(k, static_cast<const std::uint8_t *>(key), key_length)) return cc_param_error;
    const bool ecb = (options & opt_ecb) != 0;
    const bool pad = (options & opt_pkcs7) != 0;

    const auto *in = static_cast<const std::uint8_t *>(data_in);
    auto *out = static_cast<std::uint8_t *>(data_out);
    std::uint8_t chain[16] = {};
    if (!ecb && iv) std::memcpy(chain, iv, 16);

    if (op == op_encrypt) {
        const std::size_t full = data_in_length / 16;
        const std::size_t rest = data_in_length % 16;
        const std::size_t needed = pad ? (full + 1) * 16 : data_in_length;
        if (!pad && rest) return cc_align_error;
        if (data_out_available < needed) {
            if (data_out_moved) *data_out_moved = needed;
            return cc_buffer_too_small;
        }
        std::uint8_t block[16];
        for (std::size_t i = 0; i < full; i++) {
            std::memcpy(block, in + i * 16, 16);
            if (!ecb) for (int j = 0; j < 16; j++) block[j] ^= chain[j];
            aes_encrypt_block(k, block, out + i * 16);
            if (!ecb) std::memcpy(chain, out + i * 16, 16);
        }
        if (pad) {
            const auto padding = static_cast<std::uint8_t>(16 - rest);
            std::memcpy(block, in + full * 16, rest);
            std::memset(block + rest, padding, padding);
            if (!ecb) for (int j = 0; j < 16; j++) block[j] ^= chain[j];
            aes_encrypt_block(k, block, out + full * 16);
        }
        if (data_out_moved) *data_out_moved = needed;
        return cc_success;
    }
    if (op != op_decrypt) return cc_param_error;
    if (data_in_length % 16) return cc_align_error;
    if (data_out_available < data_in_length) {
        if (data_out_moved) *data_out_moved = data_in_length;
        return cc_buffer_too_small;
    }
    std::uint8_t next[16];
    for (std::size_t i = 0; i < data_in_length / 16; i++) {
        if (!ecb) std::memcpy(next, in + i * 16, 16);
        aes_decrypt_block(k, in + i * 16, out + i * 16);
        if (!ecb) {
            for (int j = 0; j < 16; j++) out[i * 16 + j] ^= chain[j];
            std::memcpy(chain, next, 16);
        }
    }
    std::size_t moved = data_in_length;
    if (pad && moved) {
        const std::uint8_t padding = out[moved - 1];
        if (padding == 0 || padding > 16) return cc_decode_error;
        for (std::uint8_t j = 0; j < padding; j++)
            if (out[moved - 1 - j] != padding) return cc_decode_error;
        moved -= padding;
    }
    if (data_out_moved) *data_out_moved = moved;
    return cc_success;
}

} // extern "C"

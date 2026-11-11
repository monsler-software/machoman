#include "machoman/translation_cache.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
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

using digest = std::array<std::uint8_t, 32>;
digest finish(sha256_state &state) { digest d; sha256_final(state, d.data()); return d; }
digest hash(std::span<const std::uint8_t> bytes) {
    sha256_state s; sha256_init(s); sha256_update(s, bytes.data(), bytes.size()); return finish(s);
}
struct host_image { std::uintptr_t base = 0; digest identity{}; };
const host_image &translator_image() {
    static const host_image image = [] {
        host_image out;
        std::filesystem::path path;
#ifdef _WIN32
        HMODULE module = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&mm_translate_buffer), &module)) return out;
        wchar_t name[32768];
        const auto length = GetModuleFileNameW(module, name, 32768);
        if (!length || length == 32768) return out;
        path = std::wstring(name, length);
        out.base = reinterpret_cast<std::uintptr_t>(module);
#else
        Dl_info info{};
        if (!dladdr(reinterpret_cast<void *>(&mm_translate_buffer), &info) || !info.dli_fname) return out;
        out.base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
        path = info.dli_fname;
#endif
        std::ifstream f(path, std::ios::binary);
        if (!f) return host_image{};
        sha256_state state; sha256_init(state);
        std::array<std::uint8_t, 65536> chunk;
        while (f) {
            f.read(reinterpret_cast<char *>(chunk.data()), chunk.size());
            sha256_update(state, chunk.data(), static_cast<std::size_t>(f.gcount()));
        }
        if (!f.eof()) return host_image{};
        out.identity = finish(state);
        return out;
    }();
    return image;
}
std::array<std::uint8_t, 8> word(std::uint64_t value) {
    std::array<std::uint8_t, 8> out{};
    for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(value >> (i * 8));
    return out;
}
std::uint64_t read_word(const std::uint8_t *p) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t{p[i]} << (i * 8);
    return value;
}
digest cache_key(std::span<const std::uint8_t> image, std::span<const std::uint8_t> text, std::uint64_t base) {
    sha256_state s; sha256_init(s);
    const auto binary_hash = hash(image), text_hash = hash(text);
    const auto address = word(base);
    sha256_update(s, binary_hash.data(), binary_hash.size());
    sha256_update(s, text_hash.data(), text_hash.size());
    sha256_update(s, translator_image().identity.data(), 32);
    const auto features = word(mm_x64_codegen_features());
    sha256_update(s, features.data(), features.size());
    const auto residency = word(mm_register_cache_enabled());
    sha256_update(s, residency.data(), residency.size());
    sha256_update(s, address.data(), address.size());
    /* Only WATCH changes emitted instructions; limits/dumps affect runtime helpers. */
    if (const auto *watch = std::getenv("MACHOMAN_WATCH"))
        sha256_update(s, reinterpret_cast<const std::uint8_t *>(watch), std::strlen(watch));
    return finish(s);
}
constexpr std::size_t header_size = 120;
constexpr std::uint64_t max_cache_bytes = 2ull * 1024 * 1024 * 1024;
bool load(const std::filesystem::path &path, const digest &key, std::size_t instructions,
          std::uint64_t base, mm_translation_result &out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto length = f.tellg();
    if (length < static_cast<std::streamoff>(header_size) || length > static_cast<std::streamoff>(max_cache_bytes)) return false;
    f.seekg(0);
    std::array<std::uint8_t, header_size> header;
    f.read(reinterpret_cast<char *>(header.data()), header.size());
    if (!f || std::memcmp(header.data(), "MMAOTC02", 8) || std::memcmp(header.data() + 8, key.data(), 32)) return false;
    const auto code_size = read_word(header.data() + 72), count = read_word(header.data() + 80),
               reloc_count = read_word(header.data() + 88), unsupported = read_word(header.data() + 96),
               saved_base = read_word(header.data() + 104), dispatch = read_word(header.data() + 112);
    if (count != instructions || unsupported > count || saved_base != base || !code_size || dispatch >= code_size ||
        code_size > max_cache_bytes || count > max_cache_bytes / 32 || reloc_count > code_size / 8) return false;
    const auto expected = header_size + code_size + count * 32 + reloc_count * 8;
    if (expected != static_cast<std::uint64_t>(length)) return false;
    mm_translation_result result;
    result.code.bytes.resize(static_cast<std::size_t>(code_size));
    result.notes.reserve(static_cast<std::size_t>(count));
    result.code.host_relocations.reserve(static_cast<std::size_t>(reloc_count));
    sha256_state sum; sha256_init(sum);
    auto checked_header = header;
    std::fill(checked_header.begin() + 40, checked_header.begin() + 72, 0);
    sha256_update(sum, checked_header.data(), checked_header.size());
    auto read = [&](std::uint8_t *p, std::size_t size) {
        f.read(reinterpret_cast<char *>(p), static_cast<std::streamsize>(size));
        if (!f) return false;
        sha256_update(sum, p, size);
        return true;
    };
    if (!read(result.code.bytes.data(), result.code.size())) return false;
    std::uint64_t prev = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        std::array<std::uint8_t, 32> note;
        if (!read(note.data(), note.size())) return false;
        const auto source = read_word(note.data()), offset = read_word(note.data() + 8),
                   flags = read_word(note.data() + 16), entry = read_word(note.data() + 24);
        if (source != i * 4 || offset >= code_size || entry >= code_size || !entry ||
            (i && offset <= prev) || (flags >> 32) > 1) return false;
        prev = offset;
        result.notes.push_back({source, offset, static_cast<mm_a64_opcode>(flags & 0xffffffff), (flags >> 32) != 0, entry});
    }
    std::uint64_t previous_entry = result.notes.empty() ? 0 : result.notes.back().output_offset;
    for (const auto &note : result.notes) {
        if (note.entry_offset == note.output_offset) continue;
        if (note.entry_offset <= previous_entry) return false;
        previous_entry = note.entry_offset;
    }
    prev = 0;
    for (std::uint64_t i = 0; i < reloc_count; ++i) {
        std::array<std::uint8_t, 8> value;
        if (!read(value.data(), value.size())) return false;
        const auto offset = read_word(value.data());
        if (offset > code_size - 8 || (i && offset < prev + 8)) return false;
        prev = offset;
        result.code.host_relocations.push_back(offset);
    }
    const auto checksum = finish(sum);
    if (std::memcmp(checksum.data(), header.data() + 40, 32)) return false;
    for (const auto offset : result.code.host_relocations) {
        auto *p = result.code.bytes.data() + offset;
        const auto relative = read_word(p);
        if (relative > UINTPTR_MAX - translator_image().base) return false;
        const auto address = word(translator_image().base + relative);
        std::memcpy(p, address.data(), 8);
    }
    result.unsupported_count = static_cast<std::size_t>(unsupported);
    result.text_base = base; result.dispatch_stub = static_cast<std::size_t>(dispatch);
    out = std::move(result);
    return true;
}
bool save(const std::filesystem::path &path, const digest &key, const mm_translation_result &result) {
    if (result.code.size() > max_cache_bytes || result.notes.size() > max_cache_bytes / 32 ||
        result.code.host_relocations.size() > max_cache_bytes / 8 ||
        header_size + result.code.size() + result.notes.size() * 32 + result.code.host_relocations.size() * 8 > max_cache_bytes)
        return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    static std::atomic<unsigned> sequence{0};
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    auto temp = path;
    temp += ".tmp-" + std::to_string(pid) + "-" + std::to_string(sequence++);
#ifdef _WIN32
    std::FILE *f = _wfopen(temp.c_str(), L"wbx");
#else
    const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    std::FILE *f = fd < 0 ? nullptr : fdopen(fd, "wb");
    if (!f && fd >= 0) close(fd);
#endif
    if (!f) return false;
    bool good = true;
    sha256_state sum; sha256_init(sum);
    auto write = [&](std::span<const std::uint8_t> bytes, bool hashed = true) {
        if (good && std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) good = false;
        if (hashed) sha256_update(sum, bytes.data(), bytes.size());
    };
    std::array<std::uint8_t, header_size> header{};
    std::memcpy(header.data(), "MMAOTC02", 8); std::memcpy(header.data() + 8, key.data(), 32);
    const std::uint64_t fields[] = {result.code.size(), result.notes.size(), result.code.host_relocations.size(),
                                  result.unsupported_count, result.text_base, result.dispatch_stub};
    for (unsigned i = 0; i < 6; ++i) {
        const auto value = word(fields[i]); std::memcpy(header.data() + 72 + i * 8, value.data(), 8);
    }
    write(header);
    std::size_t start = 0;
    for (const auto offset : result.code.host_relocations) {
        if (offset < start || offset > result.code.size() || result.code.size() - offset < 8) { good = false; break; }
        write(std::span(result.code.bytes).subspan(start, offset - start));
        std::uintptr_t address;
        std::memcpy(&address, result.code.bytes.data() + offset, 8);
#ifdef _WIN32
        HMODULE owner = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(address), &owner) || reinterpret_cast<std::uintptr_t>(owner) != translator_image().base) good = false;
#else
        Dl_info info{};
        if (!dladdr(reinterpret_cast<void *>(address), &info) || reinterpret_cast<std::uintptr_t>(info.dli_fbase) != translator_image().base) good = false;
#endif
        write(word(address - translator_image().base));
        start = static_cast<std::size_t>(offset + 8);
    }
    write(std::span(result.code.bytes).subspan(start));
    for (const auto &note : result.notes) {
        write(word(note.source_offset)); write(word(note.output_offset));
        write(word(static_cast<std::uint32_t>(note.op) | (std::uint64_t{note.supported} << 32)));
        write(word(note.entry_offset ? note.entry_offset : note.output_offset));
    }
    for (const auto offset : result.code.host_relocations) write(word(offset));
    const auto checksum = finish(sum);
    if (std::fseek(f, 40, SEEK_SET) != 0) good = false;
    write(checksum, false);
    if (std::fclose(f) != 0) good = false;
    if (good) {
#ifdef _WIN32
        good = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
        good = rename(temp.c_str(), path.c_str()) == 0;
#endif
    }
    if (!good) std::filesystem::remove(temp, error);
    return good;
}
} // namespace

std::filesystem::path mm_translation_cache_path(const std::filesystem::path &binary) {
    return binary.parent_path() / ".machoman-cache" / (binary.filename().string() + ".x86_64.aot");
}
mm_cache_status mm_translate_cached(const std::filesystem::path &binary,
    std::span<const std::uint8_t> image, std::span<const std::uint8_t> text,
    std::uint64_t base, mm_translation_result &result) {
    const char *disabled = std::getenv("MACHOMAN_TRANSLATION_CACHE");
    if ((disabled && std::strcmp(disabled, "0") == 0) || !translator_image().base) {
        result = {}; mm_translate_buffer(text, base, result); return mm_cache_status::bypassed;
    }
    const auto key = cache_key(image, text, base);
    const auto path = mm_translation_cache_path(binary);
    try {
        if (load(path, key, text.size() / 4, base, result)) return mm_cache_status::hit;
    } catch (const std::exception &) { /* Cache read failure is a miss. */ }
    result = {}; mm_translate_buffer(text, base, result);
    try {
        if (save(path, key, result)) return mm_cache_status::stored;
    } catch (const std::exception &) { /* Fresh translation remains usable. */ }
    return mm_cache_status::bypassed;
}


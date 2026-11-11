#include "machoman/translation_cache.h"
#include "machoman/runtime.h"
#include <array>
#include <cstring>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <string>

static void require(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static void execute(const mm_translation_result &cached, std::span<const std::uint8_t> code) {
    mm_translation_result fresh;
    mm_translate_buffer(code, cached.text_base, fresh);
    mm_cpu_context a, b;
    a.x[1] = b.x[1] = 0; a.x[2] = b.x[2] = 1;
    a.v[1][0] = b.v[1][0] = 0x3ff0000000000000ull;
    require(mm_runtime_exec(cached, cached.text_base, a) == mm_runtime_status::ok, "cached code executes");
    require(mm_runtime_exec(fresh, cached.text_base, b) == mm_runtime_status::ok, "fresh code executes");
    require(a.x[0] == 0x77073096 && !std::memcmp(a.x, b.x, sizeof a.x) &&
            !std::memcmp(a.v, b.v, sizeof a.v) && a.fpsr == b.fpsr && a.eflags == b.eflags,
            "cached CRC/SIMD helpers and registers match fresh translation");
    require(cached.code.host_relocations.size() >= 2, "both CRC and SIMD helpers have relocations");
    if (mm_register_cache_enabled())
        require(cached.notes[4].entry_offset > cached.notes.back().output_offset, "independent register preload survives cache roundtrip");
}
int main(int argc, char **argv) {
    require(argc == 3, "mode and fixture path required");
    const std::filesystem::path binary = argv[2];
    const auto path = mm_translation_cache_path(binary);
    const std::array<std::uint32_t, 6> words{0x1ac24020, 0x0e216800, 0x6ea24423, 0x4e61d424, 0x4e61d485, 0xd65f03c0};
    const auto text = std::span(reinterpret_cast<const std::uint8_t *>(words.data()), sizeof words);
    constexpr std::uint64_t base = 0x100000000ull;
    mm_translation_result cached;
    if (std::string_view(argv[1]) == "disabled") {
        require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::bypassed, "cache can be disabled");
        execute(cached, text);
        return 0;
    }
    if (std::string_view(argv[1]) == "write") {
        std::filesystem::create_directories(binary.parent_path());
        std::filesystem::remove(path);
        require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::stored, "first use writes cache");
        execute(cached, text);
        std::ofstream(binary.string() + ".host-address") << reinterpret_cast<std::uintptr_t>(&mm_translate_buffer);
        puts("cache written with relocatable CRC/SIMD helpers");
        return 0;
    }
    require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::hit, "new process loads disk cache");
    execute(cached, text);
    std::uintptr_t previous = 0;
    std::ifstream(binary.string() + ".host-address") >> previous;
    std::printf("ASLR: writer=%llx reader=%llx\n", static_cast<unsigned long long>(previous),
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&mm_translate_buffer)));
    std::vector<std::uint8_t> changed_image(text.begin(), text.end()); changed_image.push_back(42);
    require(mm_translate_cached(binary, changed_image, text, base, cached) == mm_cache_status::stored, "non-code source changes invalidate cache");
    require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::stored, "original image restored");
    auto changed = words; changed[0] = 0x1ac24820;
    auto changed_text = std::span(reinterpret_cast<const std::uint8_t *>(changed.data()), sizeof changed);
    require(mm_translate_cached(binary, changed_text, changed_text, base, cached) == mm_cache_status::stored, "changed source invalidates cache");
    require(mm_translate_cached(binary, text, text, base + 0x4000, cached) == mm_cache_status::stored, "changed guest slide invalidates cache");
    require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::stored, "original restored");
    if (mm_x64_codegen_features() & MM_X64_FEATURE_AVX2) {
        const auto *setting = std::getenv("MACHOMAN_DISABLE_AVX2");
        const bool was_set = setting != nullptr;
        const std::string previous_setting = setting ? setting : "";
#ifdef _WIN32
        _putenv_s("MACHOMAN_DISABLE_AVX2", "1");
#else
        setenv("MACHOMAN_DISABLE_AVX2", "1", 1);
#endif
        require(mm_x64_codegen_features() == 0, "AVX2 disable selects fallback capability");
        require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::stored, "AVX2 cache not reused by fallback CPU profile");
        require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::hit, "fallback CPU profile cache reused");
        execute(cached, text);
#ifdef _WIN32
        _putenv_s("MACHOMAN_DISABLE_AVX2", previous_setting.c_str());
#else
        if (was_set) setenv("MACHOMAN_DISABLE_AVX2", previous_setting.c_str(), 1);
        else unsetenv("MACHOMAN_DISABLE_AVX2");
#endif
        require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::stored, "restoring AVX2 invalidates fallback CPU profile cache");
        require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::hit, "AVX2 CPU profile cache reused");
        execute(cached, text);
    }
    if (mm_register_cache_enabled()) {
        const auto *setting = std::getenv("MACHOMAN_REGISTER_CACHE");
        const bool was_set = setting != nullptr;
        const std::string previous_setting = setting ? setting : "";
#ifdef _WIN32
        _putenv_s("MACHOMAN_REGISTER_CACHE", "0");
#else
        setenv("MACHOMAN_REGISTER_CACHE", "0", 1);
#endif
        require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::stored, "register residency changes invalidate cache");
        require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::hit, "disabled residency profile reused");
        execute(cached, text);
#ifdef _WIN32
        _putenv_s("MACHOMAN_REGISTER_CACHE", previous_setting.c_str());
#else
        if (was_set) setenv("MACHOMAN_REGISTER_CACHE", previous_setting.c_str(), 1);
        else unsetenv("MACHOMAN_REGISTER_CACHE");
#endif
        require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::stored, "restoring residency invalidates cache");
        execute(cached, text);
    }
    { std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary); f.seekp(static_cast<std::streamoff>(120 + cached.code.size() + 4 * 32 + 24)); char bad[8]; std::memset(bad, 0xff, 8); f.write(bad, 8); }
    require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::stored, "out-of-bounds independent entry rejected");
    { std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary); f.seekg(112); char byte; f.read(&byte, 1); byte ^= 1; f.seekp(112); f.write(&byte, 1); }
    require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::stored, "header metadata is checksummed");
    { std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary); f.seekp(120); char bad = 0; f.write(&bad, 1); }
    require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::stored, "payload corruption is a cache miss");
    execute(cached, text);
    std::filesystem::resize_file(path, 43);
    require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::stored, "truncation is a cache miss");
    { std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary); f.seekp(40); char bad[32]{}; f.write(bad, 32); }
    require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::stored, "checksum corruption is a cache miss");
    { std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary); f.seekp(72); char huge[8]; std::memset(huge, 0xff, 8); f.write(huge, 8); }
    require(mm_translate_cached(binary, text, text, base, cached) == mm_cache_status::stored, "invalid sizes rejected before allocation");
    /* Parent is a regular file: caching must fail without breaking execution. */
    const auto blocker = binary.parent_path() / "not-a-directory";
    { std::ofstream f(blocker); f << "blocker"; }
    require(mm_translate_cached(blocker / "guest", text, text, base, cached) == mm_cache_status::bypassed, "unwritable cache falls back");
    execute(cached, text);
    puts("cross-process cache, source/slide invalidation, corruption bounds and I/O fallback passed");
}

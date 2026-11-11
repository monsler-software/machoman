/* Decode + translate + execute hand-assembled arm64 snippets through the
 * real runtime entry path, checking the guest register file afterwards.
 * Encodings were produced with `clang --target=aarch64`. */
#include "machoman/arch/arm64_decoder.h"
#include "machoman/runtime.h"
#include "machoman/translator.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <bit>
#include <cstring>
#include <initializer_list>
#include <vector>
#include <string>
#include <xmmintrin.h>
#if !defined(_WIN32)
#include <atomic>
#include <barrier>
#include <csignal>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <thread>
#endif

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

constexpr std::uint32_t RET = 0xD65F03C0;

mm_a64_insn decode(std::uint32_t word) {
    std::uint8_t b[4];
    std::memcpy(b, &word, 4);
    return mm_a64_decode(b, 0x1000);
}

mm_translation_result translate(std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint8_t> code;
    for (std::uint32_t w : words) {
        std::uint8_t b[4];
        std::memcpy(b, &w, 4);
        code.insert(code.end(), b, b + 4);
    }
    mm_translation_result r;
    mm_translate_buffer(code, 0x1000, r);
    return r;
}

void test_null_branch() {
    for (std::uint32_t insn : {0xd61f0000u, 0xd63f0000u}) { /* br x0; blr x0 */
        mm_cpu_context ctx{};
        const auto code = translate({insn});
        check(mm_runtime_exec(code, 0x1000, ctx) == mm_runtime_status::bad_target,
              "null indirect branch is not a successful guest return");
        check(ctx.fault_va == 0, "null indirect branch records its target");
    }
    mm_cpu_context ctx{};
    const auto code = translate({RET});
    check(mm_runtime_exec(code, 0x1000, ctx) == mm_runtime_status::ok,
          "top-level ret still returns to the host");
}

#if !defined(_WIN32)
void prior_fault_handler(int, siginfo_t *, void *) {}
void test_parallel_faults() {
    const auto translated = translate({0xf9400020, RET}); /* ldr x0, [x1] */
    mm_runtime_code code(translated);
    struct sigaction previous{}, custom{}, restored{};
    custom.sa_sigaction = prior_fault_handler;
    custom.sa_flags = SA_SIGINFO;
    sigemptyset(&custom.sa_mask);
    sigaction(SIGSEGV, &custom, &previous);
    sigset_t blocked{}, previous_mask{};
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &blocked, &previous_mask);
    std::barrier start(2);
    std::atomic<unsigned> caught = 0;
    std::atomic<unsigned> masks_preserved = 0;
    auto fault = [&] {
        for (unsigned i = 0; i < 128; i++) {
            start.arrive_and_wait();
            mm_cpu_context ctx{};
            if (code.run(ctx, 0x1000) == mm_runtime_status::memory_fault && ctx.fault_va == 0x1000 && ctx.fault_addr == 0)
                ++caught;
        }
        sigset_t after{};
        pthread_sigmask(SIG_SETMASK, nullptr, &after);
        if (sigismember(&after, SIGUSR1) && !sigismember(&after, SIGSEGV)) ++masks_preserved;
    };
    std::thread first(fault), second(fault);
    first.join();
    second.join();
    pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
    sigaction(SIGSEGV, nullptr, &restored);
    sigaction(SIGSEGV, &previous, nullptr);
    check(caught == 256, "parallel guest faults keep their own contexts and handler");
    check(masks_preserved == 2, "guest fault handling preserves thread signal masks");
    check((restored.sa_flags & SA_SIGINFO) && restored.sa_sigaction == prior_fault_handler,
          "runtime restores the previous host fault handler");
}
#endif

/* Runs `words` + RET with `ctx` as the initial register file. */
bool run(std::initializer_list<std::uint32_t> words, mm_cpu_context &ctx) {
    std::vector<std::uint32_t> all(words);
    all.push_back(RET);
    std::vector<std::uint8_t> code(all.size() * 4);
    std::memcpy(code.data(), all.data(), code.size());
    mm_translation_result r;
    mm_translate_buffer(code, 0x1000, r);
    if (r.unsupported_count != 0)
        return false;
    return mm_runtime_exec(r, 0x1000, ctx) == mm_runtime_status::ok;
}


/* CRC32 and CRC32C (ARMv8 CRC extension). The expected values are the
 * definition of the instruction — a raw CRC step with no inversion — and
 * the Ethernet ones agree with zlib: zlib.crc32(data) == ~CRC32W(~0, data). */
void test_crc() {
    struct { std::uint32_t word; std::uint64_t acc, data; std::uint32_t expected; const char *what; } cases[] = {
        {0x1AC24020, 0, 0x01, 0x77073096, "crc32b"},
        {0x1AC24820, 0, 0x12345678, 0x8E2958CE, "crc32w"},
        {0x9AC24C20, 0xFFFFFFFF, 0x0123456789ABCDEFull, 0xBBC41DB8, "crc32x"},
        {0x1AC25020, 0, 0x01, 0xF26B8303, "crc32cb"},
        {0x1AC25820, 0, 0x12345678, 0xFA745634, "crc32cw"},
        {0x9AC25C20, 0xFFFFFFFF, 0x0123456789ABCDEFull, 0x9A4F27DC, "crc32cx"},
    };
    for (const auto &c : cases) {
        mm_cpu_context ctx{};
        ctx.x[1] = c.acc;
        ctx.x[2] = c.data;
        check(run({c.word}, ctx), c.what);
        check(ctx.x[0] == c.expected, c.what);
    }
    /* The high half of the result is zero: the instruction writes Wd. */
    mm_cpu_context ctx{};
    ctx.x[0] = ~0ull;
    ctx.x[1] = 0;
    ctx.x[2] = 0x01;
    check(run({0x1AC24020}, ctx) && ctx.x[0] == 0x77073096, "crc32b zero-extends into Xd");
}


/* The halving add/sub forms: add in one extra bit and shift back. The
 * lanes are bytes of v[n][0], lane 0 being the low byte. */
void test_halving() {
    mm_cpu_context ctx{};
    ctx.v[1][0] = 0x0000000000'8001FFull;   /* lanes: 255, 1, 128 */
    ctx.v[2][0] = 0x0000000000'8002FFull;   /* lanes: 255, 2, 128 */
    check(run({0x2E220420}, ctx), "uhadd.8b runs");                     /* uhadd v0.8b, v1.8b, v2.8b */
    check(ctx.v[0][0] == 0x8001FFull, "uhadd: (255,255)=255, (1,2)=1, (128,128)=128");
    check(ctx.v[0][1] == 0, "the 8B form clears the high half");

    mm_cpu_context r{};
    r.v[1][0] = 1;
    r.v[2][0] = 2;
    check(run({0x2E221420}, r) && (r.v[0][0] & 0xFF) == 2, "urhadd(1,2) rounds up to 2");

    mm_cpu_context sg{};
    sg.v[1][0] = 0xFE;  /* -2 */
    sg.v[2][0] = 0xFC;  /* -4 */
    check(run({0x0E220420}, sg) && (sg.v[0][0] & 0xFF) == 0xFD, "shadd(-2,-4) = -3");

    mm_cpu_context sub{};
    sub.v[1][0] = 10;
    sub.v[2][0] = 3;
    check(run({0x2E222420}, sub) && (sub.v[0][0] & 0xFF) == 3, "uhsub(10,3) = 3");
}

/* Widening and narrowing (three-different): the L/W forms widen, the HN
 * forms take the top half of a wide result. */
void test_widening() {
    /* uaddl v0.8h, v1.8b, v2.8b — 0x2E220020 */
    mm_cpu_context ctx{};
    ctx.v[1][0] = 0x00000000000002FFull;  /* lanes 0,1 = 255, 2 */
    ctx.v[2][0] = 0x00000000000003FFull;  /* lanes 0,1 = 255, 3 */
    check(run({0x2E220020}, ctx), "uaddl.8h runs");
    check((ctx.v[0][0] & 0xFFFF) == 510, "uaddl widens: 255+255 = 510");
    check(((ctx.v[0][0] >> 16) & 0xFFFF) == 5, "uaddl lane 1 = 5");

    /* saddl v0.8h, v1.8b, v2.8b — 0x0E220020, signed lanes */
    mm_cpu_context sg{};
    sg.v[1][0] = 0xFF;  /* -1 */
    sg.v[2][0] = 0xFE;  /* -2 */
    check(run({0x0E220020}, sg) && (sg.v[0][0] & 0xFFFF) == 0xFFFD, "saddl(-1,-2) = -3");

    /* umull v0.8h, v1.8b, v2.8b — 0x2E22C020 */
    mm_cpu_context mul{};
    mul.v[1][0] = 0xFF;
    mul.v[2][0] = 0xFF;
    check(run({0x2E22C020}, mul) && (mul.v[0][0] & 0xFFFF) == 0xFE01, "umull(255,255) = 65025");

    /* umlal v0.8h, v1.8b, v2.8b — 0x2E228020 accumulates into v0 */
    mm_cpu_context mla{};
    mla.v[0][0] = 10;
    mla.v[1][0] = 3;
    mla.v[2][0] = 4;
    check(run({0x2E228020}, mla) && (mla.v[0][0] & 0xFFFF) == 22, "umlal: 10 + 3*4 = 22");

    /* uaddw v0.8h, v1.8h, v2.8b — 0x2E221020: wide + narrow */
    mm_cpu_context w{};
    w.v[1][0] = 0x0100;   /* lane 0 = 256 */
    w.v[2][0] = 0x02;     /* lane 0 = 2 */
    check(run({0x2E221020}, w) && (w.v[0][0] & 0xFFFF) == 258, "uaddw: 256 + 2 = 258");

    /* addhn v0.8b, v1.8h, v2.8h — 0x0E224020: the top byte of the sum */
    mm_cpu_context hn{};
    hn.v[1][0] = 0x0180;  /* 384 */
    hn.v[2][0] = 0x0180;  /* 384 */
    check(run({0x0E224020}, hn) && (hn.v[0][0] & 0xFF) == 0x03, "addhn: (384+384) >> 8 = 3");

    /* raddhn v0.8b, v1.8h, v2.8h — 0x2E224020 rounds */
    mm_cpu_context rhn{};
    rhn.v[1][0] = 0x0080;
    rhn.v[2][0] = 0x0080;
    check(run({0x2E224020}, rhn) && (rhn.v[0][0] & 0xFF) == 0x01, "raddhn: (128+128+128) >> 8 = 1");

    /* uabdl v0.8h, v1.8b, v2.8b — 0x2E227020 */
    mm_cpu_context abd{};
    abd.v[1][0] = 3;
    abd.v[2][0] = 10;
    check(run({0x2E227020}, abd) && (abd.v[0][0] & 0xFFFF) == 7, "uabdl(3,10) = 7");
}

/* Absolute difference and the rounding right shift. */
void test_abd_rshr() {
    /* uabd v0.8b, v1.8b, v2.8b — 0x2E227420 */
    mm_cpu_context u{};
    u.v[1][0] = 0x0A03;   /* lanes 3, 10 */
    u.v[2][0] = 0x030A;   /* lanes 10, 3 */
    check(run({0x2E227420}, u), "uabd.8b runs");
    check((u.v[0][0] & 0xFFFF) == 0x0707, "uabd: |3-10| = |10-3| = 7");

    /* saba v0.8b, v1.8b, v2.8b — 0x0E227C20 accumulates */
    mm_cpu_context a{};
    a.v[0][0] = 5;
    a.v[1][0] = 0xFE;  /* -2 */
    a.v[2][0] = 0x02;  /*  2 */
    check(run({0x0E227C20}, a) && (a.v[0][0] & 0xFF) == 9, "saba: 5 + |-2 - 2| = 9");

    /* urshr v0.8b, v1.8b, #1 — 0x2F0925 20? built as shift-by-immediate */
    mm_cpu_context r{};
    r.v[1][0] = 0x03;
    check(run({0x2F0F2420}, r) && (r.v[0][0] & 0xFF) == 2, "urshr #1: (3 + 1) >> 1 = 2");
}

void test_decoder() {
    check(decode(0x17FFFFFF).imm == 0x1000 - 4, "b .-4 decodes negative displacement");
    check(decode(0xB4000080).imm == 0x1000 + 16 && decode(0xB4000080).op == mm_a64_opcode::cbz, "cbz .+16");
    check(decode(0x9B027C20).op == mm_a64_opcode::madd && decode(0x9B027C20).ra == 31, "mul decodes as madd ra=xzr");
    check(decode(0x8B22C020).op == mm_a64_opcode::add_ext && decode(0x8B22C020).extend == mm_a64_extend::sxtw, "extended-register add");
    {
        const auto i = decode(0xEB01001F); /* cmp x0, x1 */
        check(i.op == mm_a64_opcode::sub_reg && i.sets_flags && i.rd == 31, "cmp = subs xzr");
    }
    {
        const auto i = decode(0x8A220020); /* bic x0, x1, x2 */
        check(i.op == mm_a64_opcode::and_reg && i.invert_rm, "bic sets invert_rm");
    }
    {
        const auto i = decode(0x4B821020); /* sub w0, w1, w2, asr #4 */
        check(!i.is64 && i.shift == mm_a64_shift::asr && i.shift_amount == 4, "32-bit asr shift decoded");
    }
    check(decode(0xD2800540).op == mm_a64_opcode::movz && decode(0xD2800540).imm == 42, "movz #42");
}

/* Reference NZCV for the flag matrix tests. */
struct nzcv { bool n, z, c, v; };

nzcv flags_sub(std::uint64_t a, std::uint64_t b, bool is64) {
    const std::uint64_t m = is64 ? ~0ull : 0xFFFFFFFFull;
    const int sign = is64 ? 63 : 31;
    const std::uint64_t r = (a - b) & m;
    return {static_cast<bool>((r >> sign) & 1), r == 0, (a & m) >= (b & m),
            static_cast<bool>((((a ^ b) & (a ^ r)) >> sign) & 1)};
}

nzcv flags_add(std::uint64_t a, std::uint64_t b, bool is64) {
    const std::uint64_t m = is64 ? ~0ull : 0xFFFFFFFFull;
    const int sign = is64 ? 63 : 31;
    const std::uint64_t r = (a + b) & m;
    return {static_cast<bool>((r >> sign) & 1), r == 0, r < (a & m),
            static_cast<bool>(((~(a ^ b) & (a ^ r)) >> sign) & 1)};
}

bool cond_holds(nzcv f, unsigned cond) {
    switch (cond) {
    case 0: return f.z;            case 1: return !f.z;
    case 2: return f.c;            case 3: return !f.c;
    case 4: return f.n;            case 5: return !f.n;
    case 6: return f.v;            case 7: return !f.v;
    case 8: return f.c && !f.z;    case 9: return !(f.c && !f.z);
    case 10: return f.n == f.v;    case 11: return f.n != f.v;
    case 12: return !f.z && f.n == f.v; case 13: return f.z || f.n != f.v;
    default: return true;
    }
}

/* producer ; b.cond +12 ; movz x0,#0 ; ret ; movz x0,#1 ; ret  -> x0 = taken */
bool branch_taken(std::uint32_t producer, unsigned cond, mm_cpu_context &c) {
    const std::uint32_t bcond = 0x54000060u | cond;
    return run({producer, bcond, 0xD2800000, RET, 0xD2800020, RET}, c) && c.x[0] == 1;
}

void test_cond_branch_matrix() {
    const std::uint64_t vals[] = {0, 1, 5, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x7FFFFFFFFFFFFFFFull,
                                  0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull};
    int bad = 0;
    for (std::uint64_t a : vals)
        for (std::uint64_t b : vals)
            for (unsigned cond = 0; cond < 16; cond++) {
                mm_cpu_context c;
                c = {}; c.x[1] = a; c.x[2] = b;
                if (branch_taken(0xEB02003F, cond, c) != cond_holds(flags_sub(a, b, true), cond)) bad++; /* cmp x1, x2 */
                c = {}; c.x[1] = a; c.x[2] = b;
                if (branch_taken(0xAB02003F, cond, c) != cond_holds(flags_add(a, b, true), cond)) bad++; /* cmn x1, x2 */
                c = {}; c.x[1] = a; c.x[2] = b;
                if (branch_taken(0x2B020020, cond, c) != cond_holds(flags_add(a, b, false), cond)) bad++; /* adds w0 */
                c = {}; c.x[1] = a; c.x[2] = b;
                const std::uint64_t t = a & b;
                const nzcv logic{static_cast<bool>(t >> 63), t == 0, false, false};
                if (branch_taken(0xEA02003F, cond, c) != cond_holds(logic, cond)) bad++; /* tst x1, x2 */
            }
    check(bad == 0, "b.cond after cmp/cmn/adds/tst matches reference NZCV for all conditions");
    if (bad) std::fprintf(stderr, "  %d mismatches\n", bad);
}

void test_branch_context_flags() {
    // Enter at B.cond without executing the producer; all 16 architectural
    // NZCV states, both branch outcomes, and unrelated EFLAGS image bits.
    constexpr std::uint64_t mask = 0x8c1;
    for (unsigned flags = 0; flags < 16; ++flags) {
        const nzcv expected{bool(flags & 8), bool(flags & 4), bool(flags & 2), bool(flags & 1)};
        const std::uint64_t image = 2 | (expected.n ? 0x80 : 0) | (expected.z ? 0x40 : 0) |
            (expected.c ? 0 : 1) | (expected.v ? 0x800 : 0);
        for (unsigned cond = 0; cond < 16; ++cond) {
            for (std::uint64_t extra : {0ull, 0xffff000000000410ull}) {
                const auto code = translate({0xeb02003f, 0x54000060u | cond, 0xd2800000, RET, 0xd2800020, RET});
                mm_cpu_context c{}; c.eflags = image | extra;
                const auto saved = c.eflags;
                check(mm_runtime_exec(code, 0x1004, c) == mm_runtime_status::ok && c.x[0] == cond_holds(expected, cond),
                    "direct branch entry tests materialized NZCV and ignores unrelated RFLAGS bits");
                check(c.eflags == saved && (c.eflags & mask) == (image & mask), "B.cond preserves the full context flags image");
            }
            const auto outside = translate({0x54000100u | cond, RET});
            mm_cpu_context c{}; c.eflags = image;
            const auto status = mm_runtime_exec(outside, 0x1000, c);
            check(status == (cond_holds(expected, cond) ? mm_runtime_status::bad_target : mm_runtime_status::ok) &&
                (!cond_holds(expected, cond) || c.fault_va == 0x1020) && c.eflags == image,
                "out-of-text conditional branch preserves NZCV and dispatches only on the taken path");
        }
    }
    // B.cond is not an architectural flags producer. A subsequent CSEL
    // and MRS must observe the same NZCV, including after the taken path.
    for (unsigned flags = 0; flags < 16; ++flags) {
        mm_cpu_context c{}; c.x[3] = std::uint64_t(flags) << 28; c.x[1] = 7; c.x[2] = 9;
        check(run({0xd51b4203, 0x54000040, 0xd503201f, 0x9a820020, 0xd53b4204}, c) &&
            c.x[0] == (flags & 4 ? 7 : 9) && c.x[4] == (std::uint64_t(flags) << 28),
            "MSR, B.cond, CSEL and MRS keep architectural NZCV across both paths");
    }
#if !defined(_WIN32)
    const auto code = translate({0x54000040, RET, 0xf9400020, RET});
    mm_cpu_context c{}; c.eflags = 0x842; c.x[1] = 0;
    check(mm_runtime_exec(code, 0x1000, c) == mm_runtime_status::memory_fault && c.fault_va == 0x1008 && c.eflags == 0x842,
        "fault after a taken branch retains materialized NZCV");
#endif
}

void test_vector_residency() {
    const auto host_mxcsr = _mm_getcsr();
    const std::vector<std::vector<std::uint32_t>> sequences = {
        {0x1e622820, 0x1e622823, 0x1e630804, 0x1e654084, 0x9e780080, 0x1e622885, 0x1e620c25, 0x1e6428a0, RET},
        {0x0e22d420, 0x4e20d403, 0x4e66d4a4, 0x6ea0f800, 0x0e617803, 0x4e64d463, 0x6ee1b863, 0x6e61d863, 0x4e64d463, RET},
        {0x1e622820, 0x1e622823, 0xb4000046, 0x1e612820, 0x1e630804, 0x1e602885, RET},
        {0x1e622820, 0x1e602803, 0xfd4000e4, 0x1e642865, RET},
    };
    const auto *setting = std::getenv("MACHOMAN_REGISTER_CACHE");
    const bool was_set = setting != nullptr, enabled = mm_register_cache_enabled();
    const std::string previous_setting = setting ? setting : "";
    auto restore_setting = [&] {
#ifdef _WIN32
        _putenv_s("MACHOMAN_REGISTER_CACHE", previous_setting.c_str());
#else
        if (was_set) setenv("MACHOMAN_REGISTER_CACHE", previous_setting.c_str(), 1);
        else unsetenv("MACHOMAN_REGISTER_CACHE");
#endif
    };
    auto translate_at = [](const auto &words, std::uint64_t base) {
        mm_translation_result result;
        mm_translate_buffer({reinterpret_cast<const std::uint8_t *>(words.data()), words.size() * 4}, base, result);
        return result;
    };
    bool found_cold = false;
    for (const auto &words : sequences) {
        const auto optimized = translate_at(words, 0x1000);
#ifdef _WIN32
        _putenv_s("MACHOMAN_REGISTER_CACHE", "0");
#else
        setenv("MACHOMAN_REGISTER_CACHE", "0", 1);
#endif
        const auto reference = translate_at(words, 0x8000);
        restore_setting();
        mm_runtime_code fast(optimized), slow(reference);
        check(fast.ok() && slow.ok(), "map register-residency differential code");
        const auto &first = optimized.notes.front();
        const auto first_entry = first.entry_offset ? first.entry_offset : first.output_offset;
        const auto code_base = mm_runtime_code::translation_of(0x1000) - first_entry;
        bool has_cold = false;
        for (const auto &note : optimized.notes) {
            check(fast.guest_pc(code_base + note.output_offset) == 0x1000 + note.source_offset,
                "residency body PC maps to the original guest instruction");
            const auto entry = note.entry_offset ? note.entry_offset : note.output_offset;
            check(fast.guest_pc(code_base + entry) == 0x1000 + note.source_offset,
                "residency independent-entry PC maps to the original guest instruction");
            has_cold |= entry != note.output_offset;
        }
        found_cold |= has_cold;
        for (unsigned mode = 0; mode < 4; ++mode) {
            constexpr unsigned rounding[] = {0, 2, 1, 3};
            for (unsigned seed = 0; seed < 16; ++seed) {
                for (std::size_t entry = 0; entry < words.size(); ++entry) {
                    mm_cpu_context a{};
                    for (unsigned reg = 0; reg < 32; ++reg) {
                        a.x[reg] = 0x1234567800000000ull | reg;
                        const double low = (double(reg) + 0.25 * double(seed)) * (seed & 1 ? -1 : 1);
                        const double high = double(reg) + 0.125 * double(seed) + 1;
                        std::memcpy(&a.v[reg][0], &low, 8); std::memcpy(&a.v[reg][1], &high, 8);
                        if (seed >= 12) {
                            constexpr std::uint64_t special[] = {0x7ff800007fc12345ull, 0x7ff0000000000000ull, 1, 0x8000000000000000ull};
                            a.v[reg][0] = special[seed - 12];
                            a.v[reg][1] = special[seed - 12] ^ 0x8000000000000000ull;
                        }
                    }
                    const double memory = -2.75;
                    a.x[7] = reinterpret_cast<std::uintptr_t>(&memory);
                    a.x[6] = seed & 1; a.x[30] = 0; a.eflags = 0x8c3;
                    a.fpcr = std::uint64_t(mode) << 22; a.fpsr = 0x8000000;
                    a.mxcsr = 0x1f80 | (rounding[mode] << 13);
                    auto b = a;
                    _mm_setcsr(a.mxcsr);
                    const auto status_a = fast.run(a, 0x1000 + 4 * entry);
                    const auto mxcsr_a = _mm_getcsr();
                    _mm_setcsr(b.mxcsr);
                    const auto status_b = slow.run(b, 0x8000 + 4 * entry);
                    check(status_a == status_b && !std::memcmp(a.x, b.x, sizeof a.x) && !std::memcmp(a.v, b.v, sizeof a.v) &&
                        a.eflags == b.eflags && a.fpcr == b.fpcr && a.fpsr == b.fpsr && mxcsr_a == _mm_getcsr(),
                        "cached/fallback FP chains agree from every instruction, branch path and rounding mode");
                }
            }
        }
#if !defined(_WIN32)
        if (words.size() == 5) {
            mm_cpu_context a{}; const double one = 1;
            std::memcpy(&a.v[1][0], &one, 8); std::memcpy(&a.v[2][0], &one, 8);
            a.eflags = 0x8c3;
            auto b = a;
            check(fast.run(a, 0x1000) == mm_runtime_status::memory_fault && slow.run(b, 0x8000) == mm_runtime_status::memory_fault &&
                a.fault_va == 0x1008 && b.fault_va == 0x8008 && !std::memcmp(a.v, b.v, sizeof a.v) &&
                a.v[0][0] == 0x4000000000000000ull && a.v[3][0] == 0x4010000000000000ull && a.eflags == b.eflags,
                "fault after resident FP results exposes fully materialized guest vectors and NZCV");
        }
#endif
    }
    if (enabled) check(found_cold, "FP chains have independent entry preload stubs");
    restore_setting();
    _mm_setcsr(host_mxcsr);
}

void test_branches() {
    mm_cpu_context c;

    c = {}; check(run({0x14000002, 0xD2800020, 0xD2800040}, c) && c.x[0] == 2, "b forward skips movz");

    /* movz x0,#0 ; L: add x0,x0,#1 ; subs x1,x1,#1 ; b.ne L */
    c = {}; c.x[1] = 10;
    check(run({0xD2800000, 0x91000400, 0xF1000421, 0x54FFFFC1}, c) && c.x[0] == 10, "backward b.ne loop");
    /* movz x0,#0 ; L: add x0,x0,#1 ; sub x1,x1,#1 ; cbnz x1, L */
    c = {}; c.x[1] = 7;
    check(run({0xD2800000, 0x91000400, 0xD1000421, 0xB5FFFFC1}, c) && c.x[0] == 7, "backward cbnz loop");

    c = {}; c.x[1] = 0; check(run({0xB4000041, 0xD2800020}, c) && c.x[0] == 0, "cbz taken");
    c = {}; c.x[1] = 0x100000000ull; check(run({0x35000041, 0xD2800020}, c) && c.x[0] == 1, "cbnz w ignores upper half");
    c = {}; c.x[1] = 1ull << 40; check(run({0xB6400041, 0xD2800020}, c) && c.x[0] == 1, "tbz #40 not taken when bit set");
    c = {}; c.x[1] = 0;          check(run({0xB6400041, 0xD2800020}, c) && c.x[0] == 0, "tbz #40 taken when bit clear");
    c = {}; c.x[1] = 8;          check(run({0x37180041, 0xD2800020}, c) && c.x[0] == 0, "tbnz #3 taken");

    /* bl +12 ; mov x30, x4 (restore the return-to-host sentinel) ; ret ; movz x0,#7 ; ret */
    c = {}; check(run({0x94000003, 0xAA0403FE, RET, 0xD28000E0}, c) && c.x[0] == 7, "bl/ret round trip");
    /* blr x3 ; mov x30, x4 ; ret ; movz x0,#7 ; ret */
    c = {}; c.x[3] = 0x1000 + 12;
    check(run({0xD63F0060, 0xAA0403FE, RET, 0xD28000E0}, c) && c.x[0] == 7, "blr/ret round trip");
    /* br x3 ; movz x0,#1 ; ret ; movz x0,#2 ; ret */
    c = {}; c.x[3] = 0x1000 + 12;
    check(run({0xD61F0060, 0xD2800020, RET, 0xD2800040}, c) && c.x[0] == 2, "br x3");

    /* exits */
    auto r = translate({0xD61F0060});
    c = {}; c.x[3] = 0x5000;
    check(mm_runtime_exec(r, 0x1000, c) == mm_runtime_status::bad_target && c.fault_va == 0x5000, "br outside text reports bad_target");
    c = {}; c.x[3] = 0x1002;
    check(mm_runtime_exec(r, 0x1000, c) == mm_runtime_status::bad_target && c.fault_va == 0x1002, "misaligned br reports bad_target");
    r = translate({0x14000010}); /* b .+64, outside the 4-byte text */
    c = {}; check(mm_runtime_exec(r, 0x1000, c) == mm_runtime_status::bad_target && c.fault_va == 0x1040, "direct b outside text");
    r = translate({0xD4001001}); c = {}; c.x[16] = 4;
    check(mm_runtime_exec(r, 0x1000, c) == mm_runtime_status::syscall && c.fault_va == 0x1000 && c.x[16] == 4, "svc exits with syscall");
    r = translate({0xD4200020}); c = {};
    check(mm_runtime_exec(r, 0x1000, c) == mm_runtime_status::brk && c.fault_va == 0x1000, "brk exits");
    r = translate({0xD503201F, 0x00000000}); c = {}; /* nop ; udf #0 */
    check(r.unsupported_count == 1 && mm_runtime_exec(r, 0x1000, c) == mm_runtime_status::unsupported_insn && c.fault_va == 0x1004,
          "unsupported instruction exits with its VA");
}

void test_conditional_ops() {
    mm_cpu_context c;
    constexpr std::uint32_t CMP12 = 0xEB02003F; /* cmp x1, x2 */

    c = {}; c.x[1] = 5; c.x[2] = 5; check(run({CMP12, 0x9A820020}, c) && c.x[0] == 5, "csel eq taken -> rn");
    c = {}; c.x[1] = 5; c.x[2] = 6; check(run({CMP12, 0x9A820020}, c) && c.x[0] == 6, "csel eq not taken -> rm");
    c = {}; c.x[1] = 5; c.x[2] = 5; check(run({CMP12, 0x9A821420}, c) && c.x[0] == 6, "csinc ne not taken -> rm+1");
    c = {}; c.x[1] = 5; c.x[2] = 9; check(run({CMP12, 0xDA82B020}, c) && c.x[0] == 5, "csinv lt taken -> rn");
    c = {}; c.x[1] = 9; c.x[2] = 5; check(run({CMP12, 0xDA82B020}, c) && c.x[0] == ~std::uint64_t{5}, "csinv lt not taken -> ~rm");
    c = {}; c.x[1] = 3; c.x[2] = 5; check(run({CMP12, 0xDA82A420}, c) && c.x[0] == static_cast<std::uint64_t>(-5), "csneg ge not taken -> -rm");
    c = {}; c.x[1] = 9; c.x[2] = 5; check(run({CMP12, 0x1A9F97E0}, c) && c.x[0] == 1, "cset hi");
    c = {}; c.x[1] = 5; c.x[2] = 5; check(run({CMP12, 0x1A9F97E0}, c) && c.x[0] == 0, "cset hi false");
    c = {}; c.x[1] = 9; c.x[2] = 5; check(run({CMP12, 0x9A81D420}, c) && c.x[0] == 10, "cinc gt");

    /* cmp x1,x2 ; ccmp x1, #5, #8, ne ; csel x0, x1, x2, eq */
    c = {}; c.x[1] = 5; c.x[2] = 6;
    check(run({CMP12, 0xFA451828, 0x9A820020}, c) && c.x[0] == 5, "ccmp: cond true -> compares x1 with 5 -> eq");
    /* cmp x1,x2 ; ccmp x1, #5, #8, ne ; csel x0, x1, x2, mi */
    c = {}; c.x[1] = 7; c.x[2] = 7;
    check(run({CMP12, 0xFA451828, 0x9A824020}, c) && c.x[0] == 7, "ccmp: cond false -> nzcv #8 sets N");
    /* cmp x1,x2 ; ccmn w1, w2, #2, ge ; csel x0, x1, x2, eq */
    c = {}; c.x[1] = 0xFFFFFFFF; c.x[2] = 1; /* ge holds; w1 + w2 = -1 + 1 */
    check(run({CMP12, 0x3A42A022, 0x9A820020}, c) && c.x[0] == 0xFFFFFFFF, "ccmn w: -1 + 1 -> Z set");

    c = {}; c.x[1] = 10; c.x[2] = 3; check(run({CMP12, 0x9A020020}, c) && c.x[0] == 14, "adc with C=1");
    c = {}; c.x[1] = 3; c.x[2] = 10; check(run({CMP12, 0x9A020020}, c) && c.x[0] == 13, "adc with C=0");
    c = {}; c.x[1] = 10; c.x[2] = 3; check(run({CMP12, 0xDA020020}, c) && c.x[0] == 7, "sbc with C=1");
    c = {}; c.x[1] = 3; c.x[2] = 10; check(run({CMP12, 0xDA020020}, c) && c.x[0] == static_cast<std::uint64_t>(-8), "sbc with C=0");
    /* cmp x1,x2 (C=1) ; adcs x0, x1, x2 ; cset w0, hs  with x1=~0, x2=0 -> carry out */
    c = {}; c.x[1] = ~0ull; c.x[2] = 0;
    check(run({CMP12, 0xBA020020, 0x1A9F37E0}, c) && c.x[0] == 1, "adcs carry out -> hs");
    /* cmp x1,x2 (C=1) ; sbcs x0, x1, x2 ; cset w0, eq  with x1 == x2 */
    c = {}; c.x[1] = 5; c.x[2] = 5;
    check(run({CMP12, 0xFA020020, 0x1A9F17E0}, c) && c.x[0] == 1, "sbcs zero result -> eq");
}

void test_alu() {
    mm_cpu_context c;

    c = {}; c.x[1] = 3; c.x[2] = 4;
    check(run({0x8B020020}, c) && c.x[0] == 7, "add x0, x1, x2");

    c = {}; c.x[1] = 10; c.x[2] = 3;
    check(run({0xCB020020}, c) && c.x[0] == 7, "sub x0, x1, x2");

    c = {}; c.x[0] = 21;
    check(run({0x8B000000}, c) && c.x[0] == 42, "add x0, x0, x0");

    c = {}; c.x[1] = 1; c.x[2] = 2;
    check(run({0x8B020C20}, c) && c.x[0] == 17, "add x0, x1, x2, lsl #3");

    c = {}; c.x[1] = 100; c.x[2] = 0x80;
    check(run({0xCB421020}, c) && c.x[0] == 92, "sub x0, x1, x2, lsr #4");

    c = {}; c.x[1] = 0; c.x[2] = 0xFFFFFFF0;
    check(run({0x4B821020}, c) && c.x[0] == 1, "sub w0, w1, w2, asr #4 (sign-extends w2)");

    c = {}; c.x[1] = 6; c.x[2] = 7;
    check(run({0x9B027C20}, c) && c.x[0] == 42, "mul x0, x1, x2");

    c = {}; c.x[1] = 0xF0;
    check(run({0xAA2103E0}, c) && c.x[0] == ~std::uint64_t{0xF0}, "mvn x0, x1");

    c = {}; c.x[1] = 0xFF; c.x[2] = 0x0F;
    check(run({0x8A220020}, c) && c.x[0] == 0xF0, "bic x0, x1, x2");

    c = {}; c.x[1] = 0xFF; c.x[2] = 0x0F;
    check(run({0xCA020020}, c) && c.x[0] == 0xF0, "eor x0, x1, x2");

    c = {}; c.x[1] = 0; c.x[2] = 0x12345678;
    check(run({0x2AC22020}, c) && c.x[0] == 0x78123456, "orr w0, w1, w2, ror #8");

    c = {}; c.x[1] = 0xFFFFFFFF;
    check(run({0x11000420}, c) && c.x[0] == 0, "add w0, w1, #1 wraps at 32 bits");
}

void test_xzr_sp() {
    mm_cpu_context c;

    c = {}; c.x[1] = 0x1234; c.x[31] = 0xDEAD;
    check(run({0xAA0103E0}, c) && c.x[0] == 0x1234 && c.x[31] == 0xDEAD, "mov x0, x1 reads xzr as zero, leaves sp");

    c = {}; c.x[0] = 5; c.x[1] = 7; c.x[31] = 0xDEAD;
    check(run({0xEB01001F}, c) && c.x[0] == 5 && c.x[31] == 0xDEAD, "cmp x0, x1 does not write sp");

    std::vector<std::uint64_t> stack(64);
    c = {}; c.x[1] = 0x77;
    c.x[31] = reinterpret_cast<std::uintptr_t>(stack.data() + stack.size());
    const std::uint64_t sp0 = c.x[31];
    /* sub sp, sp, #32 ; str x1, [sp, #8] ; ldr x0, [sp, #8] ; add sp, sp, #32 */
    check(run({0xD10083FF, 0xF90007E1, 0xF94007E0, 0x910083FF}, c) && c.x[0] == 0x77 && c.x[31] == sp0,
          "stack round-trip through sp");

    std::uint64_t value = 0xCAFE;
    c = {}; c.x[1] = reinterpret_cast<std::uintptr_t>(&value);
    check(run({0xF9400022}, c) && c.x[2] == 0xCAFE, "ldr x2, [x1]");
}

void test_mov_wide() {
    mm_cpu_context c;

    c = {};
    check(run({0xD2BFFFE0}, c) && c.x[0] == 0xFFFF0000, "movz x0, #0xffff, lsl #16");

    c = {};
    check(run({0x92BFFFE0}, c) && c.x[0] == 0xFFFFFFFF0000FFFFull, "movn x0, #0xffff, lsl #16");

    c = {};
    check(run({0x128000A0}, c) && c.x[0] == 0xFFFFFFFA, "movn w0, #5");

    c = {}; c.x[0] = 0x1111222233334444ull;
    check(run({0xF2D579A0}, c) && c.x[0] == 0x1111ABCD33334444ull, "movk x0, #0xabcd, lsl #32");
}

void test_loads_stores() {
    mm_cpu_context c;
    alignas(16) std::uint8_t mem[64];
    const auto base = reinterpret_cast<std::uintptr_t>(mem + 16);
    auto reset = [&] {
        for (std::size_t i = 0; i < sizeof mem; i++)
            mem[i] = static_cast<std::uint8_t>(0x80 + i - 16); /* mem+16 -> 0x80, 0x81, ... */
        c = {};
        c.x[1] = base;
    };

    reset(); check(run({0x39400C20}, c) && c.x[0] == 0x83, "ldrb w0, [x1, #3]");
    reset(); check(run({0x79400420}, c) && c.x[0] == 0x8382, "ldrh w0, [x1, #2]");
    reset(); check(run({0xB9400420}, c) && c.x[0] == 0x87868584, "ldr w0, [x1, #4]");
    reset(); check(run({0x39800420}, c) && c.x[0] == 0xFFFFFFFFFFFFFF81ull, "ldrsb x0, [x1, #1]");
    reset(); check(run({0x39C00420}, c) && c.x[0] == 0xFFFFFF81ull, "ldrsb w0, [x1, #1]");
    reset(); check(run({0x79800020}, c) && c.x[0] == 0xFFFFFFFFFFFF8180ull, "ldrsh x0, [x1]");
    reset(); check(run({0xB9800420}, c) && c.x[0] == 0xFFFFFFFF87868584ull, "ldrsw x0, [x1, #4]");

    reset(); c.x[2] = 0x1122334455667788ull;
    check(run({0x39001422}, c) && mem[16 + 5] == 0x88 && mem[16 + 6] == 0x86, "strb w2, [x1, #5]");
    reset(); c.x[2] = 0x1122334455667788ull;
    check(run({0x79000C22}, c) && mem[16 + 6] == 0x88 && mem[16 + 7] == 0x77 && mem[16 + 8] == 0x88, "strh w2, [x1, #6]");
    reset(); c.x[2] = 0x1122334455667788ull;
    check(run({0xB9000822}, c) && std::memcmp(mem + 24, "\x88\x77\x66\x55\x8c", 5) == 0, "str w2, [x1, #8]");

    reset(); check(run({0xF85F8020}, c) && c.x[0] == 0x7F7E7D7C7B7A7978ull, "ldur x0, [x1, #-8]");
    reset(); c.x[2] = 0xAABBCCDD;
    check(run({0xB81FC022}, c) && std::memcmp(mem + 12, "\xdd\xcc\xbb\xaa\x80", 5) == 0, "stur w2, [x1, #-4]");

    reset(); check(run({0xF8410420}, c) && c.x[0] == 0x8786858483828180ull && c.x[1] == base + 16, "ldr x0, [x1], #16");
    reset(); check(run({0xF8410C20}, c) && c.x[0] == 0x9796959493929190ull && c.x[1] == base + 16, "ldr x0, [x1, #16]!");
    reset(); c.x[2] = 0x0102030405060708ull;
    check(run({0xF81F8422}, c) && mem[16] == 0x08 && mem[23] == 0x01 && c.x[1] == base - 8, "str x2, [x1], #-8");

    reset(); c.x[2] = 8;  check(run({0xF8626820}, c) && c.x[0] == 0x8F8E8D8C8B8A8988ull, "ldr x0, [x1, x2]");
    reset(); c.x[2] = 1;  check(run({0xF8627820}, c) && c.x[0] == 0x8F8E8D8C8B8A8988ull, "ldr x0, [x1, x2, lsl #3]");
    reset(); c.x[2] = 0xFFFFFFFF00000001ull; /* only w2 counts */
    check(run({0xB8625820}, c) && c.x[0] == 0x87868584, "ldr w0, [x1, w2, uxtw #2]");
    reset(); c.x[2] = 0xFFFFFFFF; /* w2 = -1 */
    check(run({0x3862C820}, c) && c.x[0] == 0x7F, "ldrb w0, [x1, w2, sxtw]");

    reset(); check(run({0xA9400820}, c) && c.x[0] == 0x8786858483828180ull && c.x[2] == 0x8F8E8D8C8B8A8988ull, "ldp x0, x2, [x1]");
    reset(); c.x[2] = 0x1111; c.x[3] = 0x2222;
    check(run({0xA9010C22}, c) && mem[32] == 0x11 && mem[33] == 0x11 && mem[40] == 0x22 && mem[41] == 0x22, "stp x2, x3, [x1, #16]");
    reset(); check(run({0x29410820}, c) && c.x[0] == 0x8B8A8988 && c.x[2] == 0x8F8E8D8C, "ldp w0, w2, [x1, #8]");
    reset(); check(run({0x69400820}, c) && c.x[0] == 0xFFFFFFFF83828180ull && c.x[2] == 0xFFFFFFFF87868584ull, "ldpsw x0, x2, [x1]");

    /* stp x2, x3, [sp, #-32]! ; ldp x2, x3, [sp], #32  -> callee-save idiom */
    std::vector<std::uint64_t> stack(16);
    reset(); c.x[2] = 0xAAAA; c.x[3] = 0xBBBB;
    c.x[31] = reinterpret_cast<std::uintptr_t>(stack.data() + stack.size());
    const std::uint64_t sp0 = c.x[31];
    check(run({0xA9BE0FE2, 0xA8C20FE2}, c) && c.x[2] == 0xAAAA && c.x[3] == 0xBBBB && c.x[31] == sp0 &&
              stack[12] == 0xAAAA && stack[13] == 0xBBBB,
          "stp/ldp pre/post-index on sp");

    /* PC-relative: code is translated as if at 0x1000. */
    c = {}; check(run({0x10000800}, c) && c.x[0] == 0x1100, "adr x0, .+0x100");
    c = {}; check(run({0x90000000}, c) && c.x[0] == 0x1000, "adrp x0, page+0");
    c = {}; check(run({0xB0000000}, c) && c.x[0] == 0x2000, "adrp x0, page+1");
    {
        const auto i = decode(0x58000040);
        check(i.op == mm_a64_opcode::ldr && i.addr == mm_a64_addr::literal && i.imm == 0x1008 && i.mem_size == 8,
              "ldr x0, literal resolves to absolute VA");
    }

    reset(); check(run({0xF9800020}, c) && c.x[0] == 0, "prfm is a nop");
    check(decode(0x00000000).op == mm_a64_opcode::unknown, "udf stays unknown");
}

void test_bitfield_logical_imm() {
    mm_cpu_context c;
    constexpr std::uint64_t V = 0x8899AABBCCDDEEFFull;

    c = {}; c.x[1] = V;      check(run({0xD37CEC20}, c) && c.x[0] == (V << 4), "lsl x0, x1, #4");
    c = {}; c.x[1] = V;      check(run({0xD344FC20}, c) && c.x[0] == (V >> 4), "lsr x0, x1, #4");
    c = {}; c.x[1] = V;      check(run({0x9344FC20}, c) && c.x[0] == 0xF8899AABBCCDDEEFull, "asr x0, x1, #4");
    c = {}; c.x[1] = 0xCCDDEEFF; check(run({0x531C6C20}, c) && c.x[0] == 0xCDDEEFF0, "lsl w0, w1, #4");
    c = {}; c.x[1] = 0xCCDDEEFF; check(run({0x13047C20}, c) && c.x[0] == 0xFCCDDEEF, "asr w0, w1, #4");
    c = {}; c.x[1] = V;      check(run({0xD3483C20}, c) && c.x[0] == 0xEE, "ubfx x0, x1, #8, #8");
    c = {}; c.x[1] = V;      check(run({0x93483C20}, c) && c.x[0] == 0xFFFFFFFFFFFFFFEEull, "sbfx x0, x1, #8, #8");
    c = {}; c.x[1] = V;      check(run({0x53001C20}, c) && c.x[0] == 0xFF, "uxtb w0, w1");
    c = {}; c.x[1] = 0xCCDDEEFF; check(run({0x93407C20}, c) && c.x[0] == 0xFFFFFFFFCCDDEEFFull, "sxtw x0, w1");
    c = {}; c.x[1] = 0x1234EEFF; check(run({0x93403C20}, c) && c.x[0] == 0xFFFFFFFFFFFFEEFFull, "sxth x0, w1");
    c = {}; c.x[0] = 0x1111111111111111ull; c.x[1] = 0xAB;
    check(run({0xB3781C20}, c) && c.x[0] == 0x111111111111AB11ull, "bfi x0, x1, #8, #8");
    c = {}; c.x[0] = 0x1111111111111111ull; c.x[1] = 0xABCD;
    check(run({0xB3483C20}, c) && c.x[0] == 0x11111111111111ABull, "bfxil x0, x1, #8, #8");

    c = {}; c.x[1] = V; check(run({0x92401C20}, c) && c.x[0] == 0xFF, "and x0, x1, #0xff");
    c = {}; c.x[1] = 0x0F0F; check(run({0xB204CC20}, c) && c.x[0] == 0xF0F0F0F0F0F0FFFFull, "orr x0, x1, #0xf0f0...");
    c = {}; c.x[1] = 0x12345678; check(run({0x52103C20}, c) && c.x[0] == 0xEDCB5678, "eor w0, w1, #0xffff0000");
    c = {}; c.x[1] = 0xFFFFFFFF; check(run({0x12000020}, c) && c.x[0] == 1, "and w0, w1, #1");
    c = {}; c.x[1] = 0x8; c.x[31] = 0xDEAD; check(run({0xF27D003F}, c) && c.x[31] == 0xDEAD, "tst does not write sp");
}

void test_mul_div_shift() {
    mm_cpu_context c;

    c = {}; c.x[1] = 6; c.x[2] = 7; c.x[3] = 100; check(run({0x9B020C20}, c) && c.x[0] == 142, "madd");
    c = {}; c.x[1] = 6; c.x[2] = 7; c.x[3] = 100; check(run({0x9B028C20}, c) && c.x[0] == 58, "msub");
    c = {}; c.x[1] = 6; c.x[2] = 7; check(run({0x9B02FC20}, c) && c.x[0] == static_cast<std::uint64_t>(-42), "mneg");
    c = {}; c.x[1] = 0xFFFFFFFF; c.x[2] = 3; /* w1 = -1 */
    check(run({0x9B227C20}, c) && c.x[0] == static_cast<std::uint64_t>(-3), "smull");
    c = {}; c.x[1] = 0xFFFFFFFF; c.x[2] = 3;
    check(run({0x9BA27C20}, c) && c.x[0] == 0x2FFFFFFFDull, "umull");
    c = {}; c.x[1] = static_cast<std::uint64_t>(-1); c.x[2] = 5;
    check(run({0x9B427C20}, c) && c.x[0] == static_cast<std::uint64_t>(-1), "smulh (-1 * 5 -> high = -1)");
    c = {}; c.x[1] = 0x8000000000000000ull; c.x[2] = 4;
    check(run({0x9BC27C20}, c) && c.x[0] == 2, "umulh");

    c = {}; c.x[1] = 100; c.x[2] = 7; check(run({0x9AC20820}, c) && c.x[0] == 14, "udiv");
    c = {}; c.x[1] = 100; c.x[2] = 0; check(run({0x9AC20820}, c) && c.x[0] == 0, "udiv by zero -> 0");
    c = {}; c.x[1] = static_cast<std::uint64_t>(-100); c.x[2] = 7;
    check(run({0x9AC20C20}, c) && c.x[0] == static_cast<std::uint64_t>(-14), "sdiv negative");
    c = {}; c.x[1] = 0x8000000000000000ull; c.x[2] = static_cast<std::uint64_t>(-1);
    check(run({0x9AC20C20}, c) && c.x[0] == 0x8000000000000000ull, "sdiv INT64_MIN / -1 does not trap");
    c = {}; c.x[1] = 5; c.x[2] = 0; check(run({0x9AC20C20}, c) && c.x[0] == 0, "sdiv by zero -> 0");
    c = {}; c.x[1] = 0x80000000; c.x[2] = 0xFFFFFFFF;
    check(run({0x1AC20C20}, c) && c.x[0] == 0x80000000, "sdiv w INT32_MIN / -1");
    c = {}; c.x[1] = 0xFFFFFFF6; c.x[2] = 3; /* -10 / 3 */
    check(run({0x1AC20C20}, c) && c.x[0] == 0xFFFFFFFD, "sdiv w negative");

    c = {}; c.x[1] = 1; c.x[2] = 65; check(run({0x9AC22020}, c) && c.x[0] == 2, "lsl x by reg (mod 64)");
    c = {}; c.x[1] = 0x80000000; c.x[2] = 33; check(run({0x1AC22420}, c) && c.x[0] == 0x40000000, "lsr w by reg (mod 32)");
    c = {}; c.x[1] = static_cast<std::uint64_t>(-16); c.x[2] = 2;
    check(run({0x9AC22820}, c) && c.x[0] == static_cast<std::uint64_t>(-4), "asr x by reg");
    c = {}; c.x[1] = 0x12345678; c.x[2] = 8; check(run({0x1AC22C20}, c) && c.x[0] == 0x78123456, "ror w by reg");

    c = {}; c.x[1] = 0x0102030405060708ull; check(run({0xDAC00C20}, c) && c.x[0] == 0x0807060504030201ull, "rev x");
    c = {}; c.x[1] = 0x01020304; check(run({0x5AC00820}, c) && c.x[0] == 0x04030201, "rev w");
    c = {}; c.x[1] = 1; check(run({0xDAC01020}, c) && c.x[0] == 63, "clz x");
    c = {}; c.x[1] = 0; check(run({0xDAC01020}, c) && c.x[0] == 64, "clz x of 0");
    c = {}; c.x[1] = 1; check(run({0x5AC01020}, c) && c.x[0] == 31, "clz w");
}

void test_extended_addsub() {
    mm_cpu_context c;
    c = {}; c.x[1] = 100; c.x[2] = 0xFFFFFFFF00000001ull;
    check(run({0x8B224820}, c) && c.x[0] == 104, "add x0, x1, w2, uxtw #2");
    c = {}; c.x[1] = 100; c.x[2] = 0xFF; /* sxtb -> -1 */
    check(run({0xCB228020}, c) && c.x[0] == 101, "sub x0, x1, w2, sxtb");
    c = {}; c.x[31] = 0x1000; c.x[2] = 0x10;
    check(run({0x8B2263E0}, c) && c.x[0] == 0x1010, "add x0, sp, x2, uxtx reads sp");
    c = {}; c.x[1] = 5; c.x[2] = 0x8000; c.x[31] = 0xDEAD;
    check(run({0xEB22A03F}, c) && c.x[31] == 0xDEAD, "cmp x1, w2, sxth does not write sp");
}

void test_atomics() {
    mm_cpu_context c;
    alignas(16) std::uint64_t mem[4];
    const auto base = reinterpret_cast<std::uintptr_t>(mem);
    auto reset = [&] { mem[0] = 0x1122334455667788ull; mem[1] = 0; mem[2] = 0; mem[3] = 0; c = {}; c.x[1] = base; };

    reset(); check(run({0xC8DFFC20}, c) && c.x[0] == 0x1122334455667788ull, "ldar x0");
    reset(); check(run({0x08DFFC20}, c) && c.x[0] == 0x88, "ldarb w0");
    reset(); c.x[2] = 0xAABB; check(run({0xC89FFC22}, c) && mem[0] == 0xAABB, "stlr x2");
    reset(); c.x[2] = 0xAABB; check(run({0x089FFC22}, c) && mem[0] == 0x11223344556677BBull, "stlrb w2");

    /* ldxr x0,[x1] ; stxr w3, x2, [x1] : succeeds, status 0 */
    reset(); c.x[2] = 42; check(run({0xC85F7C20, 0xC8037C22}, c) && c.x[0] == 0x1122334455667788ull && c.x[3] == 0 && mem[0] == 42,
                                 "ldxr/stxr success");
    /* stxr without a preceding ldxr: status 1, memory untouched */
    reset(); c.x[2] = 42; check(run({0xC8037C22}, c) && c.x[3] == 1 && mem[0] == 0x1122334455667788ull, "stxr without monitor fails");
    /* ldaxr w0 ; (memory changes underneath: emulate by storing) ; stlxr w3, w2 fails */
    reset(); c.x[2] = 7; c.x[4] = 99;
    /* ldaxr w0,[x1] ; str x4,[x1] ; stlxr w3, w2, [x1] */
    check(run({0x885FFC20, 0xF9000024, 0x8803FC22}, c) && c.x[3] == 1 && mem[0] == 99, "stlxr fails after intervening store");

    reset(); c.x[0] = 0x1122334455667788ull; c.x[2] = 5;
    check(run({0xC8A07C22}, c) && mem[0] == 5 && c.x[0] == 0x1122334455667788ull, "cas success swaps and returns old");
    reset(); c.x[0] = 1; c.x[2] = 5;
    check(run({0xC8A07C22}, c) && mem[0] == 0x1122334455667788ull && c.x[0] == 0x1122334455667788ull, "cas failure returns current");
    reset(); c.x[0] = 0x55667788; c.x[2] = 9;
    check(run({0x88E0FC22}, c) && mem[0] == 0x1122334400000009ull && c.x[0] == 0x55667788, "casal w");
    reset(); c.x[0] = 0x88; c.x[2] = 0x01;
    check(run({0x08A07C22}, c) && mem[0] == 0x1122334455667701ull && c.x[0] == 0x88, "casb");

    reset(); mem[0] = 10; c.x[2] = 5; check(run({0xF8220020}, c) && mem[0] == 15 && c.x[0] == 10, "ldadd x");
    reset(); mem[0] = 0xFFFFFFFF; c.x[2] = 1; check(run({0xB8E20020}, c) && mem[0] == 0 && c.x[0] == 0xFFFFFFFF, "ldaddal w wraps");
    reset(); mem[0] = 0xFF; c.x[2] = 0x0F; check(run({0xF8221020}, c) && mem[0] == 0xF0 && c.x[0] == 0xFF, "ldclr");
    reset(); mem[0] = 0xFF; c.x[2] = 0x0F; check(run({0xF8222020}, c) && mem[0] == 0xF0 && c.x[0] == 0xFF, "ldeor");
    reset(); mem[0] = 0xF0; c.x[2] = 0x0F; check(run({0xF8223020}, c) && mem[0] == 0xFF && c.x[0] == 0xF0, "ldset");
    reset(); mem[0] = 0xFFFFFFFE; c.x[2] = 3; check(run({0xB8224020}, c) && mem[0] == 3 && c.x[0] == 0xFFFFFFFE, "ldsmax w (-2 vs 3)");
    reset(); mem[0] = 0xFFFFFFFE; c.x[2] = 3; check(run({0xB8225020}, c) && mem[0] == 0xFFFFFFFE, "ldsmin w keeps -2");
    reset(); mem[0] = 5; c.x[2] = static_cast<std::uint64_t>(-1); check(run({0xF8226020}, c) && mem[0] == ~0ull, "ldumax");
    reset(); mem[0] = 5; c.x[2] = static_cast<std::uint64_t>(-1); check(run({0xF8227020}, c) && mem[0] == 5, "ldumin");
    reset(); c.x[2] = 77; check(run({0xF8228020}, c) && mem[0] == 77 && c.x[0] == 0x1122334455667788ull, "swp x");
    reset(); c.x[2] = 0xAB; check(run({0x38228020}, c) && mem[0] == 0x11223344556677ABull && c.x[0] == 0x88, "swpb");
    reset(); mem[0] = 1; c.x[2] = 2; c.x[31] = 0xDEAD; check(run({0xF822003F}, c) && mem[0] == 3 && c.x[31] == 0xDEAD, "stadd (rt = xzr)");
}

std::uint64_t reference_u64_double(std::uint64_t value, unsigned rounding) {
    if (!value) return 0;
    unsigned exponent = 63u - std::countl_zero(value);
    std::uint64_t significand;
    if (exponent <= 52) significand = value << (52 - exponent);
    else {
        const auto shift = exponent - 52;
        significand = value >> shift;
        const auto tail = value & ((1ull << shift) - 1);
        const auto half = 1ull << (shift - 1);
        const bool increment = rounding == 0 ? tail > half || (tail == half && (significand & 1)) : rounding == 1 && tail;
        if (increment && ++significand == (1ull << 53)) { significand >>= 1; ++exponent; }
    }
    return (std::uint64_t(exponent + 1023) << 52) | (significand & ((1ull << 52) - 1));
}

void test_packed_u64_to_double() {
    const auto host_mxcsr = _mm_getcsr();
    const std::uint64_t values[] = {
        0, 1, 0xffffffffull, 0x100000000ull, (1ull << 52) - 1, (1ull << 52) + 1,
        (1ull << 53) - 1, (1ull << 53) + 1, (1ull << 53) + 3,
        (1ull << 63) - 1, 1ull << 63, (1ull << 63) + 1025, ~0ull,
    };
    for (unsigned mode = 0; mode < 4; ++mode) {
        for (auto instruction : {0x6e61d820u, 0x6e61d821u}) {
            const auto translated = translate({0xd51b4403, instruction, RET});
            mm_runtime_code code(translated);
            auto convert = [&](std::uint64_t first, std::uint64_t second) {
                mm_cpu_context c{};
                c.x[3] = std::uint64_t(mode) << 22; c.v[1][0] = first; c.v[1][1] = second;
                c.v[2][0] = 0x123456789abcdef0ull; c.v[2][1] = 0xfedcba9876543210ull; c.eflags = 0x8c3;
                const unsigned dest = instruction & 31;
                check(code.run(c, 0x1000) == mm_runtime_status::ok &&
                    c.v[dest][0] == reference_u64_double(first, mode) && c.v[dest][1] == reference_u64_double(second, mode),
                    "packed uint64->double matches integer IEEE rounding oracle, including alias and +0");
                check(c.v[2][0] == 0x123456789abcdef0ull && c.v[2][1] == 0xfedcba9876543210ull && c.eflags == 0x8c3,
                    "packed uint64->double preserves other vectors and NZCV");
                if (!dest) check(c.v[1][0] == first && c.v[1][1] == second, "packed uint64->double preserves a distinct source");
            };
            for (auto first : values) for (auto second : values) convert(first, second);
            std::uint64_t state = 0x123456789abcdef0ull;
            for (unsigned i = 0; i < 2048; ++i) {
                state ^= state << 13; state ^= state >> 7; state ^= state << 17;
                const auto first = state;
                state ^= state << 13; state ^= state >> 7; state ^= state << 17;
                convert(first, state);
            }
        }
    }
    _mm_setcsr(host_mxcsr);
}

void test_q_memory_boundaries() {
    for (unsigned offset = 0; offset < 16; ++offset) {
        alignas(16) std::uint8_t source[96], destination[96];
        for (unsigned i = 0; i < sizeof source; ++i) source[i] = static_cast<std::uint8_t>(i * 3 + offset);
        std::memset(destination, 0xcc, sizeof destination);
        mm_cpu_context c{};
        c.x[1] = reinterpret_cast<std::uintptr_t>(source + offset);
        c.x[2] = reinterpret_cast<std::uintptr_t>(destination + offset);
        c.eflags = 0x8c3;
        check(run({0x3cc10420, 0x3c810440}, c) && !std::memcmp(source + offset, destination + offset, 16) &&
            c.x[1] == reinterpret_cast<std::uintptr_t>(source + offset + 16) &&
            c.x[2] == reinterpret_cast<std::uintptr_t>(destination + offset + 16) && c.eflags == 0x8c3,
            "unaligned Q post-index roundtrip preserves both halves, writeback and NZCV");
        check(destination[offset + 16] == 0xcc && (!offset || destination[offset - 1] == 0xcc), "Q store stays within its 16-byte range");
        std::memset(destination, 0xcc, sizeof destination);
        c = {}; c.x[1] = reinterpret_cast<std::uintptr_t>(source + offset); c.x[2] = reinterpret_cast<std::uintptr_t>(destination + offset);
        check(run({0x3cc10c20, 0x3c810c40}, c) && !std::memcmp(source + offset + 16, destination + offset + 16, 16) &&
            c.x[1] == reinterpret_cast<std::uintptr_t>(source + offset + 16) &&
            c.x[2] == reinterpret_cast<std::uintptr_t>(destination + offset + 16), "unaligned Q pre-index roundtrip updates bases once");
        std::memset(destination, 0xcc, sizeof destination);
        c = {}; c.x[1] = reinterpret_cast<std::uintptr_t>(source + offset); c.x[2] = reinterpret_cast<std::uintptr_t>(destination + offset);
        check(run({0xacc10420, 0xac810440}, c) && !std::memcmp(source + offset, destination + offset, 32) &&
            c.x[1] == reinterpret_cast<std::uintptr_t>(source + offset + 32) &&
            c.x[2] == reinterpret_cast<std::uintptr_t>(destination + offset + 32), "unaligned Q pair roundtrip preserves both vectors and writeback");
    }
#if !defined(_WIN32)
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    auto *memory = static_cast<std::uint8_t *>(mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (memory == MAP_FAILED) { check(false, "allocate Q memory guard pages"); return; }
    std::memset(memory, 0x5a, page);
    if (mprotect(memory + page, page, PROT_NONE) != 0) {
        check(false, "protect Q memory guard page"); munmap(memory, page * 2); return;
    }
    const auto boundary = reinterpret_cast<std::uintptr_t>(memory + page);
    for (auto instruction : {0x3cc10420u, 0x3cc10c20u, 0x3c810420u}) {
        const auto code = translate({instruction, RET});
        mm_cpu_context c{};
        c.x[1] = boundary - (instruction == 0x3cc10c20u ? 24 : 8);
        c.v[0][0] = 0x123456789abcdef0ull; c.v[0][1] = 0xfedcba9876543210ull; c.eflags = 0x8c3;
        const auto original_base = c.x[1];
        check(mm_runtime_exec(code, 0x1000, c) == mm_runtime_status::memory_fault && c.fault_va == 0x1000 && c.fault_addr == boundary &&
            c.x[1] == original_base && c.v[0][0] == 0x123456789abcdef0ull && c.v[0][1] == 0xfedcba9876543210ull && c.eflags == 0x8c3,
            "cross-page Q fault leaves base, destination vector and NZCV unchanged");
    }
    munmap(memory, page * 2);
#endif
}

void test_simd_stage1() {
    mm_cpu_context c;
    alignas(16) std::uint8_t mem[64];
    const auto base = reinterpret_cast<std::uintptr_t>(mem);
    auto reset = [&] {
        for (std::size_t i = 0; i < sizeof mem; i++) mem[i] = static_cast<std::uint8_t>(i);
        c = {}; c.x[1] = base;
        c.v[1][0] = 0x1111111111111111ull; c.v[1][1] = 0x2222222222222222ull;
        c.v[2][0] = 0xF0F0F0F0F0F0F0F0ull; c.v[2][1] = 0x0F0F0F0F0F0F0F0Full;
    };

    reset(); check(run({0x3DC00020}, c) && c.v[0][0] == 0x0706050403020100ull && c.v[0][1] == 0x0F0E0D0C0B0A0908ull, "ldr q0");
    reset(); check(run({0x3D800420}, c) && std::memcmp(mem + 16, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0, "str q0 (zero) ");
    reset(); check(run({0xFD400420}, c) && c.v[0][0] == 0x0F0E0D0C0B0A0908ull && c.v[0][1] == 0, "ldr d0 zeroes upper half");
    reset(); c.v[0][1] = 0xDEAD; check(run({0xBD400020}, c) && c.v[0][0] == 0x03020100 && c.v[0][1] == 0, "ldr s0");
    reset(); c.v[0][0] = 0xAB; check(run({0x3D000C20}, c) && mem[3] == 0xAB && mem[4] == 4, "str b0");
    reset(); check(run({0xAD400420}, c) && c.v[0][1] == 0x0F0E0D0C0B0A0908ull && c.v[1][0] == 0x1716151413121110ull, "ldp q0, q1");
    reset(); c.v[0][0] = 1; c.v[0][1] = 2; c.v[1][0] = 3; c.v[1][1] = 4;
    check(run({0xAD010420}, c) && mem[32] == 1 && mem[40] == 2 && mem[48] == 3 && mem[56] == 4, "stp q0, q1, [x1, #32]");
    reset(); c.x[2] = 1; check(run({0x3CE27820}, c) && c.v[0][0] == 0x1716151413121110ull, "ldr q0, [x1, x2, lsl #4]");

    reset(); check(run({0x9E660020}, c) && c.x[0] == 0x1111111111111111ull, "fmov x0, d1");
    reset(); check(run({0x1E260020}, c) && c.x[0] == 0x11111111, "fmov w0, s1");
    reset(); c.x[1] = 0xABCD; c.v[0][1] = 7; check(run({0x9E670020}, c) && c.v[0][0] == 0xABCD && c.v[0][1] == 0, "fmov d0, x1");
    reset(); c.x[1] = 0x1ABCD1234ull; check(run({0x1E270020}, c) && c.v[0][0] == 0xABCD1234 && c.v[0][1] == 0, "fmov s0, w1");
    reset(); check(run({0x9EAE0020}, c) && c.x[0] == 0x2222222222222222ull, "fmov x0, v1.d[1]");
    reset(); c.x[1] = 9; c.v[0][0] = 5; check(run({0x9EAF0020}, c) && c.v[0][0] == 5 && c.v[0][1] == 9, "fmov v0.d[1], x1 keeps low half");
    reset(); c.x[1] = 0xEE; c.v[0][0] = 0; check(run({0x4E071C20}, c) && c.v[0][0] == 0xEE000000, "mov v0.b[3], w1");
    reset(); c.v[1][0] = 0x00AA000000000000ull | 0x0000110000000000ull; /* b[5] = 0x11 */
    check(run({0x0E0B3C20}, c) && c.x[0] == 0x11, "umov w0, v1.b[5]");
    reset(); check(run({0x4E183C20}, c) && c.x[0] == 0x2222222222222222ull, "umov x0, v1.d[1]");
    reset(); c.v[1][0] = 0xFFFE0000; /* h[1] = 0xFFFE */
    check(run({0x4E062C20}, c) && c.x[0] == static_cast<std::uint64_t>(-2), "smov x0, v1.h[1]");
    reset(); c.x[1] = 0x1AB; check(run({0x4E010C20}, c) && c.v[0][0] == 0xABABABABABABABABull && c.v[0][1] == 0xABABABABABABABABull, "dup v0.16b, w1");
    reset(); c.x[1] = 0x1F0000001ull; check(run({0x4E040C20}, c) && c.v[0][0] == 0xF0000001F0000001ull && c.v[0][1] == 0xF0000001F0000001ull, "dup v0.4s, w1");
    reset(); c.x[1] = 77; check(run({0x4E080C20}, c) && c.v[0][0] == 77 && c.v[0][1] == 77, "dup v0.2d, x1");

    reset(); c.v[0][0] = 1; c.v[0][1] = 1; check(run({0x6F00E400}, c) && c.v[0][0] == 0 && c.v[0][1] == 0, "movi v0.2d, #0");
    reset(); check(run({0x4F01E540}, c) && c.v[0][0] == 0x2A2A2A2A2A2A2A2Aull && c.v[0][1] == 0x2A2A2A2A2A2A2A2Aull, "movi v0.16b, #0x2a");
    reset(); check(run({0x4F0327E0}, c) && c.v[0][0] == 0x00007F0000007F00ull, "movi v0.4s, #0x7f, lsl #8");
    reset(); check(run({0x6F000420}, c) && c.v[0][0] == 0xFFFFFFFEFFFFFFFEull && c.v[0][1] == 0xFFFFFFFEFFFFFFFEull, "mvni v0.4s, #1");
    reset(); check(run({0x6F05E540}, c) && c.v[0][0] == 0xFF00FF00FF00FF00ull && c.v[0][1] == 0xFF00FF00FF00FF00ull, "movi v0.2d, byte mask");

    reset(); check(run({0x4E221C20}, c) && c.v[0][0] == 0x1010101010101010ull && c.v[0][1] == 0x0202020202020202ull, "and v.16b");
    reset(); check(run({0x4E621C20}, c) && c.v[0][0] == 0x0101010101010101ull && c.v[0][1] == 0x2020202020202020ull, "bic v.16b");
    reset(); c.v[0][1] = 0xDEAD; check(run({0x0EA21C20}, c) && c.v[0][0] == 0xF1F1F1F1F1F1F1F1ull && c.v[0][1] == 0, "orr v.8b zeroes upper");
    reset(); check(run({0x6E221C20}, c) && c.v[0][0] == 0xE1E1E1E1E1E1E1E1ull && c.v[0][1] == 0x2D2D2D2D2D2D2D2Dull, "eor v.16b");
    reset(); c.v[0][0] = 0x00000000FFFFFFFFull; c.v[0][1] = 0xFFFFFFFF00000000ull;
    check(run({0x6E621C20}, c) && c.v[0][0] == 0xF0F0F0F011111111ull && c.v[0][1] == 0x222222220F0F0F0Full, "bsl");
}

/* NEON checks against scalar reference computations. Vectors are viewed
 * as little-endian byte arrays; lane helpers extract/insert elements. */
struct vec { std::uint8_t b[16]; };

vec vget(const mm_cpu_context &c, int n) { vec v; std::memcpy(v.b, c.v[n], 16); return v; }
void vset(mm_cpu_context &c, int n, const vec &v) { std::memcpy(c.v[n], v.b, 16); }
std::uint64_t lane(const vec &v, int e, int i) { std::uint64_t x = 0; std::memcpy(&x, v.b + i * e, static_cast<std::size_t>(e)); return x; }
void set_lane(vec &v, int e, int i, std::uint64_t x) { std::memcpy(v.b + i * e, &x, static_cast<std::size_t>(e)); }
std::int64_t slane(const vec &v, int e, int i) {
    const std::uint64_t x = lane(v, e, i);
    const int sh = 64 - 8 * e;
    return static_cast<std::int64_t>(x << sh) >> sh;
}
std::uint64_t emask(int e) { return e == 8 ? ~0ull : (1ull << (8 * e)) - 1; }

vec pattern(std::uint8_t seed) { vec v; for (int i = 0; i < 16; i++) v.b[i] = static_cast<std::uint8_t>(seed + i * 37); return v; }

bool veq(const vec &a, const vec &b, bool q) { return std::memcmp(a.b, b.b, q ? 16 : 8) == 0 && (q || lane(a, 8, 1) == 0); }

void test_variable_shift_counts() {
    // Every signed low-byte count, different counts in neighbouring lanes,
    // ignored upper count bytes, all alias combinations and both backends.
    for (unsigned size = 0; size < 4; ++size) {
        const int e = 1 << size, bits = 8 * e;
        for (bool full : {false, true}) {
            const bool scalar = size == 3 && !full;
            const auto shape = scalar ? 0x5e200000u : full ? 0x4e200000u : 0x0e200000u;
            for (bool sg : {false, true}) {
                for (unsigned alias = 0; alias < 4; ++alias) {
                    const unsigned dest = alias == 0 ? 0 : alias == 2 ? 2 : 1;
                    const unsigned count_reg = alias == 3 ? 1 : 2;
                    const auto instruction = shape | (sg ? 0 : 1u << 29) | (size << 22) |
                        (count_reg << 16) | 0x4420u | dest;
                    const auto decoded = decode(instruction);
                    check(decoded.op == mm_a64_opcode::vop && decoded.vop == (sg ? mm_a64_vop::sshl : mm_a64_vop::ushl) &&
                        decoded.elem_size == e && decoded.q == full, "decode variable shift shape");
                    const auto translated = translate({instruction, RET});
                    mm_runtime_code code(translated);
                    for (unsigned count = 0; count < 256; ++count) {
                        for (unsigned sample = 0; sample < 6; ++sample) {
                            mm_cpu_context c{};
                            for (unsigned reg = 0; reg < 32; ++reg) {
                                c.x[reg] = 0x1234567800000000ull | reg;
                                c.v[reg][0] = 0x1122334455667788ull ^ reg;
                                c.v[reg][1] = 0x8877665544332211ull ^ reg;
                            }
                            c.x[30] = 0; c.eflags = 0x8c3; c.fpsr = 0x8000000; c.fpcr = 0;
                            vec input{}, shifts{};
                            const std::uint64_t mask = emask(e), sign = 1ull << (bits - 1);
                            for (int i = 0; i < 16 / e; ++i) {
                                const auto value = sample == 0 ? 0 : sample == 1 ? mask : sample == 2 ? sign :
                                    sample == 3 ? sign - 1 : sample == 4 ? 1 :
                                    (0x9e3779b97f4a7c15ull * (count + 3u * static_cast<unsigned>(i) + 1)) & mask;
                                set_lane(input, e, i, value);
                                const auto byte = (count + 73u * static_cast<unsigned>(i)) & 255;
                                set_lane(shifts, e, i, (0xa5de8899ff001100ull & mask & ~0xffull) | byte);
                            }
                            vset(c, 1, input);
                            vset(c, static_cast<int>(count_reg), shifts);
                            const auto original = c;
                            const auto data = vget(c, 1), counts = vget(c, static_cast<int>(count_reg));
                            vec result{};
                            for (int i = 0; i < (full ? 16 : 8) / e; ++i) {
                                const auto value = lane(data, e, i);
                                const int byte = counts.b[i * e], n = byte < 128 ? byte : byte - 256;
                                std::uint64_t shifted;
                                if (n >= 0) shifted = n >= bits ? 0 : (value << n) & mask;
                                else if (-n >= bits) shifted = sg && (value & sign) ? mask : 0;
                                else {
                                    shifted = value >> -n;
                                    if (sg && (value & sign)) shifted |= mask ^ (mask >> -n);
                                }
                                set_lane(result, e, i, shifted);
                            }
                            auto expected = original;
                            vset(expected, static_cast<int>(dest), result);
                            const auto status = code.run(c, 0x1000);
                            const bool ok = status == mm_runtime_status::ok &&
                                !std::memcmp(c.v, expected.v, sizeof c.v) && !std::memcmp(c.x, original.x, sizeof c.x) &&
                                c.eflags == original.eflags && c.fpcr == original.fpcr && c.fpsr == original.fpsr;
                            if (!ok) {
                                std::fprintf(stderr, "variable shift word=%08x count=%u sample=%u status=%u result=%016llx/%016llx expected=%016llx/%016llx\n",
                                    instruction, count, sample, static_cast<unsigned>(status),
                                    static_cast<unsigned long long>(c.v[dest][0]), static_cast<unsigned long long>(c.v[dest][1]),
                                    static_cast<unsigned long long>(expected.v[dest][0]), static_cast<unsigned long long>(expected.v[dest][1]));
                                check(false, "USHL/SSHL full signed-byte matrix preserves guest state, lane bounds and aliases");
                                return;
                            }
                        }
                    }
                }
            }
        }
    }
}


/* ---- ARMv8 crypto extension ---- */

std::uint32_t rol32(std::uint32_t x, int n) { return n == 0 ? x : (x << n) | (x >> (32 - n)); }
std::uint32_t ror32(std::uint32_t x, int n) { return n == 0 ? x : (x >> n) | (x << (32 - n)); }

/* The SHA-1/SHA-256 instruction semantics, modelled independently from the
 * emission (transcribed from the ARM ARM pseudocode). */
struct ref_sha1 { std::uint32_t w[4], e; };
void ref_sha1_rounds(ref_sha1 &s, const std::uint32_t w[4], int fn) {
    for (int i = 0; i < 4; i++) {
        const std::uint32_t t = fn == 0 ? (s.w[1] & (s.w[2] ^ s.w[3])) ^ s.w[3]
                              : fn == 1 ? s.w[1] ^ s.w[2] ^ s.w[3]
                                        : (s.w[1] & s.w[2]) | ((s.w[1] | s.w[2]) & s.w[3]);
        const std::uint32_t full = t + rol32(s.w[0], 5) + s.e + w[i];
        s.e = s.w[3];
        s.w[3] = s.w[2];
        s.w[2] = ror32(s.w[1], 2);
        s.w[1] = s.w[0];
        s.w[0] = full;
    }
}

std::uint32_t sha256_cho(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (x & (y ^ z)) ^ z; }
std::uint32_t sha256_maj(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (x & y) | ((x | y) & z); }
std::uint32_t sha256_S0(std::uint32_t x) { return ror32(x, 2) ^ ror32(x, 13) ^ ror32(x, 22); }
std::uint32_t sha256_S1(std::uint32_t x) { return ror32(x, 6) ^ ror32(x, 11) ^ ror32(x, 25); }
std::uint32_t sha256_s0(std::uint32_t x) { return ror32(x, 7) ^ ror32(x, 18) ^ (x >> 3); }
std::uint32_t sha256_s1(std::uint32_t x) { return ror32(x, 17) ^ ror32(x, 19) ^ (x >> 10); }

void ref_sha256h(std::uint32_t d[4], std::uint32_t n[4], const std::uint32_t m[4]) {
    for (int i = 0; i < 4; i++) {
        const std::uint32_t t = sha256_cho(n[0], n[1], n[2]) + n[3] + sha256_S1(n[0]) + m[i];
        n[3] = n[2]; n[2] = n[1]; n[1] = n[0];
        n[0] = d[3] + t;
        const std::uint32_t full = t + sha256_maj(d[0], d[1], d[2]) + sha256_S0(d[0]);
        d[3] = d[2]; d[2] = d[1]; d[1] = d[0];
        d[0] = full;
    }
}
void ref_sha256h2(std::uint32_t d[4], const std::uint32_t n[4], const std::uint32_t m[4]) {
    for (int i = 0; i < 4; i++) {
        const std::uint32_t t = sha256_cho(d[0], d[1], d[2]) + d[3] + sha256_S1(d[0]) + m[i];
        d[3] = d[2]; d[2] = d[1]; d[1] = d[0];
        d[0] = n[3 - i] + t;
    }
}

/* Runs a snippet that may exceed one page. */
bool run_long(const std::vector<std::uint32_t> &words, mm_cpu_context &ctx) {
    std::vector<std::uint8_t> code((words.size() + 1) * 4);
    for (std::size_t i = 0; i < words.size(); i++) std::memcpy(&code[i * 4], &words[i], 4);
    std::memcpy(&code[words.size() * 4], &RET, 4);
    mm_translation_result r;
    mm_translate_buffer(code, 0x1000, r);
    if (r.unsupported_count != 0)
        return false;
    return mm_runtime_exec(r, 0x1000, ctx) == mm_runtime_status::ok;
}

std::uint32_t lane32(const mm_cpu_context &c, int vreg, int lane) {
    return static_cast<std::uint32_t>(c.v[vreg][lane / 2] >> (32 * (lane % 2)));
}
void set_lane32(mm_cpu_context &c, int vreg, int lane, std::uint32_t x) {
    const std::uint64_t m = ~(0xFFFFFFFFull << (32 * (lane % 2)));
    c.v[vreg][lane / 2] = (c.v[vreg][lane / 2] & m) | (static_cast<std::uint64_t>(x) << (32 * (lane % 2)));
}

void test_crypto() {
    const auto sha1_3reg = [](std::uint32_t op6, std::uint32_t rd, std::uint32_t rn, std::uint32_t rm) {
        return 0x5E000000u | (op6 << 10) | (rm << 16) | (rn << 5) | rd;
    };
    const auto sha1_2reg = [](std::uint32_t op6, std::uint32_t rd, std::uint32_t rn) {
        return 0x5E280000u | (op6 << 10) | (rn << 5) | rd;
    };

    /* sha1h: lane 0 rotated, upper lanes zeroed. */
    mm_cpu_context c{};
    set_lane32(c, 1, 0, 0x12345678);
    set_lane32(c, 0, 0, 0xDEADBEEF); set_lane32(c, 0, 1, 1); set_lane32(c, 0, 2, 2); set_lane32(c, 0, 3, 3);
    check(run({sha1_2reg(0x2, 0, 1)}, c) && lane32(c, 0, 0) == ror32(0x12345678, 2) &&
              c.v[0][1] == 0, "sha1h");

    /* sha1c/p/m against the reference. */
    for (int fn = 0; fn < 3; fn++) {
        ref_sha1 s{{0x01234567, 0x89ABCDEF, 0xFEDCBA98, 0x76543210}, 0xC3D2E1F0};
        const std::uint32_t w[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
        mm_cpu_context cc{};
        for (int l = 0; l < 4; l++) set_lane32(cc, 0, l, s.w[l]);
        set_lane32(cc, 1, 0, s.e);
        for (int l = 0; l < 4; l++) set_lane32(cc, 2, l, w[l]);
        check(run({sha1_3reg(static_cast<std::uint32_t>(fn * 4), 0, 1, 2)}, cc), "sha1 round group runs");
        ref_sha1_rounds(s, w, fn);
        bool ok = true;
        for (int l = 0; l < 4; l++) ok = ok && lane32(cc, 0, l) == s.w[l];
        ok = ok && lane32(cc, 1, 0) == 0xC3D2E1F0; /* Vn is read-only */
        check(ok, fn == 0 ? "sha1c" : fn == 1 ? "sha1p" : "sha1m");
    }

    /* sha1su0: result = (Vn.lo :: Vd.hi) ^ Vd ^ Vm. */
    {
        mm_cpu_context cc{};
        set_lane32(cc, 0, 0, 0x10000001); set_lane32(cc, 0, 1, 0x20000002);
        set_lane32(cc, 0, 2, 0x30000003); set_lane32(cc, 0, 3, 0x40000004);
        set_lane32(cc, 1, 0, 0x50000005); set_lane32(cc, 1, 1, 0x60000006);
        set_lane32(cc, 1, 2, 0x70000007); set_lane32(cc, 1, 3, 0x80000008);
        set_lane32(cc, 2, 0, 0x90000009); set_lane32(cc, 2, 1, 0xA000000A);
        set_lane32(cc, 2, 2, 0xB000000B); set_lane32(cc, 2, 3, 0xC000000C);
        check(run({sha1_3reg(0xC, 0, 1, 2)}, cc), "sha1su0 runs");
        /* result.lo (lanes 0,1) = Vd.hi ^ Vd.lo ^ Vm.lo; lanes 2,3 get
         * Vn.lo ^ Vd.hi ^ Vm.hi — the 64-bit halves cross. */
        const bool ok = lane32(cc, 0, 0) == (0x30000003u ^ 0x10000001u ^ 0x90000009u) &&
                        lane32(cc, 0, 1) == (0x40000004u ^ 0x20000002u ^ 0xA000000Au) &&
                        lane32(cc, 0, 2) == (0x50000005u ^ 0x30000003u ^ 0xB000000Bu) &&
                        lane32(cc, 0, 3) == (0x60000006u ^ 0x40000004u ^ 0xC000000Cu);
        check(ok, "sha1su0");
    }

    /* sha1su1: T = Vd ^ (Vn >> 32); lane i = rol(T[i], 1), lane 3 ^= rol(T0, 2). */
    {
        mm_cpu_context cc{};
        set_lane32(cc, 0, 0, 0x00000004); set_lane32(cc, 0, 1, 0x00000003);
        set_lane32(cc, 0, 2, 0x00000002); set_lane32(cc, 0, 3, 0x00000001);
        set_lane32(cc, 1, 0, 0x00000010); set_lane32(cc, 1, 1, 0x00000020);
        set_lane32(cc, 1, 2, 0x00000030); set_lane32(cc, 1, 3, 0x00000040);
        check(run({sha1_2reg(0x6, 0, 1)}, cc), "sha1su1 runs");
        const std::uint32_t t[4] = {0x00000004u ^ 0x00000020u, 0x00000003u ^ 0x00000030u,
                                    0x00000002u ^ 0x00000040u, 0x00000001u};
        const bool ok = lane32(cc, 0, 0) == rol32(t[0], 1) && lane32(cc, 0, 1) == rol32(t[1], 1) &&
                        lane32(cc, 0, 2) == rol32(t[2], 1) &&
                        lane32(cc, 0, 3) == (rol32(t[3], 1) ^ rol32(t[0], 2));
        check(ok, "sha1su1");
    }

    /* sha256h/h2 against the reference. */
    for (int part = 0; part < 2; part++) {
        std::uint32_t d[4] = {0x01234567, 0x89ABCDEF, 0xFEDCBA98, 0x76543210};
        std::uint32_t n[4] = {0x95A24C2C, 0x1F8D1A2F, 0x550D6C2E, 0xB3E85A1A};
        const std::uint32_t m[4] = {0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5};
        mm_cpu_context cc{};
        for (int l = 0; l < 4; l++) set_lane32(cc, 0, l, d[l]);
        for (int l = 0; l < 4; l++) set_lane32(cc, 1, l, n[l]);
        for (int l = 0; l < 4; l++) set_lane32(cc, 2, l, m[l]);
        const bool ran = run({sha1_3reg(part == 0 ? 0x10u : 0x14u, 0, 1, 2)}, cc);
        check(ran, "sha256 hash group runs");
        if (part == 0) ref_sha256h(d, n, m); else ref_sha256h2(d, n, m);
        bool ok = ran;
        for (int l = 0; l < 4; l++) ok = ok && lane32(cc, 0, l) == d[l];
        check(ok, part == 0 ? "sha256h" : "sha256h2");
    }

    /* sha256su0/su1 against the reference. */
    {
        std::uint32_t d[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
        std::uint32_t n[4] = {0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC, 0xDDDDDDDD};
        mm_cpu_context cc{};
        for (int l = 0; l < 4; l++) set_lane32(cc, 0, l, d[l]);
        for (int l = 0; l < 4; l++) set_lane32(cc, 1, l, n[l]);
        check(run({sha1_2reg(0xA, 0, 1)}, cc), "sha256su0 runs");
        const std::uint32_t s0v[4] = {sha256_s0(d[1]), sha256_s0(d[2]), sha256_s0(d[3]), sha256_s0(n[0])};
        bool ok = true;
        for (int l = 0; l < 4; l++) ok = ok && lane32(cc, 0, l) == d[l] + s0v[l];
        check(ok, "sha256su0");

        const std::uint32_t m[4] = {0x13579BDF, 0x2468ACE0, 0x13579BDF, 0x2468ACE0};
        for (int l = 0; l < 4; l++) set_lane32(cc, 0, l, d[l]);
        for (int l = 0; l < 4; l++) set_lane32(cc, 2, l, m[l]);
        check(run({sha1_3reg(0x18, 0, 1, 2)}, cc), "sha256su1 runs");
        std::uint32_t r[4] = {d[0], d[1], d[2], d[3]};
        r[0] += sha256_s1(m[2]) + n[1];
        r[1] += sha256_s1(m[3]) + n[2];
        r[2] += sha256_s1(r[0]) + n[3];
        r[3] += sha256_s1(r[1]) + m[0];
        ok = true;
        for (int l = 0; l < 4; l++) ok = ok && lane32(cc, 0, l) == r[l];
        check(ok, "sha256su1");
    }

    /* AES rounds: known answers for the state 00 11 22 .. ff with a zero key
     * vector (and the inverse round, the column mixer and its inverse). */
    {
        /* The vectors are in guest byte order: lane 0's low byte is the
         * state's byte 0 (0x33 here, not 0x00). */
        c = {};
        c.v[1][0] = 0x4455667700112233ull; c.v[1][1] = 0xCCDDEEFF8899AABBull; /* bytes 33 22 .. cc */
        check(run({0x4E284820}, c) && c.v[0][0] == 0x63C1ACF54BEE33C3ull &&
                  c.v[0][1] == 0xC4FC93161B8228EAull, "aese");
        c = {};
        c.v[1][0] = 0x4455667700112233ull; c.v[1][1] = 0xCCDDEEFF8899AABBull;
        check(run({0x4E285820}, c) && c.v[0][0] == 0x97C9940286F99966ull &&
                  c.v[0][1] == 0x52ED627D27E3D3FEull, "aesd");
        c = {};
        c.v[1][0] = 0x4455667700112233ull; c.v[1][1] = 0xCCDDEEFF8899AABBull; /* Rn = 1 */
        check(run({0x4E286820}, c) && c.v[0][0] == 0x2277005566334411ull &&
                  c.v[0][1] == 0xAAFF88DDEEBBCC99ull, "aesmc");
        c = {};
        c.v[1][0] = 0x4455667700112233ull; c.v[1][1] = 0xCCDDEEFF8899AABBull;
        check(run({0x4E287820}, c) && c.v[0][0] == 0xAAFF88DDEEBBCC99ull &&
                  c.v[0][1] == 0x2277005566334411ull, "aesimc");
    }

    /* End to end: SHA-1("abc") through the crypto instructions, using the
     * round/schedule structure of the ARMv8 accelerated implementations. */
    {
        const auto ins_w = [&](std::vector<std::uint32_t> &code, std::uint32_t vreg, int lane) {
            code.push_back((1u << 30) | (0x70u << 21) | ((static_cast<std::uint32_t>(lane) << 3 | 4u) << 16) |
                           (3u << 11) | (1u << 10) | (6u << 5) | vreg); /* ins vN.s[lane], w6 */
        };
        const auto k_load = [&](std::vector<std::uint32_t> &code, std::uint32_t vreg, std::uint32_t k) {
            code.push_back(0x52800000u | ((k & 0xFFFF) << 5) | 6);            /* movz w6, #imm16 */
            code.push_back(0x72800000u | (1u << 21) | ((k >> 16) << 5) | 6);               /* movk w6, #imm16, lsl #16 */
            /* dup would read V6's own lane, which the context keeps apart from
             * the GPR file — replicate through ins instead. */
            for (int lane = 0; lane < 4; lane++) ins_w(code, vreg, lane);
        };
        std::vector<std::uint32_t> code;
        /* dga saved copy in v14, e initial in v15; working state v0, e regs v1/v2.
         * The state words and e go in through movz/movk + ins (the snippet has
         * no memory to load from). */
        const std::uint32_t init_a[4] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476};
        for (int l = 0; l < 4; l++) {
            code.push_back(0x52800000u | ((init_a[l] & 0xFFFF) << 5) | 6);
            code.push_back(0x72800000u | (1u << 21) | ((init_a[l] >> 16) << 5) | 6);
            code.push_back((1u << 30) | (0x70u << 21) | ((static_cast<std::uint32_t>(l) << 3 | 4u) << 16) |
                           (3u << 11) | (1u << 10) | (6u << 5) | 14u); /* ins v14.s[l], w6 */
        }
        /* v15.s[0] = e0; v1.s[0] = e0 (the working e alternates between v1/v2) */
        for (int vreg : {15, 1}) {
            code.push_back(0x52800000u | ((0xC3D2E1F0u & 0xFFFF) << 5) | 6);
            code.push_back(0x72800000u | (1u << 21) | ((0xC3D2E1F0u >> 16) << 5) | 6);
            code.push_back((1u << 30) | (0x70u << 21) | (4u << 16) | (3u << 11) | (1u << 10) | (6u << 5) |
                           static_cast<std::uint32_t>(vreg)); /* ins vN.s[0], w6 */
        }
        /* round constants into v4..v7 */
        const std::uint32_t rc[4] = {0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6};
        for (int i = 0; i < 4; i++) k_load(code, 4 + i, rc[i]);
        /* orr v0.16b, v14, v14: working abcd = initial */
        code.push_back((1u << 30) | (0x0Eu << 24) | (2u << 22) | (1u << 21) | (0x7u << 10) |
                       (14u << 16) | (14u << 5) | 0u);
        /* message: W0 = 61626380, W15 = 180 ("abc" padded); zero lanes skipped */
        const auto set_w = [&](std::uint32_t vreg, int lane, std::uint32_t val) {
            if (val == 0) return;
            code.push_back(0x52800000u | ((val & 0xFFFF) << 5) | 6);
            code.push_back(0x72800000u | (1u << 21) | ((val >> 16) << 5) | 6);
            code.push_back((1u << 30) | (0x70u << 21) | ((static_cast<std::uint32_t>(lane) << 3 | 4u) << 16) |
                           (3u << 11) | (1u << 10) | (6u << 5) | vreg);
        };
        set_w(8, 0, 0x61626380);
        set_w(11, 3, 0x00000018); /* "abc" is 24 bits */
        for (int g = 0; g < 20; g++) {
            const std::uint32_t r0 = 8 + g % 4, r1 = 8 + (g + 1) % 4, r2 = 8 + (g + 2) % 4, r3 = 8 + (g + 3) % 4;
            const std::uint32_t rc_reg = 4 + g / 5;
            /* t = W[4g..4g+3] + K — before the schedule overwrites r0 */
            code.push_back((1u << 30) | (0x0Eu << 24) | (2u << 22) | (1u << 21) | (0x10u << 11) | (1u << 10) |
                           (r0 << 16) | (rc_reg << 5) | 3u); /* add v3.4s, r0, rc */
            if (g < 16)
                code.push_back(sha1_3reg(0xC, r0, r1, r2)); /* sha1su0: W[4g+16..4g+19] */
            /* e for the next group = rol30(a), read before the rounds move a */
            const std::uint32_t e_dst = g % 2 == 0 ? 2 : 1;
            const std::uint32_t e_src = g % 2 == 0 ? 1 : 2;
            code.push_back(sha1_2reg(0x2, e_dst, 0)); /* sha1h e_next, dga.s[0] */
            /* round function: groups of 4 rounds; SHA-1 uses choose (0-19),
             * parity (20-39), majority (40-59), parity (60-79) */
            const std::uint32_t f6 = g < 5 ? 0x0u : g < 10 ? 0x4u : g < 15 ? 0x8u : 0x4u;
            code.push_back(sha1_3reg(f6, 0, e_src, 3)); /* sha1<op> q0, s_e, q_t */
            if (g < 16)
                code.push_back(sha1_2reg(0x6, r0, r3)); /* sha1su1 */
        }
        /* accumulate: v14 += v0 (abcd); e_final sits in v1 after group 19 */
        code.push_back((1u << 30) | (0x0Eu << 24) | (2u << 22) | (1u << 21) | (0x10u << 11) | (1u << 10) |
                       (0u << 16) | (14u << 5) | 14u); /* add v14.4s, v14, v0 */

        mm_cpu_context cc{};
        check(run_long(code, cc), "sha1 e2e runs");
        const std::uint32_t digest[5] = {lane32(cc, 14, 0), lane32(cc, 14, 1), lane32(cc, 14, 2),
                                         lane32(cc, 14, 3), lane32(cc, 1, 0) + 0xC3D2E1F0u};
        /* SHA-1("abc") = a9993e36 4706816a ba3e2571 7850c26c 9cd0d89d */
        const bool ok = digest[0] == 0xA9993E36u && digest[1] == 0x4706816Au && digest[2] == 0xBA3E2571u &&
                        digest[3] == 0x7850C26Cu && digest[4] == 0x9CD0D89Du;
        check(ok, "sha1 e2e of \"abc\"");
        if (!ok)
            std::fprintf(stderr, "  sha1 got %08x %08x %08x %08x %08x\n", digest[0], digest[1], digest[2], digest[3], digest[4]);
    }
}

void test_neon() {
    mm_cpu_context c;
    const vec p1 = pattern(0x10), p2 = pattern(0x83);
    auto prep = [&] { c = {}; vset(c, 1, p1); vset(c, 2, p2); };
    auto expect = [&](std::uint32_t insn, int e, bool q, auto f, const char *what) {
        prep();
        vec r{};
        for (int i = 0; i < (q ? 16 : 8) / e; i++) set_lane(r, e, i, f(i) & emask(e));
        if (!run({insn}, c)) { check(false, what); return; }
        check(veq(vget(c, 0), r, q), what);
    };
    auto u1 = [&](int e, int i) { return lane(p1, e, i); };
    auto u2 = [&](int e, int i) { return lane(p2, e, i); };
    auto s1 = [&](int e, int i) { return slane(p1, e, i); };
    auto s2 = [&](int e, int i) { return slane(p2, e, i); };

    expect(0x6E228C20, 1, true, [&](int i) { return u1(1, i) == u2(1, i) ? ~0ull : 0ull; }, "cmeq.16b");
    expect(0x6EA23420, 4, true, [&](int i) { return u1(4, i) > u2(4, i) ? ~0ull : 0ull; }, "cmhi.4s");
    expect(0x6EE23C20, 8, true, [&](int i) { return u1(8, i) >= u2(8, i) ? ~0ull : 0ull; }, "cmhs.2d");
    expect(0x4E623420, 2, true, [&](int i) { return s1(2, i) > s2(2, i) ? ~0ull : 0ull; }, "cmgt.8h");
    expect(0x6E22A420, 1, true, [&](int i) { return i < 8 ? std::max(u1(1, 2 * i), u1(1, 2 * i + 1)) : std::max(u2(1, 2 * (i - 8)), u2(1, 2 * (i - 8) + 1)); }, "umaxp.16b");
    expect(0x4EA2BC20, 4, true, [&](int i) { return i < 2 ? u1(4, 2 * i) + u1(4, 2 * i + 1) : u2(4, 2 * (i - 2)) + u2(4, 2 * (i - 2) + 1); }, "addp.4s");
    expect(0x5EF1B820, 8, false, [&](int) { return u1(8, 0) + u1(8, 1); }, "addp d (scalar)");
    expect(0x4E71B820, 2, false, [&](int i) { if (i) return std::uint64_t{0}; std::uint64_t t = 0; for (int k = 0; k < 8; k++) t += u1(2, k); return t & 0xFFFF; }, "addv h");
    expect(0x6E31A820, 1, false, [&](int i) { if (i) return std::uint64_t{0}; std::uint64_t m = 255; for (int k = 0; k < 16; k++) m = std::min(m, u1(1, k)); return m; }, "uminv b");
    expect(0x6EB0A820, 4, false, [&](int i) { if (i) return std::uint64_t{0}; std::uint64_t m = 0; for (int k = 0; k < 4; k++) m = std::max(m, u1(4, k)); return m; }, "umaxv s");
    expect(0x6E303820, 2, false, [&](int i) { if (i) return std::uint64_t{0}; std::uint64_t t = 0; for (int k = 0; k < 16; k++) t += u1(1, k); return t; }, "uaddlv h");
    expect(0x0F0C8420, 1, false, [&](int i) { return (u1(2, i) >> 4) & 0xFF; }, "shrn.8b #4");
    expect(0x6E021820, 1, true, [&](int i) { return i + 3 < 16 ? u1(1, i + 3) : u2(1, i + 3 - 16); }, "ext #3");
    prep(); { vec idx{}; for (int i = 0; i < 16; i++) idx.b[i] = static_cast<std::uint8_t>((i * 5) % 20); vset(c, 2, idx);
        vec r{}; for (int i = 0; i < 16; i++) r.b[i] = idx.b[i] < 16 ? p1.b[idx.b[i]] : 0;
        check(run({0x4E020020}, c) && veq(vget(c, 0), r, true), "tbl 1 reg"); }
    expect(0x4E423820, 2, true, [&](int i) { return i % 2 == 0 ? u1(2, i / 2) : u2(2, i / 2); }, "zip1.8h");
    expect(0x4E021820, 1, true, [&](int i) { return i < 8 ? u1(1, 2 * i) : u2(1, 2 * (i - 8)); }, "uzp1.16b");
    expect(0x4E825820, 4, true, [&](int i) { return i < 2 ? u1(4, 2 * i + 1) : u2(4, 2 * (i - 2) + 1); }, "uzp2.4s");
    expect(0x0E422820, 2, false, [&](int i) { return i % 2 == 0 ? u1(2, i) : u2(2, i - 1); }, "trn1.4h");
    expect(0x0E612820, 2, false, [&](int i) { return u1(4, i) & 0xFFFF; }, "xtn.4h");
    prep(); vset(c, 0, p2); { vec r = p2; for (int i = 0; i < 8; i++) r.b[8 + i] = static_cast<std::uint8_t>(lane(p1, 2, i));
        check(run({0x4E212820}, c) && veq(vget(c, 0), r, true), "xtn2.16b keeps low half"); }
    /* Saturating narrowing: all widths, scalar/vector/high-half forms,
     * source/destination aliasing, exact bounds and sticky FPSR.QC. */
    for (int family = 0; family < 3; family++) {
        const std::uint32_t base = family == 0 ? 0x0E214820u : family == 1 ? 0x2E214820u : 0x2E212820u;
        const bool unsigned_source = family == 1, signed_dest = family == 0;
        for (int dest_size : {1, 2, 4}) {
            const int source_size = 2 * dest_size;
            const std::uint32_t size_bits = static_cast<std::uint32_t>(dest_size == 1 ? 0 : dest_size == 2 ? 1 : 2) << 22;
            const std::uint64_t max = (1ull << (8 * dest_size - (signed_dest ? 1 : 0))) - 1;
            const std::int64_t min = signed_dest ? -static_cast<std::int64_t>(max) - 1 : 0;
            const std::uint64_t inputs[] = {
                static_cast<std::uint64_t>(min), max, static_cast<std::uint64_t>(min - 1), max + 1,
                0, 1, emask(source_size), 1ull << (8 * source_size - 1)
            };
            for (int form = 0; form < 3; form++) {
                const bool high = form == 1, scalar = form == 2;
                for (bool alias : {false, true}) {
                    for (int start = 0; start < 8; start++) {
                        c = {};
                        vec source{}, result = high ? p2 : vec{};
                        const int lanes = scalar ? 1 : 16 / source_size;
                        for (int i = 0; i < 16 / source_size; i++)
                            set_lane(source, source_size, i, inputs[(start + i) % 8]);
                        if (high && alias) result = source;
                        bool saturated = false;
                        for (int i = 0; i < lanes; i++) {
                            std::uint64_t value;
                            if (unsigned_source) {
                                const auto x = lane(source, source_size, i);
                                value = std::min(x, max);
                                saturated |= x > max;
                            } else {
                                const auto x = slane(source, source_size, i);
                                const auto y = std::clamp(x, min, static_cast<std::int64_t>(max));
                                value = static_cast<std::uint64_t>(y);
                                saturated |= x != y;
                            }
                            set_lane(result, dest_size, i + (high ? 8 / dest_size : 0), value);
                        }
                        vset(c, 1, source);
                        if (!alias) vset(c, 0, p2);
                        /* Keep unrelated status bits and sometimes an already set QC. */
                        const std::uint64_t initial_fpsr = 0x11 | (start % 2 ? 1ull << 27 : 0);
                        c.fpsr = initial_fpsr;
                        c.eflags = 0x83; /* N=1, Z=0, C=1, V=0 */
                        std::uint32_t word = base | size_bits | (high ? 1u << 30 : 0) | (scalar ? 0x50000000u : 0);
                        if (alias) word |= 1; /* rd = rn = 1 (the Live case) */
                        check(run({word}, c) && veq(vget(c, alias ? 1 : 0), result, true),
                              "saturating narrow bounds, layout and aliasing");
                        check(c.fpsr == (initial_fpsr | (saturated ? 1ull << 27 : 0)),
                              "saturating narrow updates cumulative FPSR.QC only");
                        check(c.eflags == 0x83, "saturating narrow preserves NZCV");
                    }
                }
            }
            check(decode(base | (3u << 22)).op == mm_a64_opcode::unknown,
                  "saturating narrow rejects reserved size");
        }
    }
    expect(0x6F090420, 1, true, [&](int i) { return u1(1, i) >> 7; }, "ushr.16b #7");
    expect(0x4F090420, 1, true, [&](int i) { return static_cast<std::uint64_t>(s1(1, i) >> 7); }, "sshr.16b #7");
    expect(0x4F7C5420, 8, true, [&](int i) { return u1(8, i) << 60; }, "shl.2d #60");
    expect(0x4F7C0420, 8, true, [&](int i) { return static_cast<std::uint64_t>(s1(8, i) >> 4); }, "sshr.2d #4");
    for (int e : {1, 2, 4, 8}) {
        for (int form = 0; form < 3; form++) { /* Q=0, Q=1, scalar D */
            if ((form == 0 && e == 8) || (form == 2 && e != 8)) continue;
            const bool q = form == 1;
            for (bool left : {false, true}) {
                for (int n : {left ? 0 : 1, 8 * e - 1, left ? 8 * e - 1 : 8 * e}) {
                    for (bool alias : {false, true}) {
                        prep(); vset(c, 0, p2);
                        const vec &dest = alias ? p1 : p2;
                        vec result{};
                        for (int i = 0; i < (q ? 16 : 8) / e; i++) {
                            const auto x = lane(p1, e, i), d = lane(dest, e, i);
                            const auto shifted = n == 64 ? 0 : left ? x << n : x >> n;
                            const auto mask = n == 64 ? 0 : (left ? emask(e) << n : emask(e) >> n) & emask(e);
                            set_lane(result, e, i, shifted | (d & ~mask));
                        }
                        const auto hb = static_cast<std::uint32_t>(left ? 8 * e + n : 16 * e - n);
                        const std::uint32_t word = (left ? 0x2F005420u : 0x2F004420u) | (hb << 16)
                            | (q ? 1u << 30 : 0) | (form == 2 ? 0x50000000u : 0) | (alias ? 1u : 0);
                        check(run({word}, c) && veq(vget(c, alias ? 1 : 0), result, true),
                              "sli/sri preserve destination bits and handle shift limits and aliasing");
                    }
                }
            }
        }
    }
    /* Exact instruction encountered while Live builds its UI. */
    prep(); vset(c, 19, p1); vset(c, 18, p2);
    { vec result{}; for (int i = 0; i < 4; i++) set_lane(result, 4, i, (u1(4, i) << 24) | (u2(4, i) & 0xFFFFFF));
        check(run({0x6F385672}, c) && veq(vget(c, 18), result, true), "Live sli.4s #24"); }
    expect(0x4E205820, 1, true, [&](int i) { return static_cast<std::uint64_t>(std::popcount(static_cast<unsigned>(u1(1, i)))); }, "cnt.16b");
    expect(0x4EA00820, 4, true, [&](int i) { return u1(4, i ^ 1); }, "rev64.4s");
    expect(0x4E201820, 1, true, [&](int i) { return u1(1, i ^ 1); }, "rev16.16b");
    expect(0x4E629C20, 2, true, [&](int i) { return u1(2, i) * u2(2, i); }, "mul.8h");
    expect(0x4E229C20, 1, true, [&](int i) { return u1(1, i) * u2(1, i); }, "mul.16b");
    expect(0x2F0AA420, 2, true, [&](int i) { return u1(1, i) << 2; }, "ushll.8h #2");
    expect(0x4F20A420, 8, true, [&](int i) { return static_cast<std::uint64_t>(s1(4, i + 2)); }, "sshll2.2d");
    prep(); vset(c, 0, p2); { vec r{}; for (int l = 0; l < 4; l++) { std::uint64_t t = lane(p2, 4, l); for (int k = 0; k < 4; k++) t += u1(1, 4 * l + k) * u2(1, 4 * l + k); set_lane(r, 4, l, t & 0xFFFFFFFF); }
        check(run({0x6E829420}, c) && veq(vget(c, 0), r, true), "udot.4s accumulates"); }
    prep(); { vec sh{}; for (int i = 0; i < 16; i++) sh.b[i] = static_cast<std::uint8_t>(i - 8); vset(c, 2, sh);
        vec r{}; for (int i = 0; i < 16; i++) { const int n = i - 8; const std::uint64_t x = u1(1, i); r.b[i] = static_cast<std::uint8_t>(n >= 8 ? 0 : n >= 0 ? x << n : -n >= 8 ? 0 : x >> -n); }
        check(run({0x6E224420}, c) && veq(vget(c, 0), r, true), "ushl.16b by register"); }
    prep(); { vec sh{}; set_lane(sh, 8, 0, static_cast<std::uint64_t>(-3)); set_lane(sh, 8, 1, 5); vset(c, 2, sh);
        vec r{}; set_lane(r, 8, 0, static_cast<std::uint64_t>(s1(8, 0) >> 3)); set_lane(r, 8, 1, u1(8, 1) << 5);
        check(run({0x4EE24420}, c) && veq(vget(c, 0), r, true), "sshl.2d by register"); }
    expect(0x4E140420, 4, true, [&](int) { return u1(4, 2); }, "dup v.4s, v1.s[2]");
    prep(); vset(c, 0, p2); { vec r = p2; set_lane(r, 2, 3, lane(p1, 2, 7));
        check(run({0x6E0E7420}, c) && veq(vget(c, 0), r, true), "mov v0.h[3], v1.h[7]"); }
    expect(0x0E20A820, 1, false, [&](int i) { return s1(1, i) < 0 ? 0xFFull : 0ull; }, "cmlt.8b #0");
    expect(0x6EE0B820, 8, true, [&](int i) { return 0 - u1(8, i); }, "neg.2d");
    expect(0x6EA26420, 4, true, [&](int i) { return std::max(u1(4, i), u2(4, i)); }, "umax.4s");
    prep(); vset(c, 0, p2); { vec r{}; for (int i = 0; i < 4; i++) set_lane(r, 4, i, (lane(p2, 4, i) + u1(4, i) * u2(4, i)) & 0xFFFFFFFF);
        check(run({0x4EA29420}, c) && veq(vget(c, 0), r, true), "mla.4s"); }
    expect(0x6E202820, 2, true, [&](int i) { return u1(1, 2 * i) + u1(1, 2 * i + 1); }, "uaddlp.8h");

    /* structure loads */
    alignas(16) std::uint8_t mem[64];
    for (int i = 0; i < 64; i++) mem[i] = static_cast<std::uint8_t>(i + 1);
    prep(); c.x[1] = reinterpret_cast<std::uintptr_t>(mem);
    check(run({0x4D40C820}, c) && c.v[0][0] == 0x0403020104030201ull && c.v[0][1] == 0x0403020104030201ull, "ld1r.4s");
    prep(); c.x[1] = reinterpret_cast<std::uintptr_t>(mem);
    check(run({0x4CDFA020}, c) && c.v[0][0] == 0x0807060504030201ull && c.v[1][1] == 0x201F1E1D1C1B1A19ull && c.x[1] == reinterpret_cast<std::uintptr_t>(mem) + 32, "ld1 {v0,v1}, [x1], #32");
    prep(); vset(c, 0, p2); c.x[1] = reinterpret_cast<std::uintptr_t>(mem);
    { vec r = p2; r.b[5] = 1; check(run({0x0D401420}, c) && veq(vget(c, 0), r, true), "ld1 {v0.b}[5]"); }
    prep(); vset(c, 0, p1); c.x[1] = reinterpret_cast<std::uintptr_t>(mem + 16); c.x[2] = 4;
    check(run({0x0D829020}, c) && lane(p1, 4, 1) == (static_cast<std::uint64_t>(mem[16]) | mem[17] << 8 | mem[18] << 16 | static_cast<std::uint64_t>(mem[19]) << 24) && c.x[1] == reinterpret_cast<std::uintptr_t>(mem) + 20, "st1 {v0.s}[1], [x1], x2");

    /* ld2/ld3/ld4 de-interleave, st2/st3/st4 interleave; a round trip has
     * to reproduce the source registers exactly. */
    auto mem_lane = [&](int esize, int index) {
        std::uint64_t v = 0;
        for (int k = 0; k < esize; k++) v |= static_cast<std::uint64_t>(mem[index * esize + k]) << (8 * k);
        return v;
    };
    prep(); c.x[1] = reinterpret_cast<std::uintptr_t>(mem);
    { /* ld2 {v0.4s, v1.4s}, [x1] */
        vec r0{}, r1{};
        for (int i = 0; i < 4; i++) { set_lane(r0, 4, i, mem_lane(4, 2 * i)); set_lane(r1, 4, i, mem_lane(4, 2 * i + 1)); }
        check(run({0x4C408820}, c) && veq(vget(c, 0), r0, true) && veq(vget(c, 1), r1, true), "ld2.4s");
    }
    prep(); c.x[1] = reinterpret_cast<std::uintptr_t>(mem);
    { /* ld3 {v0.8b, v1.8b, v2.8b}, [x1]: the upper halves must be zeroed */
        vec r0{}, r1{}, r2{};
        for (int i = 0; i < 8; i++) { set_lane(r0, 1, i, mem_lane(1, 3 * i)); set_lane(r1, 1, i, mem_lane(1, 3 * i + 1)); set_lane(r2, 1, i, mem_lane(1, 3 * i + 2)); }
        check(run({0x0C404020}, c) && veq(vget(c, 0), r0, false) && veq(vget(c, 1), r1, false) && veq(vget(c, 2), r2, false) &&
                  c.v[0][1] == 0 && c.v[2][1] == 0, "ld3.8b");
    }
    prep(); c.x[1] = reinterpret_cast<std::uintptr_t>(mem);
    { /* ld4 {v0-v3}.4s then st4 back into the second half of mem */
        std::uint8_t before[64];
        std::memcpy(before, mem, 64);
        c.x[2] = reinterpret_cast<std::uintptr_t>(mem + 64) - 64;
        (void)c.x[2];
        check(run({0x4C400020, 0x4C000020}, c) && std::memcmp(mem, before, 64) == 0, "ld4.4s + st4.4s round trip");
    }
    prep(); c.x[1] = reinterpret_cast<std::uintptr_t>(mem);
    { /* ld2r {v0.4s, v1.4s}, [x1] */
        const std::uint64_t a = mem_lane(4, 0), b = mem_lane(4, 1);
        check(run({0x4D60C820}, c) && c.v[0][0] == (a | (a << 32)) && c.v[0][1] == (a | (a << 32)) &&
                  c.v[1][0] == (b | (b << 32)), "ld2r.4s");
    }
    prep(); vset(c, 0, p1); vset(c, 1, p2); c.x[1] = reinterpret_cast<std::uintptr_t>(mem);
    { /* ld2 {v0.s, v1.s}[1], [x1]: only lane 1 of each register changes */
        vec r0 = p1, r1 = p2;
        set_lane(r0, 4, 1, mem_lane(4, 0));
        set_lane(r1, 4, 1, mem_lane(4, 1));
        check(run({0x0D609020}, c) && veq(vget(c, 0), r0, true) && veq(vget(c, 1), r1, true), "ld2 {v0.s,v1.s}[1]");
    }
    prep(); c.x[1] = reinterpret_cast<std::uintptr_t>(mem);
    check(run({0x0CDF8820}, c) && c.x[1] == reinterpret_cast<std::uintptr_t>(mem) + 16, "ld2 {v0.2s,v1.2s}, [x1], #16");
}

/* Floating point against the host's own arithmetic. */
double dbits(std::uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }
std::uint64_t bitsd(double d) { std::uint64_t u; std::memcpy(&u, &d, 8); return u; }
float fbits(std::uint64_t u) { float f; std::uint32_t x = static_cast<std::uint32_t>(u); std::memcpy(&f, &x, 4); return f; }
std::uint64_t bitsf(float f) { std::uint32_t u; std::memcpy(&u, &f, 4); return u; }

void test_fp() {
    mm_cpu_context c;
    auto setd = [&](int n, double d) { c.v[n][0] = bitsd(d); c.v[n][1] = 0; };
    auto sets = [&](int n, float f) { c.v[n][0] = bitsf(f); c.v[n][1] = 0; };
    auto getd = [&](int n) { return dbits(c.v[n][0]); };
    auto gets = [&](int n) { return fbits(c.v[n][0]); };
    auto scalar_ok = [&](int n) { return c.v[n][1] == 0; };

    c = {}; setd(1, 1.5); setd(2, 2.25); check(run({0x1E622820}, c) && getd(0) == 3.75 && scalar_ok(0), "fadd d");
    c = {}; sets(1, 1.5f); sets(2, 2.25f); check(run({0x1E223820}, c) && gets(0) == -0.75f && c.v[0][0] >> 32 == 0, "fsub s zeroes upper");
    c = {}; setd(1, 1.5); setd(2, -2.0); check(run({0x1E620820}, c) && getd(0) == -3.0, "fmul d");
    c = {}; sets(1, 1.0f); sets(2, 4.0f); check(run({0x1E221820}, c) && gets(0) == 0.25f, "fdiv s");
    c = {}; setd(1, 1.5); setd(2, -2.0); check(run({0x1E624820}, c) && getd(0) == 1.5, "fmax d");
    c = {}; sets(1, 1.5f); sets(2, -2.0f); check(run({0x1E225820}, c) && gets(0) == -2.0f, "fmin s");
    c = {}; setd(1, 1.5); setd(2, 2.0); check(run({0x1E628820}, c) && getd(0) == -3.0, "fnmul d");
    c = {}; setd(1, -1.5); check(run({0x1E60C020}, c) && getd(0) == 1.5, "fabs d");
    c = {}; sets(1, 1.5f); check(run({0x1E214020}, c) && gets(0) == -1.5f, "fneg s");
    c = {}; setd(1, 2.25); check(run({0x1E61C020}, c) && getd(0) == 1.5, "fsqrt d");
    c = {}; sets(1, 1.25f); check(run({0x1E22C020}, c) && getd(0) == 1.25 && scalar_ok(0), "fcvt d, s");
    c = {}; setd(1, 1.25); check(run({0x1E624020}, c) && gets(0) == 1.25f && c.v[0][0] >> 32 == 0, "fcvt s, d");
    c = {}; setd(1, 2.5); check(run({0x1E644020}, c) && getd(0) == 2.0, "frintn ties to even");
    c = {}; setd(1, 2.1); check(run({0x1E64C020}, c) && getd(0) == 3.0, "frintp");
    c = {}; setd(1, -2.1); check(run({0x1E654020}, c) && getd(0) == -3.0, "frintm");
    c = {}; setd(1, -2.9); check(run({0x1E65C020}, c) && getd(0) == -2.0, "frintz");
    c = {}; setd(1, 2.5); check(run({0x1E664020}, c) && getd(0) == 3.0, "frinta ties away");
    c = {}; setd(1, -2.5); check(run({0x1E664020}, c) && getd(0) == -3.0, "frinta negative ties away");
    c = {}; setd(1, 0.49999999999999994); check(run({0x1E664020}, c) && getd(0) == 0.0, "frinta just below half");
    c = {}; setd(1, 2.0); setd(2, 3.0); setd(3, 10.0);
    check(run({0x1F420C20}, c) && getd(0) == 16.0, "fmadd");
    c = {}; setd(1, 2.0); setd(2, 3.0); setd(3, 10.0);
    check(run({0x1F428C20}, c) && getd(0) == 4.0, "fmsub = a - n*m");
    c = {}; sets(1, 2.0f); sets(2, 3.0f); sets(3, 10.0f);
    check(run({0x1F220C20}, c) && gets(0) == -16.0f, "fnmadd = -a - n*m");
    c = {}; setd(1, 2.0); setd(2, 3.0); setd(3, 10.0);
    check(run({0x1F628C20}, c) && getd(0) == -4.0, "fnmsub = -a + n*m");

    /* fcmp -> b.cond: cmp d1, d2 ; b.cond +12 ; movz x0,#0 ; ret ; movz x0,#1 ; ret */
    auto fbranch = [&](std::uint32_t cmp, unsigned cond) { return run({cmp, 0x54000060u | cond, 0xD2800000, RET, 0xD2800020, RET}, c) && c.x[0] == 1; };
    c = {}; setd(1, 1.0); setd(2, 2.0); check(fbranch(0x1E622020, 11), "fcmp lt -> b.lt");
    c = {}; setd(1, 1.0); setd(2, 2.0); check(!fbranch(0x1E622020, 2), "fcmp lt: not hs");
    c = {}; setd(1, 2.0); setd(2, 2.0); check(fbranch(0x1E622020, 0), "fcmp eq");
    c = {}; setd(1, 3.0); setd(2, 2.0); check(fbranch(0x1E622020, 12), "fcmp gt");
    c = {}; setd(1, dbits(0x7FF8000000000000ull)); setd(2, 2.0); check(fbranch(0x1E622020, 6), "fcmp NaN -> vs");
    c = {}; setd(1, dbits(0x7FF8000000000000ull)); setd(2, 2.0); check(!fbranch(0x1E622020, 0) , "fcmp NaN not eq");
    c = {}; setd(1, dbits(0x7FF8000000000000ull)); setd(2, 2.0); check(fbranch(0x1E622020, 2), "fcmp NaN -> hs (C set)");
    c = {}; sets(1, -0.5f); check(fbranch(0x1E202028, 4), "fcmp s, #0.0 -> mi");
    /* fccmp d1, d2, #8, ne after cmp x1,x2 (ne true when x1!=x2): flags = fcmp(d1,d2) */
    c = {}; c.x[1] = 1; c.x[2] = 2; setd(1, 5.0); setd(2, 2.0);
    check(run({0xEB02003F, 0x1E621428, 0x54000060u | 12, 0xD2800000, RET, 0xD2800020, RET}, c) && c.x[0] == 1, "fccmp cond true -> gt");
    c = {}; c.x[1] = 1; c.x[2] = 1; setd(1, 5.0); setd(2, 2.0);
    check(run({0xEB02003F, 0x1E621428, 0x54000060u | 4, 0xD2800000, RET, 0xD2800020, RET}, c) && c.x[0] == 1, "fccmp cond false -> nzcv #8 (N)");
    c = {}; c.x[1] = 3; c.x[2] = 2; setd(1, 1.0); setd(2, 2.0);
    check(run({0xEB02003F, 0x1E62CC20}, c) && getd(0) == 1.0 && scalar_ok(0), "fcsel gt -> d1");
    c = {}; c.x[1] = 1; c.x[2] = 2; setd(1, 1.0); setd(2, 2.0);
    check(run({0xEB02003F, 0x1E62CC20}, c) && getd(0) == 2.0, "fcsel gt false -> d2");
    c = {}; check(run({0x1E6F1000}, c) && getd(0) == 1.5 && scalar_ok(0), "fmov d0, #1.5");
    c = {}; check(run({0x1E3A1000}, c) && gets(0) == -0.25f, "fmov s0, #-0.25");

    c = {}; setd(1, -3.99); check(run({0x9E780020}, c) && c.x[0] == static_cast<std::uint64_t>(-3), "fcvtzs x");
    c = {}; setd(1, 1e30); check(run({0x9E780020}, c) && c.x[0] == 0x7FFFFFFFFFFFFFFFull, "fcvtzs x saturates +");
    c = {}; setd(1, -1e30); check(run({0x9E780020}, c) && c.x[0] == 0x8000000000000000ull, "fcvtzs x saturates -");
    c = {}; setd(1, dbits(0x7FF8000000000000ull)); check(run({0x9E780020}, c) && c.x[0] == 0, "fcvtzs NaN -> 0");
    c = {}; sets(1, 3e9f); check(run({0x1E380020}, c) && c.x[0] == 0x7FFFFFFF, "fcvtzs w saturates");
    c = {}; sets(1, -7.5f); check(run({0x1E380020}, c) && c.x[0] == 0xFFFFFFF9, "fcvtzs w negative");
    c = {}; setd(1, 1.8446744073709552e19); check(run({0x9E790020}, c) && c.x[0] == ~0ull, "fcvtzu x saturates");
    c = {}; setd(1, 9223372036854775808.0 + 4096.0); check(run({0x9E790020}, c) && c.x[0] == 0x8000000000001000ull, "fcvtzu x above 2^63");
    c = {}; setd(1, -5.0); check(run({0x9E790020}, c) && c.x[0] == 0, "fcvtzu negative -> 0");
    c = {}; setd(1, 5e9); check(run({0x1E790020}, c) && c.x[0] == 0xFFFFFFFF, "fcvtzu w saturates");
    c = {}; setd(1, 2.5); check(run({0x9E600020}, c) && c.x[0] == 2, "fcvtns ties even");
    c = {}; sets(1, 2.1f); check(run({0x1E280020}, c) && c.x[0] == 3, "fcvtps");
    c = {}; setd(1, -2.1); check(run({0x9E700020}, c) && c.x[0] == static_cast<std::uint64_t>(-3), "fcvtms");
    c = {}; setd(1, 2.5); check(run({0x9E640020}, c) && c.x[0] == 3, "fcvtas ties away");
    c = {}; c.x[1] = static_cast<std::uint64_t>(-7); check(run({0x9E620020}, c) && getd(0) == -7.0 && scalar_ok(0), "scvtf d, x");
    c = {}; c.x[1] = 0xFFFFFFF9; check(run({0x1E220020}, c) && gets(0) == -7.0f, "scvtf s, w");
    c = {}; c.x[1] = 0xFFFFFFFFFFFFFFFFull; check(run({0x9E630020}, c) && getd(0) == 18446744073709551616.0, "ucvtf d, x (top bit)");
    c = {}; c.x[1] = 0xFFFFFFFF; check(run({0x1E230020}, c) && gets(0) == 4294967296.0f, "ucvtf s, w");

    /* Fixed point: the integer is scaled by 2^-fbits on the way in and by
     * 2^fbits on the way out, with the same saturation as the integer forms. */
    c = {}; c.x[1] = 0x00018000; check(run({0x1E03C420}, c) && gets(0) == 3.0f, "ucvtf s, w, #15");
    c = {}; c.x[2] = 0xFFFFFFFFFFFF8000ull; check(run({0x9E428041}, c) && getd(1) == -32768.0 / 4294967296.0, "scvtf d, x, #32");
    c = {}; c.v[1][0] = bitsf(3.5f); check(run({0x1E18C420}, c) && c.x[0] == 0x0001C000, "fcvtzs w, s, #15");
    c = {}; c.v[1][0] = bitsd(2.5); check(run({0x9E598020}, c) && c.x[0] == 0x280000000ull, "fcvtzu x, d, #32");
    c = {}; c.v[1][0] = bitsd(-1.0); check(run({0x9E598020}, c) && c.x[0] == 0, "fcvtzu x, d, #32 (negative saturates to 0)");

    /* casp is a 128-bit compare-and-swap: on a match the pair is stored,
     * otherwise the memory value is loaded into the expected pair. */
    {
        alignas(16) std::uint64_t pair[2] = {0x1111111111111111ull, 0x2222222222222222ull};
        c = {}; c.x[8] = reinterpret_cast<std::uintptr_t>(pair);
        c.x[0] = 0x1111111111111111ull; c.x[1] = 0x2222222222222222ull;   /* expected: matches */
        c.x[2] = 0xAAAAAAAAAAAAAAAAull; c.x[3] = 0xBBBBBBBBBBBBBBBBull;  /* new */
        check(run({0x4860FD02}, c) && pair[0] == 0xAAAAAAAAAAAAAAAAull && pair[1] == 0xBBBBBBBBBBBBBBBBull &&
                  c.x[0] == 0x1111111111111111ull && c.x[1] == 0x2222222222222222ull, "caspal (match)");
        c = {}; c.x[8] = reinterpret_cast<std::uintptr_t>(pair);
        c.x[0] = 0; c.x[1] = 0;                                           /* expected: does not match */
        c.x[2] = 1; c.x[3] = 2;
        check(run({0x4860FD02}, c) && pair[0] == 0xAAAAAAAAAAAAAAAAull && pair[1] == 0xBBBBBBBBBBBBBBBBull &&
                  c.x[0] == 0xAAAAAAAAAAAAAAAAull && c.x[1] == 0xBBBBBBBBBBBBBBBBull, "caspal (mismatch reloads)");
        std::uint32_t words[2] = {7, 9};
        c = {}; c.x[8] = reinterpret_cast<std::uintptr_t>(words);
        c.x[0] = 7; c.x[1] = 9; c.x[2] = 70; c.x[3] = 90;
        check(run({0x08207D02}, c) && words[0] == 70 && words[1] == 90, "casp (32-bit pair)");
    }

    /* eor3/bcax fold three registers in one instruction. */
    c = {}; c.v[3][0] = 0xF0F0F0F0F0F0F0F0ull; c.v[3][1] = 1; c.v[1][0] = 0x00FF00FF00FF00FFull; c.v[2][0] = 0x0F0F0F0F0F0F0F0Full; c.v[2][1] = 2;
    check(run({0xCE010865}, c) && c.v[5][0] == (0xF0F0F0F0F0F0F0F0ull ^ 0x00FF00FF00FF00FFull ^ 0x0F0F0F0F0F0F0F0Full) &&
              c.v[5][1] == 3, "eor3");
    c = {}; c.v[3][0] = 0xFFFF0000FFFF0000ull; c.v[1][0] = 0x00FF00FF00FF00FFull; c.v[2][0] = 0x0F0F0F0F0F0F0F0Full;
    check(run({0xCE210866}, c) && c.v[6][0] == (0xFFFF0000FFFF0000ull ^ (0x00FF00FF00FF00FFull & ~0x0F0F0F0F0F0F0F0Full)), "bcax");

    /* vectors */
    auto set4s = [&](int n, float a, float b, float x, float y) { c.v[n][0] = bitsf(a) | bitsf(b) << 32; c.v[n][1] = bitsf(x) | bitsf(y) << 32; };
    auto get4s = [&](int n, int i) { return fbits((c.v[n][i / 2] >> (32 * (i % 2))) & 0xFFFFFFFF); };
    auto set2d = [&](int n, double a, double b) { c.v[n][0] = bitsd(a); c.v[n][1] = bitsd(b); };
    c = {}; set4s(1, 1, 2, 3, 4); set4s(2, 10, 20, 30, 40);
    check(run({0x4E22D420}, c) && get4s(0, 0) == 11 && get4s(0, 3) == 44, "fadd.4s");
    c = {}; set2d(1, 1.5, -2); set2d(2, 2, 4); check(run({0x6E62DC20}, c) && getd(0) == 3 && dbits(c.v[0][1]) == -8, "fmul.2d");
    c = {}; set4s(1, 1, 2, 3, 4); set4s(2, 10, 20, 30, 40);
    check(run({0x0EA2D420}, c) && get4s(0, 0) == -9 && get4s(0, 1) == -18 && c.v[0][1] == 0, "fsub.2s zeroes upper");
    c = {}; set4s(1, 1, 2, 3, 4); set4s(2, 1, 0, 3, 0);
    check(run({0x4E22E420}, c) && (c.v[0][0] & 0xFFFFFFFF) == 0xFFFFFFFF && (c.v[0][0] >> 32) == 0 && (c.v[0][1] & 0xFFFFFFFF) == 0xFFFFFFFF, "fcmeq.4s");
    c = {}; set2d(1, 5, 1); set2d(2, 2, 2); check(run({0x6EE2E420}, c) && c.v[0][0] == ~0ull && c.v[0][1] == 0, "fcmgt.2d");
    c = {}; set4s(1, -1, 0, 1, -0.0f); check(run({0x6EA0C820}, c) && (c.v[0][0] & 0xFFFFFFFF) == 0 && (c.v[0][0] >> 32) == 0xFFFFFFFF && (c.v[0][1] >> 32) == 0xFFFFFFFF, "fcmge.4s #0");
    c = {}; set4s(0, 1, 1, 1, 1); set4s(1, 2, 2, 2, 2); set4s(2, 3, 4, 5, 6);
    check(run({0x4E22CC20}, c) && get4s(0, 0) == 7 && get4s(0, 3) == 13, "fmla.4s");
    c = {}; set2d(0, 1, 1); set2d(1, 2, 2); set2d(2, 3, 4); check(run({0x4EE2CC20}, c) && getd(0) == -5 && dbits(c.v[0][1]) == -7, "fmls.2d");
    c = {}; set4s(1, 1, 2, 3, 4); set4s(2, 0, 0, 0, 10); check(run({0x4FA29820}, c) && get4s(0, 0) == 10 && get4s(0, 3) == 40, "fmul.4s by element [3]");
    c = {}; set2d(0, 1, 1); set2d(1, 2, 3); set2d(2, 0, 10); check(run({0x4FC21820}, c) && getd(0) == 21 && dbits(c.v[0][1]) == 31, "fmla.2d by element [1]");
    c = {}; set4s(1, 1, 2, 3, 4); set4s(2, 10, 20, 30, 40); check(run({0x6E22D420}, c) && get4s(0, 0) == 3 && get4s(0, 1) == 7 && get4s(0, 2) == 30 && get4s(0, 3) == 70, "faddp.4s");
    c = {}; set4s(1, 1, -2, 3, -4); check(run({0x6EA0F820}, c) && get4s(0, 0) == -1 && get4s(0, 3) == 4, "fneg.4s");
    c = {}; set2d(1, -1, -2); check(run({0x4EE0F820}, c) && getd(0) == 1 && dbits(c.v[0][1]) == 2, "fabs.2d");
    c = {}; set4s(1, 4, 9, 16, 25); check(run({0x6EA1F820}, c) && get4s(0, 1) == 3 && get4s(0, 3) == 5, "fsqrt.4s");
    c = {}; set4s(1, 1.5f, -1.5f, 2.9f, -0.1f); check(run({0x4E219820}, c) && get4s(0, 0) == 1 && get4s(0, 1) == -2 && get4s(0, 3) == -1, "frintm.4s");
    c = {}; set4s(1, 1.9f, -1.9f, 3e9f, -3e9f);
    check(run({0x4EA1B820}, c) && (c.v[0][0] & 0xFFFFFFFF) == 1 && (c.v[0][0] >> 32) == 0xFFFFFFFF && (c.v[0][1] & 0xFFFFFFFF) == 0x7FFFFFFF && (c.v[0][1] >> 32) == 0x80000000, "fcvtzs.4s saturates");
    c = {}; set4s(1, 1.9f, -1.9f, 5e9f, 3e9f);
    check(run({0x6EA1B820}, c) && (c.v[0][0] & 0xFFFFFFFF) == 1 && (c.v[0][0] >> 32) == 0 && (c.v[0][1] & 0xFFFFFFFF) == 0xFFFFFFFF && (c.v[0][1] >> 32) == 3000000000u, "fcvtzu.4s");
    c = {}; set2d(1, -2.5, 1e30); check(run({0x4EE1B820}, c) && c.v[0][0] == static_cast<std::uint64_t>(-2) && c.v[0][1] == 0x7FFFFFFFFFFFFFFFull, "fcvtzs.2d");
    c = {}; c.v[1][0] = 0xFFFFFFFE00000003ull; c.v[1][1] = 0; check(run({0x4E21D820}, c) && get4s(0, 0) == 3 && get4s(0, 1) == -2, "scvtf.4s");
    /* The vector fixed-point conversions scale every lane. */
    c = {}; setd(1, 3.5); c.v[1][1] = bitsd(-2.0);
    check(run({0x4F72FC20}, c) && c.v[0][0] == static_cast<std::uint64_t>(static_cast<std::int64_t>(3.5 * 16384)) &&
              static_cast<std::int64_t>(c.v[0][1]) == static_cast<std::int64_t>(-2.0 * 16384), "fcvtzs.2d #14");
    c = {}; c.v[3][0] = (static_cast<std::uint64_t>(512) << 32) | 256; c.v[3][1] = 0;
    check(run({0x4F38E462}, c) && get4s(2, 0) == 1.0f && get4s(2, 1) == 2.0f, "scvtf.4s #8");

    c = {}; c.v[1][0] = 0xFFFFFFFE00000003ull; c.v[1][1] = 0; check(run({0x6E21D820}, c) && get4s(0, 0) == 3 && get4s(0, 1) == 4294967296.0f, "ucvtf.4s");
    c = {}; c.v[1][0] = static_cast<std::uint64_t>(-9); c.v[1][1] = 7; check(run({0x4E61D820}, c) && getd(0) == -9 && dbits(c.v[0][1]) == 7, "scvtf.2d");
    c = {}; set4s(1, 1.5f, -2.5f, 0, 0); check(run({0x0E617820}, c) && getd(0) == 1.5 && dbits(c.v[0][1]) == -2.5, "fcvtl");
    c = {}; set2d(1, 1.5, -2.5); check(run({0x0E616820}, c) && get4s(0, 0) == 1.5f && get4s(0, 1) == -2.5f && c.v[0][1] == 0, "fcvtn");
    c = {}; set2d(1, 1, 5); set2d(2, 7, 3); check(run({0x6E62F420}, c) && getd(0) == 5 && dbits(c.v[0][1]) == 7, "fmaxp.2d");

    /* scalar AdvSIMD forms (V registers on both sides) */
    c = {}; c.v[1][0] = 0xFFFFFFF9; check(run({0x5E21D820}, c) && gets(0) == -7.0f && scalar_ok(0), "scvtf s0, s1");
    c = {}; setd(1, -3.7); check(run({0x5EE1B820}, c) && c.v[0][0] == static_cast<std::uint64_t>(-3) && scalar_ok(0), "fcvtzs d0, d1");
    c = {}; c.v[1][0] = ~0ull; check(run({0x7E61D820}, c) && getd(0) == 18446744073709551616.0, "ucvtf d0, d1");
    c = {}; set2d(1, 1.5, 2.25); check(run({0x7E70D820}, c) && getd(0) == 3.75 && scalar_ok(0), "faddp d0, v1.2d");
    c = {}; set4s(1, 1, 7, 100, 100); check(run({0x7E30F820}, c) && gets(0) == 7 && c.v[0][0] >> 32 == 0 && scalar_ok(0), "fmaxp s0, v1.2s");
    c = {}; setd(1, 1.0); setd(2, 3.5); check(run({0x7EE2D420}, c) && getd(0) == 2.5, "fabd d");
    c = {}; check(run({0x4F03F600}, c) && get4s(0, 0) == 1.0f && get4s(0, 3) == 1.0f, "fmov v0.4s, #1.0");
    c = {}; check(run({0x6F04F480}, c) && getd(0) == -2.5 && dbits(c.v[0][1]) == -2.5, "fmov v0.2d, #-2.5");

    /* FPCR: msr fpcr, x1 ; mrs x0, fpcr ; frintx d0, d1 ; fmul d0, d1, d2 */
    const double denorm = dbits(0x0000000000000001ull);
    c = {}; c.x[1] = 0; setd(1, denorm); setd(2, 1.0);
    check(run({0xD51B4401, 0xD53B4400, 0x1E620820}, c) && c.v[0][0] == 1 && c.x[0] == 0, "FZ=0: denormal survives");
    c = {}; c.x[1] = 1u << 24; setd(1, denorm); setd(2, 1.0);
    check(run({0xD51B4401, 0xD53B4400, 0x1E620820}, c) && c.v[0][0] == 0 && c.x[0] == (1u << 24), "FZ=1: denormal flushed to zero");
    c = {}; c.x[1] = 1u << 22; setd(1, 2.1); /* RMode = RP */
    check(run({0xD51B4401, 0x1E674020}, c) && getd(0) == 3.0, "FPCR RMode=RP: frintx rounds up");
    c = {}; c.x[1] = 2u << 22; setd(1, 2.9); /* RMode = RM */
    check(run({0xD51B4401, 0x1E674020}, c) && getd(0) == 2.0, "FPCR RMode=RM: frintx rounds down");
    c = {}; c.x[1] = 0; setd(1, 2.5);
    check(run({0xD51B4401, 0x1E674020}, c) && getd(0) == 2.0, "FPCR RMode=RN restored");
}

} // namespace

static void test_shifted_w_reads() {
    /* W operands can be views of X registers last written with 64-bit values. */
    for (unsigned shift = 0; shift < 4; ++shift) {
        for (unsigned amount : {1u, 8u, 16u, 31u}) {
            for (std::uint64_t rhs : {0x100000002ull, 0xffff000080000001ull, 0x8000000000000001ull}) {
                mm_cpu_context ctx;
                ctx.x[1] = 0x5555000012345678ull; ctx.x[2] = rhs;
                const auto low = static_cast<std::uint32_t>(rhs);
                const auto shifted = shift == 0 ? low << amount : shift == 1 ? low >> amount :
                    shift == 2 ? static_cast<std::uint32_t>(static_cast<std::int32_t>(low) >> amount) : std::rotr(low, amount);
                const std::uint32_t opcode = shift == 3 ? 0x2a000000 : 0x0b000000;
                const auto word = opcode | (shift << 22) | (2u << 16) | (amount << 10) | (1u << 5);
                const auto lhs = static_cast<std::uint32_t>(ctx.x[1]);
                const auto expected = shift == 3 ? lhs | shifted : lhs + shifted;
                check(run({word}, ctx) && ctx.x[0] == expected && ctx.x[2] == rhs, "shifted W source excludes upper X bits");
            }
        }
    }
    mm_cpu_context ctx; ctx.x[0] = 0x100000002ull;
    check(run({0x0b407c09, 0x13017d29}, ctx) && ctx.x[9] == 1,
          "packed rational numerator 2 decreases to 1 instead of looping");
}

int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--register-cache-only") {
        test_vector_residency();
        if (failures) return 1;
        puts("PASS: register residency checks");
        return 0;
    }
    test_vector_residency();
    test_shifted_w_reads();
    test_null_branch();
#if !defined(_WIN32)
    test_parallel_faults();
#endif
    test_decoder();
    test_cond_branch_matrix();
    test_branch_context_flags();
    test_branches();
    test_conditional_ops();
    test_alu();
    test_xzr_sp();
    test_mov_wide();
    test_loads_stores();
    test_bitfield_logical_imm();
    test_mul_div_shift();
    test_extended_addsub();
    test_atomics();
    test_crc();
    test_halving();
    test_widening();
    test_abd_rshr();
    test_simd_stage1();
    test_q_memory_boundaries();
    test_packed_u64_to_double();
    test_neon();
    test_variable_shift_counts();
    test_crypto();
    test_fp();

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS: translator decode/execute checks\n");
    return 0;
}

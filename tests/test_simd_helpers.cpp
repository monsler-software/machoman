#include "machoman/runtime.h"
#include "machoman/translator.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {
void inputs(mm_cpu_context &ctx, std::uint64_t seed, unsigned kind) {
    std::uint64_t state = seed;
    const std::uint64_t f32[] = {0, 0x80000000, 0x3f800000, 0x40000000, 0x7f800000, 0xff800000,
        0x7fc00001, 0x7f800001, 1, 0x00800000, 0x7f7fffff, 0x3eaaaaab};
    const std::uint64_t f16[] = {0, 0x8000, 0x3c00, 0x4000, 0x7c00, 0xfc00, 0x7e01, 0x7c01, 1, 0x0400, 0x7bff, 0x3555};
    const std::uint64_t f64[] = {0, 1ull << 63, 0x3ff0000000000000, 0x4000000000000000, 0x7ff0000000000000,
        0xfff0000000000000, 0x7ff8000000000001, 0x7ff0000000000001, 1, 0x0010000000000000,
        0x7fefffffffffffff, 0x3fd5555555555555};
    for (unsigned reg = 0; reg < 32; reg++) {
        for (auto &half : ctx.v[reg]) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; half = state; }
        if (kind) {
            const unsigned size = kind == 1 ? 4 : kind == 2 ? 2 : 8;
            const auto *pattern = kind == 1 ? f32 : kind == 2 ? f16 : f64;
            for (unsigned lane = 0; lane < 16 / size; lane++) {
                const auto value = pattern[(reg + lane + seed) % 12];
                std::memcpy(reinterpret_cast<char *>(ctx.v[reg]) + lane * size, &value, size);
            }
        }
    }
    for (unsigned reg = 0; reg < 29; reg++) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        ctx.x[reg] = state;
    }
    ctx.fpsr = seed & 1 ? 1ull << 27 : 0;
}
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    std::ifstream fixture(argv[1]);
    if (!fixture) return 2;
    std::string line;
    unsigned failures = 0, count = 0;
    while (std::getline(fixture, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::uint32_t word;
        std::uint64_t seed, control, lo, hi, fpsr, nzcv;
        unsigned kind, gpr_dest;
        std::istringstream row(line);
        if (!(row >> std::hex >> word >> seed >> std::dec >> kind >> std::hex >> control >> std::dec >> gpr_dest >> std::hex >> lo >> hi >> fpsr >> nzcv)) return 2;
        const std::uint32_t code[] = {word, 0xd65f03c0};
        mm_translation_result translated;
        mm_translate_buffer({reinterpret_cast<const std::uint8_t *>(code), sizeof(code)}, 0x1000, translated);
        mm_cpu_context ctx{};
        inputs(ctx, seed, kind);
        ctx.fpcr = control;
        const auto initial = ctx;
        const unsigned dest = word & 31;
        bool ok = !translated.unsupported_count && mm_runtime_exec(translated, 0x1000, ctx) == mm_runtime_status::ok;
        const auto got_lo = gpr_dest ? ctx.x[dest] : ctx.v[dest][0], got_hi = gpr_dest ? 0 : ctx.v[dest][1];
        ok &= got_lo == lo && got_hi == hi && ctx.fpsr == fpsr;
        for (unsigned reg = 0; reg < 32; reg++) if (gpr_dest || reg != dest)
            ok &= ctx.v[reg][0] == initial.v[reg][0] && ctx.v[reg][1] == initial.v[reg][1];
        for (unsigned reg = 0; reg < 32; reg++) if (!gpr_dest || reg != dest) ok &= ctx.x[reg] == initial.x[reg];
        const auto actual_nzcv = ((ctx.eflags & 0x80) ? 1u << 31 : 0) | ((ctx.eflags & 0x40) ? 1u << 30 : 0)
            | ((ctx.eflags & 1) ? 0 : 1u << 29) | ((ctx.eflags & 0x800) ? 1u << 28 : 0);
        ok &= actual_nzcv == nzcv && ctx.fpcr == initial.fpcr;
        if (!ok) {
            if (failures < 40) std::fprintf(stderr,
                "%08x seed=%llx kind=%u fpcr=%llx unsupported=%zu got=%016llx:%016llx fpsr=%llx expected=%016llx:%016llx fpsr=%llx\n",
                word, static_cast<unsigned long long>(seed), kind, static_cast<unsigned long long>(control), translated.unsupported_count,
                static_cast<unsigned long long>(got_hi), static_cast<unsigned long long>(got_lo),
                static_cast<unsigned long long>(ctx.fpsr), static_cast<unsigned long long>(hi),
                static_cast<unsigned long long>(lo), static_cast<unsigned long long>(fpsr));
            failures++;
        }
        count++;
    }
    std::printf("SIMD oracle: %u / %u vectors passed\n", count - failures, count);
    return failures ? 1 : count ? 0 : 2;
}

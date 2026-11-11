#include "simd_helpers.h"
#include "simd_fp_bits.h"

template<mm_simd_helper Kind> static void execute_fp(mm_cpu_context *ctx, std::uint32_t w) {
    constexpr auto kind = Kind;
    using namespace mm_simd;
    const unsigned d = w & 31, n = (w >> 5) & 31, m = (w >> 16) & 31;
    const bool q = (w >> 30) & 1, scalar = (w >> 28) & 1;
    const vector first = read(*ctx, n), second = read(*ctx, m), dest = read(*ctx, d);
    vector result{};
    switch (kind) {
    case mm_simd_helper::fp_negate_half:
        for (unsigned i = 0; i < (q ? 8u : 4u); i++) {
            const auto value = get<2>(first, i);
            put<2>(result, i, (w & (1u << 29)) ? value ^ 0x8000 : value & 0x7fff);
        }
        break;
    case mm_simd_helper::fp_widen_half: {
        /* FPConvert does not apply FZ16 to half-precision operands. */
        fp_bits<4> fp{ctx->fpcr & ~(1ull << 19), ctx->fpsr};
        for (unsigned i = 0; i < 4; i++) put<4>(result, i, fp.convert<2>(get<2>(first, i + (q ? 4 : 0))));
        break;
    }
    case mm_simd_helper::fp_narrow_half: {
        fp_bits<2> fp{ctx->fpcr & ~(1ull << 19), ctx->fpsr};
        if (q) result = dest;
        for (unsigned i = 0; i < 4; i++) put<2>(result, i + (q ? 4 : 0), fp.convert<4>(get<4>(first, i)));
        break;
    }
    case mm_simd_helper::fp_reduce: {
        fp_bits<4> fp{ctx->fpcr, ctx->fpsr};
        const bool minimum = (w >> 23) & 1;
        const auto lo = fp.minmax(get<4>(first, 0), get<4>(first, 1), minimum);
        const auto hi = fp.minmax(get<4>(first, 2), get<4>(first, 3), minimum);
        put<4>(result, 0, fp.minmax(lo, hi, minimum));
        break;
    }
    case mm_simd_helper::fp_complex_add: {
        visit_width((w >> 22) & 3, [&]<unsigned E> {
            if constexpr (E > 1) {
                fp_bits<E> fp{ctx->fpcr, ctx->fpsr};
                const bool rotate270 = (w >> 12) & 1;
                for (unsigned i = 0; i < (q ? 16u : 8u) / E; i++) {
                    auto b = get<E>(second, i ^ 1);
                    if (bool(i & 1) == rotate270) b ^= fp.sign;
                    put<E>(result, i, fp.add(get<E>(first, i), b));
                }
            }
        });
        break;
    }
    case mm_simd_helper::fp_compare_abs: case mm_simd_helper::fp_estimate: case mm_simd_helper::fp_step:
        visit_width((w & (1u << 22)) ? 3 : 2, [&]<unsigned E> {
            if constexpr (E >= 4) {
                fp_bits<E> fp{ctx->fpcr, ctx->fpsr};
                for (unsigned i = 0; i < (scalar ? 1u : (q ? 16u : 8u) / E); i++) {
                    const auto a = get<E>(first, i), b = get<E>(second, i);
                    const auto value = kind == mm_simd_helper::fp_compare_abs ? fp.compare_abs(a, b, (w >> 23) & 1)
                        : kind == mm_simd_helper::fp_estimate ? fp.estimate(a, (w >> 29) & 1) : fp.step(a, b, (w >> 23) & 1);
                    put<E>(result, i, value);
                }
            }
        });
        break;
    default: return;
    }
    std::memcpy(ctx->v[d], result.data(), 16);
}

mm_simd_native mm_simd_fp_native(mm_simd_helper kind) {
    if (kind >= mm_simd_helper::fp_half_vector) return mm_simd_half_native(kind);
    switch (kind) {
#define NATIVE(K) case mm_simd_helper::K: return &execute_fp<mm_simd_helper::K>
    NATIVE(fp_widen_half); NATIVE(fp_narrow_half); NATIVE(fp_negate_half); NATIVE(fp_reduce);
    NATIVE(fp_compare_abs); NATIVE(fp_complex_add); NATIVE(fp_estimate); NATIVE(fp_step);
#undef NATIVE
    default: return nullptr;
    }
}

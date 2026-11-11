#include "emit.h"
#include "simd_helpers.h"
#include "simd_lanes.h"
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <type_traits>

mm_simd_helper mm_decode_simd_helper(std::uint32_t w) {
    switch (w & 0xffe0fc00u) {
    case 0xce608000: return mm_simd_helper::sha512h;
    case 0xce608400: return mm_simd_helper::sha512h2;
    case 0xce608800: return mm_simd_helper::sha512su1;
    case 0xce60c000: return mm_simd_helper::sm3partw1;
    case 0xce60c400: return mm_simd_helper::sm3partw2;
    case 0xce60c800: return mm_simd_helper::sm4ekey;
    case 0xce608c00: return mm_simd_helper::rax1;
    }
    if ((w & 0xfffffc00u) == 0xcec08000u) return mm_simd_helper::sha512su0;
    if ((w & 0xfffffc00u) == 0xcec08400u) return mm_simd_helper::sm4e;
    if ((w & 0xffe0c000u) == 0xce408000u) return mm_simd_helper::sm3tt;
    if ((w & 0xffe08000u) == 0xce400000u) return mm_simd_helper::sm3ss1;
    if ((w & 0xffe00000u) == 0xce800000u) return mm_simd_helper::xar;
    const unsigned size = (w >> 22) & 3;
    const bool scalar = (w >> 28) & 1, q = (w >> 30) & 1;
    const auto half_three = w & 0x9f60fc00u;
    if (half_three == 0x0e400c00u || half_three == 0x0e401400u || half_three == 0x0e401c00u || half_three == 0x0e402400u
        || half_three == 0x0e403400u || half_three == 0x0e403c00u) return mm_simd_helper::fp_half_vector;
    const auto half_elem = w & 0xbf00f400u;
    if (size == 0 && (half_elem == 0x0f001000u || half_elem == 0x0f005000u || half_elem == 0x0f009000u))
        return mm_simd_helper::fp_half_element;
    switch (w & 0xbffffc00u) {
    case 0x0ef8d800: case 0x0ef8e800: case 0x2ef8c800:
    case 0x0ef9b800: case 0x2ef9b800: case 0x0e79d800: case 0x2e79d800:
        return mm_simd_helper::fp_half_misc;
    }
    if ((w & 0x9fc0f400u) == 0x0f80e000u) return mm_simd_helper::dot_element;
    if ((w & 0x8f20fc00u) == 0x0e205400u && (!scalar || (q && size == 3)) && (scalar || size != 3 || q))
        return mm_simd_helper::round_shift;
    if ((w & 0xbf3ffc00u) == 0x2e204800u && size < 3) return mm_simd_helper::clz;
    if ((w & 0xbffffc00u) == 0x0e217800u) return mm_simd_helper::fp_widen_half;
    if ((w & 0xbffffc00u) == 0x0e216800u) return mm_simd_helper::fp_narrow_half;
    if ((w & 0x9ffffc00u) == 0x0ef8f800u) return mm_simd_helper::fp_negate_half;
    if ((w & 0xbf7ffc00u) == 0x2e30f800u && q) return mm_simd_helper::fp_reduce;
    const bool fp_shape = (!scalar || q) && (scalar || !(w & (1u << 22)) || q);
    if ((w & 0xaf20fc00u) == 0x2e20ec00u && fp_shape) return mm_simd_helper::fp_compare_abs;
    if ((w & 0xbf20ec00u) == 0x2e00e400u && size > 0 && (size != 3 || q)) return mm_simd_helper::fp_complex_add;
    if ((w & 0x8fbffc00u) == 0x0ea1d800u && fp_shape) return mm_simd_helper::fp_estimate;
    if ((w & 0xaf20fc00u) == 0x0e20fc00u && fp_shape) return mm_simd_helper::fp_step;
    if ((w & 0xbfe08c00u) == 0x0e000000u && ((w & 0x1000) || ((w >> 13) & 3) >= 2))
        return mm_simd_helper::table;
    if ((w & 0xbf3ffc00u) == 0x2e213800u && size < 3) return mm_simd_helper::shll;
    if ((w & 0xbffffc00u) == 0x2e605800u) return mm_simd_helper::rbit;
    if ((w & 0xaffffc00u) == 0x0ee0b800u && q) return mm_simd_helper::abs64;
    if ((w & 0x8f20dc00u) == 0x0e200c00u && (!scalar || q) && (scalar || size != 3 || q))
        return mm_simd_helper::saturating_arithmetic;
    if ((w & 0x8f20fc00u) == 0x0e20b400u && (size == 1 || size == 2) && (!scalar || q))
        return mm_simd_helper::doubling_mul_high;
    const auto element = w & 0xbf00f400u;
    if ((element == 0x2f002000u || element == 0x2f006000u || element == 0x2f00a000u || element == 0x0f008000u)
        && (size == 1 || size == 2)) return mm_simd_helper::multiply_element;
    const auto narrow = w & 0x9f20fc00u;
    if ((narrow == 0x0e204000u || narrow == 0x0e206000u) && size < 3)
        return mm_simd_helper::narrow_high;
    if ((w & 0xbf20fc00u) == 0x0e20e000u && (size == 0 || size == 3))
        return mm_simd_helper::polynomial_multiply;
    return mm_simd_helper::none;
}

namespace {
using namespace mm_simd;

/* Every helper snapshots sources and commits once. This handles all
 * rd/rn/rm aliases, including width-changing and table operations. */
template<mm_simd_helper Kind> void execute_simd(mm_cpu_context *ctx, std::uint32_t w) {
    constexpr auto kind = Kind;
    const unsigned d = w & 31, n = (w >> 5) & 31, m = (w >> 16) & 31, size = (w >> 22) & 3;
    const bool q = (w >> 30) & 1, scalar = (w >> 28) & 1, u = (w >> 29) & 1;
    const vector dest = read(*ctx, d), first = read(*ctx, n), second = read(*ctx, m);
    vector result{};
    bool qc = false;
    switch (kind) {
    case mm_simd_helper::table: {
        const unsigned length = ((w >> 13) & 3) + 1;
        const bool extend = (w >> 12) & 1;
        for (unsigned i = 0; i < (q ? 16u : 8u); i++) {
            const unsigned index = second[i];
            if (index < 16 * length) result[i] = read(*ctx, (n + index / 16) & 31)[index % 16];
            else if (extend) result[i] = dest[i];
        }
        break;
    }
    case mm_simd_helper::rbit:
        for (unsigned i = 0; i < (q ? 16u : 8u); i++) {
            const auto x = first[i];
            for (unsigned b = 0; b < 8; b++) result[i] |= static_cast<std::uint8_t>(((x >> b) & 1) << (7 - b));
        }
        break;
    case mm_simd_helper::abs64:
        for (unsigned i = 0; i < (scalar ? 1u : 2u); i++) {
            const wide value = signed_lane<8>(first, i);
            put<8>(result, i, value < 0 ? -value : value);
        }
        break;
    case mm_simd_helper::shll:
        visit_width(size, [&]<unsigned E> {
            if constexpr (E < 8) for (unsigned i = 0; i < 8 / E; i++)
                put<2 * E>(result, i, wide{get<E>(first, i + (q ? 8 / E : 0))} << (8 * E));
        });
        break;
    case mm_simd_helper::saturating_arithmetic:
        visit_width(size, [&]<unsigned E> {
            for (unsigned i = 0; i < (scalar ? 1u : (q ? 16u : 8u) / E); i++) {
                const wide a = u ? wide{get<E>(first, i)} : signed_lane<E>(first, i);
                const wide b = u ? wide{get<E>(second, i)} : signed_lane<E>(second, i);
                put<E>(result, i, saturate<E>((w & 0x2000) ? a - b : a + b, !u, qc));
            }
        });
        break;
    case mm_simd_helper::doubling_mul_high:
        visit_width(size, [&]<unsigned E> {
            if constexpr (E == 2 || E == 4) for (unsigned i = 0; i < (scalar ? 1u : (q ? 16u : 8u) / E); i++) {
                const wide product = 2 * signed_lane<E>(first, i) * signed_lane<E>(second, i);
                const wide rounding = u ? wide{1} << (8 * E - 1) : 0;
                put<E>(result, i, saturate<E>((product + rounding) >> (8 * E), true, qc));
            }
        });
        break;
    case mm_simd_helper::multiply_element:
        visit_width(size, [&]<unsigned E> {
            if constexpr (E == 2 || E == 4) {
                const unsigned index = E == 2 ? ((w >> 20) & 3) | ((w >> 9) & 4)
                                             : ((w >> 21) & 1) | ((w >> 10) & 2);
                const auto element_vector = read(*ctx, E == 2 ? m & 15 : m);
                const wide element = get<E>(element_vector, index);
                const unsigned opcode = (w >> 12) & 15;
                if (opcode == 8) {
                    for (unsigned i = 0; i < (q ? 16u : 8u) / E; i++)
                        put<E>(result, i, wide{get<E>(first, i)} * element);
                } else {
                    for (unsigned i = 0; i < 8 / E; i++) {
                        const wide product = wide{get<E>(first, i + (q ? 8 / E : 0))} * element;
                        const wide acc = get<2 * E>(dest, i);
                        put<2 * E>(result, i, opcode == 2 ? acc + product : opcode == 6 ? acc - product : product);
                    }
                }
            }
        });
        break;
    case mm_simd_helper::narrow_high:
        if (q) result = dest;
        visit_width(size, [&]<unsigned E> {
            if constexpr (E < 8) for (unsigned i = 0; i < 8 / E; i++) {
                const wide a = get<2 * E>(first, i), b = get<2 * E>(second, i);
                wide value = (w & 0x2000) ? a - b : a + b;
                if (u) value += wide{1} << (8 * E - 1);
                put<E>(result, i + (q ? 8 / E : 0), value >> (8 * E));
            }
        });
        break;
    case mm_simd_helper::polynomial_multiply:
        if (size == 3) {
            const auto a = get<8>(first, q ? 1 : 0), b = get<8>(second, q ? 1 : 0);
            uwide product = 0;
            for (unsigned i = 0; i < 64; i++) if ((b >> i) & 1) product ^= uwide{a} << i;
            put<8>(result, 0, static_cast<wide>(static_cast<std::uint64_t>(product)));
            put<8>(result, 1, static_cast<wide>(static_cast<std::uint64_t>(product >> 64)));
        } else {
            for (unsigned lane = 0; lane < 8; lane++) {
                const auto a = first[lane + (q ? 8 : 0)], b = second[lane + (q ? 8 : 0)];
                unsigned product = 0;
                for (unsigned i = 0; i < 8; i++) if ((b >> i) & 1) product ^= static_cast<unsigned>(a) << i;
                put<2>(result, lane, product);
            }
        }
        break;
    case mm_simd_helper::dot_element: {
        const unsigned index = ((w >> 21) & 1) | ((w >> 10) & 2);
        for (unsigned i = 0; i < (q ? 4u : 2u); i++) {
            wide value = get<4>(dest, i);
            for (unsigned j = 0; j < 4; j++) value += (u ? wide{first[i * 4 + j]} : signed_lane<1>(first, i * 4 + j))
                * (u ? wide{second[index * 4 + j]} : signed_lane<1>(second, index * 4 + j));
            put<4>(result, i, value);
        }
        break;
    }
    case mm_simd_helper::round_shift:
        visit_width(size, [&]<unsigned E> {
            for (unsigned i = 0; i < (scalar ? 1u : (q ? 16u : 8u) / E); i++) {
                const int shift = static_cast<std::int8_t>(second[i * E]);
                constexpr int lane_bits = static_cast<int>(8 * E);
                const wide value = u ? wide{get<E>(first, i)} : signed_lane<E>(first, i);
                const wide shifted = shift >= 0 ? (shift >= lane_bits ? 0 : static_cast<wide>(get<E>(first, i)) << shift)
                    : -shift > lane_bits ? 0 : (value + (wide{1} << (-shift - 1))) >> -shift;
                put<E>(result, i, shifted);
            }
        });
        break;
    case mm_simd_helper::clz:
        visit_width(size, [&]<unsigned E> {
            if constexpr (E < 8) for (unsigned i = 0; i < (q ? 16u : 8u) / E; i++)
                put<E>(result, i, std::countl_zero(get<E>(first, i)));
        });
        break;
    case mm_simd_helper::none: return; /* unreachable after validated decoding */
    default: return;
    }
    std::memcpy(ctx->v[d], result.data(), 16);
    if (qc) ctx->fpsr |= 1ull << 27;
}
} // namespace

namespace mm_emit {
bool emit_simd_helper(mm_x64_buf &buf, const mm_a64_insn &insn) {
    const auto kind = mm_decode_simd_helper(insn.raw);
    mm_simd_native function = nullptr;
    if (kind >= mm_simd_helper::fp_widen_half) function = mm_simd_fp_native(kind);
    else if (kind >= mm_simd_helper::sha512h) function = mm_simd_crypto_native(kind);
    else switch (kind) {
#define NATIVE(K) case mm_simd_helper::K: function = &execute_simd<mm_simd_helper::K>; break
    NATIVE(table); NATIVE(shll); NATIVE(rbit); NATIVE(abs64); NATIVE(saturating_arithmetic);
    NATIVE(doubling_mul_high); NATIVE(multiply_element); NATIVE(narrow_high); NATIVE(polynomial_multiply);
    NATIVE(dot_element); NATIVE(round_shift); NATIVE(clz);
#undef NATIVE
    default: break;
    }
    return emit_simd_native(buf, function, insn.raw);
}

bool emit_simd_native(mm_x64_buf &buf, mm_simd_native function, std::uint32_t raw) {
    if (!function) return false;
    /* Match the native-call convention used by CRC and watch helpers.
     * Guest registers are in ctx; R15 and the host stack are preserved. */
    mm_x64_emit_push_reg(buf, MM_RBP);
    mm_x64_emit_mov_reg(buf, MM_RBP, MM_RSP);
    mm_x64_emit_alu_imm32(buf, mm_x64_alu::and_, MM_RSP, -16, true);
    mm_x64_emit_mov_reg(buf, MM_RDI, CTX);
    mm_x64_emit_mov_imm(buf, MM_RSI, raw);
    mm_x64_emit_host_address(buf, A, reinterpret_cast<std::uintptr_t>(function));
    mm_x64_emit_call_reg(buf, A);
    mm_x64_emit_mov_reg(buf, MM_RSP, MM_RBP);
    mm_x64_emit_pop_reg(buf, MM_RBP);
    return true;
}
}

#include "emit.h"
#include "simd_helpers.h"
#include "simd_fp_bits.h"

namespace {
using namespace mm_simd;
bool condition(unsigned cond, std::uint64_t flags) {
    const bool n = flags & 0x80, z = flags & 0x40, c = !(flags & 1), v = flags & 0x800;
    const bool base[] = {z, c, n, v, c && !z, n == v, !z && n == v, true};
    return cond >= 14 ? true : base[cond / 2] != bool(cond & 1);
}
std::uint64_t from_integer(fp_bits<2> &fp, std::uint64_t bits, unsigned width, bool sg, int scale = 0) {
    const wide value = sg ? (width == 16 ? wide{std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(bits))}
        : width == 32 ? wide{std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(bits))} : wide{std::bit_cast<std::int64_t>(bits)})
        : width == 16 ? bits & 0xffff : width == 32 ? bits & 0xffffffff : wide{bits};
    return fp.pack(value < 0, static_cast<uwide>(value < 0 ? -value : value), -scale);
}
template<mm_a64_fop Op> void scalar_half(mm_cpu_context *ctx, std::uint32_t w) {
    const unsigned d = w & 31, n = (w >> 5) & 31, m = (w >> 16) & 31;
    const auto first = read(*ctx, n), second = read(*ctx, m);
    const auto a = get<2>(first, 0), b = get<2>(second, 0);
    fp_bits<2> fp{ctx->fpcr, ctx->fpsr};
    vector result{};
    std::uint64_t value = 0;
    if constexpr (Op == mm_a64_fop::fcvt) {
        const unsigned source = (w >> 22) & 3, target = (w >> 15) & 7;
        const auto control = ctx->fpcr & ~(1ull << 19);
        if (target == 7) {
            fp_bits<2> dest{control, ctx->fpsr};
            value = source == 0 ? dest.convert<4>(get<4>(first, 0)) : dest.convert<8>(get<8>(first, 0));
            put<2>(result, 0, value);
        } else if (target == 4) {
            fp_bits<4> dest{control, ctx->fpsr}; put<4>(result, 0, dest.convert<2>(a));
        } else {
            fp_bits<8> dest{control, ctx->fpsr}; put<8>(result, 0, dest.convert<2>(a));
        }
    } else if constexpr (Op == mm_a64_fop::fcvtzs || Op == mm_a64_fop::fcvtzu) {
        const unsigned bits = (w >> 31) ? 64 : 32;
        value = fp.to_integer(a, bits, Op == mm_a64_fop::fcvtzs);
        if (d != 31) ctx->x[d] = bits == 32 ? value & 0xffffffff : value;
        return;
    } else if constexpr (Op == mm_a64_fop::fcmp || Op == mm_a64_fop::fcmp0) {
        const unsigned nzcv = fp.relation(a, Op == mm_a64_fop::fcmp0 ? 0 : b, w & 16);
        ctx->eflags = 2 | ((nzcv & 8) ? 0x80 : 0) | ((nzcv & 4) ? 0x40 : 0)
            | ((nzcv & 2) ? 0 : 1) | ((nzcv & 1) ? 0x800 : 0);
        return;
    } else {
        if constexpr (Op == mm_a64_fop::scvtf || Op == mm_a64_fop::ucvtf) {
            value = from_integer(fp, n == 31 ? 0 : ctx->x[n], (w >> 31) ? 64 : 32, Op == mm_a64_fop::scvtf,
                                  (w & (1u << 21)) ? 0 : 64 - static_cast<int>((w >> 10) & 63));
        } else if constexpr (Op == mm_a64_fop::fmov) value = a;
        else if constexpr (Op == mm_a64_fop::fabs) value = a & ~fp.sign;
        else if constexpr (Op == mm_a64_fop::fneg) value = a ^ fp.sign;
        else if constexpr (Op == mm_a64_fop::fmul) value = fp.multiply(a, b);
        else if constexpr (Op == mm_a64_fop::fdiv) value = fp.divide(a, b);
        else if constexpr (Op == mm_a64_fop::fadd) value = fp.add(a, b);
        else if constexpr (Op == mm_a64_fop::fsub) value = fp.subtract(a, b);
        else if constexpr (Op == mm_a64_fop::fmax || Op == mm_a64_fop::fmin) value = fp.minmax(a, b, Op == mm_a64_fop::fmin);
        else if constexpr (Op == mm_a64_fop::fcsel) value = condition((w >> 12) & 15, ctx->eflags) ? a : b;
        else if constexpr (Op == mm_a64_fop::fmov_imm) {
            const auto imm = (w >> 13) & 255, bit = (imm >> 6) & 1;
            value = ((imm & 128) << 8) | (((bit ^ 1) << 4 | (bit ? 12 : 0) | ((imm >> 4) & 3)) << 10) | ((imm & 15) << 6);
        } else if constexpr (Op == mm_a64_fop::fmadd || Op == mm_a64_fop::fmsub || Op == mm_a64_fop::fnmadd || Op == mm_a64_fop::fnmsub) {
            auto c = get<2>(read(*ctx, (w >> 10) & 31), 0);
            const bool negative_product = Op == mm_a64_fop::fmsub || Op == mm_a64_fop::fnmadd;
            if constexpr (Op == mm_a64_fop::fnmadd || Op == mm_a64_fop::fnmsub) c ^= static_cast<std::uint16_t>(fp.sign);
            value = fp.fused(a ^ (negative_product ? fp.sign : 0), b, c);
        }
        put<2>(result, 0, value);
    }
    std::memcpy(ctx->v[d], result.data(), 16);
}

template<mm_simd_helper Kind> void vector_half(mm_cpu_context *ctx, std::uint32_t w) {
    const unsigned d = w & 31, n = (w >> 5) & 31, m = (w >> 16) & 31, lanes = (w & (1u << 30)) ? 8 : 4;
    const bool u = (w >> 29) & 1, subtract = (w >> 23) & 1;
    const auto first = read(*ctx, n), second = read(*ctx, m), dest = read(*ctx, d);
    vector result{};
    fp_bits<2> fp{ctx->fpcr, ctx->fpsr};
    for (unsigned i = 0; i < lanes; i++) {
        auto a = get<2>(first, i), b = get<2>(second, i);
        std::uint64_t value = 0;
        if constexpr (Kind == mm_simd_helper::fp_half_element) {
            const unsigned index = ((w >> 20) & 3) | ((w >> 9) & 4);
            b = get<2>(read(*ctx, m & 15), index);
            const unsigned op = (w >> 12) & 15;
            value = op == 9 ? fp.multiply(a, b) : fp.fused(a ^ (op == 5 ? fp.sign : 0), b, get<2>(dest, i));
        } else if constexpr (Kind == mm_simd_helper::fp_half_misc) {
            const auto op = w & 0xbffffc00u;
            if (op == 0x0ef9b800u || op == 0x2ef9b800u) value = fp.to_integer(a, 16, !u);
            else if (op == 0x0e79d800u || op == 0x2e79d800u) value = from_integer(fp, a, 16, !u);
            else {
                const auto r = fp.relation(a, 0, op != 0x0ef8d800u);
                const bool yes = op == 0x0ef8d800u ? r == 6 : op == 0x0ef8e800u ? r == 8 : r == 6 || r == 2;
                value = yes ? 0xffff : 0;
            }
        } else {
            switch ((w >> 11) & 31) {
            case 1: value = fp.fused(a ^ (subtract ? fp.sign : 0), b, get<2>(dest, i)); break;
            case 2:
                if (u && !subtract) {
                    const auto &source = i < lanes / 2 ? first : second;
                    const unsigned j = (i % (lanes / 2)) * 2;
                    value = fp.add(get<2>(source, j), get<2>(source, j + 1));
                } else {
                    value = subtract ? fp.subtract(a, b) : fp.add(a, b);
                    if (u) value &= ~fp.sign;
                }
                break;
            case 3: value = fp.multiply(a, b); break;
            case 4: {
                const auto r = fp.relation(a, b, u);
                value = (u ? subtract ? r == 2 : r == 2 || r == 6 : r == 6) ? 0xffff : 0;
                break;
            }
            case 6: value = fp.minmax(a, b, subtract); break;
            case 7: value = fp.divide(a, b); break;
            }
        }
        put<2>(result, i, value);
    }
    std::memcpy(ctx->v[d], result.data(), 16);
}
}

mm_simd_native mm_simd_half_native(mm_simd_helper kind) {
    switch (kind) {
    case mm_simd_helper::fp_half_vector: return &vector_half<mm_simd_helper::fp_half_vector>;
    case mm_simd_helper::fp_half_element: return &vector_half<mm_simd_helper::fp_half_element>;
    case mm_simd_helper::fp_half_misc: return &vector_half<mm_simd_helper::fp_half_misc>;
    default: return nullptr;
    }
}
namespace mm_emit {
bool emit_half_fp(mm_x64_buf &buf, const mm_a64_insn &insn) {
    mm_simd_native function = nullptr;
    switch (insn.fop) {
#define HALF(K) case mm_a64_fop::K: function = &scalar_half<mm_a64_fop::K>; break
    HALF(fcvt); HALF(fcvtzs); HALF(fcvtzu); HALF(fcmp); HALF(fcmp0); HALF(scvtf); HALF(ucvtf);
    HALF(fmov); HALF(fabs); HALF(fneg); HALF(fmul); HALF(fdiv); HALF(fadd); HALF(fsub);
    HALF(fmax); HALF(fmin); HALF(fcsel); HALF(fmov_imm); HALF(fmadd); HALF(fmsub); HALF(fnmadd); HALF(fnmsub);
#undef HALF
    default: break;
    }
    return emit_simd_native(buf, function, insn.raw);
}
}

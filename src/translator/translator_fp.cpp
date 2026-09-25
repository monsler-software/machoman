/* Floating-point emission, scalar and vector, on SSE2/SSE4.1/FMA3.
 * Scalar forms compute in lane 0 of an xmm register and then write the
 * whole V register with the rest zeroed, as AArch64 requires. Rounding
 * follows MXCSR (round to nearest by default; FPCR is stored but not
 * yet applied). Known approximations: fmax/fmin NaN propagation follows
 * x86 (second operand wins), vector int conversions saturate per lane
 * only for the unsigned path. */
#include "emit.h"

#include <cmath>

namespace mm_emit {
namespace {

using fop = mm_a64_fop;
using fk = mm_x64_fk;
using fp = mm_x64_fp;

fk kind(int esize, bool packed) { return esize == 8 ? (packed ? fk::pd : fk::sd) : (packed ? fk::ps : fk::ss); }

constexpr std::uint64_t SIGN32 = 0x8000000080000000ull, SIGN64 = 0x8000000000000000ull;
constexpr std::uint64_t MAG32 = 0x7FFFFFFF7FFFFFFFull, MAG64 = 0x7FFFFFFFFFFFFFFFull;
constexpr std::uint64_t EFL_CF = 1u << 0, EFL_ZF = 1u << 6, EFL_SF = 1u << 7, EFL_OF = 1u << 11;
constexpr std::int32_t OFF_EFLAGS = offsetof(mm_cpu_context, eflags);

std::uint64_t f64bits(double d) { std::uint64_t u; __builtin_memcpy(&u, &d, 8); return u; }
std::uint64_t f32bits(float f) { std::uint32_t u; __builtin_memcpy(&u, &f, 4); return u; }
std::uint64_t splat(int esize, double v) { return esize == 8 ? f64bits(v) : f32bits(static_cast<float>(v)) * 0x100000001ull; }

/* Store lane 0 of `xmm` as the scalar result of rd (rest zero). */
void store_scalar(mm_x64_buf &buf, std::uint8_t rd, int xmm, int esize) {
    const int z = xmm == X3 ? X2 : X3;
    mm_x64_emit_sse_rr(buf, mm_x64_sse::pxor, z, z);
    mm_x64_emit_fp_rr(buf, fp::mov, kind(esize, false), z, xmm);
    vstore(buf, rd, z, true);
}

void vstore_fp(mm_x64_buf &buf, const mm_a64_insn &insn, int xmm) {
    if (insn.fvec) vstore(buf, insn.rd, xmm, insn.q);
    else store_scalar(buf, insn.rd, xmm, insn.elem_size);
}

/* ucomis flags -> guest NZCV image in ctx.eflags: lt 1000, eq 0110, gt 0010, unordered 0011. */
void flags_from_ucomi(mm_x64_buf &buf) {
    const std::size_t j_un = mm_x64_emit_jcc_rel8(buf, mm_x64_cc::p);
    const std::size_t j_lt = mm_x64_emit_jcc_rel8(buf, mm_x64_cc::b);
    const std::size_t j_eq = mm_x64_emit_jcc_rel8(buf, mm_x64_cc::e);
    mm_x64_emit_mov_imm(buf, A, 0x2);                          /* gt: C=1 -> CF=0 */
    const std::size_t d1 = mm_x64_emit_jmp_rel8(buf);
    mm_x64_patch_rel8_to_here(buf, j_eq);
    mm_x64_emit_mov_imm(buf, A, 0x2 | EFL_ZF);                 /* eq: Z=1 C=1 */
    const std::size_t d2 = mm_x64_emit_jmp_rel8(buf);
    mm_x64_patch_rel8_to_here(buf, j_lt);
    mm_x64_emit_mov_imm(buf, A, 0x2 | EFL_SF | EFL_CF);        /* lt: N=1, C=0 -> CF=1 */
    const std::size_t d3 = mm_x64_emit_jmp_rel8(buf);
    mm_x64_patch_rel8_to_here(buf, j_un);
    mm_x64_emit_mov_imm(buf, A, 0x2 | EFL_OF);                 /* unordered: C=1 V=1 */
    mm_x64_patch_rel8_to_here(buf, d1);
    mm_x64_patch_rel8_to_here(buf, d2);
    mm_x64_patch_rel8_to_here(buf, d3);
    mm_x64_emit_mov_store(buf, CTX, OFF_EFLAGS, A);
}

mm_x64_cc host_cc(std::uint8_t cond) {
    static constexpr mm_x64_cc table[14] = {mm_x64_cc::e, mm_x64_cc::ne, mm_x64_cc::ae, mm_x64_cc::b, mm_x64_cc::s, mm_x64_cc::ns,
                                            mm_x64_cc::o, mm_x64_cc::no, mm_x64_cc::a, mm_x64_cc::be, mm_x64_cc::ge, mm_x64_cc::l,
                                            mm_x64_cc::g, mm_x64_cc::le};
    return table[cond & 0xF];
}

std::uint64_t nzcv_to_eflags(std::uint8_t nzcv) {
    std::uint64_t e = 0x2;
    if (nzcv & 8) e |= EFL_SF;
    if (nzcv & 4) e |= EFL_ZF;
    if (!(nzcv & 2)) e |= EFL_CF;
    if (nzcv & 1) e |= EFL_OF;
    return e;
}

/* frinta: round half away from zero = trunc(x) + sign(x) where |x - trunc(x)| >= 0.5. */
void frinta(mm_x64_buf &buf, fk k, int esize, int x) {
    /* X1 = trunc(x); X2 = |x - X1| >= 0.5 mask; X3 = copysign(1, x) & mask; result = X1 + X3 */
    mm_x64_emit_round(buf, k, X1, x, 0x0B);
    mm_x64_emit_sse_rr(buf, mm_x64_sse::movdqa, X2, x);
    mm_x64_emit_fp_rr(buf, fp::sub, k, X2, X1);
    vconst(buf, X3, esize == 8 ? MAG64 : MAG32, esize == 8 ? MAG64 : MAG32);
    mm_x64_emit_fp_rr(buf, fp::and_, k, X2, X3);                       /* |d| */
    vconst(buf, X3, splat(esize, 0.5), splat(esize, 0.5));
    mm_x64_emit_cmpfp(buf, k, X2, X3, 5);                              /* nlt: |d| >= 0.5 */
    vconst(buf, X3, splat(esize, 1.0), splat(esize, 1.0));
    mm_x64_emit_fp_rr(buf, fp::and_, k, X2, X3);                       /* 1.0 where rounding away */
    vconst(buf, X3, esize == 8 ? SIGN64 : SIGN32, esize == 8 ? SIGN64 : SIGN32);
    mm_x64_emit_fp_rr(buf, fp::and_, k, X3, x);                        /* sign of x */
    mm_x64_emit_fp_rr(buf, fp::or_, k, X2, X3);                        /* copysign(1, x) or 0 */
    mm_x64_emit_fp_rr(buf, fp::add, k, X1, X2);
    mm_x64_emit_sse_rr(buf, mm_x64_sse::movdqa, x, X1);
}

std::uint8_t round_mode(fop f) {
    switch (f) {
    case fop::frintn: case fop::fcvtns: case fop::fcvtnu: return 0x08;
    case fop::frintp: case fop::fcvtps: case fop::fcvtpu: return 0x0A;
    case fop::frintm: case fop::fcvtms: case fop::fcvtmu: return 0x09;
    case fop::frintz: case fop::fcvtzs: case fop::fcvtzu: return 0x0B;
    default: return 0x04; /* current MXCSR */
    }
}

/* Converts the double in lane 0 of X0 to an integer in rax with AArch64
 * saturation and NaN -> 0. */
void double_to_int(mm_x64_buf &buf, bool is_signed, bool is64) {
    mm_x64_emit_fp_rr(buf, fp::ucomi, fk::sd, X0, X0);
    const std::size_t j_nan = mm_x64_emit_jcc_rel8(buf, mm_x64_cc::p);
    if (is_signed) {
        if (is64) {
            vconst(buf, X1, f64bits(-9223372036854775808.0), 0);
            mm_x64_emit_fp_rr(buf, fp::max, fk::sd, X0, X1);
            vconst(buf, X1, f64bits(9223372036854775808.0), 0);
            mm_x64_emit_fp_rr(buf, fp::ucomi, fk::sd, X0, X1);
            const std::size_t j_big = mm_x64_emit_jcc_rel8(buf, mm_x64_cc::ae);
            mm_x64_emit_cvttfp2si(buf, true, A, X0, true);
            const std::size_t j_done = mm_x64_emit_jmp_rel8(buf);
            mm_x64_patch_rel8_to_here(buf, j_big);
            mm_x64_emit_mov_imm(buf, A, 0x7FFFFFFFFFFFFFFFull);
            const std::size_t j_done2 = mm_x64_emit_jmp_rel8(buf);
            mm_x64_patch_rel8_to_here(buf, j_nan);
            mm_x64_emit_xor_reg(buf, A, A);
            mm_x64_patch_rel8_to_here(buf, j_done);
            mm_x64_patch_rel8_to_here(buf, j_done2);
        } else {
            vconst(buf, X1, f64bits(-2147483648.0), 0);
            mm_x64_emit_fp_rr(buf, fp::max, fk::sd, X0, X1);
            vconst(buf, X1, f64bits(2147483647.0), 0);
            mm_x64_emit_fp_rr(buf, fp::min, fk::sd, X0, X1);
            mm_x64_emit_cvttfp2si(buf, true, A, X0, true);
            const std::size_t j_done = mm_x64_emit_jmp_rel8(buf);
            mm_x64_patch_rel8_to_here(buf, j_nan);
            mm_x64_emit_xor_reg(buf, A, A);
            mm_x64_patch_rel8_to_here(buf, j_done);
        }
    } else {
        vconst(buf, X1, 0, 0);
        mm_x64_emit_fp_rr(buf, fp::max, fk::sd, X0, X1); /* negatives -> 0 */
        if (is64) {
            vconst(buf, X1, f64bits(18446744073709551616.0), 0);
            mm_x64_emit_fp_rr(buf, fp::ucomi, fk::sd, X0, X1);
            const std::size_t j_max = mm_x64_emit_jcc_rel8(buf, mm_x64_cc::ae);
            vconst(buf, X1, f64bits(9223372036854775808.0), 0);
            mm_x64_emit_fp_rr(buf, fp::ucomi, fk::sd, X0, X1);
            const std::size_t j_high = mm_x64_emit_jcc_rel8(buf, mm_x64_cc::ae);
            mm_x64_emit_cvttfp2si(buf, true, A, X0, true);
            const std::size_t j_done = mm_x64_emit_jmp_rel8(buf);
            mm_x64_patch_rel8_to_here(buf, j_high);
            mm_x64_emit_fp_rr(buf, fp::sub, fk::sd, X0, X1);
            mm_x64_emit_cvttfp2si(buf, true, A, X0, true);
            mm_x64_emit_mov_imm(buf, T, SIGN64);
            mm_x64_emit_xor_reg(buf, A, T);
            const std::size_t j_done2 = mm_x64_emit_jmp_rel8(buf);
            mm_x64_patch_rel8_to_here(buf, j_max);
            mm_x64_emit_mov_imm(buf, A, ~0ull);
            const std::size_t j_done3 = mm_x64_emit_jmp_rel8(buf);
            mm_x64_patch_rel8_to_here(buf, j_nan);
            mm_x64_emit_xor_reg(buf, A, A);
            mm_x64_patch_rel8_to_here(buf, j_done);
            mm_x64_patch_rel8_to_here(buf, j_done2);
            mm_x64_patch_rel8_to_here(buf, j_done3);
        } else {
            vconst(buf, X1, f64bits(4294967295.0), 0);
            mm_x64_emit_fp_rr(buf, fp::min, fk::sd, X0, X1);
            mm_x64_emit_cvttfp2si(buf, true, A, X0, true);
            const std::size_t j_done = mm_x64_emit_jmp_rel8(buf);
            mm_x64_patch_rel8_to_here(buf, j_nan);
            mm_x64_emit_xor_reg(buf, A, A);
            mm_x64_patch_rel8_to_here(buf, j_done);
        }
    }
}

/* The fixed-point conversion forms carry the number of fractional bits in
 * `imm`; converting is the integer conversion plus a scale by a power of
 * two, which is exact. */
void scale_by_power_of_two(mm_x64_buf &buf, int esize, int exponent) {
    const double factor = std::ldexp(1.0, exponent);
    vconst(buf, X1, esize == 8 ? f64bits(factor) : f32bits(static_cast<float>(factor)), 0);
    mm_x64_emit_fp_rr(buf, fp::mul, kind(esize, false), X0, X1);
}

/* The same scale applied to every lane of X0. */
void scale_vector_by_power_of_two(mm_x64_buf &buf, int esize, int exponent) {
    const double factor = std::ldexp(1.0, exponent);
    vconst(buf, X1, splat(esize, factor), splat(esize, factor));
    mm_x64_emit_fp_rr(buf, fp::mul, esize == 8 ? fk::pd : fk::ps, X0, X1);
}

/* rax (integer of width is64) -> double/float in lane 0 of X0 (rest zero). */
void int_to_fp(mm_x64_buf &buf, bool is_signed, bool is64, int esize) {
    const bool dbl = esize == 8;
    mm_x64_emit_sse_rr(buf, mm_x64_sse::pxor, X0, X0);
    if (is_signed || !is64) {
        if (!is_signed) mm_x64_emit_mov_reg32(buf, A, A); /* unsigned 32 fits a signed 64 */
        mm_x64_emit_cvtsi2fp(buf, dbl, X0, A, is_signed ? is64 : true);
        return;
    }
    /* unsigned 64: if the top bit is set, convert (x >> 1 | x & 1) and double it */
    mm_x64_emit_test_rr(buf, A, A, true);
    const std::size_t j_neg = mm_x64_emit_jcc_rel8(buf, mm_x64_cc::s);
    mm_x64_emit_cvtsi2fp(buf, dbl, X0, A, true);
    const std::size_t j_done = mm_x64_emit_jmp_rel8(buf);
    mm_x64_patch_rel8_to_here(buf, j_neg);
    mm_x64_emit_mov_reg(buf, T, A);
    mm_x64_emit_shr_imm(buf, A, 1);
    mm_x64_emit_alu_imm32(buf, mm_x64_alu::and_, T, 1, true);
    mm_x64_emit_or_reg(buf, A, T);
    mm_x64_emit_cvtsi2fp(buf, dbl, X0, A, true);
    mm_x64_emit_fp_rr(buf, fp::add, kind(esize, false), X0, X0);
    mm_x64_patch_rel8_to_here(buf, j_done);
}

bool cvt_signed(fop f) {
    switch (f) {
    case fop::fcvtzs: case fop::fcvtns: case fop::fcvtps: case fop::fcvtms: case fop::fcvtas: case fop::scvtf: return true;
    default: return false;
    }
}

/* Lane-broadcast of element `idx` of X1 (esize 4/8). */
void dup_lane(mm_x64_buf &buf, int esize, int idx) {
    if (esize == 8) mm_x64_emit_pshufd(buf, X1, X1, idx ? 0xEE : 0x44);
    else mm_x64_emit_pshufd(buf, X1, X1, static_cast<std::uint8_t>(idx * 0x55));
}

} // namespace

bool emit_fp_op(mm_x64_buf &buf, const mm_a64_insn &insn) {
    const int e = insn.elem_size;
    const bool vec = insn.fvec;
    const fk k = kind(e, vec);

    switch (insn.fop) {
    case fop::fmov:
        vload(buf, X0, insn.rn);
        vstore_fp(buf, insn, X0);
        return true;
    case fop::fmov_imm:
        vconst(buf, X0, static_cast<std::uint64_t>(insn.imm), 0);
        vstore(buf, insn.rd, X0, true);
        return true;
    case fop::fabs:
        vload(buf, X0, insn.rn);
        vconst(buf, X1, e == 8 ? MAG64 : MAG32, e == 8 ? MAG64 : MAG32);
        mm_x64_emit_fp_rr(buf, fp::and_, k, X0, X1);
        vstore_fp(buf, insn, X0);
        return true;
    case fop::fneg:
        vload(buf, X0, insn.rn);
        vconst(buf, X1, e == 8 ? SIGN64 : SIGN32, e == 8 ? SIGN64 : SIGN32);
        mm_x64_emit_fp_rr(buf, fp::xor_, k, X0, X1);
        vstore_fp(buf, insn, X0);
        return true;
    case fop::fsqrt:
        vload(buf, X0, insn.rn);
        mm_x64_emit_fp_rr(buf, fp::sqrt, k, X0, X0);
        vstore_fp(buf, insn, X0);
        return true;
    case fop::fcvt: /* scalar precision change: imm = destination width */
        vload(buf, X0, insn.rn);
        if (insn.imm == 8) mm_x64_emit_fp_rr(buf, fp::cvt_widen, fk::ss, X0, X0);
        else mm_x64_emit_fp_rr(buf, fp::cvt_narrow, fk::sd, X0, X0);
        store_scalar(buf, insn.rd, X0, static_cast<int>(insn.imm));
        return true;
    case fop::frintn: case fop::frintp: case fop::frintm: case fop::frintz: case fop::frintx: case fop::frinti:
        vload(buf, X0, insn.rn);
        mm_x64_emit_round(buf, k, X0, X0, round_mode(insn.fop));
        vstore_fp(buf, insn, X0);
        return true;
    case fop::frinta:
        vload(buf, X0, insn.rn);
        frinta(buf, k, e, X0);
        vstore_fp(buf, insn, X0);
        return true;

    case fop::fmul: case fop::fdiv: case fop::fadd: case fop::fsub: case fop::fmax: case fop::fmin:
    case fop::fmaxnm: case fop::fminnm: case fop::fnmul: {
        vload(buf, X0, insn.rn);
        vload(buf, X1, insn.rm);
        fp op;
        switch (insn.fop) {
        case fop::fmul: case fop::fnmul: op = fp::mul; break;
        case fop::fdiv: op = fp::div; break;
        case fop::fadd: op = fp::add; break;
        case fop::fsub: op = fp::sub; break;
        case fop::fmax: case fop::fmaxnm: op = fp::max; break;
        default: op = fp::min; break;
        }
        mm_x64_emit_fp_rr(buf, op, k, X0, X1);
        if (insn.fop == fop::fnmul) {
            vconst(buf, X1, e == 8 ? SIGN64 : SIGN32, e == 8 ? SIGN64 : SIGN32);
            mm_x64_emit_fp_rr(buf, fp::xor_, k, X0, X1);
        }
        vstore_fp(buf, insn, X0);
        return true;
    }

    case fop::fmadd: case fop::fmsub: case fop::fnmadd: case fop::fnmsub: {
        vload(buf, X0, insn.rn);
        vload(buf, X1, insn.rm);
        vload(buf, X2, insn.ra);
        /* 213: X0 = op(X1 * X0, X2) */
        const mm_x64_fma f = insn.fop == fop::fmadd ? mm_x64_fma::fmadd : insn.fop == fop::fmsub ? mm_x64_fma::fnmadd
                           : insn.fop == fop::fnmadd ? mm_x64_fma::fnmsub : mm_x64_fma::fmsub;
        mm_x64_emit_fma213(buf, f, e == 8, X0, X1, X2);
        store_scalar(buf, insn.rd, X0, e);
        return true;
    }

    case fop::fcmp: case fop::fcmp0:
        vload(buf, X0, insn.rn);
        if (insn.fop == fop::fcmp0) mm_x64_emit_sse_rr(buf, mm_x64_sse::pxor, X1, X1);
        else vload(buf, X1, insn.rm);
        mm_x64_emit_fp_rr(buf, fp::ucomi, kind(e, false), X0, X1);
        flags_from_ucomi(buf);
        return true;
    case fop::fccmp: {
        std::size_t to_else = 0;
        const bool conditional = insn.cond < 14;
        if (conditional) {
            mm_x64_emit_push_mem(buf, CTX, OFF_EFLAGS);
            mm_x64_emit_popf(buf);
            to_else = mm_x64_emit_jcc_rel8(buf, static_cast<mm_x64_cc>(static_cast<std::uint8_t>(host_cc(insn.cond)) ^ 1));
        }
        vload(buf, X0, insn.rn);
        vload(buf, X1, insn.rm);
        mm_x64_emit_fp_rr(buf, fp::ucomi, kind(e, false), X0, X1);
        flags_from_ucomi(buf);
        if (conditional) {
            const std::size_t to_end = mm_x64_emit_jmp_rel8(buf);
            mm_x64_patch_rel8_to_here(buf, to_else);
            mm_x64_emit_mov_store_imm32(buf, CTX, OFF_EFLAGS, static_cast<std::int32_t>(nzcv_to_eflags(insn.nzcv)));
            mm_x64_patch_rel8_to_here(buf, to_end);
        }
        return true;
    }
    case fop::fcsel:
        mm_x64_emit_mov_load(buf, A, CTX, voff(insn.rm, 0));
        mm_x64_emit_mov_load(buf, B, CTX, voff(insn.rn, 0));
        if (insn.cond >= 14) {
            mm_x64_emit_mov_reg(buf, A, B);
        } else {
            mm_x64_emit_push_mem(buf, CTX, OFF_EFLAGS);
            mm_x64_emit_popf(buf);
            mm_x64_emit_cmovcc(buf, host_cc(insn.cond), A, B);
        }
        mm_x64_emit_mov_store(buf, CTX, voff(insn.rd, 0), A);
        store_v_zero_high(buf, insn.rd);
        return true;

    case fop::fcvtzs: case fop::fcvtzu: case fop::fcvtns: case fop::fcvtnu: case fop::fcvtps: case fop::fcvtpu:
    case fop::fcvtms: case fop::fcvtmu: case fop::fcvtas: case fop::fcvtau: {
        const bool sg = cvt_signed(insn.fop);
        if (!vec && insn.op == mm_a64_opcode::fop && insn.simd_scalar) {
            /* scalar SIMD form: Vd <- convert(Vn), integer width = FP width */
            vload(buf, X0, insn.rn);
            if (insn.fop == fop::fcvtas || insn.fop == fop::fcvtau) frinta(buf, kind(e, false), e, X0);
            else if (insn.fop != fop::fcvtzs && insn.fop != fop::fcvtzu) mm_x64_emit_round(buf, kind(e, false), X0, X0, round_mode(insn.fop));
            if (e == 4) mm_x64_emit_fp_rr(buf, fp::cvt_widen, fk::ss, X0, X0);
            if (insn.imm) scale_by_power_of_two(buf, 8, static_cast<int>(insn.imm));
            double_to_int(buf, sg, e == 8);
            mm_x64_emit_mov_store(buf, CTX, voff(insn.rd, 0), A);
            store_v_zero_high(buf, insn.rd);
            return true;
        }
        if (!vec) {
            vload(buf, X0, insn.rn);
            if (insn.fop == fop::fcvtas || insn.fop == fop::fcvtau) frinta(buf, kind(e, false), e, X0);
            else if (insn.fop != fop::fcvtzs && insn.fop != fop::fcvtzu) mm_x64_emit_round(buf, kind(e, false), X0, X0, round_mode(insn.fop));
            if (e == 4) mm_x64_emit_fp_rr(buf, fp::cvt_widen, fk::ss, X0, X0);
            /* Fixed point: the result is the value scaled by 2^fbits, and
             * the scaling happens in double precision so the saturation
             * check below sees the true magnitude. */
            if (insn.imm) scale_by_power_of_two(buf, 8, static_cast<int>(insn.imm));
            double_to_int(buf, sg, insn.is64);
            store(buf, insn.rd, A, insn.is64, reg31::xzr);
            return true;
        }
        /* vector: per lane through the scalar path (exact saturation), rebuilt in X2 */
        const int lanes = (insn.q ? 16 : 8) / e;
        for (int i = 0; i < lanes; i++) {
            vload(buf, X0, insn.rn);
            if (e == 8) { if (i) mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X0, 8); }
            else if (i) mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X0, static_cast<std::uint8_t>(4 * i));
            if (insn.fop == fop::fcvtas || insn.fop == fop::fcvtau) frinta(buf, kind(e, false), e, X0);
            else if (insn.fop != fop::fcvtzs && insn.fop != fop::fcvtzu) mm_x64_emit_round(buf, kind(e, false), X0, X0, round_mode(insn.fop));
            if (e == 4) mm_x64_emit_fp_rr(buf, fp::cvt_widen, fk::ss, X0, X0);
            if (insn.imm) scale_by_power_of_two(buf, 8, static_cast<int>(insn.imm));
            double_to_int(buf, sg, e == 8);
            mm_x64_emit_store(buf, CTX, voff(insn.rd, 0) + i * e, A, e); /* rd lanes are only read via rn loads above: rd == rn is handled by re-loading? no */
        }
        if (!insn.q) store_v_zero_high(buf, insn.rd);
        return true;
    }
    case fop::scvtf: case fop::ucvtf: {
        const bool sg = insn.fop == fop::scvtf;
        if (!vec && insn.simd_scalar) {
            mm_x64_emit_mov_load(buf, A, CTX, voff(insn.rn, 0));
            if (e == 4) { if (sg) mm_x64_emit_movsxd(buf, A, A); else mm_x64_emit_mov_reg32(buf, A, A); }
            int_to_fp(buf, sg, e == 8, e);
            if (insn.imm) scale_by_power_of_two(buf, e, -static_cast<int>(insn.imm));
            vstore(buf, insn.rd, X0, true);
            return true;
        }
        if (!vec) {
            load(buf, A, insn.rn, reg31::xzr);
            if (!insn.is64) { if (sg) mm_x64_emit_movsxd(buf, A, A); else mm_x64_emit_mov_reg32(buf, A, A); }
            int_to_fp(buf, sg, insn.is64, e);
            if (insn.imm) scale_by_power_of_two(buf, e, -static_cast<int>(insn.imm));
            vstore(buf, insn.rd, X0, true);
            return true;
        }
        if (e == 4 && sg) {
            vload(buf, X0, insn.rn);
            mm_x64_emit_cvtdq2ps(buf, X0, X0);
            if (insn.imm) scale_vector_by_power_of_two(buf, e, -static_cast<int>(insn.imm));
            vstore(buf, insn.rd, X0, insn.q);
            return true;
        }
        if (e == 4) { /* unsigned 32: hi*65536 + lo */
            vload(buf, X0, insn.rn);
            mm_x64_emit_sse_rr(buf, mm_x64_sse::movdqa, X1, X0);
            mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrld, X1, 16);
            vconst(buf, X2, 0x0000FFFF0000FFFFull, 0x0000FFFF0000FFFFull);
            mm_x64_emit_sse_rr(buf, mm_x64_sse::pand, X0, X2);
            mm_x64_emit_cvtdq2ps(buf, X0, X0);
            mm_x64_emit_cvtdq2ps(buf, X1, X1);
            vconst(buf, X2, splat(4, 65536.0), splat(4, 65536.0));
            mm_x64_emit_fp_rr(buf, fp::mul, fk::ps, X1, X2);
            mm_x64_emit_fp_rr(buf, fp::add, fk::ps, X0, X1);
            if (insn.imm) scale_vector_by_power_of_two(buf, e, -static_cast<int>(insn.imm));
            vstore(buf, insn.rd, X0, insn.q);
            return true;
        }
        /* 2d: per lane */
        for (int i = 0; i < 2; i++) {
            mm_x64_emit_mov_load(buf, A, CTX, voff(insn.rn, i));
            int_to_fp(buf, sg, true, 8);
            if (i == 0) mm_x64_emit_sse_rr(buf, mm_x64_sse::movdqa, X2, X0);
            else mm_x64_emit_sse_rr(buf, mm_x64_sse::punpcklqdq, X2, X0);
        }
        mm_x64_emit_sse_rr(buf, mm_x64_sse::movdqa, X0, X2);
        if (insn.imm) scale_vector_by_power_of_two(buf, e, -static_cast<int>(insn.imm));
        vstore(buf, insn.rd, X0, true);
        return true;
    }

    /* ---- vector only ---------------------------------------------------- */
    case fop::fcmeq: case fop::fcmge: case fop::fcmgt: case fop::fcmeq0: case fop::fcmge0: case fop::fcmgt0:
    case fop::fcmle0: case fop::fcmlt0: {
        vload(buf, X0, insn.rn);
        const bool zero = insn.fop >= fop::fcmeq0 && insn.fop <= fop::fcmlt0;
        if (zero) mm_x64_emit_sse_rr(buf, mm_x64_sse::pxor, X1, X1);
        else vload(buf, X1, insn.rm);
        int out = X0;
        switch (insn.fop) {
        case fop::fcmeq: case fop::fcmeq0: mm_x64_emit_cmpfp(buf, k, X0, X1, 0); break;
        case fop::fcmge: case fop::fcmge0: mm_x64_emit_cmpfp(buf, k, X1, X0, 2); out = X1; break; /* rm <= rn */
        case fop::fcmgt: case fop::fcmgt0: mm_x64_emit_cmpfp(buf, k, X1, X0, 1); out = X1; break; /* rm < rn */
        case fop::fcmle0: mm_x64_emit_cmpfp(buf, k, X0, X1, 2); break;
        default:          mm_x64_emit_cmpfp(buf, k, X0, X1, 1); break; /* fcmlt0 */
        }
        vstore(buf, insn.rd, out, insn.q);
        return true;
    }
    case fop::fmla: case fop::fmls:
        vload(buf, X0, insn.rd);
        vload(buf, X1, insn.rn);
        vload(buf, X2, insn.rm);
        mm_x64_emit_fma231(buf, insn.fop == fop::fmla ? mm_x64_fma::fmadd : mm_x64_fma::fnmadd, e == 8, true, X0, X1, X2);
        vstore(buf, insn.rd, X0, insn.q);
        return true;
    case fop::fmul_elem: case fop::fmla_elem: case fop::fmls_elem:
        vload(buf, X1, insn.rm);
        dup_lane(buf, e, insn.elem_index);
        if (insn.fop == fop::fmul_elem) {
            vload(buf, X0, insn.rn);
            mm_x64_emit_fp_rr(buf, fp::mul, k, X0, X1);
        } else {
            vload(buf, X0, insn.rd);
            vload(buf, X2, insn.rn);
            mm_x64_emit_fma231(buf, insn.fop == fop::fmla_elem ? mm_x64_fma::fmadd : mm_x64_fma::fnmadd, e == 8, true, X0, X2, X1);
        }
        vstore(buf, insn.rd, X0, insn.q);
        return true;
    case fop::faddp: case fop::fmaxp: case fop::fminp:
        pairwise_split(buf, insn, e);
        mm_x64_emit_fp_rr(buf, insn.fop == fop::faddp ? fp::add : insn.fop == fop::fmaxp ? fp::max : fp::min, k, X2, X3);
        if (insn.imm == 1) store_scalar(buf, insn.rd, X2, e); /* scalar pairwise: lane 0 only */
        else vstore(buf, insn.rd, X2, insn.q);
        return true;
    case fop::fabd:
        vload(buf, X0, insn.rn);
        vload(buf, X1, insn.rm);
        mm_x64_emit_fp_rr(buf, fp::sub, k, X0, X1);
        vconst(buf, X1, e == 8 ? MAG64 : MAG32, e == 8 ? MAG64 : MAG32);
        mm_x64_emit_fp_rr(buf, fp::and_, k, X0, X1);
        vstore_fp(buf, insn, X0);
        return true;
    case fop::fcvtl: case fop::fcvtl2:
        vload(buf, X0, insn.rn);
        if (insn.fop == fop::fcvtl2) mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X0, 8);
        mm_x64_emit_fp_rr(buf, fp::cvt_widen, fk::ps, X0, X0);
        vstore(buf, insn.rd, X0, true);
        return true;
    case fop::fcvtn: case fop::fcvtn2:
        vload(buf, X0, insn.rn);
        mm_x64_emit_fp_rr(buf, fp::cvt_narrow, fk::pd, X0, X0);
        if (insn.fop == fop::fcvtn) { vstore(buf, insn.rd, X0, false); return true; }
        vload(buf, X1, insn.rd);
        mm_x64_emit_sse_rr(buf, mm_x64_sse::punpcklqdq, X1, X0);
        vstore(buf, insn.rd, X1, true);
        return true;
    case fop::fmov_gpr_lane:
        return false;
    }
    return false;
}

} // namespace mm_emit

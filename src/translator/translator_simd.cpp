/* AdvSIMD integer emission. Operands are moved from the context into
 * xmm0-xmm3, operated on with SSE2/SSSE3/SSE4.1, and stored back. Lane
 * widths follow insn.elem_size; 64-bit (Q=0) forms store the low half and
 * zero the high half, as the architecture requires. */
#include "emit.h"

namespace mm_emit {
namespace {

using vop = mm_a64_vop;
using sse = mm_x64_sse;

sse by_size(int esize, sse b, sse w, sse d, sse q) {
    return esize == 1 ? b : esize == 2 ? w : esize == 4 ? d : q;
}

std::uint64_t lane_mask(int esize) { /* low esize bytes of every 2*esize lane */
    switch (esize) {
    case 1: return 0x00FF00FF00FF00FFull;
    case 2: return 0x0000FFFF0000FFFFull;
    default: return 0x00000000FFFFFFFFull;
    }
}

std::uint64_t byte_splat(std::uint8_t b) { return 0x0101010101010101ull * b; }

/* pshufb control selecting, for each destination byte, the source byte
 * given by f(i) (or 0x80 for zero). */
template <typename F>
void shuffle_const(mm_x64_buf &buf, int xmm, F f) {
    std::uint64_t lo = 0, hi = 0;
    for (int i = 0; i < 8; i++) lo |= static_cast<std::uint64_t>(f(i) & 0xFF) << (8 * i);
    for (int i = 8; i < 16; i++) hi |= static_cast<std::uint64_t>(f(i) & 0xFF) << (8 * (i - 8));
    vconst(buf, xmm, lo, hi);
}

void vzero(mm_x64_buf &buf, int xmm) { mm_x64_emit_sse_rr(buf, sse::pxor, xmm, xmm); }
void vones(mm_x64_buf &buf, int xmm) { mm_x64_emit_sse_rr(buf, sse::pcmpeqb, xmm, xmm); }
void vmov(mm_x64_buf &buf, int dst, int src) { mm_x64_emit_sse_rr(buf, sse::movdqa, dst, src); }

/* USHL/SSHL interpret just the signed low byte of each count lane. AVX2
 * logical shifts saturate to zero for oversized unsigned counts, so the
 * negative count naturally suppresses the unused direction. No per-lane
 * branches, helpers, or partially written destination register. */
void emit_packed_shift(mm_x64_buf &buf, const mm_a64_insn &insn) {
    using vs = mm_x64_vshift;
    using vv = mm_x64_vshift_var;
    const bool wide = insn.elem_size == 8, sg = insn.vop == vop::sshl;
    vload(buf, X0, insn.rn);
    vload(buf, X1, insn.rm);
    if (!wide) {
        mm_x64_emit_sse_shift_imm(buf, vs::pslld, X1, 24);
        mm_x64_emit_sse_shift_imm(buf, vs::psrad, X1, 24);
    } else {
        mm_x64_emit_sse_shift_imm(buf, vs::psllq, X1, 56);
        vmov(buf, X2, X1);
        mm_x64_emit_sse_shift_imm(buf, vs::psrad, X2, 31); // high dword sign mask
        mm_x64_emit_sse_shift_imm(buf, vs::psrlq, X1, 32);
        mm_x64_emit_sse_shift_imm(buf, vs::psrad, X1, 24); // low dword signed byte
        mm_x64_emit_sse_rr(buf, sse::por, X1, X2);
    }
    vzero(buf, X2);
    mm_x64_emit_sse_rr(buf, wide ? sse::psubq : sse::psubd, X2, X1);
    mm_x64_emit_vshift_var(buf, sg ? vv::psravd : wide ? vv::psrlvq : vv::psrlvd, X3, X0, X2);
    if (sg) {
        // Arithmetic right shift saturates to sign bits, even when unused:
        // retain it only for negative ARM counts. Left shift then provides
        // zero/positive counts, including oversized counts that yield zero.
        vzero(buf, X2);
        mm_x64_emit_sse_rr(buf, sse::pcmpgtd, X2, X1);
        mm_x64_emit_sse_rr(buf, sse::pand, X3, X2);
    }
    mm_x64_emit_vshift_var(buf, wide ? vv::psllvq : vv::psllvd, X0, X0, X1);
    mm_x64_emit_sse_rr(buf, sse::por, X0, X3);
    vstore(buf, insn.rd, X0, insn.q);
}

void shift_imm(mm_x64_buf &buf, int xmm, int esize, bool left, bool arith, std::uint8_t n) {
    using vs = mm_x64_vshift;
    if (esize == 2) mm_x64_emit_sse_shift_imm(buf, left ? vs::psllw : arith ? vs::psraw : vs::psrlw, xmm, n);
    else if (esize == 4) mm_x64_emit_sse_shift_imm(buf, left ? vs::pslld : arith ? vs::psrad : vs::psrld, xmm, n);
    else mm_x64_emit_sse_shift_imm(buf, left ? vs::psllq : vs::psrlq, xmm, n); /* no psraq: handled by caller */
}

/* Byte-lane shifts have no SSE form: shift as words and mask the bits
 * that crossed lanes. Arithmetic byte shifts go through 16-bit lanes. */
void shift_bytes(mm_x64_buf &buf, int xmm, bool left, bool arith, std::uint8_t n) {
    if (n >= 8 && !arith) { vzero(buf, xmm); return; }
    if (!arith) {
        mm_x64_emit_sse_shift_imm(buf, left ? mm_x64_vshift::psllw : mm_x64_vshift::psrlw, xmm, n);
        const std::uint8_t m = left ? static_cast<std::uint8_t>(0xFF << n) : static_cast<std::uint8_t>(0xFF >> n);
        vconst(buf, X3, byte_splat(m), byte_splat(m));
        mm_x64_emit_sse_rr(buf, sse::pand, xmm, X3);
        return;
    }
    /* sshr.b: sign-extend halves to words, shift, pack (results fit int8) */
    if (n > 7) n = 7;
    vmov(buf, X3, xmm);
    mm_x64_emit_sse_rr(buf, sse::pmovsxbw, X2, xmm);
    mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X3, 8);
    mm_x64_emit_sse_rr(buf, sse::pmovsxbw, X3, X3);
    mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psraw, X2, n);
    mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psraw, X3, n);
    mm_x64_emit_sse_rr(buf, sse::packsswb, X2, X3);
    vmov(buf, xmm, X2);
}

/* 64-bit arithmetic right shift has no SSE2 form: do it per half in GPRs. */
void sshr_q(mm_x64_buf &buf, const mm_a64_insn &insn, std::uint8_t n, bool accumulate) {
    for (int h = 0; h < (insn.q ? 2 : 1); h++) {
        mm_x64_emit_mov_load(buf, A, CTX, voff(insn.rn, h));
        mm_x64_emit_sar_imm(buf, A, static_cast<std::uint8_t>(n > 63 ? 63 : n));
        if (accumulate) {
            mm_x64_emit_mov_load(buf, B, CTX, voff(insn.rd, h));
            mm_x64_emit_add_reg(buf, A, B);
        }
        mm_x64_emit_mov_store(buf, CTX, voff(insn.rd, h), A);
    }
    if (!insn.q) store_v_zero_high(buf, insn.rd);
}

/* Lane-wise multiply; bytes go through 16-bit lanes. Result in X0. */
void emit_mul(mm_x64_buf &buf, int esize) {
    if (esize == 2) { mm_x64_emit_sse_rr(buf, sse::pmullw, X0, X1); return; }
    if (esize == 4) { mm_x64_emit_sse_rr(buf, sse::pmulld, X0, X1); return; }
    /* bytes: low halves and high halves widened separately */
    mm_x64_emit_sse_rr(buf, sse::pmovzxbw, X2, X0);
    mm_x64_emit_sse_rr(buf, sse::pmovzxbw, X3, X1);
    mm_x64_emit_sse_rr(buf, sse::pmullw, X2, X3);
    mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X0, 8);
    mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X1, 8);
    mm_x64_emit_sse_rr(buf, sse::pmovzxbw, X0, X0);
    mm_x64_emit_sse_rr(buf, sse::pmovzxbw, X1, X1);
    mm_x64_emit_sse_rr(buf, sse::pmullw, X0, X1);
    vconst(buf, X3, 0x00FF00FF00FF00FFull, 0x00FF00FF00FF00FFull);
    mm_x64_emit_sse_rr(buf, sse::pand, X2, X3);
    mm_x64_emit_sse_rr(buf, sse::pand, X0, X3);
    mm_x64_emit_sse_rr(buf, sse::packuswb, X2, X0);
    vmov(buf, X0, X2);
}

/* Unsigned lane compares via max: a >= b  <=>  max(a, b) == a. X0 = a, X1 = b.
 * Leaves cmhs in X2 and cmhi in X3. 64-bit lanes use a sign-flipped
 * signed compare instead. */
void emit_unsigned_cmp(mm_x64_buf &buf, int esize) {
    if (esize == 8) {
        vconst(buf, X3, 0x8000000000000000ull, 0x8000000000000000ull);
        mm_x64_emit_sse_rr(buf, sse::pxor, X0, X3);
        mm_x64_emit_sse_rr(buf, sse::pxor, X1, X3);
        vmov(buf, X3, X0);
        mm_x64_emit_sse_rr(buf, sse::pcmpgtq, X3, X1);  /* a > b */
        vmov(buf, X2, X0);
        mm_x64_emit_sse_rr(buf, sse::pcmpeqq, X2, X1);
        mm_x64_emit_sse_rr(buf, sse::por, X2, X3);      /* a >= b */
        return;
    }
    vmov(buf, X2, X0);
    mm_x64_emit_sse_rr(buf, by_size(esize, sse::pmaxub, sse::pmaxuw, sse::pmaxud, sse::pmaxud), X2, X1);
    mm_x64_emit_sse_rr(buf, by_size(esize, sse::pcmpeqb, sse::pcmpeqw, sse::pcmpeqd, sse::pcmpeqq), X2, X0); /* a >= b */
    vmov(buf, X3, X0);
    mm_x64_emit_sse_rr(buf, by_size(esize, sse::pcmpeqb, sse::pcmpeqw, sse::pcmpeqd, sse::pcmpeqq), X3, X1); /* a == b */
    mm_x64_emit_sse_rr(buf, sse::pandn, X3, X2);                                                            /* a > b */
}

/* Fold X0's lanes with `op` down to lane 0 (psrldq-halving). */
void reduce(mm_x64_buf &buf, sse op, int esize, bool q) {
    for (int w = q ? 8 : 4; w >= esize; w /= 2) {
        vmov(buf, X1, X0);
        mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X1, static_cast<std::uint8_t>(w));
        mm_x64_emit_sse_rr(buf, op, X0, X1);
    }
}

void store_scalar_lane0(mm_x64_buf &buf, std::uint8_t rd, int esize) {
    mm_x64_emit_movq_to_gpr(buf, A, X0);
    if (esize < 8) mm_x64_emit_extend_reg(buf, A, A, unsigned_kind(esize));
    mm_x64_emit_mov_store(buf, CTX, voff(rd, 0), A);
    store_v_zero_high(buf, rd);
}

/* Narrow X0's 2*esize lanes to esize lanes in the low 8 bytes (truncating). */
void narrow(mm_x64_buf &buf, int esize) {
    if (esize == 4) { mm_x64_emit_pshufd(buf, X0, X0, 0x88); return; }
    vconst(buf, X3, lane_mask(esize), lane_mask(esize));
    mm_x64_emit_sse_rr(buf, sse::pand, X0, X3);
    mm_x64_emit_sse_rr(buf, esize == 1 ? sse::packuswb : sse::packusdw, X0, X0);
}

/* Widen the low 8 bytes of X0 (esize lanes) to 2*esize lanes. */
void widen(mm_x64_buf &buf, int esize, bool is_signed) {
    if (is_signed) mm_x64_emit_sse_rr(buf, esize == 1 ? sse::pmovsxbw : esize == 2 ? sse::pmovsxwd : sse::pmovsxdq, X0, X0);
    else           mm_x64_emit_sse_rr(buf, esize == 1 ? sse::pmovzxbw : esize == 2 ? sse::pmovzxwd : sse::pmovzxdq, X0, X0);
}

/* Evens of X0 -> X2 (low 8 bytes), odds of X0 -> X3 (low 8 bytes). */
void split_even_odd(mm_x64_buf &buf, int esize) {
    shuffle_const(buf, X2, [=](int i) { return i < 8 ? (i / esize) * 2 * esize + i % esize : 0x80; });
    shuffle_const(buf, X3, [=](int i) { return i < 8 ? (i / esize) * 2 * esize + esize + i % esize : 0x80; });
    vmov(buf, X1, X0);
    mm_x64_emit_sse_rr(buf, sse::pshufb, X1, X3);
    vmov(buf, X3, X1);                                /* odds */
    mm_x64_emit_sse_rr(buf, sse::pshufb, X0, X2);
    vmov(buf, X2, X0);                                /* evens */
}

} // namespace

void pairwise_split(mm_x64_buf &buf, const mm_a64_insn &insn, int esize) {
    vload(buf, X0, insn.rn);
    vload(buf, X1, insn.rm);
    if (!insn.q) {
        mm_x64_emit_sse_rr(buf, sse::punpcklqdq, X0, X1); /* 8 lanes of rn:rm */
        split_even_odd(buf, esize);
        return;
    }
    /* q: evens/odds of rn in the low 8 bytes, of rm in the high 8 */
    split_even_odd(buf, esize);                       /* X2 = evens(rn), X3 = odds(rn); X1 clobbered */
    vload(buf, X0, insn.rm);
    shuffle_const(buf, X1, [=](int i) { return i < 8 ? (i / esize) * 2 * esize + i % esize : 0x80; });
    mm_x64_emit_sse_rr(buf, sse::pshufb, X0, X1);      /* X0 = evens(rm) */
    mm_x64_emit_sse_rr(buf, sse::punpcklqdq, X2, X0);  /* X2 = evens(rn) | evens(rm) */
    vload(buf, X0, insn.rm);
    shuffle_const(buf, X1, [=](int i) { return i < 8 ? (i / esize) * 2 * esize + esize + i % esize : 0x80; });
    mm_x64_emit_sse_rr(buf, sse::pshufb, X0, X1);      /* X0 = odds(rm) */
    mm_x64_emit_sse_rr(buf, sse::punpcklqdq, X3, X0);  /* X3 = odds(rn) | odds(rm) */
}

namespace {

/* Pairwise op over the stream (rn, rm): result lanes = op(pairs of rn) ++ op(pairs of rm). */
void emit_pairwise(mm_x64_buf &buf, const mm_a64_insn &insn, sse op) {
    pairwise_split(buf, insn, insn.elem_size);
    mm_x64_emit_sse_rr(buf, op, X2, X3);
    vstore(buf, insn.rd, X2, insn.q);
}

sse add_op(int e)  { return by_size(e, sse::paddb, sse::paddw, sse::paddd, sse::paddq); }
sse sub_op(int e)  { return by_size(e, sse::psubb, sse::psubw, sse::psubd, sse::psubq); }
sse eq_op(int e)   { return by_size(e, sse::pcmpeqb, sse::pcmpeqw, sse::pcmpeqd, sse::pcmpeqq); }
sse gt_op(int e)   { return by_size(e, sse::pcmpgtb, sse::pcmpgtw, sse::pcmpgtd, sse::pcmpgtq); }

} // namespace

bool emit_vector_op(mm_x64_buf &buf, const mm_a64_insn &insn) {
    const int e = insn.elem_size;
    const bool q = insn.q;

    switch (insn.vop) {
    /* ---- three-register lane ops ---------------------------------------- */
    case vop::add: case vop::sub: case vop::cmeq: case vop::cmgt: case vop::cmge:
    case vop::cmhi: case vop::cmhs: case vop::cmtst: case vop::smax: case vop::smin:
    case vop::umax: case vop::umin: case vop::mul: case vop::mla: case vop::mls:
    case vop::uqadd: case vop::sqadd: case vop::uqsub: case vop::sqsub: {
        vload(buf, X0, insn.rn);
        vload(buf, X1, insn.rm);
        int out = X0;
        switch (insn.vop) {
        case vop::add:  mm_x64_emit_sse_rr(buf, add_op(e), X0, X1); break;
        case vop::sub:  mm_x64_emit_sse_rr(buf, sub_op(e), X0, X1); break;
        case vop::cmeq: mm_x64_emit_sse_rr(buf, eq_op(e), X0, X1); break;
        case vop::cmgt: mm_x64_emit_sse_rr(buf, gt_op(e), X0, X1); break;
        case vop::cmge: /* NOT (rm > rn) */
            mm_x64_emit_sse_rr(buf, gt_op(e), X1, X0);
            vones(buf, X0);
            mm_x64_emit_sse_rr(buf, sse::pxor, X0, X1);
            break;
        case vop::cmhi: emit_unsigned_cmp(buf, e); out = X3; break;
        case vop::cmhs: emit_unsigned_cmp(buf, e); out = X2; break;
        case vop::cmtst:
            mm_x64_emit_sse_rr(buf, sse::pand, X0, X1);
            vzero(buf, X1);
            mm_x64_emit_sse_rr(buf, eq_op(e), X0, X1);
            vones(buf, X1);
            mm_x64_emit_sse_rr(buf, sse::pxor, X0, X1);
            break;
        case vop::smax: if (e == 8) return false; mm_x64_emit_sse_rr(buf, by_size(e, sse::pmaxsb, sse::pmaxsw, sse::pmaxsd, sse::pmaxsd), X0, X1); break;
        case vop::smin: if (e == 8) return false; mm_x64_emit_sse_rr(buf, by_size(e, sse::pminsb, sse::pminsw, sse::pminsd, sse::pminsd), X0, X1); break;
        case vop::umax: if (e == 8) return false; mm_x64_emit_sse_rr(buf, by_size(e, sse::pmaxub, sse::pmaxuw, sse::pmaxud, sse::pmaxud), X0, X1); break;
        case vop::umin: if (e == 8) return false; mm_x64_emit_sse_rr(buf, by_size(e, sse::pminub, sse::pminuw, sse::pminud, sse::pminud), X0, X1); break;
        case vop::mul:  if (e == 8) return false; emit_mul(buf, e); break;
        case vop::mla: case vop::mls:
            if (e == 8) return false;
            emit_mul(buf, e);
            vload(buf, X1, insn.rd);
            if (insn.vop == vop::mla) { mm_x64_emit_sse_rr(buf, add_op(e), X0, X1); }
            else { mm_x64_emit_sse_rr(buf, sub_op(e), X1, X0); out = X1; }
            break;
        case vop::uqadd: if (e > 2) return false; mm_x64_emit_sse_rr(buf, e == 1 ? sse::paddusb : sse::paddusw, X0, X1); break;
        case vop::sqadd: if (e > 2) return false; mm_x64_emit_sse_rr(buf, e == 1 ? sse::paddsb : sse::paddsw, X0, X1); break;
        case vop::uqsub: if (e > 2) return false; mm_x64_emit_sse_rr(buf, e == 1 ? sse::psubusb : sse::psubusw, X0, X1); break;
        case vop::sqsub: if (e > 2) return false; mm_x64_emit_sse_rr(buf, e == 1 ? sse::psubsb : sse::psubsw, X0, X1); break;
        default: return false;
        }
        vstore(buf, insn.rd, out, q);
        return true;
    }

    case vop::uabd: case vop::sabd: case vop::uaba: case vop::saba: {
        /* |a - b| per lane, accumulated for the ABA forms. The difference
         * is taken in a 64-bit register, where it cannot wrap. */
        const bool sg = insn.vop == vop::sabd || insn.vop == vop::saba;
        const bool accumulate = insn.vop == vop::uaba || insn.vop == vop::saba;
        const int lanes = (q ? 16 : 8) / e;
        const mm_x64_load lk = sg ? (e == 1 ? mm_x64_load::s8_64 : e == 2 ? mm_x64_load::s16_64
                                                                : e == 4 ? mm_x64_load::s32_64 : mm_x64_load::u64)
                                  : unsigned_kind(e);
        for (int i = 0; i < lanes; i++) {
            const std::int32_t d_off = voff(insn.rd, 0) + i * e;
            mm_x64_emit_load(buf, A, CTX, voff(insn.rn, 0) + i * e, lk);
            mm_x64_emit_load(buf, B, CTX, voff(insn.rm, 0) + i * e, lk);
            mm_x64_emit_sub_reg(buf, A, B);
            mm_x64_emit_mov_reg(buf, T, A);
            mm_x64_emit_sar_imm(buf, T, 63);
            mm_x64_emit_xor_reg(buf, A, T);
            mm_x64_emit_sub_reg(buf, A, T);
            if (accumulate) {
                mm_x64_emit_load(buf, T, CTX, d_off, unsigned_kind(e));
                mm_x64_emit_add_reg(buf, A, T);
            }
            mm_x64_emit_store(buf, CTX, d_off, A, e);
        }
        if (!q) store_v_zero_high(buf, insn.rd);
        return true;
    }

    case vop::urshr: case vop::srshr: {
        /* A shift right that rounds: add half of what is shifted out. */
        const bool sg = insn.vop == vop::srshr;
        const int lanes = (q ? 16 : 8) / e;
        const auto shift = static_cast<std::uint8_t>(insn.imm);
        const mm_x64_load lk = sg ? (e == 1 ? mm_x64_load::s8_64 : e == 2 ? mm_x64_load::s16_64
                                                                : e == 4 ? mm_x64_load::s32_64 : mm_x64_load::u64)
                                  : unsigned_kind(e);
        for (int i = 0; i < lanes; i++) {
            mm_x64_emit_load(buf, A, CTX, voff(insn.rn, 0) + i * e, lk);
            mm_x64_emit_mov_imm(buf, T, std::uint64_t{1} << (shift - 1));
            mm_x64_emit_add_reg(buf, A, T);
            if (sg) mm_x64_emit_sar_imm(buf, A, shift);
            else mm_x64_emit_shr_imm(buf, A, shift);
            mm_x64_emit_store(buf, CTX, voff(insn.rd, 0) + i * e, A, e);
        }
        if (!q) store_v_zero_high(buf, insn.rd);
        return true;
    }

    /* ---- widening and narrowing (three-different) ------------------------ */

    /* The L forms widen both sources, the W forms only the second, and the
     * HN forms narrow a wide result by taking its top half. Everything is
     * done a lane at a time in a GPR, where the wide intermediate is free;
     * `e` is the narrow element throughout, so the wide one is 2*e. The
     * "2" forms read (or, for HN, write) the upper half of the register. */
    case vop::saddl: case vop::saddl2: case vop::uaddl: case vop::uaddl2:
    case vop::ssubl: case vop::ssubl2: case vop::usubl: case vop::usubl2:
    case vop::saddw: case vop::saddw2: case vop::uaddw: case vop::uaddw2:
    case vop::ssubw: case vop::ssubw2: case vop::usubw: case vop::usubw2:
    case vop::smull_v: case vop::smull2_v: case vop::umull_v: case vop::umull2_v:
    case vop::smlal: case vop::smlal2: case vop::umlal: case vop::umlal2:
    case vop::smlsl: case vop::smlsl2: case vop::umlsl: case vop::umlsl2:
    case vop::sabdl: case vop::sabdl2: case vop::uabdl: case vop::uabdl2: {
        const vop op = insn.vop;
        auto one_of = [&](std::initializer_list<vop> set) {
            for (vop v : set) if (op == v) return true;
            return false;
        };
        const bool upper = one_of({vop::saddl2, vop::uaddl2, vop::ssubl2, vop::usubl2, vop::saddw2, vop::uaddw2,
                                   vop::ssubw2, vop::usubw2, vop::smull2_v, vop::umull2_v, vop::smlal2, vop::umlal2,
                                   vop::smlsl2, vop::umlsl2, vop::sabdl2, vop::uabdl2});
        const bool sg = one_of({vop::saddl, vop::saddl2, vop::ssubl, vop::ssubl2, vop::saddw, vop::saddw2,
                                vop::ssubw, vop::ssubw2, vop::smull_v, vop::smull2_v, vop::smlal, vop::smlal2,
                                vop::smlsl, vop::smlsl2, vop::sabdl, vop::sabdl2});
        const bool wide_first = one_of({vop::saddw, vop::saddw2, vop::uaddw, vop::uaddw2,
                                        vop::ssubw, vop::ssubw2, vop::usubw, vop::usubw2});
        const bool subtract = one_of({vop::ssubl, vop::ssubl2, vop::usubl, vop::usubl2,
                                      vop::ssubw, vop::ssubw2, vop::usubw, vop::usubw2});
        const bool multiply = one_of({vop::smull_v, vop::smull2_v, vop::umull_v, vop::umull2_v,
                                      vop::smlal, vop::smlal2, vop::umlal, vop::umlal2,
                                      vop::smlsl, vop::smlsl2, vop::umlsl, vop::umlsl2});
        const bool accumulate = one_of({vop::smlal, vop::smlal2, vop::umlal, vop::umlal2,
                                        vop::smlsl, vop::smlsl2, vop::umlsl, vop::umlsl2});
        const bool negate_product = one_of({vop::smlsl, vop::smlsl2, vop::umlsl, vop::umlsl2});
        const bool absolute = one_of({vop::sabdl, vop::sabdl2, vop::uabdl, vop::uabdl2});

        const int wide = e * 2;
        const int lanes = 8 / e;                    /* a 64-bit half of narrow lanes */
        auto signed_kind = [](int size) {
            return size == 1 ? mm_x64_load::s8_64 : size == 2 ? mm_x64_load::s16_64
                 : size == 4 ? mm_x64_load::s32_64 : mm_x64_load::u64;
        };
        const mm_x64_load nk = sg ? signed_kind(e) : unsigned_kind(e);
        const mm_x64_load wk = sg ? signed_kind(wide) : unsigned_kind(wide);
        /* Preserve both source vectors before writing widened lanes. When
         * Rd aliases a narrow source, the first store overwrites later inputs. */
        vload(buf, X0, insn.rn);
        vload(buf, X1, insn.rm);
        if (upper && !wide_first) mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X0, 8);
        if (upper) mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X1, 8);
        for (int i = 0; i < lanes; i++) {
            const std::int32_t d_off = voff(insn.rd, 0) + i * wide;
            mm_x64_emit_movq_to_gpr(buf, A, X0);
            mm_x64_emit_movq_to_gpr(buf, B, X1);
            mm_x64_emit_extend_reg(buf, A, A, wide_first ? wk : nk);
            mm_x64_emit_extend_reg(buf, B, B, nk);
            mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X0, static_cast<std::uint8_t>(wide_first ? wide : e));
            mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X1, static_cast<std::uint8_t>(e));
            if (multiply) {
                mm_x64_emit_imul_reg(buf, A, B);
                if (negate_product) mm_x64_emit_neg_reg(buf, A);
                if (accumulate) {
                    mm_x64_emit_load(buf, T, CTX, d_off, wk);
                    mm_x64_emit_add_reg(buf, A, T);
                }
            } else if (absolute) {
                /* |a - b| in the wider type, so it never wraps: the
                 * branchless form (x ^ mask) - mask with mask = x >> 63,
                 * which does not depend on flags surviving the sequence. */
                mm_x64_emit_sub_reg(buf, A, B);
                mm_x64_emit_mov_reg(buf, T, A);
                mm_x64_emit_sar_imm(buf, T, 63);
                mm_x64_emit_xor_reg(buf, A, T);
                mm_x64_emit_sub_reg(buf, A, T);
            } else if (subtract) {
                mm_x64_emit_sub_reg(buf, A, B);
            } else {
                mm_x64_emit_add_reg(buf, A, B);
            }
            mm_x64_emit_store(buf, CTX, d_off, A, wide);
        }
        return true;
    }

    case vop::addhn: case vop::addhn2: case vop::raddhn: case vop::raddhn2:
    case vop::subhn: case vop::subhn2: case vop::rsubhn: case vop::rsubhn2: {
        const vop op = insn.vop;
        const bool upper = op == vop::addhn2 || op == vop::raddhn2 || op == vop::subhn2 || op == vop::rsubhn2;
        const bool round = op == vop::raddhn || op == vop::raddhn2 || op == vop::rsubhn || op == vop::rsubhn2;
        const bool subtract = op == vop::subhn || op == vop::subhn2 || op == vop::rsubhn || op == vop::rsubhn2;
        const int wide = e * 2;
        const int lanes = 8 / e;
        const std::uint8_t shift = static_cast<std::uint8_t>(8 * e);
        for (int i = 0; i < lanes; i++) {
            mm_x64_emit_load(buf, A, CTX, voff(insn.rn, 0) + i * wide, unsigned_kind(wide));
            mm_x64_emit_load(buf, B, CTX, voff(insn.rm, 0) + i * wide, unsigned_kind(wide));
            if (subtract) mm_x64_emit_sub_reg(buf, A, B);
            else mm_x64_emit_add_reg(buf, A, B);
            /* The rounding forms add half of the discarded part first; the
             * constant goes through a register because at e == 4 it no
             * longer fits a sign-extended imm32. */
            if (round) {
                mm_x64_emit_mov_imm(buf, T, std::uint64_t{1} << (shift - 1));
                mm_x64_emit_add_reg(buf, A, T);
            }
            mm_x64_emit_shr_imm(buf, A, shift);
            mm_x64_emit_store(buf, CTX, voff(insn.rd, 0) + (upper ? 8 : 0) + i * e, A, e);
        }
        if (!upper) store_v_zero_high(buf, insn.rd);
        return true;
    }

    case vop::uhadd: case vop::shadd: case vop::urhadd: case vop::srhadd:
    case vop::uhsub: case vop::shsub: {
        /* The halving forms add or subtract in one more bit than the lane
         * has and shift the result back, so they never overflow. Done a
         * lane at a time in a GPR, where that extra bit is free; SSE has
         * only the rounding unsigned add (pavgb/pavgw) of the six. */
        const bool sg = insn.vop == vop::shadd || insn.vop == vop::srhadd || insn.vop == vop::shsub;
        const bool round = insn.vop == vop::urhadd || insn.vop == vop::srhadd;
        const bool subtract = insn.vop == vop::uhsub || insn.vop == vop::shsub;
        const int lanes = (q ? 16 : 8) / e;
        const mm_x64_load lk = sg ? (e == 1 ? mm_x64_load::s8_64 : e == 2 ? mm_x64_load::s16_64
                                                                 : e == 4 ? mm_x64_load::s32_64 : mm_x64_load::u64)
                                  : unsigned_kind(e);
        for (int i = 0; i < lanes; i++) {
            mm_x64_emit_load(buf, A, CTX, voff(insn.rn, 0) + i * e, lk);
            mm_x64_emit_load(buf, B, CTX, voff(insn.rm, 0) + i * e, lk);
            if (subtract) mm_x64_emit_sub_reg(buf, A, B);
            else mm_x64_emit_add_reg(buf, A, B);
            if (round) mm_x64_emit_add_imm32(buf, A, 1);
            if (sg) mm_x64_emit_sar_imm(buf, A, 1);
            else mm_x64_emit_shr_imm(buf, A, 1);
            mm_x64_emit_store(buf, CTX, voff(insn.rd, 0) + i * e, A, e);
        }
        if (!q) store_v_zero_high(buf, insn.rd);
        return true;
    }

    case vop::ushl: case vop::sshl: {
        if ((e == 4 || (e == 8 && insn.vop == vop::ushl)) &&
            (mm_x64_codegen_features() & MM_X64_FEATURE_AVX2)) {
            emit_packed_shift(buf, insn);
            return true;
        }
        /* Per lane: count = low byte of rm lane (signed); left if >= 0, else right by -count. */
        const bool sg = insn.vop == vop::sshl;
        const int lanes = (q ? 16 : 8) / e;
        const mm_x64_load lk = sg ? (e == 1 ? mm_x64_load::s8_64 : e == 2 ? mm_x64_load::s16_64 : e == 4 ? mm_x64_load::s32_64 : mm_x64_load::u64)
                                  : unsigned_kind(e);
        for (int i = 0; i < lanes; i++) {
            const std::int32_t n_off = voff(insn.rn, 0) + i * e, m_off = voff(insn.rm, 0) + i * e;
            mm_x64_emit_load(buf, A, CTX, n_off, lk);
            mm_x64_emit_load(buf, B, CTX, m_off, mm_x64_load::s8_64);
            mm_x64_emit_test_rr(buf, B, B, true);
            const std::size_t j_neg = mm_x64_emit_jcc_rel8(buf, mm_x64_cc::s);
            /* left */
            mm_x64_emit_cmp_imm32(buf, B, 8 * e);
            const std::size_t j_zero = mm_x64_emit_jcc_rel8(buf, mm_x64_cc::ae);
            mm_x64_emit_shl_cl(buf, A, false);
            const std::size_t j_done1 = mm_x64_emit_jmp_rel8(buf);
            /* right */
            mm_x64_patch_rel8_to_here(buf, j_neg);
            mm_x64_emit_neg_reg(buf, B);
            mm_x64_emit_cmp_imm32(buf, B, 8 * e);
            const std::size_t j_sat = mm_x64_emit_jcc_rel8(buf, mm_x64_cc::ae);
            if (sg) mm_x64_emit_sar_cl(buf, A, false); else mm_x64_emit_shr_cl(buf, A, false);
            const std::size_t j_done2 = mm_x64_emit_jmp_rel8(buf);
            /* saturated: 0, or all sign bits for sshl */
            mm_x64_patch_rel8_to_here(buf, j_sat);
            if (sg) { mm_x64_emit_sar_imm(buf, A, 63); const std::size_t j = mm_x64_emit_jmp_rel8(buf); mm_x64_patch_rel8_to_here(buf, j_zero); mm_x64_emit_xor_reg(buf, A, A); mm_x64_patch_rel8_to_here(buf, j); }
            else { mm_x64_patch_rel8_to_here(buf, j_zero); mm_x64_emit_xor_reg(buf, A, A); }
            mm_x64_patch_rel8_to_here(buf, j_done1);
            mm_x64_patch_rel8_to_here(buf, j_done2);
            /* lanes of rd may alias rn/rm: stash results in the high half of xmm? simpler: write into X2 lanes */
            mm_x64_emit_store(buf, CTX, voff(insn.rd, 0) + i * e, A, e); /* safe: lane i of rd only read for lane i above */
        }
        if (!q) store_v_zero_high(buf, insn.rd);
        return true;
    }

    case vop::addp:
        if (e == 8 && !q) { /* scalar addp d, v.2d */
            mm_x64_emit_mov_load(buf, A, CTX, voff(insn.rn, 0));
            mm_x64_emit_mov_load(buf, B, CTX, voff(insn.rn, 1));
            mm_x64_emit_add_reg(buf, A, B);
            mm_x64_emit_mov_store(buf, CTX, voff(insn.rd, 0), A);
            store_v_zero_high(buf, insn.rd);
            return true;
        }
        emit_pairwise(buf, insn, add_op(e));
        return true;
    case vop::smaxp: if (e == 8) return false; emit_pairwise(buf, insn, by_size(e, sse::pmaxsb, sse::pmaxsw, sse::pmaxsd, sse::pmaxsd)); return true;
    case vop::sminp: if (e == 8) return false; emit_pairwise(buf, insn, by_size(e, sse::pminsb, sse::pminsw, sse::pminsd, sse::pminsd)); return true;
    case vop::umaxp: if (e == 8) return false; emit_pairwise(buf, insn, by_size(e, sse::pmaxub, sse::pmaxuw, sse::pmaxud, sse::pmaxud)); return true;
    case vop::uminp: if (e == 8) return false; emit_pairwise(buf, insn, by_size(e, sse::pminub, sse::pminuw, sse::pminud, sse::pminud)); return true;

    /* ---- one-register ops ----------------------------------------------- */
    case vop::not_:
        vload(buf, X0, insn.rn);
        vones(buf, X1);
        mm_x64_emit_sse_rr(buf, sse::pxor, X0, X1);
        vstore(buf, insn.rd, X0, q);
        return true;
    case vop::neg:
        vload(buf, X1, insn.rn);
        vzero(buf, X0);
        mm_x64_emit_sse_rr(buf, sub_op(e), X0, X1);
        vstore(buf, insn.rd, X0, q);
        return true;
    case vop::abs:
        if (e == 8) return false;
        vload(buf, X0, insn.rn);
        mm_x64_emit_sse_rr(buf, by_size(e, sse::pabsb, sse::pabsw, sse::pabsd, sse::pabsd), X0, X0);
        vstore(buf, insn.rd, X0, q);
        return true;
    case vop::cmeq0: case vop::cmgt0: case vop::cmlt0: case vop::cmge0: case vop::cmle0: {
        vload(buf, X0, insn.rn);
        vzero(buf, X1);
        int out = X0;
        switch (insn.vop) {
        case vop::cmeq0: mm_x64_emit_sse_rr(buf, eq_op(e), X0, X1); break;
        case vop::cmgt0: mm_x64_emit_sse_rr(buf, gt_op(e), X0, X1); break;
        case vop::cmlt0: mm_x64_emit_sse_rr(buf, gt_op(e), X1, X0); out = X1; break;
        case vop::cmge0: /* NOT (0 > x) */
            mm_x64_emit_sse_rr(buf, gt_op(e), X1, X0); vones(buf, X0); mm_x64_emit_sse_rr(buf, sse::pxor, X0, X1); break;
        default: /* cmle0: NOT (x > 0) */
            mm_x64_emit_sse_rr(buf, gt_op(e), X0, X1); vones(buf, X1); mm_x64_emit_sse_rr(buf, sse::pxor, X0, X1); break;
        }
        vstore(buf, insn.rd, out, q);
        return true;
    }
    case vop::rev64: case vop::rev32: case vop::rev16: {
        const int container = insn.vop == vop::rev64 ? 8 : insn.vop == vop::rev32 ? 4 : 2;
        if (e >= container) return false;
        vload(buf, X0, insn.rn);
        shuffle_const(buf, X1, [=](int i) { return (i / container) * container + (container - e - (i % container) / e * e) + i % e; });
        mm_x64_emit_sse_rr(buf, sse::pshufb, X0, X1);
        vstore(buf, insn.rd, X0, q);
        return true;
    }
    case vop::cnt:
        vload(buf, X0, insn.rn);
        vconst(buf, X3, 0x0F0F0F0F0F0F0F0Full, 0x0F0F0F0F0F0F0F0Full);
        vmov(buf, X1, X0);
        mm_x64_emit_sse_rr(buf, sse::pand, X0, X3);                   /* low nibbles */
        mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrlw, X1, 4);
        mm_x64_emit_sse_rr(buf, sse::pand, X1, X3);                   /* high nibbles */
        vconst(buf, X2, 0x0302020102010100ull, 0x0403030203020201ull); /* popcount LUT */
        vmov(buf, X3, X2);
        mm_x64_emit_sse_rr(buf, sse::pshufb, X2, X0);
        mm_x64_emit_sse_rr(buf, sse::pshufb, X3, X1);
        mm_x64_emit_sse_rr(buf, sse::paddb, X2, X3);
        vstore(buf, insn.rd, X2, q);
        return true;
    case vop::xtn: case vop::xtn2:
        vload(buf, X0, insn.rn);
        narrow(buf, e / 2);
        if (insn.vop == vop::xtn) { vstore(buf, insn.rd, X0, false); return true; }
        vload(buf, X1, insn.rd);
        mm_x64_emit_sse_rr(buf, sse::punpcklqdq, X1, X0);
        vstore(buf, insn.rd, X1, true);
        return true;
    case vop::sqxtn: case vop::sqxtn2: case vop::uqxtn: case vop::uqxtn2:
    case vop::sqxtun: case vop::sqxtun2: {
        const int dest_size = e / 2;
        const bool unsigned_source = insn.vop == vop::uqxtn || insn.vop == vop::uqxtn2;
        const bool signed_dest = insn.vop == vop::sqxtn || insn.vop == vop::sqxtn2;
        const bool high = insn.vop == vop::sqxtn2 || insn.vop == vop::uqxtn2 || insn.vop == vop::sqxtun2;
        const std::uint64_t max = (std::uint64_t{1} << (8 * dest_size - (signed_dest ? 1 : 0))) - 1;
        const std::uint64_t min = signed_dest ? ~max : 0;
        /* Snapshot before any destination stores: SQXTN2 can overwrite
         * lanes that have not yet been read when rd == rn. */
        vload(buf, X0, insn.rn);
        auto clamp = [&](std::uint64_t bound, mm_x64_cc within) {
            mm_x64_emit_mov_imm(buf, B, bound);
            mm_x64_emit_cmp_reg(buf, A, B);
            const auto skip = mm_x64_emit_jcc_rel8(buf, within);
            mm_x64_emit_mov_reg(buf, A, B);
            mm_x64_emit_mov_load(buf, T, CTX, offsetof(mm_cpu_context, fpsr));
            mm_x64_emit_alu_imm32(buf, mm_x64_alu::or_, T, 1u << 27, true); /* cumulative saturation, FPSR.QC */
            mm_x64_emit_mov_store(buf, CTX, offsetof(mm_cpu_context, fpsr), T);
            mm_x64_patch_rel8_to_here(buf, skip);
        };
        for (int i = 0; i < (insn.simd_scalar ? 1 : 16 / e); i++) {
            mm_x64_emit_movq_to_gpr(buf, A, X0);
            if (e < 8) {
                const auto kind = unsigned_source ? unsigned_kind(e)
                    : e == 2 ? mm_x64_load::s16_64 : mm_x64_load::s32_64;
                mm_x64_emit_extend_reg(buf, A, A, kind);
            }
            if (!unsigned_source) clamp(min, mm_x64_cc::ge);
            clamp(max, unsigned_source ? mm_x64_cc::be : mm_x64_cc::le);
            if (insn.simd_scalar) {
                mm_x64_emit_extend_reg(buf, A, A, unsigned_kind(dest_size));
                mm_x64_emit_mov_store(buf, CTX, voff(insn.rd, 0), A);
            } else {
                mm_x64_emit_store(buf, CTX, voff(insn.rd, high ? 1 : 0) + i * dest_size, A, dest_size);
            }
            mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X0, static_cast<std::uint8_t>(e));
        }
        if (!high) store_v_zero_high(buf, insn.rd);
        return true;
    }
    case vop::uaddlp: case vop::saddlp: case vop::uadalp: case vop::sadalp: {
        vload(buf, X0, insn.rn);
        const bool sg = insn.vop == vop::saddlp || insn.vop == vop::sadalp;
        const bool acc = insn.vop == vop::uadalp || insn.vop == vop::sadalp;
        if (e == 4) {
            mm_x64_emit_pshufd(buf, X1, X0, 0x0D);   /* odds -> lanes 0,1 */
            mm_x64_emit_pshufd(buf, X0, X0, 0x08);   /* evens */
            widen(buf, 4, sg);
            vmov(buf, X2, X0);
            vmov(buf, X0, X1);
            widen(buf, 4, sg);
            mm_x64_emit_sse_rr(buf, sse::paddq, X0, X2);
        } else if (sg) { /* sign-extend evens via shift pair, odds via arithmetic shift */
            vmov(buf, X1, X0);
            shift_imm(buf, X0, 2 * e, true, false, static_cast<std::uint8_t>(8 * e));
            shift_imm(buf, X0, 2 * e, false, true, static_cast<std::uint8_t>(8 * e));
            shift_imm(buf, X1, 2 * e, false, true, static_cast<std::uint8_t>(8 * e));
            mm_x64_emit_sse_rr(buf, add_op(2 * e), X0, X1);
        } else {
            vmov(buf, X1, X0);
            vconst(buf, X3, lane_mask(e), lane_mask(e));
            mm_x64_emit_sse_rr(buf, sse::pand, X0, X3);
            shift_imm(buf, X1, 2 * e, false, false, static_cast<std::uint8_t>(8 * e));
            mm_x64_emit_sse_rr(buf, add_op(2 * e), X0, X1);
        }
        if (acc) { vload(buf, X1, insn.rd); mm_x64_emit_sse_rr(buf, add_op(2 * e), X0, X1); }
        vstore(buf, insn.rd, X0, q);
        return true;
    }

    /* ---- shifts by immediate ---------------------------------------------- */
    case vop::sli: case vop::sri: {
        const auto n = static_cast<std::uint8_t>(insn.imm);
        const bool left = insn.vop == vop::sli;
        vload(buf, X0, insn.rn);
        vload(buf, X1, insn.rd);
        vones(buf, X2); /* mask of destination bits replaced by the insert */
        if (e == 1) {
            shift_bytes(buf, X0, left, false, n);
            shift_bytes(buf, X2, left, false, n);
        } else {
            shift_imm(buf, X0, e, left, false, n);
            shift_imm(buf, X2, e, left, false, n);
        }
        mm_x64_emit_sse_rr(buf, sse::pandn, X2, X1);
        mm_x64_emit_sse_rr(buf, sse::por, X0, X2);
        vstore(buf, insn.rd, X0, q);
        return true;
    }
    case vop::sshr: case vop::ushr: case vop::shl: case vop::ssra: case vop::usra: {
        const auto n = static_cast<std::uint8_t>(insn.imm);
        const bool left = insn.vop == vop::shl;
        const bool arith = insn.vop == vop::sshr || insn.vop == vop::ssra;
        const bool acc = insn.vop == vop::ssra || insn.vop == vop::usra;
        if (arith && e == 8) { sshr_q(buf, insn, n, acc); return true; }
        vload(buf, X0, insn.rn);
        if (e == 1) shift_bytes(buf, X0, left, arith, n);
        else if (n >= 8 * e) { if (arith) shift_imm(buf, X0, e, false, true, static_cast<std::uint8_t>(8 * e - 1)); else vzero(buf, X0); }
        else shift_imm(buf, X0, e, left, arith, n);
        if (acc) { vload(buf, X1, insn.rd); mm_x64_emit_sse_rr(buf, add_op(e), X0, X1); }
        vstore(buf, insn.rd, X0, q);
        return true;
    }
    case vop::shrn: case vop::shrn2: {
        const auto n = static_cast<std::uint8_t>(insn.imm);
        vload(buf, X0, insn.rn);
        if (n) shift_imm(buf, X0, e, false, false, n);
        narrow(buf, e / 2);
        if (insn.vop == vop::shrn) { vstore(buf, insn.rd, X0, false); return true; }
        vload(buf, X1, insn.rd);
        mm_x64_emit_sse_rr(buf, sse::punpcklqdq, X1, X0);
        vstore(buf, insn.rd, X1, true);
        return true;
    }
    case vop::sshll: case vop::sshll2: case vop::ushll: case vop::ushll2: {
        const bool sg = insn.vop == vop::sshll || insn.vop == vop::sshll2;
        vload(buf, X0, insn.rn);
        if (insn.vop == vop::sshll2 || insn.vop == vop::ushll2)
            mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X0, 8);
        widen(buf, e, sg);
        if (insn.imm) shift_imm(buf, X0, 2 * e, true, false, static_cast<std::uint8_t>(insn.imm));
        vstore(buf, insn.rd, X0, true);
        return true;
    }

    /* ---- across lanes ----------------------------------------------------- */
    case vop::addv: case vop::smaxv: case vop::sminv: case vop::umaxv: case vop::uminv: {
        if (e == 8) return false;
        sse op;
        switch (insn.vop) {
        case vop::addv:  op = add_op(e); break;
        case vop::smaxv: op = by_size(e, sse::pmaxsb, sse::pmaxsw, sse::pmaxsd, sse::pmaxsd); break;
        case vop::sminv: op = by_size(e, sse::pminsb, sse::pminsw, sse::pminsd, sse::pminsd); break;
        case vop::umaxv: op = by_size(e, sse::pmaxub, sse::pmaxuw, sse::pmaxud, sse::pmaxud); break;
        default:         op = by_size(e, sse::pminub, sse::pminuw, sse::pminud, sse::pminud); break;
        }
        vload(buf, X0, insn.rn);
        reduce(buf, op, e, q);
        store_scalar_lane0(buf, insn.rd, e);
        return true;
    }
    case vop::uaddlv:
        if (e == 8) return false;
        vload(buf, X0, insn.rn);
        if (!q) { vmov(buf, X1, X0); vzero(buf, X0); mm_x64_emit_sse_rr(buf, sse::punpcklqdq, X0, X0); mm_x64_emit_movq_to_gpr(buf, A, X1); mm_x64_emit_movq_from_gpr(buf, X0, A); }
        /* widen pairs then reduce as 2*e lanes */
        {
            vmov(buf, X1, X0);
            vconst(buf, X3, lane_mask(e), lane_mask(e));
            mm_x64_emit_sse_rr(buf, sse::pand, X0, X3);
            shift_imm(buf, X1, 2 * e, false, false, static_cast<std::uint8_t>(8 * e));
            mm_x64_emit_sse_rr(buf, add_op(2 * e), X0, X1);
        }
        reduce(buf, add_op(2 * e), 2 * e, true);
        store_scalar_lane0(buf, insn.rd, 2 * e);
        return true;

    /* ---- permutes --------------------------------------------------------- */
    case vop::ext:
        vload(buf, X0, insn.rn);
        vload(buf, X1, insn.rm);
        if (q) {
            mm_x64_emit_palignr(buf, X1, X0, static_cast<std::uint8_t>(insn.imm));
            vstore(buf, insn.rd, X1, true);
        } else {
            mm_x64_emit_sse_rr(buf, sse::punpcklqdq, X0, X1);
            mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X0, static_cast<std::uint8_t>(insn.imm));
            vstore(buf, insn.rd, X0, false);
        }
        return true;
    case vop::tbl1: case vop::tbl2: {
        vload(buf, X1, insn.rm);                         /* indices */
        vconst(buf, X3, byte_splat(15), byte_splat(15));
        vmov(buf, X2, X1);
        mm_x64_emit_sse_rr(buf, sse::pcmpgtb, X2, X3);   /* index > 15 (signed; >=128 already has bit 7) */
        mm_x64_emit_sse_rr(buf, sse::por, X2, X1);       /* out-of-range -> bit 7 set -> pshufb zeroes */
        vload(buf, X0, insn.rn);
        mm_x64_emit_sse_rr(buf, sse::pshufb, X0, X2);
        if (insn.vop == vop::tbl2) {
            vconst(buf, X3, byte_splat(16), byte_splat(16));
            mm_x64_emit_sse_rr(buf, sse::psubb, X1, X3);  /* indices - 16 */
            vconst(buf, X3, byte_splat(15), byte_splat(15));
            vmov(buf, X2, X1);
            mm_x64_emit_sse_rr(buf, sse::pcmpgtb, X2, X3);
            mm_x64_emit_sse_rr(buf, sse::por, X2, X1);
            vload(buf, X1, static_cast<std::uint8_t>((insn.rn + 1) & 31));
            mm_x64_emit_sse_rr(buf, sse::pshufb, X1, X2);
            mm_x64_emit_sse_rr(buf, sse::por, X0, X1);
        }
        vstore(buf, insn.rd, X0, q);
        return true;
    }
    case vop::zip1: case vop::zip2: {
        vload(buf, X0, insn.rn);
        vload(buf, X1, insn.rm);
        const bool hi = insn.vop == vop::zip2;
        if (q) {
            mm_x64_emit_sse_rr(buf, hi ? by_size(e, sse::punpckhbw, sse::punpckhwd, sse::punpckhdq, sse::punpckhqdq)
                                       : by_size(e, sse::punpcklbw, sse::punpcklwd, sse::punpckldq, sse::punpcklqdq), X0, X1);
        } else {
            mm_x64_emit_sse_rr(buf, by_size(e, sse::punpcklbw, sse::punpcklwd, sse::punpckldq, sse::punpcklqdq), X0, X1);
            if (hi) mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X0, 8);
        }
        vstore(buf, insn.rd, X0, q);
        return true;
    }
    case vop::uzp1: case vop::uzp2: case vop::trn1: case vop::trn2: {
        const bool odd = insn.vop == vop::uzp2 || insn.vop == vop::trn2;
        const bool trn = insn.vop == vop::trn1 || insn.vop == vop::trn2;
        vload(buf, X0, insn.rn);
        vload(buf, X1, insn.rm);
        if (!q) {
            mm_x64_emit_sse_rr(buf, sse::punpcklqdq, X0, X1);
            split_even_odd(buf, e);                       /* X2 evens, X3 odds of the 8-lane stream */
            if (!trn) { vstore(buf, insn.rd, odd ? X3 : X2, false); return true; }
            /* trn on the concatenated stream: interleave evens/odds of rn half with those of rm half */
            /* lanes: rn has 8/e elements; evens(rn) = X2[0 .. 4/e), evens(rm) = X2[4/e .. 8/e) */
            const int half = 4 / e; /* elements per half of the 8-byte result */
            shuffle_const(buf, X0, [=](int i) {
                if (i >= 8) return 0x80;
                const int lane = i / e, k = i % e;
                const int src = (lane % 2 == 0) ? lane / 2 : half + lane / 2;
                return src * e + k;
            });
            mm_x64_emit_sse_rr(buf, sse::pshufb, odd ? X3 : X2, X0);
            vstore(buf, insn.rd, odd ? X3 : X2, false);
            return true;
        }
        /* q: evens/odds of rn (X2/X3 low 8), then of rm */
        vmov(buf, X3, X1);
        split_even_odd(buf, e);                           /* uses X0 -> X2 evens(rn), X3 odds(rn); X1 clobbered */
        vload(buf, X0, insn.rm);
        vmov(buf, X1, X2);                                /* keep evens(rn) */
        shuffle_const(buf, X2, [=](int i) { return i < 8 ? (i / e) * 2 * e + (odd ? e : 0) + i % e : 0x80; });
        mm_x64_emit_sse_rr(buf, sse::pshufb, X0, X2);     /* X0 = selected(rm) */
        if (trn) {
            mm_x64_emit_sse_rr(buf, odd ? by_size(e, sse::punpcklbw, sse::punpcklwd, sse::punpckldq, sse::punpcklqdq)
                                        : by_size(e, sse::punpcklbw, sse::punpcklwd, sse::punpckldq, sse::punpcklqdq),
                               odd ? X3 : X1, X0);
            vstore(buf, insn.rd, odd ? X3 : X1, true);
        } else {
            mm_x64_emit_sse_rr(buf, sse::punpcklqdq, odd ? X3 : X1, X0);
            vstore(buf, insn.rd, odd ? X3 : X1, true);
        }
        return true;
    }
    case vop::udot: case vop::sdot: {
        const bool sg = insn.vop == vop::sdot;
        const sse widen_op = sg ? sse::pmovsxbw : sse::pmovzxbw;
        /* products of bytes 0-7 -> X2 (8 words), of bytes 8-15 -> X3 */
        vload(buf, X0, insn.rn);
        vload(buf, X1, insn.rm);
        mm_x64_emit_sse_rr(buf, widen_op, X2, X0);
        mm_x64_emit_sse_rr(buf, widen_op, X3, X1);
        mm_x64_emit_sse_rr(buf, sse::pmullw, X2, X3);
        mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X0, 8);
        mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X1, 8);
        mm_x64_emit_sse_rr(buf, widen_op, X0, X0);
        mm_x64_emit_sse_rr(buf, widen_op, X1, X1);
        mm_x64_emit_sse_rr(buf, sse::pmullw, X0, X1);
        vmov(buf, X3, X0);
        /* pair-sum words into dwords: X2 -> [s01 s23 s45 s67], X3 likewise */
        for (int x : {X2, X3}) {
            vmov(buf, X1, x);
            if (sg) {
                mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::pslld, x, 16);
                mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrad, x, 16);
                mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrad, X1, 16);
            } else {
                vconst(buf, X0, 0x0000FFFF0000FFFFull, 0x0000FFFF0000FFFFull);
                mm_x64_emit_sse_rr(buf, sse::pand, x, X0);
                mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrld, X1, 16);
            }
            mm_x64_emit_sse_rr(buf, sse::paddd, x, X1);
        }
        mm_x64_emit_sse_rr(buf, sse::phaddd, X2, X3);      /* [l0 l1 h0 h1] */
        vload(buf, X0, insn.rd);
        mm_x64_emit_sse_rr(buf, sse::paddd, X0, X2);
        vstore(buf, insn.rd, X0, q);
        return true;
    }
    case vop::dup_elem:
        vload(buf, X0, insn.rn);
        shuffle_const(buf, X1, [=](int i) { return insn.elem_index * e + i % e; });
        mm_x64_emit_sse_rr(buf, sse::pshufb, X0, X1);
        vstore(buf, insn.rd, X0, q);
        return true;
    case vop::ins_elem:
        mm_x64_emit_load(buf, A, CTX, voff(insn.rn, 0) + static_cast<std::int32_t>(insn.imm) * e, unsigned_kind(e));
        mm_x64_emit_store(buf, CTX, voff(insn.rd, 0) + insn.elem_index * e, A, e);
        return true;
    }
    return false;
}

/* ---- ARMv8 crypto extension -------------------------------------------- */

namespace {

/* Extract lane i of an xmm into a 32-bit GPR. */
void extract_lane32(mm_x64_buf &buf, int gpr, int xmm, int lane) {
    if (lane == 0) mm_x64_emit_movd_to_gpr(buf, gpr, xmm);
    else mm_x64_emit_pextrd(buf, gpr, xmm, static_cast<std::uint8_t>(lane));
}

/* The SHA-1 round function over the lane-rotated state vectors: result in
 * x_c (lane 0); x_b and x_d are consumed. b/c/d name the value each vector
 * holds in lane 0, not the register roles. */
void sha1_f(mm_x64_buf &buf, mm_a64_crypto op, int x_b, int x_c, int x_d) {
    switch (op) {
    case mm_a64_crypto::sha1c: /* (b & (c ^ d)) ^ d */
        mm_x64_emit_sse_rr(buf, sse::pxor, x_c, x_d);
        mm_x64_emit_sse_rr(buf, sse::pand, x_c, x_b);
        mm_x64_emit_sse_rr(buf, sse::pxor, x_c, x_d);
        break;
    case mm_a64_crypto::sha1p: /* b ^ c ^ d */
        mm_x64_emit_sse_rr(buf, sse::pxor, x_c, x_d);
        mm_x64_emit_sse_rr(buf, sse::pxor, x_c, x_b);
        break;
    default: /* sha1m: (b & c) | (b & d) | (c & d) */
        mm_x64_emit_sse_rr(buf, sse::pand, x_c, x_d); /* c & d */
        mm_x64_emit_sse_rr(buf, sse::pand, x_d, x_b); /* b & d */
        mm_x64_emit_sse_rr(buf, sse::por, x_c, x_d);
        mm_x64_emit_pshufd(buf, x_d, x_b, 0x55);      /* c broadcast (b holds c in lane 1) */
        mm_x64_emit_sse_rr(buf, sse::pand, x_d, x_b); /* b & c */
        mm_x64_emit_sse_rr(buf, sse::por, x_c, x_d);
        break;
    }
}

/* Sigma0/Sigma1 of SHA-256 in the GPRs: dst = Sigma(src), src preserved.
 * Uses T as scratch. */
/* The GPR shifts are the encoder's 64-bit forms, so a 32-bit rotate is
 * built from a left and a right shift: for a zero-extended dword the low
 * half of (x << n) | (x >> (32 - n)) is exactly the 32-bit rotate. The high
 * half stays garbage; it never reaches the result because every consumer
 * goes through adds (whose low half only depends on low halves) and
 * pextrd/pinsrd. */

/* acc = ror32(src, shr_n) = (src << shl_n) | (src >> shr_n). */
void ror32_into(mm_x64_buf &buf, int acc, int src, std::uint8_t shl_n, std::uint8_t shr_n) {
    mm_x64_emit_mov_reg(buf, acc, src);
    mm_x64_emit_shl_imm(buf, acc, shl_n);
    mm_x64_emit_mov_reg(buf, T, src);
    mm_x64_emit_shr_imm(buf, T, shr_n);
    mm_x64_emit_or_reg(buf, acc, T);
}

/* acc ^= ror32(src, shr_n); the accumulator takes a round trip through the
 * stack because the rotate needs both remaining registers. */
void xor_ror32(mm_x64_buf &buf, int acc, int src, std::uint8_t shl_n, std::uint8_t shr_n) {
    mm_x64_emit_push(buf, acc);
    mm_x64_emit_mov_reg(buf, acc, src);
    mm_x64_emit_shl_imm(buf, acc, shl_n);
    mm_x64_emit_mov_reg(buf, T, src);
    mm_x64_emit_shr_imm(buf, T, shr_n);
    mm_x64_emit_or_reg(buf, acc, T);
    mm_x64_emit_pop(buf, T);
    mm_x64_emit_xor_reg(buf, acc, T);
}

/* Sigma0/Sigma1 of SHA-256 in the GPRs: dst = Sigma(src), src preserved. */
void sha256_sigma0(mm_x64_buf &buf, int dst, int src) {
    ror32_into(buf, dst, src, 30, 2);
    xor_ror32(buf, dst, src, 19, 13);
    xor_ror32(buf, dst, src, 10, 22);
}
void sha256_sigma1(mm_x64_buf &buf, int dst, int src) {
    ror32_into(buf, dst, src, 26, 6);
    xor_ror32(buf, dst, src, 21, 11);
    xor_ror32(buf, dst, src, 7, 25);
}

/* The schedule's small sigma1 (the hash rounds use the big Sigma1 above). */
void sha256_small_sigma1(mm_x64_buf &buf, int dst, int src) {
    ror32_into(buf, dst, src, 15, 17);
    xor_ror32(buf, dst, src, 13, 19);
    mm_x64_emit_mov_reg(buf, T, src);
    mm_x64_emit_shr_imm(buf, T, 10);
    mm_x64_emit_xor_reg(buf, dst, T);
}
} // namespace

bool emit_crypto_op(mm_x64_buf &buf, const mm_a64_insn &insn) {
    switch (insn.crypto) {
    case mm_a64_crypto::sha1h:
        /* Sd = ror(Sn<31:0>, 2); movd zero-extends, so the upper lanes of
         * the destination are zeroed the way a scalar write does. */
        vload(buf, X0, insn.rn);
        mm_x64_emit_movd_to_gpr(buf, A, X0);
        mm_x64_emit_mov_reg(buf, T, A);
        mm_x64_emit_shl_imm(buf, A, 30);
        mm_x64_emit_shr_imm(buf, T, 2);
        mm_x64_emit_or_reg(buf, A, T); /* ror 2 */
        mm_x64_emit_movd_from_gpr(buf, X0, A);
        vstore(buf, insn.rd, X0, true);
        return true;

    case mm_a64_crypto::sha1su1: {
        /* T = Vd ^ (Vn >> 32); lanes 0..2 = rol(T, 1), lane 3 ^= rol(T0, 2). */
        vload(buf, X0, insn.rd);
        vload(buf, X1, insn.rn);
        mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X1, 4); /* (w1, w2, w3, 0) */
        mm_x64_emit_sse_rr(buf, sse::pxor, X1, X0);                   /* T */
        mm_x64_emit_pshufd(buf, X2, X1, 0x00);                        /* T0 broadcast, for lane 3 */
        vmov(buf, X3, X1);                                            /* per-lane copy for the wrap bits */
        mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::pslld, X1, 1);
        mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrld, X3, 31);
        mm_x64_emit_sse_rr(buf, sse::por, X1, X3);                    /* rol(T, 1) */
        /* rol2(T0) = (T0 << 2) | (T0 >> 30): X0 is free since the pxor. */
        vmov(buf, X0, X2);
        mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::pslld, X2, 2);  /* T0 << 2 */
        mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrld, X0, 30); /* T0 >> 30 */
        mm_x64_emit_sse_rr(buf, sse::por, X2, X0);                    /* rol2(T0) */
        vconst(buf, X3, 0, 0xFFFFFFFF00000000ull);                    /* lane 3 mask */
        mm_x64_emit_sse_rr(buf, sse::pand, X2, X3);
        mm_x64_emit_sse_rr(buf, sse::pxor, X1, X2);
        vstore(buf, insn.rd, X1, true);
        return true;
    }

    case mm_a64_crypto::sha256su0: {
        /* T = (d1, d2, d3, Vn0); lane i = sigma0(T[i]) + Vd[i]. x86 palignr
         * takes its SOURCE as the high half of the concatenation, so the
         * destination register is the one holding Vn. */
        vload(buf, X0, insn.rd);
        vload(buf, X1, insn.rn);
        vmov(buf, X2, X0);
        mm_x64_emit_palignr(buf, X1, X2, 4); /* (Vd : Vn) >> 4 bytes */
        vmov(buf, X2, X1);
        mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::pslld, X2, 14);
        vmov(buf, X3, X1);
        mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrld, X3, 18);
        mm_x64_emit_sse_rr(buf, sse::por, X2, X3); /* ror 18 */
        vmov(buf, X3, X1);
        mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrld, X3, 3);
        mm_x64_emit_sse_rr(buf, sse::pxor, X2, X3); /* ^ (>> 3) */
        vmov(buf, X3, X1);
        mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrld, X3, 7);
        mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::pslld, X1, 25);
        mm_x64_emit_sse_rr(buf, sse::por, X3, X1); /* ror 7 */
        mm_x64_emit_sse_rr(buf, sse::pxor, X2, X3); /* sigma0 */
        mm_x64_emit_sse_rr(buf, sse::paddd, X2, X0);
        vstore(buf, insn.rd, X2, true);
        return true;
    }

    case mm_a64_crypto::aese:
    case mm_a64_crypto::aesd: {
        /* AESE = SubBytes(ShiftRows(Vd ^ Vn)): x86 aesenc is
         * MixColumns(SubBytes(ShiftRows())) ^ src, and InvMixColumns
         * (aesimc) undoes its MixColumns; aesdec mirrors this for AESD
         * with three aesimc (MixColumns = InvMixColumns^3). */
        vload(buf, X0, insn.rd);
        vload(buf, X1, insn.rn);
        mm_x64_emit_sse_rr(buf, sse::pxor, X0, X1);
        vzero(buf, X2);
        if (insn.crypto == mm_a64_crypto::aese) {
            mm_x64_emit_aesenc(buf, X0, X2);
            mm_x64_emit_aesimc(buf, X0, X0);
        } else {
            mm_x64_emit_aesdec(buf, X0, X2);
            mm_x64_emit_aesimc(buf, X0, X0);
            mm_x64_emit_aesimc(buf, X0, X0);
            mm_x64_emit_aesimc(buf, X0, X0);
        }
        vstore(buf, insn.rd, X0, true);
        return true;
    }

    case mm_a64_crypto::aesmc: /* MixColumns = InvMixColumns^3 */
    case mm_a64_crypto::aesimc: {
        vload(buf, X0, insn.rn);
        mm_x64_emit_aesimc(buf, X0, X0);
        if (insn.crypto == mm_a64_crypto::aesmc) {
            mm_x64_emit_aesimc(buf, X0, X0);
            mm_x64_emit_aesimc(buf, X0, X0);
        }
        vstore(buf, insn.rd, X0, true);
        return true;
    }

    case mm_a64_crypto::sha1su0: {
        /* result = (Vn.lo :: Vd.hi) ^ Vd ^ Vm. */
        vload(buf, X0, insn.rd);
        vload(buf, X2, insn.rm);
        vload(buf, X1, insn.rn);
        mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::pslldq, X1, 8); /* (0, Vn.lo) */
        vmov(buf, X3, X0);
        mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrldq, X3, 8); /* (Vd.hi, 0) */
        mm_x64_emit_sse_rr(buf, sse::por, X3, X1);                    /* (Vn.lo :: Vd.hi) */
        mm_x64_emit_sse_rr(buf, sse::pxor, X3, X0);
        mm_x64_emit_sse_rr(buf, sse::pxor, X3, X2);
        vstore(buf, insn.rd, X3, true);
        return true;
    }

    case mm_a64_crypto::sha1c:
    case mm_a64_crypto::sha1p:
    case mm_a64_crypto::sha1m: {
        /* Four SHA-1 rounds: t = f(b,c,d) + rol(a,5) + e + w[i];
         * (a,b,c,d,e) <- (t, a, rol30(b), c, d). Vn is read-only: the caller
         * keeps e there and rotates it with sha1h between round groups. */
        vload(buf, X0, insn.rd); /* (a, b, c, d) */
        vload(buf, X1, insn.rn);
        mm_x64_emit_pextrd(buf, T, X1, 0); /* e */
        for (int i = 0; i < 4; i++) {
            vload(buf, X3, insn.rm); /* w, reloaded each round: the scratch
                                      * registers churn inside the round body */
            mm_x64_emit_pextrd(buf, B, X3, static_cast<std::uint8_t>(i)); /* w[i] */
            mm_x64_emit_pshufd(buf, X1, X0, 0x39); /* (b, c, d, a) */
            mm_x64_emit_pshufd(buf, X3, X0, 0x4E); /* (c, d, a, b) */
            mm_x64_emit_pshufd(buf, X2, X0, 0x93); /* (d, a, b, c) */
            sha1_f(buf, insn.crypto, X1 /* b */, X3 /* c */, X2 /* d */); /* f in X3 */
            /* rol5(a) into X2 */
            mm_x64_emit_pshufd(buf, X2, X0, 0x00);
            mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::pslld, X2, 5);
            mm_x64_emit_pshufd(buf, X1, X0, 0x00);
            mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrld, X1, 27);
            mm_x64_emit_sse_rr(buf, sse::por, X2, X1);
            mm_x64_emit_sse_rr(buf, sse::paddd, X3, X2); /* f + rol5(a) */
            mm_x64_emit_movd_from_gpr(buf, X1, B);       /* w[i], upper lanes zero */
            mm_x64_emit_sse_rr(buf, sse::paddd, X3, X1);
            mm_x64_emit_movd_to_gpr(buf, A, X3);
            mm_x64_emit_add_reg(buf, A, T);    /* a_new = f + rol5(a) + w[i] + e */
            mm_x64_emit_pextrd(buf, T, X0, 3); /* e_new = d_old */
            /* rol30 of every lane into X1 */
            vmov(buf, X1, X0);
            mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::pslld, X1, 30);
            vmov(buf, X2, X0);
            mm_x64_emit_sse_shift_imm(buf, mm_x64_vshift::psrld, X2, 2);
            mm_x64_emit_sse_rr(buf, sse::por, X1, X2);
            /* (a_new, a_old, rol30(b), c_old) into X2 */
            mm_x64_emit_pextrd(buf, B, X0, 0); /* a_old */
            mm_x64_emit_movd_from_gpr(buf, X2, A);
            mm_x64_emit_pinsrd(buf, X2, B, 1);
            mm_x64_emit_pextrd(buf, B, X1, 1); /* rol30(b) */
            mm_x64_emit_pinsrd(buf, X2, B, 2);
            mm_x64_emit_pextrd(buf, B, X0, 2); /* c_old */
            mm_x64_emit_pinsrd(buf, X2, B, 3);
            vmov(buf, X0, X2);
        }
        vstore(buf, insn.rd, X0, true);
        return true;
    }

    case mm_a64_crypto::sha256h:
    case mm_a64_crypto::sha256h2: {
        /* Four SHA-256 rounds. For h: Vd = (a,b,c,d) and Vn = (e,f,g,h) are
         * both chains, and the result is the working (a,b,c,d). For h2:
         * Vd = (e,f,g,h) is the only chain, Vn is a read-only copy of the
         * (a,b,c,d) state whose lane 3-i is round i's "d" input, and the
         * result is the working (e,f,g,h). The caller adds the working
         * values to the saved state once per block. */
        const bool part1 = insn.crypto == mm_a64_crypto::sha256h;
        vload(buf, X0, insn.rd); /* h: abcd / h2: efgh — the chain that updates */
        vload(buf, X2, insn.rn); /* h: efgh / h2: abcd state copy */
        vload(buf, X1, insn.rm); /* w */
        for (int i = 0; i < 4; i++) {
            /* T1 = cho(e,f,g) + h + sigma1(e) + w[i] over the state the
             * cho reads: X2 for h, X0 for h2. */
            const int x = part1 ? X2 : X0;
            extract_lane32(buf, A, x, 0); /* e */
            sha256_sigma1(buf, B, A);
            mm_x64_emit_push(buf, B);
            extract_lane32(buf, B, x, 1); /* f */
            extract_lane32(buf, T, x, 2); /* g */
            mm_x64_emit_xor_reg(buf, B, T);
            mm_x64_emit_and_reg(buf, B, A);
            mm_x64_emit_xor_reg(buf, B, T); /* cho */
            mm_x64_emit_pop(buf, T);
            mm_x64_emit_add_reg(buf, B, T); /* cho + sigma1 */
            extract_lane32(buf, T, x, 3);   /* h */
            mm_x64_emit_add_reg(buf, B, T);
            extract_lane32(buf, T, X1, i); /* w[i] */
            mm_x64_emit_add_reg(buf, B, T); /* B = T1 */
            if (part1)
                mm_x64_emit_push(buf, B); /* T1 survives the T2 computation below;
                                           * h2 never reaches it and must not leak a push */
            /* The (e,f,g,h) chain's new lane 0: for h it is d_state + T1
             * (read before the abcd chain moves); for h2 the state copy's
             * lane 3-i is round i's d input. */
            if (part1)
                extract_lane32(buf, T, X0, 3);
            else
                extract_lane32(buf, T, X2, 3 - i);
            mm_x64_emit_add_reg(buf, T, B);
            mm_x64_emit_push(buf, T);
            if (part1) {
                mm_x64_emit_pshufd(buf, X2, X2, 0x93); /* (h, e, f, g) */
                mm_x64_emit_pop(buf, T);
                mm_x64_emit_pinsrd(buf, X2, T, 0); /* (e_new, e, f, g) */
            } else {
                mm_x64_emit_pshufd(buf, X0, X0, 0x93);
                mm_x64_emit_pop(buf, T);
                mm_x64_emit_pinsrd(buf, X0, T, 0);
                continue;
            }
            /* h only: T2 = sigma0(a) + maj(a,b,c) over the abcd chain, then
             * a_new = T1 + T2. */
            extract_lane32(buf, A, X0, 0); /* a */
            sha256_sigma0(buf, B, A);
            mm_x64_emit_push(buf, B);
            extract_lane32(buf, B, X0, 1); /* b */
            extract_lane32(buf, T, X0, 2); /* c */
            mm_x64_emit_push(buf, T);
            mm_x64_emit_mov_reg(buf, T, A);
            mm_x64_emit_and_reg(buf, T, B); /* a & b */
            mm_x64_emit_push(buf, T);
            mm_x64_emit_mov_reg(buf, T, A);
            mm_x64_emit_or_reg(buf, T, B); /* a | b */
            mm_x64_emit_push(buf, T);
            mm_x64_emit_pop(buf, B);       /* a | b */
            mm_x64_emit_pop(buf, T);       /* a & b */
            mm_x64_emit_pop(buf, A);       /* c */
            mm_x64_emit_and_reg(buf, B, A); /* (a|b) & c */
            mm_x64_emit_or_reg(buf, B, T); /* maj */
            mm_x64_emit_pop(buf, T);       /* sigma0 */
            mm_x64_emit_add_reg(buf, B, T); /* B = T2 */
            mm_x64_emit_pop(buf, T);       /* T1, pushed before the chain update */
            mm_x64_emit_add_reg(buf, B, T); /* a_new = T1 + T2 */
            mm_x64_emit_pshufd(buf, X0, X0, 0x93); /* (d, a, b, c) */
            mm_x64_emit_pinsrd(buf, X0, B, 0); /* (a_new, a, b, c) */
        }
        vstore(buf, insn.rd, X0, true);
        return true;
    }

    case mm_a64_crypto::sha256su1: {
        /* lane 0 += sigma1(w2) + n1; lane 1 += sigma1(w3) + n2;
         * lane 2 += sigma1(d0_new) + n3; lane 3 += sigma1(d1_new) + w0. */
        vload(buf, X0, insn.rd); /* d */
        vload(buf, X1, insn.rn); /* n */
        vload(buf, X2, insn.rm); /* w */
        for (int lane = 0; lane < 4; lane++) {
            const int sigma_src_reg = lane == 0 ? X2 : lane == 1 ? X2 : X0;
            const int sigma_src_lane = lane == 0 ? 2 : lane == 1 ? 3 : lane - 2;
            const int add1_reg = lane < 2 ? X1 : lane == 2 ? X1 : X2;
            const int add1_lane = lane == 0 ? 1 : lane == 1 ? 2 : lane == 2 ? 3 : 0;
            extract_lane32(buf, A, sigma_src_reg, sigma_src_lane);
            sha256_small_sigma1(buf, B, A);
            extract_lane32(buf, T, add1_reg, add1_lane);
            mm_x64_emit_add_reg(buf, B, T);
            extract_lane32(buf, T, X0, lane); /* original d lane */
            mm_x64_emit_add_reg(buf, B, T);
            mm_x64_emit_pinsrd(buf, X0, B, static_cast<std::uint8_t>(lane));
        }
        vstore(buf, insn.rd, X0, true);
        return true;
    }
    }
    return false;
}

} // namespace mm_emit

#include "machoman/arch/arm64_decoder.h"
#include "simd_helpers.h"

#include <cstring>

namespace {

std::uint32_t bits(std::uint32_t v, int hi, int lo) {
    const std::uint32_t mask = (hi - lo == 31) ? 0xFFFFFFFFu : ((1u << (hi - lo + 1)) - 1u);
    return (v >> lo) & mask;
}

bool bit(std::uint32_t v, int i) { return (v >> i) & 1u; }

std::int64_t sign_extend(std::uint32_t v, int bit_count) {
    const std::uint32_t sign_bit = 1u << (bit_count - 1);
    return static_cast<std::int64_t>(static_cast<std::int32_t>((v ^ sign_bit) - sign_bit));
}

/* DecodeBitMasks(N, imms, immr) from the ARM ARM: the logical-immediate
 * encoding of a rotated, replicated run of ones. Returns false for the
 * reserved encodings. */
bool decode_bit_masks(bool n, std::uint32_t imms, std::uint32_t immr, bool is64, std::uint64_t &wmask) {
    const std::uint32_t pattern = (static_cast<std::uint32_t>(n) << 6) | (~imms & 0x3Fu);
    if (pattern == 0)
        return false;
    int len = 0;
    while ((pattern >> (len + 1)) != 0)
        len++;
    if (len < 1 || (!is64 && len > 5))
        return false;
    const std::uint32_t esize = 1u << len;
    const std::uint32_t levels = esize - 1;
    const std::uint32_t s = imms & levels;
    const std::uint32_t r = immr & levels;
    if (s == levels)
        return false;
    std::uint64_t welem = (s + 1 == 64) ? ~0ull : ((1ull << (s + 1)) - 1);
    /* ROR within esize */
    if (r != 0) {
        const std::uint64_t emask = (esize == 64) ? ~0ull : ((1ull << esize) - 1);
        welem = ((welem >> r) | (welem << (esize - r))) & emask;
    }
    wmask = 0;
    for (std::uint32_t i = 0; i < 64; i += esize)
        wmask |= welem << i;
    if (!is64)
        wmask &= 0xFFFFFFFFull;
    return true;
}

/* Load/store register forms share the size/opc -> (mem_size, signed,
 * is64, load?) mapping. Returns false for unallocated combinations.
 * PRFM (size=11 opc=10) is reported as a nop. */
bool ldst_kind(std::uint32_t size, std::uint32_t opc, mm_a64_insn &out, bool &is_load, bool &is_prfm) {
    is_prfm = false;
    out.mem_size = static_cast<std::uint8_t>(1u << size);
    switch (opc) {
    case 0: is_load = false; out.mem_signed = false; out.is64 = true; return true;
    case 1: is_load = true;  out.mem_signed = false; out.is64 = true; return true;
    case 2:
        if (size == 3) { is_prfm = true; return true; }
        is_load = true; out.mem_signed = true; out.is64 = true; return true;
    case 3:
        if (size >= 2) return false;
        is_load = true; out.mem_signed = true; out.is64 = false; return true;
    }
    return false;
}

/* AdvSIMD load/store structures (group 0, bits 29..28 = 00 with V=1):
 *   multiple: 0 Q 001100 P L 0 Rm opcode size Rn Rt
 *   single:   0 Q 001101 P L R Rm opcode S size Rn Rt
 * P = post-index (Rm = 31 -> immediate = bytes transferred). */
void decode_ldst_struct(std::uint32_t insn, mm_a64_insn &out) {
    const bool q = bit(insn, 30), post = bit(insn, 23), load = bit(insn, 22), single = bit(insn, 24);
    const std::uint32_t rm = bits(insn, 20, 16), size = bits(insn, 11, 10);
    out.simd = true;
    out.q = q;
    out.is64 = true;
    out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
    out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
    if (!post && rm != 0) return;
    std::uint32_t bytes = 0;

    if (!single) {
        const std::uint32_t opcode = bits(insn, 15, 12);
        std::uint32_t nregs;
        bool interleaved = false;
        switch (opcode) {
        case 0x7: nregs = 1; break;
        case 0xA: nregs = 2; break;
        case 0x6: nregs = 3; break;
        case 0x2: nregs = 4; break;
        case 0x8: nregs = 2; interleaved = true; break; /* ld2/st2 */
        case 0x4: nregs = 3; interleaved = true; break; /* ld3/st3 */
        case 0x0: nregs = 4; interleaved = true; break; /* ld4/st4 */
        default: return;
        }
        /* The 64-bit form of an interleaved store needs at least two
         * elements per register, so `.1d` (size 3, Q 0) is unallocated. */
        if (interleaved && size == 3 && !q) return;
        if (interleaved) out.op = load ? mm_a64_opcode::ldn_multi : mm_a64_opcode::stn_multi;
        else out.op = load ? mm_a64_opcode::ld1_multi : mm_a64_opcode::st1_multi;
        out.rt2 = static_cast<std::uint8_t>(nregs);
        out.elem_size = static_cast<std::uint8_t>(1u << size);
        bytes = nregs * (q ? 16 : 8);
    } else {
        const bool r = bit(insn, 21);
        const std::uint32_t opcode = bits(insn, 15, 13), s_bit = bits(insn, 12, 12);
        /* opcode<0> picks the pair {1,2} or {3,4}, R picks within it. */
        const std::uint32_t nregs = ((opcode & 1) ? 3u : 1u) + (r ? 1u : 0u);
        out.rt2 = static_cast<std::uint8_t>(nregs);
        if (opcode >= 6) { /* ld1r/ld2r/ld3r/ld4r */
            if (!load || s_bit) return;
            out.op = nregs == 1 ? mm_a64_opcode::ld1r : mm_a64_opcode::ldnr;
            out.elem_size = static_cast<std::uint8_t>(1u << size);
            bytes = out.elem_size * nregs;
        } else {
            std::uint32_t esize, index;
            switch (opcode & ~1u) {
            case 0: esize = 1; index = (static_cast<std::uint32_t>(q) << 3) | (s_bit << 2) | size; break;
            case 2: if (size & 1) return; esize = 2; index = (static_cast<std::uint32_t>(q) << 2) | (s_bit << 1) | (size >> 1); break;
            case 4:
                if (size == 0) { esize = 4; index = (static_cast<std::uint32_t>(q) << 1) | s_bit; }
                else if (size == 1 && !s_bit) { esize = 8; index = q ? 1u : 0u; }
                else return;
                break;
            default: return;
            }
            if (nregs == 1) out.op = load ? mm_a64_opcode::ld1_lane : mm_a64_opcode::st1_lane;
            else out.op = load ? mm_a64_opcode::ldn_lane : mm_a64_opcode::stn_lane;
            out.elem_size = static_cast<std::uint8_t>(esize);
            out.elem_index = static_cast<std::uint8_t>(index);
            bytes = esize * nregs;
        }
    }
    if (!post) {
        out.addr = mm_a64_addr::offset;
    } else if (rm == 31) {
        out.addr = mm_a64_addr::post_index;
        out.imm = bytes;
    } else {
        out.addr = mm_a64_addr::post_index_reg;
        out.rm = static_cast<std::uint8_t>(rm);
    }
}

/* SIMD&FP load/store (V=1): same addressing forms as the integer ones,
 * with the access width taken from size:opc(bit 23). */
void decode_ldst_simd(std::uint32_t insn, std::uint64_t pc, mm_a64_insn &out) {
    const std::uint32_t size = bits(insn, 31, 30);
    const std::uint32_t group = bits(insn, 29, 28);
    out.simd = true;
    out.is64 = true;
    out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
    out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));

    if (group == 1 && bits(insn, 25, 24) == 0) { /* literal: opc 011 1 00 imm19 Rt */
        if (size == 3) return;
        out.mem_size = static_cast<std::uint8_t>(4u << size);
        out.op = mm_a64_opcode::ldr;
        out.addr = mm_a64_addr::literal;
        out.imm = static_cast<std::int64_t>(pc + static_cast<std::uint64_t>(sign_extend(bits(insn, 23, 5), 19) * 4));
        return;
    }
    if (group == 2) { /* pair: opc 101 1 0 mode L imm7 Rt2 Rn Rt */
        if (size == 3) return;
        const std::uint32_t mode = bits(insn, 24, 23);
        out.mem_size = static_cast<std::uint8_t>(4u << size);
        out.op = bit(insn, 22) ? mm_a64_opcode::ldp : mm_a64_opcode::stp;
        out.rt2 = static_cast<std::uint8_t>(bits(insn, 14, 10));
        out.imm = sign_extend(bits(insn, 21, 15), 7) * out.mem_size;
        out.addr = mode == 1 ? mm_a64_addr::post_index : mode == 3 ? mm_a64_addr::pre_index : mm_a64_addr::offset;
        return;
    }
    if (group == 0) {
        decode_ldst_struct(insn, out);
        return;
    }
    if (group != 3)
        return;

    const std::uint32_t opc = bits(insn, 23, 22);
    const bool load = opc & 1;
    if (opc & 2) {
        if (size != 0) return;
        out.mem_size = 16;
    } else {
        out.mem_size = static_cast<std::uint8_t>(1u << size);
    }

    if (bit(insn, 24)) {
        out.addr = mm_a64_addr::offset;
        out.imm = static_cast<std::int64_t>(bits(insn, 21, 10)) * out.mem_size;
    } else if (!bit(insn, 21)) {
        out.imm = sign_extend(bits(insn, 20, 12), 9);
        switch (bits(insn, 11, 10)) {
        case 0: out.addr = mm_a64_addr::offset; break;
        case 1: out.addr = mm_a64_addr::post_index; break;
        case 3: out.addr = mm_a64_addr::pre_index; break;
        default: return;
        }
    } else if (bits(insn, 11, 10) == 2) {
        const std::uint32_t option = bits(insn, 15, 13);
        if (!(option & 2)) return;
        out.addr = mm_a64_addr::reg_offset;
        out.rm = static_cast<std::uint8_t>(bits(insn, 20, 16));
        out.extend = static_cast<mm_a64_extend>(option);
        out.ext_shift = bit(insn, 12) ? static_cast<std::uint8_t>(out.mem_size == 16 ? 4 : size) : 0;
    } else {
        return;
    }
    out.op = load ? mm_a64_opcode::ldr : mm_a64_opcode::str;
}

/* VFPExpandImm: 8-bit FP immediate -> IEEE bits of the given width. */
std::uint64_t vfp_expand_imm(std::uint32_t imm8, int width) {
    const std::uint64_t sign = (imm8 >> 7) & 1, b6 = (imm8 >> 6) & 1, frac = imm8 & 0x3F;
    const std::uint64_t exp_lsb3 = (imm8 >> 4) & 3; /* bits 5:4 */
    if (width == 8) {
        /* exp = NOT(b6):Replicate(b6,8):imm8<5:4>, 11 bits; frac = imm8<3:0>:Zeros(48) */
        const std::uint64_t exp = ((b6 ^ 1) << 10) | (b6 ? 0x3FCu : 0u) | exp_lsb3;
        return (sign << 63) | (exp << 52) | ((frac & 0xF) << 48);
    }
    if (width == 2) {
        const std::uint64_t exp = ((b6 ^ 1) << 4) | (b6 ? 0xcu : 0u) | exp_lsb3;
        return (sign << 15) | (exp << 10) | ((frac & 0xF) << 6);
    }
    const std::uint64_t exp = ((b6 ^ 1) << 7) | (b6 ? 0x7Cu : 0u) | exp_lsb3; /* NOT(b6):Rep(b6,5):imm8<5:4> */
    return (sign << 31) | (exp << 23) | ((frac & 0xF) << 19);
}

/* Scalar floating-point data processing: bit 28 = 1, bit 30 = 0, bits
 * 27..24 = 1110 (or 1111 for 3-source). type: 00 single, 01 double. */
void decode_fp_scalar(std::uint32_t insn, mm_a64_insn &out) {
    const std::uint32_t type = bits(insn, 23, 22);
    if (bit(insn, 31) && bits(insn, 28, 21) != 0xF1u) {
        /* sf=1 is only valid for int<->fp conversions (handled below) */
    }
    if (type != 0 && type != 1 && type != 3)
        return; /* half precision, etc. */
    out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
    out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
    out.rm = static_cast<std::uint8_t>(bits(insn, 20, 16));
    out.elem_size = type == 0 ? 4 : type == 1 ? 8 : 2;
    auto set = [&](mm_a64_fop f) { out.op = mm_a64_opcode::fop; out.fop = f; };

    if (bits(insn, 28, 24) == 0x1Fu) { /* 3-source: M 0 S 11111 type o1 Rm o0 Ra Rn Rd */
        if (bit(insn, 31) || bit(insn, 29)) return;
        out.ra = static_cast<std::uint8_t>(bits(insn, 14, 10));
        const std::uint32_t k = (bits(insn, 21, 21) << 1) | bits(insn, 15, 15);
        set(k == 0 ? mm_a64_fop::fmadd : k == 1 ? mm_a64_fop::fmsub : k == 2 ? mm_a64_fop::fnmadd : mm_a64_fop::fnmsub);
        return;
    }
    if (bits(insn, 28, 24) != 0x1Eu || bit(insn, 29))
        return;

    if (!bit(insn, 21)) {
        /* Fixed-point conversions: sf 0 0 11110 type 0 rmode opcode scale Rn Rd.
         * `scale` encodes 64 - fbits; fbits is never 0, so a non-zero imm
         * is what marks these apart from the integer forms below. */
        const std::uint32_t rmode = bits(insn, 20, 19), opcode = bits(insn, 18, 16), scale = bits(insn, 15, 10);
        out.is64 = bit(insn, 31);
        if (!out.is64 && scale < 32) return; /* unallocated for 32-bit sources */
        out.imm = static_cast<std::int64_t>(64 - scale);
        if (rmode == 0 && opcode == 2) set(mm_a64_fop::scvtf);
        else if (rmode == 0 && opcode == 3) set(mm_a64_fop::ucvtf);
        else if (rmode == 3 && opcode == 0) set(mm_a64_fop::fcvtzs);
        else if (rmode == 3 && opcode == 1) set(mm_a64_fop::fcvtzu);
        return;
    }
    const std::uint32_t low = bits(insn, 15, 10);
    if (low == 0) { /* conversion between int and fp: sf 0 0 11110 type 1 rmode opcode 000000 */
        const std::uint32_t rmode = bits(insn, 20, 19), opcode = bits(insn, 18, 16);
        out.is64 = bit(insn, 31);
        switch ((rmode << 3) | opcode) {
        case 0x00: set(mm_a64_fop::fcvtns); break;
        case 0x01: set(mm_a64_fop::fcvtnu); break;
        case 0x02: set(mm_a64_fop::scvtf); break;
        case 0x03: set(mm_a64_fop::ucvtf); break;
        case 0x04: set(mm_a64_fop::fcvtas); break;
        case 0x05: set(mm_a64_fop::fcvtau); break;
        case 0x08: set(mm_a64_fop::fcvtps); break;
        case 0x09: set(mm_a64_fop::fcvtpu); break;
        case 0x10: set(mm_a64_fop::fcvtms); break;
        case 0x11: set(mm_a64_fop::fcvtmu); break;
        case 0x18: set(mm_a64_fop::fcvtzs); break;
        case 0x19: set(mm_a64_fop::fcvtzu); break;
        default: break; /* fmov general forms are decoded elsewhere */
        }
        return;
    }
    if (bit(insn, 31))
        return;
    if (bits(insn, 14, 10) == 0x10u) { /* 1-source: opcode bits 20..15 */
        switch (bits(insn, 20, 15)) {
        case 0x00: set(mm_a64_fop::fmov); return;
        case 0x01: set(mm_a64_fop::fabs); return;
        case 0x02: set(mm_a64_fop::fneg); return;
        case 0x03: set(mm_a64_fop::fsqrt); return;
        case 0x04: if (type != 0) { set(mm_a64_fop::fcvt); out.imm = 4; } return;
        case 0x05: if (type != 1) { set(mm_a64_fop::fcvt); out.imm = 8; } return;
        case 0x07: if (type != 3) { set(mm_a64_fop::fcvt); out.imm = 2; } return;
        case 0x08: set(mm_a64_fop::frintn); return;
        case 0x09: set(mm_a64_fop::frintp); return;
        case 0x0A: set(mm_a64_fop::frintm); return;
        case 0x0B: set(mm_a64_fop::frintz); return;
        case 0x0C: set(mm_a64_fop::frinta); return;
        case 0x0E: set(mm_a64_fop::frintx); return;
        case 0x0F: set(mm_a64_fop::frinti); return;
        default: return;
        }
    }
    if (bits(insn, 11, 10) == 2) { /* 2-source: opcode bits 15..12 */
        switch (bits(insn, 15, 12)) {
        case 0x0: set(mm_a64_fop::fmul); return;
        case 0x1: set(mm_a64_fop::fdiv); return;
        case 0x2: set(mm_a64_fop::fadd); return;
        case 0x3: set(mm_a64_fop::fsub); return;
        case 0x4: set(mm_a64_fop::fmax); return;
        case 0x5: set(mm_a64_fop::fmin); return;
        case 0x6: set(mm_a64_fop::fmaxnm); return;
        case 0x7: set(mm_a64_fop::fminnm); return;
        case 0x8: set(mm_a64_fop::fnmul); return;
        default: return;
        }
    }
    if (bits(insn, 13, 10) == 0x8u) { /* compare: op(2) 1000 Rn opcode2 */
        const std::uint32_t opcode2 = bits(insn, 4, 0);
        if (bits(insn, 15, 14) != 0) return;
        if (opcode2 == 0x00 || opcode2 == 0x10) set(mm_a64_fop::fcmp);
        else if (opcode2 == 0x08 || opcode2 == 0x18) set(mm_a64_fop::fcmp0);
        return;
    }
    if (bits(insn, 11, 10) == 1) { /* conditional compare: Rm cond 01 Rn op nzcv */
        set(mm_a64_fop::fccmp);
        out.cond = static_cast<std::uint8_t>(bits(insn, 15, 12));
        out.nzcv = static_cast<std::uint8_t>(bits(insn, 3, 0));
        return;
    }
    if (bits(insn, 11, 10) == 3) { /* conditional select */
        set(mm_a64_fop::fcsel);
        out.cond = static_cast<std::uint8_t>(bits(insn, 15, 12));
        return;
    }
    if (bits(insn, 12, 10) == 4 && bits(insn, 9, 5) == 0) { /* immediate: imm8 100 00000 Rd */
        set(mm_a64_fop::fmov_imm);
        out.imm = static_cast<std::int64_t>(vfp_expand_imm(bits(insn, 20, 13), out.elem_size));
        return;
    }
}

/* Vector floating point: three-same (opcode >= 11000), two-reg misc and
 * by-element forms, single/double only (sz = bit 22, bit 23 = extra op bit). */
void decode_fp_vector(std::uint32_t insn, mm_a64_insn &out, bool scalar = false) {
    const bool q = scalar ? false : bit(insn, 30), u = bit(insn, 29), a = bit(insn, 23), sz = bit(insn, 22);
    out.q = q;
    out.fvec = !scalar;
    out.simd_scalar = scalar;
    out.elem_size = sz ? 8 : 4;
    out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
    out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
    out.rm = static_cast<std::uint8_t>(bits(insn, 20, 16));
    auto set = [&](mm_a64_fop f) { out.op = mm_a64_opcode::fop; out.fop = f; };
    const std::uint32_t grp = bits(insn, 27, 24); /* 1110 (0xE) three-same/misc, 1111 (0xF) by element */
    const bool two_reg_misc = grp == 0xEu && bit(insn, 21) && bits(insn, 20, 17) == 0 && bits(insn, 11, 10) == 2;
    const bool cvt_lh = two_reg_misc && !u && !a && (bits(insn, 16, 12) == 0x16 || bits(insn, 16, 12) == 0x17);
    if (!scalar && sz && !q && !cvt_lh) return; /* .1d vectors are unallocated */
    if (scalar && cvt_lh) return;

    if (scalar && grp == 0xEu && bit(insn, 21) && bits(insn, 20, 17) == 0x8 && bits(insn, 11, 10) == 2) {
        /* scalar pairwise: faddp/fmaxp/fminp/fmaxnmp/fminnmp d, v.2d */
        if (!u) return;
        switch ((a ? 32u : 0u) | bits(insn, 16, 12)) {
        case 0x0D: set(mm_a64_fop::faddp); break;
        case 0x0C: case 0x0F: set(mm_a64_fop::fmaxp); break;
        case 0x2C: case 0x2F: set(mm_a64_fop::fminp); break;
        default: return;
        }
        out.rm = out.rn; /* pairwise over rn's own lanes */
        out.fvec = true;
        out.q = sz;      /* 2s (8 bytes) or 2d (16 bytes) source; the store below narrows to a scalar */
        out.imm = 1;     /* marker: scalar result */
        return;
    }

    if (grp == 0xFu && !bit(insn, 10)) { /* by element: size L M Rm opcode H 0 */
        if (!bit(insn, 23)) return; /* integer by-element or fp16 */
        const std::uint32_t opcode = bits(insn, 15, 12);
        const std::uint32_t l = bits(insn, 21, 21), h = bits(insn, 11, 11);
        if (sz) { if (l) return; out.elem_index = static_cast<std::uint8_t>(h); }
        else { out.elem_index = static_cast<std::uint8_t>((h << 1) | l); }
        out.rm = static_cast<std::uint8_t>(bits(insn, 20, 16)); /* M bit is part of Rm for FP */
        if (!u && opcode == 0x1) set(mm_a64_fop::fmla_elem);
        else if (!u && opcode == 0x5) set(mm_a64_fop::fmls_elem);
        else if (!u && opcode == 0x9) set(mm_a64_fop::fmul_elem);
        return;
    }
    if (grp != 0xEu)
        return;
    if (bit(insn, 21) && bit(insn, 10)) { /* three-same, opcode bits 15..11 >= 0x18 */
        const std::uint32_t opcode = bits(insn, 15, 11);
        if (opcode < 0x18) return;
        switch ((u ? 64u : 0u) | (a ? 32u : 0u) | opcode) {
        case 0x18: set(mm_a64_fop::fmaxnm); return;
        case 0x19: set(mm_a64_fop::fmla); return;
        case 0x1A: set(mm_a64_fop::fadd); return;
        case 0x1C: set(mm_a64_fop::fcmeq); return;
        case 0x7A: set(mm_a64_fop::fabd); return;
        case 0x1E: set(mm_a64_fop::fmax); return;
        case 0x38: set(mm_a64_fop::fminnm); return;
        case 0x39: set(mm_a64_fop::fmls); return;
        case 0x3A: set(mm_a64_fop::fsub); return;
        case 0x3E: set(mm_a64_fop::fmin); return;
        case 0x5A: set(mm_a64_fop::faddp); return;
        case 0x5B: set(mm_a64_fop::fmul); return;
        case 0x5C: set(mm_a64_fop::fcmge); return;
        case 0x5E: set(mm_a64_fop::fmaxp); return;
        case 0x5F: set(mm_a64_fop::fdiv); return;
        case 0x7C: set(mm_a64_fop::fcmgt); return;
        case 0x7E: set(mm_a64_fop::fminp); return;
        default: return;
        }
    }
    if (bit(insn, 21) && bits(insn, 20, 17) == 0 && bits(insn, 11, 10) == 2) { /* two-reg misc */
        const std::uint32_t opcode = bits(insn, 16, 12);
        switch ((u ? 64u : 0u) | (a ? 32u : 0u) | opcode) {
        case 0x16: if (!sz) return; set(q ? mm_a64_fop::fcvtn2 : mm_a64_fop::fcvtn); out.elem_size = 8; return; /* fcvtn: 2d -> 2s (sz=0 is fp16) */
        case 0x17: if (!sz) return; set(q ? mm_a64_fop::fcvtl2 : mm_a64_fop::fcvtl); out.elem_size = 4; return; /* fcvtl: 2s -> 2d */
        case 0x18: set(mm_a64_fop::frintn); return;
        case 0x19: set(mm_a64_fop::frintm); return;
        case 0x1A: set(mm_a64_fop::fcvtns); return;
        case 0x1B: set(mm_a64_fop::fcvtms); return;
        case 0x1C: set(mm_a64_fop::fcvtas); return;
        case 0x1D: set(mm_a64_fop::scvtf); return;
        case 0x2C: set(mm_a64_fop::fcmgt0); return;
        case 0x2D: set(mm_a64_fop::fcmeq0); return;
        case 0x2E: set(mm_a64_fop::fcmlt0); return;
        case 0x2F: set(mm_a64_fop::fabs); return;
        case 0x38: set(mm_a64_fop::frintp); return;
        case 0x39: set(mm_a64_fop::frintz); return;
        case 0x3A: set(mm_a64_fop::fcvtps); return;
        case 0x3B: set(mm_a64_fop::fcvtzs); return;
        case 0x58: set(mm_a64_fop::frinta); return;
        case 0x59: set(mm_a64_fop::frintx); return;
        case 0x5A: set(mm_a64_fop::fcvtnu); return;
        case 0x5B: set(mm_a64_fop::fcvtmu); return;
        case 0x5C: set(mm_a64_fop::fcvtau); return;
        case 0x5D: set(mm_a64_fop::ucvtf); return;
        case 0x6C: set(mm_a64_fop::fcmge0); return;
        case 0x6D: set(mm_a64_fop::fcmle0); return;
        case 0x6F: set(mm_a64_fop::fneg); return;
        case 0x79: set(mm_a64_fop::frinti); return;
        case 0x7A: set(mm_a64_fop::fcvtpu); return;
        case 0x7B: set(mm_a64_fop::fcvtzu); return;
        case 0x7F: set(mm_a64_fop::fsqrt); return;
        default: return;
        }
    }
}

/* AdvSIMD modified immediate: expands abcdefgh per cmode/op into the 64-bit
 * pattern (ARM ARM AdvSIMDExpandImm). Returns false for FP forms. */
bool expand_imm(std::uint32_t op, std::uint32_t cmode, std::uint32_t abcdefgh, std::uint64_t &imm) {
    const std::uint64_t v = abcdefgh;
    switch (cmode >> 1) {
    case 0: imm = (v << 0) * 0x0000000100000001ull; break;
    case 1: imm = (v << 8) * 0x0000000100000001ull; break;
    case 2: imm = (v << 16) * 0x0000000100000001ull; break;
    case 3: imm = (v << 24) * 0x0000000100000001ull; break;
    case 4: imm = (v << 0) * 0x0001000100010001ull; break;
    case 5: imm = (v << 8) * 0x0001000100010001ull; break;
    case 6:
        imm = ((cmode & 1) ? (v << 16) | 0xFFFF : (v << 8) | 0xFF) * 0x0000000100000001ull;
        break;
    case 7:
        if (!(cmode & 1) && op == 0) { imm = v * 0x0101010101010101ull; break; }
        if (!(cmode & 1) && op == 1) {
            imm = 0;
            for (int i = 0; i < 8; i++)
                if (v & (1u << i)) imm |= 0xFFull << (i * 8);
            break;
        }
        return false; /* fmov (vector, immediate) */
    }
    return true;
}

/* Vector/scalar integer SIMD groups; leaves out.op unknown for anything
 * FP or not modelled. bit 28 distinguishes scalar (1) from vector (0). */
void decode_simd(std::uint32_t insn, mm_a64_insn &out) {
    const bool scalar = bit(insn, 28), q = bit(insn, 30), u = bit(insn, 29);
    if (scalar && !q)
        return; /* scalar floating-point data processing, not SIMD */
    const std::uint32_t size = bits(insn, 23, 22);
    out.q = scalar ? false : q;
    out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
    out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
    out.rm = static_cast<std::uint8_t>(bits(insn, 20, 16));
    auto set = [&](mm_a64_vop v, std::uint32_t esize_bytes) {
        out.op = mm_a64_opcode::vop;
        out.vop = v;
        out.elem_size = static_cast<std::uint8_t>(esize_bytes);
    };
    auto set_crypto = [&](mm_a64_crypto c) {
        out.op = mm_a64_opcode::crypto;
        out.crypto = c;
        out.q = true; /* every crypto instruction is a whole-register form */
    };

    /* Advanced SIMD crypto: the AES two-register group and the SHA1/SHA256
     * two-register group share one shape (bit 21, Rm field 01000, size 00,
     * bit 10 clear — three-same opcodes have it set) and differ only by the
     * scalar prefix bit 28; the six-bit opcode in bits 15:10 has bit 1 set
     * for every member. The SHA three-register
     * group is the scalar prefix with bit 21 clear. */
    if (q && !u && size == 0 && bit(insn, 21) && bits(insn, 20, 16) == 8 && !bit(insn, 10)) {
        switch (bits(insn, 15, 10)) {
        case 0x02: if (scalar) { set_crypto(mm_a64_crypto::sha1h); return; } break;
        case 0x06: if (scalar) { set_crypto(mm_a64_crypto::sha1su1); return; } break;
        case 0x0A: if (scalar) { set_crypto(mm_a64_crypto::sha256su0); return; } break;
        case 0x12: if (!scalar) { set_crypto(mm_a64_crypto::aese); return; } break;
        case 0x16: if (!scalar) { set_crypto(mm_a64_crypto::aesd); return; } break;
        case 0x1A: if (!scalar) { set_crypto(mm_a64_crypto::aesmc); return; } break;
        case 0x1E: if (!scalar) { set_crypto(mm_a64_crypto::aesimc); return; } break;
        }
        return; /* the group shape with an unallocated opcode */
    }
    if (scalar && q && !u && size == 0 && !bit(insn, 21) && bits(insn, 11, 10) == 0) {
        switch (bits(insn, 15, 10)) {
        case 0x00: set_crypto(mm_a64_crypto::sha1c); return;
        case 0x04: set_crypto(mm_a64_crypto::sha1p); return;
        case 0x08: set_crypto(mm_a64_crypto::sha1m); return;
        case 0x0C: set_crypto(mm_a64_crypto::sha1su0); return;
        case 0x10: set_crypto(mm_a64_crypto::sha256h); return;
        case 0x14: set_crypto(mm_a64_crypto::sha256h2); return;
        case 0x18: set_crypto(mm_a64_crypto::sha256su1); return;
        }
        return; /* the group shape with an unallocated opcode */
    }

    /* Shift by immediate: 0 Q U 011110 immh immb opcode 1 Rn Rd (immh != 0) */
    if (bits(insn, 27, 23) == 0x1Eu && bit(insn, 10) && bits(insn, 22, 19) != 0) {
        const std::uint32_t immh = bits(insn, 22, 19), immb = bits(insn, 18, 16), opcode = bits(insn, 15, 11);
        int lg = 3;
        while (!(immh & (1u << lg))) lg--;
        const std::uint32_t esize = 8u << lg, hb = (immh << 3) | immb;
        const std::uint32_t right = 2 * esize - hb, left = hb - esize;
        /* The fixed-point float conversions share this encoding group; for
         * them immh:immb encodes the fractional bits rather than a shift,
         * and the element width comes from immh alone. */
        if (opcode == 0x1Cu || opcode == 0x1Fu) {
            const std::uint32_t fesize = (immh & 0x8u) ? 8u : (immh & 0x4u) ? 4u : 0u;
            if (fesize == 0) return; /* half precision */
            out.op = mm_a64_opcode::fop;
            out.fvec = !scalar;
            out.simd_scalar = scalar;
            out.q = q;
            out.elem_size = static_cast<std::uint8_t>(fesize);
            out.imm = static_cast<std::int64_t>(16 * fesize - hb);
            if (opcode == 0x1Cu) out.fop = u ? mm_a64_fop::ucvtf : mm_a64_fop::scvtf;
            else out.fop = u ? mm_a64_fop::fcvtzu : mm_a64_fop::fcvtzs;
            return;
        }
        if (scalar && lg != 3) return; /* scalar shifts on b/h/s are saturating forms */
        switch ((u ? 32u : 0u) | opcode) {
        case 0x00: set(mm_a64_vop::sshr, esize / 8); out.imm = right; return;
        case 0x02: set(mm_a64_vop::ssra, esize / 8); out.imm = right; return;
        case 0x04: set(mm_a64_vop::srshr, esize / 8); out.imm = right; return;
        case 0x24: set(mm_a64_vop::urshr, esize / 8); out.imm = right; return;
        case 0x0A: set(mm_a64_vop::shl, esize / 8);  out.imm = left;  return;
        case 0x10: if (scalar || lg == 3) return; set(q ? mm_a64_vop::shrn2 : mm_a64_vop::shrn, 2 * esize / 8); out.imm = right; return;
        case 0x14: if (scalar || lg == 3) return; set(q ? mm_a64_vop::sshll2 : mm_a64_vop::sshll, esize / 8); out.imm = left; return;
        case 0x20: set(mm_a64_vop::ushr, esize / 8); out.imm = right; return;
        case 0x22: set(mm_a64_vop::usra, esize / 8); out.imm = right; return;
        case 0x28: if (!scalar && lg == 3 && !q) return; set(mm_a64_vop::sri, esize / 8); out.imm = right; return;
        case 0x2A: if (!scalar && lg == 3 && !q) return; set(mm_a64_vop::sli, esize / 8); out.imm = left; return;
        case 0x34: if (scalar || lg == 3) return; set(q ? mm_a64_vop::ushll2 : mm_a64_vop::ushll, esize / 8); out.imm = left; return;
        default: return;
        }
    }

    if (bits(insn, 27, 24) != 0xEu)
        return;
    const std::uint32_t esize = 1u << size;

    if (bit(insn, 21)) {
        if (bit(insn, 10)) { /* three-same: opcode bits 15..11 */
            const std::uint32_t opcode = bits(insn, 15, 11);
            if (scalar && size != 3 && opcode != 0x01 && opcode != 0x05) return;
            switch ((u ? 32u : 0u) | opcode) {
            case 0x00: set(mm_a64_vop::shadd, esize); return;
            case 0x20: set(mm_a64_vop::uhadd, esize); return;
            case 0x02: set(mm_a64_vop::srhadd, esize); return;
            case 0x22: set(mm_a64_vop::urhadd, esize); return;
            case 0x04: set(mm_a64_vop::shsub, esize); return;
            case 0x24: set(mm_a64_vop::uhsub, esize); return;
            case 0x01: set(mm_a64_vop::sqadd, esize); return;
            case 0x05: set(mm_a64_vop::sqsub, esize); return;
            case 0x06: set(mm_a64_vop::cmgt, esize); return;
            case 0x07: set(mm_a64_vop::cmge, esize); return;
            case 0x08: set(mm_a64_vop::sshl, esize); return;
            case 0x28: set(mm_a64_vop::ushl, esize); return;
            case 0x0E: set(mm_a64_vop::sabd, esize); return;
            case 0x2E: set(mm_a64_vop::uabd, esize); return;
            case 0x0F: set(mm_a64_vop::saba, esize); return;
            case 0x2F: set(mm_a64_vop::uaba, esize); return;
            case 0x0C: set(mm_a64_vop::smax, esize); return;
            case 0x0D: set(mm_a64_vop::smin, esize); return;
            case 0x10: set(mm_a64_vop::add, esize); return;
            case 0x11: set(mm_a64_vop::cmtst, esize); return;
            case 0x12: set(mm_a64_vop::mla, esize); return;
            case 0x13: set(mm_a64_vop::mul, esize); return;
            case 0x14: set(mm_a64_vop::smaxp, esize); return;
            case 0x15: set(mm_a64_vop::sminp, esize); return;
            case 0x17: set(mm_a64_vop::addp, esize); return;
            case 0x21: set(mm_a64_vop::uqadd, esize); return;
            case 0x25: set(mm_a64_vop::uqsub, esize); return;
            case 0x26: set(mm_a64_vop::cmhi, esize); return;
            case 0x27: set(mm_a64_vop::cmhs, esize); return;
            case 0x2C: set(mm_a64_vop::umax, esize); return;
            case 0x2D: set(mm_a64_vop::umin, esize); return;
            case 0x30: set(mm_a64_vop::sub, esize); return;
            case 0x31: set(mm_a64_vop::cmeq, esize); return;
            case 0x32: set(mm_a64_vop::mls, esize); return;
            case 0x34: set(mm_a64_vop::umaxp, esize); return;
            case 0x35: set(mm_a64_vop::uminp, esize); return;
            default: return;
            }
        }
        if (bits(insn, 11, 10) == 0) {
            /* Three-different: the sources and the destination have
             * different widths. `size` names the narrow element; the "2"
             * forms take their narrow operands from the upper half. */
            if (scalar || size == 3) return;
            const std::uint32_t opcode = bits(insn, 15, 12);
            auto pick = [&](mm_a64_vop lo, mm_a64_vop hi) { set(q ? hi : lo, esize); };
            switch ((u ? 16u : 0u) | opcode) {
            case 0x00: pick(mm_a64_vop::saddl, mm_a64_vop::saddl2); return;
            case 0x10: pick(mm_a64_vop::uaddl, mm_a64_vop::uaddl2); return;
            case 0x01: pick(mm_a64_vop::saddw, mm_a64_vop::saddw2); return;
            case 0x11: pick(mm_a64_vop::uaddw, mm_a64_vop::uaddw2); return;
            case 0x02: pick(mm_a64_vop::ssubl, mm_a64_vop::ssubl2); return;
            case 0x12: pick(mm_a64_vop::usubl, mm_a64_vop::usubl2); return;
            case 0x03: pick(mm_a64_vop::ssubw, mm_a64_vop::ssubw2); return;
            case 0x13: pick(mm_a64_vop::usubw, mm_a64_vop::usubw2); return;
            case 0x04: pick(mm_a64_vop::addhn, mm_a64_vop::addhn2); return;
            case 0x14: pick(mm_a64_vop::raddhn, mm_a64_vop::raddhn2); return;
            case 0x06: pick(mm_a64_vop::subhn, mm_a64_vop::subhn2); return;
            case 0x16: pick(mm_a64_vop::rsubhn, mm_a64_vop::rsubhn2); return;
            case 0x07: pick(mm_a64_vop::sabdl, mm_a64_vop::sabdl2); return;
            case 0x17: pick(mm_a64_vop::uabdl, mm_a64_vop::uabdl2); return;
            case 0x08: pick(mm_a64_vop::smlal, mm_a64_vop::smlal2); return;
            case 0x18: pick(mm_a64_vop::umlal, mm_a64_vop::umlal2); return;
            case 0x0A: pick(mm_a64_vop::smlsl, mm_a64_vop::smlsl2); return;
            case 0x1A: pick(mm_a64_vop::umlsl, mm_a64_vop::umlsl2); return;
            case 0x0C: pick(mm_a64_vop::smull_v, mm_a64_vop::smull2_v); return;
            case 0x1C: pick(mm_a64_vop::umull_v, mm_a64_vop::umull2_v); return;
            default: return;
            }
        }
        if (bits(insn, 11, 10) != 2)
            return; /* the rest of three-different is not modelled */
        const std::uint32_t opcode = bits(insn, 16, 12);
        if (bits(insn, 20, 17) == 0x0) { /* two-register misc */
            /* Saturating narrow: size describes the destination width.
             * Scalar Q is fixed at 1; only vector Q selects the "2" form. */
            if (opcode == 0x14 || (u && opcode == 0x12)) {
                if (size == 3) return;
                out.simd_scalar = scalar;
                const bool high = !scalar && q;
                if (opcode == 0x12) set(high ? mm_a64_vop::sqxtun2 : mm_a64_vop::sqxtun, 2 * esize);
                else if (u) set(high ? mm_a64_vop::uqxtn2 : mm_a64_vop::uqxtn, 2 * esize);
                else set(high ? mm_a64_vop::sqxtn2 : mm_a64_vop::sqxtn, 2 * esize);
                return;
            }
            if (scalar && size != 3) return;
            switch ((u ? 32u : 0u) | opcode) {
            case 0x00: if (scalar) return; set(mm_a64_vop::rev64, esize); return;
            case 0x01: if (scalar) return; set(mm_a64_vop::rev16, esize); return;
            case 0x02: if (scalar) return; set(mm_a64_vop::saddlp, esize); return;
            case 0x05: if (scalar) return; set(mm_a64_vop::cnt, esize); return;
            case 0x06: if (scalar) return; set(mm_a64_vop::sadalp, esize); return;
            case 0x26: if (scalar) return; set(mm_a64_vop::uadalp, esize); return;
            case 0x08: set(mm_a64_vop::cmgt0, esize); return;
            case 0x09: set(mm_a64_vop::cmeq0, esize); return;
            case 0x0A: set(mm_a64_vop::cmlt0, esize); return;
            case 0x0B: set(mm_a64_vop::abs, esize); return;
            case 0x12: if (scalar) return; set(q ? mm_a64_vop::xtn2 : mm_a64_vop::xtn, 2 * esize); return;
            case 0x20: if (scalar) return; set(mm_a64_vop::rev32, esize); return;
            case 0x22: if (scalar) return; set(mm_a64_vop::uaddlp, esize); return;
            case 0x25: if (scalar || size != 0) return; set(mm_a64_vop::not_, 1); return;
            case 0x28: set(mm_a64_vop::cmge0, esize); return;
            case 0x29: set(mm_a64_vop::cmle0, esize); return;
            case 0x2B: set(mm_a64_vop::neg, esize); return;
            default: return;
            }
        }
        if (bits(insn, 20, 17) == 0x8) { /* across lanes (vector) / pairwise (scalar) */
            if (scalar) {
                if (!u && opcode == 0x1B && size == 3) set(mm_a64_vop::addp, 8); /* addp d, v.2d */
                return;
            }
            switch ((u ? 32u : 0u) | opcode) {
            case 0x0A: set(mm_a64_vop::smaxv, esize); return;
            case 0x1A: set(mm_a64_vop::sminv, esize); return;
            case 0x1B: set(mm_a64_vop::addv, esize); return;
            case 0x03: return; /* saddlv: not modelled */
            case 0x23: set(mm_a64_vop::uaddlv, esize); return;
            case 0x2A: set(mm_a64_vop::umaxv, esize); return;
            case 0x3A: set(mm_a64_vop::uminv, esize); return;
            default: return;
            }
        }
        return;
    }

    /* three-register extension: 0 Q U 01110 size 0 Rm 1 opcode 1 Rn Rd */
    if (!scalar && !bit(insn, 21) && bit(insn, 15) && bits(insn, 11, 10) == 1) {
        const std::uint32_t opcode = bits(insn, 14, 11);
        if (opcode == 2 && size == 2) set(u ? mm_a64_vop::udot : mm_a64_vop::sdot, 1);
        return;
    }

    if (scalar) { /* scalar copy: dup d0, v1.d[i] */
        if (!u && bits(insn, 23, 21) == 0 && bits(insn, 15, 10) == 0x01u) {
            const std::uint32_t imm5 = bits(insn, 20, 16);
            int sz = 0;
            while (sz < 4 && !(imm5 & (1u << sz))) sz++;
            if (sz == 4) return;
            set(mm_a64_vop::dup_elem, 1u << sz);
            out.elem_index = static_cast<std::uint8_t>(imm5 >> (sz + 1));
        }
        return;
    }

    if (bit(insn, 15))
        return;
    if (u) { /* ext: 0 Q 101110 00 0 Rm 0 imm4 0 Rn Rd */
        if (size == 0 && !bit(insn, 10)) {
            const std::uint32_t idx = bits(insn, 14, 11);
            if (!q && idx >= 8) return;
            set(mm_a64_vop::ext, 1);
            out.imm = idx;
        }
        return;
    }
    if (bits(insn, 11, 10) == 2) { /* permute */
        switch (bits(insn, 14, 12)) {
        case 1: set(mm_a64_vop::uzp1, esize); return;
        case 2: set(mm_a64_vop::trn1, esize); return;
        case 3: set(mm_a64_vop::zip1, esize); return;
        case 5: set(mm_a64_vop::uzp2, esize); return;
        case 6: set(mm_a64_vop::trn2, esize); return;
        case 7: set(mm_a64_vop::zip2, esize); return;
        default: return;
        }
    }
    if (size == 0 && bits(insn, 11, 10) == 0 && !bit(insn, 12)) { /* tbl: len in bits 14..13 */
        const std::uint32_t len = bits(insn, 14, 13);
        if (len == 0) set(mm_a64_vop::tbl1, 1);
        else if (len == 1) set(mm_a64_vop::tbl2, 1);
    }
}

void decode_ldst(std::uint32_t insn, std::uint64_t pc, mm_a64_insn &out) {
    const std::uint32_t size = bits(insn, 31, 30);
    const std::uint32_t group = bits(insn, 29, 28);
    out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0)); /* Rt */
    out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));

    /* Load register (literal): opc 011 0 00 imm19 Rt */
    if (group == 1 && bits(insn, 25, 24) == 0) {
        out.addr = mm_a64_addr::literal;
        out.imm = static_cast<std::int64_t>(pc + static_cast<std::uint64_t>(sign_extend(bits(insn, 23, 5), 19) * 4));
        switch (size) { /* here the field is `opc` */
        case 0: out.op = mm_a64_opcode::ldr; out.mem_size = 4; out.is64 = true; return;
        case 1: out.op = mm_a64_opcode::ldr; out.mem_size = 8; out.is64 = true; return;
        case 2: out.op = mm_a64_opcode::ldr; out.mem_size = 4; out.mem_signed = true; out.is64 = true; return;
        default: out.op = mm_a64_opcode::nop; return; /* prfm literal */
        }
    }

    /* Load/store pair: opc 101 0 mode L imm7 Rt2 Rn Rt */
    if (group == 2) {
        const std::uint32_t mode = bits(insn, 24, 23);
        const bool load = bit(insn, 22);
        switch (size) { /* `opc` */
        case 0: out.mem_size = 4; break;
        case 1: if (!load) return; out.mem_size = 4; out.mem_signed = true; break; /* ldpsw */
        case 2: out.mem_size = 8; break;
        default: return;
        }
        out.is64 = true;
        out.op = load ? mm_a64_opcode::ldp : mm_a64_opcode::stp;
        out.rt2 = static_cast<std::uint8_t>(bits(insn, 14, 10));
        out.imm = sign_extend(bits(insn, 21, 15), 7) * out.mem_size;
        out.addr = mode == 1 ? mm_a64_addr::post_index : mode == 3 ? mm_a64_addr::pre_index : mm_a64_addr::offset;
        return;
    }

    /* Load/store exclusive & ordered: size 001000 o2 L o1 Rs o0 Rt2 Rn Rt */
    if (group == 0 && bits(insn, 27, 24) == 0x8u) {
        const bool o2 = bit(insn, 23), L = bit(insn, 22), o1 = bit(insn, 21);
        out.mem_size = static_cast<std::uint8_t>(1u << size);
        out.is64 = true;
        out.rt2 = static_cast<std::uint8_t>(bits(insn, 20, 16)); /* Rs */
        if (bits(insn, 14, 10) != 31 && !(o2 == 0 && o1 == 1))
            return;
        if (!o2 && !o1) { out.op = L ? mm_a64_opcode::ldxr : mm_a64_opcode::stxr; return; }
        if (o2 && !o1)  { out.op = L ? mm_a64_opcode::ldar : mm_a64_opcode::stlr; return; }
        if (o2 && o1)   { out.op = mm_a64_opcode::cas; return; }
        /* CASP: 0 sz 001000 0 L 1 Rs o0 11111 Rn Rt, with Rs and Rt even.
         * `size` is the pair's element width here, not the access width. */
        if (!o2 && o1 && bits(insn, 14, 10) == 31 && (size == 0 || size == 1)) {
            const std::uint32_t rs = bits(insn, 20, 16), rt = bits(insn, 4, 0);
            if ((rs & 1) || (rt & 1)) return;
            out.op = mm_a64_opcode::casp;
            out.mem_size = static_cast<std::uint8_t>(size == 0 ? 4 : 8);
            return;
        }
        return; /* ldxp/stxp: not modelled */
    }

    /* LDAPUR/STLUR (unscaled, RCpc): size 011001 opc 0 imm9 00 Rn Rt */
    if (group == 1 && bit(insn, 24) && !bit(insn, 21) && bits(insn, 11, 10) == 0) {
        bool load = false, prfm = false;
        if (!ldst_kind(size, bits(insn, 23, 22), out, load, prfm) || prfm)
            return;
        out.imm = sign_extend(bits(insn, 20, 12), 9);
        out.addr = mm_a64_addr::offset;
        out.op = load ? mm_a64_opcode::ldr : mm_a64_opcode::str; /* x86 loads/stores are already acquire/release */
        return;
    }

    if (group != 3)
        return;

    /* LSE atomic memory operations: size 111 0 00 A R 1 Rs o3 opc 00 Rn Rt */
    if (!bit(insn, 24) && bit(insn, 21) && bits(insn, 11, 10) == 0) {
        const std::uint32_t o3 = bits(insn, 15, 15), opc3 = bits(insn, 14, 12);
        out.mem_size = static_cast<std::uint8_t>(1u << size);
        out.is64 = true;
        out.rt2 = static_cast<std::uint8_t>(bits(insn, 20, 16)); /* Rs */
        if (o3 == 0) {
            out.op = mm_a64_opcode::atomic;
            out.atomic = static_cast<mm_a64_atomic>(opc3);
        } else if (opc3 == 0) {
            out.op = mm_a64_opcode::atomic;
            out.atomic = mm_a64_atomic::swp;
        } else if (opc3 == 4 && out.rt2 == 31) {
            out.op = mm_a64_opcode::ldar; /* ldapr */
        }
        return;
    }

    const std::uint32_t opc = bits(insn, 23, 22);
    bool load = false, prfm = false;
    if (!ldst_kind(size, opc, out, load, prfm))
        return;

    if (bit(insn, 24)) { /* unsigned immediate, scaled */
        out.addr = mm_a64_addr::offset;
        out.imm = static_cast<std::int64_t>(bits(insn, 21, 10)) * out.mem_size;
    } else if (!bit(insn, 21)) { /* imm9 forms */
        out.imm = sign_extend(bits(insn, 20, 12), 9);
        switch (bits(insn, 11, 10)) {
        case 0: out.addr = mm_a64_addr::offset; break;      /* unscaled (ldur/stur) */
        case 1: out.addr = mm_a64_addr::post_index; break;
        case 3: out.addr = mm_a64_addr::pre_index; break;
        default: return;                                    /* unprivileged (ldtr) */
        }
    } else if (bits(insn, 11, 10) == 2) { /* register offset */
        const std::uint32_t option = bits(insn, 15, 13);
        if (!(option & 2))
            return; /* reserved */
        out.addr = mm_a64_addr::reg_offset;
        out.rm = static_cast<std::uint8_t>(bits(insn, 20, 16));
        out.extend = static_cast<mm_a64_extend>(option);
        out.ext_shift = bit(insn, 12) ? static_cast<std::uint8_t>(size) : 0;
    } else {
        return; /* PAC-authenticated loads */
    }

    if (prfm) {
        if (out.addr == mm_a64_addr::pre_index || out.addr == mm_a64_addr::post_index)
            return;
        out.op = mm_a64_opcode::nop;
        return;
    }
    out.op = load ? mm_a64_opcode::ldr : mm_a64_opcode::str;
}

} // namespace

mm_a64_insn mm_a64_decode(std::span<const std::uint8_t> bytes, std::uint64_t pc) {
    std::uint32_t insn;
    std::memcpy(&insn, bytes.data(), sizeof insn);

    mm_a64_insn out;
    out.raw = insn;
    if (mm_decode_simd_helper(insn) != mm_simd_helper::none) {
        out.op = mm_a64_opcode::simd_helper;
        return out;
    }

    auto target = [pc](std::int64_t off) { return static_cast<std::int64_t>(pc + static_cast<std::uint64_t>(off)); };

    /* Unconditional branch (register): 1101011 0 opc(2) 11111 0000 00 Rn 00000 */
    if ((insn & 0xFFFFFC1Fu) == 0xD65F0000u || (insn & 0xFFFFFC1Fu) == 0xD61F0000u ||
        (insn & 0xFFFFFC1Fu) == 0xD63F0000u) {
        const std::uint32_t opc = bits(insn, 22, 21);
        out.op = opc == 0 ? mm_a64_opcode::br : opc == 1 ? mm_a64_opcode::blr : mm_a64_opcode::ret;
        out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
        return out;
    }

    /* SHA3 three-register logic (used far outside cryptography, as a way to
     * fold three vectors in one instruction):
     *   EOR3 1100 1110 000 Rm 0 Ra Rn Rd
     *   BCAX 1100 1110 001 Rm 0 Ra Rn Rd */
    if (bits(insn, 31, 24) == 0xCEu && bits(insn, 23, 22) == 0 && !bit(insn, 15)) {
        out.op = bit(insn, 21) ? mm_a64_opcode::bcax : mm_a64_opcode::eor3;
        out.simd = true;
        out.q = true;
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
        out.ra = static_cast<std::uint8_t>(bits(insn, 14, 10));
        out.rm = static_cast<std::uint8_t>(bits(insn, 20, 16));
        return out;
    }

    if (insn == 0xD503201Fu) {
        out.op = mm_a64_opcode::nop;
        return out;
    }

    /* Exceptions: 11010100 opc(3) imm16 000 LL: svc = opc 000 LL 01, brk = opc 001 LL 00 */
    if (bits(insn, 31, 24) == 0xD4u) {
        out.imm = static_cast<std::int64_t>(bits(insn, 20, 5));
        if (bits(insn, 23, 21) == 0 && bits(insn, 4, 0) == 1)
            out.op = mm_a64_opcode::svc;
        else if (bits(insn, 23, 21) == 1 && bits(insn, 4, 0) == 0)
            out.op = mm_a64_opcode::brk;
        return out;
    }

    /* System: 1101010100 L op0 op1 CRn CRm op2 Rt */
    if (bits(insn, 31, 22) == 0x354u) {
        const bool L = bit(insn, 21);
        const std::uint32_t op0 = bits(insn, 20, 19), op1 = bits(insn, 18, 16), crn = bits(insn, 15, 12),
                            crm = bits(insn, 11, 8), op2 = bits(insn, 7, 5);
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        if (!L && op0 == 0 && op1 == 3 && crn == 2) { /* hints: nop/yield/wfe/wfi/sev/sevl/csdb/bti/pac hints */
            out.op = mm_a64_opcode::nop;
            return out;
        }
        if (!L && op0 == 0 && op1 == 3 && crn == 3 && (op2 == 4 || op2 == 5 || op2 == 6)) { /* dsb/dmb/isb */
            out.op = mm_a64_opcode::barrier;
            return out;
        }
        if (!L && op0 == 0 && op1 == 3 && crn == 3 && op2 == 2) { /* clrex */
            out.op = mm_a64_opcode::nop;
            return out;
        }
        if (op0 >= 2) { /* mrs / msr (register) */
            const std::uint32_t key = (op0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2;
            switch (key) {
            case (3u << 14) | (3u << 11) | (4u << 7) | (2u << 3) | 0: out.sysreg = mm_a64_sysreg::nzcv; break;
            case (3u << 14) | (3u << 11) | (4u << 7) | (4u << 3) | 0: out.sysreg = mm_a64_sysreg::fpcr; break;
            case (3u << 14) | (3u << 11) | (4u << 7) | (4u << 3) | 1: out.sysreg = mm_a64_sysreg::fpsr; break;
            case (3u << 14) | (3u << 11) | (13u << 7) | (0u << 3) | 2: out.sysreg = mm_a64_sysreg::tpidr_el0; break;
            case (3u << 14) | (3u << 11) | (13u << 7) | (0u << 3) | 3: out.sysreg = mm_a64_sysreg::tpidrro_el0; break;
            case (3u << 14) | (3u << 11) | (14u << 7) | (0u << 3) | 2: out.sysreg = mm_a64_sysreg::cntvct_el0; break;
            case (3u << 14) | (3u << 11) | (14u << 7) | (0u << 3) | 0: out.sysreg = mm_a64_sysreg::cntfrq_el0; break;
            case (3u << 14) | (3u << 11) | (0u << 7) | (0u << 3) | 7: out.sysreg = mm_a64_sysreg::dczid_el0; break;
            case (3u << 14) | (3u << 11) | (0u << 7) | (0u << 3) | 1: out.sysreg = mm_a64_sysreg::ctr_el0; break;
            default: return out;
            }
            out.op = L ? mm_a64_opcode::mrs : mm_a64_opcode::msr;
            return out;
        }
        return out;
    }

    /* B / BL: op 00101 imm26 */
    if (bits(insn, 30, 26) == 0x05u) {
        out.op = bit(insn, 31) ? mm_a64_opcode::bl : mm_a64_opcode::b;
        out.imm = target(sign_extend(bits(insn, 25, 0), 26) * 4);
        return out;
    }

    /* B.cond: 0101010 0 imm19 0 cond */
    if (bits(insn, 31, 24) == 0x54u && !bit(insn, 4)) {
        out.op = mm_a64_opcode::b_cond;
        out.cond = static_cast<std::uint8_t>(bits(insn, 3, 0));
        out.imm = target(sign_extend(bits(insn, 23, 5), 19) * 4);
        return out;
    }

    /* CBZ/CBNZ: sf 011010 op imm19 Rt */
    if (bits(insn, 30, 25) == 0x1Au) {
        out.op = bit(insn, 24) ? mm_a64_opcode::cbnz : mm_a64_opcode::cbz;
        out.is64 = bit(insn, 31);
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.imm = target(sign_extend(bits(insn, 23, 5), 19) * 4);
        return out;
    }

    /* TBZ/TBNZ: b5 011011 op b40 imm14 Rt */
    if (bits(insn, 30, 25) == 0x1Bu) {
        out.op = bit(insn, 24) ? mm_a64_opcode::tbnz : mm_a64_opcode::tbz;
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.imms = static_cast<std::uint8_t>((bits(insn, 31, 31) << 5) | bits(insn, 23, 19));
        out.is64 = out.imms >= 32;
        out.imm = target(sign_extend(bits(insn, 18, 5), 14) * 4);
        return out;
    }

    /* Conditional select: sf op 0 11010100 Rm cond 0 o2 Rn Rd */
    if (bits(insn, 28, 21) == 0xD4u && !bit(insn, 29) && !bit(insn, 11)) {
        const std::uint32_t k = (bits(insn, 30, 30) << 1) | bits(insn, 10, 10);
        out.op = k == 0 ? mm_a64_opcode::csel : k == 1 ? mm_a64_opcode::csinc
               : k == 2 ? mm_a64_opcode::csinv : mm_a64_opcode::csneg;
        out.is64 = bit(insn, 31);
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
        out.rm = static_cast<std::uint8_t>(bits(insn, 20, 16));
        out.cond = static_cast<std::uint8_t>(bits(insn, 15, 12));
        return out;
    }

    /* Conditional compare: sf op 1 11010010 Rm/imm5 cond imm 0 Rn 0 nzcv */
    if (bits(insn, 28, 21) == 0xD2u && bit(insn, 29) && !bit(insn, 10) && !bit(insn, 4)) {
        out.op = mm_a64_opcode::ccmp;
        out.ccmn = !bit(insn, 30);
        out.is64 = bit(insn, 31);
        out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
        out.cond = static_cast<std::uint8_t>(bits(insn, 15, 12));
        out.nzcv = static_cast<std::uint8_t>(bits(insn, 3, 0));
        out.ccmp_imm = bit(insn, 11);
        if (out.ccmp_imm)
            out.imm = static_cast<std::int64_t>(bits(insn, 20, 16));
        else
            out.rm = static_cast<std::uint8_t>(bits(insn, 20, 16));
        return out;
    }

    /* Add/subtract with carry: sf op S 11010000 Rm 000000 Rn Rd */
    if (bits(insn, 28, 21) == 0xD0u && bits(insn, 15, 10) == 0) {
        out.op = bit(insn, 30) ? mm_a64_opcode::sbc : mm_a64_opcode::adc;
        out.is64 = bit(insn, 31);
        out.sets_flags = bit(insn, 29);
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
        out.rm = static_cast<std::uint8_t>(bits(insn, 20, 16));
        return out;
    }

    /* MOVZ/MOVN/MOVK: sf opc 100101 hw imm16 Rd */
    if (bits(insn, 28, 23) == 0x25u) {
        const std::uint32_t opc = bits(insn, 30, 29);
        const std::uint32_t hw = bits(insn, 22, 21);
        out.is64 = bit(insn, 31);
        if (opc == 1 || (!out.is64 && hw > 1))
            return out; /* unallocated */
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.shift_amount = static_cast<std::uint8_t>(hw * 16);
        out.imm = static_cast<std::int64_t>(bits(insn, 20, 5)) << out.shift_amount;
        out.op = opc == 2 ? mm_a64_opcode::movz : opc == 0 ? mm_a64_opcode::movn : mm_a64_opcode::movk;
        return out;
    }

    /* ADD/SUB (immediate): sf op S 100010 0 sh imm12 Rn Rd */
    if (bits(insn, 28, 24) == 0x11u) {
        if (bit(insn, 23))
            return out; /* reserved shift value */
        out.op = bit(insn, 30) ? mm_a64_opcode::sub_imm : mm_a64_opcode::add_imm;
        out.is64 = bit(insn, 31);
        out.sets_flags = bit(insn, 29);
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
        out.imm = static_cast<std::int64_t>(bits(insn, 21, 10)) << (bit(insn, 22) ? 12 : 0);
        return out;
    }

    /* ADD/SUB (shifted register): sf op S 01011 shift 0 Rm imm6 Rn Rd.
     * Bit 21 set is the extended-register form (uxtb/sxtw/...), which is
     * not modelled yet, so it stays `unknown`. */
    if (bits(insn, 28, 24) == 0x0Bu) {
        const std::uint32_t shift = bits(insn, 23, 22);
        const std::uint32_t imm6 = bits(insn, 15, 10);
        out.is64 = bit(insn, 31);
        if (bit(insn, 21)) { /* extended register: opt(2)=00 1 Rm option(3) imm3 Rn Rd */
            const std::uint32_t imm3 = bits(insn, 12, 10);
            if (shift != 0 || imm3 > 4)
                return out;
            out.op = bit(insn, 30) ? mm_a64_opcode::sub_ext : mm_a64_opcode::add_ext;
            out.sets_flags = bit(insn, 29);
            out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
            out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
            out.rm = static_cast<std::uint8_t>(bits(insn, 20, 16));
            out.extend = static_cast<mm_a64_extend>(bits(insn, 15, 13));
            out.ext_shift = static_cast<std::uint8_t>(imm3);
            return out;
        }
        if (shift == 3 || (!out.is64 && imm6 >= 32))
            return out;
        out.op = bit(insn, 30) ? mm_a64_opcode::sub_reg : mm_a64_opcode::add_reg;
        out.sets_flags = bit(insn, 29);
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
        out.rm = static_cast<std::uint8_t>(bits(insn, 20, 16));
        out.shift = static_cast<mm_a64_shift>(shift);
        out.shift_amount = static_cast<std::uint8_t>(imm6);
        return out;
    }

    /* Logical (immediate): sf opc 100100 N immr imms Rn Rd */
    if (bits(insn, 28, 23) == 0x24u) {
        out.is64 = bit(insn, 31);
        std::uint64_t mask;
        if (!decode_bit_masks(bit(insn, 22), bits(insn, 15, 10), bits(insn, 21, 16), out.is64, mask))
            return out;
        switch (bits(insn, 30, 29)) {
        case 0: out.op = mm_a64_opcode::and_imm; break;
        case 1: out.op = mm_a64_opcode::orr_imm; break;
        case 2: out.op = mm_a64_opcode::eor_imm; break;
        default: out.op = mm_a64_opcode::and_imm; out.sets_flags = true; break;
        }
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
        out.imm = static_cast<std::int64_t>(mask);
        return out;
    }

    /* Extract: sf 00 100111 N 0 Rm imms Rn Rd */
    if (bits(insn, 30, 23) == 0x27u && !bit(insn, 21)) {
        out.is64 = bit(insn, 31);
        const std::uint32_t imms = bits(insn, 15, 10);
        if (bit(insn, 22) != out.is64 || (!out.is64 && imms >= 32))
            return out;
        out.op = mm_a64_opcode::extr;
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
        out.rm = static_cast<std::uint8_t>(bits(insn, 20, 16));
        out.imms = static_cast<std::uint8_t>(imms);
        return out;
    }

    /* Bitfield: sf opc 100110 N immr imms Rn Rd */
    if (bits(insn, 28, 23) == 0x26u) {
        out.is64 = bit(insn, 31);
        const std::uint32_t opc = bits(insn, 30, 29);
        const std::uint32_t immr = bits(insn, 21, 16), imms = bits(insn, 15, 10);
        if (opc == 3 || bit(insn, 22) != out.is64 || (!out.is64 && (immr >= 32 || imms >= 32)))
            return out;
        out.op = opc == 0 ? mm_a64_opcode::sbfm : opc == 1 ? mm_a64_opcode::bfm : mm_a64_opcode::ubfm;
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
        out.immr = static_cast<std::uint8_t>(immr);
        out.imms = static_cast<std::uint8_t>(imms);
        return out;
    }

    /* Logical (shifted register): sf opc 01010 shift N Rm imm6 Rn Rd */
    if (bits(insn, 28, 24) == 0x0Au) {
        const std::uint32_t opc = bits(insn, 30, 29);
        const std::uint32_t imm6 = bits(insn, 15, 10);
        out.is64 = bit(insn, 31);
        if (!out.is64 && imm6 >= 32)
            return out;
        switch (opc) {
        case 0: out.op = mm_a64_opcode::and_reg; break;
        case 1: out.op = mm_a64_opcode::orr_reg; break;
        case 2: out.op = mm_a64_opcode::eor_reg; break;
        case 3: out.op = mm_a64_opcode::and_reg; out.sets_flags = true; break;
        }
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
        out.rm = static_cast<std::uint8_t>(bits(insn, 20, 16));
        out.shift = static_cast<mm_a64_shift>(bits(insn, 23, 22));
        out.shift_amount = static_cast<std::uint8_t>(imm6);
        out.invert_rm = bit(insn, 21);
        return out;
    }

    /* Data-processing (3 source): sf op54 11011 op31 Rm o0 Ra Rn Rd */
    if (bits(insn, 28, 24) == 0x1Bu && bits(insn, 30, 29) == 0) {
        const std::uint32_t op31 = bits(insn, 23, 21);
        out.is64 = bit(insn, 31);
        out.negate_product = bit(insn, 15);
        switch (op31) {
        case 0: out.mulk = mm_a64_mul::full; break;
        case 1: out.mulk = mm_a64_mul::smull; break;
        case 5: out.mulk = mm_a64_mul::umull; break;
        case 2: out.mulk = mm_a64_mul::smulh; break;
        case 6: out.mulk = mm_a64_mul::umulh; break;
        default: return out;
        }
        if (!out.is64 && op31 != 0)
            return out;
        if ((op31 == 2 || op31 == 6) && (out.negate_product || bits(insn, 14, 10) != 31))
            return out;
        out.op = mm_a64_opcode::madd;
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
        out.rm = static_cast<std::uint8_t>(bits(insn, 20, 16));
        out.ra = static_cast<std::uint8_t>(bits(insn, 14, 10));
        return out;
    }

    /* Data-processing (2 source): sf 0 0 11010110 Rm opcode Rn Rd
     * Data-processing (1 source): sf 1 0 11010110 00000 opcode Rn Rd */
    if (bits(insn, 28, 21) == 0xD6u && !bit(insn, 29)) {
        out.is64 = bit(insn, 31);
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
        if (!bit(insn, 30)) {
            out.rm = static_cast<std::uint8_t>(bits(insn, 20, 16));
            switch (bits(insn, 15, 10)) {
            case 0x02: out.op = mm_a64_opcode::udiv; break;
            case 0x03: out.op = mm_a64_opcode::sdiv; break;
            case 0x08: out.op = mm_a64_opcode::lslv; break;
            case 0x09: out.op = mm_a64_opcode::lsrv; break;
            case 0x0A: out.op = mm_a64_opcode::asrv; break;
            case 0x0B: out.op = mm_a64_opcode::rorv; break;
            /* CRC32{C}{B,H,W,X}: the size is in the low two bits of the
             * opcode, and the accumulate operand is always 32-bit while
             * the data operand follows that size. */
            case 0x10: case 0x11: case 0x12: case 0x13:
            case 0x14: case 0x15: case 0x16: case 0x17: {
                const unsigned size = bits(insn, 11, 10);
                if (size == 3 && !bit(insn, 31)) break;   /* CRC32X needs sf=1 */
                if (size != 3 && bit(insn, 31)) break;    /* the others need sf=0 */
                out.op = bit(insn, 12) ? mm_a64_opcode::crc32c : mm_a64_opcode::crc32;
                out.imm = 1 << size;                      /* bytes of data */
                out.is64 = false;                         /* the result is 32-bit */
                break;
            }
            default: break;
            }
            return out;
        }
        if (bits(insn, 20, 16) != 0)
            return out;
        switch (bits(insn, 15, 10)) {
        case 0x00: out.op = mm_a64_opcode::rbit; break;
        case 0x01: out.op = mm_a64_opcode::rev16; break;
        case 0x02: out.op = out.is64 ? mm_a64_opcode::rev32 : mm_a64_opcode::rev; break;
        case 0x03: if (out.is64) out.op = mm_a64_opcode::rev; break;
        case 0x04: out.op = mm_a64_opcode::clz; break;
        case 0x05: out.op = mm_a64_opcode::cls; break;
        default: break;
        }
        return out;
    }

    /* ADR/ADRP: op immlo(2) 10000 immhi(19) Rd */
    if (bits(insn, 28, 24) == 0x10u) {
        const std::uint32_t imm21 = (bits(insn, 23, 5) << 2) | bits(insn, 30, 29);
        const std::int64_t off = sign_extend(imm21, 21);
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        if (bit(insn, 31)) {
            out.op = mm_a64_opcode::adrp;
            out.imm = static_cast<std::int64_t>((pc & ~0xFFFull) + static_cast<std::uint64_t>(off << 12));
        } else {
            out.op = mm_a64_opcode::adr;
            out.imm = static_cast<std::int64_t>(pc + static_cast<std::uint64_t>(off));
        }
        return out;
    }

    /* Load/store family: op0 = x1x0 (bit 27 set, bit 25 clear); bit 26 is V. */
    if (bit(insn, 27) && !bit(insn, 25)) {
        if (bit(insn, 26))
            decode_ldst_simd(insn, pc, out);
        else
            decode_ldst(insn, pc, out);
        return out;
    }

    /* FMOV (general): sf 0 0 11110 type 1 rmode opcode 000000 Rn Rd */
    if (bits(insn, 30, 24) == 0x1Eu && bit(insn, 21) && bits(insn, 15, 10) == 0 &&
        (bits(insn, 18, 16) == 6 || bits(insn, 18, 16) == 7)) {
        const bool sf = bit(insn, 31), to_gpr = bits(insn, 18, 16) == 6;
        const std::uint32_t type = bits(insn, 23, 22), rmode = bits(insn, 20, 19);
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
        if (!sf && type == 0 && rmode == 0)      { out.is64 = false; out.elem_size = 4; out.elem_index = 0; }
        else if (sf && type == 1 && rmode == 0)  { out.is64 = true;  out.elem_size = 8; out.elem_index = 0; }
        else if (sf && type == 2 && rmode == 1)  { out.is64 = true;  out.elem_size = 8; out.elem_index = 1; }
        else return out; /* half-precision forms */
        out.op = to_gpr ? mm_a64_opcode::fmov_to_gpr : mm_a64_opcode::fmov_from_gpr;
        return out;
    }

    /* AdvSIMD copy: 0 Q op 01110000 imm5 0 imm4 1 Rn Rd */
    if (!bit(insn, 31) && bits(insn, 28, 21) == 0x70u && bit(insn, 10) && !bit(insn, 15)) {
        const std::uint32_t imm5 = bits(insn, 20, 16), imm4 = bits(insn, 14, 11);
        const bool q = bit(insn, 30), op = bit(insn, 29);
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
        out.q = q;
        int sz = 0;
        while (sz < 4 && !(imm5 & (1u << sz))) sz++;
        if (sz == 4) return out;
        out.elem_size = static_cast<std::uint8_t>(1u << sz);
        out.elem_index = static_cast<std::uint8_t>(imm5 >> (sz + 1));
        if (op) { /* ins (element): imm4 >> sz is the source index */
            if (!q) return out;
            out.op = mm_a64_opcode::vop;
            out.vop = mm_a64_vop::ins_elem;
            out.imm = imm4 >> sz;
            return out;
        }
        if (imm4 == 0) { /* dup (element) */
            if (!q && sz == 3) return out;
            out.op = mm_a64_opcode::vop;
            out.vop = mm_a64_vop::dup_elem;
            return out;
        }
        switch (imm4) {
        case 1: out.op = mm_a64_opcode::dup_gen; return out;
        case 3: if (!q) return out; out.op = mm_a64_opcode::ins_gen; return out;
        case 5: /* smov: Wd for b/h (Q=0), Xd for b/h/s (Q=1) */
            if (sz == 3 || (!q && sz == 2)) return out;
            out.is64 = q; out.op = mm_a64_opcode::smov; return out;
        case 7: /* umov: Wd for b/h/s (Q=0), Xd for d (Q=1) */
            if ((q && sz != 3) || (!q && sz == 3)) return out;
            out.is64 = q; out.op = mm_a64_opcode::umov; return out;
        default: return out;
        }
    }

    /* AdvSIMD modified immediate: 0 Q op 0111100000 a b c cmode o2 1 d e f g h Rd */
    if (!bit(insn, 31) && bits(insn, 28, 19) == 0x1E0u && bit(insn, 10) && !bit(insn, 11)) {
        const std::uint32_t op = bits(insn, 29, 29), cmode = bits(insn, 15, 12);
        const std::uint32_t abcdefgh = (bits(insn, 18, 16) << 5) | bits(insn, 9, 5);
        std::uint64_t imm;
        if (cmode == 15) { /* fmov (vector, immediate): op=0 single, op=1 double (Q must be 1) */
            if (op == 1 && !bit(insn, 30)) return out;
            imm = op == 1 ? vfp_expand_imm(abcdefgh, 8) : vfp_expand_imm(abcdefgh, 4) * 0x100000001ull;
            out.op = mm_a64_opcode::movi;
            out.q = bit(insn, 30);
            out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
            out.imm = static_cast<std::int64_t>(imm);
            return out;
        }
        if (!expand_imm(op, cmode, abcdefgh, imm))
            return out;
        /* MVNI (op=1 for cmode 0xx0/10x0/110x) inverts; for cmode=1110 op=1 is the byte-mask form */
        const bool mvni = op == 1 && (cmode >> 1) != 7;
        out.op = mm_a64_opcode::movi;
        out.simd_imm_merge = cmode < 12 && (cmode & 1);
        out.q = bit(insn, 30);
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.imm = static_cast<std::int64_t>(mvni ? ~imm : imm);
        return out;
    }

    /* Scalar floating point: x 0 x 1111x ... (bit 28 set, bit 30 clear) */
    if (bit(insn, 28) && !bit(insn, 30) && bits(insn, 27, 25) == 0x7u) {
        decode_fp_scalar(insn, out);
        return out;
    }

    /* AdvSIMD integer data processing, vector and scalar forms. */
    if (!bit(insn, 31) && bits(insn, 27, 25) == 0x7u) {
        decode_simd(insn, out);
        if (out.op != mm_a64_opcode::unknown)
            return out;
        out = mm_a64_insn{};
        out.raw = insn;
        decode_fp_vector(insn, out, bit(insn, 28) && bit(insn, 30));
        if (out.op != mm_a64_opcode::unknown)
            return out;
    }

    /* AdvSIMD three-same, logical: 0 Q U 01110 size 1 Rm 00011 1 Rn Rd */
    if (!bit(insn, 31) && bits(insn, 28, 24) == 0x0Eu && bit(insn, 21) && bits(insn, 15, 10) == 0x07u) {
        out.op = mm_a64_opcode::vlogic;
        out.q = bit(insn, 30);
        out.rd = static_cast<std::uint8_t>(bits(insn, 4, 0));
        out.rn = static_cast<std::uint8_t>(bits(insn, 9, 5));
        out.rm = static_cast<std::uint8_t>(bits(insn, 20, 16));
        out.vlogic = static_cast<mm_a64_vlogic>((bit(insn, 29) ? 4u : 0u) | bits(insn, 23, 22));
        return out;
    }

    return out;
}

const char *mm_a64_opcode_name(mm_a64_opcode op) {
    switch (op) {
    case mm_a64_opcode::unknown: return "unknown";
    case mm_a64_opcode::simd_helper: return "simd.helper";
    case mm_a64_opcode::movz:    return "movz";
    case mm_a64_opcode::movk:    return "movk";
    case mm_a64_opcode::movn:    return "movn";
    case mm_a64_opcode::add_imm: return "add.imm";
    case mm_a64_opcode::sub_imm: return "sub.imm";
    case mm_a64_opcode::add_reg: return "add.reg";
    case mm_a64_opcode::sub_reg: return "sub.reg";
    case mm_a64_opcode::add_ext: return "add.ext";
    case mm_a64_opcode::sub_ext: return "sub.ext";
    case mm_a64_opcode::and_imm: return "and.imm";
    case mm_a64_opcode::orr_imm: return "orr.imm";
    case mm_a64_opcode::eor_imm: return "eor.imm";
    case mm_a64_opcode::madd:    return "madd";
    case mm_a64_opcode::udiv:    return "udiv";
    case mm_a64_opcode::sdiv:    return "sdiv";
    case mm_a64_opcode::crc32:   return "crc32";
    case mm_a64_opcode::crc32c:  return "crc32c";
    case mm_a64_opcode::lslv:    return "lslv";
    case mm_a64_opcode::lsrv:    return "lsrv";
    case mm_a64_opcode::asrv:    return "asrv";
    case mm_a64_opcode::rorv:    return "rorv";
    case mm_a64_opcode::rev:     return "rev";
    case mm_a64_opcode::rev16:   return "rev16";
    case mm_a64_opcode::rev32:   return "rev32";
    case mm_a64_opcode::rbit:    return "rbit";
    case mm_a64_opcode::cls:     return "cls";
    case mm_a64_opcode::extr:    return "extr";
    case mm_a64_opcode::barrier: return "barrier";
    case mm_a64_opcode::mrs:     return "mrs";
    case mm_a64_opcode::msr:     return "msr";
    case mm_a64_opcode::clz:     return "clz";
    case mm_a64_opcode::sbfm:    return "sbfm";
    case mm_a64_opcode::bfm:     return "bfm";
    case mm_a64_opcode::ubfm:    return "ubfm";
    case mm_a64_opcode::and_reg: return "and.reg";
    case mm_a64_opcode::orr_reg: return "orr.reg";
    case mm_a64_opcode::eor_reg: return "eor.reg";
    case mm_a64_opcode::ldr:     return "ldr";
    case mm_a64_opcode::str:     return "str";
    case mm_a64_opcode::ldp:     return "ldp";
    case mm_a64_opcode::stp:     return "stp";
    case mm_a64_opcode::adr:     return "adr";
    case mm_a64_opcode::adrp:    return "adrp";
    case mm_a64_opcode::fmov_to_gpr:   return "fmov.to_gpr";
    case mm_a64_opcode::fmov_from_gpr: return "fmov.from_gpr";
    case mm_a64_opcode::ins_gen: return "ins";
    case mm_a64_opcode::umov:    return "umov";
    case mm_a64_opcode::smov:    return "smov";
    case mm_a64_opcode::dup_gen: return "dup";
    case mm_a64_opcode::movi:    return "movi";
    case mm_a64_opcode::vlogic:  return "vlogic";
    case mm_a64_opcode::vop:     return "vop";
    case mm_a64_opcode::fop:     return "fop";
    case mm_a64_opcode::ld1_multi: return "ld1";
    case mm_a64_opcode::st1_multi: return "st1";
    case mm_a64_opcode::ld1_lane: return "ld1.lane";
    case mm_a64_opcode::st1_lane: return "st1.lane";
    case mm_a64_opcode::ld1r:    return "ld1r";
    case mm_a64_opcode::ldn_multi: return "ldN";
    case mm_a64_opcode::stn_multi: return "stN";
    case mm_a64_opcode::ldn_lane:  return "ldN.lane";
    case mm_a64_opcode::stn_lane:  return "stN.lane";
    case mm_a64_opcode::ldnr:      return "ldNr";
    case mm_a64_opcode::eor3:      return "eor3";
    case mm_a64_opcode::bcax:      return "bcax";
    case mm_a64_opcode::crypto:    return "crypto";
    case mm_a64_opcode::ldar:    return "ldar";
    case mm_a64_opcode::stlr:    return "stlr";
    case mm_a64_opcode::ldxr:    return "ldxr";
    case mm_a64_opcode::stxr:    return "stxr";
    case mm_a64_opcode::cas:     return "cas";
    case mm_a64_opcode::casp:    return "casp";
    case mm_a64_opcode::atomic:  return "atomic";
    case mm_a64_opcode::csel:    return "csel";
    case mm_a64_opcode::csinc:   return "csinc";
    case mm_a64_opcode::csinv:   return "csinv";
    case mm_a64_opcode::csneg:   return "csneg";
    case mm_a64_opcode::ccmp:    return "ccmp";
    case mm_a64_opcode::adc:     return "adc";
    case mm_a64_opcode::sbc:     return "sbc";
    case mm_a64_opcode::b:       return "b";
    case mm_a64_opcode::bl:      return "bl";
    case mm_a64_opcode::b_cond:  return "b.cond";
    case mm_a64_opcode::cbz:     return "cbz";
    case mm_a64_opcode::cbnz:    return "cbnz";
    case mm_a64_opcode::tbz:     return "tbz";
    case mm_a64_opcode::tbnz:    return "tbnz";
    case mm_a64_opcode::br:      return "br";
    case mm_a64_opcode::blr:     return "blr";
    case mm_a64_opcode::ret:     return "ret";
    case mm_a64_opcode::svc:     return "svc";
    case mm_a64_opcode::brk:     return "brk";
    case mm_a64_opcode::nop:     return "nop";
    }
    return "?";
}

const char *mm_a64_class_name(std::uint32_t insn) {
    const std::uint32_t op0 = bits(insn, 28, 25);

    if (op0 == 0x0) return "reserved/sme";
    if (op0 == 0x1) return "unallocated";
    if (op0 == 0x2) return "sve";

    if ((op0 & 0xE) == 0x8) { /* 100x: data processing — immediate */
        switch (bits(insn, 25, 23)) {
        case 0: case 1: return "dp.imm.pcrel(adr/adrp)";
        case 2:         return "dp.imm.addsub";
        case 3:         return "dp.imm.addsub_tags";
        case 4:         return "dp.imm.logical";
        case 5:         return "dp.imm.movwide";
        case 6:         return "dp.imm.bitfield";
        default:        return "dp.imm.extract";
        }
    }

    if ((op0 & 0xE) == 0xA) { /* 101x: branches, exceptions, system */
        const std::uint32_t hi = bits(insn, 31, 29);
        const std::uint32_t mid = bits(insn, 25, 22);
        if (hi == 2)             return "br.cond";
        if (hi == 6) {
            if ((mid & 0xC) == 0x0) return "exception(svc/brk)";
            if (mid == 0x4)         return "system(msr/mrs/hint)";
            if ((mid & 0x8) == 0x8) return "br.reg(br/blr/ret)";
            return "br.system.other";
        }
        if ((hi & 3) == 0)       return "br.uncond_imm(b/bl)";
        if ((hi & 3) == 1)       return bit(insn, 25) ? "br.test(tbz/tbnz)" : "br.compare(cbz/cbnz)";
        return "br.other";
    }

    if ((op0 & 0x5) == 0x4) { /* x1x0: loads and stores */
        const bool simd = bit(insn, 26);
        const std::uint32_t kind = bits(insn, 29, 28);
        if (kind == 0) return simd ? "ldst.simd.multi/single" : "ldst.exclusive/atomic";
        if (kind == 1) return simd ? "ldst.simd.literal" : "ldst.literal";
        if (kind == 2) return simd ? "ldst.simd.pair" : "ldst.pair";
        if (bit(insn, 24)) return simd ? "ldst.simd.unsigned_imm" : "ldst.unsigned_imm";
        if (bit(insn, 21)) return simd ? "ldst.simd.reg_offset" : (bits(insn, 11, 10) == 2 ? "ldst.reg_offset" : "ldst.atomic/pac");
        switch (bits(insn, 11, 10)) {
        case 0:  return simd ? "ldst.simd.unscaled" : "ldst.unscaled";
        case 1:  return simd ? "ldst.simd.post_index" : "ldst.post_index";
        case 2:  return simd ? "ldst.simd.unprivileged" : "ldst.unprivileged";
        default: return simd ? "ldst.simd.pre_index" : "ldst.pre_index";
        }
    }

    if ((op0 & 0x7) == 0x5) { /* x101: data processing — register */
        const std::uint32_t op2 = bits(insn, 24, 21);
        if (bit(insn, 28)) {
            if (op2 == 0x6) return bit(insn, 30) ? "dp.reg.1source(rev/clz)" : "dp.reg.2source(div/shift)";
            if (op2 == 0x0) return "dp.reg.addsub_carry";
            if (op2 == 0x2) return "dp.reg.cond_compare";
            if (op2 == 0x4) return "dp.reg.cond_select";
            if (op2 & 0x8)  return "dp.reg.3source(madd)";
            return "dp.reg.other";
        }
        if ((op2 & 0x8) == 0) return "dp.reg.logical_shifted";
        return bit(insn, 21) ? "dp.reg.addsub_extended" : "dp.reg.addsub_shifted";
    }

    return "simd_fp"; /* x111 */
}

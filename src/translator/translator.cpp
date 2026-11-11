#include "machoman/translator.h"
#include "emit.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <string>
#if defined(__linux__)
#include <sys/uio.h>
#include <unistd.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <mutex>
#include <unordered_map>

namespace mm_emit {

/* Defined below, beside the watch points: the software CRC32 that an
 * AArch64 CRC32B/H/W/X needs, since x86 only has the Castagnoli one. */
extern "C" std::uint32_t mm_crc32_ethernet(std::uint32_t acc, std::uint64_t value, int bytes);

std::int32_t ctx_off(std::uint8_t guest) {
    return static_cast<std::int32_t>(guest) * 8;
}

void load(mm_x64_buf &buf, int host, std::uint8_t guest, reg31 kind) {
    if (guest == 31 && kind == reg31::xzr)
        mm_x64_emit_xor_reg(buf, host, host);
    else
        mm_x64_emit_mov_load(buf, host, CTX, ctx_off(guest));
}

/* 32-bit (w) results are zero-extended on store, so the invariant "a w
 * register's upper half is zero in the context" always holds. */
void store(mm_x64_buf &buf, std::uint8_t guest, int host, bool is64, reg31 kind) {
    if (guest == 31 && kind == reg31::xzr)
        return;
    if (!is64)
        mm_x64_emit_mov_reg32(buf, host, host);
    mm_x64_emit_mov_store(buf, CTX, ctx_off(guest), host);
}

/* Loads rm into B and applies the register-form shift and N-invert. */
void load_rm_operand(mm_x64_buf &buf, const mm_a64_insn &insn) {
    load(buf, B, insn.rm, reg31::xzr);
    if (insn.shift_amount != 0) {
        /* Reading Wm truncates Xm even if its last write was 64-bit.
         * Otherwise an LSR/ROR shifts upper Xm bits into the W result. */
        if (!insn.is64) mm_x64_emit_mov_reg32(buf, B, B);
        switch (insn.shift) {
        case mm_a64_shift::lsl:
            mm_x64_emit_shl_imm(buf, B, insn.shift_amount);
            break;
        case mm_a64_shift::lsr:
            mm_x64_emit_shr_imm(buf, B, insn.shift_amount);
            break;
        case mm_a64_shift::asr:
            if (!insn.is64)
                mm_x64_emit_movsxd(buf, B, B);
            mm_x64_emit_sar_imm(buf, B, insn.shift_amount);
            break;
        case mm_a64_shift::ror:
            if (!insn.is64) {
                /* Duplicate the low half into the high half so a 64-bit
                 * rotate yields the 32-bit rotate in the low half. */
                mm_x64_emit_mov_reg(buf, T, B);
                mm_x64_emit_shl_imm(buf, T, 32);
                mm_x64_emit_or_reg(buf, B, T);
            }
            mm_x64_emit_ror_imm(buf, B, insn.shift_amount);
            break;
        }
    }
    if (insn.invert_rm)
        mm_x64_emit_not_reg(buf, B);
}

/* Leaves the effective address in B. For post-index that is just rn. */
void emit_address(mm_x64_buf &buf, const mm_a64_insn &insn) {
    if (insn.addr == mm_a64_addr::literal) {
        mm_x64_emit_mov_imm(buf, B, static_cast<std::uint64_t>(insn.imm));
        return;
    }
    load(buf, B, insn.rn, reg31::sp);
    switch (insn.addr) {
    case mm_a64_addr::offset:
    case mm_a64_addr::pre_index:
        if (insn.imm != 0)
            mm_x64_emit_add_imm32(buf, B, static_cast<std::int32_t>(insn.imm));
        break;
    case mm_a64_addr::post_index:
    case mm_a64_addr::post_index_reg:
        break;
    case mm_a64_addr::reg_offset:
        load(buf, T, insn.rm, reg31::xzr);
        if (insn.extend == mm_a64_extend::uxtw)
            mm_x64_emit_mov_reg32(buf, T, T);
        else if (insn.extend == mm_a64_extend::sxtw)
            mm_x64_emit_movsxd(buf, T, T);
        if (insn.ext_shift)
            mm_x64_emit_shl_imm(buf, T, insn.ext_shift);
        mm_x64_emit_add_reg(buf, B, T);
        break;
    case mm_a64_addr::literal:
        break;
    }
}

/* Base register update for the indexed forms; B still holds the address
 * computed by emit_address (memory ops only touch A). */
void emit_writeback(mm_x64_buf &buf, const mm_a64_insn &insn) {
    if (insn.addr == mm_a64_addr::post_index && insn.imm != 0)
        mm_x64_emit_add_imm32(buf, B, static_cast<std::int32_t>(insn.imm));
    if (insn.addr == mm_a64_addr::post_index_reg) {
        load(buf, T, insn.rm, reg31::xzr);
        mm_x64_emit_add_reg(buf, B, T);
    }
    if (insn.addr == mm_a64_addr::post_index || insn.addr == mm_a64_addr::pre_index ||
        insn.addr == mm_a64_addr::post_index_reg)
        store(buf, insn.rn, B, true, reg31::sp);
}

mm_x64_load load_kind(const mm_a64_insn &insn) {
    if (!insn.mem_signed) {
        switch (insn.mem_size) {
        case 1: return mm_x64_load::u8;
        case 2: return mm_x64_load::u16;
        case 4: return mm_x64_load::u32;
        default: return mm_x64_load::u64;
        }
    }
    switch (insn.mem_size) {
    case 1: return insn.is64 ? mm_x64_load::s8_64 : mm_x64_load::s8_32;
    case 2: return insn.is64 ? mm_x64_load::s16_64 : mm_x64_load::s16_32;
    default: return mm_x64_load::s32_64;
    }
}

/* Byte offset of half `half` (0 = bits 63:0) of SIMD register n in the context. */
std::int32_t voff(std::uint8_t n, int half) {
    return static_cast<std::int32_t>(offsetof(mm_cpu_context, v) + static_cast<std::size_t>(n) * 16 + static_cast<std::size_t>(half) * 8);
}

void store_v_zero_high(mm_x64_buf &buf, std::uint8_t n) {
    mm_x64_emit_mov_store_imm32(buf, CTX, voff(n, 1), 0);
}

void vload(mm_x64_buf &buf, int xmm, std::uint8_t vreg) {
    auto &cache = buf.vectors;
    if (cache.active) {
        for (unsigned slot = 0; slot < 2; ++slot) {
            if (cache.guest[slot] != vreg) continue;
            mm_x64_emit_sse_rr(buf, mm_x64_sse::movdqa, xmm, 4 + static_cast<int>(slot));
            return;
        }
    }
    mm_x64_emit_movdqu_load(buf, xmm, CTX, voff(vreg, 0));
}

void vstore(mm_x64_buf &buf, std::uint8_t vreg, int xmm, bool q) {
    if (q) {
        mm_x64_emit_movdqu_store(buf, CTX, voff(vreg, 0), xmm);
    } else {
        mm_x64_emit_movq_store(buf, CTX, voff(vreg, 0), xmm);
        store_v_zero_high(buf, vreg);
    }
    auto &cache = buf.vectors;
    if (cache.active) {
        unsigned slot = 0;
        while (slot < 2 && cache.guest[slot] != vreg) ++slot;
        if (slot == 2) {
            slot = cache.guest[0] < 0 ? 0u : cache.guest[1] < 0 ? 1u : cache.replace++ % 2;
        }
        cache.guest[slot] = vreg;
        cache.written |= 1u << vreg;
        const int resident = 4 + static_cast<int>(slot);
        if (q) mm_x64_emit_sse_rr(buf, mm_x64_sse::movdqa, resident, xmm);
        else mm_x64_emit_movq_rr(buf, resident, xmm);
    }
}

void vconst(mm_x64_buf &buf, int xmm, std::uint64_t lo, std::uint64_t hi) {
    mm_x64_emit_mov_imm(buf, A, lo);
    mm_x64_emit_movq_from_gpr(buf, xmm, A);
    if (hi != 0) {
        if (hi != lo) mm_x64_emit_mov_imm(buf, A, hi);
        mm_x64_emit_pinsrq(buf, xmm, A, 1);
    }
}

void emit_load_to(mm_x64_buf &buf, const mm_a64_insn &insn, std::uint8_t rt, std::int32_t disp) {
    if (insn.simd) {
        if (insn.mem_size == 16) {
            mm_x64_emit_movdqu_load(buf, X0, B, disp);
            vstore(buf, rt, X0, true);
        } else {
            mm_x64_emit_load(buf, A, B, disp, unsigned_kind(insn.mem_size));
            mm_x64_emit_mov_store(buf, CTX, voff(rt, 0), A);
            store_v_zero_high(buf, rt);
        }
        return;
    }
    mm_x64_emit_load(buf, A, B, disp, load_kind(insn));
    store(buf, rt, A, true, reg31::xzr); /* value is already extended to 64 bits */
}

void emit_store_from(mm_x64_buf &buf, const mm_a64_insn &insn, std::uint8_t rt, std::int32_t disp) {
    if (insn.simd) {
        if (insn.mem_size == 16) {
            vload(buf, X0, rt);
            mm_x64_emit_movdqu_store(buf, B, disp, X0);
        } else {
            mm_x64_emit_mov_load(buf, A, CTX, voff(rt, 0));
            mm_x64_emit_store(buf, B, disp, A, insn.mem_size);
        }
        return;
    }
    load(buf, A, rt, reg31::xzr);
    mm_x64_emit_store(buf, B, disp, A, insn.mem_size);
}

std::uint64_t replicate_factor(int elem_size) {
    switch (elem_size) {
    case 1: return 0x0101010101010101ull;
    case 2: return 0x0001000100010001ull;
    case 4: return 0x0000000100000001ull;
    default: return 1;
    }
}

/* SIMD ops that are plain 64-bit integer work on the two halves. */
void emit_simd_gpr(mm_x64_buf &buf, const mm_a64_insn &insn) {
    switch (insn.op) {
    case mm_a64_opcode::fmov_to_gpr:
        mm_x64_emit_mov_load(buf, A, CTX, voff(insn.rn, insn.elem_index));
        store(buf, insn.rd, A, insn.is64, reg31::xzr);
        return;
    case mm_a64_opcode::fmov_from_gpr:
        load(buf, A, insn.rn, reg31::xzr);
        if (insn.elem_index == 1) {
            mm_x64_emit_mov_store(buf, CTX, voff(insn.rd, 1), A);
        } else {
            if (insn.elem_size == 4)
                mm_x64_emit_mov_reg32(buf, A, A);
            mm_x64_emit_mov_store(buf, CTX, voff(insn.rd, 0), A);
            store_v_zero_high(buf, insn.rd);
        }
        return;
    case mm_a64_opcode::ins_gen:
        load(buf, A, insn.rn, reg31::xzr);
        mm_x64_emit_store(buf, CTX, voff(insn.rd, 0) + insn.elem_index * insn.elem_size, A, insn.elem_size);
        return;
    case mm_a64_opcode::umov:
        mm_x64_emit_load(buf, A, CTX, voff(insn.rn, 0) + insn.elem_index * insn.elem_size, unsigned_kind(insn.elem_size));
        store(buf, insn.rd, A, true, reg31::xzr);
        return;
    case mm_a64_opcode::smov: {
        const mm_x64_load k = insn.elem_size == 1 ? (insn.is64 ? mm_x64_load::s8_64 : mm_x64_load::s8_32)
                            : insn.elem_size == 2 ? (insn.is64 ? mm_x64_load::s16_64 : mm_x64_load::s16_32)
                                                  : mm_x64_load::s32_64;
        mm_x64_emit_load(buf, A, CTX, voff(insn.rn, 0) + insn.elem_index * insn.elem_size, k);
        store(buf, insn.rd, A, true, reg31::xzr);
        return;
    }
    case mm_a64_opcode::dup_gen:
        load(buf, A, insn.rn, reg31::xzr);
        if (insn.elem_size < 8) {
            mm_x64_emit_extend_reg(buf, A, A, unsigned_kind(insn.elem_size));
            mm_x64_emit_mov_imm(buf, T, replicate_factor(insn.elem_size));
            mm_x64_emit_imul_reg(buf, A, T);
        }
        mm_x64_emit_mov_store(buf, CTX, voff(insn.rd, 0), A);
        if (insn.q) mm_x64_emit_mov_store(buf, CTX, voff(insn.rd, 1), A);
        else store_v_zero_high(buf, insn.rd);
        return;
    case mm_a64_opcode::movi:
        for (int h = 0; h < (insn.q ? 2 : 1); ++h) {
            mm_x64_emit_mov_imm(buf, A, static_cast<std::uint64_t>(insn.imm));
            if (insn.simd_imm_merge) {
                mm_x64_emit_mov_load(buf, B, CTX, voff(insn.rd, h));
                if (insn.raw & (1u << 29)) mm_x64_emit_and_reg(buf, A, B); /* BIC: immediate already inverted */
                else mm_x64_emit_or_reg(buf, A, B);
            }
            mm_x64_emit_mov_store(buf, CTX, voff(insn.rd, h), A);
        }
        if (!insn.q) store_v_zero_high(buf, insn.rd);
        return;
    case mm_a64_opcode::vlogic:
        for (int h = 0; h < (insn.q ? 2 : 1); h++) {
            mm_x64_emit_mov_load(buf, A, CTX, voff(insn.rn, h));
            mm_x64_emit_mov_load(buf, B, CTX, voff(insn.rm, h));
            switch (insn.vlogic) {
            case mm_a64_vlogic::and_: mm_x64_emit_and_reg(buf, A, B); break;
            case mm_a64_vlogic::bic:  mm_x64_emit_not_reg(buf, B); mm_x64_emit_and_reg(buf, A, B); break;
            case mm_a64_vlogic::orr:  mm_x64_emit_or_reg(buf, A, B); break;
            case mm_a64_vlogic::orn:  mm_x64_emit_not_reg(buf, B); mm_x64_emit_or_reg(buf, A, B); break;
            case mm_a64_vlogic::eor:  mm_x64_emit_xor_reg(buf, A, B); break;
            case mm_a64_vlogic::bsl:  /* rd = ((rn ^ rm) & rd) ^ rm */
                mm_x64_emit_mov_load(buf, T, CTX, voff(insn.rd, h));
                mm_x64_emit_xor_reg(buf, A, B); mm_x64_emit_and_reg(buf, A, T); mm_x64_emit_xor_reg(buf, A, B);
                break;
            case mm_a64_vlogic::bit:  /* rd = rd ^ ((rd ^ rn) & rm) */
                mm_x64_emit_mov_load(buf, T, CTX, voff(insn.rd, h));
                mm_x64_emit_xor_reg(buf, A, T); mm_x64_emit_and_reg(buf, A, B); mm_x64_emit_xor_reg(buf, A, T);
                break;
            case mm_a64_vlogic::bif:  /* rd = rd ^ ((rd ^ rn) & ~rm) */
                mm_x64_emit_mov_load(buf, T, CTX, voff(insn.rd, h));
                mm_x64_emit_not_reg(buf, B);
                mm_x64_emit_xor_reg(buf, A, T); mm_x64_emit_and_reg(buf, A, B); mm_x64_emit_xor_reg(buf, A, T);
                break;
            }
            mm_x64_emit_mov_store(buf, CTX, voff(insn.rd, h), A);
        }
        if (!insn.q) store_v_zero_high(buf, insn.rd);
        return;
    default:
        return;
    }
}

mm_x64_load unsigned_kind(int size) {
    return size == 1 ? mm_x64_load::u8 : size == 2 ? mm_x64_load::u16 : size == 4 ? mm_x64_load::u32 : mm_x64_load::u64;
}

namespace {

constexpr std::int32_t OFF_EFLAGS = offsetof(mm_cpu_context, eflags);
constexpr std::int32_t OFF_FAULT_VA = offsetof(mm_cpu_context, fault_va);
constexpr std::int32_t OFF_EXIT_REASON = offsetof(mm_cpu_context, exit_reason);
constexpr std::int32_t OFF_BRANCH_TABLE = offsetof(mm_cpu_context, branch_table);

/* Host EFLAGS bit positions used for the NZCV image. */
constexpr std::uint64_t EFL_CF = 1u << 0, EFL_ZF = 1u << 6, EFL_SF = 1u << 7, EFL_OF = 1u << 11;

enum class flag_src { sub, add, logic };

/* Must directly follow the x86 ALU op whose flags are being captured. */
void save_flags(mm_x64_buf &buf, flag_src src) {
    if (src == flag_src::add)
        mm_x64_emit_cmc(buf);   /* CF = NOT carry */
    else if (src == flag_src::logic)
        mm_x64_emit_stc(buf);   /* C = 0 -> CF = 1 (OF is already 0 = V) */
    mm_x64_emit_pushf(buf);
    mm_x64_emit_pop_mem(buf, CTX, OFF_EFLAGS);
}

void load_flags(mm_x64_buf &buf) {
    mm_x64_emit_push_mem(buf, CTX, OFF_EFLAGS);
    mm_x64_emit_popf(buf);
}

/* A64 condition -> x86 cc under the CF = NOT C convention. */
mm_x64_cc host_cc(std::uint8_t cond) {
    switch (cond & 0xF) {
    case 0:  return mm_x64_cc::e;   /* EQ */
    case 1:  return mm_x64_cc::ne;
    case 2:  return mm_x64_cc::ae;  /* CS: C=1 -> CF=0 */
    case 3:  return mm_x64_cc::b;   /* CC */
    case 4:  return mm_x64_cc::s;   /* MI */
    case 5:  return mm_x64_cc::ns;  /* PL */
    case 6:  return mm_x64_cc::o;   /* VS */
    case 7:  return mm_x64_cc::no;  /* VC */
    case 8:  return mm_x64_cc::a;   /* HI: C && !Z -> CF=0 && ZF=0 */
    case 9:  return mm_x64_cc::be;  /* LS */
    case 10: return mm_x64_cc::ge;
    case 11: return mm_x64_cc::l;
    case 12: return mm_x64_cc::g;
    default: return mm_x64_cc::le;  /* LE; AL/NV handled by callers */
    }
}

mm_x64_cc invert(mm_x64_cc cc) { return static_cast<mm_x64_cc>(static_cast<std::uint8_t>(cc) ^ 1); }

/* Evaluate B.cond from the materialized NZCV image rather than restoring
 * host RFLAGS with POPF. Every guest boundary still has current context
 * flags, including an independent entry into the branch instruction. */
mm_x64_cc test_branch_condition(mm_x64_buf &buf, std::uint8_t cond) {
    if (cond < 10) {
        static constexpr std::uint32_t masks[] = {
            EFL_ZF, EFL_ZF, EFL_CF, EFL_CF, EFL_SF, EFL_SF,
            EFL_OF, EFL_OF, EFL_CF | EFL_ZF, EFL_CF | EFL_ZF,
        };
        static constexpr mm_x64_cc conditions[] = {
            mm_x64_cc::ne, mm_x64_cc::e, mm_x64_cc::e, mm_x64_cc::ne,
            mm_x64_cc::ne, mm_x64_cc::e, mm_x64_cc::ne, mm_x64_cc::e,
            mm_x64_cc::e, mm_x64_cc::ne,
        };
        mm_x64_emit_test_mem_imm32(buf, CTX, OFF_EFLAGS, masks[cond]);
        return conditions[cond];
    }
    // Fold OF into SF: (N != V) at bit 7, retaining Z at bit 6.
    // Mask OF before shifting so unrelated RFLAGS bits cannot affect Z.
    mm_x64_emit_mov_load(buf, A, CTX, OFF_EFLAGS);
    mm_x64_emit_mov_reg(buf, B, A);
    mm_x64_emit_alu_imm32(buf, mm_x64_alu::and_, B, EFL_OF, false);
    mm_x64_emit_shr_imm(buf, B, 4);
    mm_x64_emit_xor_reg(buf, A, B);
    mm_x64_emit_alu_imm32(buf, mm_x64_alu::and_, A, EFL_SF | (cond >= 12 ? EFL_ZF : 0), false);
    return (cond & 1) ? mm_x64_cc::ne : mm_x64_cc::e;
}

std::uint64_t nzcv_to_eflags(std::uint8_t nzcv) {
    std::uint64_t e = 0x2; /* reserved bit 1 is always set */
    if (nzcv & 8) e |= EFL_SF;
    if (nzcv & 4) e |= EFL_ZF;
    if (!(nzcv & 2)) e |= EFL_CF;
    if (nzcv & 1) e |= EFL_OF;
    return e;
}

void alu_reg(mm_x64_buf &buf, const mm_a64_insn &insn, mm_x64_alu op, flag_src src) {
    load(buf, A, insn.rn, reg31::xzr);
    load_rm_operand(buf, insn);
    mm_x64_emit_alu_rr(buf, op, A, B, insn.is64);
    if (insn.sets_flags)
        save_flags(buf, src);
    store(buf, insn.rd, A, insn.is64, reg31::xzr);
}

/* Shared code emitted once at the start of the output; every instruction
 * that leaves the translated text jumps to one of these with the relevant
 * VA in rax. Offsets are fixed for the whole translation. */
struct stubs {
    std::size_t exit = 0, bad_target = 0, unsupported = 0, svc = 0, brk = 0, dispatch = 0;
};

void emit_exit_stub(mm_x64_buf &buf, mm_exit_reason reason, bool with_fault_va) {
    if (with_fault_va)
        mm_x64_emit_mov_store(buf, CTX, OFF_FAULT_VA, A);
    mm_x64_emit_mov_store_imm32(buf, CTX, OFF_EXIT_REASON, static_cast<std::int32_t>(reason));
    mm_x64_emit_ret(buf);
}

stubs emit_stubs(mm_x64_buf &buf, std::uint64_t text_base, std::uint64_t text_size) {
    stubs st;
    st.exit = buf.size();        emit_exit_stub(buf, mm_exit_reason::returned, false);
    st.bad_target = buf.size();  emit_exit_stub(buf, mm_exit_reason::bad_target, true);
    st.unsupported = buf.size(); emit_exit_stub(buf, mm_exit_reason::unsupported, true);
    st.svc = buf.size();         emit_exit_stub(buf, mm_exit_reason::syscall, true);
    st.brk = buf.size();         emit_exit_stub(buf, mm_exit_reason::brk, true);

    /* dispatch: rax = guest VA -> jump to its translation through the
     * process-wide two-level code map (see mm_code_map in translator.h):
     * L1 is indexed by VA >> 14 (16 KiB guest pages), each L2 page holds
     * one host address per 4-byte guest instruction. VAs above 2^40, in
     * pages without code, or misaligned exit with bad_target. */
    (void)text_base; (void)text_size;
    st.dispatch = buf.size();
    mm_x64_emit_mov_reg(buf, B, A);
    mm_x64_emit_shr_imm(buf, B, MM_CODE_MAP_VA_BITS);
    mm_x64_patch_rel32(buf, mm_x64_emit_jcc_rel32(buf, mm_x64_cc::ne), st.bad_target);
    mm_x64_emit_mov_reg(buf, B, A);
    mm_x64_emit_alu_imm32(buf, mm_x64_alu::and_, B, 3, true);
    mm_x64_patch_rel32(buf, mm_x64_emit_jcc_rel32(buf, mm_x64_cc::ne), st.bad_target);
    mm_x64_emit_mov_reg(buf, B, A);
    mm_x64_emit_shr_imm(buf, B, MM_CODE_MAP_PAGE_BITS);
    mm_x64_emit_mov_load(buf, T, CTX, OFF_BRANCH_TABLE);
    mm_x64_emit_mov_load_indexed(buf, B, T, B, 8); /* B = l1[page] */
    mm_x64_emit_test_rr(buf, B, B, true);
    mm_x64_patch_rel32(buf, mm_x64_emit_jcc_rel32(buf, mm_x64_cc::e), st.bad_target);
    mm_x64_emit_mov_reg(buf, T, A);
    mm_x64_emit_alu_imm32(buf, mm_x64_alu::and_, T, (1 << MM_CODE_MAP_PAGE_BITS) - 4, true);
    mm_x64_emit_mov_load_indexed(buf, B, B, T, 2); /* 8 bytes per 4-byte insn: (va & 0x3ffc) * 2 */
    mm_x64_emit_test_rr(buf, B, B, true);
    mm_x64_patch_rel32(buf, mm_x64_emit_jcc_rel32(buf, mm_x64_cc::e), st.bad_target);
    mm_x64_emit_jmp_reg(buf, B);
    return st;
}

struct fixup {
    std::size_t   rel32_at;
    std::uint64_t target_va;
};

struct emit_ctx {
    mm_x64_buf &buf;
    const stubs &st;
    std::uint64_t text_base, text_size, pc;
    std::vector<fixup> &fixups;

    bool in_text(std::uint64_t va) const { return va >= text_base && va < text_base + text_size && (va & 3) == 0; }

    /* jmp/jcc to a guest VA: rel32 into the text, or through the dispatcher. */
    void jump_to(std::uint64_t va, mm_x64_cc cc, bool conditional) {
        if (in_text(va)) {
            fixups.push_back({conditional ? mm_x64_emit_jcc_rel32(buf, cc) : mm_x64_emit_jmp_rel32(buf), va});
            return;
        }
        std::size_t skip = 0;
        if (conditional)
            skip = mm_x64_emit_jcc_rel8(buf, invert(cc));
        mm_x64_emit_mov_imm(buf, A, va);
        mm_x64_patch_rel32(buf, mm_x64_emit_jmp_rel32(buf), st.dispatch);
        if (conditional)
            mm_x64_patch_rel8_to_here(buf, skip);
    }

    void exit_via(std::size_t stub) {
        mm_x64_emit_mov_imm(buf, A, pc);
        mm_x64_patch_rel32(buf, mm_x64_emit_jmp_rel32(buf), stub);
    }

    void set_link_register() {
        mm_x64_emit_mov_imm(buf, A, pc + 4);
        store(buf, 30, A, true, reg31::xzr);
    }
};

void emit_extend(mm_x64_buf &buf, int reg, mm_a64_extend ext) {
    switch (ext) {
    case mm_a64_extend::uxtb: mm_x64_emit_extend_reg(buf, reg, reg, mm_x64_load::u8); break;
    case mm_a64_extend::uxth: mm_x64_emit_extend_reg(buf, reg, reg, mm_x64_load::u16); break;
    case mm_a64_extend::uxtw: mm_x64_emit_extend_reg(buf, reg, reg, mm_x64_load::u32); break;
    case mm_a64_extend::uxtx: break;
    case mm_a64_extend::sxtb: mm_x64_emit_extend_reg(buf, reg, reg, mm_x64_load::s8_64); break;
    case mm_a64_extend::sxth: mm_x64_emit_extend_reg(buf, reg, reg, mm_x64_load::s16_64); break;
    case mm_a64_extend::sxtw: mm_x64_emit_extend_reg(buf, reg, reg, mm_x64_load::s32_64); break;
    case mm_a64_extend::sxtx: break;
    }
}

std::uint64_t ones(unsigned width) {
    return width >= 64 ? ~0ull : (1ull << width) - 1;
}

void and_imm64(mm_x64_buf &buf, int reg, std::uint64_t mask) {
    mm_x64_emit_mov_imm(buf, T, mask);
    mm_x64_emit_and_reg(buf, reg, T);
}

/* SBFM/BFM/UBFM per the ARM ARM: imms >= immr extracts a field starting
 * at bit immr; imms < immr inserts the low imms+1 bits at regsize-immr. */
void emit_bitfield(mm_x64_buf &buf, const mm_a64_insn &insn) {
    const unsigned regsize = insn.is64 ? 64 : 32;
    const unsigned immr = insn.immr, imms = insn.imms;

    load(buf, A, insn.rn, reg31::xzr);
    if (imms >= immr) {
        const unsigned width = imms - immr + 1;
        if (insn.op == mm_a64_opcode::sbfm) {
            /* shl so bit imms lands at 63, then sar back past immr */
            if (63 - imms) mm_x64_emit_shl_imm(buf, A, static_cast<std::uint8_t>(63 - imms));
            if (63 - imms + immr) mm_x64_emit_sar_imm(buf, A, static_cast<std::uint8_t>(63 - imms + immr));
        } else {
            if (immr) mm_x64_emit_shr_imm(buf, A, static_cast<std::uint8_t>(immr));
            and_imm64(buf, A, ones(width));
            if (insn.op == mm_a64_opcode::bfm) {
                mm_x64_emit_mov_reg(buf, B, A);
                load(buf, A, insn.rd, reg31::xzr);
                and_imm64(buf, A, ~ones(width));
                mm_x64_emit_or_reg(buf, A, B);
            }
        }
    } else {
        const unsigned width = imms + 1;
        const unsigned lsb = regsize - immr;
        if (insn.op == mm_a64_opcode::sbfm) {
            mm_x64_emit_shl_imm(buf, A, static_cast<std::uint8_t>(63 - imms));
            mm_x64_emit_sar_imm(buf, A, static_cast<std::uint8_t>(63 - imms));
        } else {
            and_imm64(buf, A, ones(width));
        }
        mm_x64_emit_shl_imm(buf, A, static_cast<std::uint8_t>(lsb));
        if (insn.op == mm_a64_opcode::bfm) {
            mm_x64_emit_mov_reg(buf, B, A);
            load(buf, A, insn.rd, reg31::xzr);
            and_imm64(buf, A, ~(ones(width) << lsb));
            mm_x64_emit_or_reg(buf, A, B);
        }
    }
    store(buf, insn.rd, A, insn.is64, reg31::xzr);
}

void emit_madd(mm_x64_buf &buf, const mm_a64_insn &insn) {
    load(buf, A, insn.rn, reg31::xzr);
    load(buf, B, insn.rm, reg31::xzr);
    switch (insn.mulk) {
    case mm_a64_mul::full:
        mm_x64_emit_imul_reg(buf, A, B);
        break;
    case mm_a64_mul::smull:
        mm_x64_emit_movsxd(buf, A, A);
        mm_x64_emit_movsxd(buf, B, B);
        mm_x64_emit_imul_reg(buf, A, B);
        break;
    case mm_a64_mul::umull:
        mm_x64_emit_mov_reg32(buf, A, A);
        mm_x64_emit_mov_reg32(buf, B, B);
        mm_x64_emit_imul_reg(buf, A, B); /* 32x32 -> 64 never overflows the signed product */
        break;
    case mm_a64_mul::smulh:
        mm_x64_emit_imul_one(buf, B);    /* rdx:rax = rax * rcx */
        mm_x64_emit_mov_reg(buf, A, T);
        break;
    case mm_a64_mul::umulh:
        mm_x64_emit_mul_one(buf, B);
        mm_x64_emit_mov_reg(buf, A, T);
        break;
    }
    if (insn.ra != 31) {
        load(buf, B, insn.ra, reg31::xzr);
        if (insn.negate_product) {
            mm_x64_emit_sub_reg(buf, B, A);
            mm_x64_emit_mov_reg(buf, A, B);
        } else {
            mm_x64_emit_add_reg(buf, A, B);
        }
    } else if (insn.negate_product) {
        mm_x64_emit_neg_reg(buf, A);
    }
    store(buf, insn.rd, A, insn.is64, reg31::xzr);
}

/* AArch64 division never traps: x/0 = 0 and INT_MIN/-1 = INT_MIN, both of
 * which #DE on x86, so they are special-cased with short branches. */
void emit_div(mm_x64_buf &buf, const mm_a64_insn &insn, bool is_signed) {
    load(buf, A, insn.rn, reg31::xzr);
    load(buf, B, insn.rm, reg31::xzr);
    if (is_signed && !insn.is64) {
        mm_x64_emit_movsxd(buf, A, A);
        mm_x64_emit_movsxd(buf, B, B);
    }
    mm_x64_emit_test_reg(buf, B, B);
    const std::size_t jz = mm_x64_emit_jcc_rel8(buf, mm_x64_cc::e);
    std::size_t jmp_done_from_neg = 0;
    bool has_neg_path = false;
    if (is_signed) {
        if (insn.is64) {
            /* rcx == -1: result is -rax (also covers INT64_MIN without #DE) */
            mm_x64_emit_cmp_imm32(buf, B, -1);
            const std::size_t jne = mm_x64_emit_jcc_rel8(buf, mm_x64_cc::ne);
            mm_x64_emit_neg_reg(buf, A);
            jmp_done_from_neg = mm_x64_emit_jmp_rel8(buf);
            has_neg_path = true;
            mm_x64_patch_rel8_to_here(buf, jne);
        }
        mm_x64_emit_cqo(buf);
        mm_x64_emit_idiv_one(buf, B);
    } else {
        mm_x64_emit_xor_reg(buf, T, T);
        mm_x64_emit_div_one(buf, B);
    }
    const std::size_t jmp_done = mm_x64_emit_jmp_rel8(buf);
    mm_x64_patch_rel8_to_here(buf, jz);
    mm_x64_emit_xor_reg(buf, A, A);
    mm_x64_patch_rel8_to_here(buf, jmp_done);
    if (has_neg_path)
        mm_x64_patch_rel8_to_here(buf, jmp_done_from_neg);
    store(buf, insn.rd, A, insn.is64, reg31::xzr);
}

void emit_shift_var(mm_x64_buf &buf, const mm_a64_insn &insn, void (*op)(mm_x64_buf &, int, bool)) {
    load(buf, A, insn.rn, reg31::xzr);
    load(buf, B, insn.rm, reg31::xzr); /* B is rcx: the count register */
    op(buf, A, !insn.is64);
    store(buf, insn.rd, A, insn.is64, reg31::xzr);
}

constexpr std::int32_t OFF_EXCL_ADDR = offsetof(mm_cpu_context, excl_addr);
constexpr std::int32_t OFF_EXCL_VALUE = offsetof(mm_cpu_context, excl_value);


/* Zero-extends `reg` to the guest access size after a sub-64-bit RMW,
 * whose result only replaced the low byte/word of the register. */
void zero_extend_to(mm_x64_buf &buf, int reg, int size) {
    if (size < 8)
        mm_x64_emit_extend_reg(buf, reg, reg, unsigned_kind(size));
}

/* x86 is TSO, so acquire loads and release stores need no fences; a
 * release store is done with xchg (implicitly locked, a full barrier) to
 * also cover the stlr;ldar ordering AArch64 guarantees. Exclusives use a
 * per-thread monitor in the context plus lock cmpxchg, so an stxr only
 * succeeds if the location still holds the value ldxr saw. */
void emit_atomic(mm_x64_buf &buf, const mm_a64_insn &insn) {
    const int size = insn.mem_size;
    load(buf, B, insn.rn, reg31::sp);
    switch (insn.op) {
    case mm_a64_opcode::ldar:
        mm_x64_emit_load(buf, A, B, 0, unsigned_kind(size));
        store(buf, insn.rd, A, true, reg31::xzr);
        return;
    case mm_a64_opcode::ldxr:
        mm_x64_emit_load(buf, A, B, 0, unsigned_kind(size));
        mm_x64_emit_mov_store(buf, CTX, OFF_EXCL_ADDR, B);
        mm_x64_emit_mov_store(buf, CTX, OFF_EXCL_VALUE, A);
        store(buf, insn.rd, A, true, reg31::xzr);
        return;
    case mm_a64_opcode::stlr:
        load(buf, A, insn.rd, reg31::xzr);
        mm_x64_emit_rmw(buf, mm_x64_rmw::xchg, B, A, size);
        return;
    case mm_a64_opcode::stxr: {
        /* status = 1 unless the monitor matches and the cmpxchg succeeds */
        load(buf, T, insn.rd, reg31::xzr);
        mm_x64_emit_mov_load(buf, A, CTX, OFF_EXCL_ADDR);
        mm_x64_emit_cmp_reg(buf, A, B);
        const std::size_t no_monitor = mm_x64_emit_jcc_rel8(buf, mm_x64_cc::ne);
        mm_x64_emit_mov_load(buf, A, CTX, OFF_EXCL_VALUE);
        mm_x64_emit_rmw(buf, mm_x64_rmw::cmpxchg, B, T, size);
        mm_x64_emit_setcc(buf, mm_x64_cc::ne, A);
        mm_x64_emit_extend_reg(buf, A, A, mm_x64_load::u8);
        const std::size_t done = mm_x64_emit_jmp_rel8(buf);
        mm_x64_patch_rel8_to_here(buf, no_monitor);
        mm_x64_emit_mov_imm(buf, A, 1);
        mm_x64_patch_rel8_to_here(buf, done);
        mm_x64_emit_mov_imm(buf, T, 0);
        mm_x64_emit_mov_store(buf, CTX, OFF_EXCL_ADDR, T); /* clear the monitor */
        store(buf, insn.rt2, A, false, reg31::xzr);
        return;
    }
    case mm_a64_opcode::cas:
        load(buf, A, insn.rt2, reg31::xzr); /* expected */
        load(buf, T, insn.rd, reg31::xzr);  /* new */
        mm_x64_emit_rmw(buf, mm_x64_rmw::cmpxchg, B, T, size);
        zero_extend_to(buf, A, size);
        store(buf, insn.rt2, A, true, reg31::xzr);
        return;
    case mm_a64_opcode::casp: {
        /* cmpxchg8b/16b is exactly CASP: rdx:rax hold the expected pair,
         * rcx:rbx the new one, and the memory value lands in rdx:rax on
         * failure. rbx is callee-saved for the host, so it is borrowed
         * around the instruction. */
        const bool wide = insn.mem_size == 8;
        /* rbx and rsi are not part of the scratch set, so they are saved
         * around the one instruction that needs five registers. */
        mm_x64_emit_push(buf, MM_RBX);
        mm_x64_emit_push(buf, MM_RSI);
        load(buf, MM_RBX, insn.rd, reg31::xzr);                                    /* new low */
        load(buf, B, static_cast<std::uint8_t>((insn.rd + 1) & 31), reg31::xzr);   /* new high */
        load(buf, A, insn.rt2, reg31::xzr);                                        /* expected low */
        load(buf, T, static_cast<std::uint8_t>((insn.rt2 + 1) & 31), reg31::xzr);  /* expected high */
        load(buf, MM_RSI, insn.rn, reg31::sp);                                     /* address */
        mm_x64_emit_cmpxchg_pair(buf, MM_RSI, wide);
        if (!wide) { mm_x64_emit_mov_reg32(buf, A, A); mm_x64_emit_mov_reg32(buf, T, T); }
        store(buf, insn.rt2, A, true, reg31::xzr);
        store(buf, static_cast<std::uint8_t>((insn.rt2 + 1) & 31), T, true, reg31::xzr);
        mm_x64_emit_pop(buf, MM_RSI);
        mm_x64_emit_pop(buf, MM_RBX);
        return;
    }
    case mm_a64_opcode::atomic:
        break;
    default:
        return;
    }

    switch (insn.atomic) {
    case mm_a64_atomic::swp:
        load(buf, A, insn.rt2, reg31::xzr);
        mm_x64_emit_rmw(buf, mm_x64_rmw::xchg, B, A, size);
        zero_extend_to(buf, A, size);
        store(buf, insn.rd, A, true, reg31::xzr);
        return;
    case mm_a64_atomic::add:
        load(buf, A, insn.rt2, reg31::xzr);
        mm_x64_emit_rmw(buf, mm_x64_rmw::xadd, B, A, size);
        zero_extend_to(buf, A, size);
        store(buf, insn.rd, A, true, reg31::xzr);
        return;
    default:
        break;
    }

    /* Generic: old = [B]; loop { new = f(old, rs); cmpxchg } */
    mm_x64_emit_load(buf, A, B, 0, unsigned_kind(size));
    const std::size_t loop = buf.size();
    const bool is_signed = insn.atomic == mm_a64_atomic::smax || insn.atomic == mm_a64_atomic::smin;
    if (is_signed && size < 8)
        mm_x64_emit_extend_reg(buf, A, A, size == 1 ? mm_x64_load::s8_64 : size == 2 ? mm_x64_load::s16_64 : mm_x64_load::s32_64);
    load(buf, T, insn.rt2, reg31::xzr);
    if (is_signed && size < 8)
        mm_x64_emit_extend_reg(buf, T, T, size == 1 ? mm_x64_load::s8_64 : size == 2 ? mm_x64_load::s16_64 : mm_x64_load::s32_64);
    else if (size < 8)
        mm_x64_emit_extend_reg(buf, T, T, unsigned_kind(size));
    switch (insn.atomic) {
    case mm_a64_atomic::clr:  mm_x64_emit_not_reg(buf, T); mm_x64_emit_and_reg(buf, T, A); break;
    case mm_a64_atomic::eor:  mm_x64_emit_xor_reg(buf, T, A); break;
    case mm_a64_atomic::set:  mm_x64_emit_or_reg(buf, T, A); break;
    case mm_a64_atomic::smax: mm_x64_emit_cmp_reg(buf, T, A); mm_x64_emit_cmovcc(buf, mm_x64_cc::l, T, A); break;
    case mm_a64_atomic::smin: mm_x64_emit_cmp_reg(buf, T, A); mm_x64_emit_cmovcc(buf, mm_x64_cc::g, T, A); break;
    case mm_a64_atomic::umax: mm_x64_emit_cmp_reg(buf, T, A); mm_x64_emit_cmovcc(buf, mm_x64_cc::b, T, A); break;
    case mm_a64_atomic::umin: mm_x64_emit_cmp_reg(buf, T, A); mm_x64_emit_cmovcc(buf, mm_x64_cc::a, T, A); break;
    default: break;
    }
    mm_x64_emit_rmw(buf, mm_x64_rmw::cmpxchg, B, T, size);
    mm_x64_patch_rel8_to(buf, mm_x64_emit_jcc_rel8(buf, mm_x64_cc::ne), loop);
    zero_extend_to(buf, A, size);
    store(buf, insn.rd, A, true, reg31::xzr);
}

/* Returns false for opcodes with no host encoding yet. */
bool translate_one(emit_ctx &e, const mm_a64_insn &insn) {
    mm_x64_buf &buf = e.buf;
    const std::uint64_t mask = insn.is64 ? ~0ull : 0xFFFFFFFFull;

    switch (insn.op) {
    case mm_a64_opcode::movz:
        mm_x64_emit_mov_imm(buf, A, static_cast<std::uint64_t>(insn.imm));
        store(buf, insn.rd, A, insn.is64, reg31::xzr);
        return true;
    case mm_a64_opcode::movn:
        mm_x64_emit_mov_imm(buf, A, ~static_cast<std::uint64_t>(insn.imm) & mask);
        store(buf, insn.rd, A, insn.is64, reg31::xzr);
        return true;
    case mm_a64_opcode::movk:
        load(buf, A, insn.rd, reg31::xzr);
        mm_x64_emit_mov_imm(buf, B, ~(0xFFFFull << insn.shift_amount) & mask);
        mm_x64_emit_and_reg(buf, A, B);
        mm_x64_emit_mov_imm(buf, B, static_cast<std::uint64_t>(insn.imm));
        mm_x64_emit_or_reg(buf, A, B);
        store(buf, insn.rd, A, insn.is64, reg31::xzr);
        return true;

    case mm_a64_opcode::add_imm:
    case mm_a64_opcode::sub_imm: {
        /* rn is SP-context; rd is SP-context unless flags are set (cmp/cmn). */
        load(buf, A, insn.rn, reg31::sp);
        const bool is_add = insn.op == mm_a64_opcode::add_imm;
        mm_x64_emit_alu_imm32(buf, is_add ? mm_x64_alu::add : mm_x64_alu::sub, A,
                              static_cast<std::int32_t>(insn.imm), insn.is64);
        if (insn.sets_flags)
            save_flags(buf, is_add ? flag_src::add : flag_src::sub);
        store(buf, insn.rd, A, insn.is64, insn.sets_flags ? reg31::xzr : reg31::sp);
        return true;
    }

    case mm_a64_opcode::add_reg: alu_reg(buf, insn, mm_x64_alu::add, flag_src::add); return true;
    case mm_a64_opcode::sub_reg: alu_reg(buf, insn, mm_x64_alu::sub, flag_src::sub); return true;
    case mm_a64_opcode::and_reg: alu_reg(buf, insn, mm_x64_alu::and_, flag_src::logic); return true;
    case mm_a64_opcode::orr_reg: alu_reg(buf, insn, mm_x64_alu::or_, flag_src::logic); return true;
    case mm_a64_opcode::eor_reg: alu_reg(buf, insn, mm_x64_alu::xor_, flag_src::logic); return true;

    case mm_a64_opcode::add_ext:
    case mm_a64_opcode::sub_ext:
        /* rn and rd are SP-context here (this is how `add x0, sp, w1, uxtw` is spelled). */
        load(buf, A, insn.rn, reg31::sp);
        load(buf, B, insn.rm, reg31::xzr);
        emit_extend(buf, B, insn.extend);
        if (insn.ext_shift)
            mm_x64_emit_shl_imm(buf, B, insn.ext_shift);
        {
            const bool is_add = insn.op == mm_a64_opcode::add_ext;
            mm_x64_emit_alu_rr(buf, is_add ? mm_x64_alu::add : mm_x64_alu::sub, A, B, insn.is64);
            if (insn.sets_flags)
                save_flags(buf, is_add ? flag_src::add : flag_src::sub);
        }
        store(buf, insn.rd, A, insn.is64, insn.sets_flags ? reg31::xzr : reg31::sp);
        return true;

    case mm_a64_opcode::and_imm:
    case mm_a64_opcode::orr_imm:
    case mm_a64_opcode::eor_imm:
        load(buf, A, insn.rn, reg31::xzr);
        mm_x64_emit_mov_imm(buf, B, static_cast<std::uint64_t>(insn.imm));
        mm_x64_emit_alu_rr(buf, insn.op == mm_a64_opcode::and_imm ? mm_x64_alu::and_
                              : insn.op == mm_a64_opcode::orr_imm ? mm_x64_alu::or_ : mm_x64_alu::xor_,
                           A, B, insn.is64);
        if (insn.sets_flags)
            save_flags(buf, flag_src::logic);
        store(buf, insn.rd, A, insn.is64, insn.sets_flags ? reg31::xzr : reg31::sp);
        return true;

    case mm_a64_opcode::csel:
    case mm_a64_opcode::csinc:
    case mm_a64_opcode::csinv:
    case mm_a64_opcode::csneg:
        load(buf, A, insn.rm, reg31::xzr);
        if (insn.op == mm_a64_opcode::csinc)      mm_x64_emit_add_imm32(buf, A, 1);
        else if (insn.op == mm_a64_opcode::csinv) mm_x64_emit_not_reg(buf, A);
        else if (insn.op == mm_a64_opcode::csneg) mm_x64_emit_neg_reg(buf, A);
        load(buf, B, insn.rn, reg31::xzr);
        if (insn.cond >= 14) {
            mm_x64_emit_mov_reg(buf, A, B); /* AL/NV: always rn */
        } else {
            load_flags(buf);
            mm_x64_emit_cmovcc(buf, host_cc(insn.cond), A, B);
        }
        store(buf, insn.rd, A, insn.is64, reg31::xzr);
        return true;

    case mm_a64_opcode::ccmp: {
        std::size_t to_else = 0;
        if (insn.cond < 14) {
            load_flags(buf);
            to_else = mm_x64_emit_jcc_rel8(buf, invert(host_cc(insn.cond)));
        }
        load(buf, A, insn.rn, reg31::xzr);
        if (insn.ccmp_imm)
            mm_x64_emit_mov_imm(buf, B, static_cast<std::uint64_t>(insn.imm));
        else
            load(buf, B, insn.rm, reg31::xzr);
        mm_x64_emit_alu_rr(buf, insn.ccmn ? mm_x64_alu::add : mm_x64_alu::cmp, A, B, insn.is64);
        save_flags(buf, insn.ccmn ? flag_src::add : flag_src::sub);
        if (insn.cond < 14) {
            const std::size_t to_end = mm_x64_emit_jmp_rel8(buf);
            mm_x64_patch_rel8_to_here(buf, to_else);
            mm_x64_emit_mov_store_imm32(buf, CTX, OFF_EFLAGS, static_cast<std::int32_t>(nzcv_to_eflags(insn.nzcv)));
            mm_x64_patch_rel8_to_here(buf, to_end);
        }
        return true;
    }

    case mm_a64_opcode::adc:
    case mm_a64_opcode::sbc:
        /* With CF = NOT C: rn + rm + C == rn - ~rm - CF, and rn - rm - !C == rn - rm - CF. */
        load(buf, A, insn.rn, reg31::xzr);
        load(buf, B, insn.rm, reg31::xzr);
        if (insn.op == mm_a64_opcode::adc)
            mm_x64_emit_not_reg(buf, B);
        load_flags(buf);
        mm_x64_emit_alu_rr(buf, mm_x64_alu::sbb, A, B, insn.is64);
        if (insn.sets_flags)
            save_flags(buf, flag_src::sub); /* borrow out of the sbb is exactly NOT C */
        store(buf, insn.rd, A, insn.is64, reg31::xzr);
        return true;

    case mm_a64_opcode::madd: emit_madd(buf, insn); return true;
    case mm_a64_opcode::udiv: emit_div(buf, insn, false); return true;
    case mm_a64_opcode::sdiv: emit_div(buf, insn, true); return true;
    case mm_a64_opcode::crc32c: {
        const int size = static_cast<int>(insn.imm);
        load(buf, A, insn.rn, reg31::xzr);      /* the accumulator: 32-bit */
        load(buf, B, insn.rm, reg31::xzr);      /* the data */
        mm_x64_emit_crc32(buf, A, B, size);
        store(buf, insn.rd, A, false, reg31::xzr);
        return true;
    }
    case mm_a64_opcode::crc32: {
        const int size = static_cast<int>(insn.imm);
        load(buf, MM_RDI, insn.rn, reg31::xzr);
        load(buf, MM_RSI, insn.rm, reg31::xzr);
        mm_x64_emit_push_reg(buf, MM_RBP);
        mm_x64_emit_mov_reg(buf, MM_RBP, MM_RSP);
        mm_x64_emit_alu_imm32(buf, mm_x64_alu::and_, MM_RSP, -16, true);
        mm_x64_emit_mov_imm(buf, MM_RDX, static_cast<std::uint64_t>(size));
        mm_x64_emit_host_address(buf, MM_RAX, reinterpret_cast<std::uintptr_t>(&mm_crc32_ethernet));
        mm_x64_emit_call_reg(buf, MM_RAX);
        mm_x64_emit_mov_reg(buf, MM_RSP, MM_RBP);
        mm_x64_emit_pop_reg(buf, MM_RBP);
        store(buf, insn.rd, MM_RAX, false, reg31::xzr);
        return true;
    }

    case mm_a64_opcode::lslv: emit_shift_var(buf, insn, mm_x64_emit_shl_cl); return true;
    case mm_a64_opcode::lsrv: emit_shift_var(buf, insn, mm_x64_emit_shr_cl); return true;
    case mm_a64_opcode::asrv: emit_shift_var(buf, insn, mm_x64_emit_sar_cl); return true;
    case mm_a64_opcode::rorv: emit_shift_var(buf, insn, mm_x64_emit_ror_cl); return true;

    case mm_a64_opcode::rev:
        load(buf, A, insn.rn, reg31::xzr);
        mm_x64_emit_bswap(buf, A, insn.is64);
        store(buf, insn.rd, A, insn.is64, reg31::xzr);
        return true;
    case mm_a64_opcode::clz:
        load(buf, A, insn.rn, reg31::xzr);
        mm_x64_emit_lzcnt(buf, A, A, insn.is64);
        store(buf, insn.rd, A, insn.is64, reg31::xzr);
        return true;
    case mm_a64_opcode::cls:
        /* leading sign bits = clz(x ^ (x >> 1)) - 1 */
        load(buf, A, insn.rn, reg31::xzr);
        if (!insn.is64) mm_x64_emit_movsxd(buf, A, A);
        mm_x64_emit_mov_reg(buf, B, A);
        mm_x64_emit_sar_imm(buf, B, 1);
        mm_x64_emit_xor_reg(buf, A, B);
        if (!insn.is64) mm_x64_emit_mov_reg32(buf, A, A);
        mm_x64_emit_lzcnt(buf, A, A, insn.is64);
        mm_x64_emit_sub_imm32(buf, A, 1);
        store(buf, insn.rd, A, insn.is64, reg31::xzr);
        return true;
    case mm_a64_opcode::rev16:
        /* swap bytes within each 16-bit lane: ((x & 0x00ff..) << 8) | ((x >> 8) & 0x00ff..) */
        load(buf, A, insn.rn, reg31::xzr);
        mm_x64_emit_mov_reg(buf, B, A);
        mm_x64_emit_shl_imm(buf, A, 8);
        mm_x64_emit_shr_imm(buf, B, 8);
        mm_x64_emit_mov_imm(buf, T, 0xFF00FF00FF00FF00ull);
        mm_x64_emit_and_reg(buf, A, T);
        mm_x64_emit_mov_imm(buf, T, 0x00FF00FF00FF00FFull);
        mm_x64_emit_and_reg(buf, B, T);
        mm_x64_emit_or_reg(buf, A, B);
        store(buf, insn.rd, A, insn.is64, reg31::xzr);
        return true;
    case mm_a64_opcode::rev32:
        /* bswap each 32-bit half: bswap64 then rotate halves */
        load(buf, A, insn.rn, reg31::xzr);
        mm_x64_emit_bswap(buf, A, true);
        mm_x64_emit_ror_imm(buf, A, 32);
        store(buf, insn.rd, A, true, reg31::xzr);
        return true;
    case mm_a64_opcode::rbit: {
        /* bit reverse via bswap + nibble/2-bit/1-bit swaps */
        load(buf, A, insn.rn, reg31::xzr);
        mm_x64_emit_bswap(buf, A, true);
        const std::uint64_t masks[3][2] = {{0xF0F0F0F0F0F0F0F0ull, 4}, {0xCCCCCCCCCCCCCCCCull, 2}, {0xAAAAAAAAAAAAAAAAull, 1}};
        for (const auto &[mask, sh] : masks) {
            mm_x64_emit_mov_reg(buf, B, A);
            mm_x64_emit_mov_imm(buf, T, mask);
            mm_x64_emit_and_reg(buf, B, T);               /* high bits of each pair */
            mm_x64_emit_shr_imm(buf, B, static_cast<std::uint8_t>(sh));
            mm_x64_emit_not_reg(buf, T);
            mm_x64_emit_and_reg(buf, A, T);               /* low bits */
            mm_x64_emit_shl_imm(buf, A, static_cast<std::uint8_t>(sh));
            mm_x64_emit_or_reg(buf, A, B);
        }
        if (!insn.is64) mm_x64_emit_shr_imm(buf, A, 32);
        store(buf, insn.rd, A, insn.is64, reg31::xzr);
        return true;
    }
    case mm_a64_opcode::extr:
        load(buf, A, insn.rm, reg31::xzr);
        load(buf, T, insn.rn, reg31::xzr);
        if (insn.imms)
            mm_x64_emit_shrd_imm(buf, A, T, insn.imms, insn.is64);
        store(buf, insn.rd, A, insn.is64, reg31::xzr);
        return true;
    case mm_a64_opcode::barrier:
        mm_x64_emit_mfence(buf);
        return true;
    case mm_a64_opcode::mrs:
        switch (insn.sysreg) {
        case mm_a64_sysreg::nzcv:
            /* eflags image -> N:Z:C:V at bits 31..28 (C = NOT CF) */
            mm_x64_emit_mov_load(buf, A, CTX, OFF_EFLAGS);
            mm_x64_emit_xor_reg(buf, B, B);
            mm_x64_emit_bt_imm(buf, A, 7);  mm_x64_emit_setcc(buf, mm_x64_cc::b, B);  mm_x64_emit_shl_imm(buf, B, 31); mm_x64_emit_mov_reg(buf, T, B);
            mm_x64_emit_xor_reg(buf, B, B);
            mm_x64_emit_bt_imm(buf, A, 6);  mm_x64_emit_setcc(buf, mm_x64_cc::b, B);  mm_x64_emit_shl_imm(buf, B, 30); mm_x64_emit_or_reg(buf, T, B);
            mm_x64_emit_xor_reg(buf, B, B);
            mm_x64_emit_bt_imm(buf, A, 0);  mm_x64_emit_setcc(buf, mm_x64_cc::ae, B); mm_x64_emit_shl_imm(buf, B, 29); mm_x64_emit_or_reg(buf, T, B);
            mm_x64_emit_xor_reg(buf, B, B);
            mm_x64_emit_bt_imm(buf, A, 11); mm_x64_emit_setcc(buf, mm_x64_cc::b, B);  mm_x64_emit_shl_imm(buf, B, 28); mm_x64_emit_or_reg(buf, T, B);
            mm_x64_emit_mov_reg(buf, A, T);
            break;
        case mm_a64_sysreg::fpcr:        mm_x64_emit_mov_load(buf, A, CTX, offsetof(mm_cpu_context, fpcr)); break;
        case mm_a64_sysreg::fpsr:        mm_x64_emit_mov_load(buf, A, CTX, offsetof(mm_cpu_context, fpsr)); break;
        case mm_a64_sysreg::tpidr_el0:   mm_x64_emit_mov_load(buf, A, CTX, offsetof(mm_cpu_context, tpidr_el0)); break;
        case mm_a64_sysreg::tpidrro_el0: mm_x64_emit_mov_load(buf, A, CTX, offsetof(mm_cpu_context, tpidrro_el0)); break;
        case mm_a64_sysreg::cntvct_el0:
            mm_x64_emit_rdtsc(buf);
            mm_x64_emit_shl_imm(buf, T, 32);
            mm_x64_emit_mov_reg32(buf, A, A);
            mm_x64_emit_or_reg(buf, A, T);
            break;
        case mm_a64_sysreg::cntfrq_el0:  mm_x64_emit_mov_imm(buf, A, 24000000); break; /* Apple silicon: 24 MHz */
        case mm_a64_sysreg::dczid_el0:   mm_x64_emit_mov_imm(buf, A, 4); break;        /* 64-byte DC ZVA blocks */
        case mm_a64_sysreg::ctr_el0:     mm_x64_emit_mov_imm(buf, A, 0x8444C004); break;
        case mm_a64_sysreg::unknown:     return false;
        }
        store(buf, insn.rd, A, true, reg31::xzr);
        return true;
    case mm_a64_opcode::msr:
        load(buf, A, insn.rd, reg31::xzr);
        switch (insn.sysreg) {
        case mm_a64_sysreg::nzcv:
            /* N:Z:C:V bits 31..28 -> eflags image (SF, ZF, !CF, OF) */
            mm_x64_emit_mov_imm(buf, T, 0x2);
            mm_x64_emit_xor_reg(buf, B, B);
            mm_x64_emit_bt_imm(buf, A, 31); mm_x64_emit_setcc(buf, mm_x64_cc::b, B);  mm_x64_emit_shl_imm(buf, B, 7);  mm_x64_emit_or_reg(buf, T, B);
            mm_x64_emit_xor_reg(buf, B, B);
            mm_x64_emit_bt_imm(buf, A, 30); mm_x64_emit_setcc(buf, mm_x64_cc::b, B);  mm_x64_emit_shl_imm(buf, B, 6);  mm_x64_emit_or_reg(buf, T, B);
            mm_x64_emit_xor_reg(buf, B, B);
            mm_x64_emit_bt_imm(buf, A, 29); mm_x64_emit_setcc(buf, mm_x64_cc::ae, B); mm_x64_emit_or_reg(buf, T, B);
            mm_x64_emit_xor_reg(buf, B, B);
            mm_x64_emit_bt_imm(buf, A, 28); mm_x64_emit_setcc(buf, mm_x64_cc::b, B);  mm_x64_emit_shl_imm(buf, B, 11); mm_x64_emit_or_reg(buf, T, B);
            mm_x64_emit_mov_store(buf, CTX, OFF_EFLAGS, T);
            break;
        case mm_a64_sysreg::fpcr: {
            /* FPCR -> MXCSR: RMode (23:22; RN/RP/RM/RZ) -> RC (14:13; RN/RD/RU/RZ), FZ (24) -> FTZ (15) | DAZ (6).
             * Exceptions stay masked; a guest that unmasks them would see traps we do not deliver. */
            mm_x64_emit_mov_store(buf, CTX, offsetof(mm_cpu_context, fpcr), A);
            mm_x64_emit_mov_imm(buf, T, 0x1F80);
            mm_x64_emit_mov_reg(buf, B, A);
            mm_x64_emit_shr_imm(buf, B, 22);
            mm_x64_emit_alu_imm32(buf, mm_x64_alu::and_, B, 3, true);
            /* swap the RP/RM codes: x = (x & 1) << 1 | (x >> 1) */
            mm_x64_emit_mov_reg(buf, A, B);
            mm_x64_emit_shr_imm(buf, A, 1);
            mm_x64_emit_alu_imm32(buf, mm_x64_alu::and_, B, 1, true);
            mm_x64_emit_shl_imm(buf, B, 1);
            mm_x64_emit_or_reg(buf, B, A);
            mm_x64_emit_shl_imm(buf, B, 13);
            mm_x64_emit_or_reg(buf, T, B);
            mm_x64_emit_mov_load(buf, A, CTX, offsetof(mm_cpu_context, fpcr));
            mm_x64_emit_bt_imm(buf, A, 24);
            const std::size_t no_fz = mm_x64_emit_jcc_rel8(buf, mm_x64_cc::ae);
            mm_x64_emit_alu_imm32(buf, mm_x64_alu::or_, T, (1 << 15) | (1 << 6), true);
            mm_x64_patch_rel8_to_here(buf, no_fz);
            mm_x64_emit_mov_store(buf, CTX, offsetof(mm_cpu_context, mxcsr), T);
            mm_x64_emit_ldmxcsr(buf, CTX, offsetof(mm_cpu_context, mxcsr));
            break;
        }
        case mm_a64_sysreg::fpsr:        mm_x64_emit_mov_store(buf, CTX, offsetof(mm_cpu_context, fpsr), A); break;
        case mm_a64_sysreg::tpidr_el0:   mm_x64_emit_mov_store(buf, CTX, offsetof(mm_cpu_context, tpidr_el0), A); break;
        default: return false; /* read-only registers */
        }
        return true;

    case mm_a64_opcode::sbfm:
    case mm_a64_opcode::bfm:
    case mm_a64_opcode::ubfm:
        emit_bitfield(buf, insn);
        return true;

    case mm_a64_opcode::ldr:
        emit_address(buf, insn);
        emit_load_to(buf, insn, insn.rd, 0);
        emit_writeback(buf, insn);
        return true;
    case mm_a64_opcode::str:
        emit_address(buf, insn);
        emit_store_from(buf, insn, insn.rd, 0);
        emit_writeback(buf, insn);
        return true;
    case mm_a64_opcode::ldp:
        emit_address(buf, insn);
        emit_load_to(buf, insn, insn.rd, 0);
        emit_load_to(buf, insn, insn.rt2, insn.mem_size);
        emit_writeback(buf, insn);
        return true;
    case mm_a64_opcode::stp:
        emit_address(buf, insn);
        emit_store_from(buf, insn, insn.rd, 0);
        emit_store_from(buf, insn, insn.rt2, insn.mem_size);
        emit_writeback(buf, insn);
        return true;

    case mm_a64_opcode::fmov_to_gpr:
    case mm_a64_opcode::fmov_from_gpr:
    case mm_a64_opcode::ins_gen:
    case mm_a64_opcode::umov:
    case mm_a64_opcode::smov:
    case mm_a64_opcode::dup_gen:
    case mm_a64_opcode::movi:
    case mm_a64_opcode::vlogic:
        emit_simd_gpr(buf, insn);
        return true;

    case mm_a64_opcode::ldar:
    case mm_a64_opcode::stlr:
    case mm_a64_opcode::ldxr:
    case mm_a64_opcode::stxr:
    case mm_a64_opcode::cas:
    case mm_a64_opcode::casp:
    case mm_a64_opcode::atomic:
        emit_atomic(buf, insn);
        return true;

    case mm_a64_opcode::ld1_multi:
    case mm_a64_opcode::st1_multi: {
        emit_address(buf, insn);
        const int stride = insn.q ? 16 : 8;
        for (int i = 0; i < insn.rt2; i++) {
            const auto v = static_cast<std::uint8_t>((insn.rd + i) & 31);
            if (insn.op == mm_a64_opcode::ld1_multi) {
                if (insn.q) mm_x64_emit_movdqu_load(buf, X0, B, i * stride);
                else mm_x64_emit_movq_load(buf, X0, B, i * stride);
                vstore(buf, v, X0, insn.q);
            } else {
                vload(buf, X0, v);
                if (insn.q) mm_x64_emit_movdqu_store(buf, B, i * stride, X0);
                else mm_x64_emit_movq_store(buf, B, i * stride, X0);
            }
        }
        emit_writeback(buf, insn);
        return true;
    }
    /* ld2/ld3/ld4 and st2/st3/st4: memory holds the registers interleaved
     * element by element, so each lane is moved on its own through the
     * scratch GPR. Element count comes from the register width. */
    case mm_a64_opcode::ldn_multi:
    case mm_a64_opcode::stn_multi: {
        emit_address(buf, insn);
        const int esize = insn.elem_size, n = insn.rt2;
        const int lanes = (insn.q ? 16 : 8) / esize;
        const bool load = insn.op == mm_a64_opcode::ldn_multi;
        for (int lane = 0; lane < lanes; lane++)
            for (int r = 0; r < n; r++) {
                const auto v = static_cast<std::uint8_t>((insn.rd + r) & 31);
                const std::int32_t mem = static_cast<std::int32_t>((lane * n + r) * esize);
                const std::int32_t reg = voff(v, 0) + lane * esize;
                if (load) {
                    mm_x64_emit_load(buf, A, B, mem, unsigned_kind(esize));
                    mm_x64_emit_store(buf, CTX, reg, A, esize);
                } else {
                    mm_x64_emit_load(buf, A, CTX, reg, unsigned_kind(esize));
                    mm_x64_emit_store(buf, B, mem, A, esize);
                }
            }
        if (load && !insn.q)
            for (int r = 0; r < n; r++) store_v_zero_high(buf, static_cast<std::uint8_t>((insn.rd + r) & 31));
        emit_writeback(buf, insn);
        return true;
    }
    case mm_a64_opcode::ldn_lane:
    case mm_a64_opcode::stn_lane: {
        emit_address(buf, insn);
        const int esize = insn.elem_size;
        const bool load = insn.op == mm_a64_opcode::ldn_lane;
        for (int r = 0; r < insn.rt2; r++) {
            const auto v = static_cast<std::uint8_t>((insn.rd + r) & 31);
            const std::int32_t mem = static_cast<std::int32_t>(r * esize);
            const std::int32_t reg = voff(v, 0) + insn.elem_index * esize;
            if (load) {
                mm_x64_emit_load(buf, A, B, mem, unsigned_kind(esize));
                mm_x64_emit_store(buf, CTX, reg, A, esize);
            } else {
                mm_x64_emit_load(buf, A, CTX, reg, unsigned_kind(esize));
                mm_x64_emit_store(buf, B, mem, A, esize);
            }
        }
        emit_writeback(buf, insn);
        return true;
    }
    case mm_a64_opcode::ldnr: {
        emit_address(buf, insn);
        const int esize = insn.elem_size;
        for (int r = 0; r < insn.rt2; r++) {
            const auto v = static_cast<std::uint8_t>((insn.rd + r) & 31);
            mm_x64_emit_load(buf, A, B, static_cast<std::int32_t>(r * esize), unsigned_kind(esize));
            if (esize < 8) {
                mm_x64_emit_mov_imm(buf, T, replicate_factor(esize));
                mm_x64_emit_imul_reg(buf, A, T);
            }
            mm_x64_emit_mov_store(buf, CTX, voff(v, 0), A);
            if (insn.q) mm_x64_emit_mov_store(buf, CTX, voff(v, 1), A);
            else store_v_zero_high(buf, v);
        }
        emit_writeback(buf, insn);
        return true;
    }

    /* eor3/bcax: three-register logic over the whole 128-bit register. */
    case mm_a64_opcode::eor3:
    case mm_a64_opcode::bcax: {
        vload(buf, X0, insn.rn);
        vload(buf, X1, insn.rm);
        vload(buf, X2, insn.ra);
        if (insn.op == mm_a64_opcode::bcax) mm_x64_emit_sse_rr(buf, mm_x64_sse::pandn, X2, X1); /* X2 = ~Va & Vm */
        else mm_x64_emit_sse_rr(buf, mm_x64_sse::pxor, X0, X2);
        mm_x64_emit_sse_rr(buf, mm_x64_sse::pxor, X0, insn.op == mm_a64_opcode::bcax ? X2 : X1);
        vstore(buf, insn.rd, X0, true);
        return true;
    }

    case mm_a64_opcode::ld1_lane:
        emit_address(buf, insn);
        mm_x64_emit_load(buf, A, B, 0, unsigned_kind(insn.elem_size));
        mm_x64_emit_store(buf, CTX, voff(insn.rd, 0) + insn.elem_index * insn.elem_size, A, insn.elem_size);
        emit_writeback(buf, insn);
        return true;
    case mm_a64_opcode::st1_lane:
        emit_address(buf, insn);
        mm_x64_emit_load(buf, A, CTX, voff(insn.rd, 0) + insn.elem_index * insn.elem_size, unsigned_kind(insn.elem_size));
        mm_x64_emit_store(buf, B, 0, A, insn.elem_size);
        emit_writeback(buf, insn);
        return true;
    case mm_a64_opcode::ld1r:
        emit_address(buf, insn);
        mm_x64_emit_load(buf, A, B, 0, unsigned_kind(insn.elem_size));
        if (insn.elem_size < 8) {
            mm_x64_emit_mov_imm(buf, T, replicate_factor(insn.elem_size));
            mm_x64_emit_imul_reg(buf, A, T);
        }
        mm_x64_emit_mov_store(buf, CTX, voff(insn.rd, 0), A);
        if (insn.q) mm_x64_emit_mov_store(buf, CTX, voff(insn.rd, 1), A);
        else store_v_zero_high(buf, insn.rd);
        emit_writeback(buf, insn);
        return true;

    case mm_a64_opcode::adr:
    case mm_a64_opcode::adrp:
        mm_x64_emit_mov_imm(buf, A, static_cast<std::uint64_t>(insn.imm));
        store(buf, insn.rd, A, true, reg31::xzr);
        return true;

    case mm_a64_opcode::b:
        e.jump_to(static_cast<std::uint64_t>(insn.imm), mm_x64_cc::e, false);
        return true;
    case mm_a64_opcode::bl:
        e.set_link_register();
        e.jump_to(static_cast<std::uint64_t>(insn.imm), mm_x64_cc::e, false);
        return true;
    case mm_a64_opcode::b_cond:
        if (insn.cond >= 14) {
            e.jump_to(static_cast<std::uint64_t>(insn.imm), mm_x64_cc::e, false);
        } else {
            const auto condition = test_branch_condition(buf, insn.cond);
            e.jump_to(static_cast<std::uint64_t>(insn.imm), condition, true);
        }
        return true;
    case mm_a64_opcode::cbz:
    case mm_a64_opcode::cbnz:
        load(buf, A, insn.rd, reg31::xzr);
        mm_x64_emit_test_rr(buf, A, A, insn.is64);
        e.jump_to(static_cast<std::uint64_t>(insn.imm), insn.op == mm_a64_opcode::cbz ? mm_x64_cc::e : mm_x64_cc::ne, true);
        return true;
    case mm_a64_opcode::tbz:
    case mm_a64_opcode::tbnz:
        load(buf, A, insn.rd, reg31::xzr);
        mm_x64_emit_bt_imm(buf, A, insn.imms);
        e.jump_to(static_cast<std::uint64_t>(insn.imm), insn.op == mm_a64_opcode::tbz ? mm_x64_cc::ae : mm_x64_cc::b, true);
        return true;
    case mm_a64_opcode::br:
        load(buf, A, insn.rn, reg31::xzr);
        mm_x64_patch_rel32(buf, mm_x64_emit_jmp_rel32(buf), e.st.dispatch);
        return true;
    case mm_a64_opcode::ret:
        load(buf, A, insn.rn, reg31::xzr);
        /* Zero is the runtime's top-level return sentinel. An indirect
         * call/jump to a missing weak import must report bad_target. */
        mm_x64_emit_test_rr(buf, A, A, true);
        mm_x64_patch_rel32(buf, mm_x64_emit_jcc_rel32(buf, mm_x64_cc::e), e.st.exit);
        mm_x64_patch_rel32(buf, mm_x64_emit_jmp_rel32(buf), e.st.dispatch);
        return true;
    case mm_a64_opcode::blr:
        load(buf, A, insn.rn, reg31::xzr);
        mm_x64_emit_mov_reg(buf, B, A);
        e.set_link_register();
        mm_x64_emit_mov_reg(buf, A, B);
        mm_x64_patch_rel32(buf, mm_x64_emit_jmp_rel32(buf), e.st.dispatch);
        return true;
    case mm_a64_opcode::svc:
        e.exit_via(e.st.svc);
        return true;
    case mm_a64_opcode::brk:
        e.exit_via(e.st.brk);
        return true;
    case mm_a64_opcode::nop:
        mm_x64_emit_nop(buf);
        return true;
    case mm_a64_opcode::vop:
        return emit_vector_op(buf, insn);
    case mm_a64_opcode::crypto:
        return emit_crypto_op(buf, insn);
    case mm_a64_opcode::simd_helper:
        return emit_simd_helper(buf, insn);
    case mm_a64_opcode::fop:
        return emit_fp_op(buf, insn);
    case mm_a64_opcode::unknown:
        return false;
    }
    return false;
}

} // namespace
} // namespace mm_emit

using namespace mm_emit;

/* ---- watch points -------------------------------------------------------- */

/* $MACHOMAN_WATCH is a comma-separated list of guest VAs. Each one gets a
 * call to mm_watch_hit emitted in front of it, which prints the guest's
 * registers as execution passes. Nothing else can show what a guest
 * function returned: the translated code holds no host state, so a host
 * debugger only ever sees the instruction, never the guest's x0.
 *
 * The call is made with the host stack realigned and only the scratch
 * registers clobbered, and host flags are dead between guest instructions
 * (NZCV lives in ctx.eflags), so it is invisible to the translation. */
/* Reads guest memory without risking a fault: process_vm_readv reports a
 * bad address instead of raising SIGSEGV, which a watch point must not do
 * — it is only looking. */
bool peek(std::uint64_t addr, void *out, std::size_t n) {
#if defined(__linux__)
    iovec local{out, n};
    iovec remote{reinterpret_cast<void *>(static_cast<std::uintptr_t>(addr)), n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n);
#else
    (void)addr; (void)out; (void)n;
    return false;
#endif
}

/* A register often holds a string; printing it is what makes a watch point
 * readable. Both of Apple's shapes are recognised: plain UTF-8 and the
 * UTF-16 a Cocoa or C++ program passes around. A register more often
 * points at the object that owns the characters than at the characters
 * themselves, so a few common header sizes are tried as well and the
 * offset is printed with the text. */
bool string_at(std::uint64_t addr, std::string &out);

bool describe_string(std::uint64_t addr, std::string &out) {
    if (string_at(addr, out)) return true;
    for (int off : {8, 16, 32}) {
        if (string_at(addr + static_cast<unsigned>(off), out)) {
            out = "+" + std::to_string(off) + ":" + out;
            return true;
        }
    }
    for (int off : {0, 8, 16}) {
        std::uint64_t data = 0;
        if (peek(addr + static_cast<unsigned>(off), &data, sizeof data) && string_at(data, out)) {
            out = "*+" + std::to_string(off) + ":" + out;
            return true;
        }
    }
    return false;
}

bool string_at(std::uint64_t addr, std::string &out) {
    if (addr < 0x1000) return false;
    char buf[64] = {};
    if (!peek(addr, buf, sizeof buf - 2)) return false;
    auto printable = [](unsigned char c) { return c >= 0x20 && c < 0x7F; };
    std::size_t n = 0;
    while (n < sizeof buf - 2 && printable(static_cast<unsigned char>(buf[n]))) n++;
    if (n >= 3 && buf[n] == '\0') { out.assign(buf, n); return true; }
    if (n == sizeof buf - 2) { out.assign(buf, n); out += "..."; return true; }
    /* UTF-16: ASCII characters have a zero high byte. */
    n = 0;
    while (n * 2 + 1 < sizeof buf - 2 && printable(static_cast<unsigned char>(buf[n * 2])) && buf[n * 2 + 1] == '\0') n++;
    if (n >= 3) {
        out.clear();
        for (std::size_t i = 0; i < n; i++) out.push_back(buf[i * 2]);
        if (n * 2 + 1 >= sizeof buf - 2) out += "...";
        return true;
    }
    return false;
}

extern "C" void mm_watch_hit(const mm_cpu_context *ctx, std::uint64_t va) {
    static const auto limit = [] {
        const char *value = std::getenv("MACHOMAN_WATCH_LIMIT");
        return value ? std::strtoull(value, nullptr, 10) : 0;
    }();
    if (limit) {
        static std::mutex mutex;
        static std::unordered_map<std::uint64_t, unsigned long long> hits;
        std::lock_guard lock(mutex);
        if (++hits[va] > limit) return;
    }
    std::fprintf(stderr, "watch 0x%llx:", static_cast<unsigned long long>(va));
    static const int registers = std::getenv("MACHOMAN_WATCH_REGISTERS") ? 31 : 9;
    for (int i = 0; i < registers; i++) {
        std::fprintf(stderr, " x%d=0x%llx", i, static_cast<unsigned long long>(ctx->x[i]));
        std::string text;
        if (describe_string(ctx->x[i], text)) std::fprintf(stderr, "(\"%s\")", text.c_str());
    }
    std::fprintf(stderr, " sp=0x%llx x30=0x%llx\n", static_cast<unsigned long long>(ctx->x[31]),
                 static_cast<unsigned long long>(ctx->x[30]));
    /* Optional bounded memory snapshots: VA:register:bytes[:pointer-offset[:data-offset]].
     * The optional offsets follow a pointer held in the watched object. Read
     * through process_vm_readv so inaccessible pointers remain harmless. */
    static const char *dump_spec = std::getenv("MACHOMAN_WATCH_DUMP");
    static const char *dump_dir = std::getenv("MACHOMAN_WATCH_DUMP_DIR");
    if (dump_spec && dump_dir) {
        for (const char *p = dump_spec; *p;) {
            char *end = nullptr;
            const auto address = std::strtoull(p, &end, 0);
            if (end == p || *end != ':') break;
            p = end + 1;
            const auto reg = std::strtoul(p, &end, 0);
            if (end == p || *end != ':') break;
            p = end + 1;
            const auto bytes = std::strtoul(p, &end, 0);
            if (end == p) break;
            bool indirect = false;
            std::uint64_t pointer_offset = 0, data_offset = 0;
            if (*end == ':') {
                p = end + 1;
                pointer_offset = std::strtoull(p, &end, 0);
                if (end == p) break;
                indirect = true;
                if (*end == ':') {
                    p = end + 1;
                    data_offset = std::strtoull(p, &end, 0);
                    if (end == p) break;
                }
            }
            if (*end && *end != ',') break;
            p = *end == ',' ? end + 1 : end;
            if (address != va || reg >= 32 || bytes == 0 || bytes > 1024 * 1024) continue;
            auto source = ctx->x[reg];
            if (indirect) {
                if (source + pointer_offset < source || !peek(source + pointer_offset, &source, sizeof source)) continue;
                if (!source || source + data_offset < source) continue;
                source += data_offset;
            }
            std::vector<unsigned char> memory(bytes);
            if (!peek(source, memory.data(), memory.size())) continue;
            static std::atomic<unsigned> sequence{0};
            char path[4096];
            std::snprintf(path, sizeof path, "%s/watch-%llx-x%lu-%u.bin", dump_dir,
                          static_cast<unsigned long long>(va), reg, sequence.fetch_add(1));
            if (auto *file = std::fopen(path, "wb")) {
                std::fwrite(memory.data(), 1, memory.size(), file);
                std::fclose(file);
                std::fprintf(stderr, "watch dump: %s address=0x%llx bytes=%lu\n", path,
                             static_cast<unsigned long long>(source), bytes);
            }
        }
    }
}

/* AArch64's CRC32 (as opposed to CRC32C) uses the Ethernet polynomial,
 * which x86 has no instruction for, so it is computed here and called from
 * the translation the same way a watch point is. Neither ARM's nor this
 * one inverts the accumulator: the instruction is a raw CRC step. */
extern "C" std::uint32_t mm_crc32_ethernet(std::uint32_t acc, std::uint64_t value, int bytes) {
    for (int i = 0; i < bytes; i++) {
        acc ^= static_cast<std::uint32_t>((value >> (i * 8)) & 0xFF);
        for (int bit = 0; bit < 8; bit++)
            acc = (acc >> 1) ^ (0xEDB88320u & static_cast<std::uint32_t>(-static_cast<std::int32_t>(acc & 1)));
    }
    return acc;
}

const std::vector<std::uint64_t> &watch_points() {
    static const std::vector<std::uint64_t> list = [] {
        std::vector<std::uint64_t> v;
        const char *spec = std::getenv("MACHOMAN_WATCH");
        for (const char *p = spec; p && *p;) {
            char *end = nullptr;
            const std::uint64_t va = std::strtoull(p, &end, 0);
            if (end == p) break;
            v.push_back(va);
            p = *end == ',' ? end + 1 : end;
        }
        return v;
    }();
    return list;
}

void emit_watch(mm_x64_buf &buf, std::uint64_t va) {
    mm_x64_emit_push_reg(buf, MM_RBP);
    mm_x64_emit_mov_reg(buf, MM_RBP, MM_RSP);
    mm_x64_emit_alu_imm32(buf, mm_x64_alu::and_, MM_RSP, -16, true);
    mm_x64_emit_mov_reg(buf, MM_RDI, MM_R15);
    mm_x64_emit_mov_imm(buf, MM_RSI, va);
    mm_x64_emit_host_address(buf, MM_RAX, reinterpret_cast<std::uintptr_t>(&mm_watch_hit));
    mm_x64_emit_call_reg(buf, MM_RAX);
    mm_x64_emit_mov_reg(buf, MM_RSP, MM_RBP);
    mm_x64_emit_pop_reg(buf, MM_RBP);
}

std::size_t mm_translate_buffer(std::span<const std::uint8_t> arm64_code, std::uint64_t base_va,
                                mm_translation_result &result) {
    const std::uint64_t text_size = arm64_code.size() & ~std::uint64_t{3};
    result.text_base = base_va;
    const stubs st = emit_stubs(result.code, base_va, text_size);
    result.dispatch_stub = st.dispatch;
    std::vector<fixup> fixups;
    std::size_t count = 0;
    struct cold_entry { std::size_t index; int guest[2]; unsigned reads; };
    std::vector<cold_entry> entries;
    const bool residency = mm_register_cache_enabled();
    auto &cache = result.code.vectors;
    cache = {};

    for (std::size_t off = 0; off + 4 <= arm64_code.size(); off += 4, count++) {
        const std::uint64_t pc = base_va + off;
        const mm_a64_insn insn = mm_a64_decode(arm64_code.subspan(off, 4), pc);
        const std::uint64_t out_off = result.code.size();
        const bool watched = !watch_points().empty() &&
            std::find(watch_points().begin(), watch_points().end(), pc) != watch_points().end();
        // Limit the first allocator to native FP emission. Other opcodes
        // may write V through scalar/lane stores or call native helpers.
        const bool native_fp = insn.op == mm_a64_opcode::fop && insn.elem_size >= 4 &&
            !(insn.fop == mm_a64_fop::fcvt && insn.imm == 2);
        if (!residency || !native_fp || watched) cache = {};
        cache.active = residency && native_fp;
        const int entry_guests[2] = {cache.guest[0], cache.guest[1]};
        const auto entry_live = (cache.guest[0] >= 0 ? 1u : 0u) | (cache.guest[1] >= 0 ? 2u : 0u);
        cache.written = 0;
        if (watched)
            emit_watch(result.code, pc);

        emit_ctx e{result.code, st, base_va, text_size, pc, fixups};
        const bool supported = translate_one(e, insn);
        if (!supported) {
            e.exit_via(st.unsupported);
            result.unsupported_count++;
        }

        const auto index = result.notes.size();
        result.notes.push_back({off, out_off, insn.op, supported, out_off});
        // An entry also needs copies that this instruction preserves for
        // later fallthrough consumers, even if it does not read them itself.
        if (entry_live) entries.push_back({index, {entry_guests[0], entry_guests[1]}, entry_live});
        // Some conversions/FCSEL store lanes directly instead of vstore.
        // Those results remain materialized but cannot reuse an old copy.
        const bool flags_only = insn.fop == mm_a64_fop::fcmp || insn.fop == mm_a64_fop::fcmp0 ||
            insn.fop == mm_a64_fop::fccmp;
        if (native_fp && !flags_only && !(cache.written & (1u << insn.rd))) {
            for (int &guest : cache.guest) if (guest == insn.rd) guest = -1;
        }
        if (!supported) cache = {};
    }

    cache = {};
    if (!entries.empty()) {
        // Independent entries are out of line: ordinary fallthrough pays
        // no jump/preload. Prevent falling off text into the entry stubs.
        mm_x64_emit_mov_imm(result.code, A, base_va + text_size);
        mm_x64_patch_rel32(result.code, mm_x64_emit_jmp_rel32(result.code), st.dispatch);
        for (const auto &entry : entries) {
            auto &note = result.notes[entry.index];
            note.entry_offset = result.code.size();
            for (unsigned slot = 0; slot < 2; ++slot) {
                if (!(entry.reads & (1u << slot))) continue;
                mm_x64_emit_movdqu_load(result.code, 4 + static_cast<int>(slot), CTX,
                    voff(static_cast<std::uint8_t>(entry.guest[slot]), 0));
            }
            mm_x64_patch_rel32(result.code, mm_x64_emit_jmp_rel32(result.code), note.output_offset);
        }
    }
    for (const fixup &f : fixups) {
        const std::size_t idx = static_cast<std::size_t>((f.target_va - base_va) / 4);
        const auto &note = result.notes[idx];
        mm_x64_patch_rel32(result.code, f.rel32_at, note.entry_offset);
    }

    return count;
}

bool mm_register_cache_enabled() {
    const auto *setting = std::getenv("MACHOMAN_REGISTER_CACHE");
    return !setting || *setting != '0';
}

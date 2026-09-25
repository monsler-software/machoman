/* Minimal x86_64 encoder: the AOT translator's back end. A thin
 * "instruction emission" toolkit over a growable byte buffer; the
 * translation logic itself (which guest op becomes which host sequence)
 * lives in translator.cpp. */
#ifndef MACHOMAN_X86_64_ENCODER_H
#define MACHOMAN_X86_64_ENCODER_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

enum mm_x64_reg : int {
    MM_RAX = 0, MM_RCX, MM_RDX, MM_RBX, MM_RSP, MM_RBP, MM_RSI, MM_RDI,
    MM_R8, MM_R9, MM_R10, MM_R11, MM_R12, MM_R13, MM_R14, MM_R15,
};

struct mm_x64_buf {
    std::vector<std::uint8_t> bytes;
    std::size_t size() const { return bytes.size(); }
};

void mm_x64_emit(mm_x64_buf &buf, std::span<const std::uint8_t> bytes);

/* mov r64, imm: picks the shortest encoding (sign-extended imm32, or imm64). */
void mm_x64_emit_mov_imm(mm_x64_buf &buf, int reg, std::uint64_t imm);

/* mov r64, [base + disp32] / mov [base + disp32], r64 */
void mm_x64_emit_mov_load(mm_x64_buf &buf, int dst, int base, std::int32_t disp);
void mm_x64_emit_mov_store(mm_x64_buf &buf, int base, std::int32_t disp, int src);

/* Sized loads into a 64-bit register. Unsigned kinds zero-extend to 64
 * bits; the s*_32 kinds sign-extend to 32 bits and zero the upper half. */
enum class mm_x64_load {
    u8, u16, u32, u64,
    s8_32, s16_32,
    s8_64, s16_64, s32_64,
};
void mm_x64_emit_load(mm_x64_buf &buf, int dst, int base, std::int32_t disp, mm_x64_load kind);
/* Store the low `size` (1/2/4/8) bytes of src. */
void mm_x64_emit_store(mm_x64_buf &buf, int base, std::int32_t disp, int src, int size);

/* Generic ALU forms with selectable operand size (w64=false -> 32-bit,
 * which zero-extends the destination). */
enum class mm_x64_alu : std::uint8_t { add = 0, or_ = 1, adc = 2, sbb = 3, and_ = 4, sub = 5, xor_ = 6, cmp = 7 };
void mm_x64_emit_alu_rr(mm_x64_buf &buf, mm_x64_alu op, int dst, int src, bool w64);
void mm_x64_emit_alu_imm32(mm_x64_buf &buf, mm_x64_alu op, int reg, std::int32_t imm, bool w64);
void mm_x64_emit_test_rr(mm_x64_buf &buf, int lhs, int rhs, bool w64);

/* Register-register 64-bit ALU ops, dst = dst OP src. */
void mm_x64_emit_mov_reg(mm_x64_buf &buf, int dst, int src);
void mm_x64_emit_mov_reg32(mm_x64_buf &buf, int dst, int src); /* mov r32, r32: zero-extends */
void mm_x64_emit_movsxd(mm_x64_buf &buf, int dst, int src);    /* movsxd r64, r32 */
void mm_x64_emit_add_reg(mm_x64_buf &buf, int dst, int src);
void mm_x64_emit_sub_reg(mm_x64_buf &buf, int dst, int src);
void mm_x64_emit_and_reg(mm_x64_buf &buf, int dst, int src);
void mm_x64_emit_or_reg(mm_x64_buf &buf, int dst, int src);
void mm_x64_emit_xor_reg(mm_x64_buf &buf, int dst, int src);
void mm_x64_emit_imul_reg(mm_x64_buf &buf, int dst, int src);
void mm_x64_emit_cmp_reg(mm_x64_buf &buf, int lhs, int rhs);
void mm_x64_emit_test_reg(mm_x64_buf &buf, int lhs, int rhs);

void mm_x64_emit_add_imm32(mm_x64_buf &buf, int reg, std::int32_t imm);
void mm_x64_emit_sub_imm32(mm_x64_buf &buf, int reg, std::int32_t imm);
void mm_x64_emit_cmp_imm32(mm_x64_buf &buf, int reg, std::int32_t imm);
void mm_x64_emit_not_reg(mm_x64_buf &buf, int reg);
void mm_x64_emit_neg_reg(mm_x64_buf &buf, int reg);

/* shl/shr/sar/ror r64, imm8 */
void mm_x64_emit_shl_imm(mm_x64_buf &buf, int reg, std::uint8_t n);
void mm_x64_emit_shr_imm(mm_x64_buf &buf, int reg, std::uint8_t n);
void mm_x64_emit_sar_imm(mm_x64_buf &buf, int reg, std::uint8_t n);
void mm_x64_emit_ror_imm(mm_x64_buf &buf, int reg, std::uint8_t n);

/* Register-register zero/sign extension: movzx/movsx r64, r8/r16; movsxd. */
void mm_x64_emit_extend_reg(mm_x64_buf &buf, int dst, int src, mm_x64_load kind);

/* Shifts by cl. `w` selects 32-bit operand size (count masked to 31, result zero-extended). */
void mm_x64_emit_shl_cl(mm_x64_buf &buf, int reg, bool w);
void mm_x64_emit_shr_cl(mm_x64_buf &buf, int reg, bool w);
void mm_x64_emit_sar_cl(mm_x64_buf &buf, int reg, bool w);
void mm_x64_emit_ror_cl(mm_x64_buf &buf, int reg, bool w);

/* rdx:rax = rax * reg (mul unsigned / imul signed); rax = rdx:rax / reg. */
void mm_x64_emit_mul_one(mm_x64_buf &buf, int reg);
void mm_x64_emit_imul_one(mm_x64_buf &buf, int reg);
void mm_x64_emit_div_one(mm_x64_buf &buf, int reg);
void mm_x64_emit_idiv_one(mm_x64_buf &buf, int reg);
void mm_x64_emit_cqo(mm_x64_buf &buf);

void mm_x64_emit_bswap(mm_x64_buf &buf, int reg, bool w64);
void mm_x64_emit_lzcnt(mm_x64_buf &buf, int dst, int src, bool w64);

/* Short jumps. Return the offset of the rel8 byte so it can be patched;
 * mm_x64_patch_rel8 sets it to jump to the current end of the buffer. */
enum class mm_x64_cc : std::uint8_t { o = 0, no, b, ae, e, ne, be, a, s, ns, p, np, l, ge, le, g };
std::size_t mm_x64_emit_jcc_rel8(mm_x64_buf &buf, mm_x64_cc cc);
std::size_t mm_x64_emit_jmp_rel8(mm_x64_buf &buf);
void mm_x64_patch_rel8_to_here(mm_x64_buf &buf, std::size_t rel8_at);

/* Near jumps with rel32; return the offset of the rel32 field for patching. */
std::size_t mm_x64_emit_jcc_rel32(mm_x64_buf &buf, mm_x64_cc cc);
std::size_t mm_x64_emit_jmp_rel32(mm_x64_buf &buf);
void mm_x64_patch_rel32(mm_x64_buf &buf, std::size_t rel32_at, std::size_t target);
void mm_x64_emit_jmp_reg(mm_x64_buf &buf, int reg);

void mm_x64_emit_cmovcc(mm_x64_buf &buf, mm_x64_cc cc, int dst, int src);
void mm_x64_emit_bt_imm(mm_x64_buf &buf, int reg, std::uint8_t bit); /* CF = reg[bit] */

/* Flag plumbing. */
void mm_x64_emit_pushf(mm_x64_buf &buf);
void mm_x64_emit_popf(mm_x64_buf &buf);
void mm_x64_emit_push_mem(mm_x64_buf &buf, int base, std::int32_t disp);
void mm_x64_emit_pop_mem(mm_x64_buf &buf, int base, std::int32_t disp);
void mm_x64_emit_cmc(mm_x64_buf &buf);
void mm_x64_emit_stc(mm_x64_buf &buf);

/* mov qword [base + disp32], imm32 (sign-extended) */
void mm_x64_emit_crc32(mm_x64_buf &buf, int dst, int src, int size); /* SSE4.2 crc32 (Castagnoli) */

void mm_x64_emit_push_reg(mm_x64_buf &buf, int reg);
void mm_x64_emit_pop_reg(mm_x64_buf &buf, int reg);
void mm_x64_emit_call_reg(mm_x64_buf &buf, int reg); /* call *reg */

void mm_x64_emit_mov_store_imm32(mm_x64_buf &buf, int base, std::int32_t disp, std::int32_t imm);
/* mov r64, [base + index * scale]  (scale 1/2/4/8) */
void mm_x64_emit_mov_load_indexed(mm_x64_buf &buf, int dst, int base, int index, int scale);

/* Read-modify-write on [base] with operand size 1/2/4/8. cmpxchg and xadd
 * carry a lock prefix; xchg with memory is implicitly locked. */
enum class mm_x64_rmw { cmpxchg, xadd, xchg };
void mm_x64_emit_rmw(mm_x64_buf &buf, mm_x64_rmw op, int base, int reg, int size);
/* lock cmpxchg8b/16b [base]: compares edx:eax / rdx:rax with the memory
 * pair and stores ecx:ebx / rcx:rbx on a match, else loads the pair into
 * edx:eax / rdx:rax. `wide` picks the 16-byte form. */
void mm_x64_emit_cmpxchg_pair(mm_x64_buf &buf, int base, bool wide);
void mm_x64_emit_setcc(mm_x64_buf &buf, mm_x64_cc cc, int reg8);
void mm_x64_patch_rel8_to(mm_x64_buf &buf, std::size_t rel8_at, std::size_t target);

void mm_x64_emit_shrd_imm(mm_x64_buf &buf, int dst, int src, std::uint8_t n, bool w64); /* dst = (src:dst) >> n */
void mm_x64_emit_mfence(mm_x64_buf &buf);
void mm_x64_emit_rdtsc(mm_x64_buf &buf); /* edx:eax */

/* ---- SSE2/SSSE3/SSE4.1 integer vector ops on xmm0-xmm15 ---------------- */
enum class mm_x64_sse : std::uint8_t {
    paddb, paddw, paddd, paddq, psubb, psubw, psubd, psubq,
    pcmpeqb, pcmpeqw, pcmpeqd, pcmpeqq, pcmpgtb, pcmpgtw, pcmpgtd, pcmpgtq,
    pmaxub, pmaxuw, pmaxud, pminub, pminuw, pminud,
    pmaxsb, pmaxsw, pmaxsd, pminsb, pminsw, pminsd,
    pand, pandn, por, pxor,
    packuswb, packusdw, packsswb, packssdw,
    punpcklbw, punpcklwd, punpckldq, punpcklqdq, punpckhbw, punpckhwd, punpckhdq, punpckhqdq,
    pshufb, pmullw, pmulld, pmuludq, pabsb, pabsw, pabsd, phaddw, phaddd,
    pmovzxbw, pmovzxwd, pmovzxdq, pmovsxbw, pmovsxwd, pmovsxdq,
    movdqa, paddusb, paddusw, psubusb, psubusw, paddsb, paddsw, psubsb, psubsw, pavgb, pavgw,
};
void mm_x64_emit_sse_rr(mm_x64_buf &buf, mm_x64_sse op, int dst, int src);
void mm_x64_emit_palignr(mm_x64_buf &buf, int dst, int src, std::uint8_t imm);   /* dst = (dst:src) >> imm bytes */
void mm_x64_emit_pshufd(mm_x64_buf &buf, int dst, int src, std::uint8_t imm);
void mm_x64_emit_pshuflw(mm_x64_buf &buf, int dst, int src, std::uint8_t imm);
void mm_x64_emit_pshufhw(mm_x64_buf &buf, int dst, int src, std::uint8_t imm);
enum class mm_x64_vshift : std::uint8_t { psrlw, psraw, psllw, psrld, psrad, pslld, psrlq, psllq, psrldq, pslldq };
void mm_x64_emit_sse_shift_imm(mm_x64_buf &buf, mm_x64_vshift op, int reg, std::uint8_t imm);
void mm_x64_emit_movdqu_load(mm_x64_buf &buf, int xmm, int base, std::int32_t disp);
void mm_x64_emit_movdqu_store(mm_x64_buf &buf, int base, std::int32_t disp, int xmm);
void mm_x64_emit_movq_load(mm_x64_buf &buf, int xmm, int base, std::int32_t disp);   /* zero-extends to 128 */
void mm_x64_emit_movq_store(mm_x64_buf &buf, int base, std::int32_t disp, int xmm);
void mm_x64_emit_movq_from_gpr(mm_x64_buf &buf, int xmm, int r64);
void mm_x64_emit_movq_to_gpr(mm_x64_buf &buf, int r64, int xmm);
void mm_x64_emit_pinsrq(mm_x64_buf &buf, int xmm, int r64, std::uint8_t idx);
void mm_x64_emit_pextrq(mm_x64_buf &buf, int r64, int xmm, std::uint8_t idx);
void mm_x64_emit_pinsrb(mm_x64_buf &buf, int xmm, int r32, std::uint8_t idx);
void mm_x64_emit_pinsrw(mm_x64_buf &buf, int xmm, int r32, std::uint8_t idx);
void mm_x64_emit_pinsrd(mm_x64_buf &buf, int xmm, int r32, std::uint8_t idx);
void mm_x64_emit_pmovmskb(mm_x64_buf &buf, int r32, int xmm);

/* ---- SSE floating point ------------------------------------------------ */
/* Lane kind: scalar single/double, packed single/double. Selects the
 * mandatory prefix (F3/F2/none/66) of the 0F-map instructions. */
enum class mm_x64_fk : std::uint8_t { ss, sd, ps, pd };
enum class mm_x64_fp : std::uint8_t { add, sub, mul, div, min, max, sqrt, mov /* movss/movsd merge */, and_, andn, or_, xor_, ucomi, unpckl, cvt_widen /* ss->sd, ps->pd */, cvt_narrow };
void mm_x64_emit_fp_rr(mm_x64_buf &buf, mm_x64_fp op, mm_x64_fk k, int dst, int src);
void mm_x64_emit_round(mm_x64_buf &buf, mm_x64_fk k, int dst, int src, std::uint8_t mode); /* roundss/sd/ps/pd */
void mm_x64_emit_cmpfp(mm_x64_buf &buf, mm_x64_fk k, int dst, int src, std::uint8_t pred);  /* cmpss/sd/ps/pd */
void mm_x64_emit_cvtsi2fp(mm_x64_buf &buf, bool is_double, int xmm, int r, bool w64);
void mm_x64_emit_cvttfp2si(mm_x64_buf &buf, bool is_double, int r, int xmm, bool w64);
void mm_x64_emit_cvtdq2ps(mm_x64_buf &buf, int dst, int src);
void mm_x64_emit_cvtdq2pd(mm_x64_buf &buf, int dst, int src);
void mm_x64_emit_cvttps2dq(mm_x64_buf &buf, int dst, int src);
void mm_x64_emit_cvttpd2dq(mm_x64_buf &buf, int dst, int src);
void mm_x64_emit_movd_from_gpr(mm_x64_buf &buf, int xmm, int r32);
void mm_x64_emit_movd_to_gpr(mm_x64_buf &buf, int r32, int xmm);
/* FMA3 (VEX): 213 scalar forms dst = f(src1*dst, src2); 231 packed forms dst = f(src1*src2, dst). */
enum class mm_x64_fma : std::uint8_t { fmadd, fmsub, fnmadd, fnmsub };
void mm_x64_emit_fma213(mm_x64_buf &buf, mm_x64_fma op, bool is_double, int dst, int src1, int src2);
void mm_x64_emit_fma231(mm_x64_buf &buf, mm_x64_fma op, bool is_double, bool packed, int dst, int src1, int src2);

void mm_x64_emit_ldmxcsr(mm_x64_buf &buf, int base, std::int32_t disp);
void mm_x64_emit_stmxcsr(mm_x64_buf &buf, int base, std::int32_t disp);

void mm_x64_emit_push(mm_x64_buf &buf, int reg);
void mm_x64_emit_pop(mm_x64_buf &buf, int reg);
/* call rel32; `rel` is relative to the end of the 5-byte instruction. */
void mm_x64_emit_call_rel32(mm_x64_buf &buf, std::int32_t rel);
void mm_x64_emit_ret(mm_x64_buf &buf);
void mm_x64_emit_nop(mm_x64_buf &buf);
void mm_x64_emit_ud2(mm_x64_buf &buf);

#endif /* MACHOMAN_X86_64_ENCODER_H */

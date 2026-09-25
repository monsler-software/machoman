/* Internal helpers shared by the translator's emission files. Not part of
 * the public API. See translator.h for the execution model. */
#ifndef MACHOMAN_TRANSLATOR_EMIT_H
#define MACHOMAN_TRANSLATOR_EMIT_H

#include "machoman/arch/arm64_decoder.h"
#include "machoman/arch/x86_64_encoder.h"
#include "machoman/translator.h"

#include <cstddef>
#include <cstdint>

namespace mm_emit {

constexpr int CTX = MM_R15; /* guest register file base, set by the runtime trampoline */
constexpr int A = MM_RAX;   /* scratch: result / first operand */
constexpr int B = MM_RCX;   /* scratch: second operand */
constexpr int T = MM_RDX;   /* scratch: temporaries inside one guest insn */

/* xmm scratch registers for SIMD emission. */
constexpr int X0 = 0, X1 = 1, X2 = 2, X3 = 3;

enum class reg31 { sp, xzr };

std::int32_t ctx_off(std::uint8_t guest);
void load(mm_x64_buf &buf, int host, std::uint8_t guest, reg31 kind);
void store(mm_x64_buf &buf, std::uint8_t guest, int host, bool is64, reg31 kind);
mm_x64_load unsigned_kind(int size);

/* Byte offset of half `half` (0 = bits 63:0) of SIMD register n in the context. */
std::int32_t voff(std::uint8_t n, int half);
void store_v_zero_high(mm_x64_buf &buf, std::uint8_t n);

/* Whole-vector moves between the context and xmm scratch registers. */
void vload(mm_x64_buf &buf, int xmm, std::uint8_t vreg);
void vstore(mm_x64_buf &buf, std::uint8_t vreg, int xmm, bool q);
/* xmm <- 128-bit constant hi:lo, through rax. */
void vconst(mm_x64_buf &buf, int xmm, std::uint64_t lo, std::uint64_t hi);

/* AdvSIMD integer data processing (translator_simd.cpp). Returns false
 * when the operation has no emission yet. */
bool emit_vector_op(mm_x64_buf &buf, const mm_a64_insn &insn);

/* Loads rn/rm and leaves the even lanes of the (rn ++ rm) stream in X2 and
 * the odd lanes in X3, laid out as the pairwise result expects
 * (translator_simd.cpp). elem = lane width. */
void pairwise_split(mm_x64_buf &buf, const mm_a64_insn &insn, int elem);

/* Floating point, scalar and vector (translator_fp.cpp). */
bool emit_fp_op(mm_x64_buf &buf, const mm_a64_insn &insn);

} // namespace mm_emit

#endif /* MACHOMAN_TRANSLATOR_EMIT_H */

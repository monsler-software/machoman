/* Shared, portable lane semantics for AdvSIMD operations without a direct
 * SSE equivalent. Decoding validates the encoding before emitting a call. */
#ifndef MACHOMAN_SIMD_HELPERS_H
#define MACHOMAN_SIMD_HELPERS_H
#include <cstdint>
enum class mm_simd_helper : std::uint8_t {
    none, table, shll, rbit, abs64, saturating_arithmetic, doubling_mul_high,
    multiply_element, narrow_high, polynomial_multiply, dot_element, round_shift, clz,
    sha512h, sha512h2, sha512su0, sha512su1, sm3ss1, sm3tt, sm3partw1, sm3partw2,
    sm4e, sm4ekey, rax1, xar,
    fp_widen_half, fp_narrow_half, fp_negate_half, fp_reduce, fp_compare_abs, fp_complex_add,
    fp_estimate, fp_step,
    fp_half_vector, fp_half_element, fp_half_misc,
};
mm_simd_helper mm_decode_simd_helper(std::uint32_t raw);
struct mm_cpu_context;
using mm_simd_native = void (*)(mm_cpu_context *, std::uint32_t);
mm_simd_native mm_simd_crypto_native(mm_simd_helper kind);
mm_simd_native mm_simd_fp_native(mm_simd_helper kind);
mm_simd_native mm_simd_half_native(mm_simd_helper kind);
#endif

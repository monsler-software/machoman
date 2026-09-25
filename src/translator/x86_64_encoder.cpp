#include "machoman/arch/x86_64_encoder.h"

#include <cstring>
#include <initializer_list>
#include <limits>

namespace {

/* REX prefix: 0100 W R X B. W=1 selects 64-bit operand size. */
std::uint8_t rex(bool w, int reg, int index, int rm) {
    return static_cast<std::uint8_t>(0x40 | (w << 3) | (((reg >> 3) & 1) << 2) |
                                     (((index >> 3) & 1) << 1) | ((rm >> 3) & 1));
}

std::uint8_t modrm(int mod, int reg, int rm) {
    return static_cast<std::uint8_t>((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

void put(mm_x64_buf &buf, std::initializer_list<std::uint8_t> bytes) {
    buf.bytes.insert(buf.bytes.end(), bytes);
}

void put_imm32(mm_x64_buf &buf, std::int32_t imm) {
    std::uint8_t b[4];
    std::memcpy(b, &imm, 4);
    buf.bytes.insert(buf.bytes.end(), b, b + 4);
}

/* [op] r64, r/m64 style with a register operand in reg and a register in rm. */
void alu_rr(mm_x64_buf &buf, std::uint8_t opcode, int reg, int rm) {
    put(buf, {rex(true, reg, 0, rm), opcode, modrm(3, reg, rm)});
}

/* Group-1 (81 /n imm32) and unary (F7 /n) forms with the extension in reg. */
void alu_ext(mm_x64_buf &buf, std::uint8_t opcode, int ext, int rm) {
    put(buf, {rex(true, 0, 0, rm), opcode, modrm(3, ext, rm)});
}

/* mod=10 [base + disp32]; rsp/r12 as base need an SIB byte. */
void mem_disp32(mm_x64_buf &buf, int reg, int base, std::int32_t disp) {
    buf.bytes.push_back(modrm(2, reg, base));
    if ((base & 7) == MM_RSP)
        buf.bytes.push_back(0x24);
    put_imm32(buf, disp);
}

void shift_imm(mm_x64_buf &buf, int ext, int reg, std::uint8_t n) {
    put(buf, {rex(true, 0, 0, reg), 0xC1, modrm(3, ext, reg), n});
}

} // namespace

void mm_x64_emit(mm_x64_buf &buf, std::span<const std::uint8_t> bytes) {
    buf.bytes.insert(buf.bytes.end(), bytes.begin(), bytes.end());
}

void mm_x64_emit_mov_imm(mm_x64_buf &buf, int reg, std::uint64_t imm) {
    const auto s = static_cast<std::int64_t>(imm);
    if (s >= std::numeric_limits<std::int32_t>::min() && s <= std::numeric_limits<std::int32_t>::max()) {
        /* REX.W C7 /0 imm32 (sign-extended) */
        put(buf, {rex(true, 0, 0, reg), 0xC7, modrm(3, 0, reg)});
        put_imm32(buf, static_cast<std::int32_t>(s));
        return;
    }
    /* REX.W B8+rd imm64 */
    put(buf, {rex(true, 0, 0, reg), static_cast<std::uint8_t>(0xB8 | (reg & 7))});
    std::uint8_t b[8];
    std::memcpy(b, &imm, 8);
    buf.bytes.insert(buf.bytes.end(), b, b + 8);
}

void mm_x64_emit_mov_load(mm_x64_buf &buf, int dst, int base, std::int32_t disp) {
    mm_x64_emit_load(buf, dst, base, disp, mm_x64_load::u64);
}

void mm_x64_emit_mov_store(mm_x64_buf &buf, int base, std::int32_t disp, int src) {
    mm_x64_emit_store(buf, base, disp, src, 8);
}

void mm_x64_emit_load(mm_x64_buf &buf, int dst, int base, std::int32_t disp, mm_x64_load kind) {
    bool w = false;
    std::uint8_t op[2] = {0x8B, 0};
    int oplen = 1;
    switch (kind) {
    case mm_x64_load::u8:     op[0] = 0x0F; op[1] = 0xB6; oplen = 2; break;           /* movzx r32, m8 */
    case mm_x64_load::u16:    op[0] = 0x0F; op[1] = 0xB7; oplen = 2; break;           /* movzx r32, m16 */
    case mm_x64_load::u32:    break;                                                  /* mov r32, m32 */
    case mm_x64_load::u64:    w = true; break;
    case mm_x64_load::s8_32:  op[0] = 0x0F; op[1] = 0xBE; oplen = 2; break;           /* movsx r32, m8 */
    case mm_x64_load::s16_32: op[0] = 0x0F; op[1] = 0xBF; oplen = 2; break;
    case mm_x64_load::s8_64:  op[0] = 0x0F; op[1] = 0xBE; oplen = 2; w = true; break;
    case mm_x64_load::s16_64: op[0] = 0x0F; op[1] = 0xBF; oplen = 2; w = true; break;
    case mm_x64_load::s32_64: op[0] = 0x63; w = true; break;                          /* movsxd r64, m32 */
    }
    /* A REX byte is always emitted; harmless when all bits are zero. */
    buf.bytes.push_back(rex(w, dst, 0, base));
    buf.bytes.insert(buf.bytes.end(), op, op + oplen);
    mem_disp32(buf, dst, base, disp);
}

void mm_x64_emit_store(mm_x64_buf &buf, int base, std::int32_t disp, int src, int size) {
    if (size == 2)
        buf.bytes.push_back(0x66);
    /* REX is required for byte stores from rsp..rdi (else they mean ah..bh)
     * and is harmless otherwise, so always emit it. */
    buf.bytes.push_back(rex(size == 8, src, 0, base));
    buf.bytes.push_back(size == 1 ? 0x88 : 0x89);
    mem_disp32(buf, src, base, disp);
}

void mm_x64_emit_alu_rr(mm_x64_buf &buf, mm_x64_alu op, int dst, int src, bool w64) {
    /* 01+8*op /r : op r/m, r */
    const auto opcode = static_cast<std::uint8_t>(0x01 | (static_cast<int>(op) << 3));
    if (w64 || ((dst | src) & 8))
        buf.bytes.push_back(rex(w64, src, 0, dst));
    put(buf, {opcode, modrm(3, src, dst)});
}

void mm_x64_emit_alu_imm32(mm_x64_buf &buf, mm_x64_alu op, int reg, std::int32_t imm, bool w64) {
    if (w64 || (reg & 8))
        buf.bytes.push_back(rex(w64, 0, 0, reg));
    put(buf, {0x81, modrm(3, static_cast<int>(op), reg)});
    put_imm32(buf, imm);
}

void mm_x64_emit_test_rr(mm_x64_buf &buf, int lhs, int rhs, bool w64) {
    if (w64 || ((lhs | rhs) & 8))
        buf.bytes.push_back(rex(w64, rhs, 0, lhs));
    put(buf, {0x85, modrm(3, rhs, lhs)});
}

void mm_x64_emit_mov_reg(mm_x64_buf &buf, int dst, int src)  { alu_rr(buf, 0x89, src, dst); }
void mm_x64_emit_add_reg(mm_x64_buf &buf, int dst, int src)  { alu_rr(buf, 0x01, src, dst); }
void mm_x64_emit_sub_reg(mm_x64_buf &buf, int dst, int src)  { alu_rr(buf, 0x29, src, dst); }
void mm_x64_emit_and_reg(mm_x64_buf &buf, int dst, int src)  { alu_rr(buf, 0x21, src, dst); }
void mm_x64_emit_or_reg(mm_x64_buf &buf, int dst, int src)   { alu_rr(buf, 0x09, src, dst); }
void mm_x64_emit_xor_reg(mm_x64_buf &buf, int dst, int src)  { alu_rr(buf, 0x31, src, dst); }
void mm_x64_emit_cmp_reg(mm_x64_buf &buf, int lhs, int rhs)  { alu_rr(buf, 0x39, rhs, lhs); }
void mm_x64_emit_test_reg(mm_x64_buf &buf, int lhs, int rhs) { alu_rr(buf, 0x85, rhs, lhs); }

void mm_x64_emit_mov_reg32(mm_x64_buf &buf, int dst, int src) {
    /* 89 /r without REX.W; a REX byte is still needed for r8-r15. */
    if ((dst | src) & 8)
        buf.bytes.push_back(rex(false, src, 0, dst));
    put(buf, {0x89, modrm(3, src, dst)});
}

void mm_x64_emit_movsxd(mm_x64_buf &buf, int dst, int src) {
    put(buf, {rex(true, dst, 0, src), 0x63, modrm(3, dst, src)});
}

void mm_x64_emit_imul_reg(mm_x64_buf &buf, int dst, int src) {
    put(buf, {rex(true, dst, 0, src), 0x0F, 0xAF, modrm(3, dst, src)});
}

void mm_x64_emit_add_imm32(mm_x64_buf &buf, int reg, std::int32_t imm) { alu_ext(buf, 0x81, 0, reg); put_imm32(buf, imm); }
void mm_x64_emit_sub_imm32(mm_x64_buf &buf, int reg, std::int32_t imm) { alu_ext(buf, 0x81, 5, reg); put_imm32(buf, imm); }
void mm_x64_emit_cmp_imm32(mm_x64_buf &buf, int reg, std::int32_t imm) { alu_ext(buf, 0x81, 7, reg); put_imm32(buf, imm); }
void mm_x64_emit_not_reg(mm_x64_buf &buf, int reg) { alu_ext(buf, 0xF7, 2, reg); }
void mm_x64_emit_neg_reg(mm_x64_buf &buf, int reg) { alu_ext(buf, 0xF7, 3, reg); }

void mm_x64_emit_shl_imm(mm_x64_buf &buf, int reg, std::uint8_t n) { shift_imm(buf, 4, reg, n); }
void mm_x64_emit_shr_imm(mm_x64_buf &buf, int reg, std::uint8_t n) { shift_imm(buf, 5, reg, n); }
void mm_x64_emit_sar_imm(mm_x64_buf &buf, int reg, std::uint8_t n) { shift_imm(buf, 7, reg, n); }
void mm_x64_emit_ror_imm(mm_x64_buf &buf, int reg, std::uint8_t n) { shift_imm(buf, 1, reg, n); }

void mm_x64_emit_extend_reg(mm_x64_buf &buf, int dst, int src, mm_x64_load kind) {
    switch (kind) {
    case mm_x64_load::u8:     put(buf, {rex(false, dst, 0, src), 0x0F, 0xB6, modrm(3, dst, src)}); return;
    case mm_x64_load::u16:    put(buf, {rex(false, dst, 0, src), 0x0F, 0xB7, modrm(3, dst, src)}); return;
    case mm_x64_load::u32:    mm_x64_emit_mov_reg32(buf, dst, src); return;
    case mm_x64_load::u64:    if (dst != src) mm_x64_emit_mov_reg(buf, dst, src); return;
    case mm_x64_load::s8_32:  put(buf, {rex(false, dst, 0, src), 0x0F, 0xBE, modrm(3, dst, src)}); return;
    case mm_x64_load::s16_32: put(buf, {rex(false, dst, 0, src), 0x0F, 0xBF, modrm(3, dst, src)}); return;
    case mm_x64_load::s8_64:  put(buf, {rex(true, dst, 0, src), 0x0F, 0xBE, modrm(3, dst, src)}); return;
    case mm_x64_load::s16_64: put(buf, {rex(true, dst, 0, src), 0x0F, 0xBF, modrm(3, dst, src)}); return;
    case mm_x64_load::s32_64: mm_x64_emit_movsxd(buf, dst, src); return;
    }
}

namespace {
void shift_cl(mm_x64_buf &buf, int ext, int reg, bool w) {
    if (w && !(reg & 8)) { put(buf, {0xD3, modrm(3, ext, reg)}); return; }
    put(buf, {rex(!w, 0, 0, reg), 0xD3, modrm(3, ext, reg)});
}
} // namespace

void mm_x64_emit_shl_cl(mm_x64_buf &buf, int reg, bool w) { shift_cl(buf, 4, reg, w); }
void mm_x64_emit_shr_cl(mm_x64_buf &buf, int reg, bool w) { shift_cl(buf, 5, reg, w); }
void mm_x64_emit_sar_cl(mm_x64_buf &buf, int reg, bool w) { shift_cl(buf, 7, reg, w); }
void mm_x64_emit_ror_cl(mm_x64_buf &buf, int reg, bool w) { shift_cl(buf, 1, reg, w); }

void mm_x64_emit_mul_one(mm_x64_buf &buf, int reg)  { alu_ext(buf, 0xF7, 4, reg); }
void mm_x64_emit_imul_one(mm_x64_buf &buf, int reg) { alu_ext(buf, 0xF7, 5, reg); }
void mm_x64_emit_div_one(mm_x64_buf &buf, int reg)  { alu_ext(buf, 0xF7, 6, reg); }
void mm_x64_emit_idiv_one(mm_x64_buf &buf, int reg) { alu_ext(buf, 0xF7, 7, reg); }
void mm_x64_emit_cqo(mm_x64_buf &buf) { put(buf, {0x48, 0x99}); }

void mm_x64_emit_bswap(mm_x64_buf &buf, int reg, bool w64) {
    put(buf, {rex(w64, 0, 0, reg), 0x0F, static_cast<std::uint8_t>(0xC8 | (reg & 7))});
}

void mm_x64_emit_lzcnt(mm_x64_buf &buf, int dst, int src, bool w64) {
    put(buf, {0xF3, rex(w64, dst, 0, src), 0x0F, 0xBD, modrm(3, dst, src)});
}

std::size_t mm_x64_emit_jcc_rel8(mm_x64_buf &buf, mm_x64_cc cc) {
    put(buf, {static_cast<std::uint8_t>(0x70 | static_cast<std::uint8_t>(cc)), 0x00});
    return buf.size() - 1;
}

std::size_t mm_x64_emit_jmp_rel8(mm_x64_buf &buf) {
    put(buf, {0xEB, 0x00});
    return buf.size() - 1;
}

void mm_x64_patch_rel8_to_here(mm_x64_buf &buf, std::size_t rel8_at) {
    const std::size_t dist = buf.size() - (rel8_at + 1);
    buf.bytes[rel8_at] = static_cast<std::uint8_t>(static_cast<std::int8_t>(dist));
}

std::size_t mm_x64_emit_jcc_rel32(mm_x64_buf &buf, mm_x64_cc cc) {
    put(buf, {0x0F, static_cast<std::uint8_t>(0x80 | static_cast<std::uint8_t>(cc))});
    put_imm32(buf, 0);
    return buf.size() - 4;
}

std::size_t mm_x64_emit_jmp_rel32(mm_x64_buf &buf) {
    buf.bytes.push_back(0xE9);
    put_imm32(buf, 0);
    return buf.size() - 4;
}

void mm_x64_patch_rel32(mm_x64_buf &buf, std::size_t rel32_at, std::size_t target) {
    const auto rel = static_cast<std::int32_t>(static_cast<std::int64_t>(target) - static_cast<std::int64_t>(rel32_at + 4));
    std::memcpy(buf.bytes.data() + rel32_at, &rel, 4);
}

void mm_x64_emit_jmp_reg(mm_x64_buf &buf, int reg) {
    if (reg & 8) buf.bytes.push_back(rex(false, 0, 0, reg));
    put(buf, {0xFF, modrm(3, 4, reg)});
}

void mm_x64_emit_cmovcc(mm_x64_buf &buf, mm_x64_cc cc, int dst, int src) {
    put(buf, {rex(true, dst, 0, src), 0x0F, static_cast<std::uint8_t>(0x40 | static_cast<std::uint8_t>(cc)), modrm(3, dst, src)});
}

void mm_x64_emit_bt_imm(mm_x64_buf &buf, int reg, std::uint8_t bit) {
    put(buf, {rex(true, 0, 0, reg), 0x0F, 0xBA, modrm(3, 4, reg), bit});
}

void mm_x64_emit_pushf(mm_x64_buf &buf) { put(buf, {0x9C}); }
void mm_x64_emit_popf(mm_x64_buf &buf)  { put(buf, {0x9D}); }
void mm_x64_emit_cmc(mm_x64_buf &buf)   { put(buf, {0xF5}); }
void mm_x64_emit_stc(mm_x64_buf &buf)   { put(buf, {0xF9}); }

void mm_x64_emit_push_mem(mm_x64_buf &buf, int base, std::int32_t disp) {
    if (base & 8) buf.bytes.push_back(rex(false, 0, 0, base));
    buf.bytes.push_back(0xFF);
    mem_disp32(buf, 6, base, disp);
}

void mm_x64_emit_pop_mem(mm_x64_buf &buf, int base, std::int32_t disp) {
    if (base & 8) buf.bytes.push_back(rex(false, 0, 0, base));
    buf.bytes.push_back(0x8F);
    mem_disp32(buf, 0, base, disp);
}

/* push/pop of a whole register and an indirect call: used by the watch
 * hook, which has to leave the host stack exactly as it found it. */
void mm_x64_emit_push_reg(mm_x64_buf &buf, int reg) {
    if (reg & 8) buf.bytes.push_back(rex(false, 0, 0, reg));
    buf.bytes.push_back(static_cast<std::uint8_t>(0x50 + (reg & 7)));
}

void mm_x64_emit_pop_reg(mm_x64_buf &buf, int reg) {
    if (reg & 8) buf.bytes.push_back(rex(false, 0, 0, reg));
    buf.bytes.push_back(static_cast<std::uint8_t>(0x58 + (reg & 7)));
}

void mm_x64_emit_call_reg(mm_x64_buf &buf, int reg) {
    if (reg & 8) buf.bytes.push_back(rex(false, 0, 0, reg));
    put(buf, {0xFF, static_cast<std::uint8_t>(0xD0 + (reg & 7))});
}

/* crc32 r32/r64, r/m (SSE4.2): the Castagnoli polynomial, which is what
 * AArch64's CRC32C* instructions compute. `size` is the operand width in
 * bytes; 8 makes it the 64-bit form accumulating into a 64-bit register. */
void mm_x64_emit_crc32(mm_x64_buf &buf, int dst, int src, int size) {
    buf.bytes.push_back(0xF2);
    if (size == 2) buf.bytes.push_back(0x66);
    const bool w = size == 8;
    if (w || (dst & 8) || (src & 8)) buf.bytes.push_back(rex(w, dst, 0, src));
    put(buf, {0x0F, 0x38, static_cast<std::uint8_t>(size == 1 ? 0xF0 : 0xF1),
              static_cast<std::uint8_t>(0xC0 | ((dst & 7) << 3) | (src & 7))});
}

void mm_x64_emit_mov_store_imm32(mm_x64_buf &buf, int base, std::int32_t disp, std::int32_t imm) {
    put(buf, {rex(true, 0, 0, base), 0xC7});
    mem_disp32(buf, 0, base, disp);
    put_imm32(buf, imm);
}

void mm_x64_emit_mov_load_indexed(mm_x64_buf &buf, int dst, int base, int index, int scale) {
    const int ss = scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0;
    /* mod=00 rm=100 -> SIB; base&7==5 (rbp/r13) would mean disp32-only, so use mod=01 disp8=0 then. */
    const bool need_disp8 = (base & 7) == MM_RBP;
    put(buf, {rex(true, dst, index, base), 0x8B, modrm(need_disp8 ? 1 : 0, dst, 4),
              static_cast<std::uint8_t>((ss << 6) | ((index & 7) << 3) | (base & 7))});
    if (need_disp8)
        buf.bytes.push_back(0);
}

void mm_x64_emit_rmw(mm_x64_buf &buf, mm_x64_rmw op, int base, int reg, int size) {
    if (op != mm_x64_rmw::xchg)
        buf.bytes.push_back(0xF0); /* lock */
    if (size == 2)
        buf.bytes.push_back(0x66);
    buf.bytes.push_back(rex(size == 8, reg, 0, base)); /* always: byte regs 4-7 need it */
    switch (op) {
    case mm_x64_rmw::cmpxchg: put(buf, {0x0F, static_cast<std::uint8_t>(size == 1 ? 0xB0 : 0xB1)}); break;
    case mm_x64_rmw::xadd:    put(buf, {0x0F, static_cast<std::uint8_t>(size == 1 ? 0xC0 : 0xC1)}); break;
    case mm_x64_rmw::xchg:    buf.bytes.push_back(size == 1 ? 0x86 : 0x87); break;
    }
    mem_disp32(buf, reg, base, 0);
}

void mm_x64_emit_cmpxchg_pair(mm_x64_buf &buf, int base, bool wide) {
    buf.bytes.push_back(0xF0); /* lock */
    buf.bytes.push_back(rex(wide, 1, 0, base));
    put(buf, {0x0F, 0xC7});
    mem_disp32(buf, 1, base, 0); /* /1 */
}

void mm_x64_emit_setcc(mm_x64_buf &buf, mm_x64_cc cc, int reg8) {
    put(buf, {rex(false, 0, 0, reg8), 0x0F, static_cast<std::uint8_t>(0x90 | static_cast<std::uint8_t>(cc)), modrm(3, 0, reg8)});
}

void mm_x64_patch_rel8_to(mm_x64_buf &buf, std::size_t rel8_at, std::size_t target) {
    const auto dist = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(rel8_at + 1);
    buf.bytes[rel8_at] = static_cast<std::uint8_t>(static_cast<std::int8_t>(dist));
}

void mm_x64_emit_shrd_imm(mm_x64_buf &buf, int dst, int src, std::uint8_t n, bool w64) {
    put(buf, {rex(w64, src, 0, dst), 0x0F, 0xAC, modrm(3, src, dst), n});
}
void mm_x64_emit_mfence(mm_x64_buf &buf) { put(buf, {0x0F, 0xAE, 0xF0}); }
void mm_x64_emit_rdtsc(mm_x64_buf &buf) { put(buf, {0x0F, 0x31}); }

namespace {

struct sse_enc { std::uint8_t map; std::uint8_t op; }; /* map: 1 = 0F, 2 = 0F 38; all 66-prefixed */

sse_enc sse_table(mm_x64_sse op) {
    switch (op) {
    case mm_x64_sse::paddb: return {1, 0xFC};   case mm_x64_sse::paddw: return {1, 0xFD};
    case mm_x64_sse::paddd: return {1, 0xFE};   case mm_x64_sse::paddq: return {1, 0xD4};
    case mm_x64_sse::psubb: return {1, 0xF8};   case mm_x64_sse::psubw: return {1, 0xF9};
    case mm_x64_sse::psubd: return {1, 0xFA};   case mm_x64_sse::psubq: return {1, 0xFB};
    case mm_x64_sse::pcmpeqb: return {1, 0x74}; case mm_x64_sse::pcmpeqw: return {1, 0x75};
    case mm_x64_sse::pcmpeqd: return {1, 0x76}; case mm_x64_sse::pcmpeqq: return {2, 0x29};
    case mm_x64_sse::pcmpgtb: return {1, 0x64}; case mm_x64_sse::pcmpgtw: return {1, 0x65};
    case mm_x64_sse::pcmpgtd: return {1, 0x66}; case mm_x64_sse::pcmpgtq: return {2, 0x37};
    case mm_x64_sse::pmaxub: return {1, 0xDE};  case mm_x64_sse::pmaxuw: return {2, 0x3E}; case mm_x64_sse::pmaxud: return {2, 0x3F};
    case mm_x64_sse::pminub: return {1, 0xDA};  case mm_x64_sse::pminuw: return {2, 0x3A}; case mm_x64_sse::pminud: return {2, 0x3B};
    case mm_x64_sse::pmaxsb: return {2, 0x3C};  case mm_x64_sse::pmaxsw: return {1, 0xEE}; case mm_x64_sse::pmaxsd: return {2, 0x3D};
    case mm_x64_sse::pminsb: return {2, 0x38};  case mm_x64_sse::pminsw: return {1, 0xEA}; case mm_x64_sse::pminsd: return {2, 0x39};
    case mm_x64_sse::pand: return {1, 0xDB};    case mm_x64_sse::pandn: return {1, 0xDF};
    case mm_x64_sse::por: return {1, 0xEB};     case mm_x64_sse::pxor: return {1, 0xEF};
    case mm_x64_sse::packuswb: return {1, 0x67}; case mm_x64_sse::packusdw: return {2, 0x2B};
    case mm_x64_sse::packsswb: return {1, 0x63}; case mm_x64_sse::packssdw: return {1, 0x6B};
    case mm_x64_sse::punpcklbw: return {1, 0x60}; case mm_x64_sse::punpcklwd: return {1, 0x61};
    case mm_x64_sse::punpckldq: return {1, 0x62}; case mm_x64_sse::punpcklqdq: return {1, 0x6C};
    case mm_x64_sse::punpckhbw: return {1, 0x68}; case mm_x64_sse::punpckhwd: return {1, 0x69};
    case mm_x64_sse::punpckhdq: return {1, 0x6A}; case mm_x64_sse::punpckhqdq: return {1, 0x6D};
    case mm_x64_sse::pshufb: return {2, 0x00};  case mm_x64_sse::pmullw: return {1, 0xD5};
    case mm_x64_sse::pmulld: return {2, 0x40};  case mm_x64_sse::pmuludq: return {1, 0xF4};
    case mm_x64_sse::pabsb: return {2, 0x1C};   case mm_x64_sse::pabsw: return {2, 0x1D}; case mm_x64_sse::pabsd: return {2, 0x1E};
    case mm_x64_sse::phaddw: return {2, 0x01};  case mm_x64_sse::phaddd: return {2, 0x02};
    case mm_x64_sse::pmovzxbw: return {2, 0x30}; case mm_x64_sse::pmovzxwd: return {2, 0x33}; case mm_x64_sse::pmovzxdq: return {2, 0x35};
    case mm_x64_sse::pmovsxbw: return {2, 0x20}; case mm_x64_sse::pmovsxwd: return {2, 0x23}; case mm_x64_sse::pmovsxdq: return {2, 0x25};
    case mm_x64_sse::movdqa: return {1, 0x6F};
    case mm_x64_sse::paddusb: return {1, 0xDC}; case mm_x64_sse::paddusw: return {1, 0xDD};
    case mm_x64_sse::psubusb: return {1, 0xD8}; case mm_x64_sse::psubusw: return {1, 0xD9};
    case mm_x64_sse::paddsb: return {1, 0xEC};  case mm_x64_sse::paddsw: return {1, 0xED};
    case mm_x64_sse::psubsb: return {1, 0xE8};  case mm_x64_sse::psubsw: return {1, 0xE9};
    case mm_x64_sse::pavgb: return {1, 0xE0};   case mm_x64_sse::pavgw: return {1, 0xE3};
    }
    return {1, 0x90};
}

void sse_prefix(mm_x64_buf &buf, std::uint8_t mandatory, int reg, int rm, bool w = false) {
    buf.bytes.push_back(mandatory);
    if (w || (reg | rm) & 8)
        buf.bytes.push_back(rex(w, reg, 0, rm));
}

} // namespace

void mm_x64_emit_sse_rr(mm_x64_buf &buf, mm_x64_sse op, int dst, int src) {
    const sse_enc e = sse_table(op);
    sse_prefix(buf, 0x66, dst, src);
    buf.bytes.push_back(0x0F);
    if (e.map == 2) buf.bytes.push_back(0x38);
    put(buf, {e.op, modrm(3, dst, src)});
}

void mm_x64_emit_palignr(mm_x64_buf &buf, int dst, int src, std::uint8_t imm) {
    sse_prefix(buf, 0x66, dst, src);
    put(buf, {0x0F, 0x3A, 0x0F, modrm(3, dst, src), imm});
}

void mm_x64_emit_pshufd(mm_x64_buf &buf, int dst, int src, std::uint8_t imm) {
    sse_prefix(buf, 0x66, dst, src);
    put(buf, {0x0F, 0x70, modrm(3, dst, src), imm});
}
void mm_x64_emit_pshuflw(mm_x64_buf &buf, int dst, int src, std::uint8_t imm) {
    sse_prefix(buf, 0xF2, dst, src);
    put(buf, {0x0F, 0x70, modrm(3, dst, src), imm});
}
void mm_x64_emit_pshufhw(mm_x64_buf &buf, int dst, int src, std::uint8_t imm) {
    sse_prefix(buf, 0xF3, dst, src);
    put(buf, {0x0F, 0x70, modrm(3, dst, src), imm});
}

void mm_x64_emit_sse_shift_imm(mm_x64_buf &buf, mm_x64_vshift op, int reg, std::uint8_t imm) {
    std::uint8_t opcode, ext;
    switch (op) {
    case mm_x64_vshift::psrlw: opcode = 0x71; ext = 2; break;
    case mm_x64_vshift::psraw: opcode = 0x71; ext = 4; break;
    case mm_x64_vshift::psllw: opcode = 0x71; ext = 6; break;
    case mm_x64_vshift::psrld: opcode = 0x72; ext = 2; break;
    case mm_x64_vshift::psrad: opcode = 0x72; ext = 4; break;
    case mm_x64_vshift::pslld: opcode = 0x72; ext = 6; break;
    case mm_x64_vshift::psrlq: opcode = 0x73; ext = 2; break;
    case mm_x64_vshift::psllq: opcode = 0x73; ext = 6; break;
    case mm_x64_vshift::psrldq: opcode = 0x73; ext = 3; break;
    default:                   opcode = 0x73; ext = 7; break; /* pslldq */
    }
    sse_prefix(buf, 0x66, 0, reg);
    put(buf, {0x0F, opcode, modrm(3, ext, reg), imm});
}

void mm_x64_emit_movdqu_load(mm_x64_buf &buf, int xmm, int base, std::int32_t disp) {
    sse_prefix(buf, 0xF3, xmm, base);
    put(buf, {0x0F, 0x6F});
    mem_disp32(buf, xmm, base, disp);
}
void mm_x64_emit_movdqu_store(mm_x64_buf &buf, int base, std::int32_t disp, int xmm) {
    sse_prefix(buf, 0xF3, xmm, base);
    put(buf, {0x0F, 0x7F});
    mem_disp32(buf, xmm, base, disp);
}
void mm_x64_emit_movq_load(mm_x64_buf &buf, int xmm, int base, std::int32_t disp) {
    sse_prefix(buf, 0xF3, xmm, base);
    put(buf, {0x0F, 0x7E});
    mem_disp32(buf, xmm, base, disp);
}
void mm_x64_emit_movq_store(mm_x64_buf &buf, int base, std::int32_t disp, int xmm) {
    sse_prefix(buf, 0x66, xmm, base);
    put(buf, {0x0F, 0xD6});
    mem_disp32(buf, xmm, base, disp);
}
void mm_x64_emit_movq_from_gpr(mm_x64_buf &buf, int xmm, int r64) {
    buf.bytes.push_back(0x66);
    put(buf, {rex(true, xmm, 0, r64), 0x0F, 0x6E, modrm(3, xmm, r64)});
}
void mm_x64_emit_movq_to_gpr(mm_x64_buf &buf, int r64, int xmm) {
    buf.bytes.push_back(0x66);
    put(buf, {rex(true, xmm, 0, r64), 0x0F, 0x7E, modrm(3, xmm, r64)});
}
void mm_x64_emit_pinsrq(mm_x64_buf &buf, int xmm, int r64, std::uint8_t idx) {
    buf.bytes.push_back(0x66);
    put(buf, {rex(true, xmm, 0, r64), 0x0F, 0x3A, 0x22, modrm(3, xmm, r64), idx});
}
void mm_x64_emit_pextrq(mm_x64_buf &buf, int r64, int xmm, std::uint8_t idx) {
    buf.bytes.push_back(0x66);
    put(buf, {rex(true, xmm, 0, r64), 0x0F, 0x3A, 0x16, modrm(3, xmm, r64), idx});
}
void mm_x64_emit_pinsrb(mm_x64_buf &buf, int xmm, int r32, std::uint8_t idx) {
    sse_prefix(buf, 0x66, xmm, r32);
    put(buf, {0x0F, 0x3A, 0x20, modrm(3, xmm, r32), idx});
}
void mm_x64_emit_pinsrw(mm_x64_buf &buf, int xmm, int r32, std::uint8_t idx) {
    sse_prefix(buf, 0x66, xmm, r32);
    put(buf, {0x0F, 0xC4, modrm(3, xmm, r32), idx});
}
void mm_x64_emit_pinsrd(mm_x64_buf &buf, int xmm, int r32, std::uint8_t idx) {
    sse_prefix(buf, 0x66, xmm, r32);
    put(buf, {0x0F, 0x3A, 0x22, modrm(3, xmm, r32), idx});
}
void mm_x64_emit_pmovmskb(mm_x64_buf &buf, int r32, int xmm) {
    sse_prefix(buf, 0x66, r32, xmm);
    put(buf, {0x0F, 0xD7, modrm(3, r32, xmm)});
}

namespace {
void fp_prefix(mm_x64_buf &buf, mm_x64_fk k, int reg, int rm, bool w = false) {
    switch (k) {
    case mm_x64_fk::ss: buf.bytes.push_back(0xF3); break;
    case mm_x64_fk::sd: buf.bytes.push_back(0xF2); break;
    case mm_x64_fk::ps: break;
    case mm_x64_fk::pd: buf.bytes.push_back(0x66); break;
    }
    if (w || (reg | rm) & 8)
        buf.bytes.push_back(rex(w, reg, 0, rm));
}
} // namespace

void mm_x64_emit_fp_rr(mm_x64_buf &buf, mm_x64_fp op, mm_x64_fk k, int dst, int src) {
    std::uint8_t opcode;
    mm_x64_fk kk = k;
    switch (op) {
    case mm_x64_fp::add:  opcode = 0x58; break;
    case mm_x64_fp::sub:  opcode = 0x5C; break;
    case mm_x64_fp::mul:  opcode = 0x59; break;
    case mm_x64_fp::div:  opcode = 0x5E; break;
    case mm_x64_fp::min:  opcode = 0x5D; break;
    case mm_x64_fp::max:  opcode = 0x5F; break;
    case mm_x64_fp::sqrt: opcode = 0x51; break;
    case mm_x64_fp::mov:  opcode = 0x10; break;
    case mm_x64_fp::and_: opcode = 0x54; kk = k == mm_x64_fk::pd || k == mm_x64_fk::sd ? mm_x64_fk::pd : mm_x64_fk::ps; break;
    case mm_x64_fp::andn: opcode = 0x55; kk = k == mm_x64_fk::pd || k == mm_x64_fk::sd ? mm_x64_fk::pd : mm_x64_fk::ps; break;
    case mm_x64_fp::or_:  opcode = 0x56; kk = k == mm_x64_fk::pd || k == mm_x64_fk::sd ? mm_x64_fk::pd : mm_x64_fk::ps; break;
    case mm_x64_fp::xor_: opcode = 0x57; kk = k == mm_x64_fk::pd || k == mm_x64_fk::sd ? mm_x64_fk::pd : mm_x64_fk::ps; break;
    case mm_x64_fp::ucomi: opcode = 0x2E; kk = k == mm_x64_fk::sd || k == mm_x64_fk::pd ? mm_x64_fk::pd : mm_x64_fk::ps; break;
    case mm_x64_fp::unpckl: opcode = 0x14; kk = k == mm_x64_fk::pd || k == mm_x64_fk::sd ? mm_x64_fk::pd : mm_x64_fk::ps; break;
    case mm_x64_fp::cvt_widen:  opcode = 0x5A; kk = k == mm_x64_fk::ss ? mm_x64_fk::ss : mm_x64_fk::ps; break; /* cvtss2sd / cvtps2pd */
    case mm_x64_fp::cvt_narrow: opcode = 0x5A; kk = k == mm_x64_fk::sd ? mm_x64_fk::sd : mm_x64_fk::pd; break; /* cvtsd2ss / cvtpd2ps */
    }
    fp_prefix(buf, kk, dst, src);
    put(buf, {0x0F, opcode, modrm(3, dst, src)});
}

void mm_x64_emit_round(mm_x64_buf &buf, mm_x64_fk k, int dst, int src, std::uint8_t mode) {
    const std::uint8_t op = k == mm_x64_fk::ps ? 0x08 : k == mm_x64_fk::pd ? 0x09 : k == mm_x64_fk::ss ? 0x0A : 0x0B;
    sse_prefix(buf, 0x66, dst, src);
    put(buf, {0x0F, 0x3A, op, modrm(3, dst, src), mode});
}

void mm_x64_emit_cmpfp(mm_x64_buf &buf, mm_x64_fk k, int dst, int src, std::uint8_t pred) {
    fp_prefix(buf, k, dst, src);
    put(buf, {0x0F, 0xC2, modrm(3, dst, src), pred});
}

void mm_x64_emit_cvtsi2fp(mm_x64_buf &buf, bool is_double, int xmm, int r, bool w64) {
    buf.bytes.push_back(is_double ? 0xF2 : 0xF3);
    buf.bytes.push_back(rex(w64, xmm, 0, r));
    put(buf, {0x0F, 0x2A, modrm(3, xmm, r)});
}

void mm_x64_emit_cvttfp2si(mm_x64_buf &buf, bool is_double, int r, int xmm, bool w64) {
    buf.bytes.push_back(is_double ? 0xF2 : 0xF3);
    buf.bytes.push_back(rex(w64, r, 0, xmm));
    put(buf, {0x0F, 0x2C, modrm(3, r, xmm)});
}

void mm_x64_emit_cvtdq2ps(mm_x64_buf &buf, int dst, int src)  { fp_prefix(buf, mm_x64_fk::ps, dst, src); put(buf, {0x0F, 0x5B, modrm(3, dst, src)}); }
void mm_x64_emit_cvtdq2pd(mm_x64_buf &buf, int dst, int src)  { fp_prefix(buf, mm_x64_fk::ss, dst, src); put(buf, {0x0F, 0xE6, modrm(3, dst, src)}); }
void mm_x64_emit_cvttps2dq(mm_x64_buf &buf, int dst, int src) { fp_prefix(buf, mm_x64_fk::ss, dst, src); put(buf, {0x0F, 0x5B, modrm(3, dst, src)}); }
void mm_x64_emit_cvttpd2dq(mm_x64_buf &buf, int dst, int src) { fp_prefix(buf, mm_x64_fk::pd, dst, src); put(buf, {0x0F, 0xE6, modrm(3, dst, src)}); }

void mm_x64_emit_movd_from_gpr(mm_x64_buf &buf, int xmm, int r32) {
    sse_prefix(buf, 0x66, xmm, r32);
    put(buf, {0x0F, 0x6E, modrm(3, xmm, r32)});
}
void mm_x64_emit_movd_to_gpr(mm_x64_buf &buf, int r32, int xmm) {
    sse_prefix(buf, 0x66, xmm, r32);
    put(buf, {0x0F, 0x7E, modrm(3, xmm, r32)});
}

namespace {
/* VEX3: C4 [~R ~X ~B mmmmm] [W ~vvvv L pp] opcode modrm; map 2 = 0F38, pp 1 = 66 */
void vex_fma(mm_x64_buf &buf, std::uint8_t opcode, bool w, int dst, int src1, int src2) {
    const unsigned d = static_cast<unsigned>(dst), s1 = static_cast<unsigned>(src1), s2 = static_cast<unsigned>(src2);
    const std::uint8_t b1 = static_cast<std::uint8_t>(((~(d >> 3) & 1u) << 7) | (1u << 6) | ((~(s2 >> 3) & 1u) << 5) | 2u);
    const std::uint8_t b2 = static_cast<std::uint8_t>(((w ? 1u : 0u) << 7) | ((~s1 & 0xFu) << 3) | 1u);
    put(buf, {0xC4, b1, b2, opcode, modrm(3, dst, src2)});
}
} // namespace

void mm_x64_emit_fma213(mm_x64_buf &buf, mm_x64_fma op, bool is_double, int dst, int src1, int src2) {
    std::uint8_t opc;
    switch (op) {
    case mm_x64_fma::fmadd:  opc = 0xA9; break;
    case mm_x64_fma::fmsub:  opc = 0xAB; break;
    case mm_x64_fma::fnmadd: opc = 0xAD; break;
    default:                 opc = 0xAF; break;
    }
    vex_fma(buf, opc, is_double, dst, src1, src2);
}

void mm_x64_emit_fma231(mm_x64_buf &buf, mm_x64_fma op, bool is_double, bool packed, int dst, int src1, int src2) {
    std::uint8_t opc;
    switch (op) {
    case mm_x64_fma::fmadd:  opc = packed ? 0xB8 : 0xB9; break;
    case mm_x64_fma::fmsub:  opc = packed ? 0xBA : 0xBB; break;
    case mm_x64_fma::fnmadd: opc = packed ? 0xBC : 0xBD; break;
    default:                 opc = packed ? 0xBE : 0xBF; break;
    }
    vex_fma(buf, opc, is_double, dst, src1, src2);
}

void mm_x64_emit_ldmxcsr(mm_x64_buf &buf, int base, std::int32_t disp) {
    if (base & 8) buf.bytes.push_back(rex(false, 0, 0, base));
    put(buf, {0x0F, 0xAE});
    mem_disp32(buf, 2, base, disp);
}
void mm_x64_emit_stmxcsr(mm_x64_buf &buf, int base, std::int32_t disp) {
    if (base & 8) buf.bytes.push_back(rex(false, 0, 0, base));
    put(buf, {0x0F, 0xAE});
    mem_disp32(buf, 3, base, disp);
}

void mm_x64_emit_push(mm_x64_buf &buf, int reg) {
    if (reg & 8) buf.bytes.push_back(rex(false, 0, 0, reg));
    buf.bytes.push_back(static_cast<std::uint8_t>(0x50 | (reg & 7)));
}

void mm_x64_emit_pop(mm_x64_buf &buf, int reg) {
    if (reg & 8) buf.bytes.push_back(rex(false, 0, 0, reg));
    buf.bytes.push_back(static_cast<std::uint8_t>(0x58 | (reg & 7)));
}

void mm_x64_emit_call_rel32(mm_x64_buf &buf, std::int32_t rel) {
    buf.bytes.push_back(0xE8);
    put_imm32(buf, rel);
}

void mm_x64_emit_ret(mm_x64_buf &buf) { put(buf, {0xC3}); }
void mm_x64_emit_nop(mm_x64_buf &buf) { put(buf, {0x90}); }
void mm_x64_emit_ud2(mm_x64_buf &buf) { put(buf, {0x0F, 0x0B}); }

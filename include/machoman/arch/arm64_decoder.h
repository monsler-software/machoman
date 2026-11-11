/* Minimal AArch64 A64 instruction decoder feeding the AOT translator.
 * Scope is intentionally narrow at this stage: enough opcodes to translate
 * simple leaf functions (arithmetic, moves, branches, loads/stores, ret)
 * end to end and prove the pipeline. Widening ISA coverage is ongoing
 * work; unrecognized encodings decode to mm_a64_opcode::unknown rather than
 * crashing the translator, so partial binaries still produce a report.
 *
 * Anything the decoder does not model exactly (extended-register forms,
 * ROR shifts, 32-bit ldr/str, ...) must decode as `unknown` — a wrong
 * translation marked as supported is worse than an honest gap. */
#ifndef MACHOMAN_ARM64_DECODER_H
#define MACHOMAN_ARM64_DECODER_H

#include <cstdint>
#include <span>

enum class mm_a64_opcode : std::uint8_t {
    unknown = 0,
    movz,
    movk,
    movn,
    add_imm,
    sub_imm,
    add_reg,
    sub_reg,
    add_ext,   /* add/sub (extended register): rm is extended per `extend` then shifted by ext_shift */
    sub_ext,
    and_reg,
    orr_reg,
    eor_reg,
    and_imm,   /* logical (immediate): mask already decoded into imm */
    orr_imm,
    eor_imm,
    madd,      /* rd = ra +/- rn * rm, see mulk / negate_product */
    udiv,
    sdiv,
    crc32,     /* CRC32B/H/W/X: the Ethernet polynomial; imm = byte width */
    crc32c,    /* CRC32CB/H/W/X: Castagnoli, which is x86's crc32 */
    lslv,      /* shift by register */
    lsrv,
    asrv,
    rorv,
    rev,       /* byte reverse of the whole register */
    rev16,     /* byte reverse within each halfword */
    rev32,     /* byte reverse within each word (64-bit only) */
    rbit,
    clz,
    cls,
    extr,      /* rd = (rn:rm) >> imms */
    barrier,   /* dmb/dsb/isb: full fence */
    mrs,       /* rd <- system register named by `sysreg` */
    msr,       /* system register <- rd(=Rt) */
    sbfm,      /* bitfield moves: immr/imms */
    bfm,
    ubfm,
    ldr,   /* all integer load forms: sizes, sign-extension and addressing in the fields below */
    str,
    ldp,
    stp,
    adr,
    adrp,
    fmov_to_gpr,   /* rd(x/w) <- Vn element (elem_index 0/1 of 64-bit, or 32-bit lane 0) */
    fmov_from_gpr, /* Vd element <- rn (writes the rest of Vd to zero unless elem_index == 1) */
    ins_gen,       /* Vd.T[elem_index] <- rn, other lanes preserved */
    umov,          /* rd <- Vn.T[elem_index], zero-extended */
    smov,          /* rd <- Vn.T[elem_index], sign-extended (is64 selects Wd/Xd) */
    dup_gen,       /* Vd.T[] <- rn replicated (q selects 64/128-bit) */
    movi,          /* Vd <- imm (64-bit pattern in imm, replicated to 128 if q); mvni pre-applied */
    vlogic,        /* Vd <- Vn op Vm bitwise; op in `vlogic`, q selects width */
    vop,           /* AdvSIMD integer data processing; see `vop`, elem_size (source lane), q, imm */
    fop,           /* floating point, scalar (fvec=false) or vector (fvec=true, q); see `fop`, elem_size = 4/8 */
    ld1_multi,     /* ld1 {Vt..Vt+n-1}, [rn]: n in rt2, whole registers (q), consecutive in memory */
    st1_multi,
    ld1_lane,      /* ld1 {Vt.T}[elem_index], [rn] */
    st1_lane,
    ld1r,          /* ld1r {Vt.T}, [rn]: load one element and replicate */
    ldn_multi,     /* ld2/ld3/ld4 {Vt..Vt+n-1}.T, [rn]: rt2 = n, memory is interleaved by element */
    stn_multi,     /* st2/st3/st4 {Vt..Vt+n-1}.T, [rn] */
    ldn_lane,      /* ld2/ld3/ld4 {Vt..Vt+n-1}.T[elem_index], [rn] */
    stn_lane,      /* st2/st3/st4 {Vt..Vt+n-1}.T[elem_index], [rn] */
    ldnr,          /* ld2r/ld3r/ld4r {Vt..Vt+n-1}.T, [rn] */
    casp,          /* casp Xs,Xs+1, Xt,Xt+1, [Xn]: atomic compare and swap of a register pair */
    eor3,          /* eor3 Vd.16b, Vn.16b, Vm.16b, Va.16b: three-way exclusive or */
    bcax,          /* bcax Vd.16b, Vn.16b, Vm.16b, Va.16b: Vn EOR (Vm AND NOT Va) */
    crypto,        /* ARMv8 crypto extension; the instruction in `crypto` */
    simd_helper,   /* validated AdvSIMD semantics implemented by a portable lane helper */
    ldar,      /* load-acquire (also ldapr): plain sized load on x86 */
    stlr,      /* store-release: xchg */
    ldxr,      /* load-exclusive (ldxr/ldaxr): load + set monitor */
    stxr,      /* store-exclusive (stxr/stlxr): cmpxchg against monitor; rt2 = status register Ws */
    cas,       /* compare-and-swap (all orderings): rt2 = Rs (expected in, old out), rd = Rt (new) */
    atomic,    /* LSE ld<op>/swp: rt2 = Rs (operand), rd = Rt (receives old value); op in `atomic` */
    csel,      /* rd = cond ? rn : rm        (cond in `cond`) */
    csinc,     /* rd = cond ? rn : rm + 1 */
    csinv,     /* rd = cond ? rn : ~rm */
    csneg,     /* rd = cond ? rn : -rm */
    ccmp,      /* flags = cond ? cmp(rn, op) : nzcv;  op = rm, or imm if ccmp_imm */
    adc,       /* rd = rn + rm + C */
    sbc,       /* rd = rn - rm - !C */
    b,         /* imm = absolute target VA for all direct branches */
    bl,
    b_cond,
    cbz,
    cbnz,
    tbz,       /* bit index in imms */
    tbnz,
    br,        /* rn */
    blr,
    ret,
    svc,       /* imm16 in imm */
    brk,
    nop,
};

enum class mm_a64_shift : std::uint8_t { lsl = 0, lsr = 1, asr = 2, ror = 3 };

enum class mm_a64_addr : std::uint8_t {
    offset,     /* [rn, #imm] */
    pre_index,  /* [rn, #imm]!  : rn += imm, then access */
    post_index, /* [rn], #imm   : access, then rn += imm */
    reg_offset, /* [rn, rm, ext #shift] */
    literal,    /* absolute VA in imm (pc-relative form already resolved) */
    post_index_reg, /* [rn], xm : access, then rn += xm */
};

enum class mm_a64_extend : std::uint8_t {
    uxtb = 0, uxth = 1, uxtw = 2, uxtx = 3, sxtb = 4, sxth = 5, sxtw = 6, sxtx = 7,
};

/* System registers the translator models (encoded op0:op1:CRn:CRm:op2 in `imm`). */
enum class mm_a64_sysreg : std::uint8_t { unknown, nzcv, fpcr, fpsr, tpidr_el0, tpidrro_el0, cntvct_el0, cntfrq_el0, dczid_el0, ctr_el0 };

enum class mm_a64_atomic : std::uint8_t { add, clr, eor, set, smax, smin, umax, umin, swp };
enum class mm_a64_vlogic : std::uint8_t { and_, bic, orr, orn, eor, bsl, bit, bif };

/* ARMv8 crypto extension instructions. All operate on whole 128-bit
 * registers (sha1h's destination is scalar but zeroes the upper lanes, as
 * every scalar write does). */
enum class mm_a64_crypto : std::uint8_t {
    sha1h,      /* Sd = ror(Sn<31:0>, 2), upper lanes zeroed */
    sha1su1,    /* schedule update 1 over (Vd, Vn) */
    sha256su0,  /* Vd += sigma0 over (Vd, Vn) */
    aese,       /* Vd = SubBytes(ShiftRows(Vd ^ Vn)) */
    aesd,       /* Vd = InvSubBytes(InvShiftRows(Vd ^ Vn)) */
    aesmc,      /* Vd = MixColumns(Vn) */
    aesimc,     /* Vd = InvMixColumns(Vn) */
    sha1c,      /* four SHA-1 choose rounds over (Vd, Sn, Vm) */
    sha1p,      /* ... parity rounds */
    sha1m,      /* ... majority rounds */
    sha1su0,    /* schedule update 0 over (Vd, Vn, Vm) */
    sha256h,    /* four SHA-256 rounds, Vd <- working (a,b,c,d) */
    sha256h2,   /* four SHA-256 rounds, Vd <- working (e,f,g,h) */
    sha256su1,  /* Vd += sigma1 over (Vd, Vn, Vm) */
};

/* Floating-point operations. elem_size is the FP width (4 = single, 8 =
 * double). Scalar forms write the whole V register (rest zero). */
enum class mm_a64_fop : std::uint8_t {
    /* one source */
    fmov, fabs, fneg, fsqrt, fcvt /* to width in imm */, frintn, frintp, frintm, frintz, frinta, frintx, frinti,
    /* two sources */
    fmul, fdiv, fadd, fsub, fmax, fmin, fmaxnm, fminnm, fnmul,
    /* three sources: rd = f(rn*rm, ra) with ra in `ra` */
    fmadd, fmsub, fnmadd, fnmsub,
    /* flags / selects */
    fcmp, fcmp0, fccmp, fcsel, fmov_imm /* bits in imm */,
    /* conversions (is64 = integer width, fp_signed = signedness; rounding in `frint`-style suffix) */
    fcvtzs, fcvtzu, fcvtns, fcvtnu, fcvtps, fcvtpu, fcvtms, fcvtmu, fcvtas, fcvtau, scvtf, ucvtf,
    /* vector only */
    fcmeq, fcmge, fcmgt, fcmeq0, fcmge0, fcmgt0, fcmle0, fcmlt0, fmla, fmls, faddp, fmaxp, fminp,
    fcvtl, fcvtl2, fcvtn, fcvtn2, fmul_elem, fmla_elem, fmls_elem, fabd, fmov_gpr_lane /* unused */,
};

/* AdvSIMD integer operations. `elem_size` is always the SOURCE lane width;
 * narrowing ops produce elem_size/2 lanes, widening ones elem_size*2. */
enum class mm_a64_vop : std::uint8_t {
    /* rd = rn op rm */
    add, sub, mul, mla, mls, cmeq, cmgt, cmge, cmhi, cmhs, cmtst, smax, smin, umax, umin,
    uhadd, shadd, urhadd, srhadd, uhsub, shsub,   /* halving add/sub, rounding or not */
    uabd, sabd, uaba, saba,                       /* absolute difference, accumulating or not */
    urshr, srshr,                                 /* rounding shift right by immediate */
    /* Widening and narrowing (three-different). "2" reads the upper half of
     * the source registers; elem_size is always the *narrow* element. */
    uaddl, uaddl2, saddl, saddl2, usubl, usubl2, ssubl, ssubl2,
    uaddw, uaddw2, saddw, saddw2, usubw, usubw2, ssubw, ssubw2,
    umull_v, umull2_v, smull_v, smull2_v, umlal, umlal2, smlal, smlal2,
    umlsl, umlsl2, smlsl, smlsl2, uabdl, uabdl2, sabdl, sabdl2,
    addhn, addhn2, raddhn, raddhn2, subhn, subhn2, rsubhn, rsubhn2,
    uqadd, sqadd, uqsub, sqsub, addp, smaxp, sminp, umaxp, uminp, ushl, sshl,
    /* rd = op rn */
    not_, neg, abs, cmeq0, cmgt0, cmge0, cmlt0, cmle0, rev64, rev32, rev16, cnt, xtn, xtn2,
    sqxtn, sqxtn2, uqxtn, uqxtn2, sqxtun, sqxtun2, uaddlp, saddlp, uadalp, sadalp,
    /* rd = rn shifted by imm (imm = shift amount) */
    sshr, ushr, shl, sli, sri, ssra, usra, shrn, shrn2, sshll, sshll2, ushll, ushll2,
    /* scalar rd = reduce(rn) */
    addv, smaxv, sminv, umaxv, uminv, uaddlv,
    /* permutes: rd = f(rn, rm); ext uses imm as the byte index */
    ext, tbl1, tbl2, zip1, zip2, uzp1, uzp2, trn1, trn2,
    udot, sdot,    /* rd.4s += dot(rn.16b, rm.16b) per 4-byte group */
    dup_elem,      /* rd = rn[elem_index] replicated */
    ins_elem,      /* rd[elem_index] = rn[imm] */
};

/* Widening/high-half multiplies of the 3-source group. */
enum class mm_a64_mul : std::uint8_t { full, smull, umull, smulh, umulh };

/* A decoded instruction in a register/immediate form independent of the
 * raw A64 bit encoding, ready to feed the translator.
 *
 * Register 31 means SP or XZR depending on the instruction; the translator
 * resolves that per opcode (add/sub immediate and ldr/str bases use SP,
 * everything else uses XZR). */
struct mm_a64_insn {
    mm_a64_opcode op = mm_a64_opcode::unknown;
    std::uint32_t raw = 0;             /* original 32-bit encoding, for diagnostics */
    std::uint8_t  rd = 0, rn = 0, rm = 0; /* register operands, 0-31 */
    std::int64_t  imm = 0;             /* immediate or branch displacement (in bytes) */
    bool          is64 = true;         /* sf: 64-bit (x) vs 32-bit (w) form */
    bool          sets_flags = false;  /* the "S" variants, e.g. SUBS */
    mm_a64_shift  shift = mm_a64_shift::lsl; /* applied to rm for register forms */
    std::uint8_t  shift_amount = 0;
    bool          invert_rm = false;   /* logical N bit: bic/orn/eon */

    std::uint8_t  ra = 31;             /* accumulator of madd/msub (31 = zero) */
    mm_a64_mul    mulk = mm_a64_mul::full;
    bool          negate_product = false; /* msub and friends */
    std::uint8_t  immr = 0, imms = 0;  /* bitfield moves; imms doubles as the tbz/tbnz bit index */
    std::uint8_t  cond = 14;           /* A64 condition code (0 = EQ ... 14 = AL) */
    std::uint8_t  nzcv = 0;            /* ccmp: flags to set when cond is false */
    bool          ccmp_imm = false;    /* ccmp/ccmn: second operand is imm5 in `imm` */
    bool          ccmn = false;        /* ccmn: compare by addition */
    mm_a64_atomic atomic = mm_a64_atomic::add;
    mm_a64_sysreg sysreg = mm_a64_sysreg::unknown;

    /* SIMD&FP */
    bool          simd = false;        /* ld/st: rd (and rt2) name V registers; mem_size may be 16 */
    bool          q = false;           /* 128-bit vector form */
    bool          simd_imm_merge = false; /* ORR/BIC modified immediate preserves destination bits */
    std::uint8_t  elem_size = 0;       /* element width in bytes for lane ops */
    std::uint8_t  elem_index = 0;
    mm_a64_vlogic vlogic = mm_a64_vlogic::and_;
    mm_a64_vop    vop = mm_a64_vop::add;
    mm_a64_crypto crypto = mm_a64_crypto::sha1h;
    mm_a64_fop    fop = mm_a64_fop::fmov;
    bool          fvec = false;        /* fop: vector (AdvSIMD) rather than scalar form */
    bool          simd_scalar = false; /* scalar AdvSIMD form (e.g. scvtf s0, s1 or sqxtn b0, h1) */

    /* Load/store only. `is64` gives the destination width for sign-extending
     * loads; unsigned loads zero-extend to 64 bits regardless. */
    std::uint8_t  rt2 = 0;             /* second register of ldp/stp */
    std::uint8_t  mem_size = 0;        /* access size in bytes: 1, 2, 4, 8 */
    bool          mem_signed = false;  /* ldrsb/ldrsh/ldrsw/ldpsw */
    mm_a64_addr   addr = mm_a64_addr::offset;
    mm_a64_extend extend = mm_a64_extend::uxtx; /* reg_offset / *_ext: how rm is extended */
    std::uint8_t  ext_shift = 0;               /* shift applied after extend */
};

/* Decodes one 4-byte A64 instruction at guest address `pc`. `bytes` must
 * hold >= 4 bytes. `pc` is only consulted by PC-relative forms (adr/adrp,
 * ldr literal), whose targets are resolved to absolute VAs in `imm`. */
mm_a64_insn mm_a64_decode(std::span<const std::uint8_t> bytes, std::uint64_t pc);

const char *mm_a64_opcode_name(mm_a64_opcode op);

/* Coarse class of a raw A64 word per the ARM ARM top-level encoding
 * tables (e.g. "ldst.pair", "dp.reg.cond_select"), independent of whether
 * the decoder understands it. Used for coverage reports. */
const char *mm_a64_class_name(std::uint32_t raw);

#endif /* MACHOMAN_ARM64_DECODER_H */

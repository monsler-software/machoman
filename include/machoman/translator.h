/* Layer 2: the AOT translator — a "reverse Rosetta 2". Where Rosetta 2
 * translates x86_64 -> arm64 to run Intel Mac binaries on Apple Silicon,
 * this walks an arm64 Mach-O's executable segments ahead of time and
 * produces x86_64 machine code, so layer 3 can run it natively on a
 * Windows/Linux x86_64 host with no per-instruction interpretation at
 * run time. Static/AOT (not a JIT) by design: translate once at venv
 * build/import time, cache the result, run it many times.
 *
 * Execution model of the emitted code:
 *   - The guest register file lives in memory (mm_cpu_context). Host r15
 *     points at it for the whole run; every translated instruction loads
 *     its operands from there and stores its result back. This costs a few
 *     memory ops per instruction but makes the translation exact: no
 *     aliasing between guest registers, XZR/SP resolved per opcode, and
 *     the host's callee-saved registers untouched except r15.
 *   - rax, rcx and rdx are scratch. Nothing else on the host is clobbered.
 *   - Guest SP is context.x[31]; the host stack is never used by guest code.
 *   - NZCV lives in context.eflags as a host EFLAGS image, saved with
 *     pushf right after every flag-setting guest instruction and restored
 *     with popf right before every consumer. Convention: CF holds NOT C
 *     (x86 "borrow" sense), so a sub/cmp saves its flags as-is, an add/cmn
 *     saves after `cmc`, and a logical op after `stc`. Every A64 condition
 *     then maps to a single jcc/cmovcc.
 *   - Direct branches become rel32 jumps to the translated target. Indirect
 *     ones (ret/br/blr) go through a dispatch stub that maps the guest VA to
 *     a host address via context.branch_table. Anything that cannot
 *     continue (VA outside the text, unsupported instruction, svc, brk,
 *     return to the x30 == 0 sentinel) writes exit_reason/fault_va and
 *     returns to the runtime trampoline.
 *
 * The whole __text is translated in one pass, so each guest instruction
 * has a known output offset and branch targets never need a CFG. */
#ifndef MACHOMAN_TRANSLATOR_H
#define MACHOMAN_TRANSLATOR_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "machoman/arch/arm64_decoder.h"
#include "machoman/arch/x86_64_encoder.h"

/* Why translated code stopped and returned to the host. */
enum class mm_exit_reason : std::uint64_t {
    running = 0,
    returned,     /* guest jumped to VA 0, the return-to-host sentinel placed in x30 */
    unsupported,  /* reached an instruction with no translation; fault_va = its VA */
    bad_target,   /* indirect/direct branch to a VA outside the translated text; fault_va = target */
    syscall,      /* svc #0: fault_va = VA of the svc, x16 = Darwin syscall number */
    brk,          /* brk #imm at fault_va */
    memory_fault, /* host SIGSEGV/SIGBUS inside translated code; fault_va = guest insn, fault_addr = data address */
    exited,       /* a host function implementing exit()/_exit() ran; exit_code holds the status */
    host_abort,   /* a host function decided the guest cannot continue (abort, stack check failure, ...) */
};

/* Guest register file and translator/runtime shared state. Layout is ABI
 * for emitted code (offsets are taken with offsetof in translator.cpp):
 * register n is at byte offset n * 8 from the context base. */
/* Layout of the process-wide code map the dispatch stub walks: guest VAs
 * are limited to MM_CODE_MAP_VA_BITS; L1 (context.branch_table) has one
 * entry per 2^MM_CODE_MAP_PAGE_BITS-byte page holding the address of an L2
 * page, which has one host address per 4-byte guest instruction (0 = no
 * translation). Every translated image registers its instructions here, so
 * indirect branches between images cost the same as within one. */
inline constexpr int MM_CODE_MAP_VA_BITS = 40;
inline constexpr int MM_CODE_MAP_PAGE_BITS = 14;
inline constexpr std::size_t MM_CODE_MAP_L1_ENTRIES = std::size_t{1} << (MM_CODE_MAP_VA_BITS - MM_CODE_MAP_PAGE_BITS);
inline constexpr std::size_t MM_CODE_MAP_L2_ENTRIES = std::size_t{1} << (MM_CODE_MAP_PAGE_BITS - 2);

struct alignas(16) mm_cpu_context {
    std::uint64_t x[32] = {};          /* x[31] is SP */
    std::uint64_t eflags = 0x2;        /* guest NZCV stored as a host EFLAGS image, see translator.h header */
    std::uint64_t fault_va = 0;
    std::uint64_t exit_reason = 0;     /* mm_exit_reason */
    std::uint64_t fault_addr = 0;      /* memory_fault: the inaccessible data address */
    std::uint64_t entry_va = 0;        /* guest VA the runtime trampoline enters at */
    std::uint64_t resume_va = 0;       /* if a host function sets this, the guest resumes here instead of x[30] */
    std::uint64_t host_tail_fn = 0;    /* if a host function sets this, the runtime calls that host function next with the same registers */
    std::uint64_t exit_code = 0;       /* exited: status passed to exit() */
    std::uint64_t excl_addr = 0;       /* exclusive monitor set by ldxr, checked by stxr */
    std::uint64_t excl_value = 0;
    std::uint64_t fpcr = 0, fpsr = 0;   /* FPCR.RMode and FPCR.FZ are mirrored into MXCSR on msr */
    std::uint32_t mxcsr = 0x1F80;       /* host MXCSR image derived from fpcr; loaded with ldmxcsr */
    std::uint32_t pad_ = 0;
    std::uint64_t tpidr_el0 = 0, tpidrro_el0 = 0;
    alignas(16) std::uint64_t v[32][2] = {}; /* SIMD&FP registers, little-endian 128-bit: v[n][0] = bits 63:0 */
    const std::uint64_t *branch_table = nullptr; /* L1 of the code map (see MM_CODE_MAP_*) */
};

struct mm_translate_note {
    std::uint64_t source_offset; /* byte offset within the input __text this insn came from */
    std::uint64_t output_offset; /* byte offset within result.code where this insn's encoding starts */
    mm_a64_opcode op;
    bool          supported;     /* false if the decoder/encoder couldn't handle it */
};

struct mm_translation_result {
    mm_x64_buf                     code;  /* emitted x86_64 machine code: shared stubs, then instructions */
    std::vector<mm_translate_note> notes; /* one per source instruction */
    std::size_t   unsupported_count = 0;
    std::uint64_t text_base = 0;          /* guest VA of instruction 0 */
    std::size_t   dispatch_stub = 0;      /* offset in code of the "jump to guest VA in rax" stub */
};

/* Translates `arm64_code` (expected to be a whole function or __text
 * section, 4-byte aligned, located at guest VA `base_va`) into x86_64
 * machine code appended to result.code. PC-relative instructions
 * (adr/adrp, ldr literal) resolve against base_va, so it must be the
 * section's real `addr` for the emitted addresses to mean anything.
 * Returns the number of source instructions processed;
 * result.unsupported_count tracks how many had no x86_64 encoding and
 * were emitted as a `ud2` trap instead, which is the metric to drive down
 * as ISA coverage grows. */
std::size_t mm_translate_buffer(std::span<const std::uint8_t> arm64_code, std::uint64_t base_va,
                                mm_translation_result &result);

#endif /* MACHOMAN_TRANSLATOR_H */

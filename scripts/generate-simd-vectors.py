#!/usr/bin/env python3
"""Generate offline ARM64 execution vectors using Unicorn 2.1.4.

Input: raw/count/address rows from work/inspect_simd_gaps.cpp. The generated
fixture needs no emulator at test time. Unicorn is a developer dependency.
"""
import argparse
import re
from pathlib import Path
import capstone
import unicorn
from unicorn import arm64_const as arm

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('words', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
cpu = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
cpu.ctl_set_cpu_model(arm.UC_CPU_ARM64_MAX)
cpu.mem_map(0x1000, 0x1000)
mask = (1 << 64) - 1

def inputs(seed, kind):
    state = seed
    out = []
    patterns = {
        1: (4, [0, 0x80000000, 0x3f800000, 0x40000000, 0x7f800000, 0xff800000, 0x7fc00001, 0x7f800001, 1, 0x00800000, 0x7f7fffff, 0x3eaaaaab]),
        2: (2, [0, 0x8000, 0x3c00, 0x4000, 0x7c00, 0xfc00, 0x7e01, 0x7c01, 1, 0x0400, 0x7bff, 0x3555]),
        3: (8, [0, 1 << 63, 0x3ff0000000000000, 0x4000000000000000, 0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000001, 0x7ff0000000000001, 1, 0x0010000000000000, 0x7fefffffffffffff, 0x3fd5555555555555])
    }
    for reg in range(32):
        value = bytearray()
        for _ in range(2):
            state ^= (state << 13) & mask
            state ^= state >> 7
            state ^= (state << 17) & mask
            value.extend(state.to_bytes(8, 'little'))
        if kind:
            size, pattern = patterns[kind]
            for lane in range(16 // size):
                value[lane*size:(lane+1)*size] = pattern[(reg + lane + seed) % len(pattern)].to_bytes(size, 'little')
        out.append(int.from_bytes(value, 'little'))
    gprs = []
    for reg in range(29):
        state ^= (state << 13) & mask
        state ^= state >> 7
        state ^= (state << 17) & mask
        gprs.append(state)
    return out, gprs

rows = ['# Unicorn 2.1.4 ARM64 MAX: word seed input_kind fpcr gpr_dest expected_lo expected_hi fpsr nzcv']
skipped = []
for line in args.words.read_text().splitlines():
    word = int(line.split()[0], 16)
    decoded = list(md.disasm(word.to_bytes(4, 'little'), 0x1000))
    if not decoded:
        skipped.append(f'{word:08x} unallocated')
        continue
    insn = decoded[0]
    kind = 0
    if insn.mnemonic.startswith('f'):
        kind = 2 if '.4h' in insn.op_str or '.8h' in insn.op_str or re.search(r'\bh\d+\b', insn.op_str) else 3 if '.2d' in insn.op_str else 1
        if insn.mnemonic == 'fcvtl' and '.4s' in insn.op_str: kind = 2
        if insn.mnemonic == 'fcvtn' and '.4s' in insn.op_str: kind = 1
        if insn.mnemonic == 'fcvt':
            source = insn.op_str.split(',')[-1].strip()
            kind = 2 if source.startswith('h') else 3 if source.startswith('d') else 1
    cases = [(0x123456789abcdef, 0, 0), (0xdedbeef, 0, 0)]
    if kind:
        cases += [(offset + 1, kind, 0) for offset in range(12)]
        for control in [1 << 22, 2 << 22, 3 << 22, 1 << 25, 1 << 24, 1 << 19]:
            cases += [(2, kind, control), (0xdedbeef, 0, control)]
    for seed, mode, control in cases:
        vectors, gprs = inputs(seed, mode)
        for reg, value in enumerate(vectors): cpu.reg_write(getattr(arm, f'UC_ARM64_REG_Q{reg}'), value)
        for reg, value in enumerate(gprs): cpu.reg_write(getattr(arm, f'UC_ARM64_REG_X{reg}'), value)
        for reg in [29, 30]: cpu.reg_write(getattr(arm, f'UC_ARM64_REG_X{reg}'), 0)
        cpu.reg_write(arm.UC_ARM64_REG_SP, 0)
        cpu.reg_write(arm.UC_ARM64_REG_FPCR, control)
        cpu.reg_write(arm.UC_ARM64_REG_FPSR, (1 << 27) if seed & 1 else 0)
        cpu.reg_write(arm.UC_ARM64_REG_NZCV, 1 << 29)
        cpu.mem_write(0x1000, word.to_bytes(4, 'little'))
        cpu.ctl_remove_cache(0x1000, 0x1004)
        try: cpu.emu_start(0x1000, 0x1004, count=1)
        except unicorn.UcError:
            skipped.append(f'{word:08x} {insn.mnemonic} {insn.op_str}')
            break
        is_gpr = bool(re.match(r'^[wx](?:\d+|zr)\b', insn.op_str))
        result = cpu.reg_read(getattr(arm, f'UC_ARM64_REG_X{word & 31}')) if is_gpr and word & 31 != 31 else 0 if is_gpr else cpu.reg_read(getattr(arm, f'UC_ARM64_REG_Q{word & 31}'))
        fpsr = cpu.reg_read(arm.UC_ARM64_REG_FPSR)
        nzcv = cpu.reg_read(arm.UC_ARM64_REG_NZCV)
        rows.append(f'{word:08x} {seed:016x} {mode} {control:08x} {int(is_gpr)} {result & mask:016x} {result >> 64:016x} {fpsr:08x} {nzcv:08x}')
args.output.write_text('\n'.join(rows) + '\n')
args.output.with_suffix('.skipped').write_text('\n'.join(skipped) + '\n')
print(f'{len(rows)-1} vectors, {len(set(skipped))} invalid/unrecognized words')

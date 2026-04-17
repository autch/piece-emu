#!/usr/bin/env python3
"""Generate shift / rotate / swap / mirror / scan{0,1} regression tests.

Coverage:
  Shifts:    srl, sll, sra, sla   (imm4 form and reg form)
  Rotates:   rr, rl               (imm4 form and reg form)
  Misc:      swap, mirror         (no flags)
             scan0, scan1         (C↔ V=0 Z↔ N=0)

Notes:
  - Shifts/rotates do NOT support ext (per CPU manual).
  - imm4 encoding: 0..7 → shift by 0..7; 1xxx → shift by 8.  We test up to 8.
  - reg-form: rs holds count.  We test rs ∈ {0..8}; behaviour for rs > 8 is
    out of scope here (the manual restricts rs to 0..8).
  - Shifts/rotates affect only Z and N (C and V unchanged).
  - swap and mirror affect no flags.
  - scan0/scan1 set C=1 when the requested bit is not found in the upper byte.

Each test runs the op in inline asm, captures result + PSR, and compares
against the Python-computed oracle.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

HERE = Path(__file__).resolve().parent

PSR_N = 1 << 0
PSR_Z = 1 << 1
PSR_V = 1 << 2
PSR_C = 1 << 3
MASK = 0xFFFFFFFF
SIGN_BIT = 0x80000000


def clamp_count(n: int) -> int:
    """S1C33 shifts/rotates: 0..7 = exact, ≥8 → 8 (per imm4 encoding)."""
    return min(n & 0xFF, 8)


def srl(rs: int, n: int) -> int:
    n = clamp_count(n)
    return (rs & MASK) >> n


def sll(rs: int, n: int) -> int:
    n = clamp_count(n)
    return (rs << n) & MASK


def sra(rs: int, n: int) -> int:
    n = clamp_count(n)
    s = rs - (1 << 32) if rs & SIGN_BIT else rs
    return (s >> n) & MASK


def sla(rs: int, n: int) -> int:
    # Same numeric result as sll; differs only in flag handling on some ISAs,
    # but the S1C33 manual lists identical Z/N effects.
    return sll(rs, n)


def rr(rs: int, n: int) -> int:
    n = clamp_count(n)
    if n == 0:
        return rs & MASK
    rs &= MASK
    return ((rs >> n) | (rs << (32 - n))) & MASK


def rl(rs: int, n: int) -> int:
    n = clamp_count(n)
    if n == 0:
        return rs & MASK
    rs &= MASK
    return ((rs << n) | (rs >> (32 - n))) & MASK


def nz_flags(result: int) -> int:
    f = 0
    if result & SIGN_BIT: f |= PSR_N
    if (result & MASK) == 0: f |= PSR_Z
    return f


def swap_op(rs: int) -> tuple[int, int]:
    rs &= MASK
    b0 = (rs >>  0) & 0xFF
    b1 = (rs >>  8) & 0xFF
    b2 = (rs >> 16) & 0xFF
    b3 = (rs >> 24) & 0xFF
    return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3, 0  # no flags


def _bit_reverse_byte(b: int) -> int:
    r = 0
    for i in range(8):
        if b & (1 << i):
            r |= 1 << (7 - i)
    return r


def mirror_op(rs: int) -> tuple[int, int]:
    rs &= MASK
    b0 = _bit_reverse_byte((rs >>  0) & 0xFF)
    b1 = _bit_reverse_byte((rs >>  8) & 0xFF)
    b2 = _bit_reverse_byte((rs >> 16) & 0xFF)
    b3 = _bit_reverse_byte((rs >> 24) & 0xFF)
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24), 0


def scan0_op(rs: int) -> tuple[int, int]:
    rs &= MASK
    upper = (rs >> 24) & 0xFF
    for i in range(8):
        if not (upper & (1 << (7 - i))):
            r = i
            f = nz_flags(r) & (PSR_Z)        # only Z; C=0; V=0; N=0
            return r, f
    # not found → result=8, C=1, Z=0, N=0, V=0
    return 8, PSR_C


def scan1_op(rs: int) -> tuple[int, int]:
    rs &= MASK
    upper = (rs >> 24) & 0xFF
    for i in range(8):
        if upper & (1 << (7 - i)):
            r = i
            f = nz_flags(r) & PSR_Z
            return r, f
    return 8, PSR_C


# ---------------------------------------------------------------------------
@dataclass
class ShiftOp:
    name: str
    func: Callable[[int, int], int]
    op_idx: int


SHIFT_OPS: list[ShiftOp] = [
    ShiftOp("srl", srl, 0),
    ShiftOp("sll", sll, 1),
    ShiftOp("sra", sra, 2),
    ShiftOp("sla", sla, 3),
    ShiftOp("rr",  rr,  4),
    ShiftOp("rl",  rl,  5),
]

# Distinct misc ops: each has its own oracle that returns (result, flags).
@dataclass
class MiscOp:
    name: str
    func: Callable[[int], tuple[int, int]]
    flag_mask: int       # which PSR bits to compare
    op_idx: int


MISC_OPS: list[MiscOp] = [
    MiscOp("swap",   swap_op,   0,                              6),
    MiscOp("mirror", mirror_op, 0,                              7),
    MiscOp("scan0",  scan0_op,  PSR_N | PSR_Z | PSR_V | PSR_C,  8),
    MiscOp("scan1",  scan1_op,  PSR_N | PSR_Z | PSR_V | PSR_C,  9),
]


# Test inputs
RS_VALUES: list[int] = [
    0, 1, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF,
    0x55555555, 0xAAAAAAAA, 0x12345678, 0xDEADBEEF,
]
# imm4 form: assembler accepts only 1..8 (rejects 0 even though encoding allows it).
# We test reg-form with 0 since that's a runtime value the assembler can't catch.
SHIFT_AMOUNTS: list[int] = [1, 2, 3, 4, 5, 7, 8]

# scan needs upper-byte patterns
SCAN_VALUES: list[int] = [
    0x00000000,  # all zero — scan0 returns 0, scan1 returns 8 (C=1)
    0xFF000000,  # upper FF — scan0 returns 8 (C=1), scan1 returns 0
    0x80000000,  # 1000 0000 in upper byte
    0x01000000,  # 0000 0001 in upper byte (scan1 finds bit at offset 7)
    0x7F000000,  # 0111 1111 in upper byte (scan0 at offset 0; scan1 at offset 1)
    0xFE000000,  # 1111 1110 in upper byte (scan0 at offset 7; scan1 at offset 0)
    0xC3000000,  # 1100 0011 — mixed pattern
    0xAAAAAAAA,  # 1010 1010 in upper byte
]


def case_id(op_idx: int, form_idx: int, val_idx: int) -> int:
    return 1 + (op_idx * 1000) + (form_idx * 100) + val_idx


def _asm_block(*lines: str) -> str:
    return "\n        ".join(f'"{ln}\\n"' for ln in lines)


def emit_shift_imm(buf: list[str], op: ShiftOp) -> int:
    """imm4 form: srl %rd, imm4 (rd is in/out)."""
    n = 0
    form_idx = 0
    for vi, (rs, sh) in enumerate(((rs, sh) for rs in RS_VALUES for sh in SHIFT_AMOUNTS)):
        cid = case_id(op.op_idx, form_idx, vi)
        result = op.func(rs, sh)
        flags = nz_flags(result)
        asm = _asm_block(f"{op.name} %0, {sh}", "ld.w %1, %%psr")
        buf.append(f"""static int case_{cid}(void) {{
    uint32_t rd = (uint32_t){rs:#010x}u;
    uint32_t psr;
    asm volatile(
        {asm}
        : "+r"(rd), "=r"(psr)
        :
    );
    CHECK({cid}, rd == {result:#010x}u);
    CHECK({cid}, (psr & {PSR_N | PSR_Z:#x}) == {flags:#x});
    return 0;
}}
""")
        n += 1
    return n


def emit_shift_reg(buf: list[str], op: ShiftOp) -> int:
    """reg form: srl %rd, %rs (count in rs, range 0..8)."""
    n = 0
    form_idx = 1
    REG_AMOUNTS = [0, 1, 2, 4, 7, 8]
    REG_RS_VALUES = [0, 1, 0xFFFFFFFF, 0x80000000, 0x55555555, 0x12345678]
    for vi, (rs_val, sh) in enumerate(((v, s) for v in REG_RS_VALUES for s in REG_AMOUNTS)):
        cid = case_id(op.op_idx, form_idx, vi)
        result = op.func(rs_val, sh)
        flags = nz_flags(result)
        # `op %rd, %rs` — rd is in/out (+r), rs is the count register (r).
        asm = _asm_block(f"{op.name} %0, %2", "ld.w %1, %%psr")
        buf.append(f"""static int case_{cid}(void) {{
    uint32_t rd = (uint32_t){rs_val:#010x}u;
    uint32_t psr;
    asm volatile(
        {asm}
        : "+r"(rd), "=r"(psr)
        : "r"((uint32_t){sh}u)
    );
    CHECK({cid}, rd == {result:#010x}u);
    CHECK({cid}, (psr & {PSR_N | PSR_Z:#x}) == {flags:#x});
    return 0;
}}
""")
        n += 1
    return n


def emit_misc(buf: list[str], op: MiscOp) -> int:
    """Misc ops take `op %rd, %rs` (rd = output, rs = input)."""
    n = 0
    form_idx = 0
    if op.name in ("scan0", "scan1"):
        values = SCAN_VALUES
    else:
        values = RS_VALUES
    for vi, rs in enumerate(values):
        cid = case_id(op.op_idx, form_idx, vi)
        result, flags = op.func(rs)
        asm = _asm_block(f"{op.name} %0, %2", "ld.w %1, %%psr")
        buf.append(f"""static int case_{cid}(void) {{
    uint32_t rd;
    uint32_t psr;
    asm volatile(
        {asm}
        : "=&r"(rd), "=r"(psr)
        : "r"((uint32_t){rs:#010x}u)
    );
    CHECK({cid}, rd == {result:#010x}u);""" + (
            f"\n    CHECK({cid}, (psr & {op.flag_mask:#x}) == {flags:#x});"
            if op.flag_mask else ""
        ) + """
    return 0;
}
""")
        n += 1
    return n


C_PROLOGUE = """// AUTO-GENERATED by gen_shift.py — do not edit by hand.
//
// Shift / rotate / swap / mirror / scan{0,1} regression tests.

#include <stdint.h>

#define CHECK(id, ok) do { if (!(ok)) return (id); } while (0)
"""

C_EPILOGUE_TEMPLATE = """
int main(void) {{
    int e;
{calls}
    return 0;
}}
"""


def main() -> int:
    out = HERE / "test_gen_shift.c"
    cases: list[str] = []

    for op in SHIFT_OPS:
        emit_shift_imm(cases, op)
        emit_shift_reg(cases, op)
    for op in MISC_OPS:
        emit_misc(cases, op)

    import re
    case_re = re.compile(r"static int case_(\d+)\(void\)")
    cids = [int(m.group(1)) for c in cases for m in [case_re.search(c)] if m]
    calls = [f"    if ((e = case_{cid}())) return e;" for cid in cids]

    out.write_text(
        C_PROLOGUE + "\n" + "\n".join(cases)
        + C_EPILOGUE_TEMPLATE.format(calls="\n".join(calls)),
        encoding="utf-8",
    )
    print(f"# wrote {out} : {len(cids)} cases", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

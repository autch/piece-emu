#!/usr/bin/env python3
"""Generate ALU instruction tests from quick_reference parser output.

Scope (PoC): add, sub, and, or, xor, cmp, not — reg-reg, reg-imm6, +1ext, +2ext
forms.  Each test case fixes input register values and an immediate, asks the
oracle for the expected result and PSR flag bits, and emits an inline-asm
fragment that captures both for runtime comparison.

Output: test_gen_alu.c (one main() running every case, returning case# on
failure, 0 on success).  The case# encodes (op_index, form_index, value_index)
for fast diagnosis.
"""

from __future__ import annotations

import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

HERE = Path(__file__).resolve().parent

# PSR bit layout (verified against existing test_alu_flags.c)
PSR_N = 1 << 0
PSR_Z = 1 << 1
PSR_V = 1 << 2
PSR_C = 1 << 3

MASK = 0xFFFFFFFF
SIGN_BIT = 0x80000000


def to_signed(x: int) -> int:
    x &= MASK
    return x - (1 << 32) if x & SIGN_BIT else x


def add_flags(a: int, b: int) -> tuple[int, int]:
    """Return (result, flag_bits) for a+b under S1C33 ALU semantics."""
    a &= MASK
    b &= MASK
    full = a + b
    res = full & MASK
    n = PSR_N if (res & SIGN_BIT) else 0
    z = PSR_Z if res == 0 else 0
    c = PSR_C if full > MASK else 0
    # signed overflow: same sign in & different sign out
    v = PSR_V if ((~(a ^ b) & (a ^ res)) & SIGN_BIT) else 0
    return res, n | z | v | c


def sub_flags(a: int, b: int) -> tuple[int, int]:
    """Return (result, flag_bits) for a-b under S1C33 ALU semantics."""
    a &= MASK
    b &= MASK
    res = (a - b) & MASK
    n = PSR_N if (res & SIGN_BIT) else 0
    z = PSR_Z if res == 0 else 0
    c = PSR_C if a < b else 0  # borrow
    v = PSR_V if (((a ^ b) & (a ^ res)) & SIGN_BIT) else 0
    return res, n | z | v | c


def and_flags(a: int, b: int) -> tuple[int, int]:
    res = (a & b) & MASK
    n = PSR_N if (res & SIGN_BIT) else 0
    z = PSR_Z if res == 0 else 0
    return res, n | z  # C, V unchanged — caller masks to N|Z


def or_flags(a: int, b: int) -> tuple[int, int]:
    res = (a | b) & MASK
    n = PSR_N if (res & SIGN_BIT) else 0
    z = PSR_Z if res == 0 else 0
    return res, n | z


def xor_flags(a: int, b: int) -> tuple[int, int]:
    res = (a ^ b) & MASK
    n = PSR_N if (res & SIGN_BIT) else 0
    z = PSR_Z if res == 0 else 0
    return res, n | z


def not_flags(_unused_a: int, b: int) -> tuple[int, int]:
    """not %rd, %rs : rd = ~rs.  rs is operand 'b' here; 'a' is unused."""
    res = (~b) & MASK
    n = PSR_N if (res & SIGN_BIT) else 0
    z = PSR_Z if res == 0 else 0
    return res, n | z


def cmp_flags(a: int, b: int) -> tuple[int, int]:
    """cmp updates flags like sub but discards result."""
    _res, flags = sub_flags(a, b)
    return a & MASK, flags  # destination unchanged


@dataclass
class AluOp:
    name: str
    func: Callable[[int, int], tuple[int, int]]
    flag_mask: int            # which flag bits are affected
    imm_signed: bool          # whether the imm6 form treats the immediate as signed
    has_not_form: bool = False  # True for 'not' (rs is operand B; rd ignored as input)
    writes_dest: bool = True    # cmp: False
    has_3op_form: bool = True   # 'not' has only one operand → no 3-op form


OPS: list[AluOp] = [
    AluOp("add", add_flags, PSR_N | PSR_Z | PSR_V | PSR_C, imm_signed=False),
    AluOp("sub", sub_flags, PSR_N | PSR_Z | PSR_V | PSR_C, imm_signed=False),
    AluOp("and", and_flags, PSR_N | PSR_Z,                 imm_signed=True),
    AluOp("or",  or_flags,  PSR_N | PSR_Z,                 imm_signed=True),
    AluOp("xor", xor_flags, PSR_N | PSR_Z,                 imm_signed=True),
    AluOp("cmp", cmp_flags, PSR_N | PSR_Z | PSR_V | PSR_C, imm_signed=True, writes_dest=False),
    AluOp("not", not_flags, PSR_N | PSR_Z,                 imm_signed=True, has_not_form=True, has_3op_form=False),
]


# Input-value sets — chosen for boundary coverage
RR_VALUES: list[tuple[int, int]] = [
    (0, 0),
    (1, 1),
    (1, 0xFFFFFFFE),                # 1 + (-2) = -1
    (0x7FFFFFFF, 1),                # signed overflow boundary
    (0xFFFFFFFF, 1),                # carry boundary
    (0x80000000, 0x80000000),       # both negative
    (0x55555555, 0xAAAAAAAA),       # alternating bits
    (0x12345678, 0x9ABCDEF0),       # general
]

# imm6 ranges:
#   unsigned (add/sub): 0..63
#   signed   (and/or/xor/not/cmp): -32..31
RI_VALUES_UNSIGNED: list[tuple[int, int]] = [
    (0, 0), (1, 0), (0, 63), (10, 5), (0xFFFFFFFF, 1),
    (0x7FFFFFFF, 1), (0x80000000, 63), (100, 50),
]

RI_VALUES_SIGNED: list[tuple[int, int]] = [
    (0, 0), (0, -1), (0xFFFFFFFF, 0), (0xFFFFFFFF, -1),
    (0x55555555, 0x0F), (0xAAAAAAAA, -16), (0x80000000, 31), (0x7FFFFFFF, -32),
]


# ext immediate combinations
# - 2-op +1ext: combined = (ext13 << 6) | imm6   → 19 bits
#   * unsigned-imm form (add/sub): zero_ext(combined) → 32-bit
#   * signed-imm   form (and/or/xor/cmp/not): sign_ext(combined) → 32-bit
# - 2-op +2ext: combined = (extH << 19) | (extL << 6) | imm6 → 32 bits
#
# We pick a few interesting (ext, imm6) pairs.

EXT1_PAIRS: list[tuple[int, int]] = [
    (0x0001, 0),       # smallest non-zero
    (0x1000, 0),       # bit 18 set → signed=negative, unsigned=large positive
    (0x0FFF, 0x3F),    # mid value
    (0x1FFF, 0x3F),    # all 19 bits set (signed = -1)
    (0x0001, 0x3F),    # combined = 0x7F
]

EXT2_PAIRS: list[tuple[int, int, int]] = [
    (0x0000, 0x0001, 0),       # combined = 64
    (0x1FFF, 0x1FFF, 0x3F),    # all 32 bits set
    (0x0001, 0x0000, 0),       # combined = 1<<19
    (0x0000, 0x1FFF, 0x00),    # combined = 0x7FFC0
]


# 3-op form (ext+reg-reg): (rs_value, ext13).  N is unsigned, range [0, 8191].
RR3_EXT1: list[tuple[int, int]] = [
    (0, 0),                    # all zero (Z=1 for sub/and/xor; for or, N=Z=0)
    (50, 100),                 # basic add/sub
    (0xFFFFFFFE, 1),           # boundary: 0xFFFFFFFE + 1 → 0xFFFFFFFF
    (0x00001000, 0x1FFF),      # mid+max ext
    (0xAAAAAAAA, 0x0F0F),      # bit pattern (good for and/or/xor)
    (0x7FFFFFFF, 1),           # signed-overflow boundary for add
    (0xFFFFFFFF, 0x1FFF),      # carry boundary for add (large unsigned)
    (0x80000000, 1),           # sign-bit set
]

# 3-op form 2-ext: (rs_value, extH, extL).  imm = (extH<<13)|extL, 26 bits unsigned.
RR3_EXT2: list[tuple[int, int, int]] = [
    (0, 0x0000, 0x0001),                # imm = 1
    (0, 0x1FFF, 0x1FFF),                # imm = 0x3FFFFFF (26 bits all ones)
    (0xFFFFFFFF, 0x0001, 0x0000),       # imm = 0x2000 (just over ext1 range)
    (0x55555555, 0x0AAA, 0x1555),       # bit-pattern interplay
    (0x00000001, 0x1FFF, 0x1FFF),       # add: 1 + 0x3FFFFFF = 0x4000000
]


def combine_ext1(ext13: int, imm6: int, signed: bool, op_imm_signed: bool) -> int:
    """Compute the effective immediate for ext+op (2-op form)."""
    raw = ((ext13 & 0x1FFF) << 6) | (imm6 & 0x3F)
    if op_imm_signed:
        # 19-bit sign extend
        return raw - (1 << 19) if (raw & (1 << 18)) else raw
    return raw  # zero extend


def combine_ext2(ext_h: int, ext_l: int, imm6: int, op_imm_signed: bool) -> int:
    raw = ((ext_h & 0x1FFF) << 19) | ((ext_l & 0x1FFF) << 6) | (imm6 & 0x3F)
    raw &= MASK
    if op_imm_signed:
        return raw - (1 << 32) if (raw & SIGN_BIT) else raw
    return raw


# ---------------------------------------------------------------------------
# Code emission
# ---------------------------------------------------------------------------

C_PROLOGUE = """// AUTO-GENERATED by gen_alu.py — do not edit by hand.
//
// Generated ALU regression tests covering reg-reg, reg-imm6, and ext+imm
// (1 and 2 ext) forms for add/sub/and/or/xor/cmp/not.
//
// Each test runs the instruction in inline asm, captures the result register
// and PSR, and compares against pre-computed expectations.  On failure, main()
// returns a non-zero case ID encoding (op,form,index).

#include <stdint.h>

#define PSR_N  0x01
#define PSR_Z  0x02
#define PSR_V  0x04
#define PSR_C  0x08

#define CHECK(id, ok) do { if (!(ok)) return (id); } while (0)
"""

C_EPILOGUE_TEMPLATE = """
int main(void) {{
    int e;
{calls}
    return 0;
}}
"""


def case_id(op_idx: int, form_idx: int, val_idx: int) -> int:
    # 8 forms max, 64 values max → fits in 16 bits comfortably
    return 1 + (op_idx * 100 * 100) + (form_idx * 100) + val_idx


def imm6_for_op(imm6: int, op: AluOp) -> int:
    """Render an imm6 bit pattern as the assembler-accepted decimal:
    - unsigned-imm ops (add/sub): 0..63 as-is
    - signed-imm   ops (and/or/xor/cmp/not): -32..31, so flip ≥0x20 to negative
    """
    imm6 &= 0x3F
    if op.imm_signed and (imm6 & 0x20):
        return imm6 - 64
    return imm6


def _asm_lines(*lines: str) -> str:
    """Build a multi-line C inline-asm string literal block.

    Returns text like:  "ext 0x1\\n"
                        "add %0, 0\\n"
                        "ld.w %1, %%psr\\n"
    """
    return "\n        ".join(f'"{ln}\\n"' for ln in lines)


def emit_rr(buf: list[str], op_idx: int, op: AluOp) -> int:
    n = 0
    for vi, (a, b) in enumerate(RR_VALUES):
        cid = case_id(op_idx, 0, vi)
        if op.has_not_form:
            res, flags = op.func(0, b)
            asm = _asm_lines("not %0, %2", "ld.w %1, %%psr")
        else:
            res, flags = op.func(a, b)
            asm = _asm_lines(f"{op.name} %0, %2", "ld.w %1, %%psr")
        inputs = f'"r"((uint32_t){b:#010x}u)'
        flags_masked = flags & op.flag_mask
        init = f'(uint32_t){a:#010x}u'
        buf.append(_emit_case(cid, op, init, asm, inputs, res, flags_masked))
        n += 1
    return n


def emit_ri(buf: list[str], op_idx: int, op: AluOp, form_idx: int) -> int:
    n = 0
    values = RI_VALUES_SIGNED if op.imm_signed else RI_VALUES_UNSIGNED
    for vi, (a, imm) in enumerate(values):
        cid = case_id(op_idx, form_idx, vi)
        if op.has_not_form:
            imm32 = imm & MASK if imm >= 0 else (imm + (1 << 32)) & MASK
            if op.imm_signed and imm < 0:
                # 6-bit sign extend to 32
                imm32 = (imm + (1 << 32)) & MASK
            res, flags = op.func(0, imm32)
            asm = _asm_lines(f"not %0, {imm}", "ld.w %1, %%psr")
        else:
            if op.imm_signed:
                imm32 = imm & MASK if imm >= 0 else (imm + (1 << 32)) & MASK
            else:
                imm32 = imm & 0x3F
            res, flags = op.func(a, imm32)
            asm = _asm_lines(f"{op.name} %0, {imm}", "ld.w %1, %%psr")
        flags_masked = flags & op.flag_mask
        init = f'(uint32_t){a:#010x}u'
        buf.append(_emit_case(cid, op, init, asm, "", res, flags_masked))
        n += 1
    return n


def emit_ri_ext1(buf: list[str], op_idx: int, op: AluOp, form_idx: int) -> int:
    n = 0
    for vi, (ext13, imm6) in enumerate(EXT1_PAIRS):
        cid = case_id(op_idx, form_idx, vi)
        a = 0xFFFFFFFF if op.name == "and" else 0
        b_signed = combine_ext1(ext13, imm6, op.imm_signed, op.imm_signed)
        b32 = b_signed & MASK if b_signed >= 0 else (b_signed + (1 << 32)) & MASK
        imm6_asm = imm6_for_op(imm6, op)
        if op.has_not_form:
            res, flags = op.func(0, b32)
            asm = _asm_lines(f"ext {ext13:#x}", f"not %0, {imm6_asm}", "ld.w %1, %%psr")
        else:
            res, flags = op.func(a, b32)
            asm = _asm_lines(f"ext {ext13:#x}", f"{op.name} %0, {imm6_asm}", "ld.w %1, %%psr")
        flags_masked = flags & op.flag_mask
        init = f'(uint32_t){a:#010x}u'
        buf.append(_emit_case(cid, op, init, asm, "", res, flags_masked))
        n += 1
    return n


def emit_ri_ext2(buf: list[str], op_idx: int, op: AluOp, form_idx: int) -> int:
    n = 0
    for vi, (ext_h, ext_l, imm6) in enumerate(EXT2_PAIRS):
        cid = case_id(op_idx, form_idx, vi)
        a = 0xFFFFFFFF if op.name == "and" else 0
        b_signed = combine_ext2(ext_h, ext_l, imm6, op.imm_signed)
        b32 = b_signed & MASK if b_signed >= 0 else (b_signed + (1 << 32)) & MASK
        imm6_asm = imm6_for_op(imm6, op)
        if op.has_not_form:
            res, flags = op.func(0, b32)
            asm = _asm_lines(
                f"ext {ext_h:#x}", f"ext {ext_l:#x}",
                f"not %0, {imm6_asm}", "ld.w %1, %%psr",
            )
        else:
            res, flags = op.func(a, b32)
            asm = _asm_lines(
                f"ext {ext_h:#x}", f"ext {ext_l:#x}",
                f"{op.name} %0, {imm6_asm}", "ld.w %1, %%psr",
            )
        flags_masked = flags & op.flag_mask
        init = f'(uint32_t){a:#010x}u'
        buf.append(_emit_case(cid, op, init, asm, "", res, flags_masked))
        n += 1
    return n


def emit_rr_ext1(buf: list[str], op_idx: int, op: AluOp, form_idx: int) -> int:
    """3-op form: ext N; op %rd, %rs → rd = rs <op> zero_ext(N)."""
    if not op.has_3op_form:
        return 0
    n = 0
    for vi, (rs_val, ext13) in enumerate(RR3_EXT1):
        cid = case_id(op_idx, form_idx, vi)
        imm = ext13 & 0x1FFF                       # always unsigned in 3-op
        res, flags = op.func(rs_val, imm)
        flags_masked = flags & op.flag_mask
        asm = _asm_lines(f"ext {ext13:#x}", f"{op.name} %0, %2", "ld.w %1, %%psr")
        buf.append(_emit_3op_case(cid, op, rs_val, asm, res, flags_masked))
        n += 1
    return n


def emit_rr_ext2(buf: list[str], op_idx: int, op: AluOp, form_idx: int) -> int:
    """3-op form: ext H; ext L; op %rd, %rs → rd = rs <op> ((H<<13)|L) (26-bit unsigned)."""
    if not op.has_3op_form:
        return 0
    n = 0
    for vi, (rs_val, ext_h, ext_l) in enumerate(RR3_EXT2):
        cid = case_id(op_idx, form_idx, vi)
        imm = ((ext_h & 0x1FFF) << 13) | (ext_l & 0x1FFF)   # unsigned
        res, flags = op.func(rs_val, imm)
        flags_masked = flags & op.flag_mask
        asm = _asm_lines(
            f"ext {ext_h:#x}", f"ext {ext_l:#x}",
            f"{op.name} %0, %2", "ld.w %1, %%psr",
        )
        buf.append(_emit_3op_case(cid, op, rs_val, asm, res, flags_masked))
        n += 1
    return n


def _emit_3op_case(cid: int, op: AluOp, rs_val: int, asm_body: str,
                   expected_res: int, expected_flags: int) -> str:
    """Emit a 3-op case: rd is early-clobber (=&r) so the assembler picks rd != rs.

    For cmp the "result" register is whatever LLVM assigned to rd (its initial
    value is undefined, so we cannot check it).  Only flags are checked for cmp.
    """
    if op.writes_dest:
        value_check = f"    CHECK({cid}, rd == {expected_res:#010x}u);\n"
    else:
        value_check = ""
    return f"""static int case_{cid}(void) {{
    uint32_t rd;
    uint32_t rs = (uint32_t){rs_val:#010x}u;
    uint32_t psr;
    asm volatile(
        {asm_body}
        : "=&r"(rd), "=r"(psr)
        : "r"(rs)
    );
{value_check}    CHECK({cid}, (psr & {op.flag_mask:#x}) == {expected_flags:#x});
    return 0;
}}
"""


def _emit_case(cid: int, op: AluOp, init: str, asm_body: str, inputs: str,
               expected_res: int, expected_flags: int) -> str:
    # cmp: result register is unchanged (still equals 'a'); only check flags.
    if op.writes_dest:
        value_check = f"    CHECK({cid}, rd == {expected_res:#010x}u);\n"
    else:
        value_check = ""
    return f"""static int case_{cid}(void) {{
    uint32_t rd = {init};
    uint32_t psr;
    asm volatile(
        {asm_body}
        : "+r"(rd), "=r"(psr)
        : {inputs}
    );
{value_check}    CHECK({cid}, (psr & {op.flag_mask:#x}) == {expected_flags:#x});
    return 0;
}}
"""


def main() -> int:
    out_path = HERE / "test_gen_alu.c"
    cases: list[str] = []
    calls: list[str] = []
    total = 0

    for op_idx, op in enumerate(OPS):
        # form 0: rr (2-op reg-reg)
        cases_before = len(cases)
        emit_rr(cases, op_idx, op)
        # form 1: ri (2-op reg-imm6)
        emit_ri(cases, op_idx, op, 1)
        # form 2: ri+ext1 (2-op reg-imm6 with 1 ext)
        emit_ri_ext1(cases, op_idx, op, 2)
        # form 3: ri+ext2 (2-op reg-imm6 with 2 ext)
        emit_ri_ext2(cases, op_idx, op, 3)
        # form 4: rr+ext1 (3-op: ext N; op %rd, %rs)
        emit_rr_ext1(cases, op_idx, op, 4)
        # form 5: rr+ext2 (3-op: ext H; ext L; op %rd, %rs)
        emit_rr_ext2(cases, op_idx, op, 5)
        emitted = len(cases) - cases_before
        total += emitted
        # Also gather call lines from the cid embedded in each case
        # Easier: record cids as we go.

    # Re-emit by scanning cases for case_NNN func names
    import re
    case_re = re.compile(r"static int case_(\d+)\(void\)")
    cids = [int(m.group(1)) for c in cases for m in [case_re.search(c)] if m]
    for cid in cids:
        calls.append(f"    if ((e = case_{cid}())) return e;")

    out = C_PROLOGUE + "\n" + "\n".join(cases) + C_EPILOGUE_TEMPLATE.format(
        calls="\n".join(calls)
    )
    out_path.write_text(out, encoding="utf-8")
    print(f"# wrote {out_path} : {total} cases", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

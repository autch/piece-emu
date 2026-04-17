#!/usr/bin/env python3
"""Generate multiply / step-divide regression tests for piece-emu.

Coverage:
  Multiply:
    mlt.h  %rd, %rs   — signed   16×16 → 64 (low in ALR, sign-ext in AHR)
    mltu.h %rd, %rs   — unsigned 16×16 → 32 (low in ALR, AHR=0)
    mlt.w  %rd, %rs   — signed   32×32 → 64 (ALR low, AHR high)
    mltu.w %rd, %rs   — unsigned 32×32 → 64 (ALR low, AHR high)

  Divide (step sequence):
    Unsigned: ld.w %alr, dividend; ld.w %r0, divisor; div0u %r0;
              div1 %r0 ×32 → ALR=quotient, AHR=remainder
    Signed:   ... div0s %r0; div1 %r0 ×32; div2s %r0; div3s
              → ALR=quotient, AHR=remainder

The .h-form multiply behaviour (AHR sign-extended) was determined by reading
test_multiply.c rather than the quick reference — the latter only mentions ALR.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

HERE = Path(__file__).resolve().parent

MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF
SIGN32 = 0x80000000


def s32(x: int) -> int:
    x &= MASK32
    return x - (1 << 32) if x & SIGN32 else x


def u_to_s16(x: int) -> int:
    x &= 0xFFFF
    return x - (1 << 16) if x & 0x8000 else x


# ---------------------------------------------------------------- Multiply oracle

def mlt_h(a: int, b: int) -> tuple[int, int]:
    """signed 16x16 → 64-bit signed result, split into (alr, ahr)."""
    res = u_to_s16(a) * u_to_s16(b)
    res64 = res & MASK64
    return res64 & MASK32, (res64 >> 32) & MASK32


def mltu_h(a: int, b: int) -> tuple[int, int]:
    """unsigned 16x16 → 32-bit result in ALR, AHR = 0."""
    res = (a & 0xFFFF) * (b & 0xFFFF)
    return res & MASK32, 0


def mlt_w(a: int, b: int) -> tuple[int, int]:
    """signed 32x32 → 64-bit signed result, split into (alr, ahr)."""
    res = s32(a) * s32(b)
    res64 = res & MASK64
    return res64 & MASK32, (res64 >> 32) & MASK32


def mltu_w(a: int, b: int) -> tuple[int, int]:
    """unsigned 32x32 → 64-bit unsigned result, split into (alr, ahr)."""
    res = (a & MASK32) * (b & MASK32)
    return res & MASK32, (res >> 32) & MASK32


# ---------------------------------------------------------------- Divide oracle

def divu_step(dividend: int, divisor: int) -> tuple[int, int]:
    """Unsigned step division — matches the result of div0u + 32×div1.
    Returns (quotient, remainder)."""
    if divisor == 0:
        raise ValueError("divide by zero")
    dividend &= MASK32
    divisor  &= MASK32
    return (dividend // divisor) & MASK32, (dividend % divisor) & MASK32


def divs_step(dividend: int, divisor: int) -> tuple[int, int]:
    """Signed step division — matches div0s + 32×div1 + div2s + div3s.
    S1C33 follows truncated division (toward zero), matching C semantics.
    See test_div.c which assertsdiv(-7,2) = -3 r -1, div(7,-2) = -3 r 1, etc.
    """
    a = s32(dividend)
    b = s32(divisor)
    if b == 0:
        raise ValueError("divide by zero")
    # Truncated division: q = trunc(a / b), r = a - q*b
    q = int(a / b)              # python int() truncates toward zero
    r = a - q * b
    return q & MASK32, r & MASK32


# ---------------------------------------------------------------- ops table

@dataclass
class MulOp:
    name: str
    func: Callable[[int, int], tuple[int, int]]
    op_idx: int


MUL_OPS: list[MulOp] = [
    MulOp("mlt.h",  mlt_h,  0),
    MulOp("mltu.h", mltu_h, 1),
    MulOp("mlt.w",  mlt_w,  2),
    MulOp("mltu.w", mltu_w, 3),
]


# ---------------------------------------------------------------- value sets

# .h-form values: cover 16-bit boundaries.  Upper bits set verifies the
# instruction ignores them (only low 16 are multiplied).
H_PAIRS: list[tuple[int, int]] = [
    (0, 0),
    (1, 0),
    (1, 1),
    (3, 4),
    (0xFFFF, 5),                 # signed: -1 * 5 = -5
    (0xFFFF, 0xFFFF),            # signed: -1 * -1 = 1; unsigned: 0xFFFE0001
    (0x7FFF, 0x7FFF),            # signed: max² = 0x3FFF0001
    (0x8000, 0x8000),            # signed: min² (bit pattern: -32768 * -32768 = 0x40000000)
    (0x8000, 0xFFFF),            # signed: min × -1 = -0x80000000 → 0x80000000
    (256, 256),                  # 65536, just exceeds 16-bit
    (0xDEAD0123, 0xBEEF0234),    # high bits set to verify they're ignored: 0x0123 × 0x0234 = 0x00029CFC
    (0x12345678, 0x87654321),    # mixed sign in low 16 (signed 0x5678 × 0x4321 vs 0x5678 × ?...)
]

# .w-form values: cover 32-bit boundaries.
W_PAIRS: list[tuple[int, int]] = [
    (0, 0),
    (1, 0),
    (1, 1),
    (100, 200),
    (0xFFFFFFFF, 1),             # signed -1 × 1 = -1
    (0xFFFFFFFF, 2),             # signed -1 × 2 = -2; unsigned: 0x1_FFFFFFFE
    (0x00010000, 0x00010000),    # 0x100000000 → ALR=0, AHR=1
    (0x7FFFFFFF, 2),             # signed: ~max × 2 = 0xFFFFFFFE; unsigned the same
    (0x80000000, 0x80000000),    # signed: min² = 0x40000000_00000000; unsigned: 0x40000000_00000000
    (0xFFFFFFFF, 0xFFFFFFFF),    # signed: -1 × -1 = 1, AHR=0; unsigned: 0xFFFE_0000_0000_0001
    (0xCAFEBABE, 0xDEADBEEF),    # arbitrary
]


# Divide value sets — small to ensure correctness without sign-overflow edge.
DIV_U_PAIRS: list[tuple[int, int]] = [
    (0, 1),                      # 0 / 1 = 0 r 0
    (1, 1),                      # 1 / 1 = 1 r 0
    (1, 2),                      # 1 / 2 = 0 r 1
    (7, 2),                      # 3 r 1
    (10, 3),                     # 3 r 1
    (100, 7),                    # 14 r 2
    (64, 8),                     # 8 r 0 exact
    (131071, 3),                 # 43690 r 1
    (0xFFFFFFFF, 2),             # 0x7FFFFFFF r 1
    (0xFFFFFFFF, 0xFFFFFFFF),    # 1 r 0
    (0x12345678, 0x100),         # 0x123456 r 0x78
    (0x80000000, 1),             # 0x80000000 r 0
]

DIV_S_PAIRS: list[tuple[int, int]] = [
    (0, 1),                      # 0 r 0
    (1, 1),                      # 1 r 0
    (-1, 1),                     # -1 r 0
    (-7, 2),                     # -3 r -1
    (7, -2),                     # -3 r 1
    (-100, -7),                  # 14 r -2
    (100, -7),                   # -14 r 2
    (0x7FFFFFFF, 1),             # max / 1
    (0x7FFFFFFF, 2),             # 0x3FFFFFFF r 1
    # NOTE: skip INT_MIN / -1 (overflow)
    (-1000, 3),                  # -333 r -1
    (12345, -67),                # -184 r 17 (C trunc)
]


def case_id(op_idx: int, val_idx: int) -> int:
    return 1 + (op_idx * 100) + val_idx


# ---------------------------------------------------------------- emit

# Unrolled 32 div1 instructions (divisor in R0 throughout).
DIV1_X32 = "\n        ".join(['"div1 %%r0\\n"'] * 32)


def emit_mul_case(buf: list[str], op: MulOp, vi: int, a: int, b: int) -> None:
    cid = case_id(op.op_idx, vi)
    alr_exp, ahr_exp = op.func(a, b)
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t alr, ahr;
    asm volatile(
        "ld.w %%r0, %2\\n"
        "ld.w %%r1, %3\\n"
        "{op.name} %%r0, %%r1\\n"
        "ld.w %0, %%alr\\n"
        "ld.w %1, %%ahr\\n"
        : "=r"(alr), "=r"(ahr)
        : "r"((uint32_t){a:#010x}u), "r"((uint32_t){b:#010x}u)
        : "r0", "r1"
    );
    CHECK({cid}, alr == {alr_exp:#010x}u);
    CHECK({cid}, ahr == {ahr_exp:#010x}u);
    return 0;
}}
""")


def emit_divu_case(buf: list[str], vi: int, dividend: int, divisor: int) -> None:
    op_idx = 4
    cid = case_id(op_idx, vi)
    q, r = divu_step(dividend, divisor)
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t alr, ahr;
    asm volatile(
        "ld.w %%r0, %3\\n"            // R0 = divisor
        "ld.w %%r4, %2\\n"            // R4 = dividend (temp)
        "ld.w %%alr, %%r4\\n"         // ALR = dividend
        "div0u %%r0\\n"
        {DIV1_X32}
        "ld.w %0, %%alr\\n"
        "ld.w %1, %%ahr\\n"
        : "=r"(alr), "=r"(ahr)
        : "r"((uint32_t){dividend:#010x}u), "r"((uint32_t){divisor:#010x}u)
        : "r0", "r4"
    );
    CHECK({cid}, alr == {q:#010x}u);
    CHECK({cid}, ahr == {r:#010x}u);
    return 0;
}}
""")


def emit_divs_case(buf: list[str], vi: int, dividend: int, divisor: int) -> None:
    op_idx = 5
    cid = case_id(op_idx, vi)
    q, r = divs_step(dividend, divisor)
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t alr, ahr;
    asm volatile(
        "ld.w %%r0, %3\\n"            // R0 = divisor
        "ld.w %%r4, %2\\n"            // R4 = dividend
        "ld.w %%alr, %%r4\\n"         // ALR = dividend
        "div0s %%r0\\n"
        {DIV1_X32}
        "div2s %%r0\\n"
        "div3s\\n"
        "ld.w %0, %%alr\\n"
        "ld.w %1, %%ahr\\n"
        : "=r"(alr), "=r"(ahr)
        : "r"((int32_t){dividend}), "r"((int32_t){divisor})
        : "r0", "r4"
    );
    CHECK({cid}, alr == {q:#010x}u);
    CHECK({cid}, ahr == {r:#010x}u);
    return 0;
}}
""")


# ---------------------------------------------------------------- main

C_PROLOGUE = """// AUTO-GENERATED by gen_muldiv.py — do not edit by hand.
//
// Multiply / step-divide regression tests.

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
    out = HERE / "test_gen_muldiv.c"
    cases: list[str] = []

    for op in MUL_OPS:
        pairs = H_PAIRS if op.name.endswith(".h") else W_PAIRS
        for vi, (a, b) in enumerate(pairs):
            emit_mul_case(cases, op, vi, a, b)

    for vi, (dvd, dvs) in enumerate(DIV_U_PAIRS):
        emit_divu_case(cases, vi, dvd, dvs)

    for vi, (dvd, dvs) in enumerate(DIV_S_PAIRS):
        emit_divs_case(cases, vi, dvd, dvs)

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

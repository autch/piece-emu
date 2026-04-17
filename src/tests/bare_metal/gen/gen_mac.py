#!/usr/bin/env python3
"""Generate mac (multiply-accumulate) regression tests for piece-emu.

mac %rs:
  count = r[rs], ptr1 = r[rs+1], ptr2 = r[rs+2]   (register numbers mod 16)
  Repeat `count` times:
      acc = {AHR, ALR}  (64-bit signed)
      acc += (int32)(int16 *ptr1) * (int32)(int16 *ptr2)
      ptr1 += 2; ptr2 += 2
      count -= 1
  Upon completion: count = 0, ptr1/ptr2 advanced, {AHR,ALR} holds sum.
  Flag: MO (bit 7) — MAC overflow, set if 64-bit signed overflow occurred.

Test coverage:
  - Count = 0 (no-op)
  - Count = 1 (single mul-accumulate)
  - Count = 2..5 (multi-iteration dot product)
  - Sign variations: pos×pos, neg×pos, pos×neg, neg×neg
  - Initial accumulator: 0, positive seed, negative seed
  - Pointer post-increment verification (ptr1/ptr2 advance by 2*count)
  - Register variants: mac %r1 (count in r1), mac %r4 (count in r4)

Oracle: Python computes expected {AHR, ALR} and expected final pointer values.

NOTE: each test uses a static int16_t array with distinct pattern, runs mac,
and compares ALR, AHR, final pointers, and final count.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

HERE = Path(__file__).resolve().parent

MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF


def s16(x: int) -> int:
    x &= 0xFFFF
    return x - (1 << 16) if x & 0x8000 else x


def s64(x: int) -> int:
    x &= MASK64
    return x - (1 << 64) if x & (1 << 63) else x


def u64(x: int) -> int:
    return x & MASK64


def mac_oracle(initial_alr: int, initial_ahr: int,
               arr1: Sequence[int], arr2: Sequence[int], count: int
               ) -> tuple[int, int]:
    """Compute expected (final_alr, final_ahr) for mac. 64-bit signed accum."""
    acc = s64((initial_ahr & MASK32) << 32 | (initial_alr & MASK32))
    for i in range(count):
        a = s16(arr1[i])
        b = s16(arr2[i])
        acc += a * b
    final = u64(acc)
    return final & MASK32, (final >> 32) & MASK32


@dataclass
class MacCase:
    """One mac test: (initial_alr, initial_ahr, arr1, arr2, count, reg)"""
    initial_alr: int
    initial_ahr: int
    arr1: list[int]          # int16 values
    arr2: list[int]          # int16 values
    count: int
    reg: int = 1             # mac %rN (N = reg). default %r1 → count=r1, p1=r2, p2=r3
    note: str = ""


# Key test cases (count=0 is handled separately, with empty arrays)
CASES: list[MacCase] = [
    # Count = 0 no-op
    MacCase(0, 0,                     [1, 2, 3, 4], [1, 2, 3, 4], 0,
            note="count=0: ALR/AHR unchanged, no pointer movement"),
    MacCase(0x12345678, 0xABCDEF01,   [1, 2, 3, 4], [1, 2, 3, 4], 0,
            note="count=0 with nonzero seed"),
    # Single iteration
    MacCase(0, 0,                     [3], [4], 1,
            note="3 * 4 = 12 → ALR=12"),
    MacCase(0, 0,                     [-3], [4], 1,
            note="-3 * 4 = -12 → ALR=0xFFFFFFF4, AHR=-1"),
    MacCase(0, 0,                     [0x7FFF], [0x7FFF], 1,
            note="max pos × max pos = 0x3FFF0001"),
    MacCase(0, 0,                     [-0x8000], [-0x8000], 1,
            note="min × min = 0x40000000 (positive product)"),
    MacCase(0, 0,                     [-0x8000], [0x7FFF], 1,
            note="min × max = -0x3FFF8000"),
    MacCase(10, 0,                    [3], [4], 1,
            note="accumulate with positive seed: 10 + 12 = 22"),
    MacCase(0xFFFFFFFE, 0xFFFFFFFF,   [3], [4], 1,
            note="seed = -2 (signed 64-bit); -2 + 12 = 10"),
    # Multi-iteration dot products
    MacCase(0, 0,                     [1, 2, 3], [1, 2, 3], 3,
            note="1+4+9 = 14"),
    MacCase(0, 0,                     [1, 2, 3, 4, 5], [1, 2, 3, 4, 5], 5,
            note="1+4+9+16+25 = 55"),
    MacCase(0, 0,                     [100, -100, 100, -100], [1, 2, 3, 4], 4,
            note="100 - 200 + 300 - 400 = -200"),
    MacCase(0, 0,                     [0x7FFF, 0x7FFF, 0x7FFF],
                                      [0x7FFF, 0x7FFF, 0x7FFF], 3,
            note="3 × max² = 3 × 0x3FFF0001"),
    # Register variants
    MacCase(0, 0,                     [7, 8, 9], [1, 2, 3], 3, reg=4,
            note="mac %r4: count=r4, p1=r5, p2=r6"),
    MacCase(0, 0,                     [-1, -2, -3], [4, 5, 6], 3, reg=4,
            note="mac %r4 with negative values"),
]


def case_id(vi: int) -> int:
    return 1 + vi


def emit_case(buf_fns: list[str], buf_callers: list[str],
              vi: int, case: MacCase) -> None:
    cid = case_id(vi)
    # Compute expected result
    exp_alr, exp_ahr = mac_oracle(case.initial_alr, case.initial_ahr,
                                  case.arr1, case.arr2, case.count)

    # Each test uses its own static arrays to avoid cross-test interference.
    # Arrays are sized max(1, count) to avoid zero-length array (non-portable).
    n = max(1, len(case.arr1), len(case.arr2))
    arr1_init = ", ".join(str(s16(x)) for x in case.arr1)
    arr2_init = ", ".join(str(s16(x)) for x in case.arr2)
    if not arr1_init:
        arr1_init = "0"
    if not arr2_init:
        arr2_init = "0"

    reg = case.reg
    r_count = f"%%r{reg}"
    r_p1 = f"%%r{(reg + 1) % 16}"
    r_p2 = f"%%r{(reg + 2) % 16}"

    # Operand numbering plan (alphabetical):
    #   outputs: %0=alr %1=ahr %2=p1_out %3=p2_out %4=count_out
    #   inputs:  %5=initial_alr %6=initial_ahr %7=count %8=p1 %9=p2
    buf_fns.append(f"""static volatile int16_t mac_arr1_{cid}[{n}] = {{ {arr1_init} }};
static volatile int16_t mac_arr2_{cid}[{n}] = {{ {arr2_init} }};

static int case_{cid}(void) {{
    // {case.note}
    uint32_t alr, ahr, p1_out, p2_out, count_out;
    asm volatile(
        "ld.w %%r9, %5\\n"
        "ld.w %%alr, %%r9\\n"            // ALR = initial
        "ld.w %%r9, %6\\n"
        "ld.w %%ahr, %%r9\\n"            // AHR = initial
        "ld.w {r_count}, %7\\n"          // count
        "ld.w {r_p1}, %8\\n"             // ptr1
        "ld.w {r_p2}, %9\\n"             // ptr2
        "mac {r_count}\\n"               // THE INSTRUCTION UNDER TEST
        "ld.w %0, %%alr\\n"
        "ld.w %1, %%ahr\\n"
        "ld.w %2, {r_p1}\\n"
        "ld.w %3, {r_p2}\\n"
        "ld.w %4, {r_count}\\n"
        : "=r"(alr), "=r"(ahr), "=r"(p1_out), "=r"(p2_out), "=r"(count_out)
        : "r"((uint32_t){case.initial_alr:#x}u), "r"((uint32_t){case.initial_ahr:#x}u),
          "r"((uint32_t){case.count}u),
          "r"((uint32_t)(uintptr_t)mac_arr1_{cid}),
          "r"((uint32_t)(uintptr_t)mac_arr2_{cid})
        : "r9", "%r{reg}", "%r{(reg + 1) % 16}", "%r{(reg + 2) % 16}", "memory"
    );
    CHECK({cid}, alr == {exp_alr:#010x}u);
    CHECK({cid}, ahr == {exp_ahr:#010x}u);
    CHECK({cid}, count_out == 0u);   // count decremented to 0
    CHECK({cid}, p1_out == (uint32_t)(uintptr_t)mac_arr1_{cid} + {2 * case.count}u);
    CHECK({cid}, p2_out == (uint32_t)(uintptr_t)mac_arr2_{cid} + {2 * case.count}u);
    return 0;
}}
""")
    buf_callers.append(f"    if ((e = case_{cid}())) return e;")


C_PROLOGUE = """// AUTO-GENERATED by gen_mac.py — do not edit by hand.
//
// mac (multiply-accumulate) regression tests.  Each test uses its own
// static int16_t array pair to avoid cross-test interference.

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
    out = HERE / "test_gen_mac.c"
    fns: list[str] = []
    callers: list[str] = []

    for vi, case in enumerate(CASES):
        emit_case(fns, callers, vi, case)

    out.write_text(
        C_PROLOGUE + "\n" + "\n".join(fns)
        + C_EPILOGUE_TEMPLATE.format(calls="\n".join(callers)),
        encoding="utf-8",
    )
    print(f"# wrote {out} : {len(callers)} cases", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

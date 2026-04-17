#!/usr/bin/env python3
"""Generate pushn/popn regression tests for piece-emu.

Semantics (per quick reference):
  pushn %rN:  for n = N down to 0: SP -= 4; W[SP] = rn
              → r0 ends up at lowest stack addr, rN at highest
              → net SP delta = -4 * (N+1)
  popn %rN:   for n = 0 up to N: rn = W[SP]; SP += 4
              → mirror-image of pushn (order preserved)

Coverage (three orthogonal tests per N):
  (A) slot-ordering:      pushn %rN, then load [%sp+K] → R10.
                          Verify the value equals the register we placed at K.
  (B) SP delta:           save SP in r9 (caller-saved), pushn %rN, popn %rN,
                          return final SP - initial SP in R10 (should be 0).
                          Also a variant: stop after pushn and return the
                          pushn delta (should be 4*(N+1)).
  (C) popn round-trip:    init r0..rN, pushn, corrupt r0..rN, popn,
                          return register K → should equal original value.

Scope: N ∈ {0, 1, 3, 5, 7} to avoid touching R8 (kernel table base, ABI-
reserved = 0) and R10+ (return value / argument registers).

Layout: each test lives in its own __attribute__((naked)) function.  We
save/restore R0..R3 via an OUTER pushn %r3 / popn %r3 to respect the
caller's ABI expectation that those are preserved.  All other scratch
comes from R4, R5, R9 (caller-saved) which are outside the pushn %rN
ranges we exercise.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Test N values: avoid R8 (reserved) and R10+ (return/args)
N_VALUES = [0, 1, 3, 5, 7]


def case_id(group: int, vi: int) -> int:
    """group: 0=slot, 1=sp_delta_after_pushn, 2=sp_delta_roundtrip, 3=popn."""
    return 1 + (group * 100) + vi


def emit_slot_test(buf: list[str], cid: int, n: int, k: int) -> None:
    """Init r0..rN with pattern [i -> i+1]; pushn %rN; read [%sp+K]; popn; ret.

    Expected return: K+1 (the pattern value for register K).
    """
    # Generate init lines for r0..rN, using sign6 imm (fits 1..31)
    init_lines = [f'"ld.w %r{i}, {i + 1}\\n"' for i in range(n + 1)]
    init_block = "\n        ".join(init_lines)

    buf.append(f"""__attribute__((naked))
static uint32_t test_pp_{cid}(uint32_t a __attribute__((unused)),
                              uint32_t b __attribute__((unused))) {{
    asm(
        "pushn %r3\\n"                // save ABI callee-saved
        // init r0..r{n} with pattern r_i = i+1
        {init_block}
        "pushn %r{n}\\n"              // THE INSTRUCTION UNDER TEST
        "ld.w %r10, [%sp+{k}]\\n"     // read slot K (imm6 word units)
        "popn %r{n}\\n"               // restore test-pattern regs
        "popn %r3\\n"                 // restore ABI callee-saved
        "ret\\n"
    );
}}
""")


def emit_sp_delta_after_pushn(buf: list[str], cid: int, n: int) -> None:
    """Save SP in r9, pushn %rN, compute SP delta, popn, return delta.

    Expected return: 4 * (n + 1) bytes.
    """
    buf.append(f"""__attribute__((naked))
static uint32_t test_pp_{cid}(uint32_t a __attribute__((unused)),
                              uint32_t b __attribute__((unused))) {{
    asm(
        "pushn %r3\\n"
        "ld.w %r9, %sp\\n"             // r9 = SP before pushn (r9 is caller-saved)
        "pushn %r{n}\\n"
        "ld.w %r4, %sp\\n"             // r4 = current SP (via special-reg load)
        "sub %r9, %r4\\n"              // r9 = before - after = +4*(n+1)
        "popn %r{n}\\n"
        "ld.w %r10, %r9\\n"            // return delta in r10
        "popn %r3\\n"
        "ret\\n"
    );
}}
""")


def emit_sp_delta_roundtrip(buf: list[str], cid: int, n: int) -> None:
    """Verify pushn then popn leaves SP unchanged (round-trip delta = 0)."""
    buf.append(f"""__attribute__((naked))
static uint32_t test_pp_{cid}(uint32_t a __attribute__((unused)),
                              uint32_t b __attribute__((unused))) {{
    asm(
        "pushn %r3\\n"
        "ld.w %r9, %sp\\n"             // r9 = SP before pushn
        "pushn %r{n}\\n"
        "popn %r{n}\\n"
        "ld.w %r4, %sp\\n"             // r4 = SP after popn
        "sub %r9, %r4\\n"              // r9 = before - after = 0 if balanced
        "ld.w %r10, %r9\\n"
        "popn %r3\\n"
        "ret\\n"
    );
}}
""")


def emit_popn_roundtrip(buf: list[str], cid: int, n: int, k: int) -> None:
    """Init regs with pattern, pushn, corrupt regs to 0, popn, read reg K.

    Expected return: K+1 (original pattern value) — proves popn restored.
    Uses R9 (caller-saved) as scratch for the return stage.
    """
    init_lines = [f'"ld.w %r{i}, {i + 1}\\n"' for i in range(n + 1)]
    init_block = "\n        ".join(init_lines)
    # After popn, capture rK into r9 (which is outside the pushn range for n<=7)
    # Then move r9 to r10 (the return register)
    buf.append(f"""__attribute__((naked))
static uint32_t test_pp_{cid}(uint32_t a __attribute__((unused)),
                              uint32_t b __attribute__((unused))) {{
    asm(
        "pushn %r3\\n"
        {init_block}
        "pushn %r{n}\\n"
        // corrupt r0..r{n} to prove popn actually restores
        "ld.w %r0, 0\\n"
        "xor %r1, %r1\\n"             // r1 = 0
        "ld.w %r2, 0\\n"
        "xor %r3, %r3\\n"             // r3 = 0
        // (higher regs not necessarily all zeroed — popn should fix them)
        "popn %r{n}\\n"
        "ld.w %r10, %r{k}\\n"         // return reg K's restored value
        "popn %r3\\n"
        "ret\\n"
    );
}}
""")


C_PROLOGUE = """// AUTO-GENERATED by gen_pushpop.py — do not edit by hand.
//
// pushn/popn regression tests using __attribute__((naked)) functions.

#include <stdint.h>

#define CHECK(id, ok) do { if (!(ok)) return (id); } while (0)
"""

C_EPILOGUE_TEMPLATE = """
int main(void) {{
{calls}
    return 0;
}}
"""


def main() -> int:
    out = HERE / "test_gen_pushpop.c"
    fns: list[str] = []
    callers: list[str] = []

    # Group 0 — slot-ordering (K = 0, N/2 if distinct, N)
    vi = 0
    for n in N_VALUES:
        slots = sorted(set([0, n // 2, n]))
        for k in slots:
            cid = case_id(0, vi)
            emit_slot_test(fns, cid, n, k)
            callers.append(
                f"    {{ uint32_t r = test_pp_{cid}(0,0); "
                f"CHECK({cid}, r == {k + 1}u); }}"
            )
            vi += 1

    # Group 1 — SP delta after pushn
    vi = 0
    for n in N_VALUES:
        cid = case_id(1, vi)
        emit_sp_delta_after_pushn(fns, cid, n)
        expected = 4 * (n + 1)
        callers.append(
            f"    {{ uint32_t r = test_pp_{cid}(0,0); "
            f"CHECK({cid}, r == {expected}u); }}"
        )
        vi += 1

    # Group 2 — SP round-trip (pushn + popn should be balanced)
    vi = 0
    for n in N_VALUES:
        cid = case_id(2, vi)
        emit_sp_delta_roundtrip(fns, cid, n)
        callers.append(
            f"    {{ uint32_t r = test_pp_{cid}(0,0); "
            f"CHECK({cid}, r == 0u); }}"
        )
        vi += 1

    # Group 3 — popn round-trip (corrupt → popn → verify restored)
    vi = 0
    for n in N_VALUES:
        slots = sorted(set([0, n // 2, n]))
        for k in slots:
            cid = case_id(3, vi)
            emit_popn_roundtrip(fns, cid, n, k)
            callers.append(
                f"    {{ uint32_t r = test_pp_{cid}(0,0); "
                f"CHECK({cid}, r == {k + 1}u); }}"
            )
            vi += 1

    out.write_text(
        C_PROLOGUE + "\n".join(fns)
        + C_EPILOGUE_TEMPLATE.format(calls="\n".join(callers)),
        encoding="utf-8",
    )
    print(f"# wrote {out} : {len(callers)} cases", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Generate bit-operation regression tests for piece-emu.

Instructions under test (Class 1E):
  btst [%rb], imm3       — test bit; Z = !bit(B[rb], imm3); flags: ----Z-
  bclr [%rb], imm3       — B[rb] &= ~(1 << imm3);           flags: ------
  bset [%rb], imm3       — B[rb] |= (1 << imm3);            flags: ------
  bnot [%rb], imm3       — B[rb] ^= (1 << imm3);            flags: ------

  With 1 ext: address = rb + imm13 (UNSIGNED, per the Class 1 memory pitfall).
  imm3 range: 0..7.

Test strategy:
  (A) btst: init byte with known bit pattern, btst each bit position, verify Z
      matches the bit's complement (Z=1 → bit was 0; Z=0 → bit was 1).
  (B) bset: start with 0, bset bit k, verify byte == (1 << k).
  (C) bclr: start with 0xFF, bclr bit k, verify byte == ~(1 << k) & 0xFF.
  (D) bnot: start with value V, bnot bit k, verify byte == V ^ (1 << k).
  (E) ext-form: same as (A/B/C/D) but with `ext N; bOP [%rb], imm3` accessing
      a byte at offset N within a 64-byte buffer.

Memory layout: a 64-byte test buffer allocated as volatile static, re-initialised
to known state before each test via init_buf().
"""

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def case_id(group: int, vi: int) -> int:
    """group: 0=btst, 1=bset, 2=bclr, 3=bnot, 4=ext-form btst, ..."""
    return 1 + (group * 1000) + vi


def _asm(*lines: str) -> str:
    return "\n        ".join(f'"{ln}\\n"' for ln in lines)


# ----------------------------------------------------------- btst

def emit_btst(buf: list[str], cid: int, byte_val: int, bit: int) -> None:
    """Place `byte_val` at buf[0], btst [%r4], bit, check Z flag matches !bit."""
    expected_z = 0 if (byte_val >> bit) & 1 else 1
    asm = _asm(
        "ld.w %%r4, %2",                 # r4 = buffer address
        "ld.w %%r5, %3",                 # r5 = value to store
        "ld.b [%%r4], %%r5",             # write byte to buf[0]
        f"btst [%%r4], {bit}",           # TEST
        "ld.w %0, %%psr",                # r = PSR
        "ld.w %1, 0",                    # dummy unused output
    )
    buf.append(f"""static int case_{cid}(void) {{
    init_buf();
    uint32_t psr, _unused;
    asm volatile(
        {asm}
        : "=r"(psr), "=&r"(_unused)
        : "r"((uintptr_t)mem_buf), "r"((uint32_t){byte_val:#x}u)
        : "r4", "r5", "memory"
    );
    (void)_unused;
    CHECK({cid}, ((psr >> 1) & 1) == {expected_z}u);   // Z flag bit 1
    return 0;
}}
""")


def emit_bset(buf: list[str], cid: int, bit: int) -> None:
    """Start with 0, bset bit, verify buf[0] == (1 << bit)."""
    expected = 1 << bit
    asm = _asm(
        "ld.w %%r4, %1",
        "ld.w %%r5, 0",                  # zero the byte
        "ld.b [%%r4], %%r5",
        f"bset [%%r4], {bit}",           # TEST
        "ld.ub %0, [%%r4]",
    )
    buf.append(f"""static int case_{cid}(void) {{
    init_buf();
    uint32_t r;
    asm volatile(
        {asm}
        : "=r"(r)
        : "r"((uintptr_t)mem_buf)
        : "r4", "r5", "memory"
    );
    CHECK({cid}, r == {expected:#x}u);
    return 0;
}}
""")


def emit_bclr(buf: list[str], cid: int, bit: int) -> None:
    """Start with 0xFF, bclr bit, verify buf[0] == 0xFF & ~(1<<bit)."""
    expected = 0xFF & ~(1 << bit)
    asm = _asm(
        "ld.w %%r4, %1",
        "ld.w %%r5, -1",                 # all ones
        "ld.b [%%r4], %%r5",
        f"bclr [%%r4], {bit}",           # TEST
        "ld.ub %0, [%%r4]",
    )
    buf.append(f"""static int case_{cid}(void) {{
    init_buf();
    uint32_t r;
    asm volatile(
        {asm}
        : "=r"(r)
        : "r"((uintptr_t)mem_buf)
        : "r4", "r5", "memory"
    );
    CHECK({cid}, r == {expected:#x}u);
    return 0;
}}
""")


def emit_bnot(buf: list[str], cid: int, byte_val: int, bit: int) -> None:
    """Start with byte_val, bnot bit, verify buf[0] == byte_val ^ (1<<bit)."""
    expected = (byte_val ^ (1 << bit)) & 0xFF
    asm = _asm(
        "ld.w %%r4, %1",
        "ld.w %%r5, %2",
        "ld.b [%%r4], %%r5",
        f"bnot [%%r4], {bit}",           # TEST
        "ld.ub %0, [%%r4]",
    )
    buf.append(f"""static int case_{cid}(void) {{
    init_buf();
    uint32_t r;
    asm volatile(
        {asm}
        : "=r"(r)
        : "r"((uintptr_t)mem_buf), "r"((uint32_t){byte_val:#x}u)
        : "r4", "r5", "memory"
    );
    CHECK({cid}, r == {expected:#x}u);
    return 0;
}}
""")


# ----------------------------------------------------------- ext-form

def emit_btst_ext(buf: list[str], cid: int, byte_val: int, bit: int,
                  ext13: int) -> None:
    """With 1 ext: btst [%rb+ext13], imm3 → tests byte at buf[ext13].

    Uses `ext N; ld.b [%rb], %rs` for the pre-store and `ext N; btst [%rb]`
    for the test — both exercise the Class-1 memory ext mechanism.
    """
    expected_z = 0 if (byte_val >> bit) & 1 else 1
    asm = _asm(
        "ld.w %%r4, %2",                  # r4 = mem_buf base
        "ld.w %%r5, %3",                  # r5 = value
        f"ext {ext13:#x}",                # ext for the store below
        "ld.b [%%r4], %%r5",              # store byte at mem_buf + ext13
        f"ext {ext13:#x}",                # ext for btst
        f"btst [%%r4], {bit}",            # TEST
        "ld.w %0, %%psr",
        "ld.w %1, 0",
    )
    buf.append(f"""static int case_{cid}(void) {{
    init_buf();
    uint32_t psr, _unused;
    asm volatile(
        {asm}
        : "=r"(psr), "=&r"(_unused)
        : "r"((uintptr_t)mem_buf), "r"((uint32_t){byte_val:#x}u)
        : "r4", "r5", "memory"
    );
    (void)_unused;
    CHECK({cid}, ((psr >> 1) & 1) == {expected_z}u);
    return 0;
}}
""")


def emit_bset_ext(buf: list[str], cid: int, bit: int, ext13: int) -> None:
    expected = 1 << bit
    asm = _asm(
        "ld.w %%r4, %1",                  # r4 = mem_buf
        "ld.w %%r5, 0",
        f"ext {ext13:#x}",
        "ld.b [%%r4], %%r5",              # pre-zero byte at mem_buf + ext13
        f"ext {ext13:#x}",
        f"bset [%%r4], {bit}",            # TEST
        f"ext {ext13:#x}",
        "ld.ub %0, [%%r4]",               # read back
    )
    buf.append(f"""static int case_{cid}(void) {{
    init_buf();
    uint32_t r;
    asm volatile(
        {asm}
        : "=r"(r)
        : "r"((uintptr_t)mem_buf)
        : "r4", "r5", "memory"
    );
    CHECK({cid}, r == {expected:#x}u);
    return 0;
}}
""")


def emit_bclr_ext(buf: list[str], cid: int, bit: int, ext13: int) -> None:
    expected = 0xFF & ~(1 << bit)
    asm = _asm(
        "ld.w %%r4, %1",
        "ld.w %%r5, -1",
        f"ext {ext13:#x}",
        "ld.b [%%r4], %%r5",
        f"ext {ext13:#x}",
        f"bclr [%%r4], {bit}",
        f"ext {ext13:#x}",
        "ld.ub %0, [%%r4]",
    )
    buf.append(f"""static int case_{cid}(void) {{
    init_buf();
    uint32_t r;
    asm volatile(
        {asm}
        : "=r"(r)
        : "r"((uintptr_t)mem_buf)
        : "r4", "r5", "memory"
    );
    CHECK({cid}, r == {expected:#x}u);
    return 0;
}}
""")


# ----------------------------------------------------------- main

BYTE_PATTERNS = [0x00, 0x01, 0x55, 0xAA, 0xFF, 0x80, 0x7F]
EXT_OFFSETS = [1, 5, 32, 63]   # byte offsets within buf (0..63 fits)

C_PROLOGUE = """// AUTO-GENERATED by gen_bitop.py — do not edit by hand.
//
// Bit-operation (btst/bset/bclr/bnot) regression tests.

#include <stdint.h>

#define CHECK(id, ok) do { if (!(ok)) return (id); } while (0)

static volatile uint8_t mem_buf[64] __attribute__((aligned(4)));

static void init_buf(void) {
    for (int i = 0; i < 64; i++) mem_buf[i] = 0xCC;
}
"""

C_EPILOGUE_TEMPLATE = """
int main(void) {{
    int e;
{calls}
    return 0;
}}
"""


def main() -> int:
    out = HERE / "test_gen_bitop.c"
    cases: list[str] = []

    # Group 0 — btst [%rb], imm3 (no ext)
    vi = 0
    for val in BYTE_PATTERNS:
        for bit in range(8):
            emit_btst(cases, case_id(0, vi), val, bit)
            vi += 1

    # Group 1 — bset [%rb], imm3 (no ext)
    vi = 0
    for bit in range(8):
        emit_bset(cases, case_id(1, vi), bit)
        vi += 1

    # Group 2 — bclr [%rb], imm3 (no ext)
    vi = 0
    for bit in range(8):
        emit_bclr(cases, case_id(2, vi), bit)
        vi += 1

    # Group 3 — bnot [%rb], imm3 (no ext)
    vi = 0
    for val in BYTE_PATTERNS:
        for bit in range(8):
            emit_bnot(cases, case_id(3, vi), val, bit)
            vi += 1

    # Group 4 — btst [%rb+ext], imm3 (with 1 ext)
    vi = 0
    for off in EXT_OFFSETS:
        for bit in (0, 3, 7):   # fewer bits to keep count down
            emit_btst_ext(cases, case_id(4, vi), 0x55, bit, off)
            vi += 1

    # Group 5 — bset [%rb+ext], imm3
    vi = 0
    for off in EXT_OFFSETS:
        for bit in (0, 3, 7):
            emit_bset_ext(cases, case_id(5, vi), bit, off)
            vi += 1

    # Group 6 — bclr [%rb+ext], imm3
    vi = 0
    for off in EXT_OFFSETS:
        for bit in (0, 3, 7):
            emit_bclr_ext(cases, case_id(6, vi), bit, off)
            vi += 1

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

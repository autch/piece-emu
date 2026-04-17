#!/usr/bin/env python3
"""Generate special-register (PSR/SP/ALR/AHR) access tests.

Instructions under test (Class 5C/5D):
  ld.w %sd, %rs       — gen → special  (sd ∈ {PSR, SP, ALR, AHR})
  ld.w %rd, %ss       — special → gen

Special register encoding: 0=PSR, 1=SP, 2=ALR, 3=AHR

Coverage:
  - ALR roundtrip: various 32-bit values (sign boundaries, patterns)
  - AHR roundtrip: same
  - PSR: write flag bits (N/Z/V/C only), read back, verify readable bits match
  - SP: verify `ld.w %rd, %sp` returns the current SP; use sub/add sp deltas
    to confirm the read reflects the updated SP value.

PSR caveat: bits 31-12 are reserved.  Writing arbitrary values may be silently
masked.  We only test low bits (N/Z/V/C + IE) for writability.

SP test pattern: save SP in r9 before any modifications, modify via sub/add sp,
read via `ld.w %r4, %sp`, verify delta, restore.

ALR/AHR caveat: on S1C33209 (with multiplier option) these are plain 32-bit
registers accessible via ld.w only.  Arithmetic that updates them (mlt.*, div*)
is tested separately in gen_muldiv.
"""

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

PSR_N = 1 << 0
PSR_Z = 1 << 1
PSR_V = 1 << 2
PSR_C = 1 << 3
PSR_IE = 1 << 4


def case_id(group: int, vi: int) -> int:
    return 1 + (group * 100) + vi


def _asm(*lines: str) -> str:
    return "\n        ".join(f'"{ln}\\n"' for ln in lines)


# ----------------------------------------------------------- ALR

def emit_alr_roundtrip(buf: list[str], cid: int, value: int) -> None:
    """Write value into ALR via gen→special, read back via special→gen."""
    asm = _asm(
        "ld.w %%alr, %1",
        "ld.w %0, %%alr",
    )
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t r;
    asm volatile(
        {asm}
        : "=r"(r)
        : "r"((uint32_t){value:#010x}u)
    );
    CHECK({cid}, r == {value & 0xFFFFFFFF:#010x}u);
    return 0;
}}
""")


def emit_ahr_roundtrip(buf: list[str], cid: int, value: int) -> None:
    asm = _asm(
        "ld.w %%ahr, %1",
        "ld.w %0, %%ahr",
    )
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t r;
    asm volatile(
        {asm}
        : "=r"(r)
        : "r"((uint32_t){value:#010x}u)
    );
    CHECK({cid}, r == {value & 0xFFFFFFFF:#010x}u);
    return 0;
}}
""")


def emit_alr_ahr_independent(buf: list[str], cid: int,
                             alr_val: int, ahr_val: int) -> None:
    """Verify writing ALR doesn't disturb AHR and vice versa."""
    asm = _asm(
        "ld.w %%alr, %2",
        "ld.w %%ahr, %3",
        "ld.w %%alr, %2",      # write ALR again
        "ld.w %0, %%alr",
        "ld.w %1, %%ahr",
    )
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t alr, ahr;
    asm volatile(
        {asm}
        : "=r"(alr), "=r"(ahr)
        : "r"((uint32_t){alr_val:#010x}u), "r"((uint32_t){ahr_val:#010x}u)
    );
    CHECK({cid}, alr == {alr_val & 0xFFFFFFFF:#010x}u);
    CHECK({cid}, ahr == {ahr_val & 0xFFFFFFFF:#010x}u);
    return 0;
}}
""")


# ----------------------------------------------------------- PSR

def emit_psr_flag_write(buf: list[str], cid: int, flag_bits: int) -> None:
    """Write PSR with only flag bits set, read back, mask low 5 bits, verify."""
    # The low 5 bits are N/Z/V/C/IE.  We write flag_bits, assume those bits
    # are writable.  IE (bit 4) is writable too.  Other bits may be reserved.
    asm = _asm(
        "ld.w %%psr, %1",
        "ld.w %0, %%psr",
    )
    mask = 0x1F
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t r;
    asm volatile(
        {asm}
        : "=r"(r)
        : "r"((uint32_t){flag_bits:#x}u)
    );
    CHECK({cid}, (r & {mask:#x}u) == ({flag_bits & mask:#x}u));
    return 0;
}}
""")


# ----------------------------------------------------------- SP

def emit_sp_read(buf: list[str], cid: int) -> None:
    """Verify `ld.w %rd, %sp` reads current SP. No modifications."""
    # Save SP twice, verify they match
    asm = _asm(
        "ld.w %0, %%sp",
        "ld.w %1, %%sp",
    )
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t sp1, sp2;
    asm volatile(
        {asm}
        : "=r"(sp1), "=r"(sp2)
    );
    CHECK({cid}, sp1 == sp2);
    return 0;
}}
""")


def emit_sp_delta(buf: list[str], cid: int, words: int) -> None:
    """sub %sp, words; read SP; add %sp, words; verify delta = -4*words.

    Uses `sub %sp, imm10` / `add %sp, imm10` with the requested word count
    (must fit in imm10 = 0..1023).
    """
    assert 1 <= words <= 1023
    delta_bytes = words * 4
    asm = _asm(
        "ld.w %0, %%sp",           # sp_before
        f"sub %%sp, {words}",      # SP -= words*4
        "ld.w %1, %%sp",           # sp_mid
        f"add %%sp, {words}",      # restore
        "ld.w %2, %%sp",           # sp_after
    )
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t sp_before, sp_mid, sp_after;
    asm volatile(
        {asm}
        : "=r"(sp_before), "=r"(sp_mid), "=r"(sp_after)
    );
    CHECK({cid}, sp_before - sp_mid == {delta_bytes}u);
    CHECK({cid}, sp_after == sp_before);
    return 0;
}}
""")


# ----------------------------------------------------------- main

ALR_VALUES = [
    0, 1, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF,
    0x12345678, 0xDEADBEEF, 0x55555555, 0xAAAAAAAA,
]
AHR_VALUES = ALR_VALUES
ALR_AHR_PAIRS = [
    (0x12345678, 0x87654321),
    (0xFFFFFFFF, 0),
    (0, 0xFFFFFFFF),
    (0x80000000, 0x7FFFFFFF),
]
PSR_FLAG_SETS = [
    0,
    PSR_N,
    PSR_Z,
    PSR_V,
    PSR_C,
    PSR_N | PSR_Z,
    PSR_V | PSR_C,
    PSR_N | PSR_V | PSR_Z | PSR_C,
    PSR_N | PSR_Z | PSR_V | PSR_C | PSR_IE,
]
SP_WORD_DELTAS = [1, 2, 8, 64, 256]

C_PROLOGUE = """// AUTO-GENERATED by gen_special.py — do not edit by hand.
//
// Special-register (PSR/SP/ALR/AHR) access tests.

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
    out = HERE / "test_gen_special.c"
    cases: list[str] = []

    # Group 0 — ALR roundtrip
    vi = 0
    for v in ALR_VALUES:
        emit_alr_roundtrip(cases, case_id(0, vi), v)
        vi += 1

    # Group 1 — AHR roundtrip
    vi = 0
    for v in AHR_VALUES:
        emit_ahr_roundtrip(cases, case_id(1, vi), v)
        vi += 1

    # Group 2 — ALR/AHR independence
    vi = 0
    for alr, ahr in ALR_AHR_PAIRS:
        emit_alr_ahr_independent(cases, case_id(2, vi), alr, ahr)
        vi += 1

    # Group 3 — PSR flag write/read
    vi = 0
    for flags in PSR_FLAG_SETS:
        emit_psr_flag_write(cases, case_id(3, vi), flags)
        vi += 1

    # Group 4 — SP read-only
    emit_sp_read(cases, case_id(4, 0))

    # Group 5 — SP delta (sub/add sp roundtrip)
    vi = 0
    for words in SP_WORD_DELTAS:
        emit_sp_delta(cases, case_id(5, vi), words)
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

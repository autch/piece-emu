#!/usr/bin/env python3
"""Generate S1C33 memory-instruction tests.

Scope (PoC):
  Loads:  ld.b / ld.ub / ld.h / ld.uh / ld.w
          - [%rb]                         (no ext)
          - [%rb] with 1 ext displacement (0..8191, UNSIGNED — Class 1 pitfall)
          - [%rb]+                        (post-increment, no ext)
  Stores: ld.b / ld.h / ld.w
          - [%rb]                         (write, then read-back via matching load)
          - [%rb]+                        (write, verify post-increment AND value)

  Skipped this round:
    - [%sp+imm6]          (needs asm-only helper for explicit SP control)
    - [%rb] with 2 exts   (would need >8 KB test buffer)

Test buffer layout (64 bytes, 4-aligned, BSS-allocated and runtime-initialised
to a fixed pattern so storesdo not corrupt subsequent loads):

  offset 00..0F : sign-extension boundary bytes / halfwords
  offset 10..1F : word-level boundary patterns
  offset 20..3F : scratch area for store tests
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

HERE = Path(__file__).resolve().parent

MASK = 0xFFFFFFFF

# ------------------------------------------------------------------ pattern
# Each byte chosen so that the sign-extension boundary is exercised at every
# load width.  Halfword and word values are little-endian compositions of the
# bytes; the comments record the natural load width interpretation.
PATTERN: list[int] = [
    # ld.b boundary bytes
    0x80, 0x7F, 0xFF, 0x00,    # [00..03]
    # ld.h boundary halfwords (LE):
    #   [04..05] → 0x8000 (negative when sign-extended)
    #   [06..07] → 0x7FFF (max positive)
    0x00, 0x80, 0xFF, 0x7F,    # [04..07]
    # general words for ld.w
    0x12, 0x34, 0x56, 0x78,    # [08..0B] → word 0x78563412
    0xAB, 0xCD, 0xEF, 0x89,    # [0C..0F] → word 0x89EFCDAB
    # word boundary patterns
    0x00, 0x00, 0x00, 0x80,    # [10..13] → 0x80000000
    0xFF, 0xFF, 0xFF, 0x7F,    # [14..17] → 0x7FFFFFFF
    0x00, 0x00, 0x00, 0x00,    # [18..1B] → 0x00000000
    0xFF, 0xFF, 0xFF, 0xFF,    # [1C..1F] → 0xFFFFFFFF
    # scratch (store target)
    *([0xCC] * 32),            # [20..3F]
]
assert len(PATTERN) == 64


def byte(off: int) -> int:
    return PATTERN[off] & 0xFF


def halfword(off: int) -> int:
    """Little-endian halfword starting at byte offset off."""
    assert off % 2 == 0
    return PATTERN[off] | (PATTERN[off + 1] << 8)


def word(off: int) -> int:
    """Little-endian word starting at byte offset off."""
    assert off % 4 == 0
    return (PATTERN[off]
            | (PATTERN[off + 1] << 8)
            | (PATTERN[off + 2] << 16)
            | (PATTERN[off + 3] << 24))


def sign_ext(value: int, bits: int) -> int:
    sign_bit = 1 << (bits - 1)
    if value & sign_bit:
        return (value - (1 << bits)) & MASK
    return value & MASK


# ------------------------------------------------------------------ ops

@dataclass
class LoadOp:
    name: str          # 'ld.b', 'ld.ub', 'ld.h', 'ld.uh', 'ld.w'
    width: int         # bytes: 1, 2, or 4
    signed: bool       # True = sign-extend, False = zero-extend
    op_idx: int        # for case_id encoding


@dataclass
class StoreOp:
    name: str          # 'ld.b', 'ld.h', 'ld.w' (same mnemonic, different form)
    width: int
    op_idx: int


LOAD_OPS = [
    LoadOp("ld.b",  1, True,  0),
    LoadOp("ld.ub", 1, False, 1),
    LoadOp("ld.h",  2, True,  2),
    LoadOp("ld.uh", 2, False, 3),
    LoadOp("ld.w",  4, False, 4),  # ld.w is intrinsically 32-bit; signed/zero N/A
]

STORE_OPS = [
    StoreOp("ld.b", 1, 5),
    StoreOp("ld.h", 2, 6),
    StoreOp("ld.w", 4, 7),
]


def expected_load(op: LoadOp, off: int) -> int:
    if op.width == 1:
        raw = byte(off)
        return sign_ext(raw, 8) if op.signed else raw
    if op.width == 2:
        raw = halfword(off)
        return sign_ext(raw, 16) if op.signed else raw
    return word(off)


# ------------------------------------------------------------------ value sets

# Offsets to test for each width.  Constrained so that off + width <= 32 (read area).
LOAD_OFFSETS_B  = [0, 1, 2, 3, 4, 5, 6, 7, 0x10, 0x13, 0x14, 0x17]
LOAD_OFFSETS_H  = [0, 2, 4, 6, 8, 10, 12, 14, 0x10, 0x12, 0x14, 0x16]
LOAD_OFFSETS_W  = [0, 4, 8, 12, 16, 20, 24, 28]

# For [%rb]+ext1 form: base register points to start of buffer; ext provides
# the byte displacement.  Range 0..63 covers buffer; we pick a spread.
EXT1_OFFSETS_B  = [0, 1, 5, 7, 0x10, 0x13, 0x17, 0x1F]
EXT1_OFFSETS_H  = [0, 2, 6, 8, 0x10, 0x12, 0x16, 0x1E]
EXT1_OFFSETS_W  = [0, 4, 8, 12, 16, 20, 24, 28]

# For [%rb]+ tests: number of consecutive accesses; base advances by width each time.
POSTINC_RUNS_B = [(0, 4), (8, 4), (0x10, 4)]   # (start, count)
POSTINC_RUNS_H = [(0, 4), (8, 4)]
POSTINC_RUNS_W = [(0, 4), (16, 4)]

# Store test values per width.
STORE_VALUES_B = [0x00, 0x55, 0x80, 0xAA, 0xFF]
STORE_VALUES_H = [0x0000, 0x55AA, 0x8000, 0x7FFF, 0xFFFF]
STORE_VALUES_W = [0x00000000, 0x55AA55AA, 0x80000000, 0x7FFFFFFF, 0xDEADBEEF]

# Store offsets in scratch area [0x20..0x3F]
STORE_OFFSETS_B = [0x20, 0x21, 0x22, 0x23, 0x2F]
STORE_OFFSETS_H = [0x20, 0x22, 0x24, 0x26, 0x30]
STORE_OFFSETS_W = [0x20, 0x24, 0x28, 0x2C, 0x30]


def case_id(op_idx: int, form_idx: int, val_idx: int) -> int:
    return 1 + (op_idx * 1000) + (form_idx * 100) + val_idx


# ------------------------------------------------------------------ emit

def _asm_block(*lines: str) -> str:
    return "\n        ".join(f'"{ln}\\n"' for ln in lines)


def emit_load_basic(buf: list[str], op: LoadOp, form_idx: int) -> int:
    n = 0
    offsets = {1: LOAD_OFFSETS_B, 2: LOAD_OFFSETS_H, 4: LOAD_OFFSETS_W}[op.width]
    for vi, off in enumerate(offsets):
        cid = case_id(op.op_idx, form_idx, vi)
        exp = expected_load(op, off)
        asm = _asm_block(f"{op.name} %0, [%1]")
        buf.append(f"""static int case_{cid}(void) {{
    init_buf();
    uint32_t r;
    asm volatile(
        {asm}
        : "=r"(r)
        : "r"((const void*)(mem_buf + {off}))
        : "memory"
    );
    CHECK({cid}, r == {exp:#010x}u);
    return 0;
}}
""")
        n += 1
    return n


def emit_load_ext1(buf: list[str], op: LoadOp, form_idx: int) -> int:
    n = 0
    offsets = {1: EXT1_OFFSETS_B, 2: EXT1_OFFSETS_H, 4: EXT1_OFFSETS_W}[op.width]
    for vi, off in enumerate(offsets):
        cid = case_id(op.op_idx, form_idx, vi)
        exp = expected_load(op, off)
        # base register points to mem_buf + 0; ext provides byte displacement
        asm = _asm_block(f"ext {off:#x}", f"{op.name} %0, [%1]")
        buf.append(f"""static int case_{cid}(void) {{
    init_buf();
    uint32_t r;
    asm volatile(
        {asm}
        : "=r"(r)
        : "r"((const void*)mem_buf)
        : "memory"
    );
    CHECK({cid}, r == {exp:#010x}u);
    return 0;
}}
""")
        n += 1
    return n


def emit_load_postinc(buf: list[str], op: LoadOp, form_idx: int) -> int:
    n = 0
    runs = {1: POSTINC_RUNS_B, 2: POSTINC_RUNS_H, 4: POSTINC_RUNS_W}[op.width]
    for vi, (start, count) in enumerate(runs):
        cid = case_id(op.op_idx, form_idx, vi)
        # Build N consecutive loads; check loaded values AND that base advanced.
        loads = []
        results = []
        for k in range(count):
            results.append(expected_load(op, start + k * op.width))
        # Generate one asm block per load (so we can use distinct outputs).
        # Use a single asm block with multiple outputs r0..rN-1, base in/out.
        asm_lines = []
        for k in range(count):
            asm_lines.append(f"{op.name} %{k}, [%{count}]+")
        asm = _asm_block(*asm_lines)
        outs = ", ".join(f'"=&r"(v{k})' for k in range(count))
        decls = "\n    ".join(f"uint32_t v{k};" for k in range(count))
        checks = "\n    ".join(
            f"CHECK({cid}, v{k} == {results[k]:#010x}u);" for k in range(count)
        )
        buf.append(f"""static int case_{cid}(void) {{
    init_buf();
    {decls}
    const volatile uint8_t* base = mem_buf + {start};
    asm volatile(
        {asm}
        : {outs}, "+r"(base)
        :
        : "memory"
    );
    {checks}
    CHECK({cid}, base == mem_buf + {start + count * op.width});
    return 0;
}}
""")
        n += 1
    return n


def emit_store_basic(buf: list[str], op: StoreOp, form_idx: int) -> int:
    n = 0
    offsets = {1: STORE_OFFSETS_B, 2: STORE_OFFSETS_H, 4: STORE_OFFSETS_W}[op.width]
    values  = {1: STORE_VALUES_B,  2: STORE_VALUES_H,  4: STORE_VALUES_W}[op.width]
    # Pair each offset with each value, but cap at len(values) to keep counts down.
    pairs = list(zip(offsets, values))
    # Verifying load: ld.ub for byte, ld.uh for halfword, ld.w for word
    verify = {1: "ld.ub", 2: "ld.uh", 4: "ld.w"}[op.width]
    width_mask = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}[op.width]
    for vi, (off, val) in enumerate(pairs):
        cid = case_id(op.op_idx, form_idx, vi)
        # Store form: ld.b [%rb], %rs
        store_asm = _asm_block(f"{op.name} [%0], %1")
        load_asm  = _asm_block(f"{verify} %0, [%1]")
        expected = val & width_mask
        buf.append(f"""static int case_{cid}(void) {{
    init_buf();
    asm volatile(
        {store_asm}
        :
        : "r"((void*)(mem_buf + {off})), "r"((uint32_t){val:#010x}u)
        : "memory"
    );
    uint32_t r;
    asm volatile(
        {load_asm}
        : "=r"(r)
        : "r"((const void*)(mem_buf + {off}))
        : "memory"
    );
    CHECK({cid}, r == {expected:#010x}u);
    return 0;
}}
""")
        n += 1
    return n


def emit_store_postinc(buf: list[str], op: StoreOp, form_idx: int) -> int:
    """Store with post-increment: write a small run of values then verify both
    the stored bytes AND that the base register advanced by N*width."""
    n = 0
    offsets = {1: STORE_OFFSETS_B, 2: STORE_OFFSETS_H, 4: STORE_OFFSETS_W}[op.width]
    values  = {1: STORE_VALUES_B,  2: STORE_VALUES_H,  4: STORE_VALUES_W}[op.width]
    verify = {1: "ld.ub", 2: "ld.uh", 4: "ld.w"}[op.width]
    width_mask = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}[op.width]
    # One case per starting offset: write 4 consecutive values
    runs = [(offsets[0], values[:4]), (offsets[2], values[1:5])]
    for vi, (start, vals) in enumerate(runs):
        cid = case_id(op.op_idx, form_idx, vi)
        # operand numbering: %0=base ("+r"), %1..%N=values ("r" inputs)
        store_lines = [f"{op.name} [%0]+, %{k + 1}" for k in range(len(vals))]
        store_asm = _asm_block(*store_lines)
        ins = ", ".join(f'"r"((uint32_t){v:#010x}u)' for v in vals)
        # After: base should equal start + len*width
        expected_base = start + len(vals) * op.width
        # Verify each stored value
        verify_lines = []
        for k, v in enumerate(vals):
            v_masked = v & width_mask
            verify_lines.append(
                f"    asm volatile(\n        \"{verify} %0, [%1]\\n\"\n"
                f"        : \"=r\"(r)\n"
                f"        : \"r\"((const void*)(mem_buf + {start + k * op.width}))\n"
                f"        : \"memory\"\n    );\n"
                f"    CHECK({cid}, r == {v_masked:#010x}u);"
            )
        verify_block = "\n".join(verify_lines)
        buf.append(f"""static int case_{cid}(void) {{
    init_buf();
    volatile uint8_t* base = mem_buf + {start};
    asm volatile(
        {store_asm}
        : "+r"(base)
        : {ins}
        : "memory"
    );
    CHECK({cid}, base == mem_buf + {expected_base});
    uint32_t r;
{verify_block}
    return 0;
}}
""")
        n += 1
    return n


# ------------------------------------------------------------------ main

C_PROLOGUE_TEMPLATE = """// AUTO-GENERATED by gen_mem.py — do not edit by hand.
//
// Memory-instruction regression tests for piece-emu.
// Each case re-initialises mem_buf to PATTERN before exercising one
// load/store form, so cases are independent.

#include <stdint.h>

#define CHECK(id, ok) do {{ if (!(ok)) return (id); }} while (0)

static const uint8_t PATTERN[64] = {{
{pattern_init}
}};

static volatile uint8_t mem_buf[64] __attribute__((aligned(4)));

static void init_buf(void) {{
    for (int i = 0; i < 64; i++) mem_buf[i] = PATTERN[i];
}}
"""

C_EPILOGUE_TEMPLATE = """
int main(void) {{
    int e;
{calls}
    return 0;
}}
"""


def main() -> int:
    out_path = HERE / "test_gen_mem.c"
    cases: list[str] = []

    for op in LOAD_OPS:
        emit_load_basic(cases, op, 0)
        emit_load_ext1(cases, op, 1)
        emit_load_postinc(cases, op, 3)   # form 2 reserved for ext2 (future)

    for op in STORE_OPS:
        emit_store_basic(cases, op, 0)
        emit_store_postinc(cases, op, 3)

    import re
    case_re = re.compile(r"static int case_(\d+)\(void\)")
    cids = [int(m.group(1)) for c in cases for m in [case_re.search(c)] if m]
    calls = [f"    if ((e = case_{cid}())) return e;" for cid in cids]

    pattern_init = ", ".join(f"0x{b:02X}" for b in PATTERN)
    # Wrap to ~8 per line for readability
    chunks = []
    for i in range(0, len(PATTERN), 8):
        chunks.append("    " + ", ".join(f"0x{b:02X}" for b in PATTERN[i:i + 8]))
    pattern_init = ",\n".join(chunks)

    out = (C_PROLOGUE_TEMPLATE.format(pattern_init=pattern_init)
           + "\n"
           + "\n".join(cases)
           + C_EPILOGUE_TEMPLATE.format(calls="\n".join(calls)))
    out_path.write_text(out, encoding="utf-8")
    print(f"# wrote {out_path} : {len(cids)} cases", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

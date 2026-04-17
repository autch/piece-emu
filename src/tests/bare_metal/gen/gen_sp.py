#!/usr/bin/env python3
"""Generate SP-relative memory tests for piece-emu.

Tests `ld.* %rd, [%sp+imm6]` and `ld.* [%sp+imm6], %rs` for ld.b/ld.ub/ld.h/
ld.uh/ld.w.  This is the addressing mode the compiler emits for local
variables, and a scaling bug here would corrupt every stack-allocated value.

Critical fact tested:
  - ld.b [%sp+imm6]:   imm6 in BYTE units      (effective offset = imm6 * 1)
  - ld.h [%sp+imm6]:   imm6 in HALFWORD units  (effective offset = imm6 * 2)
  - ld.w [%sp+imm6]:   imm6 in WORD units      (effective offset = imm6 * 4)

Each test runs in its own __attribute__((naked)) function so we control SP
explicitly:
  1. Reserve a 64-byte stack buffer
  2. Fill the buffer with a poison word (passed in R12)
  3. For LOAD: pre-store the expected byte/halfword/word at the byte offset
              corresponding to `imm6 * width` using [%rb] form, then perform
              the load under test via [%sp+imm6].  Return loaded value in R10.
     For STORE: store the input value (R13) via [%sp+imm6], then read back
              the same location via [%rb] form.  Return read-back value in R10.
  4. Restore SP and return.

If the scaling is wrong the load reads/writes the wrong byte and we either
see poison (read) or fail to corrupt the expected location (write+verify).
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

HERE = Path(__file__).resolve().parent

MASK = 0xFFFFFFFF


def sign_ext(value: int, bits: int) -> int:
    sb = 1 << (bits - 1)
    return (value - (1 << bits)) & MASK if value & sb else value & MASK


@dataclass
class Op:
    name: str            # asm mnemonic ('ld.b' etc.)
    width: int           # 1, 2, 4
    is_store: bool
    signed: bool         # for loads: sign-extend; ignored for stores
    op_idx: int          # for case_id

# Verifying load (used by store tests) — always zero-extending so we get raw bits.
VERIFY = {1: "ld.ub", 2: "ld.uh", 4: "ld.w"}

OPS: list[Op] = [
    Op("ld.b",  1, False, True,  0),
    Op("ld.ub", 1, False, False, 1),
    Op("ld.h",  2, False, True,  2),
    Op("ld.uh", 2, False, False, 3),
    Op("ld.w",  4, False, False, 4),
    Op("ld.b",  1, True,  False, 5),   # store byte
    Op("ld.h",  2, True,  False, 6),   # store halfword
    Op("ld.w",  4, True,  False, 7),   # store word
]

# Buffer: 256 bytes = 64 words.  Sized to accommodate both no-ext tests
# (max byte_offset = 63 for ld.b) and 1-ext tests (max byte_offset = (3<<6)|60
# = 252).  Larger than strictly needed for no-ext tests but uniform sizing
# keeps the generator simple.
BUF_WORDS = 64
BUF_BYTES = BUF_WORDS * 4

# Tests: (ext13_or_None, imm6, value) per op.
#
# When ext13 is None the test uses the no-ext form, in which imm6 is in
# scaled units (×width).  When ext13 is an integer the test prepends
# `ext ext13;`, switching the addressing mode to RAW BYTE displacement
# `byte_offset = (ext13 << 6) | imm6` regardless of width.  For ld.h/ld.w
# the byte_offset must satisfy alignment.
LOAD_TESTS_B: list[tuple[int | None, int, int]] = [
    # no-ext (imm6 = byte offset)
    (None, 0,  0x80),
    (None, 1,  0x7F),
    (None, 5,  0xFF),
    (None, 31, 0x55),
    (None, 63, 0xAA),
    # 1-ext (byte_offset = (ext<<6)|imm6, raw bytes; no alignment for byte op)
    (1,    0,  0x12),    # byte 64
    (1,    63, 0x34),    # byte 127
    (2,    0,  0x56),    # byte 128
    (3,    60, 0x78),    # byte 252 — max with 1 ext given 256-byte buffer
]

LOAD_TESTS_H: list[tuple[int | None, int, int]] = [
    # no-ext (imm6 in halfword units, byte_offset = imm6*2)
    (None, 0,  0x8000),
    (None, 1,  0x7FFF),
    (None, 5,  0x1234),
    (None, 15, 0xFFFF),
    (None, 31, 0xCAFE),
    # 1-ext (byte_offset must be even)
    (1,    0,  0xBEEF),  # byte 64
    (1,    62, 0xABCD),  # byte 126
    (2,    0,  0xDEAD),  # byte 128
    (3,    60, 0x1357),  # byte 252
]

LOAD_TESTS_W: list[tuple[int | None, int, int]] = [
    # no-ext (imm6 in word units, byte_offset = imm6*4)
    (None, 0,  0x80000000),
    (None, 1,  0x7FFFFFFF),
    (None, 5,  0x12345678),
    (None, 10, 0xDEADBEEF),
    (None, 15, 0xCAFEBABE),
    # 1-ext (byte_offset must be 4-aligned)
    (1,    0,  0xAA55AA55),  # byte 64
    (1,    60, 0x11223344),  # byte 124
    (2,    0,  0x55AA55AA),  # byte 128
    (3,    60, 0xFEEDFACE),  # byte 252
]

STORE_TESTS_B = LOAD_TESTS_B
STORE_TESTS_H = LOAD_TESTS_H
STORE_TESTS_W = LOAD_TESTS_W


def case_id(op_idx: int, val_idx: int) -> int:
    # 8 ops × up to 100 cases each
    return 1 + (op_idx * 100) + val_idx


def expected_load(op: Op, value: int) -> int:
    """Compute the value the load should return (sign- or zero-extended)."""
    raw = value & ((1 << (op.width * 8)) - 1)
    if op.width == 4:
        return raw
    return sign_ext(raw, op.width * 8) if op.signed else raw


def expected_store_readback(op: Op, value: int) -> int:
    """Read-back via VERIFY[width] (zero-extending) returns low N bits."""
    return value & ((1 << (op.width * 8)) - 1)


def compute_byte_offset(ext13: int | None, imm6: int, width: int) -> int:
    """No-ext: imm6 is in scaled units.  With ext: raw bytes."""
    if ext13 is None:
        return imm6 * width
    return ((ext13 & 0x1FFF) << 6) | (imm6 & 0x3F)


def offset_add_seq(byte_offset: int) -> str:
    """Emit the asm to add `byte_offset` to %r3.  Uses ext for offsets > 63."""
    if byte_offset < 64:
        return f'        "add %r3, {byte_offset}\\n"\n'
    hi = byte_offset >> 6
    lo = byte_offset & 0x3F
    return (f'        "ext {hi:#x}\\n"\n'
            f'        "add %r3, {lo}\\n"\n')


def ext_prefix(ext13: int | None) -> str:
    return f'        "ext {ext13:#x}\\n"\n' if ext13 is not None else ""


# Plain mnemonic for stores (drop signedness suffix when emitting via [%rb]).
def store_mnemonic_for_width(width: int) -> str:
    return {1: "ld.b", 2: "ld.h", 4: "ld.w"}[width]


def emit_load_fn(cid: int, op: Op, ext13: int | None, imm6: int, value: int) -> str:
    byte_offset = compute_byte_offset(ext13, imm6, op.width)
    assert 0 <= byte_offset < BUF_BYTES, (
        f"byte_offset {byte_offset} out of buffer ({BUF_BYTES}) for case {cid}")
    pre_store = store_mnemonic_for_width(op.width)
    ext_line = ext_prefix(ext13)
    add_seq = offset_add_seq(byte_offset)
    return f"""__attribute__((naked))
static uint32_t test_sp_{cid}(uint32_t poison __attribute__((unused)),
                              uint32_t value  __attribute__((unused))) {{
    asm(
        "pushn %r3\\n"
        "sub %sp, {BUF_WORDS}\\n"
        // fill buffer with poison
        "ld.w %r4, %r12\\n"
        "ld.w %r3, %sp\\n"
        "ext 1\\n"
        "ld.w %r5, 0\\n"          // r5 = 64 (BUF_WORDS) via ext+ld.w
        "1:\\n"
        "ld.w [%r3]+, %r4\\n"
        "sub %r5, 1\\n"
        "cmp %r5, 0\\n"
        "jrne 1b\\n"
        // pre-store value via [%rb] at byte_offset = {byte_offset}
        "ld.w %r3, %sp\\n"
{add_seq}        "{pre_store} [%r3], %r13\\n"
        // TEST under [%sp+imm6{'+ext' if ext13 is not None else ''}]
{ext_line}        "{op.name} %r10, [%sp+{imm6}]\\n"
        "add %sp, {BUF_WORDS}\\n"
        "popn %r3\\n"
        "ret\\n"
    );
}}
"""


def emit_store_fn(cid: int, op: Op, ext13: int | None, imm6: int, value: int) -> str:
    byte_offset = compute_byte_offset(ext13, imm6, op.width)
    assert 0 <= byte_offset < BUF_BYTES, (
        f"byte_offset {byte_offset} out of buffer ({BUF_BYTES}) for case {cid}")
    verify = VERIFY[op.width]
    ext_line = ext_prefix(ext13)
    add_seq = offset_add_seq(byte_offset)
    return f"""__attribute__((naked))
static uint32_t test_sp_{cid}(uint32_t poison __attribute__((unused)),
                              uint32_t value  __attribute__((unused))) {{
    asm(
        "pushn %r3\\n"
        "sub %sp, {BUF_WORDS}\\n"
        // fill with poison
        "ld.w %r4, %r12\\n"
        "ld.w %r3, %sp\\n"
        "ext 1\\n"
        "ld.w %r5, 0\\n"          // r5 = 64 (BUF_WORDS) via ext+ld.w
        "1:\\n"
        "ld.w [%r3]+, %r4\\n"
        "sub %r5, 1\\n"
        "cmp %r5, 0\\n"
        "jrne 1b\\n"
        // STORE TEST: [{'ext+'  if ext13 is not None else ''}ld.* [%sp+imm6], %r13]
{ext_line}        "{op.name} [%sp+{imm6}], %r13\\n"
        // VERIFY via [%rb] at byte_offset = {byte_offset}
        "ld.w %r3, %sp\\n"
{add_seq}        "{verify} %r10, [%r3]\\n"
        "add %sp, {BUF_WORDS}\\n"
        "popn %r3\\n"
        "ret\\n"
    );
}}
"""


def emit_caller(cid: int, op: Op, value: int) -> str:
    if op.is_store:
        expected = expected_store_readback(op, value)
    else:
        expected = expected_load(op, value)
    return f"""    {{
        uint32_t r = test_sp_{cid}(0xCCCCCCCCu, (uint32_t){value:#x}u);
        CHECK({cid}, r == {expected:#010x}u);
    }}"""


def tests_for(op: Op) -> list[tuple[int, int]]:
    if op.width == 1: return LOAD_TESTS_B if not op.is_store else STORE_TESTS_B
    if op.width == 2: return LOAD_TESTS_H if not op.is_store else STORE_TESTS_H
    return LOAD_TESTS_W if not op.is_store else STORE_TESTS_W


C_PROLOGUE = """// AUTO-GENERATED by gen_sp.py — do not edit by hand.
//
// SP-relative load/store tests using __attribute__((naked)) functions so
// each test fully controls its own stack frame.

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
    out = HERE / "test_gen_sp.c"
    fns: list[str] = []
    callers: list[str] = []

    for op in OPS:
        for vi, (ext13, imm6, value) in enumerate(tests_for(op)):
            cid = case_id(op.op_idx, vi)
            if op.is_store:
                fns.append(emit_store_fn(cid, op, ext13, imm6, value))
            else:
                fns.append(emit_load_fn(cid, op, ext13, imm6, value))
            callers.append(emit_caller(cid, op, value))

    out.write_text(
        C_PROLOGUE + "\n".join(fns) + C_EPILOGUE_TEMPLATE.format(calls="\n".join(callers)),
        encoding="utf-8",
    )
    print(f"# wrote {out} : {len(callers)} cases", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

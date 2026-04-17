#!/usr/bin/env python3
"""Generate branch + delay-slot regression tests for piece-emu.

Coverage:
  - Conditional branches (10 forms): jreq/jrne/jrgt/jrge/jrlt/jrle/
                                     jrugt/jruge/jrult/jrule
    Each tested in both taken and not-taken states by setting PSR explicitly.
  - Delayed conditional branches (jr*.d): same conditions, additionally
    verifies the delay-slot instruction runs whether or not the branch is taken.
  - Unconditional: jp sign8, jp.d sign8 (forward jumps within inline asm).
  - Calls: call sign8, call.d sign8 — verify return address is pushed and
    ret/ret.d resumes at the correct PC (pc+2 for call, pc+4 for call.d).
  - Register-indirect calls: call.d %rb — verify ra=pc+4 saved.
  - ret / ret.d — implicit in call/call.d cases.
  - Ext-extended conditional branches: 1-ext sign22 form.

Test pattern for cond branches:
    ld.w %psr, <pre-built PSR>      ; force PSR to known state
    ld.w r, 0                       ; r = 0 (will hold result)
    jrXX 1f                         ; branch under test
    ld.w r, 1                       ; r = 1 if NOT taken
  1:
  → r==0 means taken; r==1 means not taken.

For delayed variants we also probe a 'delay-slot ran' flag that should be 1
unconditionally (delay slot executes even when branch not taken? — actually
the slot ALWAYS executes once dispatch begins, regardless of cond outcome).

NOTE on jp.d %rb: forbidden (hardware bug, see CLAUDE.md). Skipped here.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

HERE = Path(__file__).resolve().parent

# PSR bit layout (verified against existing tests)
PSR_N = 1 << 0
PSR_Z = 1 << 1
PSR_V = 1 << 2
PSR_C = 1 << 3


def cond_taken(name: str, psr: int) -> bool:
    z = bool(psr & PSR_Z)
    n = bool(psr & PSR_N)
    v = bool(psr & PSR_V)
    c = bool(psr & PSR_C)
    return {
        "jreq":   lambda:  z,
        "jrne":   lambda:  not z,
        "jrgt":   lambda:  (not z) and (n == v),
        "jrge":   lambda:  n == v,
        "jrlt":   lambda:  n != v,
        "jrle":   lambda:  z or (n != v),
        "jrugt":  lambda:  (not z) and (not c),
        "jruge":  lambda:  not c,
        "jrult":  lambda:  c,
        "jrule":  lambda:  z or c,
    }[name]()


COND_NAMES = [
    "jreq", "jrne",
    "jrgt", "jrge", "jrlt", "jrle",
    "jrugt", "jruge", "jrult", "jrule",
]

# PSR test states — picked to give at least one taken and one not-taken
# outcome per condition.  Each state is a 4-bit mask over (N|Z|V|C).
PSR_STATES = [
    0,                              # all clear
    PSR_Z,                          # Z=1
    PSR_N,                          # N=1
    PSR_V,                          # V=1
    PSR_C,                          # C=1
    PSR_N | PSR_V,                  # N=V=1 (signed-equal-sign)
    PSR_Z | PSR_C,                  # Z=1, C=1
    PSR_N | PSR_C,                  # N=1, C=1
    PSR_N | PSR_V | PSR_C,          # N=V=C=1
    PSR_Z | PSR_N | PSR_V | PSR_C,  # all set
]


def case_id(group: int, vi: int) -> int:
    """group: 0=cond, 1=cond.d, 2=jp/call, 3=ext-cond."""
    return 1 + (group * 1000) + vi


def _asm(*lines: str) -> str:
    return "\n        ".join(f'"{ln}\\n"' for ln in lines)


# ----------------------------------------------------------- conditional branches

def emit_cond_branch(buf: list[str], cid: int, cond: str, psr: int) -> None:
    """No-delay conditional branch. Verify result reflects taken/not-taken.

    Sets PSR explicitly, then runs `cond 1f`.  After the branch, r==0 if taken,
    r==1 if not taken.  Expected based on cond_taken(cond, psr).
    """
    expected = 0 if cond_taken(cond, psr) else 1
    asm = _asm(
        "ld.w %%psr, %1",
        "ld.w %0, 0",            # r = 0 (taken outcome)
        f"{cond} 1f",            # branch under test
        "ld.w %0, 1",            # not-taken: r = 1
        "1:",
    )
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t r;
    asm volatile(
        {asm}
        : "=&r"(r)
        : "r"((uint32_t){psr:#x}u)
    );
    CHECK({cid}, r == {expected}u);
    return 0;
}}
""")


def emit_cond_branch_d(buf: list[str], cid: int, cond: str, psr: int) -> None:
    """Delayed conditional branch: cond.d  + verify delay-slot ran.

    For cond.d, the delay-slot instruction *always* executes — independent of
    whether the branch is taken.  We use a second variable `s` set in the
    delay slot to confirm.  The 'taken/not-taken' indicator is r.
    """
    expected_r = 0 if cond_taken(cond, psr) else 1
    expected_s = 1   # delay slot always runs
    asm = _asm(
        "ld.w %%psr, %2",
        "ld.w %0, 0",
        "ld.w %1, 0",
        f"{cond}.d 1f",
        "ld.w %1, 1",            # delay slot — must always execute
        "ld.w %0, 1",            # not-taken path
        "1:",
    )
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t r, s;
    asm volatile(
        {asm}
        : "=&r"(r), "=&r"(s)
        : "r"((uint32_t){psr:#x}u)
    );
    CHECK({cid}, r == {expected_r}u);
    CHECK({cid}, s == {expected_s}u);
    return 0;
}}
""")


# ----------------------------------------------------------- jp / call

def emit_jp_basic(buf: list[str], cid: int) -> None:
    """jp sign8: unconditional forward jump."""
    asm = _asm(
        "ld.w %0, 0",
        "jp 1f",
        "ld.w %0, 1",            # never reached if jp works
        "1:",
    )
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t r;
    asm volatile({asm} : "=&r"(r) : );
    CHECK({cid}, r == 0u);
    return 0;
}}
""")


def emit_jp_d(buf: list[str], cid: int) -> None:
    """jp.d sign8: jump with delay slot (always executes)."""
    asm = _asm(
        "ld.w %0, 0",
        "ld.w %1, 0",
        "jp.d 1f",
        "ld.w %1, 1",            # delay slot
        "ld.w %0, 1",            # never reached
        "1:",
    )
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t r, s;
    asm volatile({asm} : "=&r"(r), "=&r"(s) : );
    CHECK({cid}, r == 0u);
    CHECK({cid}, s == 1u);
    return 0;
}}
""")


def emit_call_ret(buf: list[str], cid: int) -> None:
    """call sign8 + ret: verify round-trip.  saves pc+2 as RA."""
    asm = _asm(
        "ld.w %0, 0",
        "call 2f",
        "ld.w %0, 1",            # ← RA (pc+2 of call) — set r = 1 after return
        "jp 3f",
        "2:",                    # function body
        "ret",
        "3:",
    )
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t r;
    asm volatile({asm} : "=&r"(r) : );
    CHECK({cid}, r == 1u);       // r = 1 means call returned to expected PC
    return 0;
}}
""")


def emit_call_ret_d(buf: list[str], cid: int) -> None:
    """call sign8 + ret.d: verify ret.d delay slot executes."""
    asm = _asm(
        "ld.w %0, 0",
        "ld.w %1, 0",
        "call 2f",
        "ld.w %0, 1",            # resume after call's pc+2
        "jp 3f",
        "2:",
        "ret.d",
        "ld.w %1, 1",            # ret.d delay slot (not popn/memory)
        "3:",
    )
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t r, s;
    asm volatile({asm} : "=&r"(r), "=&r"(s) : );
    CHECK({cid}, r == 1u);       // ret.d returned to pc+2 of call
    CHECK({cid}, s == 1u);       // ret.d delay slot ran
    return 0;
}}
""")


def emit_call_d_ret(buf: list[str], cid: int) -> None:
    """call.d sign8 + ret: saves pc+4 as RA (skipping delay slot).

    Layout:
        call.d 2f       ; ra = pc+4 (skips delay slot)
        ld.w s, 1       ; delay slot of call.d (always executes)
        ld.w r, 1       ; ← resume here
        jp 3f
      2:
        ret
      3:
    """
    asm = _asm(
        "ld.w %0, 0",
        "ld.w %1, 0",
        "call.d 2f",
        "ld.w %1, 1",            # call.d delay slot
        "ld.w %0, 1",            # resume: this is pc+4 after call.d
        "jp 3f",
        "2:",
        "ret",
        "3:",
    )
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t r, s;
    asm volatile({asm} : "=&r"(r), "=&r"(s) : );
    CHECK({cid}, r == 1u);       // call.d returned to pc+4 (skipped delay)
    CHECK({cid}, s == 1u);       // delay slot ran
    return 0;
}}
""")


def emit_call_d_ret_d(buf: list[str], cid: int) -> None:
    """call.d sign8 + ret.d: both delay slots execute.

    Layout:
        call.d 2f       ; ra = pc+4
        ld.w s, 1       ; delay of call.d
        ld.w r, 1       ; resume after ret.d
        jp 3f
      2:
        ret.d
        ld.w t, 1       ; delay of ret.d
      3:
    """
    asm = _asm(
        "ld.w %0, 0",
        "ld.w %1, 0",
        "ld.w %2, 0",
        "call.d 2f",
        "ld.w %1, 1",            # call.d delay slot
        "ld.w %0, 1",            # resume after ret.d's pc
        "jp 3f",
        "2:",
        "ret.d",
        "ld.w %2, 1",            # ret.d delay slot
        "3:",
    )
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t r, s, t;
    asm volatile({asm} : "=&r"(r), "=&r"(s), "=&r"(t) : );
    CHECK({cid}, r == 1u);       // resumed at call.d's pc+4
    CHECK({cid}, s == 1u);       // call.d delay slot ran
    CHECK({cid}, t == 1u);       // ret.d delay slot ran
    return 0;
}}
""")


# ----------------------------------------------------------- ext-extended

def emit_cond_branch_ext1(buf: list[str], cid: int, cond: str, psr: int) -> None:
    """1-ext conditional branch: target = pc + sign22.

    For testing we still use a short forward jump (within inline asm) — the
    point is to verify the assembler emits ext+jrXX correctly and the
    emulator handles the wider displacement encoding.

    Trick: use 'ext 0' which makes displacement = sign22 = (0<<8)|sign8
    → behaves like the no-ext case but exercises the ext code path in the
    emulator.
    """
    expected = 0 if cond_taken(cond, psr) else 1
    asm = _asm(
        "ld.w %%psr, %1",
        "ld.w %0, 0",
        "ext 0",                 # forces ext code path
        f"{cond} 1f",
        "ld.w %0, 1",
        "1:",
    )
    buf.append(f"""static int case_{cid}(void) {{
    uint32_t r;
    asm volatile(
        {asm}
        : "=&r"(r)
        : "r"((uint32_t){psr:#x}u)
    );
    CHECK({cid}, r == {expected}u);
    return 0;
}}
""")


# ----------------------------------------------------------- main

C_PROLOGUE = """// AUTO-GENERATED by gen_branch.py — do not edit by hand.
//
// Branch + delay-slot regression tests.  Sets PSR explicitly to drive
// conditional branch outcomes; uses inline asm with local labels to detect
// taken/not-taken paths via a captured variable.

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
    out = HERE / "test_gen_branch.c"
    cases: list[str] = []

    # group 0 — conditional branches without ext, no delay
    vi = 0
    for cond in COND_NAMES:
        for psr in PSR_STATES:
            cid = case_id(0, vi)
            emit_cond_branch(cases, cid, cond, psr)
            vi += 1

    # group 1 — conditional branches with .d
    vi = 0
    for cond in COND_NAMES:
        for psr in PSR_STATES:
            cid = case_id(1, vi)
            emit_cond_branch_d(cases, cid, cond, psr)
            vi += 1

    # group 2 — jp / jp.d / call+ret / call+ret.d / call.d+ret / call.d+ret.d
    vi = 0
    for emit_fn in (emit_jp_basic, emit_jp_d,
                    emit_call_ret, emit_call_ret_d,
                    emit_call_d_ret, emit_call_d_ret_d):
        cid = case_id(2, vi)
        emit_fn(cases, cid)
        vi += 1

    # group 3 — ext-extended conditional branches (1 ext)
    vi = 0
    for cond in COND_NAMES:
        for psr in PSR_STATES[:3]:           # smaller subset to keep test count down
            cid = case_id(3, vi)
            emit_cond_branch_ext1(cases, cid, cond, psr)
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

# piemu Implementation Reference

Source: `piemu/` (author: Autch, originally Naoyuki Sawa).
This document extracts implementation knowledge directly useful for the new emulator.
All code is the author's own work; references are not license-restricted.

---

## 0. What to Take and What to Discard

piemu is written in C. Several of its patterns exist only because C lacks better mechanisms.
The new emulator is modern C++; do not cargo-cult C idioms.

### Directly reusable (language-agnostic semantics)

| Section | Content |
|---------|---------|
| §6 | ALU operations and PSR flag update formulas — the math is the spec |
| §7 | Cycle counts per instruction |
| §8 | Branch condition codes (N, Z, V, C combinations) |
| §9 | Delay slot sequencing logic (order of: compute target → execute slot → update PC) |
| §10 | Trap mechanism: stack layout (push PC then PSR), entry conditions, IL masking |
| §11 | BCU wait-cycle formulas (read+1, write+2, 8-bit vs 16-bit scaling) |
| §12 | Known bugs (rl Z-flag, scan direction, mac loop, EXT-not-consumed behaviour) |
| §13 | Instruction dispatch table: the op1/op2 → instruction mapping |
| §5 | EXT resolution *formulas* (the bit arithmetic, not the C function signatures) |

### C idioms — replace with idiomatic C++

| piemu pattern | Why it exists in C | C++ alternative |
|---|---|---|
| `#define R(n) context->core.r[n]` | No `this`, no refs | `uint32_t& r(int n) { return gpr_[n]; }` member fn |
| `#define PSR (*(tPSR*)&s[0])` | Reinterpret-cast bitfield over raw word | A `Psr` struct/class with `bool n()`, `void set_n(bool)` accessors; or pack/unpack on read/write |
| `union INST` with bitfields | Decode via bitfield layout | Prefer explicit masks/shifts: `uint16_t op1 = (insn >> 10) & 7;` — C++ bitfield layout is still implementation-defined |
| `typedef void (*C33INST_EXEC)(PIEMU_CONTEXT*, INST)` | Function pointer table | `using Handler = void(*)(uint16_t);` in a `std::array<Handler, 65536>`, or a flat lambda array |
| `PIEMU_CONTEXT*` everywhere | No encapsulation | Put CPU state in a class; instruction handlers become methods or free functions taking `CpuState&` |
| `exec_ext_imm13` recursive eager model | Natural interrupt masking in single-thread | In C++ with the 65536-entry table, use the accumulation model: `pending_ext_[count_++] = imm13;` — interrupt masking is enforced by the single-threaded execution loop, not by recursion |
| `SDL_mutex` / `SDL_cond` for HALT | piemu is multi-threaded | New emulator is single-threaded (DESIGN_SPEC §2.1); HALT → compute next wake time → `nanosleep` or equivalent |

### §1–3 (state structure, encoding union, dispatch) in this document

Read these sections for *what fields exist* and *what the dispatch structure is*,
not for *how to represent them in C++*.  The instruction table in §13 is the most
portable takeaway from §3.

---

## 1. CPU State Structure (`include/core.h`)

```c
typedef struct _CORE {
    uint32_t r[16];   // R0–R15 (general-purpose)
    uint32_t s[4];    // S0=PSR, S1=SP, S2=ALR, S3=AHR
    uint32_t pc;
    INST ext[2];      // ext[0]=EXT1 (first/older), ext[1]=EXT2 (second)
    uint32_t d;       // delay-slot active flag
    uint32_t clk;     // cycle counter
    int in_halt;      // >0 when halted
    ...
} CORE;
```

Convenience macros (adapt for new emulator):
```c
#define PSR  (*(tPSR*)&core.s[0])  // bitfield over s[0]
#define SP   (core.s[1])
#define ALR  (core.s[2])
#define AHR  (core.s[3])
#define AR   (*(int64_t*)&core.s[2])  // ALR:AHR as 64-bit
#define PC   (core.pc)
#define EXT1 (core.ext[0])
#define EXT2 (core.ext[1])
```

PSR bitfield layout (`tPSR`):
```
bits  0    N   negative
      1    Z   zero
      2    V   overflow
      3    C   carry
      4    IE  interrupt enable
      5    —   (reserved)
      6    DS  delay-slot active (hardware)
      7    MO  MAC overflow (sticky, 0→1 only)
      8-11 IL  interrupt level
```
`s[0]` is used as both the full PSR word (for push/pop via `mem_writeW`) and the bitfield.

---

## 2. Instruction Encoding Unions (`include/core.h`)

All 16-bit instruction words are decoded via a union:

```c
typedef union _INST {
    uint16_t s;     // raw 16-bit word
    CLASS_0A c0a;   // nop/slp/halt/pushn/popn/brk/ret/call/jp (bits[12:9]=op1<4)
    CLASS_0B c0b;   // conditional branches / call sign8 / jp sign8
    CLASS_1A c1a;   // ld.*/st.* [%rb] and [%rb]+
    CLASS_1B c1b;   // add/sub/cmp/ld.w/and/or/xor/not reg-reg
    CLASS_2  c2;    // SP-relative ld/st
    CLASS_3  c3;    // immediate ALU (imm6/sign6)
    CLASS_4A c4a;   // add/sub %sp, imm10
    CLASS_4B c4b;   // shifts/rotates
    CLASS_4C c4c;   // scan/swap/mirror/div
    CLASS_5A c5a;   // special register ld
    CLASS_5B c5b;   // btst/bclr/bset/bnot
    CLASS_5C c5c;   // adc/sbc/ld.b|ub|h|uh reg-reg / mlt / mac
    CLASS_6  c6;    // ext imm13
    CLASS_7  c7;    // undefined (class 7 = all-zeros/reserved)
} INST;
```

Key field names for extraction:
```c
inst.c0a.cls    // bits[15:13] — always the class
inst.c0a.op1    // bits[12:9]
inst.c0a.op2    // bits[7:6]
inst.c0a.d      // bit[8] — delay slot flag
inst.c0a.imm2_rd_rs  // bits[3:0]
inst.c0b.sign8  // bits[7:0] — PC-relative displacement (signed)
inst.c0b.op1    // bits[12:9]
inst.c1a.op1    // bits[12:10]
inst.c1a.op2    // bits[9:8]
inst.c1a.rb     // bits[7:4]
inst.c1a.rs_rd  // bits[3:0]
inst.c2.op1     // bits[12:10]
inst.c2.imm6    // bits[9:4] (unscaled; scaling is done in ext_SPxIMM6)
inst.c2.rs_rd   // bits[3:0]
inst.c3.op1     // bits[12:10]
inst.c3.imm6_sign6   // bits[9:4]
inst.c3.rd      // bits[3:0]
inst.c4a.imm10  // bits[9:0] (word units, ×4)
inst.c4a.op1    // bits[12:10]
inst.c4b.op1    // bits[12:10]
inst.c4b.op2    // bits[9:8]
inst.c4b.rd     // bits[3:0]
inst.c4b.imm4_rs  // bits[7:4]
inst.c5b.imm3   // bits[2:0]
inst.c5b.rb     // bits[7:4]
inst.c6.imm13   // bits[12:0]
```

---

## 3. Instruction Dispatch (piemu's approach)

piemu uses **per-class function pointer tables**, not a 65536-entry flat table.
`core_inst()` dispatches by `inst.c0a.cls` (bits[15:13]):

```c
void core_inst(PIEMU_CONTEXT* context, INST inst) {
    switch (inst.c0a.cls) {
    case 0:
        if (inst.c0a.op1 < 4)
            C33INST_TABLE_C0A[inst.c0a.op1][inst.c0a.op2](context, inst);
        else
            C33INST_TABLE_C0B[inst.c0b.op1](context, inst);
        return;
    case 1: C33INST_TABLE_C1[inst.c1a.op2][inst.c1a.op1](context, inst); return;
    case 2: C33INST_TABLE_C2[inst.c2.op1](context, inst); return;
    case 3: C33INST_TABLE_C3[inst.c3.op1](context, inst); return;
    case 4:
        if (inst.c4a.op1 < 2)
            C33INST_TABLE_C4A[inst.c4a.op1](context, inst);
        else
            C33INST_TABLE_C4BC[inst.c4b.op2][inst.c4b.op1](context, inst);
        return;
    case 5: C33INST_TABLE_C5[inst.c5a.op2][inst.c5a.op1](context, inst); return;
    case 6: exec_ext_imm13(context, inst); return;
    case 7: return; // undefined — silently ignored
    }
}
```

The **new emulator** should use a 65536-entry table instead (DESIGN_SPEC §2.7), but the
per-class sub-table layout above is a useful reference for understanding the decode structure.

---

## 4. EXT Handling — piemu's "Eager Execution" Model

piemu executes the entire EXT+...+INST sequence **atomically** in a single recursive call.
This naturally prevents interrupts between EXT and its target.

```c
// class6.c
void exec_ext_imm13(PIEMU_CONTEXT *context, INST inst)
{
    NO_DELAY;
    if (!EXT1.s)       EXT1.s = inst.s;   // first ext
    else if (!EXT2.s)  EXT2.s = inst.s;   // second ext
    else               DIE();              // third ext: illegal

    PC += 2;    // advance past this EXT
    CLK += 1;

    // Execute the next instruction immediately (interrupt-masked)
    INST inst2;
    inst2.s = mem_readH(context, PC);
    core_inst(context, inst2);             // recursive: may encounter another EXT

    if (EXT1.s) DIE();  // EXT must be fully consumed by target instruction
}
```

If the next instruction is ANOTHER EXT, the recursion goes one level deeper.
The final target instruction calls one of the `ext_xxx()` functions which clears EXT1/EXT2.

**Alternative for new emulator**: the DESIGN_SPEC §2.2 recommends the "accumulate then
apply" model (same as the LLVM disassembler), which works better with a flat 65536-entry
decode table.  In that model, `pending_ext[0]` and `pending_ext[1]` are cleared when any
non-EXT instruction is decoded, regardless of whether it is extendable.

---

## 5. Immediate Resolution Functions (`core/ext.c`)

All return the effective address or resolved immediate as a plain `int`.
EXT1/EXT2 are cleared after use (the instruction "consumes" them).

### `ext_imm6` — unsigned 6-bit immediate (add, sub, SP-relative)

```c
int ext_imm6(PIEMU_CONTEXT *context, int imm6) {
    imm6 &= 0x3F;
    if (EXT2.s)  return imm6 | (EXT2.c6.imm13 << 6) | (EXT1.c6.imm13 << 19); // 32-bit, no sign
    if (EXT1.s)  return imm6 | (EXT1.c6.imm13 << 6);                          // 19-bit, no sign
    return imm6;                                                                 // 6-bit, unsigned
}
```

### `ext_sign6` — signed 6-bit immediate (ld.w, and, or, xor, not, cmp)

```c
int ext_sign6(PIEMU_CONTEXT *context, int sign6) {
    int data, bits;
    sign6 &= 0x3F;
    if (EXT2.s) { data = sign6|(EXT2.c6.imm13<<6)|(EXT1.c6.imm13<<19); bits=32; }
    else if (EXT1.s) { data = sign6|(EXT1.c6.imm13<<6); bits=19; }
    else { data = sign6; bits=6; }
    return sign_ext(data, bits);  // sign_ext: left-shift then arithmetic-right-shift
}
```

Note: with 2 ext, `bits=32`, so `sign_ext(data, 32)` returns data unchanged (full 32-bit value).

### `ext_RB` — unsigned displacement for `[%rb]`

```c
int ext_RB(PIEMU_CONTEXT *context, int rb) {
    int disp;
    if (EXT2.s)      disp = (EXT2.c6.imm13 << 0) | (EXT1.c6.imm13 << 13);  // 26-bit, unsigned
    else if (EXT1.s) disp = EXT1.c6.imm13;                                    // 13-bit, unsigned
    else             disp = 0;
    return R(rb) + disp;   // returns effective byte address (EA = rb + disp)
}
```

**Negative offsets are impossible** (unsigned). Range: [0, 8191] for 1 ext, [0, 67108863] for 2.

### `ext_3op` — unsigned immediate for 3-operand Class 1 ALU (ext+add/sub/and/or/xor %rd,%rs)

```c
int ext_3op(PIEMU_CONTEXT *context) {
    // Caller verifies EXT1.s != 0 before calling
    int data;
    if (EXT2.s)      data = (EXT2.c6.imm13 << 0) | (EXT1.c6.imm13 << 13);  // 26-bit unsigned
    else             data = EXT1.c6.imm13;                                    // 13-bit unsigned
    // clears EXT1/EXT2
    return data;
}
```

Note: `cmp`/`and`/`or`/`xor` in 3-operand (ext+rr) mode treat the immediate as **unsigned**
even though their 2-operand (immediate) form uses `sign6`.  piemu comment: "cmp/and/or/xor/not
も、3op拡張時はsignではなくimmとなります。"

### `ext_SPxIMM6` — SP-relative address

```c
int ext_SPxIMM6(PIEMU_CONTEXT *context, int imm6, int size) {
    imm6 &= 0x3F;
    int disp;
    if (EXT2.s)      disp = imm6 | (EXT2.c6.imm13 << 6) | (EXT1.c6.imm13 << 19);
    else if (EXT1.s) disp = imm6 | (EXT1.c6.imm13 << 6);
    else             disp = imm6 * size;  // ← scaling only without EXT!
    return SP + disp;
}
```

**Critical**: without EXT, `imm6` is scaled by element size (×1/×2/×4 for byte/half/word).
With EXT, the combined value is the raw byte offset (no scaling).

### `ext_PCxSIGN8` — PC-relative branch target

```c
int ext_PCxSIGN8(PIEMU_CONTEXT *context, int sign8) {
    int disp, bits;
    sign8 &= 0xFF;
    if (EXT2.s) {
        // NOTE: only 10 of EXT1's 13 bits are used (>> 3 << 21)
        disp = sign8 | (EXT2.c6.imm13 << 8) | ((EXT1.c6.imm13 >> 3) << 21);
        bits = 8 + 13 + 10;  // 31 bits total
    } else if (EXT1.s) {
        disp = sign8 | (EXT1.c6.imm13 << 8);
        bits = 8 + 13;       // 21 bits
    } else {
        disp = sign8;
        bits = 8;
    }
    return PC + sign_ext(disp, bits) * 2;
}
```

Note: `PC` here is the address of the **branch instruction itself** (not PC+2).
The LLVM disassembler uses the same formula.

The 2-ext case in piemu uses only 10 bits from EXT1 (31-bit total), while the LLVM
disassembler uses all 13 bits from EXT1 (34-bit total).  For addresses within the 28-bit
address space, both formulas produce the same result in practice.  Use the LLVM formula
(34-bit) as it is more conservative.

---

## 6. ALU Operations and PSR Flag Updates (`core/common.c`)

Each ALU operation takes `tPSR *psr` and updates flags. Canonical implementations:

```c
c33int add(tPSR *psr, c33int a, c33int b) {
    c33int c = a + b;
    psr->n = c < 0;
    psr->z = !c;
    psr->v = (a < 0 && b < 0 && c >= 0) || (a >= 0 && b >= 0 && c < 0);
    psr->c = (c33word)c < (c33word)a;   // unsigned overflow = carry
    return c;
}

c33int sub(tPSR *psr, c33int a, c33int b) {
    c33int c = a - b;
    psr->n = c < 0;
    psr->z = !c;
    psr->v = (a >= 0 && b < 0 && c < 0) || (a < 0 && b >= 0 && c >= 0);
    psr->c = (c33word)c > (c33word)a;   // unsigned borrow = carry
    return c;
}

c33int adc(tPSR *psr, c33int a, c33int b) {
    c33int c = a + b;
    c33int d = c + psr->c;
    psr->n = d < 0;
    psr->z = !d;
    psr->v = (a < 0 && b < 0 && d >= 0) || (a >= 0 && b >= 0 && d < 0);
    psr->c = ((c33word)c < (c33word)a) || ((c33word)d < (c33word)c);
    return d;
}

c33int sbc(tPSR *psr, c33int a, c33int b) {
    c33int c = a - b;
    c33int d = c - psr->c;
    psr->n = d < 0;
    psr->z = !d;
    psr->v = (a >= 0 && b < 0 && d < 0) || (a < 0 && b >= 0 && d >= 0);
    psr->c = ((c33word)c > (c33word)a) || ((c33word)d > (c33word)c);
    return d;
}

// and/or/xor: update N, Z only (not V, C)
c33int and(tPSR *psr, c33int a, c33int b) { c33int r = a & b; psr->n = r<0; psr->z = !r; return r; }
c33int or (tPSR *psr, c33int a, c33int b) { c33int r = a | b; psr->n = r<0; psr->z = !r; return r; }
c33int xor(tPSR *psr, c33int a, c33int b) { c33int r = a ^ b; psr->n = r<0; psr->z = !r; return r; }
c33int not(tPSR *psr, c33int a)           { c33int r = ~a;    psr->n = r<0; psr->z = !r; return r; }

// Shifts: update N, Z only. b must be in [0, 8] — DIE otherwise.
c33int srl(tPSR *psr, c33int a, c33int b) { c33int r = (c33word)a >> b; psr->n=r<0; psr->z=!r; return r; }
c33int sll(tPSR *psr, c33int a, c33int b) { c33int r = (c33word)a << b; psr->n=r<0; psr->z=!r; return r; }
c33int sra(tPSR *psr, c33int a, c33int b) { c33int r = (c33int) a >> b; psr->n=r<0; psr->z=!r; return r; }
c33int sla(tPSR *psr, c33int a, c33int b) { c33int r = (c33int) a << b; psr->n=r<0; psr->z=!r; return r; }

c33int rr(tPSR *psr, c33int a, c33int b) {
    c33int r = ((c33word)a >> b) | ((c33word)a << (32 - b));
    psr->n = r < 0; psr->z = !r; return r;
}
c33int rl(tPSR *psr, c33int a, c33int b) {
    c33int r = ((c33word)a << b) | ((c33word)a >> (32 - b));
    psr->n = r < 0;
    psr->z = r;     // ← BUG: should be !r (see §12)
    return r;
}
```

**Flag update summary**:
- `add`, `sub`, `adc`, `sbc`: update N, Z, V, C
- `and`, `or`, `xor`, `not`, shifts, rotates: update N, Z **only** (V and C unchanged)
- `cmp`: same as sub but result discarded
- `ld.w %rd, sign6` (Class 3): **does NOT update any flags** (no `&PSR` call)
- `btst`: updates Z only
- `scan0`, `scan1`: update Z and C only
- `swap`, `mirror`: **no flag update**
- `mlt.*`, `mltu.*`, `mac`: no flag update (MAC overflow → MO flag only)

---

## 7. Cycle Counts

```
nop/slp/halt         1
ext                  1 (+ cycles of target instruction)
add/sub/cmp (reg)    1
and/or/xor/not       1
ld.*/st.* [%rb]      1
ld.*/st.* [%sp]      1
ld.*/st.* imm        1
add/sub/and/or/xor reg-reg  1
srl/sll/sra/sla/rr/rl       1
scan0/scan1/swap/mirror      1
adc/sbc              1
ld.b/ub/h/uh %rd,%rs 1
mlt.h/mltu.h         1
add/sub %sp, imm10   1

ld.*/st.* [%rb]+     read: 2,  write: 1
btst/bclr/bset/bnot  3

mlt.w/mltu.w         5
reti                 5
mac                  2 per iteration, then 4 on completion

int imm2             10

pushn %rN            1 per register pushed (N+1 total)
popn %rN             1 per register popped (N+1 total)

call %rb             3 (no delay),  2 (delay)
ret                  4 (no delay),  3 (delay)
jp %rb               2 (no delay),  1 (delay)
call sign8           3 (no delay),  2 (delay)
jp sign8             2 (no delay),  1 (delay)
jrXX (conditional)   2 if (taken && no delay),  else 1
```

External memory access adds wait cycles per the BCU (see §10).
The `CLK += 1` in instructions counts the base cycle; BCU adds wait on top.

---

## 8. Branch Condition Codes

```c
jrgt:  !(N^V) && !Z    // signed greater-than
jrge:  !(N^V)           // signed greater-or-equal
jrlt:   (N^V)           // signed less-than
jrle:   (N^V) || Z      // signed less-or-equal
jrugt: !C && !Z         // unsigned greater-than
jruge: !C               // unsigned greater-or-equal
jrult:  C               // unsigned less-than
jrule:  C || Z          // unsigned less-or-equal
jreq:   Z               // equal
jrne:  !Z               // not-equal
```

---

## 9. Delayed Branch Implementation

piemu executes the delay slot instruction **immediately and recursively** inside the
branch handler, BEFORE updating PC to the branch target:

```c
void exec_delay(PIEMU_CONTEXT *context, int dflag) {
    if (!dflag) return;
    context->core.d = 1;                     // set delay-slot flag
    INST d_inst;
    d_inst.s = mem_readH(context, PC + 2);   // read slot at branch_addr + 2
    core_inst(context, d_inst);              // execute it; advances PC by 2
    context->core.d = 0;
}
```

Sequence for `call.d sign8`:
1. `addr = ext_PCxSIGN8(...)` using current PC (= call instruction address)
2. `exec_delay(1)` → reads delay slot at `PC+2`, executes it → `PC = call_addr + 2`
3. `mem_writeW(SP, PC + 2)` → pushes `call_addr + 4` as return address (correct: instruction after slot)
4. `PC = addr`

The delay slot flag `core.d` prevents delay-slot instructions from being used inside
another delay slot (`NO_DELAY` checks `context->core.d`).

**Instructions that CANNOT be in a delay slot** (marked `NO_DELAY`):
- All Class 6 (EXT)
- All branches (jp, call, jr*, ret, reti)
- pushn/popn
- ld.b/ub/h/uh %rd,%rs (register-register narrow cast) — Class 5C
- int imm2, brk

**Instructions that CAN be in a delay slot** (despite looking risky):
- `ld.w %sd, %rs` / `ld.w %rd, %ss` (special-register move) — piemu note: "EPSONライブラリの除算ルーチンが使ってる"
- All memory loads/stores (Class 1, 2)
- All ALU instructions

---

## 10. Trap / Interrupt Mechanism

### Trap table
Trap table base = `0x400` (`TPVECTORTOP`).  Each entry = 4-byte word (jump address).
```
trap_addr = mem_readW(TPVECTORTOP + trap_no * 4)
```

### Trap dispatch (`core_trap_from_core`)

```c
void core_trap_from_core(PIEMU_CONTEXT *context, int no, int level) {
    // Maskable interrupts (no >= 16): check IE and IL
    if (no >= 16) {
        if (!PSR.ie) return;
        if ((unsigned)level <= PSR.il) return;
    }
    // Push PC (NOT PC+2!) and PSR, jump to handler
    SP -= 4; mem_writeW(context, SP, PC);
    SP -= 4; mem_writeW(context, SP, S(0));    // S(0) = PSR
    PC = mem_readW(context, TPVECTORTOP + no * 4);
    PSR.ie = 0;
    if (no >= 16) PSR.il = level;
}
```

Trap numbers 0–15 are **non-maskable** (no IE/IL check, no IL update on entry).
Trap numbers 16+ are **maskable**: require IE=1 and level > PSR.IL.

`reti` sequence:
```c
S(0) = mem_readW(context, SP); SP += 4;  // restore PSR
PC   = mem_readW(context, SP); SP += 4;  // restore PC
```

### Device → CPU interrupt delivery

Devices call `core_trap_from_devices()` which queues the trap and wakes the CPU from HALT.
`core_workex()` calls `core_handle_trap()` after each instruction to process the queue.
For the new emulator (single-threaded), this queue can be replaced by a pending-trap flag
checked at instruction boundaries.

---

## 11. BCU Address Dispatch (`bcu.c`)

piemu uses a hand-coded switch-case dispatch:

```c
// Area table (piemu area_tbl[])
//  Area  Address range         Handler
//   0    0x000_0000–0x05F_FFFF  FRAM (internal RAM + I/O)
//   1    0x003_0000–0x05F_FFFF  IOMEM (I/O registers)
//   2    0x006_0000–0x07F_FFFF  (none — debug/reserved)
//   3    0x008_0000–0x0FF_FFFF  (none)
//   4    0x100_0000–0x1FF_FFFF  SRAM
//   5    0x200_0000–0x2FF_FFFF  (none)
//   6    0x300_0000–0x3FF_FFFF  (none)
//   7    0x400_0000–0x5FF_FFFF  USBC (USB controller)
//   8    0x600_0000–0x7FF_FFFF  (none)
//   9    0x800_0000–0xBFF_FFFF  (none)
//  10    0xC00_0000–0xFFF_FFFF  Flash
```

The dispatch switch decodes `bits[27:20]` of the 28-bit address for fast area selection.
Internal areas (FRAM, IOMEM) → `NO_WAIT` (0 extra cycles).

Wait-cycle calculation (external areas):
```c
// 16-bit device (P/ECE standard):
//   byte/halfword access: CLK += wt
//   word (32-bit) access: CLK += wt * 2
// 8-bit device:
//   CLK += wt * access_size_bytes

// Plus base cost for the bus cycle itself:
CLK += mode;   // mode=1 for READ (+1 cycle), mode=2 for WRITE (+2 cycles)
```

The `wt` value comes from BCU control registers (`bA6_4_A5WT` etc.), which the kernel
programs at boot.  Initial value at power-on = 7 (maximum wait = safest setting).

**Note**: address `0x060000` (debug area) is mapped to `DEF_NONE` in piemu — accessing it
reads 0 and writes are silently discarded.  The new emulator should intercept this range for
the semihosting port (DESIGN_SPEC §4) **before** the BCU dispatch.

---

## 12. Known Bugs and Quirks in piemu

### Bug: `rl` sets Z = result instead of Z = !result

In `core/common.c`:
```c
c33int rl(tPSR *psr, c33int a, c33int b) {
    c33int r = ((c33word)a << b) | ((c33word)a >> (32 - b));
    psr->n = r < 0;
    psr->z = r;      // ← BUG: should be !r
    return r;
}
```
`rl %rd, 0` with rd=0 would set Z=0 (wrong, should be 1).
The new emulator must use `psr->z = !r`.

### Quirk: `mac` does not advance PC during loop

```c
void exec_mac_rs(PIEMU_CONTEXT *context, INST inst) {
    if (R(inst.c5c.rs)) {   // counter != 0: do one iteration
        // ... accumulate, decrement counter, advance addresses
        CLK += 2;
        // PC stays at mac instruction — next fetch re-executes mac
    } else {                // counter == 0: done
        PC += 2;
        CLK += 4;
    }
}
```
This differs from real hardware (which executes the entire loop without re-fetching the
instruction), but allows interrupt delivery between iterations.

### Quirk: EXT + non-extendable instruction → always DIE

`exec_ext_imm13` always checks `if (EXT1.s) DIE()` after the target instruction runs,
even in non-debug builds.  If a shift or rotate instruction follows EXT, piemu aborts.
Real hardware silently ignores the EXT.  The new emulator should issue a **warning** and
continue (DESIGN_SPEC §2.6).

### Quirk: `int imm2` fires a software trap

```c
PC += 2; CLK += 10;
core_trap_from_core(context, TRAP_SOFTINT0 + inst.c0a.imm2_rd_rs, 0);
```
The `int` instruction advances PC before pushing it, so the trap handler returns to the
instruction AFTER the `int`.

### Quirk: `add/sub %sp, imm10` has NO_EXT

piemu marks these with `NO_EXT` (asserts no pending ext in debug mode).
DESIGN_SPEC lists `add/sub %sp, imm10` as extendable with 10-bit field (unsigned).
Whether the hardware supports this is unconfirmed — follow the CPU manual.

### Note: `scan0` / `scan1` direction

Initial piemu code (2003) had scan0/scan1 in the wrong direction; this was fixed in 2003.
The correct implementation scans from **MSB toward LSB**:
```c
// scan0: count leading 1s from MSB
for (ub = 0; ub < 8; ub++) {
    if (!(ua & (1u << 31))) break;   // stop at first 0
    ua <<= 1;
}
```

---

## 13. Complete Instruction Table (piemu function map)

For convenience when building a 65536-entry table, the piemu dispatch maps to these handlers.
The new emulator should verify each against the CPU manual.

**Class 0A** — `[op1][op2]`:
```
[0][0]=nop  [0][1]=slp  [0][2]=halt  [0][3]=DIE
[1][0]=pushn %rs  [1][1]=popn %rd  [1][2]=DIE  [1][3]=DIE
[2][0]=brk  [2][1]=retd  [2][2]=int imm2  [2][3]=reti
[3][0]=call %rb  [3][1]=ret  [3][2]=jp %rb  [3][3]=DIE
```

**Class 0B** — `[op1]` (op1 4–15):
```
[4]=jrgt  [5]=jrge  [6]=jrlt  [7]=jrle
[8]=jrugt  [9]=jruge  [10]=jrult  [11]=jrule
[12]=jreq  [13]=jrne  [14]=call sign8  [15]=jp sign8
```
All support `.d` (delay) bit.

**Class 1** — `[op2][op1]`:
```
[0][0]=ld.b rd,[rb]   [0][1]=ld.ub rd,[rb]  [0][2]=ld.h rd,[rb]   [0][3]=ld.uh rd,[rb]
[0][4]=ld.w rd,[rb]   [0][5]=ld.b [rb],rs   [0][6]=ld.h [rb],rs   [0][7]=ld.w [rb],rs
[1][0]=ld.b rd,[rb]+  [1][1]=ld.ub rd,[rb]+ [1][2]=ld.h rd,[rb]+  [1][3]=ld.uh rd,[rb]+
[1][4]=ld.w rd,[rb]+  [1][5]=ld.b [rb]+,rs  [1][6]=ld.h [rb]+,rs  [1][7]=ld.w [rb]+,rs
[2][0]=add rd,rs      [2][1]=sub rd,rs       [2][2]=cmp rd,rs       [2][3]=ld.w rd,rs
[2][4]=and rd,rs      [2][5]=or rd,rs        [2][6]=xor rd,rs       [2][7]=not rd,rs
[3][*]=DIE
```
Class 1B (op2=10) with EXT1 pending → 3-operand form (uses `ext_3op`).

**Class 2** — `[op1]`:
```
[0]=ld.b rd,[sp+imm6]   [1]=ld.ub rd,[sp+imm6]  [2]=ld.h rd,[sp+imm6]  [3]=ld.uh rd,[sp+imm6]
[4]=ld.w rd,[sp+imm6]   [5]=ld.b [sp+imm6],rs   [6]=ld.h [sp+imm6],rs  [7]=ld.w [sp+imm6],rs
```

**Class 3** — `[op1]` (all use 6-bit field):
```
[0]=add rd,imm6   [1]=sub rd,imm6  [2]=cmp rd,sign6  [3]=ld.w rd,sign6
[4]=and rd,sign6  [5]=or rd,sign6  [6]=xor rd,sign6  [7]=not rd,sign6
```
Note: `add`/`sub` use `ext_imm6` (unsigned); `cmp`/`ld.w`/`and`/`or`/`xor`/`not` use `ext_sign6` (signed).

**Class 4A** — `[op1]` (op1 0–1):
```
[0]=add %sp,imm10   [1]=sub %sp,imm10   [2-7]=DIE
```

**Class 4BC** — `[op2][op1]`:
```
[0][0-1]=DIE  [0][2]=srl rd,imm4  [0][3]=sll rd,imm4  [0][4]=sra rd,imm4  [0][5]=sla rd,imm4  [0][6]=rr rd,imm4  [0][7]=rl rd,imm4
[1][0-1]=DIE  [1][2]=srl rd,rs    [1][3]=sll rd,rs    [1][4]=sra rd,rs    [1][5]=sla rd,rs    [1][6]=rr rd,rs    [1][7]=rl rd,rs
[2][0-1]=DIE  [2][2]=scan0 rd,rs  [2][3]=scan1 rd,rs  [2][4]=swap rd,rs   [2][5]=mirror rd,rs [2][6-7]=DIE
[3][0-1]=DIE  [3][2]=div0s rs     [3][3]=div0u rs     [3][4]=div1 rs      [3][5]=div2s rs     [3][6]=div3s  [3][7]=DIE
```

**Class 5** — `[op2][op1]`:
```
[0][0]=ld.w %sd,%rs  [0][1]=ld.w %rd,%ss  [0][2]=btst [rb],imm3  [0][3]=bclr [rb],imm3
[0][4]=bset [rb],imm3  [0][5]=bnot [rb],imm3  [0][6]=adc rd,rs  [0][7]=sbc rd,rs
[1][0]=ld.b rd,rs  [1][1]=ld.ub rd,rs  [1][2]=ld.h rd,rs  [1][3]=ld.uh rd,rs  [1][4-7]=DIE
[2][0]=mlt.h rd,rs  [2][1]=mltu.h rd,rs  [2][2]=mlt.w rd,rs  [2][3]=mltu.w rd,rs
[2][4]=mac rs  [2][5-7]=DIE
[3][*]=DIE
```

**Class 6**: `ext imm13` (always).

**Class 7**: silently ignored (return without error) in piemu.

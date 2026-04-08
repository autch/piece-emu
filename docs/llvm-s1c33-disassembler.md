# LLVM S1C33 Disassembler Reference

This document describes the implementation of the LLVM S1C33 disassembler
(`piece-toolchain-llvm/llvm/llvm/lib/Target/S1C33/Disassembler/S1C33Disassembler.cpp`)
and the known divergences from correct emulator behaviour.  The primary audience is the
new emulator's disassembler implementor.

---

## 1. Overall Architecture

The LLVM disassembler is two-stage:

1. **TableGen-generated `decodeInstruction(DecoderTable16, ...)`** — converts the raw 16-bit
   instruction word into an `MCInst` with operands decoded from the instruction fields.
   Operand decoders are called per-field; they store sign-extended or zero-extended values
   directly into the `MCInst` operand slots.

2. **`applyPendingExt` / `applyPendingExtPCRel`** — post-decode patch pass: reads back the
   raw immediate field from the `MCInst`, re-combines it with accumulated EXT values, then
   writes a comment (not the operand) showing the full extended value.

The emulator disassembler should implement stage 1 with a 65536-entry lookup table (one
entry per possible 16-bit instruction word), which makes stage 2 straightforward.

---

## 2. EXT Accumulation

```cpp
// Member field: at most 2 pending ext values (oldest = provides highest bits)
mutable SmallVector<int64_t, 2> PendingExt;

// In getInstruction():
if (Instr.getOpcode() == S1C33::EXT) {
    if (PendingExt.size() < 2)
        PendingExt.push_back(Instr.getOperand(0).getImm());  // imm13, 0..8191
    return Result;  // display EXT as-is, do not flush
}
```

Key properties:
- `PendingExt[0]` = first (oldest) EXT instruction → contributes **highest** bits
- `PendingExt[1]` = second EXT instruction → contributes **middle** bits
- A third EXT simply overwrites index 1 (silently dropped; malformed code)
- Any non-EXT instruction flushes `PendingExt` (even if the instruction is not extendable)

---

## 3. applyPendingExt — Non-PC-Relative Instructions

Called for all non-branch instructions when `PendingExt` is non-empty.

### Combination formula

```cpp
uint64_t RawField = (uint64_t)Inst.getOperand(ImmIdx).getImm() & ((1ULL << Width) - 1);

// 1 ext:
uint64_t Combined = ((uint64_t)PendingExt[0] << Width) | RawField;
Extended = SignExtend64(Combined, 13 + Width);

// 2 exts:
uint64_t Combined = ((uint64_t)PendingExt[0] << (13 + Width)) |
                    ((uint64_t)PendingExt[1] << Width) | RawField;
Extended = SignExtend64(Combined, 13 + 13 + Width);
```

### Immediate field widths (`getExtImmWidth`)

| Instructions | Width |
|---|---|
| SP-relative ld/st (Class 2): `LDB_sp`, `LDUB_sp`, `LDH_sp`, `LDUH_sp`, `LDW_sp`, `STB_sp`, `STH_sp`, `STW_sp` | 6 |
| Immediate ALU (Class 3): `ADD_ri`, `SUB_ri`, `AND_ri`, `OR_ri`, `XOR_ri`, `CMP_ri`, `MOV_ri6`, `NOT_ri` | 6 |
| SP adjust (Class 3): `ADDSP_i`, `SUBSP_i` | 10 |
| Class 1 register-indirect (no base immediate) | N/A — handled by `applyPendingExtPCRel`? No — see §5 |

---

## 4. applyPendingExtPCRel — PC-Relative Branches

Called for branch/call instructions: `JP_i`, `JP_D_i`, `CALL_i`, `CALL_D_i`, all `JR**` variants.

### Why separate from applyPendingExt

`decodePCRelSimm8Operand` pre-computes `Address + 2 * sign8` and stores the **target address**
(or a symbolic operand) in the `MCInst`.  After combining with EXT, the displacement changes,
so the pre-computed target is wrong.  `applyPendingExtPCRel` re-extracts the raw 8-bit
displacement directly from the instruction word and recomputes:

```cpp
uint8_t RawDisp = RawInsn & 0xFF;  // bits[7:0] of the 16-bit instruction

// 1 ext:
Combined = ((uint64_t)PendingExt[0] << 8) | RawDisp;
Extended = SignExtend64(Combined, 13 + 8);   // 21-bit signed

// 2 exts:
Combined = ((uint64_t)PendingExt[0] << 21) | ((uint64_t)PendingExt[1] << 8) | RawDisp;
Extended = SignExtend64(Combined, 13 + 13 + 8);  // 34-bit signed
```

`Extended` is the **halfword-unit signed offset** (same unit as the raw 8-bit field).
The target byte address is `Address + 2 * Extended`, where `Address` = address of the
branch instruction itself (not the next instruction).

The MCInst's immediate operand is updated so that `MCInstrAnalysis::evaluateBranch()` can
reconstruct the target.

---

## 5. Known Bug: sign_extend Applied to unsigned Immediates

`applyPendingExt` calls `SignExtend64` for **all** non-PC-relative instructions, including
`ADD_ri` and `SUB_ri` whose `imm6` field is **unsigned** (zero-extended).

Concretely, with `ext 0x1800 / add %r0, 0`:
- Width = 6, RawField = 0
- Combined = 0x1800 << 6 = 0x60000
- `SignExtend64(0x60000, 19)` = `0x60000 - 0x80000` = **-131072** (wrong, shown as `# -0x20000`)
- Correct: `+393216` (`# 0x60000`)

The bug triggers whenever the high bit of the 19-bit (or 32-bit) combined value is 1,
which happens when `ext_val >= 0x1000` (for 1-ext + 6-bit) or `ext_val >= 0x4000000` (2-ext).

**This is display-only**: `llvm-objdump` shows the wrong comment for the extended value, but
code generation is not affected (codegen uses separate sign-tracking).

**New emulator impact**: when cross-validating disassembler output against `llvm-objdump`,
expect mismatches for `ext+add`/`ext+sub` with `ext_val >= 0x1000`.  The emulator must
implement the **correct** behaviour (zero-extend for `add`/`sub`/SP-relative instructions;
sign-extend for `ld.w`/`and`/`or`/`xor`/`not`/`cmp` and all branches), per DESIGN_SPEC §2.3.

---

## 6. Class 1 Register-Indirect with EXT (not handled by applyPendingExt)

`ld.* [%rb]` and `st.* [%rb]` (Class 1 with no immediate field in the base encoding) are NOT
in `getExtImmWidth`, so `applyPendingExt` does nothing for them — it finds `Width == 0` and
clears `PendingExt` silently.

The EXT value serves as a **byte displacement** (zero-extended, range 0–8191):
```
ext imm13 / ld.w %rd, [%rb]  →  EA = rb + zero_extend(imm13)
```

The LLVM disassembler does **not** show the extended displacement for these instructions.
The emulator must handle this case correctly: EXT before a Class 1 register-indirect
instruction provides an unsigned 13-bit (or 26-bit with 2 exts) byte offset.

---

## 7. Instruction Classes NOT Extendable

The following instructions consume pending EXT but ignore it (emulator should log a warning):

- Shift/rotate with immediate: `srl/sll/sra/sla/rr/rl %rd, imm4`
  The shift amount field is `imm4` with special encoding (0111=7, 1xxx=8); max shift = 8.
  EXT is architecturally prohibited; hardware ignores it silently.
- `add/sub %sp, imm10` — EXT is possible syntactically (DESIGN_SPEC lists it as unsigned),
  but LLVM assigns width=10, so the formula applies.  Whether the CPU actually supports this
  is unconfirmed; treat as extendable for now.

---

## 8. PC-Relative Offset Formula (Confirmed)

From `decodePCRelSimm8Operand`:

```cpp
int64_t Sign8 = SignExtend32<8>(Val);
int64_t Target = (int64_t)Address + 2 * Sign8;
```

`Address` = address of the branch instruction itself (no +2).
With EXT:
```
target_address = branch_addr + 2 * Extended
```
where `Extended` = the full sign-extended combined displacement (units: halfwords).

This was confirmed during LLVM backend development: `applyFixup` for `fixup_s1c33_pc_rel_8`
uses `Offset = Value` (not `Value - 2`), and the `fixup_s1c33_pc_rel_21` fixup for EXT-prefixed
branches places the fixup at the EXT instruction address and adds 2 to compensate.

---

## 9. Register Decode Table

```cpp
static const unsigned GR32DecoderTable[] = {
    S1C33::R0,  S1C33::R1,  S1C33::R2,  S1C33::R3,
    S1C33::R4,  S1C33::R5,  S1C33::R6,  S1C33::R7,
    S1C33::R8,  S1C33::R9,  S1C33::R10, S1C33::R11,
    S1C33::R12, S1C33::R13, S1C33::R14, S1C33::R15,
};
```

HWEncoding == register index.  4-bit register field → 0–15 → R0–R15.

---

## 10. Validation Strategy

The DESIGN_SPEC §3 recommends three-way comparison: MAME disassembler, LLVM `llvm-objdump`,
and the new emulator's disassembler.  Expected divergences:

| Case | MAME | LLVM objdump | New emulator (correct) |
|------|------|--------------|------------------------|
| `ext 0x1800 / add %r0, 0` | shows +0x60000 (unsigned) | shows -0x20000 (wrong, sign-extended) | shows +0x60000 |
| `ext 0x1000 / cmp %r0, 0` | shows -0x40000 (signed) | shows -0x40000 (correct) | shows -0x40000 |
| `ext 0x0048 / ext 0x0000 / ld.w %r0, 0x05` | shows 0x00120005 | shows 0x00120005 | shows 0x00120005 |
| `ext imm / ld.w %rd, [%rb]` | shows displacement | does NOT show (silent) | shows +imm (unsigned) |

Use the test cases from DESIGN_SPEC §3.1 (ext signedness boundary tests) as the canonical
validation suite.

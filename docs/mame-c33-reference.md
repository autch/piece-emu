# MAME C33 Implementation Reference

This document extracts design patterns and key data from `mame/src/devices/cpu/c33/` that are
relevant to implementing the new P/ECE emulator.  MAME is GPL-2.0 — do **not** copy code
verbatim.  Use this document for architecture guidance and cross-validation of ISA details.

---

## 1. Scope: ADV Core vs STD Core

MAME's `c33dasm.cpp` opens with:

> TODO: Only C33 ADV Core is currently supported — add support for C33 STD Core, C33 PE Core, etc.

The S1C33209 (P/ECE) uses the **STD Core** (`c33std_cpu_device_base`).  Key differences:

| Feature | STD Core (S1C33000) | ADV Core |
|---------|---------------------|----------|
| DP register | No | Yes (Class 7 loads) |
| LOOP instruction | No | Yes |
| MAC/MACCLR | Optional multiplier | Standard |
| Extra special regs | — | `%lco`, `%lca`, `%lea`, `%sor`, `%ttbr`, `%dp`, `%idir`, `%dbbr`, `%usp`, `%ssp` |

For the new emulator, implement only STD Core.  The ADV Core instructions in `c33dasm.cpp`
(Class 7 DP-relative loads, loop, pushs/pops, mac.w, mac.hw, etc.) can be flagged as
undefined opcode.

---

## 2. Device Architecture Patterns

`c33std.cpp` and `s1c33209.cpp` illustrate the MAME device patterns recommended by
`PIECE_EMULATOR_DESIGN.md §6.2`.  Clean-room equivalents for the new emulator:

### 2.1 Memory space declaration (address_space_config)

MAME declares the memory space as a single struct:
```cpp
// from c33std.cpp
m_memory_config("memory", ENDIANNESS_LITTLE, 16, 28, 0, internal_map)
//                                             ^^  ^^
//                              data bus width=16  address bits=28
```

**New emulator equivalent**: declare a bus descriptor struct per area:
```cpp
struct BusArea { uint32_t base; uint32_t size; int data_bits; bool big_endian; };
```
The key constants are: data bus = **16-bit**, address space = **28-bit**, **little-endian**.

### 2.2 Fetch path vs I/O path separation (cache / specific)

```cpp
// from c33std.cpp device_start():
space(AS_PROGRAM).cache(m_opcodes);    // fast path for instruction fetch
space(AS_PROGRAM).specific(m_data);   // full dispatch for data access
```

**New emulator equivalent**: keep two separate access paths:
- `fetch16(addr)` — hits only internal RAM and Flash; no side effects, no I/O
- `read8/16/32(addr)` / `write8/16/32(addr)` — full BCU dispatch including I/O handlers

This is the most important structural pattern: never let instruction fetch trigger I/O side effects.

### 2.3 Per-register I/O handler binding

MAME registers read/write lambdas per I/O address range.  **New emulator equivalent**: a
handler table indexed by (address - IO_BASE) / alignment, each entry holding a pair of
function pointers `{read, write}`.  Unregistered ranges return open-bus value.

### 2.4 Device lifecycle: start / reset

```cpp
void c33std_cpu_device_base::device_start() {
    // Allocate resources, register state (save_item), expose registers (state_add)
    std::fill(m_gprs, m_gprs + 16, 0);
    m_pc = m_psr = m_sp = m_alr = m_ahr = 0;
    save_item(NAME(m_gprs)); save_item(NAME(m_pc)); ...
    state_add(C33_R0, "R0", m_gprs[0]); ...
}

void c33std_cpu_device_base::device_reset() {
    m_psr = 0;
    // s1c33209 overrides to also set PC from boot vector
}

void s1c33209_device::device_reset() {
    c33std_cpu_device_base::device_reset();
    u32 boot_vector = m_data.read_dword(0x0C0'0000);  // read reset vector from Flash
    m_pc = boot_vector;
}
```

**New emulator equivalent**: separate `init()` (constructor, memory allocation) from `reset()`
(architectural state → reset values).  On reset, read the boot vector from address 0xC00000
and set PC to it.

### 2.5 Debugger register exposure (state_add)

```cpp
state_add(C33_R0,  "R0",  m_gprs[0]);
...
state_add(C33_PSR, "PSR", m_psr).mask(0x0000'0fdf);
state_add(C33_SP,  "SP",  m_sp);
state_add(C33_ALR, "ALR", m_alr);
state_add(C33_AHR, "AHR", m_ahr);
```

**New emulator equivalent**: the GDB RSP register packet must expose registers in this order:
R0–R15 (16 words), then PC, SP, PSR, ALR, AHR.  PSR mask `0x0000_0fdf` = bits used by STD Core.

---

## 3. Memory Map (from s1c33209.cpp)

```
0x000'0000 – 0x000'1FFF  Internal RAM 8 KB (mirrored at 0x002000, 32-bit wide, 1 cycle)
0x003'0000 – 0x003'FFFF  Peripheral registers (alias of 0x040000)
0x004'0000 – 0x004'FFFF  Peripheral registers (mirrored at +0x010000)
0x006'0000 – 0x007'FFFF  [reserved for debug mode — use for semihosting port]
0x010'0000 – 0x02F'FFFF  Area 4-5: external memory (P/ECE: SRAM 256 KB at 0x100000)
0x030'0000 – 0x037'FFFF  Area 6: external 8-bit I/O (P/ECE: USB controller)
0x0C0'0000 – ...         Area 10: external memory (P/ECE: Flash 512 KB–2 MB)
```

Peripheral registers are at base `0x040000`.  MAME's `peripheral_map` offsets are relative to
that base (e.g., `map(0x0140, ...)` means absolute address `0x040140`).

Boot vector: read a 32-bit little-endian word at `0x0C0'0000`.  That word is the entry address.

---

## 4. PSR Bit Definitions (from c33helpers.ipp)

```
Bit  0  N   Negative flag
Bit  1  Z   Zero flag
Bit  2  V   Overflow flag
Bit  3  C   Carry flag
Bit  4  IE  Interrupt Enable
Bit  6  DS  Delay Slot active
Bit  7  MO  MAC Overflow
Bits 8–11 IL[3:0] Interrupt Level
Bit 12  SV  (ADV Core only)
Bit 13  ME  (ADV Core only)
Bit 14  DE  (ADV Core only)
Bit 15  S   (ADV Core only)
...
```

The STD Core mask used by MAME is `0x0000_0FDF` (bits 0–3, 4, 6–11).  The `DS` flag (bit 6)
is set by hardware when the CPU is executing the delay slot after a `.d` branch.  Track this
in the emulator's CPU state to correctly handle interrupt masking during the slot.

---

## 5. Instruction Class Dispatch

All 16-bit instructions are dispatched by `bits[15:13]`:

```
000  Class 0 — control flow: nop/slp/halt, pushn/popn, call/ret/jp, branches (jr**), brk, reti
001  Class 1 — register–register: ld.*/st.* [%rb], add/sub/cmp/and/or/xor/not, shifts/rotates, EXT
010  Class 2 — SP-relative load: ld.b/ub/h/uh/w [%sp+imm6], st.* [%sp+imm6]
011  Class 3 — immediate ALU: add/sub/cmp/ld.w/and/or/xor/not %rd, imm6
100  Class 4 — shifts/rotates (reg and imm4), scan0/scan1/swap/mirror, div step (div0s–div3s)
101  Class 5 — misc: special-reg ld, btst/bclr/bset/bnot [%rb], ld.b/ub/h/uh %rd,%rs, mlt/mac, adc/sbc
110  Class 6 — EXT prefix (bits[12:0] = imm13)
111  Class 7 — ADV Core only: DP-relative loads (STD Core: undefined opcode)
```

### Class 6 (EXT) encoding

```
bits[15:13] = 110
bits[12:0]  = imm13  (the extension value)
```

Up to two sequential EXT instructions before a target instruction.  The emulator must
accumulate pending ext values and apply them at decode time.

### MAME's EXT handling (look-ahead strategy)

MAME `c33_disassembler::disassemble()` reads ahead:

```cpp
op = opcodes.r16(pc);
if (BIT(op, 13, 3) == 6) {           // is it EXT?
    ext.type = IMM13;
    ext.val = BIT(op, 0, 13);        // first ext13
    op = opcodes.r16(pc + 2);
    if (BIT(op, 13, 3) == 6) {       // second EXT?
        ext.type = IMM26;
        ext.val = (ext.val << 13) | BIT(op, 0, 13);
        op = opcodes.r16(pc + 4);
    }
}
// then decode `op` with `ext` as context
```

**New emulator equivalent**: for a disassembler, lookahead is fine.  For a CPU executor, maintain
a `pending_ext` state variable (0, 1, or 2 accumulated ext13 values), advance PC, then apply
when the non-EXT instruction arrives.  The combined value formula:

```
1 ext:  combined = (ext0 << field_width) | raw_field
2 exts: combined = (ext0 << (13 + field_width)) | (ext1 << field_width) | raw_field
```

Signedness of `combined` depends on the target instruction (see DESIGN_SPEC §2.3 / errata §5.3).

---

## 6. Opcode Cross-Validation Table (MAME vs DESIGN_SPEC Errata)

The following op1 values from MAME's decode tables agree with the corrections in
`PIECE_EMULATOR_DESIGN.md §5.3`.  Use these as ground truth for the emulator decode table.

### Class 4, op2=10 (register–register)

| Instruction | op1 (bits[4:2]) | MAME table index |
|-------------|-----------------|------------------|
| `scan0 %rd, %rs` | 010 | 10 |
| `scan1 %rd, %rs` | 011 | 14 |
| `swap %rd, %rs`  | 100 | 18 |
| `mirror %rd, %rs`| 101 | 22 |
| `swaph %rd, %rs` | 110 | 26 |
| `sat.b %rd, %rs` | 111 | 30 |

### Class 4, op2=11 (division step)

| Instruction | op1 (bits[4:2]) | MAME table index |
|-------------|-----------------|------------------|
| `div0s %rs`  | 010 | 11 |
| `div0u %rs`  | 011 | 15 |
| `div1  %rs`  | 100 | 19 |
| `div2s %rs`  | 101 | 23 |
| `div3s`      | 110 | 27 |

### Class 5, op2=01 (register–register byte/halfword load)

| Instruction | op1 (bits[4:2]) | MAME table index |
|-------------|-----------------|------------------|
| `ld.b  %rd, %rs` | 000 | 1  |
| `ld.ub %rd, %rs` | 001 | 5  |
| `ld.h  %rd, %rs` | 010 | 9  |
| `ld.uh %rd, %rs` | 011 | 13 |

### Class 5, op2=00 (special / bit operations)

| op1 (bits[4:2]) | Instruction |
|-----------------|-------------|
| 000 | `ld.w %special, %rs` |
| 001 | `ld.w %rd, %special` |
| 010 | `btst [%rb], imm3` |
| 011 | `bclr [%rb], imm3` |
| 100 | `bset [%rb], imm3` |
| 101 | `bnot [%rb], imm3` |
| 110 | `adc %rd, %rs` |
| 111 | `sbc %rd, %rs` |

### Special register numbers (Class 5 op2=00, op1=000/001)

| Register | Number (bits[3:0] of rd/rs field) |
|----------|------------------------------------|
| PSR      | 0 |
| SP       | 1 |
| ALR      | 2 |
| AHR      | 3 |

### PC-relative branch offset (confirmed)

```
target = PC_of_branch_instruction + 2 * sign_extend_8(imm8)
```

No +2 on the PC before scaling.  The MAME disassembler and LLVM backend both use this formula.

---

## 7. What MAME Does NOT Implement (Executor Stub)

`c33std_cpu_device_base::execute_run()` is a stub:

```cpp
void c33std_cpu_device_base::execute_run() {
    debugger_instruction_hook(m_pc);
    m_icount = 0;   // consume all cycles without executing anything
}
```

No instruction execution is implemented.  The new emulator must implement this from scratch
using the CPU manual and the DESIGN_SPEC as primary sources.

---

## 8. Items MAME Does Not Cover for P/ECE

The following are absent from the MAME C33 skeleton and must be sourced from the CPU manual
and S1C33209 technical manual:

- BCU wait-cycle calculation (DESIGN_SPEC §1.4)
- 16-bit timer and 8-bit timer register layout / interrupt behaviour
- DMA channel registers and transfer sequencing
- LCD/SPI interface (SPI is on the serial interface peripheral)
- P07 clock control and 24/48 MHz switching (DESIGN_SPEC §1.6)
- Interrupt priority and masking registers
- IDMA boot sequence (Flash → SRAM at reset)

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository Overview

This repository contains four interconnected sub-projects for the **Aquaplus P/ECE** handheld game console (EPSON S1C33209 SoC, S1C33000 32-bit RISC CPU core):

1. **`piece-toolchain-llvm/`** — LLVM/Clang backend for the S1C33000 CPU. Produces binaries ABI-compatible with the P/ECE SDK and kernel. **Status: Phase 6 complete, verified on real hardware (2026-03).**
2. **`mame/`** — MAME fork containing an S1C33 CPU disassembler and S1C33209 device definition (used as reference and for future emulator integration).
3. **`piemu/`** — Legacy C/SDL2 full-system emulator (circa 2003–2005). Simulates CPU, memory, LCD, audio, flash. Loads `piece.pfi` (P/ECE Flash Image) to run P/ECE games. Useful as a reference implementation of the S1C33 instruction set and peripheral behaviour.
4. **`src/`** — New emulator (implemented). C++20 bare-metal ELF loader, S1C33 CPU interpreter, GDB RSP stub, semihosting. See `PIECE_EMULATOR_DESIGN.md` for design spec.

## Key Documents

- **`docs/s1c33000_quick_reference.md`** — **Check here first** for S1C33000 instruction set, encoding, addressing modes, and register info. Prefer this over the original PDF: it saves context and is a verified, machine-readable document.
- **`PIECE_EMULATOR_DESIGN.md`** — Emulator design spec (Japanese). CPU timing, peripheral simulation, GDB RSP interface, semihosting ports, test infrastructure, headless mode JSON output.
- **`piece-toolchain-llvm/DESIGN_SPEC.md`** — LLVM backend architecture, ABI, calling convention, instruction encoding details, implementation phasing.
- **`piece-toolchain-llvm/CLAUDE.md`** — **Read this before working on the LLVM backend.** Contains critical facts about ext immediate signedness, ABI, common pitfalls, and coding conventions.
- **`piece-toolchain-llvm/docs/errata.md`** — CPU hardware bugs, compiler bugs, known C library bugs.

## Build Commands (piece-toolchain-llvm)

All commands run from the **repository root**:

```bash
make              # Build LLVM/clang/lld + regenerate sysroot
make llvm         # Build only LLVM/clang (skip sysroot)
make sysroot      # Regenerate sysroot only
make tests        # Run S1C33 lit tests (check-llvm-codegen-s1c33)
make clean        # Clean build artifacts
```

**Do not use `ninja llc` alone** — clang-22 links against `libLLVMS1C33CodeGen.a` separately and will silently use the old codegen until `clang` is also rebuilt. Always use `make`.

## Architecture Overview

### LLVM Backend (`piece-toolchain-llvm/`)

The compiler pipeline for a P/ECE application:

```
.c → clang (S1C33 backend) → .o (ELF)
SDK .lib → srf2elf → .a (ELF)
.o + .a → ld.lld + piece.ld → .elf → ppack → .pex
```

Key directories:
- `llvm/llvm/lib/Target/S1C33/` — Backend implementation (TableGen, ISel, MC layer)
- `llvm/compiler-rt/lib/builtins/s1c33/` — Float/integer runtime (replaces fp.lib/idiv.lib)
- `newlib/` — Standard C headers (replaces EPSON headers except 11 P/ECE-specific files)
- `tools/crt/` — crt0.o, libpceapi.a, libcxxrt.a
- `tools/srf2elf/` — SRF33 → ELF converter (Python)
- `tools/ppack/` — ELF → .pex packager (C, CMake)
- `tools/piece.ld` — P/ECE linker script

### MAME (`mame/src/devices/cpu/c33/`)

- `c33dasm.cpp/h` — S1C33 disassembler (authoritative reference for instruction decoding)
- `s1c33209.cpp/h` — S1C33209 SoC device definition
- `c33helpers.ipp` — Helper macros

### piemu (Legacy Emulator, `piemu/`)

C/SDL2 full-system emulator. All state lives in a single `PIEMU_CONTEXT` struct passed through the call stack.

Build:
```bash
cd piemu && mkdir build && cd build
cmake ..
make          # builds piemu + pfar + mkpfi (+ fusepfi if FUSE available)
```

Dependencies: SDL2 (required), FUSE (optional).

Run: place `piece.pfi` in the working directory, then `./piemu`.

Key components:
- `core/class{0-6}.c` — S1C33 instruction implementations (7 ISA classes)
- `bcu.c` — Bus Control Unit; routes all memory/IO reads and writes
- `iomem.c` — I/O registers, keypad, audio (SDL2 audio device)
- `flash.c` — 4 MB flash with CFI protocol
- `lcdc.c` — 128×88 monochrome display (2-bit pixels, 4× SDL zoom)
- `tools/` — `mkpfi` (create PFI image), `pfar` (archive), `fusepfi` (FUSE mount)

Memory map:
```
0x0000000–0x0001FFF   FRAM  (16 KB)
0x0040000–0x004FFFF   IOMEM (I/O registers)
0x2000000–0x201FFFF   SRAM  (128 KB)
0xC000000–0xFFFFFF    Flash (4 MB, reset vector at 0xC00000)
```

`piemu/readme.txt` (Japanese) documents keyboard bindings, flash image management, and known limitations.

### New Emulator (`src/`)

**Implemented.** C++20 / CMake+Ninja, in `src/`. Build and run:
```bash
cd build-src && cmake ../src -G Ninja && ninja   # build
./build-src/piece-emu --trace <elf>              # run with disassembly trace
./build-src/piece-emu --gdb [port] <elf>         # GDB RSP mode (default port 1234)
./build-src/piece-emu --max-cycles N <elf>       # limit execution
```

Build artifacts:
- `libpiece_core.a` — 共通エミュレーションコア（CPU/BCU/メモリ/逆アセ/GDB RSP/セミホスティング）
- `piece-emu` — ベアメタル/ヘッドレス CLI フロントエンド（POSIXのみ、SDL不要）
- `piece-emu-system` — システムモードフロントエンド（将来; SDL/imgui使用、CMakeLists.txt にコメントアウトで記載）

Semihosting ports: 0x060000=CONSOLE_CHAR, 0x060002=CONSOLE_STR, 0x060008=TEST_RESULT (0=PASS).
Loading 0x060008 requires **2 EXT instructions** (bit 18 set → 19-bit sign-extend is negative).

Memory map (emulator):
```
0x000000–0x001FFF   IRAM  (8 KB, 0-wait)
0x030000–0x07FFFF   I/O + semihosting
0x100000–0x13FFFF   SRAM  (256 KB)
0xC00000+           Flash
```

Bare-metal C tests (uses LLVM S1C33 toolchain at `/home/autch/src/llvm-c33`):
```bash
cd src/tests/bare_metal && make && make run
```
Pattern: `crt0.s` sets SP via `ext 0x80` / `ld.w %r0, 0` / `ld.w %sp, %r0`, then calls C `_start_c`.

## Language Rules

- Code and comments in `llvm/`, `newlib/`, `tools/`, `mame/`, `piemu/`: **English only**.
- `docs/` and `sdk/`: Japanese allowed (reference documents).
- Do not modify any files under `sdk/` — reference material only.

## S1C33000 CPU Critical Facts

(Full details in `piece-toolchain-llvm/CLAUDE.md` and `DESIGN_SPEC.md`)

- 16-bit fixed-length instructions, 32-bit registers (R0–R15), 28-bit address space, little-endian
- `ext` prefix extends immediates; **ext does NOT change signedness** of the target instruction
- Class 1 memory `ext` displacement is **unsigned** — negative offsets are impossible and silently corrupt
- ABI: args R12→R15, return R10, callee-saved R0–R3, scratch R4–R7/R9, **R8 reserved** (kernel table base = 0x0)
- `jp.d %rb` is **forbidden** (hardware bug); use `jp %rb` instead
- No hardware divider; step-division sequence costs 35 instructions
- Delayed branch slots: strict constraints (1-cycle, no memory, no ext, no branch)
- SP-relative load/store offsets are **scaled** (word=×4, halfword=×2, byte=×1), not bytes

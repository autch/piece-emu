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

### New Emulator — Peripheral & Kernel References

When working on `src/` peripherals or kernel boot behaviour, consult these **before** reading raw source files:

- **`docs/peripheral-implementation-status.md`** — Authoritative status of all implemented peripherals (P1 phase). Covers register maps, known design pitfalls (`bus.write16` byte order, ISR clear semantics, T8 PSET timing), and the next-step roadmap (P2: SIF3, HSDMA, LCD frontend, RTC).
- **`docs/kernel-source-reference.md`** — Distilled guide to `sdk/sysdev/pcekn/`. Contains: boot sequence, key register values set by InitHard/InitTimer/GetSysClock, ISR clear method, button bit layout, LCD/SIF3/HSDMA access patterns, and "traps for the emulator" (e.g. GetSysClock RTC hang, D-button wait). **Read this instead of the raw kernel .c files** whenever possible — the kernel sources are large and context-expensive.
- **`docs/piece-cd/`** — P/ECE SDK HTML documentation (hardware interrupt/port assignment tables, button wiring). Useful for verifying interrupt vector numbers and GPIO signal names. Key files:
  - `PIECE ハードウエア割り込み.html` — Full interrupt vector table with BIOS usage
  - `PIECE ポート解説.html` — K/P port physical wiring (K5/K6 button mapping)

`sdk/sysdev/pcekn/` (read-only) is the actual kernel source. Read individual `.c` files only when `docs/kernel-source-reference.md` does not cover the detail needed.

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

**Implemented.** C++20 / CMake+Ninja, in `src/`. Dependencies managed via vcpkg (`src/vcpkg.json`).

Build and run:
```bash
# First build (vcpkg installs gtest automatically):
cmake -S src -B build-src -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/home/autch/src/vcpkg/scripts/buildsystems/vcpkg.cmake
ninja -C build-src

./build-src/piece-emu --trace <elf>                        # run with disassembly trace
./build-src/piece-emu --gdb [port] <elf>                   # GDB RSP mode (default port 1234)
./build-src/piece-emu --max-cycles N <elf>                 # limit execution
./build-src/piece-emu --wp-write 0xADDR[:SIZE] <elf>       # SRAM write watchpoint
./build-src/piece-emu --wp-read / --wp-rw <elf>            # read / read-write watchpoint
./build-src/piece-emu --break-at 0xADDR <elf>              # dump regs when PC == ADDR

./build-src/piece-emu-system --pfi images/old/piece.pfi    # full-system SDL3 run
./build-src/piece-emu-system --pfi ... --gdb-port 1234     # with async GDB RSP (LLDB-compatible)
./build-src/piece-emu-system --pfi ... --gdb-debug         # trace RSP packet traffic

ninja -C build-src test                                    # run all unit tests (layers 1+2)
```

Build artifacts (static libraries, layered):
- `libpiece_core.a` — S1C33000 CPU, BCU, disassembler (`src/core/`)
- `libpiece_soc.a`  — S1C33209 on-chip peripherals: INTC, ClkCtl, T8/T16, PortCtrl, BcuArea, WDT, RTC (`src/soc/`)
- `libpiece_debug.a` — ELF loader, semihosting, GDB RSP stub (`src/debug/`)
- `libpiece_board.a` — board external devices: S6B0741 LCD controller (`src/board/`); links `piece_soc` publicly
- `libpiece_host.a` — SDL3 LCD renderer and event polling (`src/host/`); stub INTERFACE when SDL3 absent
- `piece-emu` — headless CLI frontend (POSIX only, no SDL)
- `piece-emu-system` — SDL3 full-system frontend (built when SDL3 found via vcpkg)

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

- Code, comments in top-level, `src/` and commit messages: **English only**.
- `docs/`: Japanese allowed (reference documents).
- Do not modify any files under `piece-toolchain-llvm/`, `mame/`, `piemu/` — reference material only.
- Do not modify any files under `sdk/` — shared with other projects, treat as read-only reference. Kernel source is at `sdk/sysdev/pcekn/`.

## IO Handler Byte-Write Semantics (Critical for Peripheral Implementation)

The bus calls 16-bit IO write handlers with the **original address** (possibly odd for byte stores).
Handlers must check `addr & 1` to distinguish byte writes to high (odd) vs low (even) bytes:

```cpp
[](uint32_t addr, uint16_t v) {
    if (addr & 1)
        reg_hi = static_cast<uint8_t>(v);        // byte store to odd address → high byte only
    else {
        reg_lo = static_cast<uint8_t>(v);        // halfword or byte store to even address
        reg_hi = static_cast<uint8_t>(v >> 8);
    }
}
```

The kernel sets individual port-data bits with byte stores (e.g. `P21D |= 0x02` → `bp[0x2D9] |= 0x02`).
Ignoring `addr & 1` silently corrupts the adjacent byte of every register pair.

## GDB RSP Modes

- `piece-emu --gdb [port] <elf>` — **sync mode**: RSP server blocks and steps CPU directly in one thread
- `piece-emu-system --gdb-port N --pfi ...` — **async mode**: RSP server in `piece-gdb` background thread; CPU runs in dedicated `piece-cpu` thread and polls `GdbRsp::take_async_run_cmd()` / calls `notify_async_stopped()`; SDL window stays live during continue runs
- Both GDB and LLDB (MCP) clients work with either mode

## Threading Model (piece-emu-system)

`piece-emu-system` uses three threads:
- **`piece-cpu`** (`std::thread`): `CpuRunner::run()` — CPU step loop, peripherals, timers
- **`piece-sdl`** (main thread): SDL3 event polling + `LcdRenderer::render()` — SDL rendering **must** stay on the main thread
- **`piece-gdb`** (optional): GDB RSP async server

Shared state: `LcdFrameBuf` (mutex-protected pixel snapshot; CPU calls `push()` on HSDMA Ch0 completion, main thread calls `take()` at ~60 Hz; latest frame wins on drop), `std::atomic<bool> quit_flag`, `std::atomic<uint16_t> shared_buttons`.

## CPU Loop Performance Design

Key constants in `system_main.cpp`: `EVENT_INTERVAL = 10'000` cycles (SDL poll interval / max timer wake delay), `MIN_TICK_BURST = 2'000` cycles (minimum cycles before `do_tick()`).
Fast-path inner loop activates when: no `--trace`, no break addresses, no `--max-cycles`, no watchpoints.
`Bus::read16/fetch16` bypass `classify()` with inline fast paths: SRAM (`addr - SRAM_BASE < SRAM_WINDOW`) then Flash/open-bus (`addr >= FLASH_BASE`).
`Cpu::h_mac` has O(1) open-bus fast path for pdwait (pointers at 0x1000000 always return 0xFFFF).
`Timer16bit::tick()` O(1): `N = (tc+counts)/cra`, `final_tc = (tc+counts)%cra`; ISR is level-triggered (raise once == N times).

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

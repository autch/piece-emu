// insn.hpp — Portable S1C33 instruction word decoder.
//
// Wraps a uint16_t and exposes named field accessors using explicit bit-mask
// operations.  No compiler-dependent bitfield layout (cf. piemu CLASS_* structs).
// All methods are constexpr: zero-overhead at runtime, compile-time-evaluable
// when the instruction word is a constant.
#pragma once
#include <cstdint>

struct Insn {
    uint16_t raw;
    constexpr explicit Insn(uint16_t v) : raw(v) {}

    // ---- Opcode fields ------------------------------------------------
    // Common to classes 1–6:
    constexpr int cls()  const { return (raw >> 13) & 7; }   // bits [15:13]
    constexpr int op1()  const { return (raw >> 10) & 7; }   // bits [12:10]
    constexpr int op2()  const { return (raw >>  8) & 3; }   // bits [9:8]

    // Class 0 uses a 4-bit op1 field and a different op2 position:
    constexpr int c0_op1() const { return (raw >>  9) & 0xF; } // bits [12:9]
    constexpr int c0_op2() const { return (raw >>  6) & 3; }   // bits [7:6]

    // ---- Register fields ----------------------------------------------
    constexpr int rd() const { return raw & 0xF; }          // bits [3:0]
    constexpr int rs() const { return (raw >> 4) & 0xF; }   // bits [7:4]
    constexpr int rb() const { return (raw >> 4) & 0xF; }   // bits [7:4], load/store alias

    // ---- Immediate / flag fields --------------------------------------
    constexpr bool     d()      const { return (raw >> 8) & 1; }      // bit  [8]   — delay slot
    constexpr int      imm2()   const { return raw & 3; }             // bits [1:0] — int vec
    constexpr int      imm3()   const { return raw & 7; }             // bits [2:0] — bit index (class 5B)
    constexpr uint32_t imm6()   const { return (raw >> 4) & 0x3F; }  // bits [9:4] — classes 2, 3
    constexpr uint32_t imm10()  const { return raw & 0x3FF; }         // bits [9:0] — class 4A SP-adjust
    constexpr uint32_t imm13()  const { return raw & 0x1FFF; }        // bits [12:0] — EXT prefix
    constexpr int      sign8()  const { return raw & 0xFF; }          // bits [7:0] — branch disp (raw)

    // ---- Class 4B shift amount ----------------------------------------
    // Encoding: bits [7:4].  0xxx → shift amount 0..7.  1xxx → 8 (max).
    constexpr int shift_amt() const {
        int v = (raw >> 4) & 0xF;
        return (v & 8) ? 8 : v;
    }
};

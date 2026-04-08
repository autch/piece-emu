// cpu_impl.hpp — Internal utilities shared across cpu_class*.cpp and cpu_dispatch.cpp.
// Do NOT include from public headers.
#pragma once
#include "cpu.hpp"
#include <bit>
#include <cstdio>

// ============================================================================
// Shared utility functions (static inline — safe for multiple TU inclusion)
// ============================================================================

static constexpr uint32_t sign_ext(uint32_t v, int bits) {
    if (bits >= 32) return v;
    uint32_t sign = 1u << (bits - 1);
    return (v ^ sign) - sign;
}

// Shift-amount encoding for Class 4B imm4 shift fields:
// imm4_rs is in bits [7:4]; 0..7 = 0..7, 1xxx = 8 (max shift is 8).
static inline int decode_shift_imm4(uint16_t insn) {
    int imm4 = (insn >> 4) & 0xF;  // bits [7:4] = imm4_rs field
    if (imm4 & 8) return 8;
    return imm4;
}

// ============================================================================
// Instruction field extractors (static inline, all classes)
// ============================================================================

// CLASS_1A (loads/stores [%rb]): rb=bits[7:4], rs_rd=bits[3:0]
// CLASS_1B (ALU reg-reg):        rd=bits[3:0], rs=bits[7:4]
// CLASS_2 (SP-relative):         rs_rd=bits[3:0], imm6=bits[9:4]
// CLASS_3 (imm ALU):             rd=bits[3:0], imm6=bits[9:4]
// CLASS_4B/4C (shifts):          rd=bits[3:0], imm4_rs=bits[7:4]
// CLASS_5A/5B/5C:                rd/sd=bits[3:0], rs/ss/rb=bits[7:4]

static inline int cls(uint16_t i) { return (i >> 13) & 7; }
static inline int op1(uint16_t i) { return (i >> 10) & 7; } // bits[12:10]
static inline int op2(uint16_t i) { return (i >>  8) & 3; } // bits[9:8]

// Generic rd/rs (used by most classes)
static inline int rd(uint16_t i) { return i & 0xF; }        // bits[3:0]
static inline int rs(uint16_t i) { return (i >> 4) & 0xF; } // bits[7:4]
static inline int rb(uint16_t i) { return (i >> 4) & 0xF; } // alias for rs

// CLASS_1A helpers
static inline int c1a_rb  (uint16_t i) { return (i >> 4) & 0xF; }
static inline int c1a_rsrd(uint16_t i) { return i & 0xF; }

// CLASS_1B helpers
static inline int c1b_rd(uint16_t i) { return i & 0xF; }
static inline int c1b_rs(uint16_t i) { return (i >> 4) & 0xF; }

// cpu_impl.hpp — Internal utilities shared across cpu_class*.cpp and cpu_dispatch.cpp.
// Do NOT include from public headers.
#pragma once
#include "cpu.hpp"
#include "insn.hpp"
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

// ============================================================================
// ALU operation core — shared by Class 1C (reg-reg) and Class 3 (imm6)
//
// do_alu<Op>(cpu, a, b, rd)
//   Performs operation Op on operands a and b, writes result to r[rd].
//   CMP does not write; MOV and NOT only use b (a is ignored).
// ============================================================================

enum class AluOp { ADD, SUB, CMP, MOV, AND, OR, XOR, NOT };

template<AluOp Op>
inline void do_alu(Cpu& cpu, uint32_t a, uint32_t b, int rd) {
    if constexpr (Op == AluOp::ADD) {
        uint64_t r = uint64_t(a) + b;
        cpu.state.r[rd] = uint32_t(r);
        cpu.state.psr.set_nzvc_add(a, b, r);
    } else if constexpr (Op == AluOp::SUB) {
        uint64_t r = uint64_t(a) - b;
        cpu.state.r[rd] = uint32_t(r);
        cpu.state.psr.set_nzvc_sub(a, b, r);
    } else if constexpr (Op == AluOp::CMP) {
        uint64_t r = uint64_t(a) - b;
        cpu.state.psr.set_nzvc_sub(a, b, r);
    } else if constexpr (Op == AluOp::MOV) {
        cpu.state.r[rd] = b;
    } else if constexpr (Op == AluOp::AND) {
        uint32_t r = a & b;
        cpu.state.r[rd] = r; cpu.state.psr.set_nz(r);
    } else if constexpr (Op == AluOp::OR) {
        uint32_t r = a | b;
        cpu.state.r[rd] = r; cpu.state.psr.set_nz(r);
    } else if constexpr (Op == AluOp::XOR) {
        uint32_t r = a ^ b;
        cpu.state.r[rd] = r; cpu.state.psr.set_nz(r);
    } else { // NOT
        uint32_t r = ~b;
        cpu.state.r[rd] = r; cpu.state.psr.set_nz(r);
    }
}

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

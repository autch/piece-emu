#include "cpu_impl.hpp"

// ============================================================================
// Handlers — Class 6: EXT (immediate extension prefix)
// ============================================================================

void Cpu::h_ext(Cpu& cpu, uint16_t insn) {
    uint32_t imm13 = insn & 0x1FFF;
    if (cpu.state.pending_ext_count < 2)
        cpu.state.pending_ext[cpu.state.pending_ext_count++] = imm13;
    // PC already advanced by step(); do NOT flush ext here
}

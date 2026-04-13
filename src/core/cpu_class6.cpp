#include "cpu_impl.hpp"
#include <format>

// ============================================================================
// Handlers — Class 6: EXT (immediate extension prefix)
// ============================================================================

void Cpu::h_ext(Cpu& cpu, uint16_t insn) {
    uint32_t imm13 = Insn{insn}.imm13();
    if (cpu.state.pending_ext_count >= 2) {
        // 3rd+ consecutive EXT: hardware uses the 1st and the latest, discarding all middle ones.
        uint32_t fault_pc = cpu.state.pc - 2;
        cpu.diag_warn_or_fault("ext_triple", fault_pc,
            std::format("excess consecutive ext at 0x{:06X}: hardware uses 1st and latest, discards middle", fault_pc));
        // Replace the 2nd slot with this latest value, mirroring hardware behaviour.
        cpu.state.pending_ext[1] = imm13;
        return;
    }
    cpu.state.pending_ext[cpu.state.pending_ext_count++] = imm13;
    // PC already advanced by step(); do NOT flush ext here
}

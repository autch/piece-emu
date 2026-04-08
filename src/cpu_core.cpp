#include "cpu_impl.hpp"
#include "bus.hpp"
#include <format>
#include <cstdio>

// ============================================================================
// EXT helpers
// ============================================================================

uint32_t Cpu::ext_imm(uint32_t imm, int width) const {
    uint32_t raw = imm & ((1u << width) - 1);
    if (state.pending_ext_count == 0) return raw;
    if (state.pending_ext_count == 1)
        return (state.pending_ext[0] << width) | raw;
    return (state.pending_ext[0] << (13 + width)) | (state.pending_ext[1] << width) | raw;
}

int32_t Cpu::ext_simm(uint32_t imm, int width) const {
    uint32_t raw = imm & ((1u << width) - 1);
    if (state.pending_ext_count == 0)
        return static_cast<int32_t>(sign_ext(raw, width));
    if (state.pending_ext_count == 1) {
        uint32_t combined = (state.pending_ext[0] << width) | raw;
        return static_cast<int32_t>(sign_ext(combined, 13 + width));
    }
    uint64_t combined = (uint64_t(state.pending_ext[0]) << (13 + width))
                      | (uint64_t(state.pending_ext[1]) << width) | raw;
    int total_bits = 26 + width;
    if (total_bits >= 32) return static_cast<int32_t>(combined);
    uint64_t sign = uint64_t(1) << (total_bits - 1);
    return static_cast<int32_t>((combined ^ sign) - sign);
}

uint32_t Cpu::ext_rb() const {
    if (state.pending_ext_count == 0) return 0;
    if (state.pending_ext_count == 1) return state.pending_ext[0];
    return (state.pending_ext[0] << 13) | state.pending_ext[1];
}

int32_t Cpu::ext_pcrel(uint32_t imm8) const {
    uint32_t raw = imm8 & 0xFF;
    if (state.pending_ext_count == 0)
        return static_cast<int32_t>(sign_ext(raw, 8));
    if (state.pending_ext_count == 1) {
        uint32_t combined = (state.pending_ext[0] << 8) | raw;
        return static_cast<int32_t>(sign_ext(combined, 21));
    }
    uint64_t combined = (uint64_t(state.pending_ext[0]) << 21)
                      | (uint64_t(state.pending_ext[1]) << 8) | raw;
    uint64_t sign = uint64_t(1) << 33;
    int64_t extended = static_cast<int64_t>((combined ^ sign) - sign);
    return static_cast<int32_t>(extended);
}

void Cpu::flush_ext() {
    state.pending_ext_count = 0;
}

void Cpu::h_undef(Cpu& cpu, uint16_t insn) {
    // The S1C33 CPU has no hardware undefined-instruction trap.
    // Dump a diagnostic oops and halt the emulator.
    const CpuState& s = cpu.state;
    uint32_t fault_pc = s.pc - 2; // step() already advanced PC past this insn

    std::fprintf(stderr, "\n--- Undefined instruction ---\n");

    // Show any pending EXT prefixes followed by the faulting instruction.
    // Each EXT was 2 bytes, so they precede fault_pc.
    int nexts = s.pending_ext_count;
    if (nexts >= 2)
        std::fprintf(stderr, "  %s\n", cpu.disasm(fault_pc - 4).c_str());
    if (nexts >= 1)
        std::fprintf(stderr, "  %s\n", cpu.disasm(fault_pc - 2).c_str());
    std::fprintf(stderr, "  0x%06X: %04X  <undef>\n", fault_pc, insn);

    // Register dump
    std::fprintf(stderr, "\nRegisters:\n");
    for (int i = 0; i < 16; i += 4) {
        std::fprintf(stderr, "  R%2d=%08X  R%2d=%08X  R%2d=%08X  R%2d=%08X\n",
                     i,   s.r[i],
                     i+1, s.r[i+1],
                     i+2, s.r[i+2],
                     i+3, s.r[i+3]);
    }
    std::fprintf(stderr,
        "   PC=%08X   SP=%08X  PSR=%08X\n"
        "  ALR=%08X  AHR=%08X\n"
        "---\n\n",
        fault_pc, s.sp, s.psr.raw,
        s.alr, s.ahr);

    cpu.state.fault   = true;
    cpu.state.in_halt = true;
}

// ============================================================================
// Trap
// ============================================================================

static constexpr uint32_t TRAP_TABLE_BASE = 0x400;

void Cpu::assert_trap(int no, int level) {
    do_trap(no, level);
}

void Cpu::do_trap(int no, int level) {
    if (no >= 16) {
        if (!state.psr.ie()) return;
        if (static_cast<uint32_t>(level) <= state.psr.il()) return;
    }
    uint32_t vec = bus_.read32(TRAP_TABLE_BASE + no * 4);
    state.sp -= 4;
    bus_.write32(state.sp, state.pc);
    state.sp -= 4;
    bus_.write32(state.sp, state.psr.raw);
    state.pc = vec;
    state.psr.set_ie(false);
    if (no >= 16)
        state.psr.set_il(static_cast<uint32_t>(level));
    state.in_halt = false;
}

// ============================================================================
// Branch helpers
// ============================================================================

void Cpu::do_branch(uint32_t target, bool delayed) {
    if (!delayed) {
        state.pc = target;
    } else {
        state.in_delay_slot = true;
        state.delay_target  = target;
    }
}

void Cpu::do_call(uint32_t target, bool delayed) {
    (void)delayed;
    do_branch(target, delayed);
}

// ============================================================================
// Cpu ctor / reset
// ============================================================================

Cpu::Cpu(Bus& bus) : bus_(bus) {}

void Cpu::reset() {
    state = CpuState{};
    state.pc = bus_.read32(Bus::FLASH_BASE);
}

// ============================================================================
// Instruction step
// ============================================================================

int Cpu::step() {
    if (state.in_halt) return 1;

    uint32_t insn_pc = state.pc;
    uint16_t insn    = bus_.fetch16(insn_pc);

    bool was_delay_slot = state.in_delay_slot;
    uint32_t delay_target = state.delay_target;

    state.pc = insn_pc + 2;

    dispatch_[insn](*this, insn);

    if (was_delay_slot) {
        state.in_delay_slot = false;
        state.pc = delay_target;
    }

    return 1;
}

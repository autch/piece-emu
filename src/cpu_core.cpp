#include "cpu_impl.hpp"
#include "bus.hpp"
#include <format>
#include <cstdio>
#include <string>

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
    // Report via DiagSink and halt the emulator.
    uint32_t fault_pc = cpu.state.pc - 2; // step() already advanced PC past this insn

    // Build detail: show any pending EXT prefixes then the faulting instruction.
    std::string detail;
    int nexts = cpu.state.pending_ext_count;
    if (nexts >= 2)
        detail += std::format("  {}\n", cpu.disasm(fault_pc - 4));
    if (nexts >= 1)
        detail += std::format("  {}\n", cpu.disasm(fault_pc - 2));
    detail += std::format("  0x{:06X}: {:04X}  <undef>", fault_pc, insn);

    cpu.diag_fault("undef", fault_pc, std::move(detail));
}

// ============================================================================
// Trap
// ============================================================================

void Cpu::assert_trap(int no, int level) {
    do_trap(no, level);
}

void Cpu::do_trap(int no, int level) {
    if (no >= 16) {
        if (!state.psr.ie()) return;
        if (static_cast<uint32_t>(level) <= state.psr.il()) return;
    }
    uint32_t vec = bus_.read32(state.ttbr + no * 4);
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
// Diagnostic helpers
// ============================================================================

// Format a register dump from the current CpuState, for use as context in a
// DiagEvent.  fault_pc is the address of the faulting instruction.
static std::string format_reg_dump(const CpuState& s, uint32_t fault_pc) {
    std::string out;
    out += "Registers:\n";
    for (int i = 0; i < 16; i += 4) {
        out += std::format("  R{:2d}={:08X}  R{:2d}={:08X}  R{:2d}={:08X}  R{:2d}={:08X}\n",
                           i,   s.r[i],
                           i+1, s.r[i+1],
                           i+2, s.r[i+2],
                           i+3, s.r[i+3]);
    }
    out += std::format("   PC={:08X}   SP={:08X}  PSR={:08X}\n"
                       "  ALR={:08X}  AHR={:08X}\n",
                       fault_pc, s.sp, s.psr.raw, s.alr, s.ahr);
    return out;
}

void Cpu::diag_warn(const char* category, uint32_t pc, std::string detail) {
    diag_->report({DiagLevel::Warning, category, pc, std::move(detail), ""});
}

void Cpu::diag_fault(const char* category, uint32_t pc, std::string detail) {
    std::string ctx = format_reg_dump(state, pc);
    diag_->report({DiagLevel::Fault, category, pc, std::move(detail), ctx});
    state.fault   = true;
    state.in_halt = true;
}

void Cpu::diag_warn_or_fault(const char* category, uint32_t pc, std::string detail) {
    if (strict_)
        diag_fault(category, pc, std::move(detail));
    else
        diag_warn(category, pc, std::move(detail));
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
// Instruction classification helpers (pure functions on the opcode)
// ============================================================================

// Returns true if this instruction can legally follow an EXT prefix.
// Instructions with EXT="-" in the manual return false.
static bool ext_capable(uint16_t insn) {
    int c = cls(insn);
    if (c == 6) return true;   // EXT before EXT: up to 2 consecutive is legal
    if (c == 7) return false;  // undefined
    if (c == 0) {
        // Branches (jr**, call sign8, jp sign8): EXT 1.  Class-0a: no EXT.
        return ((insn >> 9) & 0xF) >= 4;
    }
    if (c == 1) {
        int op2_f = (insn >> 8) & 3;
        if (op2_f == 0) return true;   // [%rb] loads/stores: EXT 1
        if (op2_f == 1) return false;  // [%rb]+ post-increment: no EXT
        if (op2_f == 2) {
            // reg-reg ALU: add/sub/cmp/and/or/xor → EXT 1; ld.w rd,rs and not → no EXT
            int op1_f = (insn >> 10) & 7;
            return op1_f != 3 && op1_f != 7;
        }
        return false;
    }
    if (c == 2) return true;   // SP-relative: EXT 2
    if (c == 3) return true;   // immediate ALU: EXT 2/3
    if (c == 4) return false;  // SP-adjust + shifts/rotates/scan/div: no EXT
    if (c == 5) {
        int op2_f = (insn >> 8) & 3;
        if (op2_f == 0) {
            // btst/bclr/bset/bnot (op1_f 2–5): EXT 1.  Special-reg moves, adc, sbc: no EXT.
            int op1_f = (insn >> 10) & 7;
            return op1_f >= 2 && op1_f <= 5;
        }
        return false;  // Class 5B (reg-reg byte/halfword), 5C (mlt/mac): no EXT
    }
    return false;
}

// Delay-slot class for the instruction that would execute in the slot.
// DC_OK   — listed as D=○ in the manual; safe to use.
// DC_HARD — always a fault: multi-cycle, memory access, or is a branch/trap/EXT.
// DC_SOFT — satisfies the three mechanical constraints (1 cycle, no memory, no EXT)
//           but is not explicitly listed as D=○; fault in strict mode, else warning.
enum DelayClass { DC_OK = 0, DC_HARD = 1, DC_SOFT = 2 };

static DelayClass delay_slot_class(uint16_t insn) {
    int c = cls(insn);
    if (c == 6) return DC_HARD;  // EXT in delay slot: D="-"
    if (c == 7) return DC_HARD;  // undefined
    if (c == 0) {
        int op1_f = (insn >> 9) & 0xF;
        if (op1_f < 4) {
            // Class 0a: nop (op2_f=0), slp (1), halt (2) pass the three mechanical
            // constraints but are not listed as D=○ → SOFT.
            // Everything else: pushn/popn (memory), brk/int/reti (trap=memory),
            // call/ret/jp (branch + possible memory) → HARD.
            int op2_f = (insn >> 6) & 3;
            if (op1_f == 0 && op2_f <= 2) return DC_SOFT;  // nop / slp / halt
        }
        return DC_HARD;  // all other class-0a + all class-0b branches
    }
    if (c == 1) {
        int op2_f = (insn >> 8) & 3;
        if (op2_f == 0) return DC_HARD;  // [%rb] memory access
        if (op2_f == 1) return DC_HARD;  // [%rb]+ memory access
        if (op2_f == 2) return DC_OK;    // reg-reg ALU: all D=○
        return DC_HARD;
    }
    if (c == 2) return DC_HARD;  // SP-relative: memory access
    if (c == 3) return DC_OK;    // immediate ALU: all D=○
    if (c == 4) {
        int op1_f = (insn >> 10) & 7;
        int op2_f = (insn >>  8) & 3;
        if (op1_f <= 1) return DC_OK;    // add/sub %sp, imm10: D=○
        if (op2_f <= 1) return DC_OK;    // imm4 and reg shifts/rotates: D=○
        if (op2_f == 2) return DC_OK;    // scan0/scan1/swap/mirror: D=○
        if (op2_f == 3) return DC_SOFT;  // div0s/div0u/div1/div2s/div3s: D="-" but 1-cycle no-mem
        return DC_HARD;
    }
    if (c == 5) {
        int op2_f = (insn >> 8) & 3;
        if (op2_f == 0) {
            int op1_f = (insn >> 10) & 7;
            switch (op1_f) {
            case 0: case 1: return DC_SOFT;  // ld.w %sd/%ss: D="-" but 1-cycle no-mem
            case 2: case 3: case 4: case 5: return DC_HARD;  // btst/bclr/bset/bnot: memory
            case 6: case 7: return DC_OK;    // adc / sbc: D=○
            default: return DC_HARD;
            }
        }
        if (op2_f == 1) return DC_SOFT;  // ld.b/ub/h/uh %rd, %rs: D="-" but 1-cycle no-mem
        if (op2_f == 2) {
            int op1_f = (insn >> 10) & 7;
            switch (op1_f) {
            case 0: case 1: return DC_OK;    // mlt.h / mltu.h: D=○
            case 2: case 3: return DC_HARD;  // mlt.w / mltu.w: 5 cycles
            case 4: return DC_HARD;           // mac: multi-cycle + memory
            default: return DC_HARD;
            }
        }
        return DC_HARD;
    }
    return DC_HARD;
}

// ============================================================================
// Instruction step
// ============================================================================

int Cpu::step() {
    if (state.in_halt) return 1;

    uint32_t insn_pc      = state.pc;
    uint16_t insn         = bus_.fetch16(insn_pc);
    bool     in_slot      = state.in_delay_slot;
    uint32_t slot_target  = state.delay_target;

    state.pc = insn_pc + 2;

    // ------------------------------------------------------------------
    // Delay-slot violation check (before dispatch)
    // ------------------------------------------------------------------
    if (in_slot) {
        // A pending EXT before the delay-slot instruction means an EXT was
        // placed inside the delay slot (EXT is D="-", always a hard fault).
        if (state.pending_ext_count > 0) {
            diag_fault("delay_slot_ext", insn_pc,
                std::format("EXT prefix applied inside delay slot at 0x{:06X}", insn_pc));
            return 1;
        }
        DelayClass dsc = delay_slot_class(insn);
        if (dsc != DC_OK) {
            std::string detail = std::format("delay slot at 0x{:06X}: {}", insn_pc, disasm(insn_pc));
            if (dsc == DC_HARD) {
                diag_fault("delay_slot_hard", insn_pc,
                    detail + " (multi-cycle, memory access, or branch — forbidden)");
            } else {
                diag_warn_or_fault("delay_slot_soft", insn_pc,
                    detail + " (not listed as delay-slot-safe in the manual)");
            }
            if (state.in_halt) return 1;
        }

        // Warn when the delay slot of ret.d or call.d writes to SP:
        // the SP has already been modified by the branch, so a further SP write corrupts the stack.
        //   add/sub %sp, imm10 : cls=4, op1_f=0 or 1
        //   ld.w %sp, %rs      : cls=5, op2_f=0, op1_f=0, rd=1 (SP)
        if (state.delay_caller != 0) {
            bool writes_sp = (cls(insn) == 4 && ((insn >> 10) & 7) <= 1)
                          || (cls(insn) == 5 && ((insn >> 8) & 3) == 0
                              && ((insn >> 10) & 7) == 0 && (insn & 0xF) == 1);
            if (writes_sp) {
                static const char* caller_name[] = { "", "ret.d", "call.d" };
                diag_warn("delay_slot_sp_clobber", insn_pc,
                    std::format("{} in delay slot of {} at 0x{:06X}: "
                                "SP already adjusted by branch — stack will be corrupted",
                                disasm(insn_pc), caller_name[state.delay_caller], insn_pc));
            }
        }
    }

    // ------------------------------------------------------------------
    // EXT-before-non-EXT-capable check (before dispatch)
    // ------------------------------------------------------------------
    if (state.pending_ext_count > 0 && !ext_capable(insn)) {
        diag_warn_or_fault("ext_incompat", insn_pc,
            std::format("EXT prefix before {} at 0x{:06X}: instruction does not support EXT",
                        disasm(insn_pc), insn_pc));
        if (state.in_halt) return 1;
        // Non-strict: continue; the handler will call flush_ext() to discard
    }

    // ------------------------------------------------------------------
    // Dispatch
    // ------------------------------------------------------------------
    dispatch_[insn](*this, insn);

    // ------------------------------------------------------------------
    // Bus-level fault check (after dispatch)
    // ------------------------------------------------------------------
    if (bus_.take_fault() && !state.fault) {
        diag_fault("bus_fault", insn_pc,
                   std::format("bus fault during instruction at 0x{:06X}", insn_pc));
    }

    // ------------------------------------------------------------------
    // Delay-slot completion
    // ------------------------------------------------------------------
    if (in_slot && !state.in_halt) {
        state.in_delay_slot = false;
        state.delay_caller  = 0;
        state.pc = slot_target;
    }

    return 1;
}

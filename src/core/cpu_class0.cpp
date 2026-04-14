#include "cpu_impl.hpp"
#include "bus.hpp"
#include <format>

// ============================================================================
// Helper: handle delay slot for class 0 branches
// state.pc is already insn+2 (pre-advanced by step()).
// ============================================================================

static void do_c0_jump(Cpu& cpu, uint32_t target, bool delayed) {
    if (!delayed) {
        cpu.state.pc = target;
    } else {
        cpu.state.in_delay_slot = true;
        cpu.state.delay_target  = target;
    }
}

// ============================================================================
// Handlers — Class 0a: control flow
// ============================================================================

void Cpu::h_nop(Cpu& cpu, uint16_t) { cpu.flush_ext(); }

void Cpu::h_slp(Cpu& cpu, uint16_t) {
    cpu.flush_ext();
    cpu.state.in_halt = true;
}

void Cpu::h_halt(Cpu& cpu, uint16_t) {
    cpu.flush_ext();
    cpu.state.in_halt = true;
}

void Cpu::h_pushn(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    int rs_n = Insn{insn}.rd();
    for (int i = rs_n; i >= 0; i--) {
        cpu.state.sp -= 4;
        cpu.bus().write32(cpu.state.sp, cpu.state.r[i]);
    }
}

void Cpu::h_popn(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    int rd_n = Insn{insn}.rd();
    for (int i = 0; i <= rd_n; i++) {
        cpu.state.r[i] = cpu.bus().read32(cpu.state.sp);
        cpu.state.sp += 4;
    }
}

void Cpu::h_brk(Cpu& cpu, uint16_t) {
    cpu.flush_ext();
    cpu.do_trap(0, 0);
}

void Cpu::h_retd(Cpu& cpu, uint16_t) {
    // ret.d: delayed return — pops PSR and PC, executes delay slot, then jumps
    cpu.flush_ext();
    uint32_t ret_addr = cpu.bus().read32(cpu.state.sp); cpu.state.sp += 4;
    cpu.state.psr.raw = cpu.bus().read32(cpu.state.sp); cpu.state.sp += 4;
    cpu.state.in_delay_slot = true;
    cpu.state.delay_target  = ret_addr;
}

void Cpu::h_int(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    int vec = Insn{insn}.imm2();
    cpu.do_trap(vec, 0);
}

void Cpu::h_reti(Cpu& cpu, uint16_t) {
    cpu.flush_ext();
    cpu.state.psr.raw = cpu.bus().read32(cpu.state.sp); cpu.state.sp += 4;
    uint32_t ret_addr = cpu.bus().read32(cpu.state.sp); cpu.state.sp += 4;
    cpu.state.pc = ret_addr;
}

void Cpu::h_call_rb(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    Insn i{insn};
    int rb_n = i.rd();   // register is in bits [3:0] for class 0a
    bool delayed = i.d();
    uint32_t target = cpu.state.r[rb_n];
    uint32_t ret_addr = cpu.state.pc + (delayed ? 2 : 0);
    cpu.state.sp -= 4;
    cpu.bus().write32(cpu.state.sp, ret_addr);
    if (delayed) cpu.state.delay_caller = 2;
    do_c0_jump(cpu, target, delayed);
}

void Cpu::h_ret(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    bool delayed = Insn{insn}.d();
    uint32_t target = cpu.bus().read32(cpu.state.sp); cpu.state.sp += 4;
    if (delayed) cpu.state.delay_caller = 1;
    do_c0_jump(cpu, target, delayed);
}

void Cpu::h_jp_rb(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    Insn i{insn};
    int rb_n = i.rd();   // register is in bits [3:0] for class 0a
    bool delayed = i.d();
    uint32_t fault_pc = cpu.state.pc - 2;

    // jp.d %rb is forbidden: a hardware bug causes the DMA controller to skip
    // the delay slot on certain memory transactions.  Refuse to execute it.
    if (delayed) {
        cpu.diag_fault("jp_d_rb", fault_pc,
            std::format("jp.d %r{} at 0x{:06X}: forbidden (hardware DMA bug, use jp %%r{})",
                        rb_n, fault_pc, rb_n));
        return;
    }

    uint32_t target = cpu.state.r[rb_n];
    do_c0_jump(cpu, target, false);
}

// ============================================================================
// Handlers — Class 0b: conditional branches
// ============================================================================

// PC-relative branch: target = insn_addr + 2 * disp
// insn_addr is (state.pc - 2) because step() pre-advances PC.
void Cpu::h_jr(Cpu& cpu, uint16_t insn, bool taken) {
    Insn i{insn};
    int32_t disp = cpu.ext_pcrel(i.sign8());
    bool delayed = i.d();
    cpu.flush_ext();
    if (taken) {
        uint32_t insn_addr = cpu.state.pc - 2;
        uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(insn_addr) + 2 * disp);
        do_c0_jump(cpu, target, delayed);
    }
    // Not taken: PC already at insn_addr+2 (pre-advanced by step()).
    // The delay slot (if d=1) executes naturally as the next instruction,
    // leaving PC at insn_addr+4 — correct for a not-taken delayed branch.
}

void Cpu::h_jrgt (Cpu& cpu, uint16_t i) { h_jr(cpu, i, !(cpu.state.psr.n()^cpu.state.psr.v()) && !cpu.state.psr.z()); }
void Cpu::h_jrge (Cpu& cpu, uint16_t i) { h_jr(cpu, i, !(cpu.state.psr.n()^cpu.state.psr.v())); }
void Cpu::h_jrlt (Cpu& cpu, uint16_t i) { h_jr(cpu, i,  (cpu.state.psr.n()^cpu.state.psr.v())); }
void Cpu::h_jrle (Cpu& cpu, uint16_t i) { h_jr(cpu, i,  (cpu.state.psr.n()^cpu.state.psr.v()) || cpu.state.psr.z()); }
void Cpu::h_jrugt(Cpu& cpu, uint16_t i) { h_jr(cpu, i, !cpu.state.psr.c() && !cpu.state.psr.z()); }
void Cpu::h_jruge(Cpu& cpu, uint16_t i) { h_jr(cpu, i, !cpu.state.psr.c()); }
void Cpu::h_jrult(Cpu& cpu, uint16_t i) { h_jr(cpu, i,  cpu.state.psr.c()); }
void Cpu::h_jrule(Cpu& cpu, uint16_t i) { h_jr(cpu, i,  cpu.state.psr.c() || cpu.state.psr.z()); }
void Cpu::h_jreq (Cpu& cpu, uint16_t i) { h_jr(cpu, i,  cpu.state.psr.z()); }
void Cpu::h_jrne (Cpu& cpu, uint16_t i) { h_jr(cpu, i, !cpu.state.psr.z()); }

void Cpu::h_call_simm8(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    int32_t disp = cpu.ext_pcrel(i.sign8());
    cpu.flush_ext();
    uint32_t insn_addr = cpu.state.pc - 2;
    uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(insn_addr) + 2 * disp);
    bool delayed = i.d();
    uint32_t ret_addr = cpu.state.pc + (delayed ? 2 : 0);
    cpu.state.sp -= 4;
    cpu.bus().write32(cpu.state.sp, ret_addr);
    if (delayed) cpu.state.delay_caller = 2;
    do_c0_jump(cpu, target, delayed);
}

void Cpu::h_jp_simm8(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    int32_t disp = cpu.ext_pcrel(i.sign8());
    cpu.flush_ext();
    uint32_t insn_addr = cpu.state.pc - 2;
    bool delayed = i.d();
    uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(insn_addr) + 2 * disp);
    do_c0_jump(cpu, target, delayed);
}

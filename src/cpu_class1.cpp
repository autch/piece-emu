#include "cpu_impl.hpp"
#include "bus.hpp"

// ============================================================================
// Handlers — Class 1A: register-indirect [%rb]
// EA = r[rb] + ext_rb() (unsigned displacement from EXT, or 0 if no EXT)
// ============================================================================

void Cpu::h_ld_b_rd_rb(Cpu& cpu, uint16_t insn) {
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    cpu.state.r[rd(insn)] = static_cast<uint32_t>(static_cast<int8_t>(
        cpu.bus_.read8(cpu.state.r[rb(insn)] + disp)));
}
void Cpu::h_ld_ub_rd_rb(Cpu& cpu, uint16_t insn) {
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    cpu.state.r[rd(insn)] = cpu.bus_.read8(cpu.state.r[rb(insn)] + disp);
}
void Cpu::h_ld_h_rd_rb(Cpu& cpu, uint16_t insn) {
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    cpu.state.r[rd(insn)] = static_cast<uint32_t>(static_cast<int16_t>(
        cpu.bus_.read16(cpu.state.r[rb(insn)] + disp)));
}
void Cpu::h_ld_uh_rd_rb(Cpu& cpu, uint16_t insn) {
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    cpu.state.r[rd(insn)] = cpu.bus_.read16(cpu.state.r[rb(insn)] + disp);
}
void Cpu::h_ld_w_rd_rb(Cpu& cpu, uint16_t insn) {
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    cpu.state.r[rd(insn)] = cpu.bus_.read32(cpu.state.r[rb(insn)] + disp);
}
void Cpu::h_st_b_rb_rs(Cpu& cpu, uint16_t insn) {
    // CLASS_1A store: rb=bits[7:4], source register=bits[3:0]=rd(insn)
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    cpu.bus_.write8(cpu.state.r[rb(insn)] + disp,
                    static_cast<uint8_t>(cpu.state.r[rd(insn)]));
}
void Cpu::h_st_h_rb_rs(Cpu& cpu, uint16_t insn) {
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    cpu.bus_.write16(cpu.state.r[rb(insn)] + disp,
                     static_cast<uint16_t>(cpu.state.r[rd(insn)]));
}
void Cpu::h_st_w_rb_rs(Cpu& cpu, uint16_t insn) {
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    cpu.bus_.write32(cpu.state.r[rb(insn)] + disp, cpu.state.r[rd(insn)]);
}

// ============================================================================
// Handlers — Class 1B: register-indirect with post-increment [%rb+]
// ============================================================================

void Cpu::h_ld_b_rd_rbx(Cpu& cpu, uint16_t insn) {
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    uint32_t ea = cpu.state.r[rb(insn)] + disp;
    cpu.state.r[rd(insn)] = static_cast<uint32_t>(static_cast<int8_t>(cpu.bus_.read8(ea)));
    cpu.state.r[rb(insn)] += 1;
}
void Cpu::h_ld_ub_rd_rbx(Cpu& cpu, uint16_t insn) {
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    uint32_t ea = cpu.state.r[rb(insn)] + disp;
    cpu.state.r[rd(insn)] = cpu.bus_.read8(ea);
    cpu.state.r[rb(insn)] += 1;
}
void Cpu::h_ld_h_rd_rbx(Cpu& cpu, uint16_t insn) {
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    uint32_t ea = cpu.state.r[rb(insn)] + disp;
    cpu.state.r[rd(insn)] = static_cast<uint32_t>(static_cast<int16_t>(cpu.bus_.read16(ea)));
    cpu.state.r[rb(insn)] += 2;
}
void Cpu::h_ld_uh_rd_rbx(Cpu& cpu, uint16_t insn) {
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    uint32_t ea = cpu.state.r[rb(insn)] + disp;
    cpu.state.r[rd(insn)] = cpu.bus_.read16(ea);
    cpu.state.r[rb(insn)] += 2;
}
void Cpu::h_ld_w_rd_rbx(Cpu& cpu, uint16_t insn) {
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    uint32_t ea = cpu.state.r[rb(insn)] + disp;
    cpu.state.r[rd(insn)] = cpu.bus_.read32(ea);
    cpu.state.r[rb(insn)] += 4;
}
void Cpu::h_st_b_rbx_rs(Cpu& cpu, uint16_t insn) {
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    uint32_t ea = cpu.state.r[rb(insn)] + disp;
    cpu.bus_.write8(ea, static_cast<uint8_t>(cpu.state.r[rd(insn)]));
    cpu.state.r[rb(insn)] += 1;
}
void Cpu::h_st_h_rbx_rs(Cpu& cpu, uint16_t insn) {
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    uint32_t ea = cpu.state.r[rb(insn)] + disp;
    cpu.bus_.write16(ea, static_cast<uint16_t>(cpu.state.r[rd(insn)]));
    cpu.state.r[rb(insn)] += 2;
}
void Cpu::h_st_w_rbx_rs(Cpu& cpu, uint16_t insn) {
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    uint32_t ea = cpu.state.r[rb(insn)] + disp;
    cpu.bus_.write32(ea, cpu.state.r[rd(insn)]);
    cpu.state.r[rb(insn)] += 4;
}

// ============================================================================
// Handlers — Class 1C: register-register ALU
// ============================================================================

// EXT 3-operand form: when EXT prefix is present, rd ← rs op imm13/imm26.
// Without EXT:        rd ← rd op rs  (2-operand form).
void Cpu::h_add_rd_rs(Cpu& cpu, uint16_t insn) {
    if (cpu.state.pending_ext_count) {
        uint32_t imm = cpu.ext_rb(); cpu.flush_ext();
        uint32_t a = cpu.state.r[rs(insn)];
        uint64_t r = uint64_t(a) + imm;
        cpu.state.r[rd(insn)] = uint32_t(r);
        cpu.state.psr.set_nzvc_add(a, imm, r);
    } else {
        cpu.flush_ext();
        uint32_t a = cpu.state.r[rd(insn)], b = cpu.state.r[rs(insn)];
        uint64_t r = uint64_t(a) + b;
        cpu.state.r[rd(insn)] = uint32_t(r);
        cpu.state.psr.set_nzvc_add(a, b, r);
    }
}
void Cpu::h_sub_rd_rs(Cpu& cpu, uint16_t insn) {
    if (cpu.state.pending_ext_count) {
        uint32_t imm = cpu.ext_rb(); cpu.flush_ext();
        uint32_t a = cpu.state.r[rs(insn)];
        uint64_t r = uint64_t(a) - imm;
        cpu.state.r[rd(insn)] = uint32_t(r);
        cpu.state.psr.set_nzvc_sub(a, imm, r);
    } else {
        cpu.flush_ext();
        uint32_t a = cpu.state.r[rd(insn)], b = cpu.state.r[rs(insn)];
        uint64_t r = uint64_t(a) - b;
        cpu.state.r[rd(insn)] = uint32_t(r);
        cpu.state.psr.set_nzvc_sub(a, b, r);
    }
}
void Cpu::h_cmp_rd_rs(Cpu& cpu, uint16_t insn) {
    if (cpu.state.pending_ext_count) {
        uint32_t imm = cpu.ext_rb(); cpu.flush_ext();
        uint32_t a = cpu.state.r[rs(insn)];
        uint64_t r = uint64_t(a) - imm;
        cpu.state.psr.set_nzvc_sub(a, imm, r);
    } else {
        cpu.flush_ext();
        uint32_t a = cpu.state.r[rd(insn)], b = cpu.state.r[rs(insn)];
        uint64_t r = uint64_t(a) - b;
        cpu.state.psr.set_nzvc_sub(a, b, r);
    }
}
void Cpu::h_ld_w_rd_rs(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    cpu.state.r[rd(insn)] = cpu.state.r[rs(insn)];
}
void Cpu::h_and_rd_rs(Cpu& cpu, uint16_t insn) {
    if (cpu.state.pending_ext_count) {
        uint32_t imm = cpu.ext_rb(); cpu.flush_ext();
        uint32_t r = cpu.state.r[rs(insn)] & imm;
        cpu.state.r[rd(insn)] = r; cpu.state.psr.set_nz(r);
    } else {
        cpu.flush_ext();
        uint32_t r = cpu.state.r[rd(insn)] & cpu.state.r[rs(insn)];
        cpu.state.r[rd(insn)] = r; cpu.state.psr.set_nz(r);
    }
}
void Cpu::h_or_rd_rs(Cpu& cpu, uint16_t insn) {
    if (cpu.state.pending_ext_count) {
        uint32_t imm = cpu.ext_rb(); cpu.flush_ext();
        uint32_t r = cpu.state.r[rs(insn)] | imm;
        cpu.state.r[rd(insn)] = r; cpu.state.psr.set_nz(r);
    } else {
        cpu.flush_ext();
        uint32_t r = cpu.state.r[rd(insn)] | cpu.state.r[rs(insn)];
        cpu.state.r[rd(insn)] = r; cpu.state.psr.set_nz(r);
    }
}
void Cpu::h_xor_rd_rs(Cpu& cpu, uint16_t insn) {
    if (cpu.state.pending_ext_count) {
        uint32_t imm = cpu.ext_rb(); cpu.flush_ext();
        uint32_t r = cpu.state.r[rs(insn)] ^ imm;
        cpu.state.r[rd(insn)] = r; cpu.state.psr.set_nz(r);
    } else {
        cpu.flush_ext();
        uint32_t r = cpu.state.r[rd(insn)] ^ cpu.state.r[rs(insn)];
        cpu.state.r[rd(insn)] = r; cpu.state.psr.set_nz(r);
    }
}
void Cpu::h_not_rd_rs(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();  // EXT not allowed on not %rd, %rs
    uint32_t r = ~cpu.state.r[rs(insn)];
    cpu.state.r[rd(insn)] = r; cpu.state.psr.set_nz(r);
}

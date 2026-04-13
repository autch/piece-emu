#include "cpu_impl.hpp"

// ============================================================================
// Handlers — Class 3: immediate ALU
// ============================================================================

void Cpu::h_add_rd_imm6(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    uint32_t imm = cpu.ext_imm(i.imm6(), 6); cpu.flush_ext();
    uint32_t a = cpu.state.r[i.rd()];
    uint64_t r = uint64_t(a) + imm;
    cpu.state.r[i.rd()] = uint32_t(r);
    cpu.state.psr.set_nzvc_add(a, imm, r);
}
void Cpu::h_sub_rd_imm6(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    uint32_t imm = cpu.ext_imm(i.imm6(), 6); cpu.flush_ext();
    uint32_t a = cpu.state.r[i.rd()];
    uint64_t r = uint64_t(a) - imm;
    cpu.state.r[i.rd()] = uint32_t(r);
    cpu.state.psr.set_nzvc_sub(a, imm, r);
}
void Cpu::h_cmp_rd_simm6(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    int32_t simm = cpu.ext_simm(i.imm6(), 6); cpu.flush_ext();
    uint32_t a = cpu.state.r[i.rd()];
    uint32_t b = static_cast<uint32_t>(simm);
    uint64_t r = uint64_t(a) - b;
    cpu.state.psr.set_nzvc_sub(a, b, r);
}
void Cpu::h_ld_w_rd_simm6(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    int32_t simm = cpu.ext_simm(i.imm6(), 6); cpu.flush_ext();
    cpu.state.r[i.rd()] = static_cast<uint32_t>(simm);
}
void Cpu::h_and_rd_simm6(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    int32_t simm = cpu.ext_simm(i.imm6(), 6); cpu.flush_ext();
    uint32_t r = cpu.state.r[i.rd()] & static_cast<uint32_t>(simm);
    cpu.state.r[i.rd()] = r; cpu.state.psr.set_nz(r);
}
void Cpu::h_or_rd_simm6(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    int32_t simm = cpu.ext_simm(i.imm6(), 6); cpu.flush_ext();
    uint32_t r = cpu.state.r[i.rd()] | static_cast<uint32_t>(simm);
    cpu.state.r[i.rd()] = r; cpu.state.psr.set_nz(r);
}
void Cpu::h_xor_rd_simm6(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    int32_t simm = cpu.ext_simm(i.imm6(), 6); cpu.flush_ext();
    uint32_t r = cpu.state.r[i.rd()] ^ static_cast<uint32_t>(simm);
    cpu.state.r[i.rd()] = r; cpu.state.psr.set_nz(r);
}
void Cpu::h_not_rd_simm6(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    int32_t simm = cpu.ext_simm(i.imm6(), 6); cpu.flush_ext();
    uint32_t r = ~static_cast<uint32_t>(simm);
    cpu.state.r[i.rd()] = r; cpu.state.psr.set_nz(r);
}

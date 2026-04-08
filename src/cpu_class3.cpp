#include "cpu_impl.hpp"

// CLASS_3: rd = bits[3:0], imm6 = bits[9:4], op1 = bits[12:10]
static inline int      c3_rd  (uint16_t i) { return i & 0xF; }
static inline uint32_t c3_imm6(uint16_t i) { return (i >> 4) & 0x3F; }

// ============================================================================
// Handlers — Class 3: immediate ALU
// ============================================================================

void Cpu::h_add_rd_imm6(Cpu& cpu, uint16_t insn) {
    uint32_t imm = cpu.ext_imm(c3_imm6(insn), 6); cpu.flush_ext();
    uint32_t a = cpu.state.r[c3_rd(insn)];
    uint64_t r = uint64_t(a) + imm;
    cpu.state.r[c3_rd(insn)] = uint32_t(r);
    cpu.state.psr.set_nzvc_add(a, imm, r);
}
void Cpu::h_sub_rd_imm6(Cpu& cpu, uint16_t insn) {
    uint32_t imm = cpu.ext_imm(c3_imm6(insn), 6); cpu.flush_ext();
    uint32_t a = cpu.state.r[c3_rd(insn)];
    uint64_t r = uint64_t(a) - imm;
    cpu.state.r[c3_rd(insn)] = uint32_t(r);
    cpu.state.psr.set_nzvc_sub(a, imm, r);
}
void Cpu::h_cmp_rd_simm6(Cpu& cpu, uint16_t insn) {
    int32_t simm = cpu.ext_simm(c3_imm6(insn), 6); cpu.flush_ext();
    uint32_t a = cpu.state.r[c3_rd(insn)];
    uint32_t b = static_cast<uint32_t>(simm);
    uint64_t r = uint64_t(a) - b;
    cpu.state.psr.set_nzvc_sub(a, b, r);
}
void Cpu::h_ld_w_rd_simm6(Cpu& cpu, uint16_t insn) {
    int32_t simm = cpu.ext_simm(c3_imm6(insn), 6); cpu.flush_ext();
    cpu.state.r[c3_rd(insn)] = static_cast<uint32_t>(simm);
}
void Cpu::h_and_rd_simm6(Cpu& cpu, uint16_t insn) {
    int32_t simm = cpu.ext_simm(c3_imm6(insn), 6); cpu.flush_ext();
    uint32_t r = cpu.state.r[c3_rd(insn)] & static_cast<uint32_t>(simm);
    cpu.state.r[c3_rd(insn)] = r; cpu.state.psr.set_nz(r);
}
void Cpu::h_or_rd_simm6(Cpu& cpu, uint16_t insn) {
    int32_t simm = cpu.ext_simm(c3_imm6(insn), 6); cpu.flush_ext();
    uint32_t r = cpu.state.r[c3_rd(insn)] | static_cast<uint32_t>(simm);
    cpu.state.r[c3_rd(insn)] = r; cpu.state.psr.set_nz(r);
}
void Cpu::h_xor_rd_simm6(Cpu& cpu, uint16_t insn) {
    int32_t simm = cpu.ext_simm(c3_imm6(insn), 6); cpu.flush_ext();
    uint32_t r = cpu.state.r[c3_rd(insn)] ^ static_cast<uint32_t>(simm);
    cpu.state.r[c3_rd(insn)] = r; cpu.state.psr.set_nz(r);
}
void Cpu::h_not_rd_simm6(Cpu& cpu, uint16_t insn) {
    int32_t simm = cpu.ext_simm(c3_imm6(insn), 6); cpu.flush_ext();
    uint32_t r = ~static_cast<uint32_t>(simm);
    cpu.state.r[c3_rd(insn)] = r; cpu.state.psr.set_nz(r);
}

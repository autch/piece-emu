#include "cpu_impl.hpp"
#include "bus.hpp"

// CLASS_2: rs_rd = bits[3:0], imm6 = bits[9:4], op1 = bits[12:10]
static inline int      c2_rdrs(uint16_t i) { return i & 0xF; }
static inline uint32_t c2_imm6(uint16_t i) { return (i >> 4) & 0x3F; }

// EA = SP + imm6 * elem_size (no EXT), or SP + combined_byte_offset (with EXT)
static uint32_t sp_ea(const Cpu& cpu, uint16_t insn, int elem_size) {
    uint32_t imm6 = c2_imm6(insn);
    if (cpu.state.pending_ext_count == 0)
        return cpu.state.sp + imm6 * elem_size;
    return cpu.state.sp + cpu.ext_imm(imm6, 6);
}

// ============================================================================
// Handlers — Class 2: SP-relative load/store
// ============================================================================

void Cpu::h_ld_b_rd_sp(Cpu& cpu, uint16_t insn) {
    uint32_t ea = sp_ea(cpu, insn, 1); cpu.flush_ext();
    cpu.state.r[c2_rdrs(insn)] = static_cast<uint32_t>(static_cast<int8_t>(cpu.bus_.read8(ea)));
}
void Cpu::h_ld_ub_rd_sp(Cpu& cpu, uint16_t insn) {
    uint32_t ea = sp_ea(cpu, insn, 1); cpu.flush_ext();
    cpu.state.r[c2_rdrs(insn)] = cpu.bus_.read8(ea);
}
void Cpu::h_ld_h_rd_sp(Cpu& cpu, uint16_t insn) {
    uint32_t ea = sp_ea(cpu, insn, 2); cpu.flush_ext();
    cpu.state.r[c2_rdrs(insn)] = static_cast<uint32_t>(static_cast<int16_t>(cpu.bus_.read16(ea)));
}
void Cpu::h_ld_uh_rd_sp(Cpu& cpu, uint16_t insn) {
    uint32_t ea = sp_ea(cpu, insn, 2); cpu.flush_ext();
    cpu.state.r[c2_rdrs(insn)] = cpu.bus_.read16(ea);
}
void Cpu::h_ld_w_rd_sp(Cpu& cpu, uint16_t insn) {
    uint32_t ea = sp_ea(cpu, insn, 4); cpu.flush_ext();
    cpu.state.r[c2_rdrs(insn)] = cpu.bus_.read32(ea);
}
void Cpu::h_st_b_sp_rs(Cpu& cpu, uint16_t insn) {
    uint32_t ea = sp_ea(cpu, insn, 1); cpu.flush_ext();
    cpu.bus_.write8(ea, static_cast<uint8_t>(cpu.state.r[c2_rdrs(insn)]));
}
void Cpu::h_st_h_sp_rs(Cpu& cpu, uint16_t insn) {
    uint32_t ea = sp_ea(cpu, insn, 2); cpu.flush_ext();
    cpu.bus_.write16(ea, static_cast<uint16_t>(cpu.state.r[c2_rdrs(insn)]));
}
void Cpu::h_st_w_sp_rs(Cpu& cpu, uint16_t insn) {
    uint32_t ea = sp_ea(cpu, insn, 4); cpu.flush_ext();
    cpu.bus_.write32(ea, cpu.state.r[c2_rdrs(insn)]);
}

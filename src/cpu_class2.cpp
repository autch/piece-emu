#include "cpu_impl.hpp"
#include "bus.hpp"

// EA = SP + imm6 * elem_size (no EXT), or SP + combined_byte_offset (with EXT)
static uint32_t sp_ea(const Cpu& cpu, uint16_t insn, int elem_size) {
    uint32_t imm6 = Insn{insn}.imm6();
    if (cpu.state.pending_ext_count == 0)
        return cpu.state.sp + imm6 * elem_size;
    return cpu.state.sp + cpu.ext_imm(imm6, 6);
}

// ============================================================================
// Load/store helpers: [%sp+imm6*scale] (class 2)
//
// do_load_sp <Size, SignExtend>
//   Reads Size bytes from EA = SP + scaled_offset.
//   If SignExtend, sign-extends the result into r[rd].
//
// do_store_sp<Size>
//   Writes the low Size bytes of r[rd] to EA = SP + scaled_offset.
// ============================================================================

template<int Size, bool SignExtend>
static void do_load_sp(Cpu& cpu, uint16_t insn) {
    uint32_t ea = sp_ea(cpu, insn, Size); cpu.flush_ext();
    uint32_t val;
    if constexpr (Size == 1) {
        val = cpu.bus().read8(ea);
        if constexpr (SignExtend) val = static_cast<uint32_t>(static_cast<int8_t>(val));
    } else if constexpr (Size == 2) {
        val = cpu.bus().read16(ea);
        if constexpr (SignExtend) val = static_cast<uint32_t>(static_cast<int16_t>(val));
    } else {
        val = cpu.bus().read32(ea);
    }
    cpu.state.r[Insn{insn}.rd()] = val;
}

template<int Size>
static void do_store_sp(Cpu& cpu, uint16_t insn) {
    uint32_t ea = sp_ea(cpu, insn, Size); cpu.flush_ext();
    uint32_t src = cpu.state.r[Insn{insn}.rd()];
    if constexpr (Size == 1)      cpu.bus().write8 (ea, static_cast<uint8_t> (src));
    else if constexpr (Size == 2) cpu.bus().write16(ea, static_cast<uint16_t>(src));
    else                          cpu.bus().write32(ea,                        src);
}

// ============================================================================
// Handlers — Class 2: SP-relative load/store
// ============================================================================

void Cpu::h_ld_b_rd_sp (Cpu& cpu, uint16_t insn) { do_load_sp <1, true> (cpu, insn); }
void Cpu::h_ld_ub_rd_sp(Cpu& cpu, uint16_t insn) { do_load_sp <1, false>(cpu, insn); }
void Cpu::h_ld_h_rd_sp (Cpu& cpu, uint16_t insn) { do_load_sp <2, true> (cpu, insn); }
void Cpu::h_ld_uh_rd_sp(Cpu& cpu, uint16_t insn) { do_load_sp <2, false>(cpu, insn); }
void Cpu::h_ld_w_rd_sp (Cpu& cpu, uint16_t insn) { do_load_sp <4, false>(cpu, insn); }
void Cpu::h_st_b_sp_rs (Cpu& cpu, uint16_t insn) { do_store_sp<1>(cpu, insn); }
void Cpu::h_st_h_sp_rs (Cpu& cpu, uint16_t insn) { do_store_sp<2>(cpu, insn); }
void Cpu::h_st_w_sp_rs (Cpu& cpu, uint16_t insn) { do_store_sp<4>(cpu, insn); }

#include "cpu_impl.hpp"
#include "bus.hpp"

// ============================================================================
// Load/store helpers: [%rb] (class 1A) and [%rb+] (class 1B)
//
// do_load_rb <Size, SignExtend, PostInc>
//   Reads Size bytes from EA = r[rb] + ext_rb_disp.
//   If SignExtend, sign-extends the result into r[rd].
//   If PostInc,    increments r[rb] by Size after the read.
//
// do_store_rb<Size, PostInc>
//   Writes the low Size bytes of r[rd] to EA = r[rb] + ext_rb_disp.
//   (bits[3:0] hold the source register for stores — same field as rd for loads)
//   If PostInc, increments r[rb] by Size after the write.
// ============================================================================

template<int Size, bool SignExtend, bool PostInc>
static void do_load_rb(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    uint32_t ea   = cpu.state.r[i.rb()] + disp;
    uint32_t val;
    if constexpr (Size == 1) {
        val = cpu.bus_.read8(ea);
        if constexpr (SignExtend) val = static_cast<uint32_t>(static_cast<int8_t>(val));
    } else if constexpr (Size == 2) {
        val = cpu.bus_.read16(ea);
        if constexpr (SignExtend) val = static_cast<uint32_t>(static_cast<int16_t>(val));
    } else {
        val = cpu.bus_.read32(ea);
    }
    cpu.state.r[i.rd()] = val;
    if constexpr (PostInc) cpu.state.r[i.rb()] += Size;
}

template<int Size, bool PostInc>
static void do_store_rb(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    uint32_t disp = cpu.ext_rb(); cpu.flush_ext();
    uint32_t ea   = cpu.state.r[i.rb()] + disp;
    if constexpr (Size == 1)      cpu.bus_.write8 (ea, static_cast<uint8_t> (cpu.state.r[i.rd()]));
    else if constexpr (Size == 2) cpu.bus_.write16(ea, static_cast<uint16_t>(cpu.state.r[i.rd()]));
    else                          cpu.bus_.write32(ea,                        cpu.state.r[i.rd()]);
    if constexpr (PostInc) cpu.state.r[i.rb()] += Size;
}

// ============================================================================
// Handlers — Class 1A: register-indirect [%rb]
// EA = r[rb] + ext_rb() (unsigned displacement from EXT, or 0 if no EXT)
// ============================================================================

void Cpu::h_ld_b_rd_rb  (Cpu& cpu, uint16_t insn) { do_load_rb <1, true,  false>(cpu, insn); }
void Cpu::h_ld_ub_rd_rb (Cpu& cpu, uint16_t insn) { do_load_rb <1, false, false>(cpu, insn); }
void Cpu::h_ld_h_rd_rb  (Cpu& cpu, uint16_t insn) { do_load_rb <2, true,  false>(cpu, insn); }
void Cpu::h_ld_uh_rd_rb (Cpu& cpu, uint16_t insn) { do_load_rb <2, false, false>(cpu, insn); }
void Cpu::h_ld_w_rd_rb  (Cpu& cpu, uint16_t insn) { do_load_rb <4, false, false>(cpu, insn); }
void Cpu::h_st_b_rb_rs  (Cpu& cpu, uint16_t insn) { do_store_rb<1, false>(cpu, insn); }
void Cpu::h_st_h_rb_rs  (Cpu& cpu, uint16_t insn) { do_store_rb<2, false>(cpu, insn); }
void Cpu::h_st_w_rb_rs  (Cpu& cpu, uint16_t insn) { do_store_rb<4, false>(cpu, insn); }

// ============================================================================
// Handlers — Class 1B: register-indirect with post-increment [%rb+]
// ============================================================================

void Cpu::h_ld_b_rd_rbx (Cpu& cpu, uint16_t insn) { do_load_rb <1, true,  true>(cpu, insn); }
void Cpu::h_ld_ub_rd_rbx(Cpu& cpu, uint16_t insn) { do_load_rb <1, false, true>(cpu, insn); }
void Cpu::h_ld_h_rd_rbx (Cpu& cpu, uint16_t insn) { do_load_rb <2, true,  true>(cpu, insn); }
void Cpu::h_ld_uh_rd_rbx(Cpu& cpu, uint16_t insn) { do_load_rb <2, false, true>(cpu, insn); }
void Cpu::h_ld_w_rd_rbx (Cpu& cpu, uint16_t insn) { do_load_rb <4, false, true>(cpu, insn); }
void Cpu::h_st_b_rbx_rs (Cpu& cpu, uint16_t insn) { do_store_rb<1, true>(cpu, insn); }
void Cpu::h_st_h_rbx_rs (Cpu& cpu, uint16_t insn) { do_store_rb<2, true>(cpu, insn); }
void Cpu::h_st_w_rbx_rs (Cpu& cpu, uint16_t insn) { do_store_rb<4, true>(cpu, insn); }

// ============================================================================
// Handlers — Class 1C: register-register ALU
// ============================================================================

// EXT 3-operand form: when EXT prefix is present, rd ← rs op imm13/imm26.
// Without EXT:        rd ← rd op rs  (2-operand form).
void Cpu::h_add_rd_rs(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    if (cpu.state.pending_ext_count) {
        uint32_t imm = cpu.ext_rb(); cpu.flush_ext();
        uint32_t a = cpu.state.r[i.rs()];
        uint64_t r = uint64_t(a) + imm;
        cpu.state.r[i.rd()] = uint32_t(r);
        cpu.state.psr.set_nzvc_add(a, imm, r);
    } else {
        cpu.flush_ext();
        uint32_t a = cpu.state.r[i.rd()], b = cpu.state.r[i.rs()];
        uint64_t r = uint64_t(a) + b;
        cpu.state.r[i.rd()] = uint32_t(r);
        cpu.state.psr.set_nzvc_add(a, b, r);
    }
}
void Cpu::h_sub_rd_rs(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    if (cpu.state.pending_ext_count) {
        uint32_t imm = cpu.ext_rb(); cpu.flush_ext();
        uint32_t a = cpu.state.r[i.rs()];
        uint64_t r = uint64_t(a) - imm;
        cpu.state.r[i.rd()] = uint32_t(r);
        cpu.state.psr.set_nzvc_sub(a, imm, r);
    } else {
        cpu.flush_ext();
        uint32_t a = cpu.state.r[i.rd()], b = cpu.state.r[i.rs()];
        uint64_t r = uint64_t(a) - b;
        cpu.state.r[i.rd()] = uint32_t(r);
        cpu.state.psr.set_nzvc_sub(a, b, r);
    }
}
void Cpu::h_cmp_rd_rs(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    if (cpu.state.pending_ext_count) {
        uint32_t imm = cpu.ext_rb(); cpu.flush_ext();
        uint32_t a = cpu.state.r[i.rs()];
        uint64_t r = uint64_t(a) - imm;
        cpu.state.psr.set_nzvc_sub(a, imm, r);
    } else {
        cpu.flush_ext();
        uint32_t a = cpu.state.r[i.rd()], b = cpu.state.r[i.rs()];
        uint64_t r = uint64_t(a) - b;
        cpu.state.psr.set_nzvc_sub(a, b, r);
    }
}
void Cpu::h_ld_w_rd_rs(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    cpu.state.r[i.rd()] = cpu.state.r[i.rs()];
}
void Cpu::h_and_rd_rs(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    if (cpu.state.pending_ext_count) {
        uint32_t imm = cpu.ext_rb(); cpu.flush_ext();
        uint32_t r = cpu.state.r[i.rs()] & imm;
        cpu.state.r[i.rd()] = r; cpu.state.psr.set_nz(r);
    } else {
        cpu.flush_ext();
        uint32_t r = cpu.state.r[i.rd()] & cpu.state.r[i.rs()];
        cpu.state.r[i.rd()] = r; cpu.state.psr.set_nz(r);
    }
}
void Cpu::h_or_rd_rs(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    if (cpu.state.pending_ext_count) {
        uint32_t imm = cpu.ext_rb(); cpu.flush_ext();
        uint32_t r = cpu.state.r[i.rs()] | imm;
        cpu.state.r[i.rd()] = r; cpu.state.psr.set_nz(r);
    } else {
        cpu.flush_ext();
        uint32_t r = cpu.state.r[i.rd()] | cpu.state.r[i.rs()];
        cpu.state.r[i.rd()] = r; cpu.state.psr.set_nz(r);
    }
}
void Cpu::h_xor_rd_rs(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    if (cpu.state.pending_ext_count) {
        uint32_t imm = cpu.ext_rb(); cpu.flush_ext();
        uint32_t r = cpu.state.r[i.rs()] ^ imm;
        cpu.state.r[i.rd()] = r; cpu.state.psr.set_nz(r);
    } else {
        cpu.flush_ext();
        uint32_t r = cpu.state.r[i.rd()] ^ cpu.state.r[i.rs()];
        cpu.state.r[i.rd()] = r; cpu.state.psr.set_nz(r);
    }
}
void Cpu::h_not_rd_rs(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();  // EXT not allowed on not %rd, %rs
    uint32_t r = ~cpu.state.r[i.rs()];
    cpu.state.r[i.rd()] = r; cpu.state.psr.set_nz(r);
}

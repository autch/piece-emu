#include "cpu_impl.hpp"
#include "bus.hpp"

// Special register numbers: PSR=0, SP=1, ALR=2, AHR=3
static uint32_t& special_reg(CpuState& s, int n) {
    switch (n & 3) {
    case 0: return s.psr.raw;
    case 1: return s.sp;
    case 2: return s.alr;
    default: return s.ahr;
    }
}

// ============================================================================
// Handlers — Class 5A: special register moves and bit operations
// ============================================================================

void Cpu::h_ld_w_sd_rs(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    special_reg(cpu.state, i.rd()) = cpu.state.r[i.rs()];
}
void Cpu::h_ld_w_rd_ss(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    cpu.state.r[i.rd()] = special_reg(cpu.state, i.rs());
}
void Cpu::h_btst(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    uint32_t ea = cpu.state.r[i.rb()] + cpu.ext_rb();
    cpu.flush_ext();
    uint8_t byte_val = cpu.bus().read8(ea);
    cpu.state.psr.set_z(!((byte_val >> i.imm3()) & 1));
}
void Cpu::h_bclr(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    uint32_t ea = cpu.state.r[i.rb()] + cpu.ext_rb();
    cpu.flush_ext();
    cpu.bus().write8(ea, cpu.bus().read8(ea) & ~(1u << i.imm3()));
}
void Cpu::h_bset(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    uint32_t ea = cpu.state.r[i.rb()] + cpu.ext_rb();
    cpu.flush_ext();
    cpu.bus().write8(ea, cpu.bus().read8(ea) | (1u << i.imm3()));
}
void Cpu::h_bnot(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    uint32_t ea = cpu.state.r[i.rb()] + cpu.ext_rb();
    cpu.flush_ext();
    cpu.bus().write8(ea, cpu.bus().read8(ea) ^ (1u << i.imm3()));
}
void Cpu::h_adc(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    uint32_t a = cpu.state.r[i.rd()], b = cpu.state.r[i.rs()];
    uint64_t r = uint64_t(a) + b + (cpu.state.psr.c() ? 1 : 0);
    cpu.state.r[i.rd()] = uint32_t(r);
    cpu.state.psr.set_nzvc_add(a, b, r);
}
void Cpu::h_sbc(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    uint32_t a = cpu.state.r[i.rd()], b = cpu.state.r[i.rs()];
    uint64_t r = uint64_t(a) - b - (cpu.state.psr.c() ? 1 : 0);
    cpu.state.r[i.rd()] = uint32_t(r);
    cpu.state.psr.set_nzvc_sub(a, b, r);
}

// ============================================================================
// Handlers — Class 5B: byte/halfword register-register sign/zero extends
// ============================================================================

void Cpu::h_ld_b_rd_rs2(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    cpu.state.r[i.rd()] = static_cast<uint32_t>(static_cast<int8_t>(cpu.state.r[i.rs()]));
}
void Cpu::h_ld_ub_rd_rs2(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    cpu.state.r[i.rd()] = cpu.state.r[i.rs()] & 0xFF;
}
void Cpu::h_ld_h_rd_rs2(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    cpu.state.r[i.rd()] = static_cast<uint32_t>(static_cast<int16_t>(cpu.state.r[i.rs()]));
}
void Cpu::h_ld_uh_rd_rs2(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    cpu.state.r[i.rd()] = cpu.state.r[i.rs()] & 0xFFFF;
}

// ============================================================================
// Handlers — Class 5C: multiply / MAC
// ============================================================================

void Cpu::h_mlt_h(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    int32_t a = static_cast<int16_t>(cpu.state.r[i.rd()]);
    int32_t b = static_cast<int16_t>(cpu.state.r[i.rs()]);
    int64_t res = int64_t(a) * b;
    cpu.state.alr = static_cast<uint32_t>(res);
    cpu.state.ahr = static_cast<uint32_t>(res >> 32);
}
void Cpu::h_mltu_h(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    uint32_t a = cpu.state.r[i.rd()] & 0xFFFF;
    uint32_t b = cpu.state.r[i.rs()] & 0xFFFF;
    uint64_t res = uint64_t(a) * b;
    cpu.state.alr = static_cast<uint32_t>(res);
    cpu.state.ahr = static_cast<uint32_t>(res >> 32);
}
void Cpu::h_mlt_w(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    int64_t a = static_cast<int32_t>(cpu.state.r[i.rd()]);
    int64_t b = static_cast<int32_t>(cpu.state.r[i.rs()]);
    int64_t res = a * b;
    cpu.state.alr = static_cast<uint32_t>(res);
    cpu.state.ahr = static_cast<uint32_t>(res >> 32);
}
void Cpu::h_mltu_w(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    uint64_t a = cpu.state.r[i.rd()];
    uint64_t b = cpu.state.r[i.rs()];
    uint64_t res = a * b;
    cpu.state.alr = static_cast<uint32_t>(res);
    cpu.state.ahr = static_cast<uint32_t>(res >> 32);
}
void Cpu::h_mac(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    int rs_n = Insn{insn}.rs();        // count register
    int r1_n = (rs_n + 1) & 0xF;      // <rs+1>: pointer to first operand
    int r2_n = (rs_n + 2) & 0xF;      // <rs+2>: pointer to second operand

    uint32_t count = cpu.state.r[rs_n];
    if (count == 0) return;            // rs=0: no operation

    // Reconstruct signed 64-bit accumulator from {AHR, ALR}
    uint64_t uacc = (uint64_t(cpu.state.ahr) << 32) | cpu.state.alr;

    for (uint32_t k = count; k > 0; k--) {
        int16_t h1 = static_cast<int16_t>(cpu.bus().read16(cpu.state.r[r1_n]));
        int16_t h2 = static_cast<int16_t>(cpu.bus().read16(cpu.state.r[r2_n]));
        int64_t prod = int64_t(h1) * int64_t(h2);

        uint64_t uprod = static_cast<uint64_t>(prod);
        uint64_t usum  = uacc + uprod;

        // MO is sticky: once set it stays set until software clears it
        if (!cpu.state.psr.mo()) {
            // Signed 64-bit overflow: same-sign inputs, different-sign output
            bool ov = (~(uacc ^ uprod) & (uacc ^ usum)) >> 63;
            if (ov) cpu.state.psr.set_mo(true);
        }
        uacc = usum;

        cpu.state.r[r1_n] += 2;
        cpu.state.r[r2_n] += 2;
        cpu.state.r[rs_n]--;
    }

    cpu.state.alr = static_cast<uint32_t>(uacc);
    cpu.state.ahr = static_cast<uint32_t>(uacc >> 32);
}

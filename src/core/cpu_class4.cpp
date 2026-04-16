#include "cpu_impl.hpp"
#include <bit>

// ============================================================================
// Handlers — Class 4A: SP adjust
// ============================================================================

void Cpu::h_add_sp_imm10(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();  // ext not allowed on SP-adjust (imm10 is in words)
    cpu.state.sp += Insn{insn}.imm10() * 4;
}
void Cpu::h_sub_sp_imm10(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();  // ext not allowed on SP-adjust (imm10 is in words)
    cpu.state.sp -= Insn{insn}.imm10() * 4;
}

// ============================================================================
// Handlers — Class 4B: immediate shifts/rotates
// ============================================================================

void Cpu::h_srl_rd_imm4(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    int n = i.shift_amt();
    uint32_t v = cpu.state.r[i.rd()];
    bool carry = n > 0 ? (v >> (n-1)) & 1 : false;
    cpu.state.r[i.rd()] = v >> n;
    cpu.state.psr.set_nz(v >> n);
    if (n > 0) cpu.state.psr.set_c(carry);
}
void Cpu::h_sll_rd_imm4(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    int n = i.shift_amt();
    uint32_t v = cpu.state.r[i.rd()];
    bool carry = n > 0 ? (v >> (32 - n)) & 1 : false;
    cpu.state.r[i.rd()] = v << n;
    cpu.state.psr.set_nz(v << n);
    if (n > 0) cpu.state.psr.set_c(carry);
}
void Cpu::h_sra_rd_imm4(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    int n = i.shift_amt();
    int32_t v = static_cast<int32_t>(cpu.state.r[i.rd()]);
    bool carry = n > 0 ? (static_cast<uint32_t>(v) >> (n-1)) & 1 : false;
    int32_t r = v >> n;
    cpu.state.r[i.rd()] = static_cast<uint32_t>(r);
    cpu.state.psr.set_nz(static_cast<uint32_t>(r));
    if (n > 0) cpu.state.psr.set_c(carry);
}
void Cpu::h_sla_rd_imm4(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    int n = i.shift_amt();
    uint32_t v = cpu.state.r[i.rd()];
    bool carry = n > 0 ? (v >> (32 - n)) & 1 : false;
    uint32_t r = v << n;
    cpu.state.r[i.rd()] = r;
    cpu.state.psr.set_nz(r);
    if (n > 0) {
        cpu.state.psr.set_c(carry);
        // V = 1 if sign bit changed (arithmetic overflow)
        cpu.state.psr.set_v((v >> 31) != (r >> 31));
    }
}
void Cpu::h_rr_rd_imm4(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    int n = i.shift_amt();
    uint32_t v = cpu.state.r[i.rd()];
    uint32_t r = std::rotr(v, n);
    bool carry = (v >> (n - 1)) & 1;
    cpu.state.r[i.rd()] = r;
    cpu.state.psr.set_nz(r);
    cpu.state.psr.set_c(carry);
}
void Cpu::h_rl_rd_imm4(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    int n = i.shift_amt();
    uint32_t v = cpu.state.r[i.rd()];
    uint32_t r = std::rotl(v, n);
    bool carry = (v >> (32 - n)) & 1;
    cpu.state.r[i.rd()] = r;
    cpu.state.psr.set_nz(r);
    cpu.state.psr.set_c(carry);
}

// ============================================================================
// Handlers — Class 4C: register shifts/rotates
// ============================================================================

void Cpu::h_srl_rd_rs(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    int n = cpu.state.r[i.rs()] & 0x1F;
    uint32_t v = cpu.state.r[i.rd()];
    bool carry = n > 0 ? (v >> (n-1)) & 1 : false;
    cpu.state.r[i.rd()] = v >> n;
    cpu.state.psr.set_nz(v >> n);
    if (n > 0) cpu.state.psr.set_c(carry);
}
void Cpu::h_sll_rd_rs(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    int n = cpu.state.r[i.rs()] & 0x1F;
    uint32_t v = cpu.state.r[i.rd()];
    bool carry = n > 0 ? (v >> (32 - n)) & 1 : false;
    cpu.state.r[i.rd()] = v << n;
    cpu.state.psr.set_nz(v << n);
    if (n > 0) cpu.state.psr.set_c(carry);
}
void Cpu::h_sra_rd_rs(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    int n = cpu.state.r[i.rs()] & 0x1F;
    int32_t v = static_cast<int32_t>(cpu.state.r[i.rd()]);
    bool carry = n > 0 ? (static_cast<uint32_t>(v) >> (n-1)) & 1 : false;
    int32_t r = v >> n;
    cpu.state.r[i.rd()] = static_cast<uint32_t>(r);
    cpu.state.psr.set_nz(static_cast<uint32_t>(r));
    if (n > 0) cpu.state.psr.set_c(carry);
}
void Cpu::h_sla_rd_rs(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    int n = cpu.state.r[i.rs()] & 0x1F;
    uint32_t v = cpu.state.r[i.rd()];
    bool carry = n > 0 ? (v >> (32 - n)) & 1 : false;
    cpu.state.r[i.rd()] = v << n;
    cpu.state.psr.set_nz(v << n);
    if (n > 0) cpu.state.psr.set_c(carry);
}
void Cpu::h_rr_rd_rs(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    int n = cpu.state.r[i.rs()] & 0x1F;
    uint32_t v = cpu.state.r[i.rd()];
    uint32_t r = n ? std::rotr(v, n) : v;
    bool carry = n ? (v >> (n - 1)) & 1 : false;
    cpu.state.r[i.rd()] = r;
    cpu.state.psr.set_nz(r);
    if (n) cpu.state.psr.set_c(carry);
}
void Cpu::h_rl_rd_rs(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    int n = cpu.state.r[i.rs()] & 0x1F;
    uint32_t v = cpu.state.r[i.rd()];
    uint32_t r = n ? std::rotl(v, n) : v;
    bool carry = n ? (v >> (32 - n)) & 1 : false;
    cpu.state.r[i.rd()] = r;
    cpu.state.psr.set_nz(r);
    if (n) cpu.state.psr.set_c(carry);
}

// ============================================================================
// Handlers — Class 4D: scan/swap/mirror
// ============================================================================

void Cpu::h_scan0(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    // Scan upper 8 bits of rs for the first 0 bit, MSB first.
    uint32_t top = cpu.state.r[i.rs()] >> 24;
    int pos = 0;
    while (pos < 8 && ((top >> (7 - pos)) & 1)) pos++;
    cpu.state.r[i.rd()] = pos;          // 0..7 = offset, 8 = not found
    cpu.state.psr.set_z(pos == 0);
    cpu.state.psr.set_c(pos == 8);        // C=1 when no 0 found
    cpu.state.psr.set_v(false);
    cpu.state.psr.set_n(false);
}
void Cpu::h_scan1(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    // Scan upper 8 bits of rs for the first 1 bit, MSB first.
    uint32_t top = cpu.state.r[i.rs()] >> 24;
    int pos = 0;
    while (pos < 8 && !((top >> (7 - pos)) & 1)) pos++;
    cpu.state.r[i.rd()] = pos;
    cpu.state.psr.set_z(pos == 0);
    cpu.state.psr.set_c(pos == 8);        // C=1 when no 1 found
    cpu.state.psr.set_v(false);
    cpu.state.psr.set_n(false);
}
void Cpu::h_swap(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    uint32_t v = cpu.state.r[i.rs()];
    // Byte reversal: rd(31:24)←rs(7:0), rd(23:16)←rs(15:8), ...
    cpu.state.r[i.rd()] = ((v & 0x000000FF) << 24) | ((v & 0x0000FF00) << 8)
                         | ((v & 0x00FF0000) >> 8)  | ((v & 0xFF000000) >> 24);
}
void Cpu::h_mirror(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    cpu.flush_ext();
    uint32_t v = cpu.state.r[i.rs()];
    // Bit reversal within each byte: rd(31:24)←rs(24:31), ...
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    cpu.state.r[i.rd()] = v;
}

// ============================================================================
// Handlers — Class 4D: division steps
// ============================================================================

// Zero-divide trap vector (base + 16, trap number = 4).  Level is ignored by
// do_trap() for traps < 16 (it bypasses the IE/IL gating).
static constexpr int kTrapZeroDiv = 4;

void Cpu::h_div0s(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    uint32_t divisor = cpu.state.r[Insn{insn}.rs()];
    if (divisor == 0) {
        cpu.assert_trap(kTrapZeroDiv, 0);
        return;
    }
    bool neg_dividend = (cpu.state.alr >> 31) & 1;
    cpu.state.ahr = neg_dividend ? 0xFFFFFFFFu : 0u;
    cpu.state.psr.set_ds(neg_dividend);
    cpu.state.psr.set_n((divisor >> 31) & 1);
}
void Cpu::h_div0u(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    uint32_t divisor = cpu.state.r[Insn{insn}.rs()];
    if (divisor == 0) {
        cpu.assert_trap(kTrapZeroDiv, 0);
        return;
    }
    cpu.state.ahr = 0;
    cpu.state.psr.set_ds(false);
    cpu.state.psr.set_n(false);
}
void Cpu::h_div1(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    uint32_t divisor = cpu.state.r[Insn{insn}.rs()];
    // {AHR, ALR} <<= 1 as a 64-bit value (MSB of ALR shifts into LSB of AHR).
    cpu.state.ahr = (cpu.state.ahr << 1) | (cpu.state.alr >> 31);
    cpu.state.alr <<= 1;
    uint32_t ahr = cpu.state.ahr;
    uint32_t tmp;
    bool q_bit;
    bool ds = cpu.state.psr.ds();
    bool n  = cpu.state.psr.n();
    if (!ds) {
        if (!n) {                   // +dividend / +divisor
            tmp = ahr - divisor;
            q_bit = (tmp <= ahr);   // no borrow (unsigned compare)
        } else {                    // +dividend / -divisor
            tmp = ahr + divisor;
            q_bit = (tmp < ahr);    // carry out (unsigned compare)
        }
    } else {
        if (!n) {                   // -dividend / +divisor
            tmp = ahr + divisor;
            q_bit = (tmp >= ahr);   // no carry (unsigned compare)
        } else {                    // -dividend / -divisor
            tmp = ahr - divisor;
            q_bit = (tmp > ahr);    // borrow (unsigned compare)
        }
    }
    if (q_bit) {
        cpu.state.ahr = tmp;
        cpu.state.alr |= 1u;
    }
}
void Cpu::h_div2s(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    if (!cpu.state.psr.ds()) return;    // only applies when dividend was negative
    uint32_t divisor = cpu.state.r[Insn{insn}.rs()];
    uint32_t tmp = cpu.state.psr.n() ? (cpu.state.ahr - divisor)
                                     : (cpu.state.ahr + divisor);
    if (tmp == 0) {
        cpu.state.ahr = 0;
        cpu.state.alr += 1;
    }
}
void Cpu::h_div3s(Cpu& cpu, uint16_t) {
    cpu.flush_ext();
    // If dividend and divisor had opposite signs, negate the quotient.
    if (cpu.state.psr.ds() != cpu.state.psr.n())
        cpu.state.alr = 0u - cpu.state.alr;
}

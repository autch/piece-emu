#include "cpu_impl.hpp"
#include <bit>

// CLASS_4B/4C: rd = bits[3:0], imm4_rs = bits[7:4]
static inline int c4_rd  (uint16_t i) { return i & 0xF; }
static inline int c4_rs_n(uint16_t i) { return (i >> 4) & 0xF; }

// CLASS_4A: imm10 = bits[9:0]
static inline uint32_t c4a_imm10(uint16_t i) { return i & 0x3FF; }

// ============================================================================
// Handlers — Class 4A: SP adjust
// ============================================================================

void Cpu::h_add_sp_imm10(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();  // ext not allowed on SP-adjust (imm10 is in words)
    cpu.state.sp += c4a_imm10(insn) * 4;
}
void Cpu::h_sub_sp_imm10(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();  // ext not allowed on SP-adjust (imm10 is in words)
    cpu.state.sp -= c4a_imm10(insn) * 4;
}

// ============================================================================
// Handlers — Class 4B: immediate shifts/rotates
// ============================================================================

void Cpu::h_srl_rd_imm4(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    int n = decode_shift_imm4(insn);
    uint32_t v = cpu.state.r[c4_rd(insn)];
    bool carry = n > 0 ? (v >> (n-1)) & 1 : false;
    cpu.state.r[c4_rd(insn)] = v >> n;
    cpu.state.psr.set_nz(v >> n);
    if (n > 0) cpu.state.psr.set_c(carry);
}
void Cpu::h_sll_rd_imm4(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    int n = decode_shift_imm4(insn);
    uint32_t v = cpu.state.r[c4_rd(insn)];
    bool carry = n > 0 ? (v >> (32 - n)) & 1 : false;
    cpu.state.r[c4_rd(insn)] = v << n;
    cpu.state.psr.set_nz(v << n);
    if (n > 0) cpu.state.psr.set_c(carry);
}
void Cpu::h_sra_rd_imm4(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    int n = decode_shift_imm4(insn);
    int32_t v = static_cast<int32_t>(cpu.state.r[c4_rd(insn)]);
    bool carry = n > 0 ? (static_cast<uint32_t>(v) >> (n-1)) & 1 : false;
    int32_t r = v >> n;
    cpu.state.r[c4_rd(insn)] = static_cast<uint32_t>(r);
    cpu.state.psr.set_nz(static_cast<uint32_t>(r));
    if (n > 0) cpu.state.psr.set_c(carry);
}
void Cpu::h_sla_rd_imm4(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    int n = decode_shift_imm4(insn);
    uint32_t v = cpu.state.r[c4_rd(insn)];
    bool carry = n > 0 ? (v >> (32 - n)) & 1 : false;
    uint32_t r = v << n;
    cpu.state.r[c4_rd(insn)] = r;
    cpu.state.psr.set_nz(r);
    if (n > 0) {
        cpu.state.psr.set_c(carry);
        // V = 1 if sign bit changed (arithmetic overflow)
        cpu.state.psr.set_v((v >> 31) != (r >> 31));
    }
}
void Cpu::h_rr_rd_imm4(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    int n = decode_shift_imm4(insn);
    uint32_t v = cpu.state.r[c4_rd(insn)];
    uint32_t r = std::rotr(v, n);
    bool carry = (v >> (n - 1)) & 1;
    cpu.state.r[c4_rd(insn)] = r;
    cpu.state.psr.set_nz(r);
    cpu.state.psr.set_c(carry);
}
void Cpu::h_rl_rd_imm4(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    int n = decode_shift_imm4(insn);
    uint32_t v = cpu.state.r[c4_rd(insn)];
    uint32_t r = std::rotl(v, n);
    bool carry = (v >> (32 - n)) & 1;
    cpu.state.r[c4_rd(insn)] = r;
    cpu.state.psr.set_nz(r);
    cpu.state.psr.set_c(carry);
}

// ============================================================================
// Handlers — Class 4C: register shifts/rotates
// ============================================================================

void Cpu::h_srl_rd_rs(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    int n = cpu.state.r[c4_rs_n(insn)] & 0x1F;
    uint32_t v = cpu.state.r[c4_rd(insn)];
    bool carry = n > 0 ? (v >> (n-1)) & 1 : false;
    cpu.state.r[c4_rd(insn)] = v >> n;
    cpu.state.psr.set_nz(v >> n);
    if (n > 0) cpu.state.psr.set_c(carry);
}
void Cpu::h_sll_rd_rs(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    int n = cpu.state.r[c4_rs_n(insn)] & 0x1F;
    uint32_t v = cpu.state.r[c4_rd(insn)];
    bool carry = n > 0 ? (v >> (32 - n)) & 1 : false;
    cpu.state.r[c4_rd(insn)] = v << n;
    cpu.state.psr.set_nz(v << n);
    if (n > 0) cpu.state.psr.set_c(carry);
}
void Cpu::h_sra_rd_rs(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    int n = cpu.state.r[c4_rs_n(insn)] & 0x1F;
    int32_t v = static_cast<int32_t>(cpu.state.r[c4_rd(insn)]);
    bool carry = n > 0 ? (static_cast<uint32_t>(v) >> (n-1)) & 1 : false;
    int32_t r = v >> n;
    cpu.state.r[c4_rd(insn)] = static_cast<uint32_t>(r);
    cpu.state.psr.set_nz(static_cast<uint32_t>(r));
    if (n > 0) cpu.state.psr.set_c(carry);
}
void Cpu::h_sla_rd_rs(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    int n = cpu.state.r[c4_rs_n(insn)] & 0x1F;
    uint32_t v = cpu.state.r[c4_rd(insn)];
    bool carry = n > 0 ? (v >> (32 - n)) & 1 : false;
    cpu.state.r[c4_rd(insn)] = v << n;
    cpu.state.psr.set_nz(v << n);
    if (n > 0) cpu.state.psr.set_c(carry);
}
void Cpu::h_rr_rd_rs(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    int n = cpu.state.r[c4_rs_n(insn)] & 0x1F;
    uint32_t v = cpu.state.r[c4_rd(insn)];
    uint32_t r = n ? std::rotr(v, n) : v;
    bool carry = n ? (v >> (n - 1)) & 1 : false;
    cpu.state.r[c4_rd(insn)] = r;
    cpu.state.psr.set_nz(r);
    if (n) cpu.state.psr.set_c(carry);
}
void Cpu::h_rl_rd_rs(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    int n = cpu.state.r[c4_rs_n(insn)] & 0x1F;
    uint32_t v = cpu.state.r[c4_rd(insn)];
    uint32_t r = n ? std::rotl(v, n) : v;
    bool carry = n ? (v >> (32 - n)) & 1 : false;
    cpu.state.r[c4_rd(insn)] = r;
    cpu.state.psr.set_nz(r);
    if (n) cpu.state.psr.set_c(carry);
}

// ============================================================================
// Handlers — Class 4D: scan/swap/mirror
// ============================================================================

void Cpu::h_scan0(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    // Scan upper 8 bits of rs for the first 0 bit, MSB first.
    uint32_t top = cpu.state.r[rs(insn)] >> 24;
    int pos = 0;
    while (pos < 8 && ((top >> (7 - pos)) & 1)) pos++;
    cpu.state.r[rd(insn)] = pos;          // 0..7 = offset, 8 = not found
    cpu.state.psr.set_z(pos == 0);
    cpu.state.psr.set_c(pos == 8);        // C=1 when no 0 found
    cpu.state.psr.set_v(false);
    cpu.state.psr.set_n(false);
}
void Cpu::h_scan1(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    // Scan upper 8 bits of rs for the first 1 bit, MSB first.
    uint32_t top = cpu.state.r[rs(insn)] >> 24;
    int pos = 0;
    while (pos < 8 && !((top >> (7 - pos)) & 1)) pos++;
    cpu.state.r[rd(insn)] = pos;
    cpu.state.psr.set_z(pos == 0);
    cpu.state.psr.set_c(pos == 8);        // C=1 when no 1 found
    cpu.state.psr.set_v(false);
    cpu.state.psr.set_n(false);
}
void Cpu::h_swap(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    uint32_t v = cpu.state.r[rs(insn)];
    // Byte reversal: rd(31:24)←rs(7:0), rd(23:16)←rs(15:8), ...
    cpu.state.r[rd(insn)] = ((v & 0x000000FF) << 24) | ((v & 0x0000FF00) << 8)
                           | ((v & 0x00FF0000) >> 8)  | ((v & 0xFF000000) >> 24);
}
void Cpu::h_mirror(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    uint32_t v = cpu.state.r[rs(insn)];
    // Bit reversal within each byte: rd(31:24)←rs(24:31), ...
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    cpu.state.r[rd(insn)] = v;
}

// ============================================================================
// Handlers — Class 4D: division steps
// ============================================================================

void Cpu::h_div0s(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    // AHR = sign extension of ALR (all-1s if negative, all-0s if positive)
    bool neg_dividend = (cpu.state.alr >> 31) & 1;
    cpu.state.ahr = neg_dividend ? 0xFFFFFFFFu : 0u;
    // DS = sign of dividend (ALR), N = sign of divisor (rs)
    cpu.state.psr.set_ds(neg_dividend);
    cpu.state.psr.set_n((cpu.state.r[rs(insn)] >> 31) & 1);
    // C is unchanged
}
void Cpu::h_div0u(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    cpu.state.ahr = 0;
    cpu.state.psr.set_ds(false);
    cpu.state.psr.set_n(false);
    // C is unchanged
    (void)rs(insn);
}
void Cpu::h_div1(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    uint32_t divisor = cpu.state.r[rs(insn)];
    bool c = cpu.state.psr.c();
    uint64_t rem = (uint64_t(cpu.state.ahr) << 32) | cpu.state.alr;
    rem = (rem << 1) | (c ? 1 : 0);
    uint32_t hi = uint32_t(rem >> 32);
    bool sub_ok = hi >= divisor;
    if (sub_ok) hi -= divisor;
    cpu.state.ahr = hi;
    cpu.state.alr = (uint32_t(rem) & ~1u) | (sub_ok ? 1 : 0);
    cpu.state.psr.set_c(sub_ok);
}
void Cpu::h_div2s(Cpu& cpu, uint16_t insn) {
    cpu.flush_ext();
    uint32_t divisor = cpu.state.r[rs(insn)];
    if (!cpu.state.psr.c())
        cpu.state.ahr += divisor;
}
void Cpu::h_div3s(Cpu& cpu, uint16_t) {
    cpu.flush_ext();
    if (static_cast<int32_t>(cpu.state.ahr) < 0)
        cpu.state.alr--;
}

/**
 * This header defines functions to generate 16-bit instruction encodings for the C33 architecture.
 */

#pragma once

#include <cstdint>

namespace c33
{
    constexpr uint32_t sign_ext(uint32_t v, int bits) {
        if (bits >= 32) return v;
        uint32_t sign = 1u << (bits - 1);
        return (v ^ sign) - sign;
    }


    constexpr uint16_t c33_class(uint8_t cls)
    {
        return (cls & 0b111) << 13;
    }

    // CLASS 0

    // single operands
    constexpr uint16_t c33_c0a(uint8_t op1, uint8_t d, uint8_t op2, uint8_t imm2_rd_rs)
    {
        return c33_class(0) | ((op1 & 0b1111) << 9) | ((d & 1) << 8) | ((op2 & 0b11) << 6) | (imm2_rd_rs & 0b1111);
    }

    constexpr uint16_t c33_nop() { return c33_c0a(0b0000, 0b00, 0, 0); }
    constexpr uint16_t c33_slp() { return c33_c0a(0b0000, 0b01, 0, 0); }
    constexpr uint16_t c33_halt() { return c33_c0a(0b0000, 0b10, 0, 0); }
    constexpr uint16_t c33_pushn(uint8_t rs) { return c33_c0a(0b0001, 0b00, 0, rs); }
    constexpr uint16_t c33_popn(uint8_t rd) { return c33_c0a(0b0001, 0b01, 0, rd); }
    constexpr uint16_t c33_brk() { return c33_c0a(0b0010, 0b00, 0, 0); }
    constexpr uint16_t c33_retd() { return c33_c0a(0b0010, 0b01, 0, 0); }
    constexpr uint16_t c33_int(uint8_t imm2) { return c33_c0a(0b0010, 0b10, 0, imm2 & 0b11); }
    constexpr uint16_t c33_reti() { return c33_c0a(0b0010, 0b11, 0, 0); }
    constexpr uint16_t c33_call(uint8_t rb, uint8_t d) { return c33_c0a(0b0011, 0b00, d, rb); }
    constexpr uint16_t c33_ret(uint8_t d) { return c33_c0a(0b0011, 0b01, d, 0); }
    constexpr uint16_t c33_jp_rb(uint8_t rb, uint8_t d) { return c33_c0a(0b0011, 0b10, d, rb); }

    // PC-relative (un)conditional branches, call
    constexpr uint16_t c33_c0b(uint8_t op1, uint8_t d, int8_t sign8)
    {
        return c33_class(0) | ((op1 & 0b1111) << 9) | ((d & 1) << 8) | (sign8 & 0xFF);
    }

    // jr** sign8, call sign8, jp sign8
    constexpr uint16_t c33_jrgt_sign8(int8_t sign8, uint8_t d) { return c33_c0b(0b0100, d, sign8); }
    constexpr uint16_t c33_jrge_sign8(int8_t sign8, uint8_t d) { return c33_c0b(0b0101, d, sign8); }
    constexpr uint16_t c33_jrlt_sign8(int8_t sign8, uint8_t d) { return c33_c0b(0b0110, d, sign8); }
    constexpr uint16_t c33_jrle_sign8(int8_t sign8, uint8_t d) { return c33_c0b(0b0111, d, sign8); }
    constexpr uint16_t c33_jrugt_sign8(int8_t sign8, uint8_t d) { return c33_c0b(0b1000, d, sign8); }
    constexpr uint16_t c33_jruge_sign8(int8_t sign8, uint8_t d) { return c33_c0b(0b1001, d, sign8); }
    constexpr uint16_t c33_jrult_sign8(int8_t sign8, uint8_t d) { return c33_c0b(0b1010, d, sign8); }
    constexpr uint16_t c33_jrule_sign8(int8_t sign8, uint8_t d) { return c33_c0b(0b1011, d, sign8); }
    constexpr uint16_t c33_jreq_sign8(int8_t sign8, uint8_t d) { return c33_c0b(0b1100, d, sign8); }
    constexpr uint16_t c33_jrne_sign8(int8_t sign8, uint8_t d) { return c33_c0b(0b1101, d, sign8); }
    constexpr uint16_t c33_call_sign8(int8_t sign8, uint8_t d) { return c33_c0b(0b1110, d, sign8); }
    constexpr uint16_t c33_jp_sign8(int8_t sign8, uint8_t d) { return c33_c0b(0b1111, d, sign8); }

    // CLASS 1

    // load, store
    constexpr uint16_t c33_c1a(uint8_t op1, uint8_t op2, uint8_t rb, uint8_t rs_rd)
    {
        return c33_class(1) | ((op1 & 0b111) << 10) | ((op2 & 0b11) << 8) | ((rb & 0b1111) << 4) | (rs_rd & 0b1111);
    }

    // ld.* %rd, [%rb]
    constexpr uint16_t c33_ld_b_rd_ind_rb(uint8_t rd, uint8_t rb) { return c33_c1a(0b000, 0b00, rb, rd); }
    constexpr uint16_t c33_ld_ub_rd_ind_rb(uint8_t rd, uint8_t rb) { return c33_c1a(0b001, 0b00, rb, rd); }
    constexpr uint16_t c33_ld_h_rd_ind_rb(uint8_t rd, uint8_t rb) { return c33_c1a(0b010, 0b00, rb, rd); }
    constexpr uint16_t c33_ld_uh_rd_ind_rb(uint8_t rd, uint8_t rb) { return c33_c1a(0b011, 0b00, rb, rd); }
    constexpr uint16_t c33_ld_w_rd_ind_rb(uint8_t rd, uint8_t rb) { return c33_c1a(0b100, 0b00, rb, rd); }

    // ld.* [%rb], %rs
    constexpr uint16_t c33_ld_b_ind_rb_rs(uint8_t rb, uint8_t rs) { return c33_c1a(0b101, 0b00, rb, rs); }
    constexpr uint16_t c33_ld_h_ind_rb_rs(uint8_t rb, uint8_t rs) { return c33_c1a(0b110, 0b00, rb, rs); }
    constexpr uint16_t c33_ld_w_ind_rb_rs(uint8_t rb, uint8_t rs) { return c33_c1a(0b111, 0b00, rb, rs); }

    // ld.* %rd, [%rb]+
    constexpr uint16_t c33_ld_b_rd_ind_rb_PI(uint8_t rd, uint8_t rb) { return c33_c1a(0b000, 0b01, rb, rd); }
    constexpr uint16_t c33_ld_ub_rd_ind_rb_PI(uint8_t rd, uint8_t rb) { return c33_c1a(0b001, 0b01, rb, rd); }
    constexpr uint16_t c33_ld_h_rd_ind_rb_PI(uint8_t rd, uint8_t rb) { return c33_c1a(0b010, 0b01, rb, rd); }
    constexpr uint16_t c33_ld_uh_rd_ind_rb_PI(uint8_t rd, uint8_t rb) { return c33_c1a(0b011, 0b01, rb, rd); }
    constexpr uint16_t c33_ld_w_rd_ind_rb_PI(uint8_t rd, uint8_t rb) { return c33_c1a(0b100, 0b01, rb, rd); }

    // ld.* [%rb]+, %rs
    constexpr uint16_t c33_ld_b_ind_rb_PI_rs(uint8_t rb, uint8_t rs) { return c33_c1a(0b101, 0b01, rb, rs); }
    constexpr uint16_t c33_ld_h_ind_rb_PI_rs(uint8_t rb, uint8_t rs) { return c33_c1a(0b110, 0b01, rb, rs); }
    constexpr uint16_t c33_ld_w_ind_rb_PI_rs(uint8_t rb, uint8_t rs) { return c33_c1a(0b111, 0b01, rb, rs); }

    // reg-reg ALU
    constexpr uint16_t c33_c1b(uint8_t op1, uint8_t op2, uint8_t rs, uint8_t rd)
    {
        return c33_class(1) | ((op1 & 0b111) << 10) | ((op2 & 0b11) << 8) | ((rs & 0b1111) << 4) | (rd & 0b1111);
    }

    // op %rd, %rs
    constexpr uint16_t c33_add_rd_rs(uint8_t rd, uint8_t rs) { return c33_c1b(0b000, 0b10, rs, rd); }
    constexpr uint16_t c33_sub_rd_rs(uint8_t rd, uint8_t rs) { return c33_c1b(0b001, 0b10, rs, rd); }
    constexpr uint16_t c33_cmp_rd_rs(uint8_t rd, uint8_t rs) { return c33_c1b(0b010, 0b10, rs, rd); }
    constexpr uint16_t c33_ld_w_rd_rs(uint8_t rd, uint8_t rs) { return c33_c1b(0b011, 0b10, rs, rd); }
    constexpr uint16_t c33_and_rd_rs(uint8_t rd, uint8_t rs) { return c33_c1b(0b100, 0b10, rs, rd); }
    constexpr uint16_t c33_or_rd_rs(uint8_t rd, uint8_t rs) { return c33_c1b(0b101, 0b10, rs, rd); }
    constexpr uint16_t c33_xor_rd_rs(uint8_t rd, uint8_t rs) { return c33_c1b(0b110, 0b10, rs, rd); }
    constexpr uint16_t c33_not_rd_rs(uint8_t rd, uint8_t rs) { return c33_c1b(0b111, 0b10, rs, rd); }

    // CLASS 2

    // sp-relative load/store
    constexpr uint16_t c33_c2(uint8_t op1, uint8_t imm6, uint8_t rs_rd)
    {
        return c33_class(2) | ((op1 & 0b111) << 10) | ((imm6 & 0b111111) << 4) | (rs_rd & 0b1111);
    }

    // ld.* %rd, [%sp+imm6]
    constexpr uint16_t c33_ld_b_rd_ind_sp_imm6(uint8_t rd, uint8_t imm6) { return c33_c2(0b000, imm6, rd); }
    constexpr uint16_t c33_ld_ub_rd_ind_sp_imm6(uint8_t rd, uint8_t imm6) { return c33_c2(0b001, imm6, rd); }
    constexpr uint16_t c33_ld_h_rd_ind_sp_imm6(uint8_t rd, uint8_t imm6) { return c33_c2(0b010, imm6, rd); }
    constexpr uint16_t c33_ld_uh_rd_ind_sp_imm6(uint8_t rd, uint8_t imm6) { return c33_c2(0b011, imm6, rd); }
    constexpr uint16_t c33_ld_w_rd_ind_sp_imm6(uint8_t rd, uint8_t imm6) { return c33_c2(0b100, imm6, rd); }
    // ld.* [%sp+imm6], %rs
    constexpr uint16_t c33_ld_b_ind_sp_imm6_rs(uint8_t imm6, uint8_t rs) { return c33_c2(0b101, imm6, rs); }
    constexpr uint16_t c33_ld_h_ind_sp_imm6_rs(uint8_t imm6, uint8_t rs) { return c33_c2(0b110, imm6, rs); }
    constexpr uint16_t c33_ld_w_ind_sp_imm6_rs(uint8_t imm6, uint8_t rs) { return c33_c2(0b111, imm6, rs); }

    // CLASS 3

    // ALU with immediate operand
    constexpr uint16_t c33_c3(uint8_t op1, uint8_t imm6_sign6, uint8_t rd)
    {
        return c33_class(3) | ((op1 & 0b111) << 10) | ((imm6_sign6 & 0b11'1111) << 4) | (rd & 0b1111);
    }

    // op %rd, imm6/sign6
    constexpr uint16_t c33_add_rd_imm6(uint8_t rd, uint8_t imm6) { return c33_c3(0b000, imm6, rd); }
    constexpr uint16_t c33_sub_rd_imm6(uint8_t rd, uint8_t imm6) { return c33_c3(0b001, imm6, rd); }
    constexpr uint16_t c33_cmp_rd_sign6(uint8_t rd, int8_t sign6) { return c33_c3(0b010, sign6, rd); }
    constexpr uint16_t c33_ld_w_rd_sign6(uint8_t rd, int8_t sign6) { return c33_c3(0b011, sign6, rd); }
    constexpr uint16_t c33_and_rd_sign6(uint8_t rd, int8_t sign6) { return c33_c3(0b100, sign6, rd); }
    constexpr uint16_t c33_or_rd_sign6(uint8_t rd, int8_t sign6) { return c33_c3(0b101, sign6, rd); }
    constexpr uint16_t c33_xor_rd_sign6(uint8_t rd, int8_t sign6) { return c33_c3(0b110, sign6, rd); }
    constexpr uint16_t c33_not_rd_sign6(uint8_t rd, int8_t sign6) { return c33_c3(0b111, sign6, rd); }

    // CLASS 4

    // add/sub with immediate
    constexpr uint16_t c33_c4a(uint8_t op1, uint16_t imm10)
    {
        return c33_class(4) | ((op1 & 0b111) << 10) | (imm10 & 0b11'1111'1111);
    }

    // op %sp, imm10
    constexpr uint16_t c33_add_sp_imm10(uint16_t imm10) { return c33_c4a(0b000, imm10); }
    constexpr uint16_t c33_sub_sp_imm10(uint16_t imm10) { return c33_c4a(0b001, imm10); }

    // shift/rotate with immediate or register
    constexpr uint16_t c33_c4b(uint8_t op1, uint8_t op2, uint8_t imm4, uint8_t rd)
    {
        return c33_class(4) | ((op1 & 0b111) << 10) | ((op2 & 0b11) << 8) | ((imm4 & 0b1111) << 4) | (rd & 0b1111);
    }

    // op %rd, imm4 (imm4 <= 8)
    constexpr uint16_t c33_srl_rd_imm4(uint8_t rd, uint8_t imm4) { return c33_c4b(0b010, 0b00, imm4, rd); }
    constexpr uint16_t c33_sll_rd_imm4(uint8_t rd, uint8_t imm4) { return c33_c4b(0b011, 0b00, imm4, rd); }
    constexpr uint16_t c33_sra_rd_imm4(uint8_t rd, uint8_t imm4) { return c33_c4b(0b100, 0b00, imm4, rd); }
    constexpr uint16_t c33_sla_rd_imm4(uint8_t rd, uint8_t imm4) { return c33_c4b(0b101, 0b00, imm4, rd); }
    constexpr uint16_t c33_rr_rd_imm4(uint8_t rd, uint8_t imm4) { return c33_c4b(0b110, 0b00, imm4, rd); }
    constexpr uint16_t c33_rl_rd_imm4(uint8_t rd, uint8_t imm4) { return c33_c4b(0b111, 0b00, imm4, rd); }

    // op %rd, %rs
    constexpr uint16_t c33_srl_rd_rs(uint8_t rd, uint8_t rs) { return c33_c4b(0b010, 0b01, rs, rd); }
    constexpr uint16_t c33_sll_rd_rs(uint8_t rd, uint8_t rs) { return c33_c4b(0b011, 0b01, rs, rd); }
    constexpr uint16_t c33_sra_rd_rs(uint8_t rd, uint8_t rs) { return c33_c4b(0b100, 0b01, rs, rd); }
    constexpr uint16_t c33_sla_rd_rs(uint8_t rd, uint8_t rs) { return c33_c4b(0b101, 0b01, rs, rd); }
    constexpr uint16_t c33_rr_rd_rs(uint8_t rd, uint8_t rs) { return c33_c4b(0b110, 0b01, rs, rd); }
    constexpr uint16_t c33_rl_rd_rs(uint8_t rd, uint8_t rs) { return c33_c4b(0b111, 0b01, rs, rd); }

    // bit operations, step divisions
    constexpr uint16_t c33_c4c(uint8_t op1, uint8_t op2, uint8_t rs, uint8_t rd)
    {
        return c33_class(4) | ((op1 & 0b111) << 10) | ((op2 & 0b11) << 8) | ((rs & 0b1111) << 4) | (rd & 0b1111);
    }

    // op %rd, %rs
    constexpr uint16_t c33_scan0_rd_rs(uint8_t rd, uint8_t rs) { return c33_c4c(0b010, 0b10, rs, rd); }
    constexpr uint16_t c33_scan1_rd_rs(uint8_t rd, uint8_t rs) { return c33_c4c(0b011, 0b10, rs, rd); }
    constexpr uint16_t c33_swap_rd_rs(uint8_t rd, uint8_t rs) { return c33_c4c(0b100, 0b10, rs, rd); }
    constexpr uint16_t c33_mirror_rd_rs(uint8_t rd, uint8_t rs) { return c33_c4c(0b101, 0b10, rs, rd); }

    // division step operations
    constexpr uint16_t c33_div0s_rs(uint8_t rs) { return c33_c4c(0b010, 0b11, rs, 0); }
    constexpr uint16_t c33_div0u_rs(uint8_t rs) { return c33_c4c(0b011, 0b11, rs, 0); }
    constexpr uint16_t c33_div1_rs(uint8_t rs) { return c33_c4c(0b100, 0b11, rs, 0); }
    constexpr uint16_t c33_div2s_rs(uint8_t rs) { return c33_c4c(0b101, 0b11, rs, 0); }
    constexpr uint16_t c33_div3s() { return c33_c4c(0b110, 0b11, 0, 0); }

    // CLASS 5

    // data transfer with special registers
    constexpr uint16_t c33_c5a(uint8_t op1, uint8_t op2, uint8_t rs_ss, uint8_t sd_rd)
    {
        return c33_class(5) | ((op1 & 0b111) << 10) | ((op2 & 0b11) << 8) | ((rs_ss & 0b1111) << 4) | (sd_rd & 0b1111);
    }

    // ss/rs = 0:PSR, 1:SP, 2:ALR, 3:AHR
    constexpr uint16_t c33_ld_w_sd_rs(uint8_t sd, uint8_t rs) { return c33_c5a(0b000, 0b00, rs, sd); }
    constexpr uint16_t c33_ld_w_rd_ss(uint8_t rd, uint8_t ss) { return c33_c5a(0b001, 0b00, ss, rd); }

    // bit test/set/reset operations
    constexpr uint16_t c33_c5b(uint8_t op1, uint8_t op2, uint8_t rb, uint8_t imm3)
    {
        return c33_class(5) | ((op1 & 0b111) << 10) | ((op2 & 0b11) << 8) | ((rb & 0b1111) << 4) | (imm3 & 0b111);
    }

    // bit test/set/reset with bit index in immediate
    constexpr uint16_t c33_btst_ind_rb_imm3(uint8_t rb, uint8_t imm3) { return c33_c5b(0b010, 0b00, rb, imm3); }
    constexpr uint16_t c33_bclr_ind_rb_imm3(uint8_t rb, uint8_t imm3) { return c33_c5b(0b011, 0b00, rb, imm3); }
    constexpr uint16_t c33_bset_ind_rb_imm3(uint8_t rb, uint8_t imm3) { return c33_c5b(0b100, 0b00, rb, imm3); }
    constexpr uint16_t c33_bnot_ind_rb_imm3(uint8_t rb, uint8_t imm3) { return c33_c5b(0b101, 0b00, rb, imm3); }

    // reg-reg ALU, multiply, MAC
    constexpr uint16_t c33_c5c(uint8_t op1, uint8_t op2, uint8_t rs, uint8_t rd)
    {
        return c33_class(5) | ((op1 & 0b111) << 10) | ((op2 & 0b11) << 8) | ((rs & 0b1111) << 4) | (rd & 0b1111);
    }

    // op %rd, %rs
    constexpr uint16_t c33_adc_rd_rs(uint8_t rd, uint8_t rs) { return c33_c5c(0b110, 0b00, rs, rd); }
    constexpr uint16_t c33_sbc_rd_rs(uint8_t rd, uint8_t rs) { return c33_c5c(0b111, 0b00, rs, rd); }
    constexpr uint16_t c33_ld_b_rd_rs(uint8_t rd, uint8_t rs) { return c33_c5c(0b000, 0b01, rs, rd); }
    constexpr uint16_t c33_ld_ub_rd_rs(uint8_t rd, uint8_t rs) { return c33_c5c(0b001, 0b01, rs, rd); }
    constexpr uint16_t c33_ld_h_rd_rs(uint8_t rd, uint8_t rs) { return c33_c5c(0b010, 0b01, rs, rd); }
    constexpr uint16_t c33_ld_uh_rd_rs(uint8_t rd, uint8_t rs) { return c33_c5c(0b011, 0b01, rs, rd); }

    // multiply operations
    constexpr uint16_t c33_mlt_h_rd_rs(uint8_t rd, uint8_t rs) { return c33_c5c(0b000, 0b10, rs, rd); }
    constexpr uint16_t c33_mltu_h_rd_rs(uint8_t rd, uint8_t rs) { return c33_c5c(0b001, 0b10, rs, rd); }
    constexpr uint16_t c33_mlt_w_rd_rs(uint8_t rd, uint8_t rs) { return c33_c5c(0b010, 0b10, rs, rd); }
    constexpr uint16_t c33_mltu_w_rd_rs(uint8_t rd, uint8_t rs) { return c33_c5c(0b011, 0b10, rs, rd); }

    // MAC
    constexpr uint16_t c33_mac_rs(uint8_t rs) { return c33_c5c(0b100, 0b10, rs, 0); }

    // CLASS 6

    constexpr uint16_t c33_c6(uint16_t imm13)
    {
        return c33_class(6) | (imm13 & 0b1'1111'1111'1111);
    }

    // immediate extension
    constexpr uint16_t c33_ext_imm13(uint16_t imm13)
    {
        return c33_c6(imm13);
    }

    // CLASS 7 - RESERVED
    constexpr uint16_t c33_c7()
    {
        return c33_class(7);
    }
}

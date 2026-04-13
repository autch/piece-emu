#include "cpu_impl.hpp"

// ============================================================================
// Dispatch table construction
// ============================================================================

std::array<Cpu::Handler, 65536> Cpu::build_table() {
    std::array<Handler, 65536> t;
    t.fill(h_undef);

    for (uint32_t i = 0; i < 65536; i++) {
        uint16_t insn = static_cast<uint16_t>(i);
        int c = Insn{insn}.cls();

        if (c == 6) { t[i] = h_ext; continue; }

        if (c == 7) { t[i] = h_undef; continue; } // ADV Core only

        if (c == 0) {
            // Class 0 (bits[15:13]=000):
            //   CLASS_0A: op1 = bits[12:9] (4 bits), op2 = bits[7:6] (2 bits), d = bit[8]
            //             op1 values 0..3 → use 0A table indexed [op1][op2]
            //   CLASS_0B: op1 = bits[12:9] (4 bits), d = bit[8], sign8 = bits[7:0]
            //             op1 values 4..15 → branch/call/jump
            int op1_f = (insn >> 9) & 0xF; // bits[12:9]
            int op2_f = (insn >> 6) & 3;   // bits[7:6]
            if (op1_f < 4) {
                // Class 0a
                switch (op1_f) {
                case 0:
                    switch (op2_f) {
                    case 0: t[i] = h_nop;  break;
                    case 1: t[i] = h_slp;  break;
                    case 2: t[i] = h_halt; break;
                    default: break;
                    }
                    break;
                case 1:
                    if (op2_f == 0) t[i] = h_pushn;
                    else if (op2_f == 1) t[i] = h_popn;
                    break;
                case 2:
                    switch (op2_f) {
                    case 0: t[i] = h_brk;  break;
                    case 1: t[i] = h_retd; break;
                    case 2: t[i] = h_int;  break;
                    case 3: t[i] = h_reti; break;
                    }
                    break;
                case 3:
                    switch (op2_f) {
                    case 0: t[i] = h_call_rb; break;
                    case 1: t[i] = h_ret;     break;
                    case 2: t[i] = h_jp_rb;   break;
                    default: break;
                    }
                    break;
                }
            } else {
                // Class 0b: op1 4..15 → branch/call/jump with sign8 offset
                switch (op1_f) {
                case 4:  t[i] = h_jrgt;      break;
                case 5:  t[i] = h_jrge;      break;
                case 6:  t[i] = h_jrlt;      break;
                case 7:  t[i] = h_jrle;      break;
                case 8:  t[i] = h_jrugt;     break;
                case 9:  t[i] = h_jruge;     break;
                case 10: t[i] = h_jrult;     break;
                case 11: t[i] = h_jrule;     break;
                case 12: t[i] = h_jreq;      break;
                case 13: t[i] = h_jrne;      break;
                case 14: t[i] = h_call_simm8; break;
                case 15: t[i] = h_jp_simm8;  break;
                default: break;
                }
            }
            continue;
        }

        if (c == 1) {
            // Class 1 (bits[15:13]=001):
            //   CLASS_1A/1B: op1 = bits[12:10] (3 bits), op2 = bits[9:8] (2 bits)
            //   op2=0: [%rb] no post-inc; op2=1: [%rb+] post-inc; op2=2: reg-reg ALU
            int op2_f = (insn >> 8) & 3;  // bits[9:8]
            int op1_f = (insn >> 10) & 7; // bits[12:10]
            switch (op2_f) {
            case 0: // [%rb]
                switch (op1_f) {
                case 0: t[i] = h_ld_b_rd_rb;  break;
                case 1: t[i] = h_ld_ub_rd_rb; break;
                case 2: t[i] = h_ld_h_rd_rb;  break;
                case 3: t[i] = h_ld_uh_rd_rb; break;
                case 4: t[i] = h_ld_w_rd_rb;  break;
                case 5: t[i] = h_st_b_rb_rs;  break;
                case 6: t[i] = h_st_h_rb_rs;  break;
                case 7: t[i] = h_st_w_rb_rs;  break;
                }
                break;
            case 1: // [%rb+]
                switch (op1_f) {
                case 0: t[i] = h_ld_b_rd_rbx;  break;
                case 1: t[i] = h_ld_ub_rd_rbx; break;
                case 2: t[i] = h_ld_h_rd_rbx;  break;
                case 3: t[i] = h_ld_uh_rd_rbx; break;
                case 4: t[i] = h_ld_w_rd_rbx;  break;
                case 5: t[i] = h_st_b_rbx_rs;  break;
                case 6: t[i] = h_st_h_rbx_rs;  break;
                case 7: t[i] = h_st_w_rbx_rs;  break;
                }
                break;
            case 2: // reg-reg ALU
                switch (op1_f) {
                case 0: t[i] = h_add_rd_rs;  break;
                case 1: t[i] = h_sub_rd_rs;  break;
                case 2: t[i] = h_cmp_rd_rs;  break;
                case 3: t[i] = h_ld_w_rd_rs; break;
                case 4: t[i] = h_and_rd_rs;  break;
                case 5: t[i] = h_or_rd_rs;   break;
                case 6: t[i] = h_xor_rd_rs;  break;
                case 7: t[i] = h_not_rd_rs;  break;
                }
                break;
            case 3: // class 1 op2=3 undefined
                break;
            }
            continue;
        }

        if (c == 2) {
            int op1_f = (insn >> 10) & 7;
            switch (op1_f) {
            case 0: t[i] = h_ld_b_rd_sp;  break;
            case 1: t[i] = h_ld_ub_rd_sp; break;
            case 2: t[i] = h_ld_h_rd_sp;  break;
            case 3: t[i] = h_ld_uh_rd_sp; break;
            case 4: t[i] = h_ld_w_rd_sp;  break;
            case 5: t[i] = h_st_b_sp_rs;  break;
            case 6: t[i] = h_st_h_sp_rs;  break;
            case 7: t[i] = h_st_w_sp_rs;  break;
            }
            continue;
        }

        if (c == 3) {
            int op1_f = (insn >> 10) & 7;
            switch (op1_f) {
            case 0: t[i] = h_add_rd_imm6;   break;
            case 1: t[i] = h_sub_rd_imm6;   break;
            case 2: t[i] = h_cmp_rd_simm6;  break;
            case 3: t[i] = h_ld_w_rd_simm6; break;
            case 4: t[i] = h_and_rd_simm6;  break;
            case 5: t[i] = h_or_rd_simm6;   break;
            case 6: t[i] = h_xor_rd_simm6;  break;
            case 7: t[i] = h_not_rd_simm6;  break;
            }
            continue;
        }

        if (c == 4) {
            int op1_f = (insn >> 10) & 7;
            int op2_f = (insn >>  8) & 3;
            if (op1_f == 0) {
                t[i] = h_add_sp_imm10; continue;
            }
            if (op1_f == 1) {
                t[i] = h_sub_sp_imm10; continue;
            }
            // op1 >= 2: shift/rotate/scan/div
            // op2: 0=imm shift, 1=reg shift, 2=scan/swap, 3=div
            switch (op2_f) {
            case 0: // imm4 shifts: op1[2:0]=010..111
                switch (op1_f) {
                case 2: t[i] = h_srl_rd_imm4; break;
                case 3: t[i] = h_sll_rd_imm4; break;
                case 4: t[i] = h_sra_rd_imm4; break;
                case 5: t[i] = h_sla_rd_imm4; break;
                case 6: t[i] = h_rr_rd_imm4;  break;
                case 7: t[i] = h_rl_rd_imm4;  break;
                }
                break;
            case 1: // reg shifts
                switch (op1_f) {
                case 2: t[i] = h_srl_rd_rs; break;
                case 3: t[i] = h_sll_rd_rs; break;
                case 4: t[i] = h_sra_rd_rs; break;
                case 5: t[i] = h_sla_rd_rs; break;
                case 6: t[i] = h_rr_rd_rs;  break;
                case 7: t[i] = h_rl_rd_rs;  break;
                }
                break;
            case 2: // scan/swap (op1: 010=scan0, 011=scan1, 100=swap, 101=mirror)
                switch (op1_f) {
                case 2: t[i] = h_scan0;  break;
                case 3: t[i] = h_scan1;  break;
                case 4: t[i] = h_swap;   break;
                case 5: t[i] = h_mirror; break;
                }
                break;
            case 3: // div steps
                switch (op1_f) {
                case 2: t[i] = h_div0s; break;
                case 3: t[i] = h_div0u; break;
                case 4: t[i] = h_div1;  break;
                case 5: t[i] = h_div2s; break;
                case 6: t[i] = h_div3s; break;
                }
                break;
            }
            continue;
        }

        if (c == 5) {
            int op1_f = (insn >> 10) & 7;
            int op2_f = (insn >>  8) & 3;
            switch (op2_f) {
            case 0: // special / bit ops / adc / sbc
                switch (op1_f) {
                case 0: t[i] = h_ld_w_sd_rs; break;
                case 1: t[i] = h_ld_w_rd_ss; break;
                case 2: t[i] = h_btst; break;
                case 3: t[i] = h_bclr; break;
                case 4: t[i] = h_bset; break;
                case 5: t[i] = h_bnot; break;
                case 6: t[i] = h_adc;  break;
                case 7: t[i] = h_sbc;  break;
                }
                break;
            case 1: // byte/halfword reg-reg loads
                switch (op1_f) {
                case 0: t[i] = h_ld_b_rd_rs2;  break;
                case 1: t[i] = h_ld_ub_rd_rs2; break;
                case 2: t[i] = h_ld_h_rd_rs2;  break;
                case 3: t[i] = h_ld_uh_rd_rs2; break;
                }
                break;
            case 2: // multiply / MAC
                switch (op1_f) {
                case 0: t[i] = h_mlt_h;  break;
                case 1: t[i] = h_mltu_h; break;
                case 2: t[i] = h_mlt_w;  break;
                case 3: t[i] = h_mltu_w; break;
                case 4: t[i] = h_mac;    break;
                }
                break;
            case 3: break; // undefined
            }
            continue;
        }
    }

    return t;
}

const std::array<Cpu::Handler, 65536> Cpu::dispatch_ = Cpu::build_table();

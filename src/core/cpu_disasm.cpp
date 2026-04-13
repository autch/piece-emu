#include "cpu_impl.hpp"
#include "bus.hpp"
#include <format>
#include <string>

// ============================================================================
// Disassembler
// ============================================================================

std::string Cpu::disasm(uint32_t addr) const {
    uint16_t raw = bus_.fetch16(addr);
    Insn i{raw};

    auto sx = [](uint32_t v, int bits) -> int32_t {
        return static_cast<int32_t>(sign_ext(v, bits));
    };

    std::string mnem;

    switch (i.cls()) {
    case 6: { // EXT
        mnem = std::format("ext\t{}", i.imm13());
        break;
    }
    case 0: { // Class 0a (c0_op1 0..3) / Class 0b (c0_op1 4..15)
        int op1_f = i.c0_op1(); // bits [12:9]
        int op2_f = i.c0_op2(); // bits [7:6]  (class 0a)
        bool d    = i.d();
        int rdn   = i.rd();

        if (op1_f < 4) { // Class 0a
            switch (op1_f) {
            case 0:
                switch (op2_f) {
                case 0: mnem = "nop"; break;
                case 1: mnem = "slp"; break;
                case 2: mnem = "halt"; break;
                default: mnem = "?0a_0_3"; break;
                }
                break;
            case 1:
                switch (op2_f) {
                case 0: mnem = std::format("pushn\t%r{}", rdn); break;
                case 1: mnem = std::format("popn\t%r{}", rdn); break;
                case 3: mnem = std::format("jpr{}\t%r{}", d?".d":"", rdn); break;
                default: mnem = "?0a_1"; break;
                }
                break;
            case 2:
                switch (op2_f) {
                case 0: mnem = "brk"; break;
                case 1: mnem = "retd"; break;
                case 2: mnem = std::format("int\t{}", i.imm2()); break;
                case 3: mnem = "reti"; break;
                }
                break;
            case 3:
                switch (op2_f) {
                case 0: mnem = std::format("call{}\t%r{}", d?".d":"", rdn); break;
                case 1: mnem = std::format("ret{}", d?".d":""); break;
                case 2: mnem = std::format("jp{}\t%r{}", d?".d":"", rdn); break;
                default: mnem = "?0a_3_3"; break;
                }
                break;
            }
        } else { // Class 0b: PC-relative branches/calls
            int32_t off = sx(i.sign8(), 8);
            // Target = insn_addr + 2 * disp  (same as h_jr formula)
            uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(addr) + 2 * off);
            static const char* br_names[12] = {
                "jrgt","jrge","jrlt","jrle","jrugt","jruge","jrult","jrule",
                "jreq","jrne","call","jp"
            };
            int bidx = op1_f - 4; // 0..11
            if (bidx < 12) {
                mnem = std::format("{}{}\t0x{:06X}", br_names[bidx], d?".d":"", target);
            } else {
                mnem = std::format("?0b_{}", op1_f);
            }
        }
        break;
    }
    case 1: { // Class 1: memory ops (o2=0/1) or ALU reg-reg (o2=2)
        int o1  = i.op1();
        int o2  = i.op2();
        int rbn = i.rb();
        int rdn = i.rd();
        if (o2 == 2) { // ALU reg-reg: op1 selects operation
            static const char* alunames[] = {"add","sub","cmp","ld.w","and","or","xor","not"};
            mnem = std::format("{}\t%r{}, %r{}", alunames[o1], rdn, rbn);
        } else { // memory: o2=0 → [%rb], o2=1 → [%rb]+
            const char* pi = (o2 == 1) ? "+" : "";
            if (o1 <= 3) { // loads: b/ub/h/uh
                static const char* ldnames[] = {"ld.b","ld.ub","ld.h","ld.uh"};
                mnem = std::format("{}\t%r{}, [%r{}]{}", ldnames[o1], rdn, rbn, pi);
            } else if (o1 == 4) {
                mnem = std::format("ld.w\t%r{}, [%r{}]{}", rdn, rbn, pi);
            } else if (o1 == 5) {
                mnem = std::format("ld.b\t[%r{}]{}, %r{}", rbn, pi, rdn);
            } else if (o1 == 6) {
                mnem = std::format("ld.h\t[%r{}]{}, %r{}", rbn, pi, rdn);
            } else { // o1 == 7: store word
                mnem = std::format("ld.w\t[%r{}]{}, %r{}", rbn, pi, rdn);
            }
        }
        break;
    }
    case 2: { // Class 2: SP-relative
        int o1   = i.op1();
        int rdrs = i.rd();
        int32_t off6 = sx(i.imm6(), 6);
        static const char* ldst2[] = {"ld.b","ld.ub","ld.h","ld.uh","ld.w","st.b","st.h","st.w"};
        static const int scales2[] = {1, 1, 2, 2, 4, 1, 2, 4};
        if (o1 <= 4) { // loads
            mnem = std::format("{}\t%r{}, [%sp+{}]", ldst2[o1], rdrs, off6 * scales2[o1]);
        } else { // stores
            mnem = std::format("{}\t[%sp+{}], %r{}", ldst2[o1], off6 * scales2[o1], rdrs);
        }
        break;
    }
    case 3: { // Class 3: immediate ALU
        int o1   = i.op1();
        int rdn  = i.rd();
        int32_t simm = sx(i.imm6(), 6);
        static const char* names3[] = {"add","sub","cmp","ld.w","and","or","xor","not"};
        mnem = std::format("{}\t%r{}, {}", names3[o1], rdn, simm);
        break;
    }
    case 4: { // Class 4: SP adjust / shifts / misc
        int o1  = i.op1();
        int o2  = i.op2();
        int rdn = i.rd();
        int rsn = i.rs();
        if (o1 == 0) { // add %sp, imm10
            mnem = std::format("add\t%sp, {}", i.imm10() * 4);
        } else if (o1 == 1) { // sub %sp, imm10
            mnem = std::format("sub\t%sp, {}", i.imm10() * 4);
        } else if (o2 == 0) { // imm4 shifts
            static const char* shnames_imm[] = {"?","?","srl","sll","sra","sla","rr","rl"};
            mnem = std::format("{}\t%r{}, {}", shnames_imm[o1], rdn, rsn);
        } else if (o2 == 1) { // reg shifts
            static const char* shnames_rs[] = {"?","?","srl","sll","sra","sla","rr","rl"};
            mnem = std::format("{}\t%r{}, %r{}", shnames_rs[o1], rdn, rsn);
        } else if (o2 == 2) { // scan/swap/mirror
            static const char* misc2[] = {"?","?","scan0","scan1","swap","mirror","?","?"};
            mnem = std::format("{}\t%r{}, %r{}", misc2[o1], rdn, rsn);
        } else { // o2==3: div steps
            static const char* divnames[] = {"?","?","div0s","div0u","div1","div2s","div3s","?"};
            mnem = std::format("{}\t%r{}", divnames[o1], rsn);
        }
        break;
    }
    case 5: { // Class 5: special regs / bit ops / MAC
        int o1  = i.op1();
        int o2  = i.op2();
        int rdn = i.rd();
        int rsn = i.rs();
        static const char* spreg[] = {"%psr","%sp","%alr","%ahr"};
        if (o1 == 0 && o2 == 0) { // ld.w %special, %rs (write special)
            mnem = std::format("ld.w\t{}, %r{}", spreg[rdn & 3], rsn);
        } else if (o1 == 1 && o2 == 0) { // ld.w %rd, %special (read special)
            mnem = std::format("ld.w\t%r{}, {}", rdn, spreg[rsn & 3]);
        } else if (o1 == 2) { // bit ops [%rb], imm3
            // imm3 is in bits [2:0]; rb is in bits [7:4]
            static const char* bop[] = {"btst","bclr","bset","bnot"};
            mnem = std::format("{}\t[%r{}], {}", bop[o2], rsn, i.imm3());
        } else if (o1 == 3) { // adc/sbc
            mnem = std::format("{}\t%r{}, %r{}", (o2 == 0) ? "adc" : "sbc", rdn, rsn);
        } else if (o1 == 4) { // ld byte/half reg-reg
            static const char* ldnames5[] = {"ld.b","ld.ub","ld.h","ld.uh"};
            mnem = std::format("{}\t%r{}, %r{}", ldnames5[o2], rdn, rsn);
        } else if (o1 == 5) { // multiply
            static const char* macnames[] = {"mlt.h","mltu.h","mlt.w","mltu.w"};
            mnem = std::format("{}\t%r{}, %r{}", macnames[o2], rdn, rsn);
        } else if (o1 == 6 && o2 == 0) { // mac (adc/sbc group, op1=6)
            mnem = std::format("mac\t%r{}, %r{}", rdn, rsn);
        } else {
            mnem = std::format("?5_{}{}", o1, o2);
        }
        break;
    }
    default:
        mnem = std::format("?cls{}", i.cls());
        break;
    }

    return std::format("0x{:06X}: {:04X}  {}", addr, raw, mnem);
}

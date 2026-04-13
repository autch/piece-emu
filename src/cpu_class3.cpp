#include "cpu_impl.hpp"

// ============================================================================
// Handlers — Class 3: immediate ALU  (rd ← rd op imm6, EXT-extended)
//
// do_alu_imm<Op, SignExt>
//   Fetches the immediate operand from imm6, extended by EXT if present.
//   SignExt = false for add/sub (zero-extended); true for all others.
//   Delegates the operation to do_alu<Op> from cpu_impl.hpp.
// ============================================================================

template<AluOp Op, bool SignExt = true>
static void do_alu_imm(Cpu& cpu, uint16_t insn) {
    Insn i{insn};
    uint32_t b;
    if constexpr (SignExt)
        b = static_cast<uint32_t>(cpu.ext_simm(i.imm6(), 6));
    else
        b = cpu.ext_imm(i.imm6(), 6);
    cpu.flush_ext();
    do_alu<Op>(cpu, cpu.state.r[i.rd()], b, i.rd());
}

void Cpu::h_add_rd_imm6  (Cpu& cpu, uint16_t insn) { do_alu_imm<AluOp::ADD, false>(cpu, insn); }
void Cpu::h_sub_rd_imm6  (Cpu& cpu, uint16_t insn) { do_alu_imm<AluOp::SUB, false>(cpu, insn); }
void Cpu::h_cmp_rd_simm6 (Cpu& cpu, uint16_t insn) { do_alu_imm<AluOp::CMP       >(cpu, insn); }
void Cpu::h_ld_w_rd_simm6(Cpu& cpu, uint16_t insn) { do_alu_imm<AluOp::MOV       >(cpu, insn); }
void Cpu::h_and_rd_simm6 (Cpu& cpu, uint16_t insn) { do_alu_imm<AluOp::AND       >(cpu, insn); }
void Cpu::h_or_rd_simm6  (Cpu& cpu, uint16_t insn) { do_alu_imm<AluOp::OR        >(cpu, insn); }
void Cpu::h_xor_rd_simm6 (Cpu& cpu, uint16_t insn) { do_alu_imm<AluOp::XOR       >(cpu, insn); }
void Cpu::h_not_rd_simm6 (Cpu& cpu, uint16_t insn) { do_alu_imm<AluOp::NOT       >(cpu, insn); }

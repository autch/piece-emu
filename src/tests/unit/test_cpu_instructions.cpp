// test_cpu_instructions.cpp — Layer 2: CPU instruction component tests
// Uses real Bus (RAM only) + real Cpu to test instruction execution end-to-end.
// No mocks. The Bus is a real object with IRAM connected; no flash needed.
#include "cpu.hpp"
#include "bus.hpp"
#include <gtest/gtest.h>
#include <initializer_list>
#include <cstdint>

// ============================================================================
// TestFixture: minimal environment (IRAM-only bus + CPU)
// ============================================================================
class CpuInsnFixture : public ::testing::Test {
protected:
    Bus bus;
    Cpu cpu{bus};

    CpuInsnFixture() : bus(0) {} // zero-size flash: IRAM and SRAM only

    // Write a sequence of 16-bit instruction words into IRAM starting at addr 0,
    // set PC to 0, and execute each instruction in order.
    void exec(std::initializer_list<uint16_t> insns) {
        uint8_t* p = bus.iram_ptr();
        uint32_t offset = 0;
        for (uint16_t insn : insns) {
            p[offset]   = insn & 0xFF;
            p[offset+1] = insn >> 8;
            offset += 2;
        }
        cpu.state.pc = 0;
        for (std::size_t i = 0; i < insns.size(); ++i) {
            cpu.step();
        }
    }

    uint32_t reg(int n) const { return cpu.state.r[n]; }
    void set_reg(int n, uint32_t v) { cpu.state.r[n] = v; }
};

// ============================================================================
// Class 3: immediate ALU
// ============================================================================

TEST_F(CpuInsnFixture, LdW_Rd_Simm6_Positive) {
    // ld.w %r0, 10: class=3, op1=3, imm6=10, rd=0
    // insn = (3<<13)|(3<<10)|(10<<4)|0 = 0x6C00|0x00A0 = 0x6CA0
    exec({0x6CA0});
    EXPECT_EQ(reg(0), 10u);
}

TEST_F(CpuInsnFixture, LdW_Rd_Simm6_Negative) {
    // ld.w %r0, -1: imm6=0b111111=63, sign-extended = -1
    // insn = (3<<13)|(3<<10)|(63<<4)|0 = 0x6C00|0x03F0 = 0x6FF0
    exec({0x6FF0});
    EXPECT_EQ(reg(0), 0xFFFFFFFFu);
}

TEST_F(CpuInsnFixture, Add_Rd_Imm6) {
    // add %r0, 5: class=3, op1=0, imm6=5, rd=0
    // insn = (3<<13)|(0<<10)|(5<<4)|0 = 0x6000|0x0050 = 0x6050
    set_reg(0, 10);
    exec({0x6050});
    EXPECT_EQ(reg(0), 15u);
}

TEST_F(CpuInsnFixture, Add_Rd_Imm6_SetsZFlag) {
    // add %r0, 0 with r0=0: result 0, Z should be set
    // insn = (3<<13)|(0<<10)|(0<<4)|0 = 0x6000
    set_reg(0, 0);
    exec({0x6000});
    EXPECT_TRUE(cpu.state.psr.z());
    EXPECT_FALSE(cpu.state.psr.n());
}

TEST_F(CpuInsnFixture, Sub_Rd_Imm6) {
    // sub %r0, 3: class=3, op1=1, imm6=3, rd=0
    // insn = (3<<13)|(1<<10)|(3<<4)|0 = 0x6000|0x0400|0x0030 = 0x6430
    set_reg(0, 10);
    exec({0x6430});
    EXPECT_EQ(reg(0), 7u);
}

TEST_F(CpuInsnFixture, Cmp_Rd_Simm6_Equal) {
    // cmp %r0, 5: class=3, op1=2, imm6=5, rd=0
    // insn = (3<<13)|(2<<10)|(5<<4)|0 = 0x6000|0x0800|0x0050 = 0x6850
    set_reg(0, 5);
    exec({0x6850});
    EXPECT_TRUE(cpu.state.psr.z());  // r0 == 5: equal
    EXPECT_FALSE(cpu.state.psr.n());
}

// ============================================================================
// Class 3 + EXT: extended immediates
// ============================================================================

TEST_F(CpuInsnFixture, ExtAdd_Unsigned) {
    // ext 0x1000 / add %r0, 0:
    //   ext insn: (6<<13)|(0x1000) = 0xC000|0x1000 = 0xD000
    //   add insn: 0x6000 (add %r0, 0)
    //   ext_imm(0, 6) with pending[0]=0x1000 = (0x1000 << 6) | 0 = 0x40000 = 262144
    set_reg(0, 0);
    exec({0xD000, 0x6000});
    EXPECT_EQ(reg(0), 0x40000u);
}

TEST_F(CpuInsnFixture, ExtLdW_SignExtend) {
    // ext 0: pending[0]=0, ext_simm(0, 6): combined=(0<<6)|0=0, sign_ext(0,19)=0
    // Not so interesting. Use ext 0x1000:
    // ext_simm(0, 6): combined = 0x40000, sign_ext(0x40000, 19):
    //   bit18 of 0x40000 = 1 (0x40000 = 0b0_1000_0000_0000_0000_0000)
    //   sign = 1<<18 = 0x40000
    //   result = (0x40000 ^ 0x40000) - 0x40000 = -0x40000 = -262144
    // ld.w insn: (3<<13)|(3<<10)|(0<<4)|0 = 0x6C00
    set_reg(0, 0);
    exec({0xD000, 0x6C00});
    EXPECT_EQ(reg(0), static_cast<uint32_t>(-262144));
}

TEST_F(CpuInsnFixture, TwoExt_LdW) {
    // ext 1 / ext 1 / ld.w %r0, 0:
    //   ext1: 0xC001 (pending[0]=1)
    //   ext2: 0xC001 (pending[1]=1, count=2)
    //   ld.w insn: 0x6C00
    //   ext_simm(0, 6): combined = (1<<19)|(1<<6)|0 = 0x80040
    //   total_bits = 26 + 6 = 32; since >= 32: return as int32_t(0x80040) = 524352
    set_reg(0, 0);
    exec({0xC001, 0xC001, 0x6C00});
    // combined = (1<<(13+6)) | (1<<6) | 0 = (1<<19) | 64 = 524288 + 64 = 524352
    // total_bits = 26 + 6 = 32: truncate to int32_t(524352u) = 524352 (positive, MSB=0)
    EXPECT_EQ(reg(0), 524352u);
}

// ============================================================================
// Class 1C: register-register ALU
// ============================================================================

TEST_F(CpuInsnFixture, Add_Rd_Rs) {
    // add %r0, %r1: class=1, o1=0, o2=2, rb=r1, rd=r0
    // insn = (1<<13)|(0<<10)|(2<<8)|(1<<4)|0 = 0x2000|0x0200|0x0010 = 0x2210
    set_reg(0, 7);
    set_reg(1, 3);
    exec({0x2210});
    EXPECT_EQ(reg(0), 10u);
}

TEST_F(CpuInsnFixture, Sub_Rd_Rs) {
    // sub %r0, %r1: class=1, o1=1, o2=2, rb=r1, rd=r0
    // insn = (1<<13)|(1<<10)|(2<<8)|(1<<4)|0 = 0x2000|0x0400|0x0200|0x0010 = 0x2610
    set_reg(0, 10);
    set_reg(1, 4);
    exec({0x2610});
    EXPECT_EQ(reg(0), 6u);
}

TEST_F(CpuInsnFixture, And_Rd_Rs) {
    // and %r0, %r1: class=1, o1=4, o2=2, rb=r1, rd=r0
    // insn = (1<<13)|(4<<10)|(2<<8)|(1<<4)|0 = 0x2000|0x1000|0x0200|0x0010 = 0x3210
    set_reg(0, 0xFF00);
    set_reg(1, 0x0FF0);
    exec({0x3210});
    EXPECT_EQ(reg(0), 0x0F00u);
}

TEST_F(CpuInsnFixture, Or_Rd_Rs) {
    // or %r0, %r1: class=1, o1=5, o2=2, rb=r1, rd=r0
    // insn = (1<<13)|(5<<10)|(2<<8)|(1<<4)|0 = 0x2000|0x1400|0x0200|0x0010 = 0x3610
    set_reg(0, 0xF0F0);
    set_reg(1, 0x0F0F);
    exec({0x3610});
    EXPECT_EQ(reg(0), 0xFFFFu);
}

TEST_F(CpuInsnFixture, Xor_Rd_Rs) {
    // xor %r0, %r1: class=1, o1=6, o2=2, rb=r1, rd=r0
    // insn = (1<<13)|(6<<10)|(2<<8)|(1<<4)|0 = 0x2000|0x1800|0x0200|0x0010 = 0x3A10
    set_reg(0, 0xAAAA);
    set_reg(1, 0xAAAA);
    exec({0x3A10});
    EXPECT_EQ(reg(0), 0u);
    EXPECT_TRUE(cpu.state.psr.z());
}

// ============================================================================
// Class 1A: register-indirect load/store
// ============================================================================

TEST_F(CpuInsnFixture, LdW_Rd_Rb) {
    // ld.w %r0, [%r1]: class=1, o1=4, o2=0, rb=r1, rd=r0
    // insn = (1<<13)|(4<<10)|(0<<8)|(1<<4)|0 = 0x2000|0x1000|0x0010 = 0x3010
    // Place a 32-bit value at IRAM address 0x0100, point r1 there.
    bus.write32(0x0100, 0xCAFEBABEu);
    set_reg(1, 0x0100);
    exec({0x3010});
    EXPECT_EQ(reg(0), 0xCAFEBABEu);
}

TEST_F(CpuInsnFixture, StW_Rb_Rs) {
    // st.w [%r1], %r0 (stored as ld.w [%rb]+, %rs with o2=0 for store):
    // class=1, o1=7, o2=0, rb=r1, rs=r0
    // insn = (1<<13)|(7<<10)|(0<<8)|(1<<4)|0 = 0x2000|0x1C00|0x0010 = 0x3C10
    set_reg(0, 0x12345678u);
    set_reg(1, 0x0200);
    exec({0x3C10});
    EXPECT_EQ(bus.read32(0x0200), 0x12345678u);
}

TEST_F(CpuInsnFixture, LdB_Rd_Rb_SignExtend) {
    // ld.b %r0, [%r1]: class=1, o1=0, o2=0, rb=r1, rd=r0
    // insn = (1<<13)|(0<<10)|(0<<8)|(1<<4)|0 = 0x2000|0x0010 = 0x2010
    bus.write8(0x0300, 0x80); // -128 when sign-extended
    set_reg(1, 0x0300);
    exec({0x2010});
    EXPECT_EQ(reg(0), 0xFFFFFF80u); // sign-extended -128
}

TEST_F(CpuInsnFixture, LdUb_Rd_Rb_NoSignExtend) {
    // ld.ub %r0, [%r1]: class=1, o1=1, o2=0, rb=r1, rd=r0
    // insn = (1<<13)|(1<<10)|(0<<8)|(1<<4)|0 = 0x2000|0x0400|0x0010 = 0x2410
    bus.write8(0x0300, 0x80);
    set_reg(1, 0x0300);
    exec({0x2410});
    EXPECT_EQ(reg(0), 0x80u); // zero-extended
}

// ============================================================================
// Class 2: SP-relative load/store
// ============================================================================

TEST_F(CpuInsnFixture, LdW_Rd_Sp) {
    // ld.w %r0, [%sp+4]: class=2, o1=4, imm6=1 (×4=4), rd=r0
    // insn = (2<<13)|(4<<10)|(1<<4)|0 = 0x4000|0x1000|0x0010 = 0x5010
    cpu.state.sp = 0x0400;
    bus.write32(0x0404, 0xDEAD1234u);
    exec({0x5010});
    EXPECT_EQ(reg(0), 0xDEAD1234u);
}

TEST_F(CpuInsnFixture, StW_Sp_Rs) {
    // st.w [%sp+0], %r0: class=2, o1=7, imm6=0, rs=r0
    // insn = (2<<13)|(7<<10)|(0<<4)|0 = 0x4000|0x1C00 = 0x5C00
    cpu.state.sp = 0x0500;
    set_reg(0, 0x11223344u);
    exec({0x5C00});
    EXPECT_EQ(bus.read32(0x0500), 0x11223344u);
}

// ============================================================================
// Class 4B: shift immediate
// ============================================================================

TEST_F(CpuInsnFixture, Srl_Rd_Imm4) {
    // srl %r0, 1: class=4 (bits[15:13]=100), o1=2 (srl), o2=0 (imm shift)
    // shamt=1 in bits[7:4], rd=0 in bits[3:0]
    // insn = (4<<13)|(2<<10)|(0<<8)|(1<<4)|0 = 0x8000|0x0800|0x0010 = 0x8810
    set_reg(0, 0x80000000u);
    exec({0x8810});
    EXPECT_EQ(reg(0), 0x40000000u); // logical right shift by 1
}

TEST_F(CpuInsnFixture, Sll_Rd_Imm4) {
    // sll %r0, 1: class=4, o1=3 (sll), o2=0 (imm shift)
    // insn = (4<<13)|(3<<10)|(0<<8)|(1<<4)|0 = 0x8000|0x0C00|0x0010 = 0x8C10
    set_reg(0, 1);
    exec({0x8C10});
    EXPECT_EQ(reg(0), 2u); // left shift by 1
}

TEST_F(CpuInsnFixture, Sra_Rd_Imm4) {
    // sra %r0, 1: class=4, o1=4 (sra), o2=0 (imm shift)
    // insn = (4<<13)|(4<<10)|(0<<8)|(1<<4)|0 = 0x8000|0x1000|0x0010 = 0x9010
    set_reg(0, 0x80000000u);
    exec({0x9010});
    EXPECT_EQ(reg(0), 0xC0000000u); // arithmetic right shift preserves sign
}

// ============================================================================
// Class 0b: conditional branches
// ============================================================================

TEST_F(CpuInsnFixture, Jreq_Taken) {
    // jreq +2: op1_f=12 (bits[12:9]=1100), bit8=0, imm8=1 → target = 0 + 2*1 = 2
    // insn = (12<<9)|(0<<8)|1 = 0x1801
    // After exec: Z flag must be set, branch taken to 0x0002
    cpu.state.psr.set_z(true);
    exec({0x1801});
    EXPECT_EQ(cpu.state.pc, 2u);
}

TEST_F(CpuInsnFixture, Jreq_NotTaken) {
    // jreq +2 when Z=0: branch not taken, PC advances to next insn (0x0002)
    cpu.state.psr.set_z(false);
    exec({0x1801});
    EXPECT_EQ(cpu.state.pc, 2u); // not taken: PC = 0 + 2 (instruction size) = 2
}

TEST_F(CpuInsnFixture, Jrne_Taken) {
    // jrne: op1_f=13 (0b1101), bit8=0, imm8=2 → target = 0 + 2*2 = 4
    // insn = (13<<9)|2 = 0x1A02
    // jrne taken when Z=0
    cpu.state.psr.set_z(false);
    exec({0x1A02});
    EXPECT_EQ(cpu.state.pc, 4u);
}

TEST_F(CpuInsnFixture, Jp_Simm8) {
    // jp: op1_f=15 (0b1111), bit8=0, imm8=4 → target = 0 + 2*4 = 8
    // insn = (15<<9)|4 = 0x1E04
    exec({0x1E04});
    EXPECT_EQ(cpu.state.pc, 8u);
}

// ============================================================================
// Undefined instruction detection
// ============================================================================

TEST_F(CpuInsnFixture, UndefSetsHaltAndFault) {
    // opcode 0x0140 is not a valid instruction (class0a, op1_f=0, op2_f=1 with rdn=0 is slp)
    // Let's use 0x00C0 which is reserved (class0a, op1_f=0, op2_f=3)
    exec({0x00C0});
    EXPECT_TRUE(cpu.state.in_halt);
    EXPECT_TRUE(cpu.state.fault);
}

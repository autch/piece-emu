// test_disasm.cpp — Layer 1: disassembler output unit tests
// Tests Cpu::disasm() by placing instruction words in IRAM and checking mnemonics.
#include "cpu.hpp"
#include "bus.hpp"
#include <gtest/gtest.h>
#include <cstring>

class DisasmFixture : public ::testing::Test {
protected:
    Bus bus;
    Cpu cpu{bus};

    DisasmFixture() : bus(0) {}

    // Write a 16-bit instruction at address 0 in IRAM and disassemble it.
    std::string dis(uint16_t insn) {
        uint8_t* p = bus.iram_ptr();
        p[0] = insn & 0xFF;
        p[1] = insn >> 8;
        return cpu.disasm(0);
    }
};

// disasm() returns "0x{addr:06X}: {raw:04X}  {mnemonic}" — helpers to match just mnemonic.
// Use full-string comparison with known address 0x000000.
#define EXPECT_DISASM(insn_val, expected_mnem) \
    EXPECT_EQ(dis(insn_val), "0x000000: " #insn_val "  " expected_mnem)

// ---- Class 0a: special instructions ----

TEST_F(DisasmFixture, Nop) {
    EXPECT_EQ(dis(0x0000), "0x000000: 0000  nop");
}

TEST_F(DisasmFixture, Slp) {
    // slp: class0, op1_f=0, op2_f=1 → bits[7:6]=01 → 0x0040
    EXPECT_EQ(dis(0x0040), "0x000000: 0040  slp");
}

TEST_F(DisasmFixture, Halt) {
    // halt: op2_f=2 → bits[7:6]=10 → 0x0080
    EXPECT_EQ(dis(0x0080), "0x000000: 0080  halt");
}

// ---- Class 0a: ret/call/jp ----

TEST_F(DisasmFixture, Ret) {
    // ret: op1_f=3, op2_f=1, d=0 → bits[12:9]=0011, bit8=0, bits[7:6]=01
    // insn = (3<<9) | (0<<8) | (1<<6) = 0x0600 | 0x0040 = 0x0640
    EXPECT_EQ(dis(0x0640), "0x000000: 0640  ret");
}

// ---- Class 0b: PC-relative branches ----

TEST_F(DisasmFixture, Jreq_ZeroOffset) {
    // jreq: op1_f=12 (bits[12:9]=1100), bit8=0 (no delay), imm8=0
    // target = 0 + 2*0 = 0
    // insn = (12<<9) | 0 = 0x1800
    EXPECT_EQ(dis(0x1800), "0x000000: 1800  jreq\t0x000000");
}

TEST_F(DisasmFixture, Jp_Simm8) {
    // jp: op1_f=15 (0b1111), bit8=0, imm8=0
    // insn = (15<<9) | 0 = 0x1E00
    EXPECT_EQ(dis(0x1E00), "0x000000: 1E00  jp\t0x000000");
}

// ---- Class 1C: ALU register-register ----

TEST_F(DisasmFixture, Add_R0_R1) {
    // add %r0, %r1: class=1, o1=0 (add), o2=2 (ALU), rb=r1, rd=r0
    // insn = (1<<13) | (0<<10) | (2<<8) | (1<<4) | 0
    //      = 0x2000 | 0x0200 | 0x0010 = 0x2210
    EXPECT_EQ(dis(0x2210), "0x000000: 2210  add\t%r0, %r1");
}

TEST_F(DisasmFixture, Sub_R2_R3) {
    // sub %r2, %r3: class=1, o1=1, o2=2, rb=r3, rd=r2
    // insn = (1<<13) | (1<<10) | (2<<8) | (3<<4) | 2
    //      = 0x2000 | 0x0400 | 0x0200 | 0x0030 | 0x0002 = 0x2632
    EXPECT_EQ(dis(0x2632), "0x000000: 2632  sub\t%r2, %r3");
}

TEST_F(DisasmFixture, Ld_W_R4_R5) {
    // ld.w %r4, %r5: class=1, o1=3 (ld.w), o2=2, rb=r5, rd=r4
    // insn = (1<<13) | (3<<10) | (2<<8) | (5<<4) | 4
    //      = 0x2000 | 0x0C00 | 0x0200 | 0x0050 | 0x0004 = 0x2E54
    EXPECT_EQ(dis(0x2E54), "0x000000: 2E54  ld.w\t%r4, %r5");
}

// ---- Class 1A: memory load [%rb] ----

TEST_F(DisasmFixture, Ld_W_Rd_Rb) {
    // ld.w %r0, [%r1]: class=1, o1=4, o2=0, rb=r1, rd=r0
    // insn = (1<<13) | (4<<10) | (0<<8) | (1<<4) | 0
    //      = 0x2000 | 0x1000 | 0x0010 = 0x3010
    EXPECT_EQ(dis(0x3010), "0x000000: 3010  ld.w\t%r0, [%r1]");
}

// ---- Class 3: immediate ALU ----

TEST_F(DisasmFixture, Add_R0_Imm6_Positive) {
    // add %r0, 1: class=3, o1=0, rd=r0, imm6=1
    // insn = (3<<13) | (0<<10) | (1<<4) | 0 = 0x6000 | 0x0010 = 0x6010
    EXPECT_EQ(dis(0x6010), "0x000000: 6010  add\t%r0, 1");
}

TEST_F(DisasmFixture, Add_R0_Imm6_Negative) {
    // add %r0, -1: imm6=0b111111=63 → sign-extended = -1
    // insn = (3<<13) | (0<<10) | (63<<4) | 0 = 0x6000 | 0x03F0 = 0x63F0
    EXPECT_EQ(dis(0x63F0), "0x000000: 63F0  add\t%r0, -1");
}

// ---- Class 6: EXT ----

TEST_F(DisasmFixture, Ext_Zero) {
    // ext 0: class=6, imm13=0 → 0xC000
    EXPECT_EQ(dis(0xC000), "0x000000: C000  ext\t0");
}

TEST_F(DisasmFixture, Ext_One) {
    // ext 1: 0xC001
    EXPECT_EQ(dis(0xC001), "0x000000: C001  ext\t1");
}

TEST_F(DisasmFixture, Ext_MaxImm13) {
    // ext 8191 (0x1FFF): 0xDFFF
    EXPECT_EQ(dis(0xDFFF), "0x000000: DFFF  ext\t8191");
}

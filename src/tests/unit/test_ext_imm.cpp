// test_ext_imm.cpp — Layer 1: EXT immediate synthesis unit tests
// Tests Cpu::ext_imm() and Cpu::ext_simm() for all combinations of
// pending EXT count (0/1/2) and various immediate widths.
#include "cpu.hpp"
#include "bus.hpp"
#include <gtest/gtest.h>

// We need a Bus to construct Cpu, but ext_imm/ext_simm only read state.
// Use a zero-size flash Bus so it exists but is never accessed.
struct ExtImmFixture : public ::testing::Test {
protected:
    Bus bus;
    Cpu cpu{bus};

    ExtImmFixture() : bus(0) {}

    void set_ext(int count, uint32_t ext0 = 0, uint32_t ext1 = 0) {
        cpu.state.pending_ext_count = count;
        cpu.state.pending_ext[0] = ext0;
        cpu.state.pending_ext[1] = ext1;
    }
};

// ---- ext_imm (unsigned) ----

TEST_F(ExtImmFixture, NoExt_Passthrough) {
    set_ext(0);
    // imm6 = 0b101010 = 42, width 6
    EXPECT_EQ(cpu.ext_imm(42, 6), 42u);
}

TEST_F(ExtImmFixture, NoExt_MaxImm6) {
    set_ext(0);
    EXPECT_EQ(cpu.ext_imm(63, 6), 63u);
}

TEST_F(ExtImmFixture, OneExt_CombinesHighBits) {
    // ext 0x1000, then imm6=0: result = 0x1000 << 6 = 0x40000
    set_ext(1, 0x1000);
    EXPECT_EQ(cpu.ext_imm(0, 6), 0x40000u);
}

TEST_F(ExtImmFixture, OneExt_CombinesWithLow) {
    // ext 1, imm6=1: result = (1 << 6) | 1 = 65
    set_ext(1, 1);
    EXPECT_EQ(cpu.ext_imm(1, 6), 65u);
}

TEST_F(ExtImmFixture, TwoExt_CombinesAllThree) {
    // ext[0]=1, ext[1]=1, imm6=1:
    // result = (1 << (13+6)) | (1 << 6) | 1 = (1<<19) | 64 | 1
    set_ext(2, 1, 1);
    uint32_t expected = (1u << 19) | (1u << 6) | 1u;
    EXPECT_EQ(cpu.ext_imm(1, 6), expected);
}

TEST_F(ExtImmFixture, NoExt_ImmmaskIgnoresHighBits) {
    set_ext(0);
    // Pass imm with high bits set; only width=6 bits should survive
    EXPECT_EQ(cpu.ext_imm(0xFFFF, 6), 63u);
}

// ---- ext_simm (sign-extended) ----

TEST_F(ExtImmFixture, Simm_NoExt_PositiveValue) {
    set_ext(0);
    // imm6=10 (positive, MSB of 6-bit = bit5 = 0): result = +10
    EXPECT_EQ(cpu.ext_simm(10, 6), 10);
}

TEST_F(ExtImmFixture, Simm_NoExt_NegativeValue) {
    set_ext(0);
    // imm6=0b100000 = 32 in 6-bit = sign-extended to -32
    EXPECT_EQ(cpu.ext_simm(32, 6), -32);
}

TEST_F(ExtImmFixture, Simm_NoExt_MinusOne) {
    set_ext(0);
    // imm6=0b111111 = 63 → sign-extended = -1
    EXPECT_EQ(cpu.ext_simm(63, 6), -1);
}

TEST_F(ExtImmFixture, Simm_OneExt_PositiveResult) {
    set_ext(0);
    // No ext, imm8=0: 0 is still 0 (width=8)
    EXPECT_EQ(cpu.ext_simm(0, 8), 0);
}

TEST_F(ExtImmFixture, Simm_OneExt_SignExtend19bit) {
    // ext[0]=0x1000 (13 bits), imm6=0: combined = 0x40000 (19 bits)
    // MSB of 19-bit: bit18 = 0 in 0x40000 (0x40000 = 0b0_1000_0000_0000_0000_0000)
    // Wait: 0x1000 << 6 = 0x40000 = 0b0100_0000_0000_0000_0000
    // 19-bit value: bit18 = 1 → this is negative!
    // sign_ext(0x40000, 19): 0x40000 has bit18 set. sign = 1<<18=0x40000
    // (0x40000 ^ 0x40000) - 0x40000 = 0 - 0x40000 = -0x40000
    set_ext(1, 0x1000);
    EXPECT_EQ(cpu.ext_simm(0, 6), -0x40000);
}

TEST_F(ExtImmFixture, Simm_OneExt_PositiveSmall) {
    // ext[0]=1, imm6=1: combined = (1<<6)|1 = 65; sign extend at 19 bits → +65
    set_ext(1, 1);
    EXPECT_EQ(cpu.ext_simm(1, 6), 65);
}

TEST_F(ExtImmFixture, Simm_TwoExt_FullWidth) {
    // ext[0]=0, ext[1]=0, imm6=0: result = 0
    set_ext(2, 0, 0);
    EXPECT_EQ(cpu.ext_simm(0, 6), 0);
}

// ---- ext_rb (base register displacement from EXT only) ----

TEST_F(ExtImmFixture, ExtRb_NoExt_Zero) {
    set_ext(0);
    EXPECT_EQ(cpu.ext_rb(), 0u);
}

TEST_F(ExtImmFixture, ExtRb_OneExt) {
    set_ext(1, 0x1234);
    EXPECT_EQ(cpu.ext_rb(), 0x1234u);
}

TEST_F(ExtImmFixture, ExtRb_TwoExt) {
    // (ext[0] << 13) | ext[1]
    set_ext(2, 1, 2);
    EXPECT_EQ(cpu.ext_rb(), (1u << 13) | 2u);
}

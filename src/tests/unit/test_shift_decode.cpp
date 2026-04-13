// test_shift_decode.cpp — Layer 1: Insn::shift_amt() unit tests
// The shift amount field imm4 in bits[7:4]:
//   0000..0111 → shift amount 0..7
//   1xxx       → shift amount 8 (max)
#include <gtest/gtest.h>
#include <cstdint>
#include "insn.hpp"

TEST(ShiftDecode, Zero) {
    // imm4 = 0b0000 → 0
    EXPECT_EQ(Insn{0x0000}.shift_amt(), 0); // bits[7:4] = 0
}

TEST(ShiftDecode, One) {
    EXPECT_EQ(Insn{0x0010}.shift_amt(), 1); // bits[7:4] = 0001
}

TEST(ShiftDecode, Seven) {
    EXPECT_EQ(Insn{0x0070}.shift_amt(), 7); // bits[7:4] = 0111
}

TEST(ShiftDecode, Eight_FromExact8) {
    EXPECT_EQ(Insn{0x0080}.shift_amt(), 8); // bits[7:4] = 1000 → 8
}

TEST(ShiftDecode, Eight_From0x9) {
    EXPECT_EQ(Insn{0x0090}.shift_amt(), 8); // bits[7:4] = 1001 → 8
}

TEST(ShiftDecode, Eight_From0xF) {
    EXPECT_EQ(Insn{0x00F0}.shift_amt(), 8); // bits[7:4] = 1111 → 8
}

TEST(ShiftDecode, HighBitsIgnored) {
    // Other bits of insn should not affect the result
    EXPECT_EQ(Insn{0xFF30}.shift_amt(), 3); // bits[7:4] = 0011 → 3
}

TEST(ShiftDecode, AllValues_0to7) {
    for (int v = 0; v < 8; ++v) {
        uint16_t insn = static_cast<uint16_t>(v << 4);
        EXPECT_EQ(Insn{insn}.shift_amt(), v) << "imm4 = " << v;
    }
}

TEST(ShiftDecode, AllValues_8to15_ClampTo8) {
    for (int v = 8; v < 16; ++v) {
        uint16_t insn = static_cast<uint16_t>(v << 4);
        EXPECT_EQ(Insn{insn}.shift_amt(), 8) << "imm4 = " << v;
    }
}

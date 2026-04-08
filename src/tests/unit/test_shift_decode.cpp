// test_shift_decode.cpp — Layer 1: decode_shift_imm4() unit tests
// The shift amount field imm4 in bits[7:4]:
//   0000..0111 → shift amount 0..7
//   1xxx       → shift amount 8 (max)
#include <gtest/gtest.h>
#include <cstdint>

// Reproduce the same logic as in cpu_impl.hpp for direct testing.
// The function reads bits[7:4] from the instruction word.
static int decode_shift_imm4(uint16_t insn) {
    int imm4 = (insn >> 4) & 0xF;
    if (imm4 & 8) return 8;
    return imm4;
}

TEST(ShiftDecode, Zero) {
    // imm4 = 0b0000 → 0
    uint16_t insn = 0x0000; // bits[7:4] = 0
    EXPECT_EQ(decode_shift_imm4(insn), 0);
}

TEST(ShiftDecode, One) {
    uint16_t insn = 0x0010; // bits[7:4] = 0001
    EXPECT_EQ(decode_shift_imm4(insn), 1);
}

TEST(ShiftDecode, Seven) {
    uint16_t insn = 0x0070; // bits[7:4] = 0111
    EXPECT_EQ(decode_shift_imm4(insn), 7);
}

TEST(ShiftDecode, Eight_FromExact8) {
    uint16_t insn = 0x0080; // bits[7:4] = 1000 → 8
    EXPECT_EQ(decode_shift_imm4(insn), 8);
}

TEST(ShiftDecode, Eight_From0x9) {
    uint16_t insn = 0x0090; // bits[7:4] = 1001 → 8
    EXPECT_EQ(decode_shift_imm4(insn), 8);
}

TEST(ShiftDecode, Eight_From0xF) {
    uint16_t insn = 0x00F0; // bits[7:4] = 1111 → 8
    EXPECT_EQ(decode_shift_imm4(insn), 8);
}

TEST(ShiftDecode, HighBitsIgnored) {
    // Other bits of insn should not affect the result
    uint16_t insn = 0xFF30; // bits[7:4] = 0011 → 3
    EXPECT_EQ(decode_shift_imm4(insn), 3);
}

TEST(ShiftDecode, AllValues_0to7) {
    for (int v = 0; v < 8; ++v) {
        uint16_t insn = static_cast<uint16_t>(v << 4);
        EXPECT_EQ(decode_shift_imm4(insn), v) << "imm4 = " << v;
    }
}

TEST(ShiftDecode, AllValues_8to15_ClampTo8) {
    for (int v = 8; v < 16; ++v) {
        uint16_t insn = static_cast<uint16_t>(v << 4);
        EXPECT_EQ(decode_shift_imm4(insn), 8) << "imm4 = " << v;
    }
}

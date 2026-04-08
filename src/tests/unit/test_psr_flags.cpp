// test_psr_flags.cpp — Layer 1: PSR flag calculation unit tests
// Tests Psr::set_nzvc_add(), set_nzvc_sub(), set_nz(), and individual flag setters.
#include "cpu.hpp"
#include <gtest/gtest.h>

// ---- Individual flag accessors ----

TEST(PsrFlags, InitialStateIsZero) {
    Psr p;
    EXPECT_EQ(p.raw, 0u);
    EXPECT_FALSE(p.n());
    EXPECT_FALSE(p.z());
    EXPECT_FALSE(p.v());
    EXPECT_FALSE(p.c());
}

TEST(PsrFlags, SetAndClearEachFlag) {
    Psr p;
    p.set_n(true);  EXPECT_TRUE(p.n());   p.set_n(false);  EXPECT_FALSE(p.n());
    p.set_z(true);  EXPECT_TRUE(p.z());   p.set_z(false);  EXPECT_FALSE(p.z());
    p.set_v(true);  EXPECT_TRUE(p.v());   p.set_v(false);  EXPECT_FALSE(p.v());
    p.set_c(true);  EXPECT_TRUE(p.c());   p.set_c(false);  EXPECT_FALSE(p.c());
    p.set_ie(true); EXPECT_TRUE(p.ie());  p.set_ie(false); EXPECT_FALSE(p.ie());
    p.set_ds(true); EXPECT_TRUE(p.ds());  p.set_ds(false); EXPECT_FALSE(p.ds());
    p.set_mo(true); EXPECT_TRUE(p.mo());  p.set_mo(false); EXPECT_FALSE(p.mo());
}

TEST(PsrFlags, InterruptLevel) {
    Psr p;
    p.set_il(7);
    EXPECT_EQ(p.il(), 7u);
    p.set_il(15);
    EXPECT_EQ(p.il(), 15u);
    p.set_il(0);
    EXPECT_EQ(p.il(), 0u);
    // Only 4 bits: 0xF mask
    p.set_il(0xFF);
    EXPECT_EQ(p.il(), 15u);
}

TEST(PsrFlags, FlagsDoNotInterfere) {
    Psr p;
    p.set_n(true);
    p.set_z(true);
    p.set_v(true);
    p.set_c(true);
    EXPECT_TRUE(p.n());
    EXPECT_TRUE(p.z());
    EXPECT_TRUE(p.v());
    EXPECT_TRUE(p.c());
    p.set_n(false);
    EXPECT_FALSE(p.n());
    EXPECT_TRUE(p.z());
    EXPECT_TRUE(p.v());
    EXPECT_TRUE(p.c());
}

// ---- set_nz ----

TEST(PsrFlags, SetNZ_Zero) {
    Psr p;
    p.set_nz(0);
    EXPECT_FALSE(p.n());
    EXPECT_TRUE(p.z());
}

TEST(PsrFlags, SetNZ_Positive) {
    Psr p;
    p.set_nz(1);
    EXPECT_FALSE(p.n());
    EXPECT_FALSE(p.z());
}

TEST(PsrFlags, SetNZ_Negative) {
    Psr p;
    p.set_nz(0x80000000u);
    EXPECT_TRUE(p.n());
    EXPECT_FALSE(p.z());
}

// ---- set_nzvc_add ----
// set_nzvc_add(a, b, r64): r = a + b

TEST(PsrFlags, Add_ZeroZero) {
    Psr p;
    p.set_nzvc_add(0, 0, 0);
    EXPECT_FALSE(p.n());
    EXPECT_TRUE(p.z());
    EXPECT_FALSE(p.v());
    EXPECT_FALSE(p.c());
}

TEST(PsrFlags, Add_NoCarryNoOverflow) {
    Psr p;
    p.set_nzvc_add(1, 2, 3);
    EXPECT_FALSE(p.n());
    EXPECT_FALSE(p.z());
    EXPECT_FALSE(p.v());
    EXPECT_FALSE(p.c());
}

TEST(PsrFlags, Add_CarryOut) {
    // 0xFFFFFFFF + 1 = 0x1_00000000
    uint64_t r64 = uint64_t(0xFFFFFFFFu) + 1;
    Psr p;
    p.set_nzvc_add(0xFFFFFFFFu, 1, r64);
    EXPECT_FALSE(p.n());
    EXPECT_TRUE(p.z());
    EXPECT_FALSE(p.v()); // both operands different sign, no signed overflow
    EXPECT_TRUE(p.c());
}

TEST(PsrFlags, Add_SignedOverflow_PosPos) {
    // 0x7FFFFFFF + 1 = 0x80000000 (positive + positive → negative: overflow)
    uint64_t r64 = uint64_t(0x7FFFFFFFu) + 1;
    Psr p;
    p.set_nzvc_add(0x7FFFFFFFu, 1, r64);
    EXPECT_TRUE(p.n());
    EXPECT_FALSE(p.z());
    EXPECT_TRUE(p.v());
    EXPECT_FALSE(p.c());
}

TEST(PsrFlags, Add_SignedOverflow_NegNeg) {
    // 0x80000000 + 0x80000000 = 0x1_00000000
    // negative + negative → carry, result zero (with carry): overflow
    uint64_t r64 = uint64_t(0x80000000u) + uint64_t(0x80000000u);
    Psr p;
    p.set_nzvc_add(0x80000000u, 0x80000000u, r64);
    EXPECT_FALSE(p.n());
    EXPECT_TRUE(p.z());
    EXPECT_TRUE(p.v());
    EXPECT_TRUE(p.c());
}

TEST(PsrFlags, Add_NegativeResult) {
    // 0xFFFFFFFE + 1 = 0xFFFFFFFF (no carry, no overflow)
    uint64_t r64 = uint64_t(0xFFFFFFFEu) + 1;
    Psr p;
    p.set_nzvc_add(0xFFFFFFFEu, 1, r64);
    EXPECT_TRUE(p.n());
    EXPECT_FALSE(p.z());
    EXPECT_FALSE(p.v());
    EXPECT_FALSE(p.c());
}

// ---- set_nzvc_sub ----
// set_nzvc_sub(a, b, r64): r = a - b (r64 is the subtraction result as unsigned 64-bit)
// C flag = borrow: set when unsigned a < b

TEST(PsrFlags, Sub_SameValues) {
    Psr p;
    p.set_nzvc_sub(5, 5, 0);
    EXPECT_FALSE(p.n());
    EXPECT_TRUE(p.z());
    EXPECT_FALSE(p.v());
    EXPECT_FALSE(p.c()); // no borrow
}

TEST(PsrFlags, Sub_PositiveResult) {
    Psr p;
    uint64_t r64 = 5 - uint64_t(3);
    p.set_nzvc_sub(5, 3, r64);
    EXPECT_FALSE(p.n());
    EXPECT_FALSE(p.z());
    EXPECT_FALSE(p.v());
    EXPECT_FALSE(p.c());
}

TEST(PsrFlags, Sub_Borrow) {
    // 0 - 1: unsigned underflow, borrow set
    uint64_t r64 = uint64_t(0) - uint64_t(1); // wraps to 0xFFFF_FFFF_FFFF_FFFF
    Psr p;
    p.set_nzvc_sub(0, 1, r64);
    EXPECT_TRUE(p.n());
    EXPECT_FALSE(p.z());
    EXPECT_FALSE(p.v()); // 0 - 1: no signed overflow (0 is positive, -1 is negative, result is negative: normal)
    EXPECT_TRUE(p.c());  // borrow
}

TEST(PsrFlags, Sub_SignedOverflow_PosNeg) {
    // 0x7FFFFFFF - 0xFFFFFFFF = 0x7FFFFFFF + 1 = 0x80000000
    // positive - negative → positive should be result but got negative: overflow
    uint32_t a = 0x7FFFFFFFu;
    uint32_t b = 0xFFFFFFFFu;
    uint64_t r64 = uint64_t(a) - uint64_t(b); // wraps
    Psr p;
    p.set_nzvc_sub(a, b, r64);
    EXPECT_TRUE(p.n());
    EXPECT_FALSE(p.z());
    EXPECT_TRUE(p.v());
    EXPECT_TRUE(p.c()); // borrow since a < b unsigned
}

TEST(PsrFlags, Sub_NegativeMinusPositive_Overflow) {
    // 0x80000000 - 1 = 0x7FFFFFFF: negative - positive → positive = overflow
    uint32_t a = 0x80000000u;
    uint32_t b = 1;
    uint64_t r64 = uint64_t(a) - uint64_t(b);
    Psr p;
    p.set_nzvc_sub(a, b, r64);
    EXPECT_FALSE(p.n());
    EXPECT_FALSE(p.z());
    EXPECT_TRUE(p.v());
    EXPECT_FALSE(p.c()); // no borrow (a >= b unsigned)
}

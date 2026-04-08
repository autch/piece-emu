// test_shifts.c — shift and rotate instruction tests (Classes 4B and 4C)
//
// Instructions: srl, sll, sra, sla, rr, rl (with immediate and register operands)
// PSR flags: N, Z, C (C = last bit shifted out; sla also sets V)
//
// Shift amounts:
//   imm4 form: encoded 0..7, with 0000=0, ..., 0111=7, 1xxx=8 (max is 8)
//   register form: low 5 bits of rs (0..31)

#include <stdint.h>

#define PSR_N 0x01
#define PSR_Z 0x02
#define PSR_V 0x04
#define PSR_C 0x08

#define CHECK(cond, code) do { if (!(cond)) return (code); } while (0)

// ============================================================================
// SRL (logical right shift): zero-fills from MSB, C = last bit shifted out
// ============================================================================
static int test_srl_imm(void) {
    uint32_t psr;

    // 0xFFFFFFFF >> 1 = 0x7FFFFFFF, C=1 (bit0 shifted out), N=0 (MSB cleared)
    uint32_t v1 = 0xFFFFFFFFu;
    asm volatile("srl %0, 1\n" "ld.w %1, %%psr\n" : "+r"(v1), "=r"(psr));
    CHECK(v1 == 0x7FFFFFFFu, 1);
    CHECK((psr & PSR_C) == PSR_C, 2);
    CHECK((psr & PSR_N) == 0, 3);

    // 0x80000000 >> 1 = 0x40000000, C=0 (bit0 was 0)
    uint32_t v2 = 0x80000000u;
    asm volatile("srl %0, 1\n" "ld.w %1, %%psr\n" : "+r"(v2), "=r"(psr));
    CHECK(v2 == 0x40000000u, 4);
    CHECK((psr & PSR_C) == 0, 5);

    return 0;
}

// ============================================================================
// SLL (logical left shift): zero-fills from LSB, C = last bit shifted out (from MSB)
// ============================================================================
static int test_sll_imm(void) {
    uint32_t psr;

    // 0xFFFFFFFF << 1 = 0xFFFFFFFE, C=1 (bit31 shifted out)
    uint32_t v1 = 0xFFFFFFFFu;
    asm volatile("sll %0, 1\n" "ld.w %1, %%psr\n" : "+r"(v1), "=r"(psr));
    CHECK(v1 == 0xFFFFFFFEu, 6);
    CHECK((psr & PSR_C) == PSR_C, 7);

    // 0x7FFFFFFF << 1 = 0xFFFFFFFE, C=0 (bit31=0 shifted out)
    uint32_t v2 = 0x7FFFFFFFu;
    asm volatile("sll %0, 1\n" "ld.w %1, %%psr\n" : "+r"(v2), "=r"(psr));
    CHECK(v2 == 0xFFFFFFFEu, 8);
    CHECK((psr & PSR_C) == 0, 9);

    return 0;
}

// ============================================================================
// SRA (arithmetic right shift): sign-fills from MSB, C = last bit shifted out
// ============================================================================
static int test_sra_imm(void) {
    uint32_t psr;

    // 0x80000000 >>1 (arithmetic) = 0xC0000000 (sign preserved), N=1
    uint32_t v1 = 0x80000000u;
    asm volatile("sra %0, 1\n" "ld.w %1, %%psr\n" : "+r"(v1), "=r"(psr));
    CHECK(v1 == 0xC0000000u, 10);
    CHECK((psr & PSR_N) == PSR_N, 11);

    // -4 (0xFFFFFFFC) >>1 = -2 (0xFFFFFFFE)
    uint32_t v2 = 0xFFFFFFFCu;
    asm volatile("sra %0, 1\n" : "+r"(v2));
    CHECK(v2 == 0xFFFFFFFEu, 12);

    return 0;
}

// ============================================================================
// SLA (arithmetic left shift = logical left, plus V if sign changes)
// ============================================================================
static int test_sla_imm(void) {
    uint32_t psr;

    // 1 << 1 = 2, no sign change, V=0
    uint32_t v1 = 1;
    asm volatile("sla %0, 1\n" "ld.w %1, %%psr\n" : "+r"(v1), "=r"(psr));
    CHECK(v1 == 2u, 13);
    CHECK((psr & PSR_V) == 0, 14);

    // 0x40000000 << 1 = 0x80000000: sign bit changes (0→1), V=1
    uint32_t v2 = 0x40000000u;
    asm volatile("sla %0, 1\n" "ld.w %1, %%psr\n" : "+r"(v2), "=r"(psr));
    CHECK(v2 == 0x80000000u, 15);
    CHECK((psr & PSR_V) == PSR_V, 16);

    return 0;
}

// ============================================================================
// RR (rotate right): bit0 wraps to bit31, C = bit0
// ============================================================================
static int test_rr_imm(void) {
    uint32_t psr;

    // 0x00000001 ror 1 = 0x80000000, C=1 (bit0 rotated to bit31)
    uint32_t v1 = 1;
    asm volatile("rr %0, 1\n" "ld.w %1, %%psr\n" : "+r"(v1), "=r"(psr));
    CHECK(v1 == 0x80000000u, 17);
    CHECK((psr & PSR_C) == PSR_C, 18);

    // 0x00000002 ror 1 = 0x00000001, C=0 (bit0=0)
    uint32_t v2 = 2;
    asm volatile("rr %0, 1\n" "ld.w %1, %%psr\n" : "+r"(v2), "=r"(psr));
    CHECK(v2 == 1u, 19);
    CHECK((psr & PSR_C) == 0, 20);

    return 0;
}

// ============================================================================
// RL (rotate left): bit31 wraps to bit0, C = bit31
// ============================================================================
static int test_rl_imm(void) {
    uint32_t psr;

    // 0x80000000 rol 1 = 0x00000001, C=1 (bit31 rotated to bit0)
    uint32_t v1 = 0x80000000u;
    asm volatile("rl %0, 1\n" "ld.w %1, %%psr\n" : "+r"(v1), "=r"(psr));
    CHECK(v1 == 1u, 21);
    CHECK((psr & PSR_C) == PSR_C, 22);

    // 0x40000000 rol 1 = 0x80000000, C=0 (bit31=0)
    uint32_t v2 = 0x40000000u;
    asm volatile("rl %0, 1\n" "ld.w %1, %%psr\n" : "+r"(v2), "=r"(psr));
    CHECK(v2 == 0x80000000u, 23);
    CHECK((psr & PSR_C) == 0, 24);

    return 0;
}

// ============================================================================
// Register-count shifts (Class 4C): srl/sll/sra/rr/rl %rd, %rs
// ============================================================================
static int test_srl_rs(void) {
    uint32_t v = 0x80u, cnt = 3; /* 128 >> 3 = 16 */
    asm volatile("srl %0, %1\n" : "+r"(v) : "r"(cnt));
    CHECK(v == 16u, 25);
    return 0;
}

static int test_sll_rs(void) {
    uint32_t v = 1, cnt = 8; /* 1 << 8 = 256 */
    asm volatile("sll %0, %1\n" : "+r"(v) : "r"(cnt));
    CHECK(v == 256u, 26);
    return 0;
}

static int test_sra_rs(void) {
    uint32_t v = 0xFFFFFFF8u, cnt = 2; /* -8 >> 2 = -2 */
    asm volatile("sra %0, %1\n" : "+r"(v) : "r"(cnt));
    CHECK(v == 0xFFFFFFFEu, 27);
    return 0;
}

static int test_rr_rs(void) {
    uint32_t v = 0x80000008u, cnt = 3; /* 0x80000008 ror 3 = 0x10000001 */
    asm volatile("rr %0, %1\n" : "+r"(v) : "r"(cnt));
    CHECK(v == 0x10000001u, 28);
    return 0;
}

static int test_rl_rs(void) {
    uint32_t v = 0x40000000u, cnt = 2; /* 0x40000000 rol 2 = 0x00000001 */
    asm volatile("rl %0, %1\n" : "+r"(v) : "r"(cnt));
    CHECK(v == 1u, 29);
    return 0;
}

// ============================================================================
// Shift by 0 (register form): result unchanged
// imm4 form requires [1,8]; use register form with count=0 instead.
// ============================================================================
static int test_shift_zero_count(void) {
    uint32_t v = 0x12345678u, cnt = 0;
    asm volatile("srl %0, %1\n" : "+r"(v) : "r"(cnt));
    CHECK(v == 0x12345678u, 30);
    return 0;
}

int main(void) {
    int e;
    if ((e = test_srl_imm()))       return e;
    if ((e = test_sll_imm()))       return e;
    if ((e = test_sra_imm()))       return e;
    if ((e = test_sla_imm()))       return e;
    if ((e = test_rr_imm()))        return e;
    if ((e = test_rl_imm()))        return e;
    if ((e = test_srl_rs()))        return e;
    if ((e = test_sll_rs()))        return e;
    if ((e = test_sra_rs()))        return e;
    if ((e = test_rr_rs()))         return e;
    if ((e = test_rl_rs()))         return e;
    if ((e = test_shift_zero_count())) return e;
    return 0;
}

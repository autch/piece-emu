// test_shifts.c — shift and rotate instruction tests (Classes 4B and 4C)
//
// Per the S1C33000 manual the flag mask for srl / sll / sra / sla / rr / rl
// is "----↔↔" — only Z and N change, and C and V are PRESERVED across the
// shift / rotate.  (clippce/framflt1 __addsf3 relies on this to keep the
// C flag set by `scan1` live across the following `sll` so that the
// `jrult.d -2` normalize-loop branch can read it.)
//
// Instructions: srl, sll, sra, sla, rr, rl (with immediate and register operands)
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

// Pre-set C and V to known values via a direct PSR write before running the
// instruction-under-test, then read PSR back.  Reusable helper-form macro.
#define DO_SHIFT_CHECK_CV_PRESERVED(insn, vreg, psrreg, scratch) \
    asm volatile( \
        "ld.w %2, %%psr\n"   /* scratch = psr */ \
        "or %2, 12\n"        /* scratch |= (C|V) */ \
        "ld.w %%psr, %2\n"   /* psr = scratch (C=1, V=1) */ \
        insn "\n"            /* the instruction under test */ \
        "ld.w %1, %%psr\n"   /* capture final PSR */ \
        : "+r"(vreg), "=r"(psrreg), "=&r"(scratch))

// ============================================================================
// SRL (logical right shift): zero-fills from MSB.  C/V preserved.
// ============================================================================
static int test_srl_imm(void) {
    uint32_t psr, scratch;

    uint32_t v1 = 0xFFFFFFFFu;
    DO_SHIFT_CHECK_CV_PRESERVED("srl %0, 1", v1, psr, scratch);
    CHECK(v1 == 0x7FFFFFFFu, 1);
    CHECK((psr & PSR_C) == PSR_C, 2);   // preserved
    CHECK((psr & PSR_V) == PSR_V, 3);   // preserved
    CHECK((psr & PSR_N) == 0, 4);       // result MSB cleared

    uint32_t v2 = 0x80000000u;
    DO_SHIFT_CHECK_CV_PRESERVED("srl %0, 1", v2, psr, scratch);
    CHECK(v2 == 0x40000000u, 5);
    CHECK((psr & PSR_C) == PSR_C, 6);   // still preserved
    CHECK((psr & PSR_V) == PSR_V, 7);

    return 0;
}

// ============================================================================
// SLL (logical left shift): zero-fills from LSB.  C/V preserved.
// ============================================================================
static int test_sll_imm(void) {
    uint32_t psr, scratch;

    uint32_t v1 = 0xFFFFFFFFu;
    DO_SHIFT_CHECK_CV_PRESERVED("sll %0, 1", v1, psr, scratch);
    CHECK(v1 == 0xFFFFFFFEu, 8);
    CHECK((psr & PSR_C) == PSR_C, 9);   // preserved
    CHECK((psr & PSR_V) == PSR_V, 10);

    uint32_t v2 = 0x7FFFFFFFu;
    DO_SHIFT_CHECK_CV_PRESERVED("sll %0, 1", v2, psr, scratch);
    CHECK(v2 == 0xFFFFFFFEu, 11);
    CHECK((psr & PSR_C) == PSR_C, 12);
    CHECK((psr & PSR_V) == PSR_V, 13);

    return 0;
}

// ============================================================================
// SRA (arithmetic right shift): sign-fills from MSB.  C/V preserved.
// ============================================================================
static int test_sra_imm(void) {
    uint32_t psr, scratch;

    uint32_t v1 = 0x80000000u;
    DO_SHIFT_CHECK_CV_PRESERVED("sra %0, 1", v1, psr, scratch);
    CHECK(v1 == 0xC0000000u, 14);
    CHECK((psr & PSR_N) == PSR_N, 15);
    CHECK((psr & PSR_C) == PSR_C, 16);

    uint32_t v2 = 0xFFFFFFFCu;
    asm volatile("sra %0, 1\n" : "+r"(v2));
    CHECK(v2 == 0xFFFFFFFEu, 17);

    return 0;
}

// ============================================================================
// SLA (arithmetic left shift): same numeric result as sll, same flag rule.
// C/V preserved per manual mask.
// ============================================================================
static int test_sla_imm(void) {
    uint32_t psr, scratch;

    uint32_t v1 = 1;
    DO_SHIFT_CHECK_CV_PRESERVED("sla %0, 1", v1, psr, scratch);
    CHECK(v1 == 2u, 18);
    CHECK((psr & PSR_V) == PSR_V, 19);   // preserved (mask did not say sla updates V)

    uint32_t v2 = 0x40000000u;
    DO_SHIFT_CHECK_CV_PRESERVED("sla %0, 1", v2, psr, scratch);
    CHECK(v2 == 0x80000000u, 20);
    CHECK((psr & PSR_V) == PSR_V, 21);
    CHECK((psr & PSR_C) == PSR_C, 22);

    return 0;
}

// ============================================================================
// RR (rotate right): bit0 wraps to bit31.  C/V preserved.
// ============================================================================
static int test_rr_imm(void) {
    uint32_t psr, scratch;

    uint32_t v1 = 1;
    DO_SHIFT_CHECK_CV_PRESERVED("rr %0, 1", v1, psr, scratch);
    CHECK(v1 == 0x80000000u, 23);
    CHECK((psr & PSR_C) == PSR_C, 24);   // preserved
    CHECK((psr & PSR_N) == PSR_N, 25);   // bit31 of result is 1

    uint32_t v2 = 2;
    DO_SHIFT_CHECK_CV_PRESERVED("rr %0, 1", v2, psr, scratch);
    CHECK(v2 == 1u, 26);
    CHECK((psr & PSR_C) == PSR_C, 27);
    CHECK((psr & PSR_V) == PSR_V, 28);

    return 0;
}

// ============================================================================
// RL (rotate left): bit31 wraps to bit0.  C/V preserved.
// ============================================================================
static int test_rl_imm(void) {
    uint32_t psr, scratch;

    uint32_t v1 = 0x80000000u;
    DO_SHIFT_CHECK_CV_PRESERVED("rl %0, 1", v1, psr, scratch);
    CHECK(v1 == 1u, 29);
    CHECK((psr & PSR_C) == PSR_C, 30);

    uint32_t v2 = 0x40000000u;
    DO_SHIFT_CHECK_CV_PRESERVED("rl %0, 1", v2, psr, scratch);
    CHECK(v2 == 0x80000000u, 31);
    CHECK((psr & PSR_C) == PSR_C, 32);
    CHECK((psr & PSR_V) == PSR_V, 33);

    return 0;
}

// ============================================================================
// Register-count shifts (Class 4C): srl/sll/sra/rr/rl %rd, %rs
// ============================================================================
static int test_srl_rs(void) {
    uint32_t v = 0x80u, cnt = 3; /* 128 >> 3 = 16 */
    asm volatile("srl %0, %1\n" : "+r"(v) : "r"(cnt));
    CHECK(v == 16u, 34);
    return 0;
}

static int test_sll_rs(void) {
    uint32_t v = 1, cnt = 8; /* 1 << 8 = 256 */
    asm volatile("sll %0, %1\n" : "+r"(v) : "r"(cnt));
    CHECK(v == 256u, 35);
    return 0;
}

static int test_sra_rs(void) {
    uint32_t v = 0xFFFFFFF8u, cnt = 2; /* -8 >> 2 = -2 */
    asm volatile("sra %0, %1\n" : "+r"(v) : "r"(cnt));
    CHECK(v == 0xFFFFFFFEu, 36);
    return 0;
}

static int test_rr_rs(void) {
    uint32_t v = 0x80000008u, cnt = 3; /* 0x80000008 ror 3 = 0x10000001 */
    asm volatile("rr %0, %1\n" : "+r"(v) : "r"(cnt));
    CHECK(v == 0x10000001u, 37);
    return 0;
}

static int test_rl_rs(void) {
    uint32_t v = 0x40000000u, cnt = 2; /* 0x40000000 rol 2 = 0x00000001 */
    asm volatile("rl %0, %1\n" : "+r"(v) : "r"(cnt));
    CHECK(v == 1u, 38);
    return 0;
}

// ============================================================================
// Shift by 0 (register form): result unchanged
// imm4 form requires [1,8]; use register form with count=0 instead.
// ============================================================================
static int test_shift_zero_count(void) {
    uint32_t v = 0x12345678u, cnt = 0;
    asm volatile("srl %0, %1\n" : "+r"(v) : "r"(cnt));
    CHECK(v == 0x12345678u, 39);
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

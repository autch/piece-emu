// test_div.c — step division instruction tests (Class 4D)
//
// S1C33 has no hardware divider. Division uses a 35-instruction sequence:
//   div0u (or div0s) + 32 × div1 + [div2s + div3s for signed]
//
// Instruction syntax (LLVM assembler requires two register operands; the CPU
// only uses rs for the divisor — rd is encoded but ignored by the hardware):
//   div0u %rd, %rs   unsigned setup: AHR=0, C=0
//   div0s %rd, %rs   signed setup: AHR=sign(ALR), C=sign(ALR)
//   div1  %rd, %rs   one restoring-division step (rs = divisor each time)
//
// Strategy: keep divisor in R0 throughout, use R0 as both rd and rs
// (i.e., "div0u %%r0, %%r0" and "div1 %%r0, %%r0").
// Dividend is loaded into ALR via a constraint-assigned register.
//
// After 32 div1 steps: ALR = quotient, AHR = remainder.

#include <stdint.h>

#define CHECK(cond, code) do { if (!(cond)) return (code); } while (0)

static uint32_t read_alr(void) {
    uint32_t v; asm volatile("ld.w %0, %%alr" : "=r"(v)); return v;
}
static uint32_t read_ahr(void) {
    uint32_t v; asm volatile("ld.w %0, %%ahr" : "=r"(v)); return v;
}

// 32 div1 steps with divisor in R0 (rd=R0 is dummy, rs=R0 is divisor).
#define DIV1_32 \
    "div1 %%r0, %%r0\n" "div1 %%r0, %%r0\n" \
    "div1 %%r0, %%r0\n" "div1 %%r0, %%r0\n" \
    "div1 %%r0, %%r0\n" "div1 %%r0, %%r0\n" \
    "div1 %%r0, %%r0\n" "div1 %%r0, %%r0\n" \
    "div1 %%r0, %%r0\n" "div1 %%r0, %%r0\n" \
    "div1 %%r0, %%r0\n" "div1 %%r0, %%r0\n" \
    "div1 %%r0, %%r0\n" "div1 %%r0, %%r0\n" \
    "div1 %%r0, %%r0\n" "div1 %%r0, %%r0\n" \
    "div1 %%r0, %%r0\n" "div1 %%r0, %%r0\n" \
    "div1 %%r0, %%r0\n" "div1 %%r0, %%r0\n" \
    "div1 %%r0, %%r0\n" "div1 %%r0, %%r0\n" \
    "div1 %%r0, %%r0\n" "div1 %%r0, %%r0\n" \
    "div1 %%r0, %%r0\n" "div1 %%r0, %%r0\n" \
    "div1 %%r0, %%r0\n" "div1 %%r0, %%r0\n" \
    "div1 %%r0, %%r0\n" "div1 %%r0, %%r0\n" \
    "div1 %%r0, %%r0\n" "div1 %%r0, %%r0\n"

// Helper: run unsigned division of `dividend` by `divisor` (stored into R0).
// Uses ld.w %%r0, %1 (explicit) and ld.w %%alr, %0 (constraint register).
#define RUN_DIVU(dividend_val, divisor_val) do {            \
    uint32_t _dvd = (dividend_val), _dvs = (divisor_val);   \
    asm volatile(                                            \
        "ld.w %%r0, %1\n"       /* R0 = divisor */          \
        "ld.w %%alr, %0\n"      /* ALR = dividend */        \
        "div0u %%r0, %%r0\n"    /* setup (rs=R0=divisor) */ \
        DIV1_32                                              \
        : : "r"(_dvd), "r"(_dvs) : "r0");                   \
} while (0)

// ============================================================================
// 7 / 2 = quotient 3, remainder 1
// ============================================================================
static int test_divu_7_2(void) {
    RUN_DIVU(7, 2);
    CHECK(read_alr() == 3u, 1);
    CHECK(read_ahr() == 1u, 2);
    return 0;
}

// ============================================================================
// 10 / 3 = quotient 3, remainder 1
// ============================================================================
static int test_divu_10_3(void) {
    RUN_DIVU(10, 3);
    CHECK(read_alr() == 3u, 3);
    CHECK(read_ahr() == 1u, 4);
    return 0;
}

// ============================================================================
// 100 / 7 = quotient 14, remainder 2
// ============================================================================
static int test_divu_100_7(void) {
    RUN_DIVU(100, 7);
    CHECK(read_alr() == 14u, 5);
    CHECK(read_ahr() == 2u, 6);
    return 0;
}

// ============================================================================
// 64 / 8 = 8, remainder 0 (exact)
// ============================================================================
static int test_divu_exact(void) {
    RUN_DIVU(64, 8);
    CHECK(read_alr() == 8u, 7);
    CHECK(read_ahr() == 0u, 8);
    return 0;
}

// ============================================================================
// 1 / 2 = quotient 0, remainder 1 (dividend < divisor)
// ============================================================================
static int test_divu_small(void) {
    RUN_DIVU(1, 2);
    CHECK(read_alr() == 0u, 9);
    CHECK(read_ahr() == 1u, 10);
    return 0;
}

// ============================================================================
// 131071 / 3 = quotient 43690, remainder 1
// 0x0001FFFF = 131071; 131071 / 3 = 43690 r1
// ============================================================================
static int test_divu_large(void) {
    RUN_DIVU(131071u, 3u);
    CHECK(read_alr() == 43690u, 11);
    CHECK(read_ahr() == 1u, 12);
    return 0;
}

int main(void) {
    int e;
    if ((e = test_divu_7_2()))    return e;
    if ((e = test_divu_10_3()))   return e;
    if ((e = test_divu_100_7()))  return e;
    if ((e = test_divu_exact()))  return e;
    if ((e = test_divu_small()))  return e;
    if ((e = test_divu_large()))  return e;
    return 0;
}

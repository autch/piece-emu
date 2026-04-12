// test_div.c — step division instruction tests (Class 4D)
//
// S1C33 has no hardware divider. Division uses a 35-instruction sequence:
//   div0u (or div0s) + 32 × div1 + [div2s + div3s for signed]
//
// Instruction syntax (single %rs operand for all except div3s which has none):
//   div0u %rs   unsigned setup: AHR=0, C=0; rs = divisor
//   div0s %rs   signed setup: AHR=sign(ALR), C=sign(ALR); rs = divisor
//   div1  %rs   one restoring-division step (rs = divisor each time)
//   div2s %rs   signed result correction step 1
//   div3s       signed result correction step 2; ALR = quotient, AHR = remainder
//
// Strategy: keep divisor in R0 throughout.
// Dividend is loaded into ALR before div0u/div0s.
// After 32 div1 steps: ALR = quotient, AHR = remainder (unsigned).

#include <stdint.h>

#define CHECK(cond, code) do { if (!(cond)) return (code); } while (0)

static uint32_t read_alr(void) {
    uint32_t v; asm volatile("ld.w %0, %%alr" : "=r"(v)); return v;
}
static uint32_t read_ahr(void) {
    uint32_t v; asm volatile("ld.w %0, %%ahr" : "=r"(v)); return v;
}

// 32 div1 steps with divisor in R0.
#define DIV1_32 \
    "div1 %%r0\n" "div1 %%r0\n" \
    "div1 %%r0\n" "div1 %%r0\n" \
    "div1 %%r0\n" "div1 %%r0\n" \
    "div1 %%r0\n" "div1 %%r0\n" \
    "div1 %%r0\n" "div1 %%r0\n" \
    "div1 %%r0\n" "div1 %%r0\n" \
    "div1 %%r0\n" "div1 %%r0\n" \
    "div1 %%r0\n" "div1 %%r0\n" \
    "div1 %%r0\n" "div1 %%r0\n" \
    "div1 %%r0\n" "div1 %%r0\n" \
    "div1 %%r0\n" "div1 %%r0\n" \
    "div1 %%r0\n" "div1 %%r0\n" \
    "div1 %%r0\n" "div1 %%r0\n" \
    "div1 %%r0\n" "div1 %%r0\n" \
    "div1 %%r0\n" "div1 %%r0\n" \
    "div1 %%r0\n" "div1 %%r0\n"

// Helper: run unsigned division of `dividend` by `divisor` (stored into R0).
#define RUN_DIVU(dividend_val, divisor_val) do {            \
    uint32_t _dvd = (dividend_val), _dvs = (divisor_val);   \
    asm volatile(                                            \
        "ld.w %%r0, %1\n"  /* R0 = divisor */               \
        "ld.w %%alr, %0\n" /* ALR = dividend */             \
        "div0u %%r0\n"     /* unsigned setup */             \
        DIV1_32                                              \
        : : "r"(_dvd), "r"(_dvs) : "r0");                   \
} while (0)

// Helper: run signed division of `dividend` by `divisor` (stored into R0).
#define RUN_DIVS(dividend_val, divisor_val) do {            \
    int32_t _dvd = (dividend_val), _dvs = (divisor_val);    \
    asm volatile(                                            \
        "ld.w %%r0, %1\n"  /* R0 = divisor */               \
        "ld.w %%alr, %0\n" /* ALR = dividend */             \
        "div0s %%r0\n"     /* signed setup */               \
        DIV1_32                                              \
        "div2s %%r0\n"     /* signed correction step 1 */   \
        "div3s\n"          /* signed correction step 2 */   \
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

// ============================================================================
// Signed: -7 / 2 = quotient -3, remainder -1
// ============================================================================
static int test_divs_neg7_2(void) {
    RUN_DIVS(-7, 2);
    CHECK((int32_t)read_alr() == -3, 13);
    CHECK((int32_t)read_ahr() == -1, 14);
    return 0;
}

// ============================================================================
// Signed: 7 / -2 = quotient -3, remainder 1
// ============================================================================
static int test_divs_7_neg2(void) {
    RUN_DIVS(7, -2);
    CHECK((int32_t)read_alr() == -3, 15);
    CHECK((int32_t)read_ahr() ==  1, 16);
    return 0;
}

// ============================================================================
// Signed: -100 / -7 = quotient 14, remainder -2
// ============================================================================
static int test_divs_neg100_neg7(void) {
    RUN_DIVS(-100, -7);
    CHECK((int32_t)read_alr() ==  14, 17);
    CHECK((int32_t)read_ahr() ==  -2, 18);
    return 0;
}

int main(void) {
    int e;
    if ((e = test_divu_7_2()))         return e;
    if ((e = test_divu_10_3()))        return e;
    if ((e = test_divu_100_7()))       return e;
    if ((e = test_divu_exact()))       return e;
    if ((e = test_divu_small()))       return e;
    if ((e = test_divu_large()))       return e;
    if ((e = test_divs_neg7_2()))      return e;
    if ((e = test_divs_7_neg2()))      return e;
    if ((e = test_divs_neg100_neg7())) return e;
    return 0;
}

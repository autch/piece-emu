// test_alu_flags.c — ALU instruction PSR flag tests
//
// PSR bit layout: N=bit0, Z=bit1, V=bit2, C=bit3
//
// add/sub/cmp: update N,Z,V,C
// and/or/xor/not: update N,Z only (V and C are NOT cleared by and/or/xor on S1C33)

#include <stdint.h>

#define PSR_N  0x01
#define PSR_Z  0x02
#define PSR_V  0x04
#define PSR_C  0x08

#define CHECK(cond, code) do { if (!(cond)) return (code); } while (0)

// ============================================================================
// ADD flags
// ============================================================================

// 0 + 0 = 0: Z=1, N=0, V=0, C=0
static int test_add_zero(void) {
    uint32_t a = 0, b = 0, psr;
    asm volatile("add %1, %2\n" "ld.w %0, %%psr\n"
        : "=r"(psr), "+r"(a) : "r"(b));
    CHECK((psr & (PSR_N|PSR_Z|PSR_V|PSR_C)) == PSR_Z, 1);
    return 0;
}

// 0x7FFFFFFF + 1 = 0x80000000: N=1, V=1, Z=0, C=0
static int test_add_signed_overflow(void) {
    uint32_t a = 0x7FFFFFFFu, b = 1, psr;
    asm volatile("add %1, %2\n" "ld.w %0, %%psr\n"
        : "=r"(psr), "+r"(a) : "r"(b));
    CHECK((psr & (PSR_N|PSR_Z|PSR_V|PSR_C)) == (PSR_N|PSR_V), 2);
    return 0;
}

// 0xFFFFFFFF + 1 = 0 (carry out): Z=1, C=1, N=0, V=0
static int test_add_carry(void) {
    uint32_t a = 0xFFFFFFFFu, b = 1, psr;
    asm volatile("add %1, %2\n" "ld.w %0, %%psr\n"
        : "=r"(psr), "+r"(a) : "r"(b));
    CHECK((psr & (PSR_N|PSR_Z|PSR_V|PSR_C)) == (PSR_Z|PSR_C), 3);
    return 0;
}

// 0x80000000 + 0x80000000 = 0: Z=1, V=1, C=1
static int test_add_both_overflow(void) {
    uint32_t a = 0x80000000u, b = 0x80000000u, psr;
    asm volatile("add %1, %2\n" "ld.w %0, %%psr\n"
        : "=r"(psr), "+r"(a) : "r"(b));
    CHECK((psr & (PSR_N|PSR_Z|PSR_V|PSR_C)) == (PSR_Z|PSR_V|PSR_C), 4);
    return 0;
}

// 1 + (-2) = -1: N=1, Z=0, V=0, C=1 (unsigned 1 + 0xFFFFFFFE = 0xFFFFFFFF, no carry)
// Actually 1 + 0xFFFFFFFE = 0xFFFFFFFF, no carry out → C=0
static int test_add_negative_result(void) {
    uint32_t a = 1, b = 0xFFFFFFFEu, psr; /* b = -2 */
    asm volatile("add %1, %2\n" "ld.w %0, %%psr\n"
        : "=r"(psr), "+r"(a) : "r"(b));
    CHECK((psr & (PSR_N|PSR_Z|PSR_V|PSR_C)) == PSR_N, 5);
    return 0;
}

// ============================================================================
// SUB flags
// ============================================================================

// 5 - 5 = 0: Z=1
static int test_sub_zero(void) {
    uint32_t a = 5, b = 5, psr;
    asm volatile("sub %1, %2\n" "ld.w %0, %%psr\n"
        : "=r"(psr), "+r"(a) : "r"(b));
    CHECK((psr & (PSR_N|PSR_Z|PSR_V|PSR_C)) == PSR_Z, 6);
    return 0;
}

// 0 - 1 = -1: N=1, C=1 (borrow)
static int test_sub_borrow(void) {
    uint32_t a = 0, b = 1, psr;
    asm volatile("sub %1, %2\n" "ld.w %0, %%psr\n"
        : "=r"(psr), "+r"(a) : "r"(b));
    CHECK((psr & (PSR_N|PSR_Z|PSR_V|PSR_C)) == (PSR_N|PSR_C), 7);
    return 0;
}

// 0x80000000 - 1 = 0x7FFFFFFF: signed overflow → V=1, N=0
static int test_sub_signed_underflow(void) {
    uint32_t a = 0x80000000u, b = 1, psr;
    asm volatile("sub %1, %2\n" "ld.w %0, %%psr\n"
        : "=r"(psr), "+r"(a) : "r"(b));
    CHECK((psr & (PSR_N|PSR_Z|PSR_V|PSR_C)) == PSR_V, 8);
    return 0;
}

// ============================================================================
// CMP flags (same as sub but no result written)
// ============================================================================

// cmp rd=5 with imm6=5 → Z=1
static int test_cmp_eq(void) {
    uint32_t r = 5, psr;
    asm volatile("cmp %1, 5\n" "ld.w %0, %%psr\n"
        : "=r"(psr) : "r"(r));
    CHECK((psr & (PSR_N|PSR_Z|PSR_V|PSR_C)) == PSR_Z, 9);
    return 0;
}

// cmp rd=3 with imm6=7: 3 < 7 → N=1, C=1
static int test_cmp_lt(void) {
    uint32_t r = 3, psr;
    asm volatile("cmp %1, 7\n" "ld.w %0, %%psr\n"
        : "=r"(psr) : "r"(r));
    CHECK((psr & (PSR_N|PSR_Z|PSR_V|PSR_C)) == (PSR_N|PSR_C), 10);
    return 0;
}

// cmp does NOT write result: rd should be unchanged
static int test_cmp_no_write(void) {
    uint32_t val = 42, other = 10;
    asm volatile("cmp %0, %1\n" : : "r"(val), "r"(other));
    CHECK(val == 42u, 11);
    return 0;
}

// ============================================================================
// AND/OR/XOR/NOT flags (N, Z only — V and C are NOT updated)
// ============================================================================

// 5 & 2 = 0: Z=1
static int test_and_zero(void) {
    uint32_t a = 5, b = 2, psr;
    asm volatile("and %1, %2\n" "ld.w %0, %%psr\n"
        : "=r"(psr), "+r"(a) : "r"(b));
    CHECK((psr & (PSR_N|PSR_Z)) == PSR_Z, 12);
    return 0;
}

// 0 | 0x80000000 = 0x80000000: N=1
static int test_or_negative(void) {
    uint32_t a = 0, b = 0x80000000u, psr;
    asm volatile("or %1, %2\n" "ld.w %0, %%psr\n"
        : "=r"(psr), "+r"(a) : "r"(b));
    CHECK((psr & PSR_N) == PSR_N, 13);
    return 0;
}

// 0xAA ^ 0xAA = 0: Z=1
static int test_xor_zero(void) {
    uint32_t a = 0xAAu, b = 0xAAu, psr;
    asm volatile("xor %1, %2\n" "ld.w %0, %%psr\n"
        : "=r"(psr), "+r"(a) : "r"(b));
    CHECK((psr & (PSR_N|PSR_Z)) == PSR_Z, 14);
    return 0;
}

// not %rd, 0 = ~0 = 0xFFFFFFFF: N=1
static int test_not_imm(void) {
    uint32_t val, psr;
    asm volatile("not %0, 0\n" "ld.w %1, %%psr\n" : "=r"(val), "=r"(psr));
    CHECK(val == 0xFFFFFFFFu, 15);
    CHECK((psr & PSR_N) == PSR_N, 16);
    return 0;
}

// ============================================================================
// ADD immediate flag test (imm6 unsigned zero-extension)
// ============================================================================

// add %rd, 31 where rd=0: result=31, Z=0, N=0
static int test_add_imm6(void) {
    uint32_t val = 0, psr;
    asm volatile("add %0, 31\n" "ld.w %1, %%psr\n" : "+r"(val), "=r"(psr));
    CHECK(val == 31u, 17);
    CHECK((psr & (PSR_N|PSR_Z)) == 0, 18);
    return 0;
}

int main(void) {
    int e;
    if ((e = test_add_zero()))           return e;
    if ((e = test_add_signed_overflow())) return e;
    if ((e = test_add_carry()))          return e;
    if ((e = test_add_both_overflow()))  return e;
    if ((e = test_add_negative_result())) return e;
    if ((e = test_sub_zero()))           return e;
    if ((e = test_sub_borrow()))         return e;
    if ((e = test_sub_signed_underflow())) return e;
    if ((e = test_cmp_eq()))             return e;
    if ((e = test_cmp_lt()))             return e;
    if ((e = test_cmp_no_write()))       return e;
    if ((e = test_and_zero()))           return e;
    if ((e = test_or_negative()))        return e;
    if ((e = test_xor_zero()))           return e;
    if ((e = test_not_imm()))            return e;
    if ((e = test_add_imm6()))           return e;
    return 0;
}

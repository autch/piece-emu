// test_misc.c — miscellaneous instruction tests
//
// Covers: scan0, scan1, swap, mirror (Class 4D)
//         adc, sbc (Class 5A)
//         btst, bclr, bset, bnot (Class 5A)
//         pushn, popn (Class 0A)
//         ld.w %special, %rs / ld.w %rd, %special (Class 5A)

#include <stdint.h>

#define PSR_N 0x01
#define PSR_Z 0x02
#define PSR_C 0x08

#define CHECK(cond, code) do { if (!(cond)) return (code); } while (0)

// ============================================================================
// scan0: scan upper 8 bits of rs for first 0 bit from MSB.
// Returns 0..7 = offset of first 0; 8 = not found (C flag set).
// ============================================================================
static int test_scan0(void) {
    uint32_t r;

    // upper 8 = 0x00: first 0 at offset 0 → 0
    uint32_t v1 = 0;
    asm volatile("scan0 %0, %1\n" : "=r"(r) : "r"(v1));
    CHECK(r == 0u, 1);

    // upper 8 = 0x80 = 1000 0000: first 0 at offset 1 → 1
    uint32_t v2 = 0x80000000u;
    asm volatile("scan0 %0, %1\n" : "=r"(r) : "r"(v2));
    CHECK(r == 1u, 2);

    // upper 8 = 0xFF: no 0 found → 8
    uint32_t v3 = 0xFF000000u;
    asm volatile("scan0 %0, %1\n" : "=r"(r) : "r"(v3));
    CHECK(r == 8u, 3);

    // upper 8 = 0xFE = 1111 1110: first 0 at offset 7 → 7
    uint32_t v4 = 0xFE000000u;
    asm volatile("scan0 %0, %1\n" : "=r"(r) : "r"(v4));
    CHECK(r == 7u, 4);

    return 0;
}

// ============================================================================
// scan1: scan upper 8 bits of rs for first 1 bit from MSB.
// Returns 0..7 = offset of first 1; 8 = not found (C flag set).
// ============================================================================
static int test_scan1(void) {
    uint32_t r;

    // upper 8 = 0xFF: first 1 at offset 0 → 0
    uint32_t v1 = 0xFFFFFFFFu;
    asm volatile("scan1 %0, %1\n" : "=r"(r) : "r"(v1));
    CHECK(r == 0u, 5);

    // upper 8 = 0x00: no 1 found → 8
    uint32_t v2 = 1;
    asm volatile("scan1 %0, %1\n" : "=r"(r) : "r"(v2));
    CHECK(r == 8u, 6);

    // upper 8 = 0x00: no 1 found → 8
    uint32_t v3 = 0;
    asm volatile("scan1 %0, %1\n" : "=r"(r) : "r"(v3));
    CHECK(r == 8u, 7);

    // upper 8 = 0x01 = 0000 0001: first 1 at offset 7 → 7
    uint32_t v4 = 0x01000000u;
    asm volatile("scan1 %0, %1\n" : "=r"(r) : "r"(v4));
    CHECK(r == 7u, 8);

    return 0;
}

// ============================================================================
// swap: reverse byte order (big-endian swap)
// rd(31:24)←rs(7:0), rd(23:16)←rs(15:8), rd(15:8)←rs(23:16), rd(7:0)←rs(31:24)
// manual example: swap(0x87654321) = 0x21436587
// ============================================================================
static int test_swap(void) {
    uint32_t r;

    static const uint32_t v1 = 0x87654321u;
    asm volatile("swap %0, %1\n" : "=r"(r) : "r"(v1));
    CHECK(r == 0x21436587u, 9);

    static const uint32_t v2 = 0x12345678u;
    asm volatile("swap %0, %1\n" : "=r"(r) : "r"(v2));
    CHECK(r == 0x78563412u, 10);

    return 0;
}

// ============================================================================
// mirror: reverse bits within each byte
// rd(31:24)←rs(24:31), rd(23:16)←rs(16:23), rd(15:8)←rs(8:15), rd(7:0)←rs(0:7)
// manual example: mirror(0x88442211) = 0x11224488
//                 mirror(0x11223344) = 0x8844CC22
// ============================================================================
static int test_mirror(void) {
    uint32_t r;

    static const uint32_t v1 = 0x88442211u;
    asm volatile("mirror %0, %1\n" : "=r"(r) : "r"(v1));
    CHECK(r == 0x11224488u, 11);

    static const uint32_t v2 = 0x11223344u;
    asm volatile("mirror %0, %1\n" : "=r"(r) : "r"(v2));
    CHECK(r == 0x8844CC22u, 12);

    return 0;
}

// ============================================================================
// adc: rd = rd + rs + C
// Set C via 0xFFFFFFFF + 1 = 0 (carry), then adc.
// Use reg-to-reg ld.w (which does NOT modify PSR) to load operands.
// ============================================================================
static int test_adc(void) {
    uint32_t r, psr;
    uint32_t five = 5, three = 3;

    // C=1: not %r, 0 → 0xFFFFFFFF; add %r, 1 → C=1; ld.w reg (copy, no PSR); adc
    // Use early clobber (&) so %0's register differs from all inputs.
    asm volatile(
        "not %0, 0\n"        // %0 = 0xFFFFFFFF
        "add %0, 1\n"        // %0 = 0, C=1
        "ld.w %0, %2\n"      // %0 = 5 (reg-to-reg copy, preserves C)
        "adc %0, %3\n"       // 5 + 3 + 1 = 9
        "ld.w %1, %%psr\n"
        : "=&r"(r), "=r"(psr)
        : "r"(five), "r"(three));
    CHECK(r == 9u, 13);
    CHECK((psr & PSR_C) == 0, 14);

    // C=0: cmp x, x → Z=1, C=0; then adc
    asm volatile(
        "cmp %2, %2\n"       // x - x = 0, C=0 (no borrow)
        "ld.w %0, %3\n"      // %0 = 5 (reg-to-reg, preserves C=0)
        "adc %0, %4\n"       // 5 + 3 + 0 = 8
        : "=r"(r)
        : "r"(r), "r"(r), "r"(five), "r"(three));
    CHECK(r == 8u, 15);

    return 0;
}

// ============================================================================
// sbc: rd = rd - rs - C
// ============================================================================
static int test_sbc(void) {
    uint32_t r;
    uint32_t ten = 10, three = 3;

    // C=0: cmp x, x → C=0; sbc 10-3-0 = 7
    asm volatile(
        "cmp %2, %2\n"       // C=0
        "ld.w %0, %3\n"      // %0 = 10
        "sbc %0, %4\n"       // 10 - 3 - 0 = 7
        : "=r"(r)
        : "r"(r), "r"(r), "r"(ten), "r"(three));
    CHECK(r == 7u, 16);

    // C=1: not %t, 0 → 0xFFFFFFFF; add %t, 1 → C=1; sbc 10-3-1 = 6
    // Use early clobber (&) so %0's register differs from all inputs.
    asm volatile(
        "not %0, 0\n"        // %0 = 0xFFFFFFFF
        "add %0, 1\n"        // C=1
        "ld.w %0, %1\n"      // %0 = 10
        "sbc %0, %2\n"       // 10 - 3 - 1 = 6
        : "=&r"(r)
        : "r"(ten), "r"(three));
    CHECK(r == 6u, 17);

    return 0;
}

// ============================================================================
// btst/bclr/bset/bnot: bit operations on memory byte
// Instruction format: bset/bclr/bnot/btst [rb], imm3
// ============================================================================
static uint8_t bitmem;

static int test_bit_ops(void) {
    uint32_t psr;

    // bset [rb], 3 → set bit 3 of byte at bitmem
    bitmem = 0x00;
    asm volatile("bset [%0], 3\n" : : "r"(&bitmem) : "memory");
    CHECK(bitmem == 0x08u, 18);

    // btst [rb], 3 → test bit 3 (set) → Z=0
    asm volatile("btst [%1], 3\n" "ld.w %0, %%psr\n" : "=r"(psr) : "r"(&bitmem));
    CHECK((psr & PSR_Z) == 0, 19); // Z=0: bit IS set

    // btst [rb], 2 → test bit 2 (clear) → Z=1
    asm volatile("btst [%1], 2\n" "ld.w %0, %%psr\n" : "=r"(psr) : "r"(&bitmem));
    CHECK((psr & PSR_Z) == PSR_Z, 20); // Z=1: bit NOT set

    // bclr [rb], 3 → clear bit 3
    asm volatile("bclr [%0], 3\n" : : "r"(&bitmem) : "memory");
    CHECK(bitmem == 0x00u, 21);

    // bnot [rb], 5 → toggle bit 5
    // 0xAA = 1010 1010. bit5=1. After bnot: 1010 1010 ^ 0010 0000 = 1000 1010 = 0x8A
    bitmem = 0xAA;
    asm volatile("bnot [%0], 5\n" : : "r"(&bitmem) : "memory");
    CHECK(bitmem == 0x8Au, 22);

    return 0;
}

// ============================================================================
// pushn / popn: push/pop register ranges R0..RN
// pushn %rN: pushes R0..RN (rN at top of stack); popn %rN: pops back to R0..RN
//
// These instructions operate on fixed register ranges R0..RN, so we must use
// explicit register names. To avoid clobber/output conflicts, we store results
// to memory and read with C, keeping output constraints on address registers only.
// ============================================================================
static int test_pushn_popn(void) {
    static uint32_t out_a, out_b, out_c;

    // Save r0=111, r1=222, r2=333 via pushn/popn; read results to memory.
    // 111/222/333 exceed simm6 range [-32,31]; pass as "r" constraints and
    // copy to explicit r0/r1/r2 via reg-to-reg ld.w.
    uint32_t v111 = 111, v222 = 222, v333 = 333;
    asm volatile(
        "ld.w %%r0, %3\n"        // r0 = 111 (reg-to-reg from constraint)
        "ld.w %%r1, %4\n"        // r1 = 222
        "ld.w %%r2, %5\n"        // r2 = 333
        "pushn %%r2\n"           // push r0, r1, r2
        "ld.w %%r0, 0\n"
        "ld.w %%r1, 0\n"
        "ld.w %%r2, 0\n"
        "popn %%r2\n"            // pop r0, r1, r2
        "ld.w [%0], %%r0\n"      // out_a = r0
        "ld.w [%1], %%r1\n"      // out_b = r1
        "ld.w [%2], %%r2\n"      // out_c = r2
        :
        : "r"(&out_a), "r"(&out_b), "r"(&out_c),
          "r"(v111), "r"(v222), "r"(v333)
        : "r0", "r1", "r2", "memory");
    CHECK(out_a == 111u, 23);
    CHECK(out_b == 222u, 24);
    CHECK(out_c == 333u, 25);

    // pushn %r0 / popn %r0: only R0 (42 > 31, use "r" constraint)
    uint32_t v42 = 42;
    asm volatile(
        "ld.w %%r0, %1\n"        // r0 = 42
        "pushn %%r0\n"
        "ld.w %%r0, 0\n"
        "popn %%r0\n"
        "ld.w [%0], %%r0\n"
        :
        : "r"(&out_a), "r"(v42)
        : "r0", "memory");
    CHECK(out_a == 42u, 26);

    return 0;
}

// ============================================================================
// Special register access: ALR, AHR, PSR via ld.w %special, %rd
// ============================================================================
static int test_special_regs(void) {
    uint32_t r;

    // Write and read ALR
    uint32_t dead = 0xDEADu;
    asm volatile(
        "ld.w %%alr, %1\n"
        "ld.w %0, %%alr\n"
        : "=r"(r) : "r"(dead));
    CHECK(r == 0xDEADu, 27);

    // Write and read AHR
    uint32_t beef = 0xBEEFu;
    asm volatile(
        "ld.w %%ahr, %1\n"
        "ld.w %0, %%ahr\n"
        : "=r"(r) : "r"(beef));
    CHECK(r == 0xBEEFu, 28);

    // Write PSR=0 (clear flags), read back and check NZVC bits
    uint32_t zero = 0;
    asm volatile(
        "ld.w %%psr, %1\n"
        "ld.w %0, %%psr\n"
        : "=r"(r) : "r"(zero));
    CHECK((r & 0x0F) == 0u, 29);

    return 0;
}

int main(void) {
    int e;
    if ((e = test_scan0()))        return e;
    if ((e = test_scan1()))        return e;
    if ((e = test_swap()))         return e;
    if ((e = test_mirror()))       return e;
    if ((e = test_adc()))          return e;
    if ((e = test_sbc()))          return e;
    if ((e = test_bit_ops()))      return e;
    if ((e = test_pushn_popn()))   return e;
    if ((e = test_special_regs())) return e;
    return 0;
}

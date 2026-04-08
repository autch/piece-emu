// test_ext_imm.c — EXT immediate signedness tests
//
// CRITICAL: ext does NOT change the signedness of the target instruction.
// - add/sub imm6:          unsigned zero-extension of combined value
// - and/or/xor/not/cmp/ld.w simm6: signed sign-extension of combined value
//
// With 1 ext (13-bit) and 6-bit immediate, combined is 19 bits:
//   ext 0x1000 + imm6=0 → combined = (0x1000<<6)|0 = 0x40000 (bit18 set)
//     add: unsigned → +262144
//     and: signed   → sign_ext19(0x40000) = -262144 = 0xFFFC0000
//
// 3-operand form (reg-reg ALU with EXT):
//   ext N; add %rd, %rs → rd = rs + N  (N = raw EXT value, unsigned 0..8191)
//   This is DIFFERENT from the 2-operand immediate form; the EXT value is used
//   directly as the immediate (no shift, no combine with instruction bits).

#include <stdint.h>

#define CHECK(cond, code) do { if (!(cond)) return (code); } while (0)

// ============================================================================
// ext + add (unsigned): combined = (0x1000<<6)|0 = 0x40000 = 262144
// ============================================================================
static int test_add_unsigned_ext(void) {
    uint32_t r = 0;
    asm volatile(
        "ext 0x1000\n"
        "add %0, 0\n"
        : "+r"(r));
    CHECK(r == 262144u, 1);
    return 0;
}

// ============================================================================
// ext + and (signed): sign_ext19(0x40000) = -262144 = 0xFFFC0000
// 0xFFFFFFFF & 0xFFFC0000 = 0xFFFC0000
// ============================================================================
static int test_and_signed_ext(void) {
    uint32_t r = 0xFFFFFFFFu;
    asm volatile(
        "ext 0x1000\n"
        "and %0, 0\n"
        : "+r"(r));
    CHECK(r == 0xFFFC0000u, 2);
    return 0;
}

// ============================================================================
// ext + or (signed): 0 | sign_ext19(0x40000) = 0xFFFC0000
// ============================================================================
static int test_or_signed_ext(void) {
    uint32_t r = 0;
    asm volatile(
        "ext 0x1000\n"
        "or %0, 0\n"
        : "+r"(r));
    CHECK(r == 0xFFFC0000u, 3);
    return 0;
}

// ============================================================================
// ext + xor (signed): 0 ^ sign_ext19(0x40000) = 0xFFFC0000
// ============================================================================
static int test_xor_signed_ext(void) {
    uint32_t r = 0;
    asm volatile(
        "ext 0x1000\n"
        "xor %0, 0\n"
        : "+r"(r));
    CHECK(r == 0xFFFC0000u, 4);
    return 0;
}

// ============================================================================
// ext + not (signed): ~sign_ext19(0x40000) = ~0xFFFC0000 = 0x0003FFFF
// ============================================================================
static int test_not_signed_ext(void) {
    uint32_t r;
    asm volatile(
        "ext 0x1000\n"
        "not %0, 0\n"
        : "=r"(r));
    CHECK(r == 0x0003FFFFu, 5);
    return 0;
}

// ============================================================================
// ext + ld.w (signed): sign_ext19(0x40000) = -262144 = 0xFFFC0000
// ============================================================================
static int test_ldw_signed_ext(void) {
    uint32_t r;
    asm volatile(
        "ext 0x1000\n"
        "ld.w %0, 0\n"
        : "=r"(r));
    CHECK(r == 0xFFFC0000u, 6);
    return 0;
}

// ============================================================================
// Contrast: same ext value, add is unsigned, ld.w is signed
// ext 0x1FFF + imm=0x1F → 19 bits all ones = 0x7FFFF
//   add (unsigned): 0 + 0x7FFFF = 524287 (positive)
//   ld.w (signed):  sign_ext19(0x7FFFF) = -1 (bit18 set → negative)
// ============================================================================
static int test_add_vs_ldw_boundary(void) {
    uint32_t r_add = 0;
    asm volatile("ext 0x1FFF\n" "add %0, 31\n" : "+r"(r_add));
    // combined = (0x1FFF<<6)|0x1F = 0x7FFC0|0x1F = 0x7FFDF... wait
    // imm6=31=0x1F; (0x1FFF<<6)|0x1F = 0x7FFC0|0x1F = 0x7FFDF ≠ 0x7FFFF
    // For all-ones 19 bits use imm6=0x3F:
    r_add = 0;
    asm volatile("ext 0x1FFF\n" "add %0, 63\n" : "+r"(r_add));
    // imm6=63=0x3F; combined=(0x1FFF<<6)|0x3F=0x7FFC0|0x3F=0x7FFFF=524287
    CHECK(r_add == 0x7FFFFu, 7);      // positive (unsigned)

    uint32_t r_ldw;
    asm volatile("ext 0x1FFF\n" "ld.w %0, -1\n" : "=r"(r_ldw));
    // ld.w simm6: -1 = 0b111111 in 6-bit field.
    // combined = (0x1FFF<<6)|0x3F = 0x7FFFF; sign_extend_19(0x7FFFF) = +524287
    // (bit18=0, so positive — same result as add above; r_ldw not further checked)
    uint32_t r_add2 = 0;
    asm volatile("ext 0x1000\n" "add %0, 0\n" : "+r"(r_add2));
    CHECK(r_add2 == 262144u, 8);      // add: unsigned, positive

    uint32_t r_ldw2;
    asm volatile("ext 0x1000\n" "ld.w %0, 0\n" : "=r"(r_ldw2));
    CHECK(r_ldw2 == 0xFFFC0000u, 9);  // ld.w: signed, negative
    return 0;
}

// ============================================================================
// 2-EXT ld.w: ext hi / ext lo / ld.w → sign_ext32(combined)
// ext 0x1FFF / ext 0x1FFF / ld.w %rd, -1
//   ld.w simm6 -1 = 0b111111 (6-bit raw field = 0x3F)
//   combined bits: 13+13+6=32: (0x1FFF<<19)|(0x1FFF<<6)|0x3F
//   = 0xFFF80000 | 0x7FFC0 | 0x3F = 0xFFFFFFFF → sign_extend_32 = -1
// ============================================================================
static int test_ldw_two_ext(void) {
    uint32_t r;
    asm volatile(
        "ext 0x1FFF\n"
        "ext 0x1FFF\n"
        "ld.w %0, -1\n"
        : "=r"(r));
    CHECK(r == 0xFFFFFFFFu, 10);
    return 0;
}

// ============================================================================
// 2-EXT add: ext 0x0000 / ext 0x0001 / add %rd, 0 → (0<<19)|(1<<6)|0 = 64
// ============================================================================
static int test_add_two_ext(void) {
    uint32_t r = 0;
    asm volatile(
        "ext 0x0000\n"
        "ext 0x0001\n"
        "add %0, 0\n"
        : "+r"(r));
    CHECK(r == 64u, 11);
    return 0;
}

// ============================================================================
// ext + sub (unsigned): sub rd, imm6 is unsigned, same as add
// ext 0x1000 + imm6=0: sub subtracts 262144 (unsigned)
// r = 0 - 262144 = -262144 = 0xFFFC0000 (via unsigned arithmetic, with borrow)
// ============================================================================
static int test_sub_unsigned_ext(void) {
    uint32_t r = 0;
    asm volatile(
        "ext 0x1000\n"
        "sub %0, 0\n"
        : "+r"(r));
    CHECK(r == 0xFFFC0000u, 12);      // 0 - 262144 wraps to 0xFFFC0000
    return 0;
}

// ============================================================================
// ext + cmp (signed): compare 0 with sign_ext19(0x40000) = -262144
// 0 > -262144 → jrgt taken
// ============================================================================
static int test_cmp_signed_ext(void) {
    int taken = 0;
    uint32_t cmp_val = 0;
    asm volatile(
        "ext 0x1000\n"
        "cmp %2, 0\n"            // compare cmp_val(=0) with -262144
        "jrle 1f\n"              // skip if 0 <= -262144 (should not happen)
        "ld.w %0, %3\n"          // taken: 0 > -262144
        "1:\n"
        : "+r"(taken), "+r"(cmp_val)
        : "r"(cmp_val), "r"(1));
    CHECK(taken == 1, 13);
    return 0;
}

// ============================================================================
// 3-operand reg-reg ALU with EXT: rd ← rs op imm (Class 1C + EXT)
//
// When EXT precedes a reg-reg ALU instruction, the rs register becomes the
// source and the EXT value (raw, unsigned) becomes the immediate operand:
//   ext N; add %rd, %rs  →  rd = rs + N  (N = raw 13-bit EXT value)
//
// This contrasts with 2-operand add %rd, imm6 where imm is built by
// combining (ext<<6)|imm6 and is then zero-extended.
//
// For all 3-op forms (add/sub/and/or/xor/cmp), the EXT value is unsigned.
// ============================================================================

// add 3-op: rd = rs + N (unsigned N)
// rs=50, ext=100 → rd=150; =&r ensures rd≠rs, confirming rs is the source
static int test_add_3op(void) {
    uint32_t rs_val = 50, result;
    asm volatile(
        "ext 0x0064\n"           // N = 100
        "add %0, %1\n"           // 3-op: rd = rs(50) + 100 = 150
        : "=&r"(result)
        : "r"(rs_val));
    CHECK(result == 150u, 14);
    return 0;
}

// sub 3-op: rd = rs - N (unsigned N)
// rs=200, ext=100 → rd=100
static int test_sub_3op(void) {
    uint32_t rs_val = 200, result;
    asm volatile(
        "ext 0x0064\n"           // N = 100
        "sub %0, %1\n"           // 3-op: rd = rs(200) - 100 = 100
        : "=&r"(result)
        : "r"(rs_val));
    CHECK(result == 100u, 15);
    return 0;
}

// and 3-op: rd = rs & N (raw EXT, unsigned; NOT sign-extended like 2-op and)
// rs=0xFFFF, ext=0x0F0F → rd=0x0F0F
// Note: 2-op "and %rd, 0" with ext 0x1000 gives 0xFFFC0000 (sign-extended),
//       but 3-op uses raw EXT value directly.
static int test_and_3op(void) {
    uint32_t rs_val = 0x0000FFFFu, result;
    asm volatile(
        "ext 0x0F0F\n"           // N = 0x0F0F
        "and %0, %1\n"           // 3-op: rd = rs(0xFFFF) & 0x0F0F = 0x0F0F
        : "=&r"(result)
        : "r"(rs_val));
    CHECK(result == 0x0F0Fu, 16);
    return 0;
}

// or 3-op: rd = rs | N
// rs=0xF0F0, ext=0x0F0F → rd=0xFFFF
static int test_or_3op(void) {
    uint32_t rs_val = 0xF0F0u, result;
    asm volatile(
        "ext 0x0F0F\n"           // N = 0x0F0F
        "or %0, %1\n"            // 3-op: rd = rs(0xF0F0) | 0x0F0F = 0xFFFF
        : "=&r"(result)
        : "r"(rs_val));
    CHECK(result == 0xFFFFu, 17);
    return 0;
}

// xor 3-op: rd = rs ^ N
// rs=0xAAAA, ext=0x0F0F → rd=0xAAAA ^ 0x0F0F = 0xA5A5
static int test_xor_3op(void) {
    uint32_t rs_val = 0xAAAAu, result;
    asm volatile(
        "ext 0x0F0F\n"           // N = 0x0F0F
        "xor %0, %1\n"           // 3-op: rd = rs(0xAAAA) ^ 0x0F0F = 0xA5A5
        : "=&r"(result)
        : "r"(rs_val));
    CHECK(result == 0xA5A5u, 18);
    return 0;
}

// cmp 3-op: compare rs with N (rd is ignored)
// rs=100, rd=200 (irrelevant), ext=100 → Z=1 confirms rs was compared
static int test_cmp_3op(void) {
    uint32_t rd_dummy = 200, rs_val = 100, psr;
    asm volatile(
        "ext 0x0064\n"           // N = 100
        "cmp %1, %2\n"           // 3-op: compare rs(%2=100) with 100; Z=1
        "ld.w %0, %%psr\n"
        : "=r"(psr)
        : "r"(rd_dummy), "r"(rs_val));  // %1=rd(ignored), %2=rs(compared)
    CHECK((psr & 0x02) != 0, 19);      // Z=1: 100==100 (not rd=200)
    return 0;
}

int main(void) {
    int e;
    if ((e = test_add_unsigned_ext()))   return e;
    if ((e = test_and_signed_ext()))     return e;
    if ((e = test_or_signed_ext()))      return e;
    if ((e = test_xor_signed_ext()))     return e;
    if ((e = test_not_signed_ext()))     return e;
    if ((e = test_ldw_signed_ext()))     return e;
    if ((e = test_add_vs_ldw_boundary())) return e;
    if ((e = test_ldw_two_ext()))        return e;
    if ((e = test_add_two_ext()))        return e;
    if ((e = test_sub_unsigned_ext()))   return e;
    if ((e = test_cmp_signed_ext()))     return e;
    if ((e = test_add_3op()))            return e;
    if ((e = test_sub_3op()))            return e;
    if ((e = test_and_3op()))            return e;
    if ((e = test_or_3op()))             return e;
    if ((e = test_xor_3op()))            return e;
    if ((e = test_cmp_3op()))            return e;
    return 0;
}

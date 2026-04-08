// test_multiply.c — multiply and MAC instruction tests (Class 5C)
//
// Instructions: mlt.h, mltu.h, mlt.w, mltu.w, mac
//
// mlt.h:  ALR:AHR = sign_ext16(rd) * sign_ext16(rs)   (16×16 signed → 64)
// mltu.h: ALR:AHR = zero_ext16(rd) * zero_ext16(rs)   (16×16 unsigned → 64)
// mlt.w:  ALR:AHR = sign32(rd) * sign32(rs)            (32×32 signed → 64)
// mltu.w: ALR:AHR = uint32(rd) * uint32(rs)            (32×32 unsigned → 64)
// mac:    ALR:AHR += R0 * rs   (rd field fixed to 0000 = R0 in encoding)
//         Syntax: mac %rs   (R0 is the implicit first multiplier)

#include <stdint.h>

#define CHECK(cond, code) do { if (!(cond)) return (code); } while (0)

static uint32_t read_alr(void) {
    uint32_t v; asm volatile("ld.w %0, %%alr" : "=r"(v)); return v;
}
static uint32_t read_ahr(void) {
    uint32_t v; asm volatile("ld.w %0, %%ahr" : "=r"(v)); return v;
}

// ============================================================================
// mlt.h: signed 16×16 multiply
// ============================================================================
static int test_mlt_h_positive(void) {
    // 3 * 4 = 12
    uint32_t a = 3, b = 4;
    asm volatile(
        "ld.w %%r0, %0\n"
        "ld.w %%r1, %1\n"
        "mlt.h %%r0, %%r1\n"
        : : "r"(a), "r"(b) : "r0", "r1");
    CHECK(read_alr() == 12u, 1);
    CHECK(read_ahr() == 0u, 2);
    return 0;
}

static int test_mlt_h_negative(void) {
    // -1 * 5 = -5: ALR=0xFFFFFFFB, AHR=0xFFFFFFFF
    uint32_t a = 0xFFFFFFFFu, b = 5; /* a low16=0xFFFF=-1 signed */
    asm volatile(
        "ld.w %%r0, %0\n"
        "ld.w %%r1, %1\n"
        "mlt.h %%r0, %%r1\n"
        : : "r"(a), "r"(b) : "r0", "r1");
    CHECK(read_alr() == 0xFFFFFFFBu, 3);
    CHECK(read_ahr() == 0xFFFFFFFFu, 4);
    return 0;
}

static int test_mlt_h_max(void) {
    // 0x7FFF * 0x7FFF = 32767 * 32767 = 1073676289 = 0x3FFF0001
    uint32_t a = 0x7FFF;
    asm volatile(
        "ld.w %%r0, %0\n"
        "ld.w %%r1, %%r0\n"
        "mlt.h %%r0, %%r1\n"
        : : "r"(a) : "r0", "r1");
    CHECK(read_alr() == 0x3FFF0001u, 5);
    CHECK(read_ahr() == 0u, 6);
    return 0;
}

// ============================================================================
// mltu.h: unsigned 16×16 multiply
// ============================================================================
static int test_mltu_h(void) {
    // 0xFFFF * 0xFFFF = 65535 * 65535 = 4294836225 = 0xFFFE0001
    uint32_t a = 0xFFFF;
    asm volatile(
        "ld.w %%r0, %0\n"
        "ld.w %%r1, %%r0\n"
        "mltu.h %%r0, %%r1\n"
        : : "r"(a) : "r0", "r1");
    CHECK(read_alr() == 0xFFFE0001u, 7);
    CHECK(read_ahr() == 0u, 8);

    // 256 * 256 = 65536 = 0x00010000
    uint32_t b = 256;
    asm volatile(
        "ld.w %%r0, %0\n"
        "ld.w %%r1, %%r0\n"
        "mltu.h %%r0, %%r1\n"
        : : "r"(b) : "r0", "r1");
    CHECK(read_alr() == 65536u, 9);
    CHECK(read_ahr() == 0u, 10);
    return 0;
}

// ============================================================================
// mlt.w: signed 32×32 multiply
// ============================================================================
static int test_mlt_w_small(void) {
    // 100 * 200 = 20000
    uint32_t a = 100, b = 200;
    asm volatile(
        "ld.w %%r0, %0\n"
        "ld.w %%r1, %1\n"
        "mlt.w %%r0, %%r1\n"
        : : "r"(a), "r"(b) : "r0", "r1");
    CHECK(read_alr() == 20000u, 11);
    CHECK(read_ahr() == 0u, 12);
    return 0;
}

static int test_mlt_w_negative(void) {
    // -1 * 1 = -1: ALR=0xFFFFFFFF, AHR=0xFFFFFFFF
    uint32_t a = 0xFFFFFFFFu, b = 1;
    asm volatile(
        "ld.w %%r0, %0\n"
        "ld.w %%r1, %1\n"
        "mlt.w %%r0, %%r1\n"
        : : "r"(a), "r"(b) : "r0", "r1");
    CHECK(read_alr() == 0xFFFFFFFFu, 13);
    CHECK(read_ahr() == 0xFFFFFFFFu, 14);
    return 0;
}

static int test_mlt_w_large(void) {
    // 0x00010000 * 0x00010000 = 0x100000000
    uint32_t a = 0x00010000u;
    asm volatile(
        "ld.w %%r0, %0\n"
        "ld.w %%r1, %%r0\n"
        "mlt.w %%r0, %%r1\n"
        : : "r"(a) : "r0", "r1");
    CHECK(read_alr() == 0u, 15);    // 0x100000000 low32 = 0
    CHECK(read_ahr() == 1u, 16);    // 0x100000000 high32 = 1
    return 0;
}

// ============================================================================
// mltu.w: unsigned 32×32 multiply
// ============================================================================
static int test_mltu_w(void) {
    // 0xFFFFFFFF * 2 unsigned = 0x1FFFFFFFE
    uint32_t a = 0xFFFFFFFFu, b = 2;
    asm volatile(
        "ld.w %%r0, %0\n"
        "ld.w %%r1, %1\n"
        "mltu.w %%r0, %%r1\n"
        : : "r"(a), "r"(b) : "r0", "r1");
    CHECK(read_alr() == 0xFFFFFFFEu, 17);
    CHECK(read_ahr() == 1u, 18);

    // mlt.w (signed) with same operands: -1 * 2 = -2
    asm volatile(
        "ld.w %%r0, %0\n"
        "ld.w %%r1, %1\n"
        "mlt.w %%r0, %%r1\n"
        : : "r"(a), "r"(b) : "r0", "r1");
    CHECK(read_alr() == 0xFFFFFFFEu, 19);
    CHECK(read_ahr() == 0xFFFFFFFFu, 20); // signed: -2 high32 = -1
    return 0;
}

// ============================================================================
// mac: multiply-accumulate
// mac %rs: {ahr,alr} += H[<rs+1>]+ × H[<rs+2>]+, repeated <rs> times
// rs  = count register (bits[7:4])
// rs+1 = pointer to first signed halfword array (post-incremented by 2)
// rs+2 = pointer to second signed halfword array (post-incremented by 2)
// rs is decremented each iteration; no-op when rs=0
// ============================================================================
static int16_t mac_arr1[3];
static int16_t mac_arr2[3];

static int test_mac_basic(void) {
    // 1 iteration: AHR:ALR = 10; += 3 * 4 = 12 → 22
    // mac %r1: r1=count, r2=ptr to arr1, r3=ptr to arr2
    mac_arr1[0] = 3;
    mac_arr2[0] = 4;
    uint32_t count = 1, ten = 10, zero = 0;
    asm volatile(
        "ld.w %%r4, %3\n"
        "ld.w %%alr, %%r4\n"     // ALR = 10
        "ld.w %%r4, %4\n"
        "ld.w %%ahr, %%r4\n"     // AHR = 0
        "ld.w %%r1, %0\n"        // r1 = 1 (count)
        "ld.w %%r2, %1\n"        // r2 = &mac_arr1[0]
        "ld.w %%r3, %2\n"        // r3 = &mac_arr2[0]
        "mac %%r1\n"
        : : "r"(count), "r"(mac_arr1), "r"(mac_arr2), "r"(ten), "r"(zero)
        : "r1", "r2", "r3", "r4");
    CHECK(read_alr() == 22u, 21);
    CHECK(read_ahr() == 0u, 22);
    return 0;
}

static int test_mac_accumulate(void) {
    // 3 iterations: [1,2,3] · [1,2,3] = 1 + 4 + 9 = 14
    mac_arr1[0] = 1; mac_arr1[1] = 2; mac_arr1[2] = 3;
    mac_arr2[0] = 1; mac_arr2[1] = 2; mac_arr2[2] = 3;
    uint32_t count = 3, zero = 0;
    asm volatile(
        "ld.w %%r4, %1\n"
        "ld.w %%alr, %%r4\n"     // ALR = 0
        "ld.w %%ahr, %%r4\n"     // AHR = 0
        "ld.w %%r1, %0\n"        // r1 = 3 (count)
        "ld.w %%r2, %2\n"        // r2 = &mac_arr1[0]
        "ld.w %%r3, %3\n"        // r3 = &mac_arr2[0]
        "mac %%r1\n"
        : : "r"(count), "r"(zero), "r"(mac_arr1), "r"(mac_arr2)
        : "r1", "r2", "r3", "r4");
    CHECK(read_alr() == 14u, 23);
    CHECK(read_ahr() == 0u, 24);
    return 0;
}

static int test_mac_negative(void) {
    // 1 iteration: AHR:ALR = 10; += (-3) * 4 = -12 → -2
    mac_arr1[0] = -3;
    mac_arr2[0] = 4;
    uint32_t count = 1, ten = 10, zero = 0;
    asm volatile(
        "ld.w %%r4, %3\n"
        "ld.w %%alr, %%r4\n"     // ALR = 10
        "ld.w %%r4, %4\n"
        "ld.w %%ahr, %%r4\n"     // AHR = 0
        "ld.w %%r1, %0\n"        // r1 = 1 (count)
        "ld.w %%r2, %1\n"        // r2 = &mac_arr1[0] (contains -3)
        "ld.w %%r3, %2\n"        // r3 = &mac_arr2[0] (contains 4)
        "mac %%r1\n"
        : : "r"(count), "r"(mac_arr1), "r"(mac_arr2), "r"(ten), "r"(zero)
        : "r1", "r2", "r3", "r4");
    CHECK(read_alr() == 0xFFFFFFFEu, 25);
    CHECK(read_ahr() == 0xFFFFFFFFu, 26);
    return 0;
}

int main(void) {
    int e;
    if ((e = test_mlt_h_positive()))  return e;
    if ((e = test_mlt_h_negative()))  return e;
    if ((e = test_mlt_h_max()))       return e;
    if ((e = test_mltu_h()))          return e;
    if ((e = test_mlt_w_small()))     return e;
    if ((e = test_mlt_w_negative()))  return e;
    if ((e = test_mlt_w_large()))     return e;
    if ((e = test_mltu_w()))          return e;
    if ((e = test_mac_basic()))       return e;
    if ((e = test_mac_accumulate()))  return e;
    if ((e = test_mac_negative()))    return e;
    return 0;
}

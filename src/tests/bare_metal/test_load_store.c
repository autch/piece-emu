// test_load_store.c — memory load/store instruction tests
//
// Tests: all data widths, sign extension, zero extension,
// register-indirect, post-increment, SP-relative.

#include <stdint.h>

#define CHECK(cond, code) do { if (!(cond)) return (code); } while (0)

// Static storage for indirect addressing tests (placed in .data/.bss)
static uint32_t mem_word;
static uint16_t mem_half;
static uint8_t  mem_byte;
static uint8_t  mem_buf[16];

// ============================================================================
// Sign extension: ld.b (signed byte → 32-bit)
// ============================================================================
static int test_ldb_sign_ext(void) {
    uint32_t r;

    mem_byte = 0x80; // -128 as signed byte
    asm volatile("ld.b %0, [%1]\n" : "=r"(r) : "r"(&mem_byte));
    CHECK(r == 0xFFFFFF80u, 1);  // -128 sign-extended

    mem_byte = 0x7F; // +127
    asm volatile("ld.b %0, [%1]\n" : "=r"(r) : "r"(&mem_byte));
    CHECK(r == 0x7Fu, 2);

    return 0;
}

// ============================================================================
// Zero extension: ld.ub (unsigned byte → 32-bit)
// ============================================================================
static int test_ldub_zero_ext(void) {
    uint32_t r;

    mem_byte = 0x80;
    asm volatile("ld.ub %0, [%1]\n" : "=r"(r) : "r"(&mem_byte));
    CHECK(r == 0x80u, 3);         // 128 (positive, no sign extension)

    mem_byte = 0xFF;
    asm volatile("ld.ub %0, [%1]\n" : "=r"(r) : "r"(&mem_byte));
    CHECK(r == 0xFFu, 4);         // 255

    return 0;
}

// ============================================================================
// ld.h / ld.uh: signed/unsigned halfword
// ============================================================================
static int test_ldh_sign_ext(void) {
    uint32_t r;

    mem_half = 0x8000;
    asm volatile("ld.h %0, [%1]\n" : "=r"(r) : "r"(&mem_half));
    CHECK(r == 0xFFFF8000u, 5);

    mem_half = 0x8000;
    asm volatile("ld.uh %0, [%1]\n" : "=r"(r) : "r"(&mem_half));
    CHECK(r == 0x8000u, 6);

    return 0;
}

// ============================================================================
// ld.w / st.w roundtrip
// ============================================================================
static int test_ldw_stw(void) {
    uint32_t stored = 0xDEADBEEFu, loaded;
    uint32_t *ptr = &mem_word;
    asm volatile(
        "ld.w [%2], %1\n"   // store
        "ld.w %0, [%2]\n"   // load back
        : "=r"(loaded)
        : "r"(stored), "r"(ptr));
    CHECK(loaded == 0xDEADBEEFu, 7);
    return 0;
}

// ============================================================================
// st.b: only low byte written, other bytes preserved
// ============================================================================
static int test_stb_partial(void) {
    // Direct C test: write via pointer, read as uint8_t and uint32_t
    volatile uint8_t *p8 = (volatile uint8_t *)mem_buf;
    volatile uint32_t *p32 = (volatile uint32_t *)mem_buf;
    *p32 = 0x11223344u;
    *p8 = 0xAAu;                 // overwrite lowest byte (little-endian: byte 0)
    uint32_t result = *p32;
    CHECK(result == 0x112233AAu, 8);
    return 0;
}

// ============================================================================
// st.h: only low halfword written
// ============================================================================
static int test_sth_partial(void) {
    volatile uint16_t *p16 = (volatile uint16_t *)mem_buf;
    volatile uint32_t *p32 = (volatile uint32_t *)mem_buf;
    *p32 = 0x11223344u;
    *p16 = 0xBEEFu;              // overwrite low halfword
    CHECK(*p32 == 0x1122BEEFu, 9);
    return 0;
}

// ============================================================================
// Post-increment addressing: ld.w %rd, [%rb]+
// rb is incremented by element size (4 for word) after load
// ============================================================================
static int test_postinc_ldw(void) {
    static uint32_t arr[4] = {10, 20, 30, 40};
    uint32_t v0, v1, ptr_after;
    uint32_t *ptr = arr;
    asm volatile(
        "ld.w %0, [%3]+\n"   // v0 = arr[0] = 10, ptr += 4
        "ld.w %1, [%3]+\n"   // v1 = arr[1] = 20, ptr += 4
        "ld.w %2, %3\n"      // ptr_after = ptr (reg-to-reg copy)
        : "=r"(v0), "=r"(v1), "=r"(ptr_after), "+r"(ptr));
    CHECK(v0 == 10u, 10);
    CHECK(v1 == 20u, 11);
    CHECK(ptr_after == (uint32_t)(&arr[2]), 12);
    return 0;
}

// ============================================================================
// Post-increment store: ld.w [%rb]+, %rs
// ============================================================================
static int test_postinc_stw(void) {
    static uint32_t dst[3] = {0, 0, 0};
    uint32_t *ptr = dst;
    uint32_t v1 = 111, v2 = 222, v3 = 333;
    asm volatile(
        "ld.w [%0]+, %1\n"   // dst[0] = 111, ptr += 4
        "ld.w [%0]+, %2\n"   // dst[1] = 222, ptr += 4
        "ld.w [%0]+, %3\n"   // dst[2] = 333, ptr += 4
        : "+r"(ptr)
        : "r"(v1), "r"(v2), "r"(v3)
        : "memory");
    CHECK(dst[0] == 111u, 13);
    CHECK(dst[1] == 222u, 14);
    CHECK(dst[2] == 333u, 15);
    return 0;
}

// ============================================================================
// SP-relative: verified via C local variables (compiler uses SP-relative loads)
// ============================================================================
static int test_sp_relative(void) {
    volatile uint32_t local_a = 0xAAAAAAAAu;
    volatile uint32_t local_b = 0xBBBBBBBBu;
    uint32_t ra = local_a;
    uint32_t rb = local_b;
    CHECK(ra == 0xAAAAAAAAu, 16);
    CHECK(rb == 0xBBBBBBBBu, 17);
    return 0;
}

// ============================================================================
// Register-register byte/halfword extends (Class 5B):
// ld.b/ld.ub/ld.h/ld.uh %rd, %rs — sign/zero-extend low byte or halfword of rs
// ============================================================================
static int test_reg_reg_extends(void) {
    uint32_t r;

    // ld.b %rd, %rs: sign-extend low byte of rs (0xFF = -1 as int8)
    uint32_t v1 = 0xFF;
    asm volatile("ld.b %0, %1\n" : "=r"(r) : "r"(v1));
    CHECK(r == 0xFFFFFFFFu, 18);

    // ld.ub %rd, %rs: zero-extend low byte
    uint32_t v2 = 0xFF;
    asm volatile("ld.ub %0, %1\n" : "=r"(r) : "r"(v2));
    CHECK(r == 0xFFu, 19);

    // ld.h %rd, %rs: sign-extend low halfword (0x8000 = -32768)
    uint32_t v3 = 0x8000;
    asm volatile("ld.h %0, %1\n" : "=r"(r) : "r"(v3));
    CHECK(r == 0xFFFF8000u, 20);

    // ld.uh %rd, %rs: zero-extend low halfword
    uint32_t v4 = 0x8000;
    asm volatile("ld.uh %0, %1\n" : "=r"(r) : "r"(v4));
    CHECK(r == 0x8000u, 21);

    return 0;
}

int main(void) {
    int e;
    if ((e = test_ldb_sign_ext()))    return e;
    if ((e = test_ldub_zero_ext()))   return e;
    if ((e = test_ldh_sign_ext()))    return e;
    if ((e = test_ldw_stw()))         return e;
    if ((e = test_stb_partial()))     return e;
    if ((e = test_sth_partial()))     return e;
    if ((e = test_postinc_ldw()))     return e;
    if ((e = test_postinc_stw()))     return e;
    if ((e = test_sp_relative()))     return e;
    if ((e = test_reg_reg_extends())) return e;
    return 0;
}

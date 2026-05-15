// test_softfloat_add.c — verify __addsf3 (soft-float add) on emulator
//
// luaspd_ns hangs at the 2nd is_prime2 call: c_prog2's i++ via __addsf3
// returns -3.0 from __addsf3(2.0, 1.0).  Pattern: when |a| > |b|, the
// result is -(|a|+|b|) instead of a+b.  This test uses the LLVM
// toolchain's compiler-rt __addsf3 (a different implementation than the
// gcc33 IRAM-resident one luaspd uses) to isolate the bug:
//
//   PASS = compiler-rt's __addsf3 works → bug is gcc33-specific
//          (either CPU emulation bug exposed by gcc33 code, or fp.lib
//          bug that real hardware tolerates somehow)
//   FAIL = compiler-rt also fails → CPU emulation bug
//
// Volatile + #pragma optimize hints prevent the compiler from constant-
// folding the additions away.
#include <stdint.h>

static volatile float a, b;

static inline uint32_t bits(float f) {
    union { float f; uint32_t u; } u = { f };
    return u.u;
}

static float test_add(float x, float y) {
    a = x; b = y;
    return a + b;
}

int main(void) {
    // a + b correct sign verification.  Each return code identifies the
    // first failing case so the runner pinpoints the bug.
    if (bits(test_add(0.0f, 1.0f)) != 0x3F800000u) return 1;  // 0+1=1.0
    if (bits(test_add(1.0f, 1.0f)) != 0x40000000u) return 2;  // 1+1=2.0
    if (bits(test_add(2.0f, 1.0f)) != 0x40400000u) return 3;  // 2+1=3.0  ← luaspd bug
    if (bits(test_add(3.0f, 1.0f)) != 0x40800000u) return 4;  // 3+1=4.0
    if (bits(test_add(2.0f, 2.0f)) != 0x40800000u) return 5;  // 2+2=4.0
    if (bits(test_add(-3.0f, 1.0f)) != 0xC0000000u) return 6; // -3+1=-2.0
    if (bits(test_add(-2.0f, 1.0f)) != 0xBF800000u) return 7; // -2+1=-1.0
    if (bits(test_add(100.0f, 1.0f)) != 0x42CA0000u) return 8; // 100+1=101
    if (bits(test_add(2.0f, -1.0f)) != 0x3F800000u) return 9; // 2-1=1.0

    return 0; // PASS
}

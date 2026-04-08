// crt_init.c — C-level startup: clears BSS, calls main(), exits via semihosting.
//
// The semihosting TEST_RESULT port is at 0x060008.
// Writing 0 → PASS, non-zero → FAIL.  The emulator halts the simulation.

#include <stdint.h>

extern uint32_t __bss_start;
extern uint32_t __bss_end;

extern int main(void);

// Semihosting: write to IO address 0x060008 to signal test result.
// 0x060008 requires 2 EXT instructions because bit 18 is set in 19-bit
// sign-extended form.  The compiler generates the correct ext+ext+ld.w
// sequence for any global variable access; we use a volatile pointer.
static void semihosting_exit(int result) {
    volatile uint32_t* const test_result = (volatile uint32_t*)0x060008u;
    *test_result = (uint32_t)result;
    // The emulator halts on TEST_RESULT write; add an infinite loop for safety.
    for (;;) {}
}

// Called from crt0.s after SP is set up.
void _start_c(void) {
    // Clear BSS (emulator already zeroes IRAM on init, but be correct).
    uint32_t* p = &__bss_start;
    uint32_t* e = &__bss_end;
    while (p < e) *p++ = 0;

    int result = main();
    semihosting_exit(result);
}

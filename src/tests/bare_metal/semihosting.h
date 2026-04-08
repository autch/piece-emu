#pragma once
// semihosting.h — emulator semihosting port helpers for test programs.
//
// Addresses (S1C33209 as emulated by piece_emu):
//   0x060000  CONSOLE_CHAR  write a single character (low byte)
//   0x060002  CONSOLE_STR   write pointer to null-terminated string
//   0x060008  TEST_RESULT   0=PASS, non-zero=FAIL; emulator halts on write

#include <stdint.h>

static inline void semi_putchar(char c) {
    *(volatile uint16_t*)0x060000u = (uint16_t)(unsigned char)c;
}

static inline void semi_puts(const char* s) {
    *(volatile uint32_t*)0x060002u = (uint32_t)(uintptr_t)s;
}

// Signal test result and halt.  Does not return.
__attribute__((noreturn))
static inline void semi_exit(int result) {
    *(volatile uint32_t*)0x060008u = (uint32_t)result;
    for (;;) {}
}

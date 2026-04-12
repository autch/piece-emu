#pragma once
// semihosting.h — emulator semihosting/debug port helpers for bare-metal test programs.
//
// All ports are at offsets from 0x060000 (SEMI_BASE).
//
// Port map:
//   +0x00  CONSOLE_CHAR   (W16) emit low byte as a character to host stdout
//   +0x02  CONSOLE_STR lo (W16) latch low 16 bits of string address
//   +0x04  CONSOLE_STR hi (W16) high 16 bits; combine with lo and print string
//   +0x08  TEST_RESULT    (W32) 0=PASS (halt), non-zero=FAIL (halt)
//   +0x0C  CYCLE_COUNT_LO (R32) lower 32 bits of emulated CPU cycle count
//   +0x10  CYCLE_COUNT_HI (R32) upper 32 bits of emulated CPU cycle count
//   +0x14  REG_SNAPSHOT   (W32) write any value to dump all CPU registers to stderr
//   +0x18  TRACE_CTL      (W32) write 1 to enable instruction trace, 0 to disable
//   +0x1C  BKPT_SET       (W32) set software breakpoint at the given address
//   +0x20  BKPT_CLR       (W32) clear software breakpoint at the given address
//   +0x24  HOST_TIME_MS   (R32) host wall-clock time in milliseconds
//
// 32-bit writes (W32) are issued as two 16-bit bus writes by the CPU:
//   lo-word to addr+0, hi-word to addr+2.
// 32-bit reads (R32) are two 16-bit bus reads:
//   lo-word from addr+0, hi-word from addr+2.

#include <stdint.h>

#define SEMI_BASE 0x060000u

// ---------------------------------------------------------------------------
// Raw 32-bit and 16-bit port accessors
// ---------------------------------------------------------------------------

#define SEMI_CONSOLE_CHAR   (*(volatile uint16_t *)(SEMI_BASE + 0x00))
// CONSOLE_STR: write 32-bit pointer; lo-word latched at +0x02, hi triggers at +0x04.
#define SEMI_CONSOLE_STR    (*(volatile const char **)(SEMI_BASE + 0x02))
#define SEMI_TEST_RESULT    (*(volatile uint32_t *)(SEMI_BASE + 0x08))
#define SEMI_CYCLE_LO       (*(volatile uint32_t *)(SEMI_BASE + 0x0C))
#define SEMI_CYCLE_HI       (*(volatile uint32_t *)(SEMI_BASE + 0x10))
#define SEMI_REG_SNAPSHOT   (*(volatile uint32_t *)(SEMI_BASE + 0x14))
#define SEMI_TRACE_CTL      (*(volatile uint32_t *)(SEMI_BASE + 0x18))
#define SEMI_BKPT_SET       (*(volatile uint32_t *)(SEMI_BASE + 0x1C))
#define SEMI_BKPT_CLR       (*(volatile uint32_t *)(SEMI_BASE + 0x20))
#define SEMI_HOST_TIME_MS   (*(volatile uint32_t *)(SEMI_BASE + 0x24))

// ---------------------------------------------------------------------------
// Inline helpers
// ---------------------------------------------------------------------------

static inline void semi_putchar(char c) {
    SEMI_CONSOLE_CHAR = (uint16_t)(unsigned char)c;
}

static inline void semi_puts(const char *s) {
    SEMI_CONSOLE_STR = s;
}

// Signal test result and halt.  Does not return.
__attribute__((noreturn))
static inline void semi_exit(int result) {
    SEMI_TEST_RESULT = (uint32_t)result;
    for (;;) {}
}

// Shorthand pass/fail wrappers.
__attribute__((noreturn)) static inline void semi_pass(void) { semi_exit(0); }
__attribute__((noreturn)) static inline void semi_fail(int code) { semi_exit(code); }

// Dump all CPU registers (R0–R15, SP, PSR, ALR, AHR, PC) to host stderr.
static inline void semi_snapshot(void) { SEMI_REG_SNAPSHOT = 1; }

// Instruction trace control.
static inline void semi_trace_on(void)  { SEMI_TRACE_CTL = 1; }
static inline void semi_trace_off(void) { SEMI_TRACE_CTL = 0; }

// Software breakpoints: halt (with register dump) when CPU reaches addr.
static inline void semi_bkpt_set(void *addr) {
    SEMI_BKPT_SET = (uint32_t)(uintptr_t)addr;
}
static inline void semi_bkpt_clr(void *addr) {
    SEMI_BKPT_CLR = (uint32_t)(uintptr_t)addr;
}

// Total emulated CPU cycles since start.
static inline uint64_t semi_cycles(void) {
    uint32_t lo = SEMI_CYCLE_LO;
    uint32_t hi = SEMI_CYCLE_HI;
    return ((uint64_t)hi << 32) | lo;
}

// Host wall-clock time in milliseconds.
static inline uint32_t semi_host_time_ms(void) {
    return SEMI_HOST_TIME_MS;
}

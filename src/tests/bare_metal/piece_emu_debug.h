#pragma once
// piece_emu_debug.h — emulator debug/semihosting port helpers for test programs.
//
// All ports are at offsets from EMU_DEBUG_BASE (0x00060000).
// 32-bit read/write macros generate two 16-bit bus transactions on S1C33.

#include <stdint.h>

#define EMU_DEBUG_BASE 0x00060000u

// CONSOLE_CHAR (W): write low byte of value as a character to host stdout.
// Usage: EMU_CONSOLE_CHAR = 'A';
#define EMU_CONSOLE_CHAR  (*(volatile uint16_t *)(EMU_DEBUG_BASE + 0x00))

// CONSOLE_STR (W): write a 32-bit pointer; emulator reads and prints the
// NUL-terminated string at that address.
// Implementation: lo-word to +0x02, hi-word to +0x04 (triggers print).
// Usage: EMU_CONSOLE_STR = "hello";
#define EMU_CONSOLE_STR   (*(volatile const char **)(EMU_DEBUG_BASE + 0x02))

// TEST_RESULT (W): write 0 for PASS, non-zero for FAIL; emulator halts.
// Usage: EMU_TEST_RESULT = 0;  // PASS
#define EMU_TEST_RESULT   (*(volatile uint32_t *)(EMU_DEBUG_BASE + 0x08))

// CYCLE_COUNT_LO/HI (R): lower/upper 32 bits of total emulated CPU cycles.
// Each is a 32-bit read (two 16-bit bus reads).
// Usage: uint64_t c = ((uint64_t)EMU_CYCLE_HI << 32) | EMU_CYCLE_LO;
#define EMU_CYCLE_LO      (*(volatile uint32_t *)(EMU_DEBUG_BASE + 0x0C))
#define EMU_CYCLE_HI      (*(volatile uint32_t *)(EMU_DEBUG_BASE + 0x10))

// REG_SNAPSHOT (W): write any value to dump all CPU registers to host stderr.
// Usage: EMU_REG_SNAPSHOT = 1;
#define EMU_REG_SNAPSHOT  (*(volatile uint32_t *)(EMU_DEBUG_BASE + 0x14))

// TRACE_CTL (W): write 1 to enable instruction disassembly trace, 0 to disable.
// Usage: EMU_TRACE_CTL = 1;
#define EMU_TRACE_CTL     (*(volatile uint32_t *)(EMU_DEBUG_BASE + 0x18))

// BKPT_SET (W): write a 32-bit code address to install a software breakpoint.
// When the CPU reaches that address, it prints a register dump and halts.
// Usage: EMU_BKPT_SET = (uint32_t)some_function;
#define EMU_BKPT_SET      (*(volatile uint32_t *)(EMU_DEBUG_BASE + 0x1C))

// BKPT_CLR (W): write a 32-bit address to remove a previously set breakpoint.
// Usage: EMU_BKPT_CLR = (uint32_t)some_function;
#define EMU_BKPT_CLR      (*(volatile uint32_t *)(EMU_DEBUG_BASE + 0x20))

// HOST_TIME_MS (R): host wall-clock time in milliseconds (std::chrono::steady_clock).
// Useful for measuring elapsed wall time in benchmarks.
// Usage: uint32_t t = EMU_HOST_TIME_MS;
#define EMU_HOST_TIME_MS  (*(volatile uint32_t *)(EMU_DEBUG_BASE + 0x24))

// ---------------------------------------------------------------------------
// Convenience inline helpers
// ---------------------------------------------------------------------------

static inline void emu_putchar(char c) {
    EMU_CONSOLE_CHAR = (uint16_t)(unsigned char)c;
}

static inline void emu_puts(const char *s) {
    EMU_CONSOLE_STR = s;
}

__attribute__((noreturn))
static inline void emu_pass(void) {
    EMU_TEST_RESULT = 0;
    for (;;) {}
}

__attribute__((noreturn))
static inline void emu_fail(int code) {
    EMU_TEST_RESULT = (uint32_t)code;
    for (;;) {}
}

static inline void emu_snapshot(void) {
    EMU_REG_SNAPSHOT = 1;
}

static inline void emu_trace_on(void)  { EMU_TRACE_CTL = 1; }
static inline void emu_trace_off(void) { EMU_TRACE_CTL = 0; }

static inline void emu_breakpoint(void *addr) {
    EMU_BKPT_SET = (uint32_t)(uintptr_t)addr;
}

static inline uint64_t emu_cycles(void) {
    uint32_t lo = EMU_CYCLE_LO;
    uint32_t hi = EMU_CYCLE_HI;
    return ((uint64_t)hi << 32) | lo;
}

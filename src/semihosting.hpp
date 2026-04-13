#pragma once
#include <cstdint>
#include <functional>

class Bus;

// Optional callbacks provided by the main loop to extend semihosting.
// All fields may be null; unconfigured features are silently ignored.
struct SemiConfig {
    // Returns the current emulated CPU cycle count.
    std::function<uint64_t()>     get_cycles;
    // Called with true to enable instruction tracing, false to disable.
    std::function<void(bool)>     set_trace;
    // Halt the CPU (e.g. cpu.state.in_halt = true).
    std::function<void()>         halt;
    // Dump all CPU registers to stderr (called by REG_SNAPSHOT and BKPT hit).
    std::function<void()>         snapshot_regs;
    // Insert a software breakpoint at the given address.
    std::function<void(uint32_t)> set_breakpoint;
    // Remove a software breakpoint at the given address.
    std::function<void(uint32_t)> clear_breakpoint;
};

// Register semihosting I/O handlers on the bus at 0x060000.
//
// Port map (offsets from 0x060000):
//   +0x00  CONSOLE_CHAR   (W)  emit low byte as a character to stdout
//   +0x02  CONSOLE_STR lo (W)  latch low 16 bits of string address
//   +0x04  CONSOLE_STR hi (W)  combine with latched lo, print NUL-terminated string
//   +0x08  TEST_RESULT    (W)  0=PASS, non-zero=FAIL; halts the emulator
//   +0x0C  CYCLE_COUNT_LO (R)  lower 32 bits of total emulated cycles (read as two 16-bit reads)
//   +0x10  CYCLE_COUNT_HI (R)  upper 32 bits of total emulated cycles (read as two 16-bit reads)
//   +0x14  REG_SNAPSHOT   (W)  write any value to dump all CPU registers to stderr
//   +0x18  TRACE_CTL      (W)  write 1 to enable instruction trace, 0 to disable
//   +0x1C  BKPT_SET       (W)  write 32-bit address to set a software breakpoint
//   +0x20  BKPT_CLR       (W)  write 32-bit address to clear a software breakpoint
//   +0x24  HOST_TIME_MS   (R)  host wall-clock time in milliseconds (read as two 16-bit reads)
//
// All other reads from the semihosting range return 0.
void semihosting_init(Bus& bus, SemiConfig cfg = {});

// Returns -1 if TEST_RESULT not yet written, 0 for PASS, non-zero for FAIL code.
int semihosting_test_result();

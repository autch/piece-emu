#pragma once
#include <cstdint>

class Bus;
class Cpu;

// Register semihosting I/O handlers on the bus at 0x060000.
//
// Implemented ports (offsets from 0x060000):
//   +0x00  CONSOLE_CHAR  write8/16: emit low byte as character to stdout
//   +0x02  CONSOLE_STR   write16: emit NUL-terminated string from memory address
//   +0x08  TEST_RESULT   write8/16: 0=PASS, non-zero=FAIL; halts emulator
//
// All reads from the semihosting range return 0.
void semihosting_init(Bus& bus, Cpu& cpu);

// Returns -1 if TEST_RESULT not yet written, 0 for PASS, non-zero for FAIL.
int semihosting_test_result();

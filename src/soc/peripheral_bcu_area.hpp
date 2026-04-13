#pragma once
#include <cstdint>

class Bus;
class Cpu;

// ============================================================================
// BcuAreaCtrl — S1C33209 BCU area setup registers
//
// Base: 0x048120 (c_BCUAREAtag layout, all 16-bit registers except rTTBR)
//
//   0x048120  rA18_15  Areas 18-17 / 16-15 wait + size + output-disable delay
//   0x048122  rA14_13
//   0x048124  rA12_11
//   0x048126  rA10_9   Area 10 = Flash (A10WT[2:0] at bits [2:0])
//   0x048128  rA8_7
//   0x04812A  rA6_4    Area 5/4 = SRAM  (A5WT[2:0]  at bits [2:0])
//   0x04812C  Dummy (lo) + rTBRP (hi)  — TTBR write protect
//   0x04812E  rBUS     bus control
//   0x048130  rDRAM    DRAM timing
//   0x048132  rACCESS  endian / internal access control
//   0x048134  rTTBR    trap table base address register (32-bit, two halfwords)
//   0x048138  rGA      read signal control
//   0x04813A  rBCLKSEL (lo) + Dummy (hi)
//
// On write to rA6_4:  bus.sram_wait  is updated from A5WT[2:0]
// On write to rA10_9: bus.flash_wait is updated from A10WT[2:0]
// On write to rTTBR:  cpu.state.ttbr is updated
//
// Cold-start default: all AxWT = 7 (maximum wait, hardware default).
// ============================================================================
class BcuAreaCtrl {
public:
    void attach(Bus& bus, Cpu& cpu);

    // Direct register access (for unit tests)
    uint16_t a10_9()   const { return a10_9_;   }
    uint16_t a6_4()    const { return a6_4_;    }
    uint32_t ttbr()    const { return ttbr_;    }

private:
    // Reset values: AxWT = 7 (maximum wait)
    uint16_t a18_15_ = 0x7777;
    uint16_t a14_13_ = 0x7777;
    uint16_t a12_11_ = 0x7777;
    uint16_t a10_9_  = 0x7777; // A10WT[2:0] at [2:0]
    uint16_t a8_7_   = 0x7777;
    uint16_t a6_4_   = 0x7777; // A5WT[2:0]  at [2:0]
    uint16_t bus_ctl_ = 0;
    uint16_t dram_    = 0;
    uint16_t access_  = 0;
    uint32_t ttbr_    = 0x400; // P/ECE kernel sets this to 0x400

    Bus* bus_ = nullptr;
    Cpu* cpu_ = nullptr;

    void on_a6_4_write(uint16_t val);
    void on_a10_9_write(uint16_t val);
    void on_ttbr_write(uint32_t val);
};

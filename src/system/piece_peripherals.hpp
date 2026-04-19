#pragma once
#include "peripheral_intc.hpp"
#include "peripheral_clkctl.hpp"
#include "peripheral_t8.hpp"
#include "peripheral_t16.hpp"
#include "peripheral_portctrl.hpp"
#include "peripheral_bcu_area.hpp"
#include "peripheral_wdt.hpp"
#include "peripheral_rtc.hpp"
#include "peripheral_hsdma.hpp"
#include "peripheral_sif3.hpp"
#include "peripheral_sound.hpp"
#include "s6b0741.hpp"

#include <cstdint>

class Bus;
class Cpu;

// ---------------------------------------------------------------------------
// PiecePeripherals — S1C33209 on-chip peripherals + P/ECE board devices.
//
// `attach(bus, cpu)` wires every peripheral to the bus and connects the
// interrupt / DMA / LCD serial-path callbacks between them.
//
// `tick(cycles)` advances every cycle-driven peripheral (timers, WDT, RTC,
// sound).  `next_wake_cycle()` returns the earliest cycle at which any
// peripheral needs tick() called; `sleep_wake_cycle()` is the subset of that
// which is OSC1-driven (valid in SLEEP mode, where OSC3 is stopped).
// ---------------------------------------------------------------------------
struct PiecePeripherals {
    InterruptController intc;
    ClockControl        clk;
    Timer8bit           t8_ch[4]  = {Timer8bit(0),  Timer8bit(1),
                                     Timer8bit(2),  Timer8bit(3)};
    Timer16bit          t16_ch[6] = {Timer16bit(0), Timer16bit(1), Timer16bit(2),
                                     Timer16bit(3), Timer16bit(4), Timer16bit(5)};
    PortCtrl            portctrl;
    BcuAreaCtrl         bcu_area;
    WatchdogTimer       wdt;
    ClockTimer          rtc;
    Hsdma               hsdma;
    Sif3                sif3;
    S6b0741             lcd;
    Sound               sound;

    uint64_t            nmi_count = 0;

    void     attach(Bus& bus, Cpu& cpu);
    void     tick(uint64_t cycles);
    uint64_t next_wake_cycle();

    // Reset all on-chip peripherals and the board-level LCD.
    //   cold=false → hot start: 0x48120..0x4813F (BCU area) and
    //                0x402C0..0x402DF (I/O port ctrl/data) preserved,
    //                LCD VRAM preserved (external device).
    //   cold=true  → cold start: also resets BCU area, PortCtrl, S6b0741.
    // The CPU runner must call clk.on_clock_change after reset so it
    // re-seeds timer wake / pacing anchors.
    void     reset(bool cold);

    // SLEEP mode: OSC3 is stopped on real hardware, so only OSC1-driven wake
    // sources are valid.  In P/ECE this is RTC (1 Hz) and external input
    // (KEY0 via button press) — the latter is polled in the CPU loop, so
    // here we only report the RTC wake time.
    uint64_t sleep_wake_cycle();
};

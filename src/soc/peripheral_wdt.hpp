#pragma once
#include "tick.hpp"
#include <cstdint>
#include <functional>

class Bus;
class ClockControl;

// ============================================================================
// WatchdogTimer — S1C33209 watchdog timer (NMI generator)
//
// Registers at 0x040170..0x040171 (c_WDTtag):
//   0x040170  rWRWD  write protect (WRWD bit 7)
//   0x040171  rEWD   enable (EWD bit 1)
//
// Hardware behaviour:
//   When EWD=1 the watchdog counter runs at cpu_clock/16384.  When it
//   overflows (every 2^15 WDT clocks = cpu_clock/2^29 ≈ 0.68 ms at 24 MHz)
//   it fires NMI (trap 7) instead of reset.
//
//   The P/ECE kernel uses this NMI to increment ClockTicks (precision timer
//   high word).  pceTimerGetPrecisionCount() combines ClockTicks<<16 with
//   T16 Ch.0 TC to give ~1 μs resolution.
//
// Emulation:
//   We fire NMI every cpu_clock/1000 cycles (~1 ms) when EWD=1.
//   This matches piemu's behaviour (NMI once per main loop iteration ≈ 1 ms).
//   The emulator never performs an actual reset.
// ============================================================================
class WatchdogTimer : public ITickable {
public:
    // assert_nmi: called with (trap_no=7, level=0) when NMI fires
    void attach(Bus& bus, const ClockControl& clk,
                std::function<void(int, int)> assert_nmi);

    void tick(uint64_t cpu_cycles) override;
    uint64_t next_wake_cycle() const override {
        if (!(ewd_ & 0x02)) return UINT64_MAX;
        return next_nmi_cycle_;
    }

    // Reset register + scheduling state; preserves attach-bound callbacks.
    void reset();

private:
    const ClockControl* clk_ = nullptr;
    std::function<void(int, int)> assert_nmi_;

    uint8_t wrwd_ = 0;
    uint8_t ewd_  = 0;

    uint64_t next_nmi_cycle_ = 0;

    // NMI period in CPU cycles: cpu_clock / 1000  (~1 ms)
    uint64_t nmi_period() const;
};

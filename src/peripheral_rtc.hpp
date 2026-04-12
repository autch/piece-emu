#pragma once
#include "tick.hpp"
#include <cstdint>

class Bus;
class ClockControl;
class InterruptController;

// ============================================================================
// ClockTimer — S1C33209 clock timer (RTC) stub
//
// Register map (byte addresses 0x040151..0x04015B, 11 bytes):
//   0x040151  rRTCSTOP  stop control
//   0x040152  rRTCCR    clock correction
//   0x040153  rRTCSEL   status / clock select:
//               bit 5 = OSC3 running (always 1 in emulation)
//               bit 3 = 1 Hz clock edge flag (toggled every half-second)
//   0x040154  rRTCIEN   interrupt enable
//   0x040155  rRTCSTA   interrupt status
//   0x040156  rSEC      seconds  (BCD)
//   0x040157  rMIN      minutes  (BCD)
//   0x040158  rHOUR     hours    (BCD)
//   0x040159  rDAY      days     (BCD, lower byte)
//   0x04015A  rMON      months   (BCD)
//   0x04015B  rYEAR     years    (BCD, 2-digit)
//
// Emulation:
//   Registers are bus-mapped as halfwords starting at 0x040150 (dummy lo byte).
//   The 1 Hz clock flag (rRTCSEL bit 3) toggles every cpu_clock_hz()/2 cycles
//   so that the kernel's GetSysClock() busy-wait completes without hanging.
//   Alarm interrupt generation (CLK_TIMER, trap 65) is not implemented (P2).
// ============================================================================
class ClockTimer : public ITickable {
public:
    void attach(Bus& bus, InterruptController& intc, const ClockControl& clk);

    void tick(uint64_t cpu_cycles) override;

    // Returns the next cycle at which the 1 Hz flag will toggle.
    uint64_t next_wake_cycle() const override { return next_half_cycle_; }

    // Direct register access (for unit tests)
    uint8_t rtcsel() const { return rtcsel_; }
    uint8_t rtcsta() const { return rtcsta_; }

private:
    static constexpr uint32_t RTC_BASE = 0x040150; // halfword base (dummy lo)

    const ClockControl*  clk_  = nullptr;
    InterruptController* intc_ = nullptr;

    uint8_t rtcstop_ = 0;
    uint8_t rtccr_   = 0;
    // bit5=1 (OSC3 on), bit3=0 initially (1 Hz flag low)
    uint8_t rtcsel_  = 0x20;
    uint8_t rtcien_  = 0;
    uint8_t rtcsta_  = 0;
    uint8_t sec_  = 0;
    uint8_t min_  = 0;
    uint8_t hour_ = 0;
    uint8_t day_  = 1;
    uint8_t mon_  = 1;
    uint8_t year_ = 0;

    uint64_t next_half_cycle_ = 0; // CPU cycle of next 1 Hz flag toggle
    bool     clk_phase_       = false; // current half-cycle phase

    // Returns the CPU cycle count for one half-period of the 1 Hz clock.
    uint64_t half_period(uint32_t cpu_hz) const
    {
        if (cpu_hz == 0) return 24'000'000u;
        return static_cast<uint64_t>(cpu_hz) / 2;
    }
};

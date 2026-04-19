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
//   rTCD (0x040153) bit 3 toggles every cpu_clock_hz()/64 cycles, simulating
//   the 32.768 kHz crystal divided by 64 then by 8 (= 64 Hz toggle rate,
//   half-period = 375,000 cycles at 24 MHz).  This drives GetSysClock() to
//   measure ≈11720 T16 counts per half-period — matching real hardware at 24 MHz.
//   Alarm interrupt generation (CLK_TIMER, trap 65) is not implemented (P2).
// ============================================================================
class ClockTimer : public ITickable {
public:
    void attach(Bus& bus, InterruptController& intc, const ClockControl& clk);

    void tick(uint64_t cpu_cycles) override;

    // Returns the next cycle at which the 1 Hz flag will toggle.
    uint64_t next_wake_cycle() const override { return next_half_cycle_; }

    // Reset register + scheduling state; preserves attach-bound pointers.
    void reset();

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

    // Returns the CPU cycle count for one half-period of rTCD bit3.
    //
    // The 32.768 kHz crystal feeds the CT sub-second counter at /64 = 512 Hz.
    // rTCD bit3 toggles every 8 increments → toggle rate = 512/8 = 64 Hz,
    // so one half-period = cpu_hz / 64.  At 24 MHz this gives 375,000 cycles.
    // With T16 Ch0 at 750 kHz (/32 prescaler), GetSysClock measures
    // a = 750,000 × (375,000/24,000,000) ≈ 11,719 ≈ 11720, matching hardware.
    uint64_t half_period(uint32_t cpu_hz) const
    {
        if (cpu_hz == 0) return 24'000'000u / 64u;
        return static_cast<uint64_t>(cpu_hz) / 64u;
    }
};

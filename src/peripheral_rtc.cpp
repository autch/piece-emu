#include "peripheral_rtc.hpp"
#include "peripheral_clkctl.hpp"
#include "bus.hpp"

void ClockTimer::tick(uint64_t cpu_cycles)
{
    if (cpu_cycles < next_half_cycle_) return;

    uint32_t cpu_hz = clk_ ? clk_->cpu_clock_hz() : 48'000'000u;
    uint64_t half   = half_period(cpu_hz);

    while (cpu_cycles >= next_half_cycle_) {
        next_half_cycle_ += half;
        clk_phase_ = !clk_phase_;

        // Toggle the 1 Hz clock flag (rRTCSEL bit 3)
        if (clk_phase_)
            rtcsel_ |=  0x08;
        else
            rtcsel_ &= ~0x08;
    }
}

void ClockTimer::attach(Bus& bus, InterruptController& /*intc*/,
                         const ClockControl& clk)
{
    clk_ = &clk;

    // Initialise the first toggle time based on the current CPU clock.
    next_half_cycle_ = half_period(clk.cpu_clock_hz());

    // ---------------------------------------------------------------------------
    // Register map: 6 halfword handlers starting at 0x040150.
    //
    // Bus address  lo byte (even)     hi byte (odd)
    // 0x040150     dummy              rRTCSTOP (0x040151)
    // 0x040152     rRTCCR             rRTCSEL  (0x040153) ← 1 Hz flag here
    // 0x040154     rRTCIEN            rRTCSTA
    // 0x040156     rSEC               rMIN
    // 0x040158     rHOUR              rDAY
    // 0x04015A     rMON               rYEAR
    // ---------------------------------------------------------------------------

    // 0x040150: dummy (lo) + rRTCSTOP (hi)
    bus.register_io(RTC_BASE, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(rtcstop_) << 8;
        },
        [this](uint32_t, uint16_t v) {
            rtcstop_ = static_cast<uint8_t>(v >> 8);
        }
    });

    // 0x040152: rRTCCR (lo) + rRTCSEL (hi)
    // rRTCSEL writes: preserve HW-managed bits (bit3=1Hz flag, bit5=OSC3 on).
    bus.register_io(RTC_BASE + 2, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(rtccr_) |
                   (static_cast<uint16_t>(rtcsel_) << 8);
        },
        [this](uint32_t, uint16_t v) {
            rtccr_ = static_cast<uint8_t>(v);
            uint8_t written = static_cast<uint8_t>(v >> 8);
            // Keep bits 3 and 5 (hardware-managed: 1Hz flag and OSC3 status)
            rtcsel_ = (written & ~0x28u) | (rtcsel_ & 0x28u);
        }
    });

    // 0x040154: rRTCIEN (lo) + rRTCSTA (hi)
    // rRTCSTA is write-1-to-clear.
    bus.register_io(RTC_BASE + 4, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(rtcien_) |
                   (static_cast<uint16_t>(rtcsta_) << 8);
        },
        [this](uint32_t, uint16_t v) {
            rtcien_ = static_cast<uint8_t>(v);
            rtcsta_ &= ~static_cast<uint8_t>(v >> 8);
        }
    });

    // 0x040156: rSEC (lo) + rMIN (hi)
    bus.register_io(RTC_BASE + 6, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(sec_) |
                   (static_cast<uint16_t>(min_) << 8);
        },
        [this](uint32_t, uint16_t v) {
            sec_ = static_cast<uint8_t>(v);
            min_ = static_cast<uint8_t>(v >> 8);
        }
    });

    // 0x040158: rHOUR (lo) + rDAY (hi)
    bus.register_io(RTC_BASE + 8, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(hour_) |
                   (static_cast<uint16_t>(day_) << 8);
        },
        [this](uint32_t, uint16_t v) {
            hour_ = static_cast<uint8_t>(v);
            day_  = static_cast<uint8_t>(v >> 8);
        }
    });

    // 0x04015A: rMON (lo) + rYEAR (hi)
    bus.register_io(RTC_BASE + 10, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(mon_) |
                   (static_cast<uint16_t>(year_) << 8);
        },
        [this](uint32_t, uint16_t v) {
            mon_  = static_cast<uint8_t>(v);
            year_ = static_cast<uint8_t>(v >> 8);
        }
    });
}

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

void ClockTimer::reset()
{
    rtcstop_ = 0;
    rtccr_   = 0;
    rtcsel_  = 0x20;   // OSC3 running
    rtcien_  = 0;
    rtcsta_  = 0;
    sec_ = 0; min_ = 0; hour_ = 0;
    day_ = 1; mon_ = 1; year_ = 0;
    clk_phase_ = false;
    next_half_cycle_ = clk_ ? half_period(clk_->cpu_clock_hz())
                            : 24'000'000u / 64u;
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

    // 0x040150: dummy (lo) + rRTCSTOP (hi at 0x040151)
    bus.register_io(RTC_BASE, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(rtcstop_) << 8;
        },
        [this](uint32_t addr, uint16_t v) {
            if (addr & 1)
                rtcstop_ = static_cast<uint8_t>(v);       // odd: val in low bits
            else
                rtcstop_ = static_cast<uint8_t>(v >> 8);  // even: val in high bits
        }
    });

    // 0x040152: rRTCCR (lo) + rRTCSEL (hi at 0x040153)
    // rRTCSEL writes: preserve HW-managed bits (bit3=1Hz flag, bit5=OSC3 on).
    bus.register_io(RTC_BASE + 2, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(rtccr_) |
                   (static_cast<uint16_t>(rtcsel_) << 8);
        },
        [this](uint32_t addr, uint16_t v) {
            if (addr & 1) {
                // Byte write to odd address 0x040153 (rRTCSEL): val in low bits.
                uint8_t written = static_cast<uint8_t>(v);
                rtcsel_ = (written & ~0x28u) | (rtcsel_ & 0x28u);
            } else {
                // Halfword write or byte write to even address 0x040152 (rRTCCR).
                rtccr_ = static_cast<uint8_t>(v);
                uint8_t written = static_cast<uint8_t>(v >> 8);
                rtcsel_ = (written & ~0x28u) | (rtcsel_ & 0x28u);
            }
        }
    });

    // 0x040154: rRTCIEN (lo) + rRTCSTA (hi at 0x040155)
    // rRTCSTA is write-1-to-clear.
    bus.register_io(RTC_BASE + 4, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(rtcien_) |
                   (static_cast<uint16_t>(rtcsta_) << 8);
        },
        [this](uint32_t addr, uint16_t v) {
            if (addr & 1) {
                // Byte write to odd address 0x040155 (rRTCSTA): val in low bits.
                rtcsta_ &= ~static_cast<uint8_t>(v);
            } else {
                // Halfword write or byte write to even address 0x040154 (rRTCIEN).
                rtcien_ = static_cast<uint8_t>(v);
                rtcsta_ &= ~static_cast<uint8_t>(v >> 8);
            }
        }
    });

    // 0x040156: rSEC (lo) + rMIN (hi at 0x040157)
    bus.register_io(RTC_BASE + 6, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(sec_) |
                   (static_cast<uint16_t>(min_) << 8);
        },
        [this](uint32_t addr, uint16_t v) {
            if (addr & 1)
                min_ = static_cast<uint8_t>(v);
            else { sec_ = static_cast<uint8_t>(v); min_ = static_cast<uint8_t>(v >> 8); }
        }
    });

    // 0x040158: rHOUR (lo) + rDAY (hi at 0x040159)
    bus.register_io(RTC_BASE + 8, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(hour_) |
                   (static_cast<uint16_t>(day_) << 8);
        },
        [this](uint32_t addr, uint16_t v) {
            if (addr & 1)
                day_  = static_cast<uint8_t>(v);
            else { hour_ = static_cast<uint8_t>(v); day_  = static_cast<uint8_t>(v >> 8); }
        }
    });

    // 0x04015A: rMON (lo) + rYEAR (hi at 0x04015B)
    bus.register_io(RTC_BASE + 10, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(mon_) |
                   (static_cast<uint16_t>(year_) << 8);
        },
        [this](uint32_t addr, uint16_t v) {
            if (addr & 1)
                year_ = static_cast<uint8_t>(v);
            else { mon_  = static_cast<uint8_t>(v); year_ = static_cast<uint8_t>(v >> 8); }
        }
    });
}

#include "peripheral_wdt.hpp"
#include "peripheral_clkctl.hpp"
#include "bus.hpp"

void WatchdogTimer::attach(Bus& bus, const ClockControl& clk,
                             std::function<void(int, int)> assert_nmi)
{
    clk_        = &clk;
    assert_nmi_ = std::move(assert_nmi);

    // 0x040170: rWRWD (lo) + rEWD (hi)
    bus.register_io(0x040170, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(wrwd_) |
                   (static_cast<uint16_t>(ewd_) << 8);
        },
        [this](uint32_t, uint16_t v) {
            wrwd_ = static_cast<uint8_t>(v);
            ewd_  = static_cast<uint8_t>(v >> 8);
        }
    });
}

uint64_t WatchdogTimer::nmi_period() const
{
    uint32_t hz = clk_->cpu_clock_hz();
    if (hz == 0) return 48000; // fallback
    return static_cast<uint64_t>(hz) / 1000; // ~1 ms
}

void WatchdogTimer::tick(uint64_t cpu_cycles)
{
    // EWD bit 1 of rEWD
    if (!(ewd_ & 0x02)) {
        next_nmi_cycle_ = cpu_cycles + nmi_period();
        return;
    }

    if (cpu_cycles >= next_nmi_cycle_) {
        next_nmi_cycle_ = cpu_cycles + nmi_period();
        if (assert_nmi_)
            assert_nmi_(7, 0); // NMI = trap 7, level 0 (non-maskable)
    }
}

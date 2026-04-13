#include "peripheral_t16.hpp"
#include "peripheral_clkctl.hpp"
#include "bus.hpp"
#include <algorithm>

static constexpr uint32_t T16_BASE = 0x048180;

uint64_t Timer16bit::cycles_per_count() const
{
    int cksl = (ctl_ >> 3) & 1;
    uint32_t hz = clk_->t16_clock_hz(ch_, cksl);
    if (hz == 0) return 0;
    uint32_t cpu_hz = clk_->cpu_clock_hz();
    return (cpu_hz + hz - 1) / hz;
}

void Timer16bit::raise_cra()
{
    // T16_CRA sources are at even IrqSource slots per channel pair
    int base = static_cast<int>(InterruptController::IrqSource::T16_CRA0);
    intc_->raise(static_cast<InterruptController::IrqSource>(base + ch_ * 2));
}

void Timer16bit::raise_crb()
{
    int base = static_cast<int>(InterruptController::IrqSource::T16_CRB0);
    intc_->raise(static_cast<InterruptController::IrqSource>(base + ch_ * 2));
}

void Timer16bit::attach(Bus& bus,
                         InterruptController& intc,
                         const ClockControl& clk)
{
    intc_ = &intc;
    clk_  = &clk;

    uint32_t base = T16_BASE + static_cast<uint32_t>(ch_) * 8;

    // base+0: rCRA (16-bit)
    bus.register_io(base, {
        [this](uint32_t) -> uint16_t { return cra_; },
        [this](uint32_t, uint16_t v)  { cra_ = v; }
    });

    // base+2: rCRB (16-bit)
    bus.register_io(base + 2, {
        [this](uint32_t) -> uint16_t { return crb_; },
        [this](uint32_t, uint16_t v)  { crb_ = v; }
    });

    // base+4: rTC (16-bit)
    bus.register_io(base + 4, {
        [this](uint32_t) -> uint16_t { return tc_; },
        [this](uint32_t, uint16_t v)  { tc_ = v; }
    });

    // base+6: rT16CTL (lo byte), Dummy (hi byte)
    bus.register_io(base + 6, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(ctl_);
        },
        [this](uint32_t, uint16_t v) {
            uint8_t val = static_cast<uint8_t>(v);
            // PRESET bit: reset TC to 0 immediately
            if (val & 0x02) {
                tc_ = 0;
                next_tick_cycle_ = 0;
            }
            ctl_ = val & ~0x02u; // PRESET is self-clearing
        }
    });
}

uint64_t Timer16bit::next_wake_cycle() const
{
    if (!(ctl_ & 0x01)) return UINT64_MAX; // PRUN=0
    uint64_t cpc = cycles_per_count();
    if (cpc == 0) return UINT64_MAX;

    bool selfm0 = !(ctl_ & 0x40); // SELFM=0: CRA resets TC
    uint64_t wake = UINT64_MAX;

    // Counts until next CRA match.
    uint64_t counts_to_cra = UINT64_MAX;
    if (cra_ != 0) {
        counts_to_cra = (tc_ < cra_)
            ? static_cast<uint64_t>(cra_ - tc_)
            : static_cast<uint64_t>(cra_); // tc_ >= cra_: wait for next period
        wake = std::min(wake, next_tick_cycle_ + counts_to_cra * cpc);
    }

    // Counts until next CRB match (CRB does not reset TC).
    if (crb_ != 0) {
        uint64_t counts_to_crb;
        if (tc_ < crb_) {
            counts_to_crb = static_cast<uint64_t>(crb_ - tc_);
        } else if (selfm0 && cra_ != 0) {
            // CRB already passed this period; wait for CRA reset then count to CRB
            counts_to_crb = counts_to_cra + static_cast<uint64_t>(crb_);
        } else {
            // SELFM=1 or no CRA: TC wraps at 0x10000
            counts_to_crb = static_cast<uint64_t>(0x10000u - tc_) + crb_;
        }
        wake = std::min(wake, next_tick_cycle_ + counts_to_crb * cpc);
    }

    return wake;
}

void Timer16bit::tick(uint64_t cpu_cycles)
{
    if (!(ctl_ & 0x01)) return; // PRUN = 0

    uint64_t cpc = cycles_per_count();
    if (cpc == 0) return; // clock stopped

    while (cpu_cycles >= next_tick_cycle_) {
        next_tick_cycle_ += cpc;
        ++tc_;

        // Check CRB first (does not reset TC)
        if (crb_ != 0 && tc_ == crb_)
            raise_crb();

        // Check CRA (resets TC when SELFM=0)
        if (cra_ != 0 && tc_ == cra_) {
            raise_cra();
            if (!(ctl_ & 0x40)) // SELFM=0: reset TC
                tc_ = 0;
        }
    }
}

#include "peripheral_t16.hpp"
#include "peripheral_clkctl.hpp"
#include "bus.hpp"
#include <algorithm>

static constexpr uint32_t T16_BASE = 0x048180;

// Recompute cached_wake_ from current state.
// Mirrors next_wake_cycle() logic but writes the result to cached_wake_.
void Timer16bit::refresh_wake()
{
    if (!(ctl_ & 0x01)) { cached_wake_ = UINT64_MAX; return; }
    uint64_t cpc = cycles_per_count();
    if (cpc == 0)        { cached_wake_ = UINT64_MAX; return; }

    bool selfm0 = !(ctl_ & 0x40);
    uint64_t wake = UINT64_MAX;
    uint64_t counts_to_cra = UINT64_MAX;

    if (cra_ != 0) {
        counts_to_cra = (tc_ < cra_)
            ? static_cast<uint64_t>(cra_ - tc_)
            : static_cast<uint64_t>(cra_);
        wake = std::min(wake, next_tick_cycle_ + counts_to_cra * cpc);
    }
    if (crb_ != 0) {
        uint64_t counts_to_crb;
        if (tc_ < crb_) {
            counts_to_crb = static_cast<uint64_t>(crb_ - tc_);
        } else if (selfm0 && cra_ != 0) {
            counts_to_crb = counts_to_cra + static_cast<uint64_t>(crb_);
        } else {
            counts_to_crb = static_cast<uint64_t>(0x10000u - tc_) + crb_;
        }
        wake = std::min(wake, next_tick_cycle_ + counts_to_crb * cpc);
    }
    cached_wake_ = wake;
}

uint64_t Timer16bit::cycles_per_count() const
{
    uint32_t gen = clk_->config_gen();
    if (gen != cpc_gen_) {
        int cksl = (ctl_ >> 3) & 1;
        uint32_t hz = clk_->t16_clock_hz(ch_, cksl);
        cached_cpc_ = (hz == 0) ? 0
                                 : (clk_->cpu_clock_hz() + hz - 1) / hz;
        cpc_gen_ = gen;
    }
    return cached_cpc_;
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
        [this](uint32_t, uint16_t v)  { cra_ = v; refresh_wake(); }
    });

    // base+2: rCRB (16-bit)
    bus.register_io(base + 2, {
        [this](uint32_t) -> uint16_t { return crb_; },
        [this](uint32_t, uint16_t v)  { crb_ = v; refresh_wake(); }
    });

    // base+4: rTC (16-bit)
    bus.register_io(base + 4, {
        [this](uint32_t) -> uint16_t { return tc_; },
        [this](uint32_t, uint16_t v)  { tc_ = v; refresh_wake(); }
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
            refresh_wake();
        }
    });
}

uint64_t Timer16bit::next_wake_cycle() const
{
    // Return pre-computed value; updated by tick() and register-write handlers.
    return cached_wake_;
}

void Timer16bit::tick(uint64_t cpu_cycles)
{
    if (!(ctl_ & 0x01)) return; // PRUN = 0

    uint64_t cpc = cycles_per_count();
    if (cpc == 0) return; // clock stopped
    if (cpu_cycles < next_tick_cycle_) return;

    // Number of timer counts to process this tick.
    uint64_t counts = (cpu_cycles - next_tick_cycle_) / cpc + 1;
    next_tick_cycle_ += counts * cpc;

    bool selfm0 = !(ctl_ & 0x40); // SELFM=0: CRA resets TC

    if (selfm0 && cra_ != 0) {
        // ---- Common case: TC resets to 0 on each CRA match ------------------
        // O(1) analytical: compute N = number of CRA fires and final TC directly.
        // ISR bits are sticky/level-triggered, so raise_cra()/raise_crb() once
        // is equivalent to raising N times.
        //
        // CRB fires when TC passes through crb_.  In SELFM=0 mode TC resets at
        // cra_, so CRB can only fire when crb_ <= cra_.
        const bool crb_possible = (crb_ != 0 && crb_ <= cra_);

        uint64_t sum      = static_cast<uint64_t>(tc_) + counts;
        uint64_t N        = sum / static_cast<uint64_t>(cra_);
        uint16_t final_tc = static_cast<uint16_t>(sum % static_cast<uint64_t>(cra_));

        if (N == 0) {
            // No CRA fire: advance TC directly.
            if (crb_possible && tc_ < crb_ &&
                    static_cast<uint64_t>(crb_ - tc_) <= counts)
                raise_crb();
        } else {
            // One or more CRA fires. Raise CRA once (sticky ISR).
            if (crb_possible) {
                bool crb_fires = (tc_ < crb_)
                              || (N >= 2)
                              || (final_tc >= crb_);
                if (crb_fires) raise_crb();
            }
            raise_cra();
        }
        tc_ = final_tc;
    } else {
        // ---- Uncommon cases: SELFM=1 (no TC reset) or CRA==0 ---------------
        // TC counts freely and wraps at 0x10000.
        // CRA and CRB are plain compare-match points with no TC reset.
        if (cra_ == 0 && crb_ == 0) {
            // Neither compare set: just advance TC (no interrupts to fire).
            tc_ = static_cast<uint16_t>(tc_ + counts);
        } else {
            // Iterative: rare path, correctness over speed.
            while (counts-- > 0) {
                ++tc_; // uint16_t wraps at 0x10000
                if (crb_ != 0 && tc_ == crb_)
                    raise_crb();
                if (cra_ != 0 && tc_ == cra_)
                    raise_cra();
            }
        }
    }

    // Update cached wake point so next_wake_cycle() is free.
    refresh_wake();
}

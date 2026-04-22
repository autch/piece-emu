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

    // S1C33209 Tech Manual III-4: TC always counts 0 → CRB → 0 (period = CRB+1).
    // CRA is a plain compare-match that fires CRA IRQ and toggles the TMx
    // output pin (no TC reset).  SELFM (CTL bit 6) selects "fine mode" for
    // the CRA-driven output waveform duty (CRA[15:1] vs TC[14:0] with
    // CRA[0] picking a half-cycle offset) — it does NOT change which
    // register resets TC.  We don't model fine-mode output fidelity; the
    // emulator fires CRA IRQ when TC == CRA regardless.
    uint64_t wake = UINT64_MAX;
    uint64_t counts_to_crb = UINT64_MAX;

    if (crb_ != 0) {
        counts_to_crb = (tc_ < crb_)
            ? static_cast<uint64_t>(crb_ - tc_)
            : static_cast<uint64_t>(crb_);
        wake = std::min(wake, next_tick_cycle_ + counts_to_crb * cpc);
    }
    if (cra_ != 0) {
        uint64_t counts_to_cra;
        if (tc_ < cra_) {
            counts_to_cra = static_cast<uint64_t>(cra_ - tc_);
        } else if (crb_ != 0 && cra_ <= crb_) {
            counts_to_cra = counts_to_crb + static_cast<uint64_t>(cra_);
        } else {
            counts_to_cra = static_cast<uint64_t>(0x10000u - tc_) + cra_;
        }
        wake = std::min(wake, next_tick_cycle_ + counts_to_cra * cpc);
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

void Timer16bit::reset()
{
    cra_              = 0;
    crb_              = 0;
    tc_               = 0;
    ctl_              = 0;
    next_tick_cycle_  = 0;
    cached_cpc_       = 0;
    cpc_gen_          = UINT32_MAX;
    cached_wake_      = UINT64_MAX;
    update_active(false); // PRUN cleared
}

void Timer16bit::update_active(bool now_running)
{
    if (!active_mask_) return;
    if (now_running) *active_mask_ |=  active_bit_;
    else             *active_mask_ &= ~active_bit_;
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
            bool was_running = (ctl_ & 0x01) != 0;
            ctl_ = val & ~0x02u; // PRESET is self-clearing
            bool now_running = (ctl_ & 0x01) != 0;
            if (was_running != now_running) update_active(now_running);
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
    // Check the time gate before cycles_per_count().  cycles_per_count()
    // was accounting for 2.2% of runtime (2.75B calls in silent-app gprof);
    // the vast majority of those calls were for ticks where no timer count
    // was due, and the result would be discarded immediately below.
    if (cpu_cycles < next_tick_cycle_) return;

    uint64_t cpc = cycles_per_count();
    if (cpc == 0) return; // clock stopped

    // Number of timer counts to process this tick.
    uint64_t counts = (cpu_cycles - next_tick_cycle_) / cpc + 1;
    next_tick_cycle_ += counts * cpc;

    // TC counts 0 → CRB → 0 (period = CRB+1 ticks).  CRA is a compare-match
    // point within the cycle that fires its own IRQ without resetting TC.
    // SELFM (ctl_ & 0x40) is "fine mode" for the output-pin duty cycle and
    // does not affect TC reset — see S1C33209 Tech Manual III-4.
    if (crb_ != 0) {
        // ---- Common case: TC resets on CRB match, period = CRB+1 -----------
        // O(1) analytical: compute N CRB fires and final TC directly.
        // ISR bits are sticky/level-triggered, so one raise covers N matches.
        //
        // CRA fires when TC passes through cra_ (only reachable if CRA <= CRB).
        const bool cra_possible = (cra_ != 0 && cra_ <= crb_);

        // Period = CRB+1 clocks (TC counts 0..CRB inclusive).  The CRB
        // match fires when TC reaches crb_ (not crb_+1), so the #fires in
        // a range is counted by how many TC values in (tc_, v_end] hit
        // crb_, i.e.  floor((v_end - crb_) / period) + 1  when v_end >= crb_.
        const uint64_t period = static_cast<uint64_t>(crb_) + 1u;
        uint64_t v_end    = static_cast<uint64_t>(tc_) + counts;
        uint64_t N        = (v_end >= crb_)
                          ? ((v_end - crb_) / period + 1u)
                          : 0u;
        uint16_t final_tc = static_cast<uint16_t>(v_end % period);

        if (N == 0) {
            // No CRB fire: advance TC directly.
            if (cra_possible && tc_ < cra_ &&
                    static_cast<uint64_t>(cra_ - tc_) <= counts)
                raise_cra();
        } else {
            // One or more CRB fires. Raise CRB once (sticky ISR).
            if (cra_possible) {
                bool cra_fires = (tc_ < cra_ && tc_ + counts >= cra_)
                              || (N >= 2)
                              || (final_tc >= cra_);
                if (cra_fires) raise_cra();
            }
            raise_crb();
        }
        tc_ = final_tc;
    } else {
        // ---- Uncommon cases: CRB == 0 (free-run counter) ------------------
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

#include "peripheral_t8.hpp"
#include "peripheral_clkctl.hpp"
#include "bus.hpp"

// Base address for 8-bit timer channel registers
static constexpr uint32_t T8_BASE = 0x040160;

uint64_t Timer8bit::cycles_per_count() const
{
    uint32_t gen = clk_->config_gen();
    if (gen != cpc_gen_) {
        uint32_t hz = clk_->t8_clock_hz(ch_);
        cached_cpc_ = (hz == 0) ? 0
                                 : (clk_->cpu_clock_hz() + hz - 1) / hz;
        cpc_gen_ = gen;
    }
    return cached_cpc_;
}

void Timer8bit::on_ctl_write(uint8_t val)
{
    uint8_t old = ctl_;
    ctl_ = val & 0x07; // only bits 0-2 are writable

    // PSET=1: preset counter to reload value immediately
    if (ctl_ & 0x02) {
        ptd_ = rld_;
    }

    // Track PTRUN transitions in the parent-owned active mask.
    bool was_running = (old  & 0x01) != 0;
    bool now_running = (ctl_ & 0x01) != 0;
    if (was_running != now_running) update_active(now_running);
}

void Timer8bit::reset()
{
    ctl_              = 0;
    rld_              = 0;
    ptd_              = 0;
    next_tick_cycle_  = 0;
    cached_cpc_       = 0;
    cpc_gen_          = UINT32_MAX;
    update_active(false); // PTRUN cleared
}

void Timer8bit::update_active(bool now_running)
{
    if (!active_mask_) return;
    if (now_running) *active_mask_ |=  active_bit_;
    else             *active_mask_ &= ~active_bit_;
}

void Timer8bit::attach(Bus& bus,
                        InterruptController& intc,
                        const ClockControl& clk)
{
    intc_ = &intc;
    clk_  = &clk;

    // Each channel occupies 4 bytes. Bus handlers are 2-byte aligned.
    uint32_t base = T8_BASE + static_cast<uint32_t>(ch_) * 4;

    // [base+0]: rT8CTL (lo byte), [base+1]: rRLD (hi byte)
    bus.register_io(base, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(ctl_) |
                   (static_cast<uint16_t>(rld_) << 8);
        },
        [this](uint32_t addr, uint16_t v) {
            if (addr & 1) {
                // Byte write to odd address (rRLD): val in low bits.
                rld_ = static_cast<uint8_t>(v);
            } else {
                // Halfword write or byte write to even address (rT8CTL).
                on_ctl_write(static_cast<uint8_t>(v));
                rld_ = static_cast<uint8_t>(v >> 8);
            }
        }
    });

    // [base+2]: rPTD (lo, read only), [base+3]: Dummy (hi)
    bus.register_io(base + 2, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(ptd_);
        },
        [](uint32_t, uint16_t) { /* counter write absorbed */ }
    });
}

uint64_t Timer8bit::next_wake_cycle() const
{
    if (!(ctl_ & 0x01)) return UINT64_MAX; // PTRUN=0: stopped
    uint64_t cpc = cycles_per_count();
    if (cpc == 0) return UINT64_MAX;       // clock source disabled
    // ptd_==0: underflow fires at next_tick_cycle_ (checked before decrement)
    // ptd_>0:  ptd_ decrements remain before underflow
    return next_tick_cycle_ + static_cast<uint64_t>(ptd_) * cpc;
}

void Timer8bit::tick(uint64_t cpu_cycles)
{
    // Not running
    if (!(ctl_ & 0x01)) return;
    // Time gate before cycles_per_count() — skip the clock lookup on the
    // common "nothing due yet" path.  See peripheral_t16.cpp for rationale.
    if (cpu_cycles < next_tick_cycle_) return;

    uint64_t cpc = cycles_per_count();
    if (cpc == 0) return; // clock stopped

    // Number of T8 input-clock counts to process in this tick.
    uint64_t counts = (cpu_cycles - next_tick_cycle_) / cpc + 1;
    next_tick_cycle_ += counts * cpc;

    // T8 is a down-counter with reload: ptd_ starts at rld_, decrements
    // per count; when a tick sees ptd_ == 0 it raises IRQ and reloads.
    // Underflow period = rld_ + 1 counts.
    if (counts <= static_cast<uint64_t>(ptd_)) {
        // No underflow in this window.
        ptd_ -= static_cast<uint8_t>(counts);
    } else {
        // At least one underflow.  ISR bit is sticky so a single raise
        // covers any number of missed underflows within the window —
        // matches real-HW latching (multiple HW underflows while the
        // CPU has the interrupt masked only yield one pending IRQ).
        const uint64_t period    = static_cast<uint64_t>(rld_) + 1u;
        const uint64_t remaining = counts - ptd_ - 1u;
        ptd_ = static_cast<uint8_t>(rld_ - (remaining % period));
        intc_->raise(
            static_cast<InterruptController::IrqSource>(
                static_cast<int>(InterruptController::IrqSource::T8_UF0) + ch_));
    }
}

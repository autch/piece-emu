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

    // If PTRUN just started (0→1), initialise next tick reference
    if (!(old & 0x01) && (ctl_ & 0x01)) {
        // next_tick_cycle_ stays at its current value; tick() will catch up
    }
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

    uint64_t cpc = cycles_per_count();
    if (cpc == 0) return; // clock stopped

    // Advance counter for every elapsed tick cycle
    while (cpu_cycles >= next_tick_cycle_) {
        next_tick_cycle_ += cpc;

        if (ptd_ == 0) {
            // Underflow: reload and raise interrupt
            ptd_ = rld_;
            intc_->raise(
                static_cast<InterruptController::IrqSource>(
                    static_cast<int>(InterruptController::IrqSource::T8_UF0) + ch_));
        } else {
            --ptd_;
        }
    }
}

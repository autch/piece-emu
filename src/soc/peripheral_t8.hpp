#pragma once
#include "tick.hpp"
#include "peripheral_intc.hpp"
#include <cstdint>

class Bus;
class ClockControl;

// ============================================================================
// Timer8bit — S1C33209 8-bit programmable timer (c_T8)
//
// Four channels (ch = 0..3), each at:
//   0x040160 + ch * 4   rT8CTL   control (PTRUN[0], PSET[1], PTOUT[2])
//   0x040161 + ch * 4   rRLD     reload register
//   0x040162 + ch * 4   rPTD     counter (read: current value, write: absorbed)
//   0x040163 + ch * 4   Dummy
//
// Operation:
//   - When PTRUN=1: counter decrements once every cycles_per_count() CPU cycles.
//   - When counter reaches 0 after counting down from rld, it underflows:
//     counter is reloaded from rRLD and an interrupt is raised.
//   - PSET=1 write: counter is immediately preset to rRLD, counting continues.
//   - PTRUN=0: counter is frozen.
//
// Clock source: ClockControl::t8_clock_hz(ch)
// Interrupt:    InterruptController::IrqSource::T8_UF0 + ch
// ============================================================================
class Timer8bit : public ITickable {
public:
    explicit Timer8bit(int ch) : ch_(ch) {}

    void attach(Bus& bus,
                InterruptController& intc,
                const ClockControl& clk);

    void tick(uint64_t cpu_cycles) override;
    uint64_t next_wake_cycle() const override;

    // Direct register access (for unit tests)
    uint8_t ctl() const { return ctl_; }
    uint8_t rld() const { return rld_; }
    uint8_t ptd() const { return ptd_; }

private:
    int ch_;
    uint8_t ctl_ = 0;
    uint8_t rld_ = 0;
    uint8_t ptd_ = 0;

    uint64_t next_tick_cycle_ = 0;

    InterruptController* intc_ = nullptr;
    const ClockControl*  clk_  = nullptr;

    // Cached cycles-per-count.  Recomputed lazily when clk_->config_gen()
    // differs from the generation at which cpc_ was last computed.
    mutable uint64_t cached_cpc_ = 0;
    mutable uint32_t cpc_gen_    = UINT32_MAX; // mismatches any real gen

    // CPU cycles per one timer count. Returns 0 if clock is stopped.
    uint64_t cycles_per_count() const;

    void on_ctl_write(uint8_t val);
};

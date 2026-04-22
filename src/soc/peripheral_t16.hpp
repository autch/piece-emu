#pragma once
#include "peripheral_intc.hpp"
#include <cstdint>

class Bus;
class ClockControl;

// ============================================================================
// Timer16bit — S1C33209 16-bit programmable timer (c_T16)
//
// Six channels (ch = 0..5), each at (base = 0x048180 + ch * 8):
//   base+0  rCRA   comparison data A (16-bit)
//   base+2  rCRB   comparison data B (16-bit)
//   base+4  rTC    counter data (16-bit, read/write)
//   base+6  rT16CTL control (1-byte, lo), Dummy (1-byte, hi)
//
// rT16CTL bit layout (c_T16tag):
//   bit 0  PRUN    — Run/Stop (1=run)
//   bit 1  PRESET  — Reset TC to 0 when written 1 (self-clearing)
//   bit 2  PTM     — clock output control (ignored in emulation)
//   bit 3  CKSL    — input clock selection: 0=clock A, 1=clock B (from CLKCTL_T16)
//   bit 4  OUTINV  — output inversion (ignored)
//   bit 5  SELCRB  — use CRB as buffer for CRA (not implemented in basic emulation)
//   bit 6  SELFM   — fine mode: compare A resets TC only when SELFM=0
//
// Operation (SELFM=0, the common mode):
//   - TC counts up each cycles_per_count() CPU cycles.
//   - TC == CRA: raise compare A interrupt, reset TC to 0.
//   - TC == CRB: raise compare B interrupt (does not reset TC).
//   - PRESET=1 write: TC is set to 0.
//
// Clock source: ClockControl::t16_clock_hz(ch, ctl.CKSL)
// Interrupts:   IrqSource::T16_CRA{ch} and T16_CRB{ch}
//
// P/ECE kernel usage:
//   Timer 0 CRA at ~1 ms period drives the context-switch / system tick.
// ============================================================================
class Timer16bit {
public:
    explicit Timer16bit(int ch) : ch_(ch) {}

    void attach(Bus& bus,
                InterruptController& intc,
                const ClockControl& clk);

    void tick(uint64_t cpu_cycles);
    uint64_t next_wake_cycle() const;

    // Reset register state and cached clock data.  Preserves channel
    // number, attach-time bus/intc/clk pointers.
    void reset();

    // Wire up a PRUN-tracking bit in a parent-owned mask.  The parent
    // (PiecePeripherals) iterates only set bits to skip stopped timers
    // without loading their state.  Must be called before attach().
    void set_active_tracker(uint32_t* mask, uint32_t bit) {
        active_mask_ = mask;
        active_bit_  = bit;
    }

    // Direct register access (for unit tests)
    uint16_t cra() const { return cra_; }
    uint16_t crb() const { return crb_; }
    uint16_t tc()  const { return tc_;  }
    uint8_t  ctl() const { return ctl_; }

private:
    int ch_;
    uint16_t cra_ = 0;
    uint16_t crb_ = 0;
    uint16_t tc_  = 0;
    uint8_t  ctl_ = 0;

    uint64_t next_tick_cycle_ = 0;

    InterruptController* intc_ = nullptr;
    const ClockControl*  clk_  = nullptr;

    // Cached cycles-per-count (same invalidation scheme as Timer8bit).
    mutable uint64_t cached_cpc_ = 0;
    mutable uint32_t cpc_gen_    = UINT32_MAX;

    // Cached result of next_wake_cycle().  Updated at the end of every tick()
    // and after any register write that changes timer state (CRA/CRB/TC/CTL).
    // next_wake_cycle() simply returns this value, eliminating all arithmetic
    // from the hot path in update_timer_wake().
    uint64_t cached_wake_ = UINT64_MAX;

    uint64_t cycles_per_count() const;

    // Recompute cached_wake_ from current state.  Called by tick() and by
    // register-write I/O handlers.
    void refresh_wake();

    // PRUN-tracking: update the parent-owned mask whenever PRUN flips.
    void update_active(bool now_running);

    uint32_t* active_mask_ = nullptr;
    uint32_t  active_bit_  = 0;

    // Helper: raise CRA or CRB interrupt for this channel
    void raise_cra();
    void raise_crb();
};

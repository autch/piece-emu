#pragma once
#include <cstdint>
#include <functional>

class Bus;

// ============================================================================
// ClockControl — S1C33209 clock/prescaler control registers
//
// Covered registers (all 1-byte at the listed addresses):
//   0x040140  c_CLKSEL_T8_45   8-bit timer 4/5 clock selection
//   0x040145  c_CLKCTL_T8_45   8-bit timer 4/5 clock control
//   0x040146  c_CLKSEL_T8      8-bit timer 0-3 clock selection
//   0x040147  c_CLKCTL_T16_0   16-bit timer 0 clock control
//   0x040148  c_CLKCTL_T16_1
//   0x040149  c_CLKCTL_T16_2
//   0x04014A  c_CLKCTL_T16_3
//   0x04014B  c_CLKCTL_T16_4
//   0x04014C  c_CLKCTL_T16_5
//   0x04014D  c_CLKCTL_T8_01   8-bit timer 0/1 clock control
//   0x04014E  c_CLKCTL_T8_23   8-bit timer 2/3 clock control
//   0x040180  rPWRCTL          Power control (CLKCHG=bit2, CLKDT[1:0]=bits7:6)
//   0x040181  rCLKSEL          Prescaler clock select (PSCDT0=bit0)
//
// CLKCTL byte layout (c_CLKCTLtag):
//   bits [2:0] TSA  — clock A division: n → divide by 2^(n+1)
//   bit  [3]   TONA — clock A enable
//   bits [6:4] TSB  — clock B division
//   bit  [7]   TONB — clock B enable
//
// CLKSEL byte layout (c_CLKSELtag for T8):
//   0x040146 bits [3:0]: P8TPCK0..P8TPCK3 — T8 Ch.0..3 raw-prescaler bypass
//   0x040140 bits [1:0]: P8TPCK4, P8TPCK5 — T8 Ch.4/5 raw-prescaler bypass
//   When P8TPCKx = 1, T8 Ch.x runs on the prescaler INPUT clock (θ/1) — i.e.
//   no division applied — and the CLKCTL TSx setting is ignored.  Default 0.
//
// Note: T8 does NOT have a "clock A vs clock B" selector per channel.  Each
// channel has its own dedicated TS field within a shared CLKCTL byte:
//   Ch.0 → 0x4014D low nibble,  Ch.1 → 0x4014D high nibble
//   Ch.2 → 0x4014E low nibble,  Ch.3 → 0x4014E high nibble
//   Ch.4 → 0x40145 low nibble,  Ch.5 → 0x40145 high nibble
//
// CPU clock pipeline:
//   OSC3     = P07==0 ? 48 MHz : 24 MHz      (external oscillator-speed pin)
//   CPU      = OSC3 / 2^CLKDT                (PWRCTL bits 7:6, 0..3)
//
// CLKCHG (PWRCTL bit 2) selects OSC3 vs OSC1 path.  P/ECE always keeps it
// at 1 (OSC3 path), so we ignore CLKCHG and always route through OSC3.
//
// P/ECE default P07 = 1 (OSC3 = 24 MHz).  pceCPUSetSpeed() writes CLKDT to
// pick ×1/×1/2/×1/4/×1/8 of that.  Apps that want "48 MHz 倍速モード"
// explicitly drive P07 = 0 to switch OSC3 to 48 MHz, and restore P07 = 1
// before exiting back to the menu.
// ============================================================================
class ClockControl {
public:
    static constexpr uint32_t CPU_CLOCK_FAST = 48'000'000; // 48 MHz
    static constexpr uint32_t CPU_CLOCK_NORM = 24'000'000; // 24 MHz (P/ECE default)

    void attach(Bus& bus);

    // Reset all clock-control registers to power-on defaults.  Preserves
    // on_clock_change (assigned by the CPU runner).  The caller is
    // responsible for invoking on_clock_change after reset so the CPU
    // runner re-seeds timer wake points and pacing anchors.
    void reset();

    // Current CPU clock frequency in Hz.
    uint32_t cpu_clock_hz() const;

    // OSC3 frequency in Hz.  Selected by the P07 pin (1 → 24 MHz,
    // 0 → 48 MHz).  This is the base that CLKCTL TSx fields divide down
    // from for T8/T16 — independent of PWRCTL.CLKDT, which only divides
    // the CPU core clock.
    uint32_t osc3_hz() const;

    // Monotonically-increasing generation counter.  Incremented on every write
    // to any clock-control register (CLKSEL, CLKCTL, PWRCTL).  Timers compare
    // their cached-cpc generation against this to decide when to recompute.
    uint32_t config_gen() const { return config_gen_; }

    // Input clock for 16-bit timer channel ch (0..5).
    // Returns 0 if the selected clock source is disabled.
    // cksl: 0 = clock A, 1 = clock B (from timer's own CTL.CKSL bit)
    uint32_t t16_clock_hz(int ch, int cksl) const;

    // Input clock for 8-bit timer channel ch (0..3).
    // Clock source (A or B) is determined by CLKSEL_T8.
    // Returns 0 if the selected clock source is disabled.
    uint32_t t8_clock_hz(int ch) const;

    // Called when CPU clock frequency changes (e.g. CLKCHG bit toggled).
    std::function<void(uint32_t new_hz)> on_clock_change;

    // Called by PortCtrl when the P07 pin state changes.
    // slow=true (P07=1): 24 MHz;  slow=false (P07=0): 48 MHz.
    void set_p07(bool slow);

    // Direct register access (for unit tests)
    uint8_t pwrctl()           const { return pwrctl_; }
    uint8_t clksel_t8()        const { return clksel_t8_03_; }
    uint8_t clksel_t8_45()     const { return clksel_t8_45_; }
    uint8_t clkctl_t16(int ch) const { return clkctl_t16_[ch]; }
    uint8_t clkctl_t8_01()     const { return clkctl_t8_[0]; }
    uint8_t clkctl_t8_23()     const { return clkctl_t8_[1]; }
    uint8_t clkctl_t8_45()     const { return clkctl_t8_[2]; }

private:
    uint32_t config_gen_       = 0;   // incremented on any register write
    uint8_t  clksel_t8_03_     = 0;   // 0x040146: P8TPCK0..3 (bits 0..3)
    uint8_t  clksel_t8_45_     = 0;   // 0x040140: P8TPCK4..5 (bits 0..1)
    uint8_t  clkctl_t16_[6]    = {};  // 0x040147-0x04014C
    uint8_t  clkctl_t8_[3]     = {};  // [0]=0x4014D Ch.0/1,
                                      // [1]=0x4014E Ch.2/3,
                                      // [2]=0x40145  Ch.4/5
    uint8_t  pwrctl_           = 0;   // 0x040180 (CLKCHG bit2, CLKDT bits 7:6)
    bool     p07_slow_         = true; // P/ECE default: P07=1 → OSC3=24 MHz

    // Compute timer clock from a CLKCTL byte with a specific divisor table.
    // use_b picks the hi-nibble (B) vs lo-nibble (A) TS/TON fields.
    uint32_t clock_from_clkctl(uint8_t clkctl, bool use_b, uint32_t base,
                               const uint32_t (&div_table)[8]) const;

    void write_single(uint32_t addr, uint8_t val);
};

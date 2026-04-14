#include "peripheral_clkctl.hpp"
#include "bus.hpp"

// ---------------------------------------------------------------------------
// Clock division: CLKCTL.TSx[2:0] → divide by 2^(TSx+1)
//   TSx=0 → /2, TSx=1 → /4, ..., TSx=7 → /256
// ---------------------------------------------------------------------------
uint32_t ClockControl::clock_from_clkctl(uint8_t clkctl, bool use_b,
                                          uint32_t base) const
{
    uint8_t ts, ton;
    if (use_b) {
        ts  = (clkctl >> 4) & 0x07;
        ton = (clkctl >> 7) & 0x01;
    } else {
        ts  = (clkctl >> 0) & 0x07;
        ton = (clkctl >> 3) & 0x01;
    }
    if (!ton) return 0; // clock stopped
    uint32_t div = 1u << (ts + 1);
    return base / div;
}

uint32_t ClockControl::cpu_clock_hz() const
{
    // CLKCHG = bit 2 of pwrctl_
    bool clkchg = (pwrctl_ >> 2) & 1;
    return clkchg ? CPU_CLOCK_NORM : CPU_CLOCK_FAST;
}

uint32_t ClockControl::t16_clock_hz(int ch, int cksl) const
{
    return clock_from_clkctl(clkctl_t16_[ch], cksl != 0, cpu_clock_hz());
}

uint32_t ClockControl::t8_clock_hz(int ch) const
{
    // CLKSEL_T8: bit ch selects clock A (0) or B (1)
    bool use_b = (clksel_t8_ >> ch) & 1;
    uint8_t ctl = (ch <= 1) ? clkctl_t8_01_ : clkctl_t8_23_;
    return clock_from_clkctl(ctl, use_b, cpu_clock_hz());
}

void ClockControl::write_single(uint32_t addr, uint8_t val)
{
    switch (addr) {
    case 0x040146: clksel_t8_    = val; break;
    case 0x040147: clkctl_t16_[0] = val; break;
    case 0x040148: clkctl_t16_[1] = val; break;
    case 0x040149: clkctl_t16_[2] = val; break;
    case 0x04014A: clkctl_t16_[3] = val; break;
    case 0x04014B: clkctl_t16_[4] = val; break;
    case 0x04014C: clkctl_t16_[5] = val; break;
    case 0x04014D: clkctl_t8_01_ = val; break;
    case 0x04014E: clkctl_t8_23_ = val; break;
    case 0x040180: {
        uint8_t old = pwrctl_;
        pwrctl_ = val;
        // Notify on CLKCHG bit change (bit 2)
        if (((old ^ val) & 0x04) && on_clock_change)
            on_clock_change(cpu_clock_hz());
        break;
    }
    default: break; // other PWRCTL registers: absorb
    }
}

void ClockControl::set_p07(bool slow)
{
    // P07 controls OSC3 speed: 0=48 MHz, 1=24 MHz.
    // Mirror to PWRCTL.CLKCHG (bit 2) to keep cpu_clock_hz() consistent.
    uint8_t new_pwrctl = slow ? (pwrctl_ | 0x04u) : (pwrctl_ & ~0x04u);
    if (new_pwrctl == pwrctl_) return;
    pwrctl_ = new_pwrctl;
    if (on_clock_change) on_clock_change(cpu_clock_hz());
}

void ClockControl::attach(Bus& bus)
{
    // 0x040140: c_CLKSEL_T8_45 (lo) + Dummy (hi) — not needed, absorb
    // 0x040142, 0x040144: Dummy
    // 0x040144: Dummy (lo) + c_CLKCTL_T8_45 (hi at 0x040145)
    // 0x040146: c_CLKSEL_T8 (lo) + c_CLKCTL_T16_0 (hi at 0x040147)
    bus.register_io(0x040146, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(clksel_t8_) |
                   (static_cast<uint16_t>(clkctl_t16_[0]) << 8);
        },
        [this](uint32_t addr, uint16_t v) {
            if (addr & 1) { write_single(0x040147, static_cast<uint8_t>(v)); }
            else { write_single(0x040146, static_cast<uint8_t>(v));
                   write_single(0x040147, static_cast<uint8_t>(v >> 8)); }
        }
    });

    // 0x040148: c_CLKCTL_T16_1 (lo) + c_CLKCTL_T16_2 (hi)
    bus.register_io(0x040148, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(clkctl_t16_[1]) |
                   (static_cast<uint16_t>(clkctl_t16_[2]) << 8);
        },
        [this](uint32_t addr, uint16_t v) {
            if (addr & 1) { write_single(0x040149, static_cast<uint8_t>(v)); }
            else { write_single(0x040148, static_cast<uint8_t>(v));
                   write_single(0x040149, static_cast<uint8_t>(v >> 8)); }
        }
    });

    // 0x04014A: c_CLKCTL_T16_3 (lo) + c_CLKCTL_T16_4 (hi)
    bus.register_io(0x04014A, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(clkctl_t16_[3]) |
                   (static_cast<uint16_t>(clkctl_t16_[4]) << 8);
        },
        [this](uint32_t addr, uint16_t v) {
            if (addr & 1) { write_single(0x04014B, static_cast<uint8_t>(v)); }
            else { write_single(0x04014A, static_cast<uint8_t>(v));
                   write_single(0x04014B, static_cast<uint8_t>(v >> 8)); }
        }
    });

    // 0x04014C: c_CLKCTL_T16_5 (lo) + c_CLKCTL_T8_01 (hi)
    bus.register_io(0x04014C, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(clkctl_t16_[5]) |
                   (static_cast<uint16_t>(clkctl_t8_01_) << 8);
        },
        [this](uint32_t addr, uint16_t v) {
            if (addr & 1) { write_single(0x04014D, static_cast<uint8_t>(v)); }
            else { write_single(0x04014C, static_cast<uint8_t>(v));
                   write_single(0x04014D, static_cast<uint8_t>(v >> 8)); }
        }
    });

    // 0x04014E: c_CLKCTL_T8_23 (lo) + Dummy (hi at 0x04014F for AD clock)
    bus.register_io(0x04014E, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(clkctl_t8_23_);
        },
        [this](uint32_t, uint16_t v) {
            write_single(0x04014E, static_cast<uint8_t>(v));
        }
    });

    // 0x040180: rPWRCTL (lo) + rCLKSEL (hi at 0x040181, PSCDT0 — absorb)
    bus.register_io(0x040180, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(pwrctl_);
        },
        [this](uint32_t, uint16_t v) {
            write_single(0x040180, static_cast<uint8_t>(v));
        }
    });
}

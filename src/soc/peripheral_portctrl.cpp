#include "peripheral_portctrl.hpp"
#include "peripheral_clkctl.hpp"
#include "bus.hpp"

// K port addresses
static constexpr uint32_t KPORT_BASE = 0x0402C0;
// Port input interrupt addresses (0x0402C6..0x0402CF)
static constexpr uint32_t PINT_BASE  = 0x0402C6;
// P port addresses (0x0402D0..0x0402DF)
static constexpr uint32_t PPORT_BASE = 0x0402D0;

void PortCtrl::set_k5(uint8_t bits)
{
    k5d_ = bits;
    check_key_irq();
}

void PortCtrl::set_k6(uint8_t bits)
{
    k6d_ = bits;
    check_key_irq();
}

void PortCtrl::check_key_irq()
{
    if (!intc_) return;

    // KEY0 (FPK0): triggered when (k5d & ~SMPK0) == (SCPK0 & ~SMPK0)
    // The source port for FPK0 is selected by rSPPK.SPPK0[1:0]:
    //   00=K5, 01=K6 (others reserved)
    uint8_t mask0 = pint_[SMPK0];
    uint8_t cmp0  = pint_[SCPK0] & ~mask0;
    uint8_t act0;
    switch (pint_[SPPK] & 0x03) {
    case 0:  act0 = k5d_ & ~mask0; break;
    case 1:  act0 = k6d_ & ~mask0; break;
    default: act0 = 0; break;
    }
    if (act0 == cmp0)
        intc_->raise(InterruptController::IrqSource::KEY0);

    // KEY1 (FPK1): same logic with SPPK[3:2] and rSCPK1/rSMPK1
    uint8_t mask1 = pint_[SMPK1];
    uint8_t cmp1  = pint_[SCPK1] & ~mask1;
    uint8_t act1;
    switch ((pint_[SPPK] >> 2) & 0x03) {
    case 0:  act1 = k5d_ & ~mask1; break;
    case 1:  act1 = k6d_ & ~mask1; break;
    default: act1 = 0; break;
    }
    if (act1 == cmp1)
        intc_->raise(InterruptController::IrqSource::KEY1);
}

void PortCtrl::reset()
{
    cfk5_ = 0;
    k5d_  = 0xFF;
    cfk6_ = 0;
    k6d_  = 0xFF;
    for (auto& b : pint_)  b = 0;
    for (auto& b : pport_) b = 0;
    pport_[1] = 0x80; // P/ECE power-on default: P07=1 (OSC3 = 24 MHz)
}

void PortCtrl::attach(Bus& bus, InterruptController& intc, ClockControl* clk)
{
    intc_ = &intc;
    clk_  = clk;

    // K port — 0x0402C0..0x0402C4 (5 bytes, 3 halfword handlers)

    // 0x0402C0: rCFK5 (lo at 0x0402C0) + rK5D (hi at 0x0402C1, read-only)
    bus.register_io(KPORT_BASE, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(cfk5_) |
                   (static_cast<uint16_t>(k5d_) << 8);
        },
        [this](uint32_t addr, uint16_t v) {
            if (!(addr & 1))
                cfk5_ = static_cast<uint8_t>(v); // even: lo byte = rCFK5
            // k5d_ is input-only — CPU writes to odd address ignored
        }
    });

    // 0x0402C2: Dummy (lo at 0x0402C2) + rCFK6 (hi at 0x0402C3)
    bus.register_io(KPORT_BASE + 2, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(cfk6_) << 8;
        },
        [this](uint32_t addr, uint16_t v) {
            if (addr & 1)
                cfk6_ = static_cast<uint8_t>(v); // odd: hi byte = rCFK6
            else
                cfk6_ = static_cast<uint8_t>(v >> 8); // halfword: hi byte
        }
    });

    // 0x0402C4: rK6D (lo, read-only) + Dummy (hi at 0x0402C5)
    bus.register_io(KPORT_BASE + 4, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(k6d_);
        },
        [](uint32_t, uint16_t) { /* k6d_ is input-only */ }
    });

    // Port input interrupt — 0x0402C6..0x0402CF (10 bytes, 5 halfword handlers)
    // Byte writes to odd addresses (rSPT2, rSPPT, rSEPT, rSCPK1, rSMPK1, etc.)
    // must only update the targeted byte; addr & 1 distinguishes hi/lo.
    for (int i = 0; i < 5; i++) {
        bus.register_io(static_cast<uint32_t>(PINT_BASE + i * 2), {
            [this, i](uint32_t) -> uint16_t {
                return static_cast<uint16_t>(pint_[i * 2]) |
                       (static_cast<uint16_t>(pint_[i * 2 + 1]) << 8);
            },
            [this, i](uint32_t addr, uint16_t v) {
                if (addr & 1) {
                    pint_[i * 2 + 1] = static_cast<uint8_t>(v);
                } else {
                    pint_[i * 2]     = static_cast<uint8_t>(v);
                    pint_[i * 2 + 1] = static_cast<uint8_t>(v >> 8);
                }
            }
        });
    }

    // P port — 0x0402D0..0x0402DF (16 bytes, 8 halfword handlers)
    // Halfword i=0 covers 0x0402D0: lo=rCFP(P0), hi=rPD(P0).
    // P07 = Port 0 rPD bit 7 (pport_[1] bit 7); notify ClockControl on change.
    // P21D (RS signal to S6B0741) = Port 2 rPD bit 1 (pport_[9] bit 1).
    // The kernel uses byte writes to odd addresses (e.g. bp[0x2d9] |= 0x02)
    // to set individual port-data bits, so addr & 1 must be respected.
    for (int i = 0; i < 8; i++) {
        bus.register_io(static_cast<uint32_t>(PPORT_BASE + i * 2), {
            [this, i](uint32_t) -> uint16_t {
                return static_cast<uint16_t>(pport_[i * 2]) |
                       (static_cast<uint16_t>(pport_[i * 2 + 1]) << 8);
            },
            [this, i](uint32_t addr, uint16_t v) {
                uint8_t old_hi = pport_[i * 2 + 1];
                if (addr & 1) {
                    // Byte write to odd address: update only the high byte.
                    pport_[i * 2 + 1] = static_cast<uint8_t>(v);
                } else {
                    // Halfword write or byte write to even address: update both.
                    pport_[i * 2]     = static_cast<uint8_t>(v);
                    pport_[i * 2 + 1] = static_cast<uint8_t>(v >> 8);
                }
                // P07 detection: halfword i=0, hi byte = rPD(P0), bit 7
                if (i == 0 && clk_) {
                    uint8_t new_hi = pport_[1];
                    if ((old_hi ^ new_hi) & 0x80)
                        clk_->set_p07((new_hi & 0x80) != 0);
                }
            }
        });
    }
}

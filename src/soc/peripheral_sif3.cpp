#include "peripheral_sif3.hpp"
#include "peripheral_hsdma.hpp"
#include "peripheral_intc.hpp"
#include "bus.hpp"

// SIF3 byte-register addresses (S1C33209 c33209e.h):
//   0x401F4  SIF2_IRDA  (low byte of first handler pair)
//   0x401F5  SIF3_TXD   (high byte of first handler pair)
//   0x401F6  SIF3_RXD   (low byte of second handler pair)
//   0x401F7  SIF3_STATUS (high byte of second handler pair)
//   0x401F8  SIF3_CTL   (low byte of third handler pair)
//   0x401F9  SIF3_IRDA  (high byte of third handler pair)
//
// Bus I/O handlers are 2-byte aligned.  Byte registers at odd addresses are
// accessed as the high byte (val >> 8) of the enclosing 16-bit handler.
// The bus passes the original (possibly odd) address so handlers can
// distinguish low/high byte writes via (addr & 1).

static constexpr uint32_t SIF3_BASE = 0x0401F4; // first aligned pair

// STATUS register bits (read-only for CPU in P2-1 stub)
static constexpr uint8_t STATUS_TDBE = 0x02; // transmit data buffer empty

void Sif3::on_txd_write(uint8_t data)
{
    // Step 1: process the byte written directly to TXD
    if (txd_cb_) txd_cb_(data);

    // Step 2: if HSDMA Ch0 is enabled, run inline DMA (piemu-compatible)
    if (hsdma_ && hsdma_->ch0_en && bus_) {
        hsdma_->do_ch0_inline(*bus_, txd_cb_ ? txd_cb_ : [](uint8_t){});
    }
}

void Sif3::attach(Bus& bus, InterruptController& intc, Hsdma& hsdma)
{
    intc_  = &intc;
    hsdma_ = &hsdma;
    bus_   = &bus;

    // Handler pair at 0x401F4:
    //   low  byte (0x401F4) = SIF2_IRDA — absorb writes, return 0 on reads
    //   high byte (0x401F5) = SIF3_TXD  — write triggers LCD transfer
    bus.register_io(SIF3_BASE, {
        [](uint32_t) -> uint16_t {
            // SIF3_TXD (high byte) read: return 0 (write-only in practice)
            // SIF2_IRDA (low byte) read: return 0 (not emulated)
            return 0x0000u;
        },
        [this](uint32_t addr, uint16_t v) {
            if (addr & 1) {
                // Odd address = high byte write = SIF3_TXD write
                on_txd_write(static_cast<uint8_t>(v));
            }
            // Even address = SIF2_IRDA write — absorbed (not emulated)
        }
    });

    // Handler pair at 0x401F6:
    //   low  byte (0x401F6) = SIF3_RXD    — return 0 (no receive data)
    //   high byte (0x401F7) = SIF3_STATUS — TDBE always set (transmit ready)
    bus.register_io(SIF3_BASE + 2, {
        [this](uint32_t) -> uint16_t {
            // status_ in high byte, RXD = 0 in low byte
            return static_cast<uint16_t>(status_) << 8;
        },
        [](uint32_t, uint16_t) {
            // STATUS is read-only; RXD is read-only — writes absorbed
        }
    });

    // Handler pair at 0x401F8:
    //   low  byte (0x401F8) = SIF3_CTL  — R/W
    //   high byte (0x401F9) = SIF3_IRDA — R/W
    bus.register_io(SIF3_BASE + 4, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(ctl_) |
                   (static_cast<uint16_t>(irda_) << 8);
        },
        [this](uint32_t addr, uint16_t v) {
            if (addr & 1) {
                // Byte write to odd address 0x0401F9 (SIF3_IRDA): val in low bits.
                irda_ = static_cast<uint8_t>(v);
            } else {
                // Halfword write or byte write to even address 0x0401F8 (SIF3_CTL).
                ctl_  = static_cast<uint8_t>(v);
                irda_ = static_cast<uint8_t>(v >> 8);
            }
        }
    });
}

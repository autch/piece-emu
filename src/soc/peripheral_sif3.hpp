#pragma once
#include <cstdint>
#include <functional>

class Bus;
class InterruptController;
class Hsdma;

// ============================================================================
// Sif3 — S1C33209 Serial Interface Channel 3 (c_SIF3)
//
// Used by the P/ECE kernel to drive the S6B0741 LCD controller via SPI.
// The LCD controller is connected as:
//   SIF3 clock  — SCLK3 (P15)
//   SIF3 MOSI   — SOUT3 (P16)
//   LCD CS      — P20 (PortCtrl)
//   LCD RS (cmd/data select) — P21 (PortCtrl)
//   LCD RESET   — P33 (PortCtrl)
//
// Register map (byte registers, 28-bit CPU addresses):
//   0x0401F4  rSIF2_IRDA  (SIF2, low byte of this handler)
//   0x0401F5  rTXD        transmit data — write triggers LCD transfer
//   0x0401F6  rRXD        receive data  — read returns 0x00 (stub)
//   0x0401F7  rSTATUS     status — bit 1 = TDBE (always 1 for stub)
//   0x0401F8  rCTL        control (R/W)
//   0x0401F9  rIRDA       IrDA   (R/W)
//
// TXD write behaviour (piemu-compatible inline DMA):
//   1. Call txd_callback(byte) for the written byte.
//   2. If HSDMA Ch0 is enabled (hsdma_.ch0_en), delegate to
//      hsdma_.do_ch0_inline(bus, txd_callback) to drain the DMA buffer.
//
// txd_callback is set in P2-2 to route bytes into the S6B0741 LCD controller.
// In P2-1 stub mode, txd_callback is a no-op.
// ============================================================================

class Sif3 {
public:
    // Attach to the bus.
    //   intc: not used for interrupts in P2-1, stored for future Tx-end IRQ.
    //   hsdma: HSDMA controller (for Ch0 inline DMA on TXD writes).
    void attach(Bus& bus, InterruptController& intc, Hsdma& hsdma);

    // Register a callback invoked for each byte written to TXD.
    // In P2-2 this will route bytes to S6B0741.
    using TxdCallback = std::function<void(uint8_t)>;
    void set_txd_callback(TxdCallback cb) { txd_cb_ = std::move(cb); }

    // Direct register access (for unit tests)
    uint8_t ctl()    const { return ctl_; }
    uint8_t status() const { return status_; }

private:
    uint8_t ctl_    = 0;      // rCTL
    uint8_t irda_   = 0;      // rIRDA
    uint8_t status_ = 0x02;   // rSTATUS — TDBE (bit1) always set

    InterruptController* intc_  = nullptr;
    Hsdma*               hsdma_ = nullptr;
    Bus*                 bus_   = nullptr;

    TxdCallback txd_cb_;

    void on_txd_write(uint8_t data);
};

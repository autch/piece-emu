#pragma once
#include "peripheral_intc.hpp"
#include <cstdint>

class Bus;
class ClockControl;

// ============================================================================
// PortCtrl — S1C33209 input/output port controller
//
// Covers three register blocks:
//
// 1. K port (key input) — 0x0402C0..0x0402C4 (c_KPORTtag):
//    0x0402C0  rCFK5  K5 function select (bits [4:0])
//    0x0402C1  rK5D   K5 input data (read-only — set by set_k5())
//    0x0402C2  Dummy
//    0x0402C3  rCFK6  K6 function select (bits [7:0])
//    0x0402C4  rK6D   K6 input data (read-only — set by set_k6())
//
// 2. Port input interrupt — 0x0402C6..0x0402CF (c_PINTtag):
//    0x0402C6  rSPT1  port input interrupt select 1 (port 0-3 source sel)
//    0x0402C7  rSPT2  port input interrupt select 2 (port 4-7)
//    0x0402C8  rSPPT  polarity select
//    0x0402C9  rSEPT  edge/level select
//    0x0402CA  rSPPK  key input interrupt select (FPK0/FPK1 source)
//    0x0402CB  Dummy
//    0x0402CC  rSCPK0 key input FPK0 comparison data
//    0x0402CD  rSCPK1 key input FPK1 comparison data
//    0x0402CE  rSMPK0 key input FPK0 mask
//    0x0402CF  rSMPK1 key input FPK1 mask
//
// 3. P port (GPIO) — 0x0402D0..0x0402DF (c_PCTLtag × 4 ports):
//    Each port has 4 bytes: rCFP (func sel), rPD (data), rIOC (dir), rCFEX (ext func)
//    Port 0: 0x0402D0..0x0402D3
//    Port 1: 0x0402D4..0x0402D7
//    Port 2: 0x0402D8..0x0402DB
//    Port 3: 0x0402DC..0x0402DF
//
// P/ECE actual hardware wiring (from SDK docs "PIECE ポート解説"):
//   K50 = USB SUSPEND  (input, not a button)
//   K51 = USB INT-N    (input)
//   K52 = 赤外線受信入力 (input)
//   K53 = ボタン SELECT  (0=押下, 1=離)
//   K54 = ボタン START   (0=押下, 1=離)
//   K60 = ボタン右      (0=押下)
//   K61 = ボタン左
//   K62 = ボタン下
//   K63 = ボタン上
//   K64 = ボタン B
//   K65 = ボタン A
//   K66 = 電池電圧 ADC (analog in)
//   K67 = Di電圧 ADC   (analog in)
//
// SDL フロントエンドでのボタン → ビット変換:
//   set_k5(bits): bit3=SELECT, bit4=START  (K5[3:4], 1=離が初期値なので注意)
//   set_k6(bits): bit0=右, bit1=左, bit2=下, bit3=上, bit4=B, bit5=A
//
// Key input interrupts (KEY0/KEY1 = FPK0/FPK1):
//   Triggered when (K5D & ~rSMPK0) == (rSCPK0 & ~rSMPK0) for KEY0, etc.
//   P/ECE BIOS uses KEY0 (vector 20, priority 4) for standby wakeup.
// ============================================================================
class PortCtrl {
public:
    // clk: optional ClockControl for P07 (OSC3 speed) notification.
    void attach(Bus& bus, InterruptController& intc, ClockControl* clk = nullptr);

    // Called by the frontend (SDL) when button state changes.
    // bits: each bit corresponds to K5x or K6x input lines.
    void set_k5(uint8_t bits);
    void set_k6(uint8_t bits);

    // Direct register access (for unit tests)
    uint8_t k5d()  const { return k5d_;  }
    uint8_t k6d()  const { return k6d_;  }
    uint8_t cfk5() const { return cfk5_; }
    uint8_t cfk6() const { return cfk6_; }
    uint8_t pd(int port) const { return pport_[port * 4 + 1]; }

    // P21 (LCD RS line): bit 1 of P2D register.
    // 0 = command mode, 1 = data mode.
    bool p21d() const { return (pd(2) >> 1) & 1; }

private:
    // K port
    uint8_t cfk5_ = 0;
    uint8_t k5d_  = 0xFF; // input-only: written by set_k5(); active-low, 0xFF = all released
    uint8_t cfk6_ = 0;
    uint8_t k6d_  = 0xFF; // input-only: written by set_k6(); active-low, 0xFF = all released

    // Port input interrupt (10 bytes, 0x0402C6..0x0402CF)
    uint8_t pint_[10] = {};

    // P port (16 bytes, 0x0402D0..0x0402DF)
    uint8_t pport_[16] = {};

    InterruptController* intc_ = nullptr;
    ClockControl*        clk_  = nullptr;

    // Re-evaluate key interrupt conditions after K5D/K6D change.
    void check_key_irq();

    // rSCPK0, rSCPK1, rSMPK0, rSMPK1 offsets in pint_[]
    static constexpr int SCPK0 = 6; // pint_[6] = rSCPK0
    static constexpr int SCPK1 = 7; // pint_[7] = rSCPK1
    static constexpr int SMPK0 = 8; // pint_[8] = rSMPK0
    static constexpr int SMPK1 = 9; // pint_[9] = rSMPK1
    static constexpr int SPPK  = 4; // pint_[4] = rSPPK (source select)
};

#pragma once
#include <cstdint>
#include <functional>

class Bus;

// ============================================================================
// Hsdma — S1C33209 High-Speed DMA controller (c_HSDMA)
//
// Four channels (ch = 0..3), each channel at base + ch * 0x10:
//   Base = 0x048220
//   +0x0  rCNT   (32-bit) transfer counter and control
//   +0x4  rSADR  (32-bit) source address
//   +0x8  rDADR  (32-bit) destination address
//   +0xC  rHSEN  (16-bit) enable register  bit[0]=HS_EN
//   +0xE  rTF    (16-bit) trigger flag register bit[0]=HS_TF
//
// P/ECE usage:
//   Ch0 — LCD (SIF3 TXD): inline DMA triggered by SIF3 TXD write
//   Ch1 — Sound (PWM PCM buffer): triggered by EN 0→1 transition
//   Ch2/3 — unused by P/ECE kernel; register R/W accepted, no side effects
//
// Inline DMA for Ch0 (piemu-compatible):
//   The SIF3 peripheral calls hsdma.do_ch0_inline(bus, txd_callback) when
//   a TXD write occurs.  If ch0_en is set, this loops reading bus bytes from
//   ch0_sadr++, decrementing ch0_cnt, and invoking txd_callback(byte) until
//   the counter reaches zero (at which point ch0_en is cleared).
// ============================================================================

class Hsdma {
public:
    // Called by SIF3 after processing the initial TXD byte.
    // If ch0 EN is active, transfers ch0.cnt bytes from ch0.sadr into cb(byte).
    // Clears ch0 EN when the counter expires.
    using TxdCallback = std::function<void(uint8_t)>;
    void do_ch0_inline(Bus& bus, const TxdCallback& cb);

    // Attach to the bus — registers I/O handlers for all four channels.
    void attach(Bus& bus);

    // --- Public channel state (read by SIF3 / sound subsystem) ---

    // Ch0 state (LCD DMA)
    bool     ch0_en  = false;
    uint32_t ch0_cnt = 0;     // transfer counter (single-address mode bits [23:0])
    uint32_t ch0_sadr = 0;    // source address [27:0]

    // Ch1 state (sound DMA) — P2-4: sound subsystem registers a callback here
    bool     ch1_en  = false;
    uint32_t ch1_cnt = 0;
    uint32_t ch1_sadr = 0;

    // Callback invoked when Ch1 EN transitions 0→1 (sound buffer trigger).
    // Set by the sound subsystem in P2-4; nullptr = stub (no-op).
    std::function<void(Bus&, uint32_t sadr, uint32_t cnt)> on_ch1_start;

private:
    // Raw register storage for Ch0..3 (used for correct R/W passthrough)
    // Each channel: cnt32, sadr32, dadr32, en16, tf16
    struct ChanRegs {
        uint32_t cnt  = 0;
        uint32_t sadr = 0;
        uint32_t dadr = 0;
        uint16_t en   = 0;
        uint16_t tf   = 0;
    };
    ChanRegs chan_[4];

    void on_en_write(int ch, uint16_t val, Bus& bus);
    void register_channel(Bus& bus, int ch);
};

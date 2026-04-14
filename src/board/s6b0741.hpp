#pragma once
#include <cstdint>

class PortCtrl;

// ============================================================================
// S6b0741 — Samsung S6B0741 LCD controller (128×64 dots, used in P/ECE)
//
// The P/ECE hardware drives the LCD in a 128×88-dot configuration using the
// SIF3 serial interface.  Data bytes are sent MSB-first over SPI, but the
// hardware wiring inverts the bit order, so each byte received here must be
// bit-reversed before processing.
//
// The RS (register-select) signal is connected to P21 (PortCtrl port 2 bit 1):
//   P21 = 0 → command byte
//   P21 = 1 → data byte
//
// Command decoding (after bit-reversal):
//   0x00–0x0F  column address low nibble:  col2 = (col2 & 0xE0) | (cmd & 0x0F) << 1
//   0x10–0x17  column address high nibble: col2 = (col2 & 0x1E) | (cmd & 0x07) << 5
//   0xB0–0xBF  page address:              page = cmd & 0x0F
//
// Data write: vram[page][col2] = byte; col2 = (col2 + 1) & 0xFF
//
// VRAM layout: 16 pages × 256 columns.
// Conversion to pixels (to_pixels): 128×88, 2 bits per pixel (4 gray levels).
// The conversion is identical to piemu's lcdc_conv().
// ============================================================================

class S6b0741 {
public:
    // Attach to PortCtrl so that p21d() can be read on each write.
    void attach(PortCtrl& portctrl);

    // Called by Sif3's TxdCallback for each byte sent over SIF3.
    // Reads portctrl.p21d() to determine command vs. data mode.
    void write(uint8_t data);

    // Convert VRAM to a 128×88 pixel array.
    // Each element is a 2-bit value (0=white, 1=light gray, 2=dark gray, 3=black).
    // out[y][x], y in [0,87], x in [0,127].
    void to_pixels(uint8_t out[88][128]) const;

    // Raw VRAM access (for testing).
    const uint8_t* vram_page(int page) const { return vram_[page]; }
    int page()  const { return page_; }
    int col2()  const { return col2_; }

private:
    PortCtrl* portctrl_ = nullptr;

    uint8_t vram_[16][256] = {};
    int     page_ = 0;
    int     col2_ = 0;
};

#include "s6b0741.hpp"
#include "peripheral_portctrl.hpp"

void S6b0741::attach(PortCtrl& portctrl)
{
    portctrl_ = &portctrl;
}

void S6b0741::write(uint8_t data)
{
    // Hardware wiring inverts bit order — reverse all 8 bits before processing.
    data = static_cast<uint8_t>(
        ((data & 0x01u) << 7) |
        ((data & 0x02u) << 5) |
        ((data & 0x04u) << 3) |
        ((data & 0x08u) << 1) |
        ((data & 0x10u) >> 1) |
        ((data & 0x20u) >> 3) |
        ((data & 0x40u) >> 5) |
        ((data & 0x80u) >> 7));

    bool is_data = portctrl_ && portctrl_->p21d();

    if (!is_data) {
        // Command byte
        switch (data & 0xF0u) {
        case 0x00u: // column address low nibble
            col2_ = (col2_ & 0xE0) | ((data & 0x0F) << 1);
            break;
        case 0x10u: // column address high nibble
            col2_ = (col2_ & 0x1E) | ((data & 0x07) << 5);
            break;
        case 0xB0u: // page address
            page_ = data & 0x0F;
            break;
        default:
            // Other S6B0741 commands (display on/off, contrast, etc.) — absorbed.
            break;
        }
    } else {
        // Data byte: write to VRAM and advance column pointer.
        vram_[page_][col2_] = data;
        col2_ = (col2_ + 1) & 0xFF;
    }
}

// Convert VRAM to a 128×88 2bpp pixel array.
// Replicates piemu's lcdc_conv() exactly.
//
// VRAM layout:
//   16 "pages", each contributing 8 pixel-columns in X.
//   Page 10 → rightmost 8 columns (x=127..120); page ordering wraps so that
//   page 0 → x=79..72, page 9 → x=7..0, page 10 → x=127..120, etc.
//
//   Within a page, column index col (0..87) maps to pixel row y=col.
//   Each row is encoded as two bytes: vram[page][col*2+0] (low bit source)
//   and vram[page][col*2+1] (high bit source).
//   For pixel i (i=0..7): pixel_value = (byte0 & 1) << 1 | (byte1 & 1);
//   then both bytes are shifted right.
void S6b0741::to_pixels(uint8_t out[88][128]) const
{
    for (int page = 0; page < 16; page++) {
        int x = 127 - ((page - 10) & 0x0F) * 8;
        for (int col = 0; col < 88; col++) {
            uint8_t c1 = vram_[page][col * 2 + 0];
            uint8_t c2 = vram_[page][col * 2 + 1];
            for (int i = 0; i < 8; i++) {
                out[col][x - i] = static_cast<uint8_t>((c1 & 1u) << 1 | (c2 & 1u));
                c1 >>= 1;
                c2 >>= 1;
            }
        }
    }
}

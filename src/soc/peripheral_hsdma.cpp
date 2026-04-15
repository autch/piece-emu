#include "peripheral_hsdma.hpp"
#include "bus.hpp"

// Base address for HSDMA registers
// Ch0: 0x048220, Ch1: 0x048230, Ch2: 0x048240, Ch3: 0x048250
static constexpr uint32_t HSDMA_BASE   = 0x048220;
static constexpr uint32_t CHAN_STRIDE   = 0x10;   // 16 bytes per channel

// Within a channel (offsets from channel base):
//   +0x0  rCNT  (32-bit → two 16-bit handlers)
//   +0x4  rSADR (32-bit)
//   +0x8  rDADR (32-bit)
//   +0xC  rHSEN (16-bit)
//   +0xE  rTF   (16-bit)

static constexpr uint32_t OFF_CNT_LO  = 0x0;
static constexpr uint32_t OFF_CNT_HI  = 0x2;
static constexpr uint32_t OFF_SADR_LO = 0x4;
static constexpr uint32_t OFF_SADR_HI = 0x6;
static constexpr uint32_t OFF_DADR_LO = 0x8;
static constexpr uint32_t OFF_DADR_HI = 0xA;
static constexpr uint32_t OFF_EN      = 0xC;
static constexpr uint32_t OFF_TF      = 0xE;

// HS_EN register bit 0 = HS_EN (enable)
static constexpr uint16_t HS_EN_BIT = 0x0001;

void Hsdma::do_ch0_inline(Bus& bus, const TxdCallback& cb)
{
    // Transfer bytes from ch0_sadr while ch0 EN is active.
    // This is called after the initial TXD byte has already been processed by SIF3.
    // Matches piemu's IO_W(pSIF3_TXD) loop logic.
    bool completed = false;
    while (ch0_en && ch0_cnt > 0) {
        uint8_t data = bus.read8(ch0_sadr);
        ch0_sadr++;
        ch0_cnt--;
        // Sync back to raw registers
        chan_[0].sadr = ch0_sadr;
        chan_[0].cnt  = ch0_cnt;
        if (ch0_cnt == 0) {
            ch0_en        = false;
            chan_[0].en  &= ~HS_EN_BIT;
            completed     = true;
        }
        cb(data);
    }
    if (completed && on_ch0_complete)
        on_ch0_complete();
}

void Hsdma::on_en_write(int ch, uint16_t val, Bus& bus)
{
    bool new_en = (val & HS_EN_BIT) != 0;
    bool old_en = (chan_[ch].en & HS_EN_BIT) != 0;
    chan_[ch].en = val;

    if (ch == 0) {
        ch0_en   = new_en;
        ch0_cnt  = chan_[0].cnt & 0x00FFFFFF; // single-address mode TC [23:0]
        ch0_sadr = chan_[0].sadr & 0x0FFFFFFF;
    } else if (ch == 1) {
        ch1_en   = new_en;
        ch1_cnt  = chan_[1].cnt & 0x00FFFFFF;
        ch1_sadr = chan_[1].sadr & 0x0FFFFFFF;

        // Rising edge (0→1): notify sound subsystem
        if (!old_en && new_en && on_ch1_start)
            on_ch1_start(bus, ch1_sadr, ch1_cnt);
    }
}

void Hsdma::register_channel(Bus& bus, int ch)
{
    uint32_t base = HSDMA_BASE + static_cast<uint32_t>(ch) * CHAN_STRIDE;

    // rCNT low halfword (bits [15:0])
    bus.register_io(base + OFF_CNT_LO, {
        [this, ch](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(chan_[ch].cnt);
        },
        [this, ch](uint32_t, uint16_t v) {
            chan_[ch].cnt = (chan_[ch].cnt & 0xFFFF0000u) | v;
            if (ch == 0) ch0_cnt = chan_[0].cnt & 0x00FFFFFFu;
            else if (ch == 1) ch1_cnt = chan_[1].cnt & 0x00FFFFFFu;
        }
    });

    // rCNT high halfword (bits [31:16])
    bus.register_io(base + OFF_CNT_HI, {
        [this, ch](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(chan_[ch].cnt >> 16);
        },
        [this, ch](uint32_t, uint16_t v) {
            chan_[ch].cnt = (chan_[ch].cnt & 0x0000FFFFu) | (static_cast<uint32_t>(v) << 16);
            if (ch == 0) ch0_cnt = chan_[0].cnt & 0x00FFFFFFu;
            else if (ch == 1) ch1_cnt = chan_[1].cnt & 0x00FFFFFFu;
        }
    });

    // rSADR low halfword
    bus.register_io(base + OFF_SADR_LO, {
        [this, ch](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(chan_[ch].sadr);
        },
        [this, ch](uint32_t, uint16_t v) {
            chan_[ch].sadr = (chan_[ch].sadr & 0xFFFF0000u) | v;
            if (ch == 0) ch0_sadr = chan_[0].sadr & 0x0FFFFFFFu;
            else if (ch == 1) ch1_sadr = chan_[1].sadr & 0x0FFFFFFFu;
        }
    });

    // rSADR high halfword
    bus.register_io(base + OFF_SADR_HI, {
        [this, ch](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(chan_[ch].sadr >> 16);
        },
        [this, ch](uint32_t, uint16_t v) {
            chan_[ch].sadr = (chan_[ch].sadr & 0x0000FFFFu) | (static_cast<uint32_t>(v) << 16);
            if (ch == 0) ch0_sadr = chan_[0].sadr & 0x0FFFFFFFu;
            else if (ch == 1) ch1_sadr = chan_[1].sadr & 0x0FFFFFFFu;
        }
    });

    // rDADR low halfword
    bus.register_io(base + OFF_DADR_LO, {
        [this, ch](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(chan_[ch].dadr);
        },
        [this, ch](uint32_t, uint16_t v) {
            chan_[ch].dadr = (chan_[ch].dadr & 0xFFFF0000u) | v;
        }
    });

    // rDADR high halfword
    bus.register_io(base + OFF_DADR_HI, {
        [this, ch](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(chan_[ch].dadr >> 16);
        },
        [this, ch](uint32_t, uint16_t v) {
            chan_[ch].dadr = (chan_[ch].dadr & 0x0000FFFFu) | (static_cast<uint32_t>(v) << 16);
        }
    });

    // rHSEN (16-bit)
    bus.register_io(base + OFF_EN, {
        [this, ch](uint32_t) -> uint16_t {
            return chan_[ch].en;
        },
        [this, ch, &bus](uint32_t, uint16_t v) {
            on_en_write(ch, v, bus);
        }
    });

    // rTF (16-bit trigger flag — write clears, read returns status)
    bus.register_io(base + OFF_TF, {
        [this, ch](uint32_t) -> uint16_t {
            return chan_[ch].tf;
        },
        [this, ch](uint32_t, uint16_t v) {
            // Writing 1 clears the corresponding trigger flag bit
            chan_[ch].tf &= ~v;
        }
    });
}

void Hsdma::attach(Bus& bus)
{
    for (int ch = 0; ch < 4; ch++)
        register_channel(bus, ch);
}

#include "peripheral_bcu_area.hpp"
#include "bus.hpp"
#include "cpu.hpp"

static constexpr uint32_t BCU_BASE = 0x048120;

void BcuAreaCtrl::on_a6_4_write(uint16_t val)
{
    a6_4_ = val;
    // A5WT[2:0] → SRAM wait (for Area 5/4 = 0x100000..0x1FFFFF)
    bus_->sram_wait = static_cast<int>(val & 0x07);
}

void BcuAreaCtrl::on_a10_9_write(uint16_t val)
{
    a10_9_ = val;
    // A10WT[2:0] → Flash wait (for Area 10 = 0xC00000..)
    bus_->flash_wait = static_cast<int>(val & 0x07);
}

void BcuAreaCtrl::on_ttbr_write(uint32_t val)
{
    ttbr_ = val;
    cpu_->state.ttbr = val;
}

void BcuAreaCtrl::attach(Bus& bus, Cpu& cpu)
{
    bus_ = &bus;
    cpu_ = &cpu;

    // 0x048120: rA18_15
    bus.register_io(BCU_BASE + 0x00, {
        [this](uint32_t) -> uint16_t { return a18_15_; },
        [this](uint32_t, uint16_t v)  { a18_15_ = v; }
    });
    // 0x048122: rA14_13
    bus.register_io(BCU_BASE + 0x02, {
        [this](uint32_t) -> uint16_t { return a14_13_; },
        [this](uint32_t, uint16_t v)  { a14_13_ = v; }
    });
    // 0x048124: rA12_11
    bus.register_io(BCU_BASE + 0x04, {
        [this](uint32_t) -> uint16_t { return a12_11_; },
        [this](uint32_t, uint16_t v)  { a12_11_ = v; }
    });
    // 0x048126: rA10_9 → updates flash_wait
    bus.register_io(BCU_BASE + 0x06, {
        [this](uint32_t) -> uint16_t { return a10_9_; },
        [this](uint32_t, uint16_t v)  { on_a10_9_write(v); }
    });
    // 0x048128: rA8_7
    bus.register_io(BCU_BASE + 0x08, {
        [this](uint32_t) -> uint16_t { return a8_7_; },
        [this](uint32_t, uint16_t v)  { a8_7_ = v; }
    });
    // 0x04812A: rA6_4 → updates sram_wait
    bus.register_io(BCU_BASE + 0x0A, {
        [this](uint32_t) -> uint16_t { return a6_4_; },
        [this](uint32_t, uint16_t v)  { on_a6_4_write(v); }
    });
    // 0x04812C: Dummy (lo) + rTBRP (hi) — absorb writes
    bus.register_io(BCU_BASE + 0x0C, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t)    { }
    });
    // 0x04812E: rBUS
    bus.register_io(BCU_BASE + 0x0E, {
        [this](uint32_t) -> uint16_t { return bus_ctl_; },
        [this](uint32_t, uint16_t v)  { bus_ctl_ = v; }
    });
    // 0x048130: rDRAM
    bus.register_io(BCU_BASE + 0x10, {
        [this](uint32_t) -> uint16_t { return dram_; },
        [this](uint32_t, uint16_t v)  { dram_ = v; }
    });
    // 0x048132: rACCESS
    bus.register_io(BCU_BASE + 0x12, {
        [this](uint32_t) -> uint16_t { return access_; },
        [this](uint32_t, uint16_t v)  { access_ = v; }
    });
    // 0x048134: rTTBR lo halfword
    bus.register_io(BCU_BASE + 0x14, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(ttbr_);
        },
        [this](uint32_t, uint16_t v) {
            on_ttbr_write((ttbr_ & 0xFFFF0000u) | v);
        }
    });
    // 0x048136: rTTBR hi halfword
    bus.register_io(BCU_BASE + 0x16, {
        [this](uint32_t) -> uint16_t {
            return static_cast<uint16_t>(ttbr_ >> 16);
        },
        [this](uint32_t, uint16_t v) {
            on_ttbr_write((ttbr_ & 0x0000FFFFu) |
                          (static_cast<uint32_t>(v) << 16));
        }
    });
    // 0x048138: rGA, 0x04813A: rBCLKSEL — absorb
    bus.register_io(BCU_BASE + 0x18, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t)    { }
    });
    bus.register_io(BCU_BASE + 0x1A, {
        [](uint32_t) -> uint16_t { return 0; },
        [](uint32_t, uint16_t)    { }
    });
}

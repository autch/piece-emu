#include "piece_peripherals.hpp"
#include "bus.hpp"
#include "cpu.hpp"

#include <algorithm>
#include <cstdint>

void PiecePeripherals::attach(Bus& bus, Cpu& cpu)
{
    intc.attach(bus, [&cpu](int trap_no, int level) {
        cpu.assert_trap(trap_no, level);
    });
    clk.attach(bus);
    // Wire the PRUN-tracking bits before attach() so any PRUN transition
    // during reset/register-initialisation lands in the parent mask.
    // T16 occupies bits 0..5, T8 occupies bits 8..11 of timer_active_mask_.
    for (int i = 0; i < 4; i++) {
        t8_ch[i].set_active_tracker(&timer_active_mask_, 1u << (8 + i));
        t8_ch[i].attach(bus, intc, clk);
    }
    for (int i = 0; i < 6; i++) {
        t16_ch[i].set_active_tracker(&timer_active_mask_, 1u << i);
        t16_ch[i].attach(bus, intc, clk);
    }
    portctrl.attach(bus, intc, &clk);
    bcu_area.attach(bus, cpu);
    wdt.attach(bus, clk, [&cpu, this](int no, int lvl) {
        ++nmi_count;
        cpu.assert_trap(no, lvl);
    });
    rtc.attach(bus, intc, clk);
    hsdma.attach(bus);
    sif3.attach(bus, intc, hsdma);
    lcd.attach(portctrl);
    sif3.set_txd_callback([this](uint8_t b) { lcd.write(b); });
}

void PiecePeripherals::tick(uint64_t cycles)
{
    // Iterate only running timers via the PRUN-bitmap.  T16 channels
    // occupy bits 0..5, T8 channels occupy bits 8..11.
    uint32_t mt16 = timer_active_mask_ & 0x003Fu;
    uint32_t mt8  = (timer_active_mask_ >> 8) & 0x000Fu;
    while (mt16) {
        int i = __builtin_ctz(mt16);
        t16_ch[i].tick(cycles);
        mt16 &= mt16 - 1;
    }
    while (mt8) {
        int i = __builtin_ctz(mt8);
        t8_ch[i].tick(cycles);
        mt8 &= mt8 - 1;
    }
    wdt.tick(cycles);
    rtc.tick(cycles);
    sound.tick(cycles);
    wake_valid_ = false; // per-peripheral wake points may have changed
}

uint64_t PiecePeripherals::next_wake_cycle()
{
    if (wake_valid_) return wake_cached_;
    uint64_t wake = UINT64_MAX;
    for (int i = 0; i < 4; i++) wake = std::min(wake, t8_ch[i].next_wake_cycle());
    for (int i = 0; i < 6; i++) wake = std::min(wake, t16_ch[i].next_wake_cycle());
    wake = std::min(wake, rtc.next_wake_cycle());
    wake = std::min(wake, wdt.next_wake_cycle());
    wake = std::min(wake, sound.next_wake_cycle());
    wake_cached_ = wake;
    wake_valid_  = true;
    return wake;
}

uint64_t PiecePeripherals::sleep_wake_cycle()
{
    return rtc.next_wake_cycle();
}

void PiecePeripherals::reset(bool cold)
{
    intc.reset();
    clk.reset();
    for (int i = 0; i < 4; i++) t8_ch[i].reset();
    for (int i = 0; i < 6; i++) t16_ch[i].reset();
    wdt.reset();
    rtc.reset();
    hsdma.reset();
    sif3.reset();
    sound.reset();
    nmi_count = 0;
    wake_valid_ = false;
    timer_active_mask_ = 0; // per-timer reset() clears PRUN and their bits

    if (cold) {
        // C33 tech manual 表3.2: cold start also initialises the BCU
        // external bus registers (0x48120..0x4813F) and the I/O port
        // control/data registers (0x402C0..0x402DF).  Power-cycle the
        // external LCD too — hot start leaves its contents intact.
        bcu_area.reset();
        portctrl.reset();
        lcd.reset();
    }
}

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
    for (int i = 0; i < 4; i++) t8_ch[i].attach(bus, intc, clk);
    for (int i = 0; i < 6; i++) t16_ch[i].attach(bus, intc, clk);
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
    for (int i = 0; i < 4; i++) t8_ch[i].tick(cycles);
    for (int i = 0; i < 6; i++) t16_ch[i].tick(cycles);
    wdt.tick(cycles);
    rtc.tick(cycles);
    sound.tick(cycles);
}

uint64_t PiecePeripherals::next_wake_cycle()
{
    uint64_t wake = UINT64_MAX;
    for (int i = 0; i < 4; i++) wake = std::min(wake, t8_ch[i].next_wake_cycle());
    for (int i = 0; i < 6; i++) wake = std::min(wake, t16_ch[i].next_wake_cycle());
    wake = std::min(wake, rtc.next_wake_cycle());
    wake = std::min(wake, wdt.next_wake_cycle());
    wake = std::min(wake, sound.next_wake_cycle());
    return wake;
}

uint64_t PiecePeripherals::sleep_wake_cycle()
{
    return rtc.next_wake_cycle();
}

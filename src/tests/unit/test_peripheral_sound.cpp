#include <gtest/gtest.h>
#include "peripheral_sound.hpp"
#include "peripheral_hsdma.hpp"
#include "peripheral_intc.hpp"
#include "peripheral_clkctl.hpp"
#include "bus.hpp"

#include <cstdint>
#include <vector>

// ============================================================================
// Sound unit tests
// ============================================================================

class SoundFixture : public ::testing::Test {
protected:
    Bus                 bus;
    Hsdma               hsdma;
    InterruptController intc;
    ClockControl        clk;
    Sound               sound;

    int  last_trap_no  = -1;
    int  last_trap_lvl = -1;
    uint64_t cur_cycles = 0;

    SoundFixture() : bus(0x40000, 0x80000) {
        intc.attach(bus, [this](int no, int lv) {
            last_trap_no  = no;
            last_trap_lvl = lv;
        });
        clk.attach(bus);
        hsdma.attach(bus);
        sound.attach(bus, hsdma, intc, clk,
                     [this]() { return cur_cycles; },
                     nullptr);
        bus.write16(0x040260 + 16, 0x0200); // IEN2 bit 1 = HSDMA1
    }

    void kick_ch1(uint32_t sadr, uint32_t cnt) {
        bus.write16(0x048230, static_cast<uint16_t>(cnt));
        bus.write16(0x048232, static_cast<uint16_t>(cnt >> 16));
        bus.write16(0x048234, static_cast<uint16_t>(sadr));
        bus.write16(0x048236, static_cast<uint16_t>(sadr >> 16));
        bus.write16(0x04823C, 0x0001);
    }
};

TEST_F(SoundFixture, ReadsSamplesOnEnableRisingEdge) {
    constexpr uint32_t SRAM = 0x100000;
    for (int i = 0; i < 128; i++)
        bus.write16(SRAM + i * 2, 750); // silence centre

    kick_ch1(SRAM, 128);
    EXPECT_EQ(sound.available(), 128u); // all samples read immediately

    int16_t out[128] = {};
    EXPECT_EQ(sound.pop(out, 128), 128u);
    for (int16_t s : out) EXPECT_NEAR(s, 0, 100);
}

TEST_F(SoundFixture, DefersCompletionByCycleCount) {
    constexpr uint32_t SRAM = 0x100000;
    for (int i = 0; i < 128; i++) bus.write16(SRAM + i * 2, 750);

    cur_cycles = 1000;
    kick_ch1(SRAM, 128);

    const uint64_t expected = 1000 + 128ULL * clk.cpu_clock_hz() / 32'000ULL;
    EXPECT_EQ(sound.next_wake_cycle(), expected);

    sound.tick(expected - 1);
    EXPECT_EQ(last_trap_no, -1);
    EXPECT_TRUE(hsdma.ch1_en);

    sound.tick(expected);
    EXPECT_EQ(last_trap_no, 23);
    EXPECT_EQ(last_trap_lvl, 1);
    EXPECT_FALSE(hsdma.ch1_en);
    EXPECT_EQ(sound.next_wake_cycle(), UINT64_MAX);
}

TEST_F(SoundFixture, PwmToPcmRange) {
    constexpr uint32_t SRAM = 0x100000;
    bus.write16(SRAM + 0, 0);
    bus.write16(SRAM + 2, 749);
    bus.write16(SRAM + 4, 750);
    bus.write16(SRAM + 6, 1500);

    kick_ch1(SRAM, 4);
    int16_t s[4] = {};
    EXPECT_EQ(sound.pop(s, 4), 4u);
    EXPECT_LT(s[0], -20000);
    EXPECT_LT(s[1], 0);
    EXPECT_NEAR(s[2], 0, 100);
    EXPECT_GT(s[3],  20000);
}

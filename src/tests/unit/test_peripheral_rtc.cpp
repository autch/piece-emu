#include <gtest/gtest.h>
#include "peripheral_rtc.hpp"
#include "peripheral_clkctl.hpp"
#include "peripheral_intc.hpp"
#include "bus.hpp"

// ============================================================================
// ClockTimer (RTC) unit tests
// ============================================================================

// IO address range: RTC registers span 0x040150–0x04015B.
// ClockControl uses 0x040140–0x04014E and 0x040180.
// InterruptController uses 0x040260–0x04029F.
// Use a Bus window that covers all three ranges.
static constexpr uint32_t BUS_IO_BASE = 0x040000;
static constexpr uint32_t BUS_IO_SIZE = 0x010000;

class RtcFixture : public ::testing::Test {
protected:
    Bus bus;
    ClockControl clk;
    InterruptController intc;
    ClockTimer rtc;

    RtcFixture() : bus(BUS_IO_BASE, BUS_IO_SIZE) {
        clk.attach(bus);
        intc.attach(bus, [](int, int) {});
        rtc.attach(bus, intc, clk);

        // Default: CLKCTL_T16_0 not set, so cpu_clock_hz() = 48 MHz.
        // half_period = 48_000_000 / 2 = 24_000_000 cycles.
    }
};

// ---------------------------------------------------------------------------
// OSC3 running flag (rRTCSEL bit 5) must always be 1 at startup
// ---------------------------------------------------------------------------
TEST_F(RtcFixture, OscRunning_Bit5_AlwaysSet) {
    EXPECT_NE(0, rtc.rtcsel() & 0x20) << "bit5 (OSC3 on) must be set at startup";
}

// ---------------------------------------------------------------------------
// 1 Hz clock flag (rRTCSEL bit 3) starts low
// ---------------------------------------------------------------------------
TEST_F(RtcFixture, ClkFlag_InitiallyLow) {
    EXPECT_EQ(0, rtc.rtcsel() & 0x08) << "bit3 (1Hz flag) must be 0 initially";
}

// ---------------------------------------------------------------------------
// 1 Hz flag toggles at the first half-period boundary
// ---------------------------------------------------------------------------
TEST_F(RtcFixture, ClkFlag_Toggles_At_HalfPeriod) {
    // next_wake_cycle() returns the first toggle point.
    uint64_t wake = rtc.next_wake_cycle();
    EXPECT_NE(UINT64_MAX, wake);

    // One cycle before: flag still low
    rtc.tick(wake - 1);
    EXPECT_EQ(0, rtc.rtcsel() & 0x08) << "flag must still be 0 before half-period";

    // At the boundary: flag goes high
    rtc.tick(wake);
    EXPECT_NE(0, rtc.rtcsel() & 0x08) << "flag must be 1 after first half-period";
}

// ---------------------------------------------------------------------------
// 1 Hz flag toggles back on the second half-period
// ---------------------------------------------------------------------------
TEST_F(RtcFixture, ClkFlag_Toggles_Back_Second_HalfPeriod) {
    uint64_t wake1 = rtc.next_wake_cycle();
    rtc.tick(wake1);
    EXPECT_NE(0, rtc.rtcsel() & 0x08) << "flag high after first toggle";

    uint64_t wake2 = rtc.next_wake_cycle();
    EXPECT_GT(wake2, wake1) << "second toggle must be later than first";
    rtc.tick(wake2);
    EXPECT_EQ(0, rtc.rtcsel() & 0x08) << "flag back low after second toggle";
}

// ---------------------------------------------------------------------------
// GetSysClock pattern: two full low→high transitions are observable
//
// The kernel's GetSysClock() does:
//   while (bit3);       wait for low
//   while (!bit3);      wait for high  ← first rising edge
//   while (bit3);       wait for low
//   while (!bit3);      wait for high  ← second rising edge
// Fast-forwarding through two complete toggle pairs must show all four phases.
// ---------------------------------------------------------------------------
TEST_F(RtcFixture, GetSysClock_TwoRisingEdgesObservable) {
    // Phase 0: flag is already low (initial state), first "wait for low" exits
    EXPECT_EQ(0, rtc.rtcsel() & 0x08);

    // Rising edge 1
    rtc.tick(rtc.next_wake_cycle());
    EXPECT_NE(0, rtc.rtcsel() & 0x08) << "first rising edge";

    // Falling edge
    rtc.tick(rtc.next_wake_cycle());
    EXPECT_EQ(0, rtc.rtcsel() & 0x08) << "falling edge (second wait-for-low exits)";

    // Rising edge 2
    rtc.tick(rtc.next_wake_cycle());
    EXPECT_NE(0, rtc.rtcsel() & 0x08) << "second rising edge";
}

// ---------------------------------------------------------------------------
// rRTCSEL readable via bus at 0x040153 (hi byte of halfword at 0x040152)
// ---------------------------------------------------------------------------
TEST_F(RtcFixture, RtcSel_ReadableViaBus) {
    // Advance to first toggle so bit3 becomes 1
    rtc.tick(rtc.next_wake_cycle());

    // Read halfword at 0x040152: hi byte = rRTCSEL
    uint16_t val = bus.read16(0x040152);
    uint8_t  sel = static_cast<uint8_t>(val >> 8);
    EXPECT_NE(0, sel & 0x08) << "rRTCSEL bit3 must be visible via bus";
    EXPECT_NE(0, sel & 0x20) << "rRTCSEL bit5 (OSC3 on) must be visible via bus";
}

// ---------------------------------------------------------------------------
// next_wake_cycle advances monotonically across multiple toggles
// ---------------------------------------------------------------------------
TEST_F(RtcFixture, NextWakeCycle_MonotonicallyIncreasing) {
    uint64_t prev = 0;
    for (int i = 0; i < 6; i++) {
        uint64_t wake = rtc.next_wake_cycle();
        EXPECT_GT(wake, prev) << "next_wake_cycle() must strictly increase";
        rtc.tick(wake);
        prev = wake;
    }
}

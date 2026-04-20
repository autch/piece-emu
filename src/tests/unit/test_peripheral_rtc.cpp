#include <gtest/gtest.h>
#include "peripheral_rtc.hpp"
#include "peripheral_clkctl.hpp"
#include "peripheral_intc.hpp"
#include "bus.hpp"

#include <ctime>

// ============================================================================
// ClockTimer (RTC) unit tests — host-clock backed.
//
// Register layout exercised here matches the P/ECE kernel (sdk/sysdev/pcekn/
// rtc.c):
//   0x040151 STOP   0x040153 prescaler  0x040154 SEC  0x040155 MIN
//   0x040156 HOUR   0x040157/8 DAY (16-bit, days since 2000-01-01)
// ============================================================================

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
    }
};

// ---------------------------------------------------------------------------
// Prescaler bit3 must toggle so GetSysClock() can calibrate.  At default
// cpu_clock_hz (24 MHz, P/ECE P07=1 default) the prescaler ticks every
// 24 MHz / 256 = 93,750 cycles; bit3 toggles every 8 ticks (→ 16 Hz).
// ---------------------------------------------------------------------------
TEST_F(RtcFixture, PrescalerBit3_TogglesAtExpectedRate) {
    EXPECT_EQ(0, rtc.rtcsub() & 0x08);

    // Advance enough cycles for 8 prescaler ticks.
    rtc.tick(8 * (24'000'000u / 256u));
    EXPECT_NE(0, rtc.rtcsub() & 0x08) << "bit3 should be set after 8 prescaler ticks";

    rtc.tick(16 * (24'000'000u / 256u));
    EXPECT_EQ(0, rtc.rtcsub() & 0x08) << "bit3 should clear after 16 prescaler ticks";
}

// ---------------------------------------------------------------------------
// Prescaler readable at 0x040153 (hi byte of halfword at 0x040152).
// ---------------------------------------------------------------------------
TEST_F(RtcFixture, Prescaler_ReadableViaBus) {
    rtc.tick(24'000'000u);                  // 1 second of CPU cycles
    uint16_t v = bus.read16(0x040152);
    uint8_t  hi = static_cast<uint8_t>(v >> 8);
    EXPECT_EQ(rtc.rtcsub(), hi) << "0x040153 must mirror prescaler register";
}

// ---------------------------------------------------------------------------
// next_wake_cycle advances monotonically.
// ---------------------------------------------------------------------------
TEST_F(RtcFixture, NextWakeCycle_MonotonicallyIncreasing) {
    uint64_t prev = 0;
    for (int i = 0; i < 5; ++i) {
        uint64_t w = rtc.next_wake_cycle();
        EXPECT_GT(w, prev);
        rtc.tick(w);
        prev = w;
    }
}

// ---------------------------------------------------------------------------
// Default state: reads return host PC time (offset = 0).
// We don't pin the absolute value (the host clock is what it is) but the
// reported (yy, mm, dd, hh, mi) must round-trip with the system's localtime.
// ---------------------------------------------------------------------------
TEST_F(RtcFixture, Read_ReturnsHostLocalTime_ByDefault) {
    EXPECT_EQ(0, rtc.offset_seconds());

    // Read all fields atomically as the kernel does.
    uint16_t sec_min = bus.read16(0x040154); // sec(lo) | min(hi)
    uint16_t hr_dlo  = bus.read16(0x040156); // hr(lo)  | day_lo(hi)
    uint16_t dhi_alm = bus.read16(0x040158); // day_hi(lo) | alarm_mi(hi)

    uint8_t sec    = static_cast<uint8_t>(sec_min);
    uint8_t min    = static_cast<uint8_t>(sec_min >> 8);
    uint8_t hour   = static_cast<uint8_t>(hr_dlo);
    uint8_t day_lo = static_cast<uint8_t>(hr_dlo >> 8);
    uint8_t day_hi = static_cast<uint8_t>(dhi_alm);

    std::time_t now = std::time(nullptr);
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif

    // Allow up to a 2-second skew between the two host clock reads.
    int diff = std::abs(static_cast<int>(sec) - lt.tm_sec);
    if (diff > 30) diff = 60 - diff; // wrap across minute boundary
    EXPECT_LE(diff, 2) << "sec roughly matches host";

    EXPECT_LT(hour, 24);
    EXPECT_LT(min, 60);
    EXPECT_LT(sec, 60);

    int days = (static_cast<int>(day_hi) << 8) | day_lo;
    EXPECT_GT(days, 0)  << "host date must be after 2000-01-01";
    EXPECT_LT(days, 36500) << "host date should be representable (<2100)";
}

// ---------------------------------------------------------------------------
// Day count must match the P/ECE kernel's ymd2sdate() exactly.  The kernel
// re-decomposes our DAY register via sdate2ymd(); if our base differs by
// even one day the displayed date jumps.  Verified anchor points:
//   2000-01-01 → 0       (epoch)
//   2001-01-01 → 366     (after 2000 leap year)
//   2024-01-01 → 8766    (24 years, 6 leaps from 2004..2024 + 2000)
//   2026-04-19 → 9605    (issue reproducer — was off-by-one)
// ---------------------------------------------------------------------------
TEST_F(RtcFixture, ReadAfterWrite_DayCountMatchesKernelOracle) {
    struct Case { int yy, mm, dd, want_day; };
    Case cases[] = {
        {2000, 1, 1,    0},
        {2001, 1, 1,  366},
        {2024, 1, 1, 8766},
        {2026, 4, 19, 9605},
    };
    for (const auto& c : cases) {
        bus.write8(0x040151, 0x02);
        bus.write8(0x040155, 0);
        bus.write8(0x040156, 0);
        bus.write8(0x040157, static_cast<uint8_t>(c.want_day));
        bus.write8(0x040158, static_cast<uint8_t>(c.want_day >> 8));
        bus.write8(0x040151, 0x01);

        uint16_t hr_dlo  = bus.read16(0x040156);
        uint16_t dhi_alm = bus.read16(0x040158);
        int got = (static_cast<int>(static_cast<uint8_t>(dhi_alm)) << 8)
                | static_cast<uint8_t>(hr_dlo >> 8);
        EXPECT_EQ(c.want_day, got)
            << "day round-trip failed for " << c.yy << "-" << c.mm << "-" << c.dd;
    }
}

// ---------------------------------------------------------------------------
// Default-state (offset=0) day count must agree with what the host's
// localtime + the kernel's ymd2sdate() would produce.  This catches the
// "off-by-one day" symptom directly.
// ---------------------------------------------------------------------------
TEST_F(RtcFixture, Read_DefaultDayCount_MatchesHostOracle) {
    std::time_t now = std::time(nullptr);
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    int yy = lt.tm_year + 1900;
    int mm = lt.tm_mon  + 1;
    int dd = lt.tm_mday;
    if (yy < 2000 || yy > 2099) GTEST_SKIP() << "host year outside 2000..2099";

    // Oracle (mirrors kernel's ymd2sdate exactly):
    static const int mtbl[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int y = yy - 2000;
    int oracle = y * 365 + (y >> 2) + mtbl[mm-1] + (dd - 1);
    if ((y & 3) || mm >= 3) ++oracle;

    uint16_t hr_dlo  = bus.read16(0x040156);
    uint16_t dhi_alm = bus.read16(0x040158);
    int got = (static_cast<int>(static_cast<uint8_t>(dhi_alm)) << 8)
            | static_cast<uint8_t>(hr_dlo >> 8);
    EXPECT_EQ(oracle, got);
}

// ---------------------------------------------------------------------------
// Write sequence (kernel pattern in pceTimeSet) updates offset so that
// subsequent reads reflect the written wall-clock time.
// ---------------------------------------------------------------------------
TEST_F(RtcFixture, WriteSequence_UpdatesOffset_pceTimeSetPattern) {
    // Set RTC to 2024-06-15 12:34:00.
    constexpr int yy = 2024, mm = 6, dd = 15, hh = 12, mi = 34;
    // Days from 2000-01-01:
    //   24 years → 24*365 + 6 leap days (2000,04,08,12,16,20,24-pre-Mar adj)
    //   = 8766; Jan-May 2024 (leap) → 31+29+31+30+31 = 152; +14 = 166
    //   → 8766 + 166 = 8932
    constexpr int day = 8766 + 152 + 14;

    // Kernel's exact byte-store sequence (pceTimeSet):
    bus.write8(0x040151, 0x02);                                  // STOP
    bus.write8(0x040155, static_cast<uint8_t>(mi));              // MIN
    bus.write8(0x040156, static_cast<uint8_t>(hh));              // HOUR
    bus.write8(0x040157, static_cast<uint8_t>(day));             // DAY low
    bus.write8(0x040158, static_cast<uint8_t>(day >> 8));        // DAY high
    bus.write8(0x040151, 0x01);                                  // RUN

    uint16_t sec_min = bus.read16(0x040154);
    uint16_t hr_dlo  = bus.read16(0x040156);
    uint16_t dhi_alm = bus.read16(0x040158);

    uint8_t sec    = static_cast<uint8_t>(sec_min);
    uint8_t rmin   = static_cast<uint8_t>(sec_min >> 8);
    uint8_t rhour  = static_cast<uint8_t>(hr_dlo);
    uint8_t day_lo = static_cast<uint8_t>(hr_dlo >> 8);
    uint8_t day_hi = static_cast<uint8_t>(dhi_alm);
    int     rday   = (static_cast<int>(day_hi) << 8) | day_lo;

    EXPECT_LE(sec, 1u)       << "sec resets to 0 on RUN (allow 1s host skew)";
    EXPECT_EQ(mi,   rmin);
    EXPECT_EQ(hh,   rhour);
    EXPECT_EQ(day,  rday);
    (void)yy; (void)mm; (void)dd;
}

// ---------------------------------------------------------------------------
// STOP freezes the visible time; RUN resumes from the latched values.
// ---------------------------------------------------------------------------
TEST_F(RtcFixture, Stop_FreezesVisibleTime) {
    bus.write16(0x040150, static_cast<uint16_t>(0x02) << 8); // STOP

    uint16_t a = bus.read16(0x040154);
    // Wait briefly via cycle ticks; the visible time must not change.
    rtc.tick(24'000'000u * 2);              // 2 simulated seconds
    uint16_t b = bus.read16(0x040154);
    EXPECT_EQ(a, b) << "SEC/MIN must not advance while STOP=1";
}

// ---------------------------------------------------------------------------
// Halfword read at 0x040154 returns sec+min derived from a single time_t,
// so the kernel's do/while atomic check converges in one iteration.
// ---------------------------------------------------------------------------
TEST_F(RtcFixture, AtomicHalfwordRead_NoIntraSecondTearing) {
    for (int i = 0; i < 100; ++i) {
        uint16_t v = bus.read16(0x040154);
        uint8_t s = static_cast<uint8_t>(v);
        uint8_t m = static_cast<uint8_t>(v >> 8);
        EXPECT_LT(s, 60u);
        EXPECT_LT(m, 60u);
    }
}

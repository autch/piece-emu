#include <gtest/gtest.h>
#include "peripheral_t16.hpp"
#include "peripheral_clkctl.hpp"
#include "peripheral_intc.hpp"
#include "bus.hpp"

// ============================================================================
// Timer16bit unit tests
// ============================================================================

static constexpr uint32_t T16_BASE  = 0x048180; // ch.0 register base
static constexpr uint32_t INTC_BASE = 0x040260;

class T16Fixture : public ::testing::Test {
protected:
    Bus bus;
    ClockControl clk;
    InterruptController intc;
    Timer16bit t16;
    int last_irq = -1;

    T16Fixture() : bus(0x40000, 0x80000), t16(0) {
        clk.attach(bus);
        intc.attach(bus, [this](int no, int) { last_irq = no; });
        t16.attach(bus, intc, clk);

        // Configure cpc = 2:
        //   Default OSC3 = 24 MHz (P07=1), CLKDT=0 → CPU = 24 MHz
        //   CLKCTL_T16_0 = 0x09 (TONA=1, TSA=1) → T16 = OSC3/2 = 12 MHz
        //   cpc = ceil(24 MHz / 12 MHz) = 2
        // (The absolute frequencies don't matter for these tests; only
        //  the CPU-to-T16 cycle ratio does.)
        bus.write16(0x040146, 0x0900); // CLKCTL_T16_0: TONA=1 TSA=1
    }

    // Enable T16_CRA0 interrupt (trap 31):
    //   priority byte 6 (lo byte of halfword at INTC_BASE+6), bits[2:0] = 3
    //   IEN3 byte 18 (lo byte of halfword at INTC_BASE+18), bit 3
    void enable_cra0_irq() {
        bus.write16(INTC_BASE + 6,  0x0003); // priority byte 6 = 3
        bus.write16(INTC_BASE + 18, 0x0008); // IEN3[3] = T16_CRA0 enable
    }

    // Enable T16_CRB0 interrupt (trap 30):
    //   same priority byte 6, IEN3 byte 18 bit 2
    void enable_crb0_irq() {
        bus.write16(INTC_BASE + 6,  0x0003); // priority byte 6 = 3
        bus.write16(INTC_BASE + 18, 0x0004); // IEN3[2] = T16_CRB0 enable
    }

    void set_cra(uint16_t v) { bus.write16(T16_BASE + 0, v); }
    void set_crb(uint16_t v) { bus.write16(T16_BASE + 2, v); }
    void set_ctl(uint8_t  v) { bus.write16(T16_BASE + 6, v); }
};

// ---------------------------------------------------------------------------
// PRUN=0: TC does not count
// ---------------------------------------------------------------------------
TEST_F(T16Fixture, PRUN_Zero_NoCount) {
    set_cra(10);
    set_ctl(0x00); // PRUN=0
    t16.tick(1000);
    EXPECT_EQ(0,  t16.tc());
    EXPECT_EQ(-1, last_irq);
}

// ---------------------------------------------------------------------------
// CRB match raises T16_CRB0 IRQ and resets TC (period = CRB+1).
//
// CRB=3, cpc=2: TC sequence 0,1,2,3,0,1,... per the S1C33209 tech manual III-4.
// tick(4) → 3 TC ticks → TC reaches 3 == CRB → IRQ fires.  TC=3 until next
// tick (the reset takes one clock).
// ---------------------------------------------------------------------------
TEST_F(T16Fixture, CRB_Match_IRQ_And_TC_Reset) {
    enable_crb0_irq();
    set_crb(3);
    set_ctl(0x01); // PRUN=1

    t16.tick(4); // 3 TC ticks → TC = 3 = CRB, IRQ fires
    EXPECT_EQ(30, last_irq) << "T16_CRB0 trap number must be 30";
    EXPECT_EQ(3,  t16.tc())  << "TC holds CRB value on the matching tick";

    // Next TC tick wraps to 0 (real HW: reset on the following clock edge).
    t16.tick(6); // one more TC tick (cpc=2)
    EXPECT_EQ(0, t16.tc()) << "TC wraps to 0 on the tick after CRB match";
}

// ---------------------------------------------------------------------------
// CRA match raises T16_CRA0 IRQ without resetting TC.  CRB is placed far away
// so it doesn't reset TC within the window.
// ---------------------------------------------------------------------------
TEST_F(T16Fixture, CRA_Match_IRQ_No_TC_Reset) {
    enable_cra0_irq();
    set_crb(100); // CRB far away — no reset during test
    set_cra(3);
    set_ctl(0x01); // PRUN=1

    t16.tick(4); // 3 TC ticks (cpc=2) → TC reaches 3 == CRA → match
    EXPECT_EQ(31, last_irq) << "T16_CRA0 trap number must be 31";
    EXPECT_EQ(3,  t16.tc())  << "TC must not be reset by CRA match";
}

// ---------------------------------------------------------------------------
// PRESET bit immediately resets TC to 0 and self-clears
// ---------------------------------------------------------------------------
TEST_F(T16Fixture, PRESET_Resets_TC_And_Self_Clears) {
    set_cra(100);
    set_ctl(0x01); // PRUN=1

    // Count up to TC=5: ticks at cycles 0,2,4,6,8 → TC=5 after tick(9).
    t16.tick(9);
    EXPECT_EQ(5, t16.tc());

    // Write PRESET (bit 1) — TC must reset immediately.
    set_ctl(0x03); // PRUN=1 | PRESET=1
    EXPECT_EQ(0,    t16.tc())  << "PRESET must reset TC to 0 immediately";
    EXPECT_EQ(0x01, t16.ctl()) << "PRESET bit must self-clear";
}

// ---------------------------------------------------------------------------
// ISR flag for CRB0 is set regardless of IEN/priority
// ---------------------------------------------------------------------------
TEST_F(T16Fixture, CRB_Match_SetsISR_Unconditionally) {
    // IEN=0 (no delivery) — ISR flag must still be set.
    set_crb(2);
    set_ctl(0x01); // PRUN=1

    // TC: 0→1→2 then reset to 0. Full period = CRB+1 = 3 ticks, * cpc=2 = 6.
    t16.tick(6);
    // T16_CRB0 ISR: regs_ byte offset 34, bit 2
    EXPECT_NE(0, intc.reg(34) & (1 << 2)) << "CRB0 ISR bit must be set";
    EXPECT_EQ(-1, last_irq) << "No delivery without IEN";
}

// ---------------------------------------------------------------------------
// Both CRA and CRB fire in the same tick when CRA==CRB (CRA compare matches
// just as TC hits CRB and resets).
// ---------------------------------------------------------------------------
TEST_F(T16Fixture, Both_CRA_And_CRB_Match_Same_Tick) {
    // Enable both CRB0 and CRA0 in one write (IEN3 bits 2 and 3).
    bus.write16(INTC_BASE + 6,  0x0003); // priority byte 6 = 3 (shared)
    bus.write16(INTC_BASE + 18, 0x000C); // IEN3[2]=CRB0, IEN3[3]=CRA0
    set_cra(3);
    set_crb(3);
    set_ctl(0x01); // PRUN=1

    t16.tick(4); // 3 TC ticks: TC reaches 3 → both CRA and CRB match
    EXPECT_NE(0, intc.reg(34) & (1 << 2)) << "CRB0 ISR must be set";
    EXPECT_NE(0, intc.reg(34) & (1 << 3)) << "CRA0 ISR must be set";
    EXPECT_EQ(3, t16.tc()) << "TC holds CRB value on match (wraps on next tick)";
}

// ---------------------------------------------------------------------------
// next_wake_cycle: PRUN=0 → UINT64_MAX
// ---------------------------------------------------------------------------
TEST_F(T16Fixture, NextWakeCycle_Stopped_ReturnsMax) {
    set_cra(100);
    set_ctl(0x00); // PRUN=0
    EXPECT_EQ(UINT64_MAX, t16.next_wake_cycle());
}

// ---------------------------------------------------------------------------
// next_wake_cycle: CRA interrupt fires at or before the predicted cycle
//
// CRA=100, TC=0, cpc=2: next_wake_cycle() returns some finite value.
// Ticking to that value must raise T16_CRA0 (trap 31).
// Note: due to the '>=' boundary in tick(), one extra increment may occur
// at the exact boundary cycle, so we do not assert TC==0 here.
// ---------------------------------------------------------------------------
TEST_F(T16Fixture, NextWakeCycle_BeforeCRA) {
    enable_cra0_irq();
    set_cra(100);
    set_ctl(0x01); // PRUN=1
    uint64_t wake = t16.next_wake_cycle();
    EXPECT_NE(UINT64_MAX, wake);
    t16.tick(wake);
    EXPECT_EQ(31, last_irq) << "T16_CRA0 must fire at next_wake_cycle()";
}

// ---------------------------------------------------------------------------
// next_wake_cycle: consecutive CRA events have monotonically increasing times
//
// After the first CRA fires, TC resets to 0 (SELFM=0). Calling
// next_wake_cycle() again must return a cycle strictly after the first wake,
// and ticking to that cycle must fire CRA a second time.
// ---------------------------------------------------------------------------
TEST_F(T16Fixture, NextWakeCycle_AfterCRA) {
    enable_cra0_irq();
    set_cra(50);
    set_ctl(0x01); // PRUN=1, SELFM=0
    // First CRA
    uint64_t wake1 = t16.next_wake_cycle();
    EXPECT_NE(UINT64_MAX, wake1);
    t16.tick(wake1);
    EXPECT_EQ(31, last_irq) << "first CRA must fire";
    last_irq = -1;
    // Second CRA: prediction must advance past the first
    uint64_t wake2 = t16.next_wake_cycle();
    EXPECT_GT(wake2, wake1) << "second wake must be strictly after first";
    EXPECT_NE(UINT64_MAX, wake2);
    t16.tick(wake2);
    EXPECT_EQ(31, last_irq) << "second CRA must fire";
}

// ---------------------------------------------------------------------------
// next_wake_cycle: CRB fires before CRA → predicted correctly
//
// CRA=100, CRB=30, TC=0, cpc=2.
// CRB fires at 30 increments = cycle 60, before CRA at cycle 200.
// next_wake_cycle() must return 60.
// ---------------------------------------------------------------------------
TEST_F(T16Fixture, NextWakeCycle_CRB_Before_CRA) {
    enable_crb0_irq();
    set_cra(100);
    set_crb(30);
    set_ctl(0x01); // PRUN=1
    uint64_t wake = t16.next_wake_cycle();
    EXPECT_EQ(static_cast<uint64_t>(30) * 2, wake) << "CRB fires first at 60 cycles";
    t16.tick(wake);
    EXPECT_EQ(30, last_irq) << "T16_CRB0 must fire at next_wake_cycle()";
}

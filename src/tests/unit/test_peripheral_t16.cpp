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

        // CLKCTL_T16_0 = 0x08: TONA=1, TSA=0 → timer clock = 48 MHz / 2 = 24 MHz.
        // CLKCTL_T16_0 is the hi byte of the halfword at 0x040146.
        // → cycles_per_count = ceil(48 MHz / 24 MHz) = 2
        bus.write16(0x040146, 0x0800);
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
// CRA match raises T16_CRA0 IRQ and resets TC (SELFM=0)
//
// CRA=3, cpc=2: TC increments at cycles 0,2,4 → TC goes 0→1→2→3.
// On TC==CRA at cycle 4: IRQ raised, TC reset to 0.
// ---------------------------------------------------------------------------
TEST_F(T16Fixture, CRA_Match_IRQ_And_TC_Reset) {
    enable_cra0_irq();
    set_cra(3);
    set_ctl(0x01); // PRUN=1, SELFM=0 (default)

    t16.tick(4); // TC reaches 3 == CRA → T16_CRA0, TC=0
    EXPECT_EQ(31, last_irq) << "T16_CRA0 trap number must be 31";
    EXPECT_EQ(0,  t16.tc())  << "TC must be reset to 0 on CRA match (SELFM=0)";
}

// ---------------------------------------------------------------------------
// CRB match raises T16_CRB0 IRQ without resetting TC
// ---------------------------------------------------------------------------
TEST_F(T16Fixture, CRB_Match_IRQ_No_TC_Reset) {
    enable_crb0_irq();
    set_cra(100); // CRA far away — no CRA match during test
    set_crb(3);
    set_ctl(0x01); // PRUN=1

    t16.tick(4); // TC reaches 3 == CRB → T16_CRB0
    EXPECT_EQ(30, last_irq) << "T16_CRB0 trap number must be 30";
    EXPECT_EQ(3,  t16.tc())  << "TC must not be reset by CRB match";
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
// ISR flag for CRA0 is set regardless of IEN/priority
// ---------------------------------------------------------------------------
TEST_F(T16Fixture, CRA_Match_SetsISR_Unconditionally) {
    // IEN=0 (no delivery) — ISR flag must still be set.
    set_cra(2);
    set_ctl(0x01); // PRUN=1

    // TC: 0→1→2 (CRA match at cycle 2).
    t16.tick(2);
    // T16_CRA0 ISR: regs_ byte offset 34, bit 3
    EXPECT_NE(0, intc.reg(34) & (1 << 3)) << "CRA0 ISR bit must be set";
    EXPECT_EQ(-1, last_irq) << "No delivery without IEN";
}

// ---------------------------------------------------------------------------
// CRB match fires before CRA in the same tick (both set on TC==CRB==CRA)
// ---------------------------------------------------------------------------
TEST_F(T16Fixture, Both_CRA_And_CRB_Match_Same_Tick) {
    // When CRA==CRB the implementation checks CRB first, then CRA.
    // Enable both CRB0 and CRA0 in one write (IEN3 bits 2 and 3) to avoid
    // the second write overwriting the first.
    bus.write16(INTC_BASE + 6,  0x0003); // priority byte 6 = 3 (shared)
    bus.write16(INTC_BASE + 18, 0x000C); // IEN3[2]=CRB0, IEN3[3]=CRA0
    set_cra(3);
    set_crb(3);
    set_ctl(0x01); // PRUN=1, SELFM=0

    t16.tick(4); // TC==3 triggers CRB, then CRA (TC reset to 0 by CRA)
    // ISR for CRB0 (bit 2 of offset 34) and CRA0 (bit 3 of offset 34) both set
    EXPECT_NE(0, intc.reg(34) & (1 << 2)) << "CRB0 ISR must be set";
    EXPECT_NE(0, intc.reg(34) & (1 << 3)) << "CRA0 ISR must be set";
    EXPECT_EQ(0, t16.tc()) << "TC reset by CRA match";
}

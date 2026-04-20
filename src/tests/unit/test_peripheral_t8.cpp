#include <gtest/gtest.h>
#include "peripheral_t8.hpp"
#include "peripheral_clkctl.hpp"
#include "peripheral_intc.hpp"
#include "bus.hpp"

// ============================================================================
// Timer8bit unit tests
// ============================================================================

static constexpr uint32_t T8_BASE   = 0x040160; // ch.0 register base
static constexpr uint32_t INTC_BASE = 0x040260;

class T8Fixture : public ::testing::Test {
protected:
    Bus bus;
    ClockControl clk;
    InterruptController intc;
    Timer8bit t8;
    int last_irq = -1;

    T8Fixture() : bus(0x40000, 0x80000), t8(0) {
        clk.attach(bus);
        intc.attach(bus, [this](int no, int) { last_irq = no; });
        t8.attach(bus, intc, clk);

        // Configure cpc = 2:
        //   Default OSC3 = 24 MHz (P07=1), CLKDT=0 → CPU = 24 MHz
        //   CLKCTL_T8_01 = 0x09 (TONA=1, TSA=1) → T8 = OSC3/2 = 12 MHz
        //   cpc = ceil(24 MHz / 12 MHz) = 2
        bus.write16(0x04014C, 0x0900); // CLKCTL_T8_01: TONA=1 TSA=1
    }

    // Set RLD then CTL so that PSET (if included) uses the new RLD value.
    void configure(uint8_t ctl, uint8_t rld) {
        // Step 1: write RLD only (CTL=0) so rld_ is set before on_ctl_write.
        bus.write16(T8_BASE, static_cast<uint16_t>(rld) << 8);
        // Step 2: write desired CTL (may trigger PSET with correct rld_).
        bus.write16(T8_BASE,
                    static_cast<uint16_t>(ctl) |
                    (static_cast<uint16_t>(rld) << 8));
    }

    // Enable T8_UF0 interrupt delivery.
    // - Priority register offset 9 (rP8TM_PSIO0) bits[2:0] = 3
    //   → hi byte of halfword at INTC_BASE + 8
    // - IEN6 offset 21 bit 0 (T8_UF0)
    //   → hi byte of halfword at INTC_BASE + 20
    void enable_irq() {
        bus.write16(INTC_BASE + 8,  0x0300); // priority = 3
        bus.write16(INTC_BASE + 20, 0x0100); // IEN6[0] = T8_UF0 enable
    }
};

// ---------------------------------------------------------------------------
// PTRUN=0: counter is frozen — no counting, no interrupt
// ---------------------------------------------------------------------------
TEST_F(T8Fixture, PTRUN_Zero_NoCount) {
    configure(0x02, 5); // PSET=1, PTRUN=0 → PTD preset to 5
    t8.tick(1000);
    EXPECT_EQ(5, t8.ptd()) << "Counter must not advance when PTRUN=0";
    EXPECT_EQ(-1, last_irq);
}

// ---------------------------------------------------------------------------
// PSET bit immediately presets the counter to RLD
// ---------------------------------------------------------------------------
TEST_F(T8Fixture, PSET_Presets_Counter) {
    configure(0x02, 7); // PSET=1, PTRUN=0, RLD=7
    EXPECT_EQ(7, t8.rld());
    EXPECT_EQ(7, t8.ptd()) << "PSET should preset PTD = RLD immediately";
}

// ---------------------------------------------------------------------------
// Clock stopped (TONA=0): no counting even with PTRUN=1
// ---------------------------------------------------------------------------
TEST_F(T8Fixture, ClockStopped_NoTick) {
    bus.write16(0x04014C, 0x0000); // CLKCTL_T8_01 = 0x00 (TONA=0)
    configure(0x03, 5);            // PTRUN=1, PSET=1, PTD=5
    t8.tick(10000);
    EXPECT_EQ(5, t8.ptd()) << "Counter must not change when clock is stopped";
    EXPECT_EQ(-1, last_irq);
}

// ---------------------------------------------------------------------------
// Underflow fires interrupt and auto-reloads from RLD
//
// Setup: RLD=3, PTD=3, cpc=2.
// Ticks at CPU cycles 0,2,4 decrement PTD: 3→2→1→0.
// Tick at cycle 6 sees PTD==0 → underflow: PTD=3, T8_UF0 IRQ.
// ---------------------------------------------------------------------------
TEST_F(T8Fixture, Underflow_FiresIRQ_And_Reloads) {
    enable_irq();
    configure(0x03, 3); // PTRUN=1, PSET=1, RLD=3, PTD=3

    // Decrement phase: ticks at 0,2,4 bring PTD from 3 down to 0.
    t8.tick(4);
    EXPECT_EQ(0,  t8.ptd());
    EXPECT_EQ(-1, last_irq) << "No IRQ before underflow tick";

    // Underflow tick at cycle 6.
    t8.tick(6);
    EXPECT_EQ(52, last_irq) << "T8_UF0 trap number must be 52";
    EXPECT_EQ(3,  t8.ptd()) << "Counter reloaded from RLD after underflow";
}

// ---------------------------------------------------------------------------
// next_wake_cycle: PTRUN=0 → UINT64_MAX
// ---------------------------------------------------------------------------
TEST_F(T8Fixture, NextWakeCycle_Stopped_ReturnsMax) {
    configure(0x02, 5); // PSET=1, PTRUN=0
    EXPECT_EQ(UINT64_MAX, t8.next_wake_cycle());
}

// ---------------------------------------------------------------------------
// next_wake_cycle: accurate prediction of underflow cycle
//
// Setup: RLD=3, PTD=3 (after PSET), cpc=2.
// next_tick_cycle_ starts at 0.  Underflow fires when ptd_ hits 0 then
// gets incremented again: after 3 decrements at cycles 0,2,4 ptd_=0,
// then the underflow tick fires at cycle 6 = next_tick_cycle_=6.
// With ptd_=3 and cpc=2: next_wake_cycle = 0 + 3*2 = 6.
// ---------------------------------------------------------------------------
TEST_F(T8Fixture, NextWakeCycle_Running_Accurate) {
    configure(0x03, 3); // PTRUN=1, PSET=1, RLD=3, PTD=3
    uint64_t wake = t8.next_wake_cycle();
    EXPECT_NE(UINT64_MAX, wake) << "Timer is running";
    // Confirm: ticking exactly to that cycle fires the underflow
    enable_irq();
    t8.tick(wake);
    EXPECT_EQ(52, last_irq) << "T8_UF0 must fire exactly at next_wake_cycle()";
}

// ---------------------------------------------------------------------------
// ISR flag is set by underflow regardless of IEN/priority
// ---------------------------------------------------------------------------
TEST_F(T8Fixture, Underflow_SetsISR_Unconditionally) {
    // IEN=0 (no delivery) — ISR flag must still be set.
    configure(0x03, 2); // PTD=2, RLD=2

    // ticks at 0,2 → PTD: 2→1→0; tick at 4 → underflow (ISR set, no delivery).
    t8.tick(4);
    // T8_UF0 ISR: regs_ byte offset 37, bit 0
    EXPECT_NE(0, intc.reg(37) & 0x01) << "ISR bit must be set by underflow";
    EXPECT_EQ(-1, last_irq) << "No interrupt delivery without IEN";
}

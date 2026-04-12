#include <gtest/gtest.h>
#include "peripheral_intc.hpp"
#include "bus.hpp"

// ============================================================================
// InterruptController unit tests
// ============================================================================

class IntcFixture : public ::testing::Test {
protected:
    Bus bus;
    InterruptController intc;
    int last_trap_no  = -1;
    int last_trap_lvl = -1;

    IntcFixture() : bus(0x40000, 0x80000) {
        intc.attach(bus, [this](int no, int lv) {
            last_trap_no  = no;
            last_trap_lvl = lv;
        });
    }

    void reset_trap() { last_trap_no = -1; last_trap_lvl = -1; }

    // Write a single byte to an INTC register via the bus (halfword aligned)
    void write_byte(uint32_t addr, uint8_t val) {
        // Read-modify-write to preserve the other byte in the halfword
        uint32_t aligned = addr & ~1u;
        uint16_t cur = bus.read16(aligned);
        if (addr & 1) cur = (cur & 0x00FF) | (static_cast<uint16_t>(val) << 8);
        else          cur = (cur & 0xFF00) | val;
        bus.write16(aligned, cur);
    }
    uint8_t read_byte(uint32_t addr) {
        uint16_t hw = bus.read16(addr & ~1u);
        return (addr & 1) ? static_cast<uint8_t>(hw >> 8)
                          : static_cast<uint8_t>(hw);
    }
};

// INTC base address
static constexpr uint32_t INTC_BASE = 0x040260;

// ---------------------------------------------------------------------------
// ISR flag is set by raise() regardless of IEN/priority
// ---------------------------------------------------------------------------
TEST_F(IntcFixture, ISR_FlagSetByRaise) {
    // T16_CRA0: ISR byte at offset 34, bit 3
    intc.raise(InterruptController::IrqSource::T16_CRA0);
    // ISR register at INTC_BASE + 34
    EXPECT_NE(0, intc.reg(34) & (1 << 3)) << "T16_CRA0 ISR flag should be set";
}

// ---------------------------------------------------------------------------
// No interrupt delivered when IEN bit is clear
// ---------------------------------------------------------------------------
TEST_F(IntcFixture, NoDelivery_IEN_Clear) {
    // Set priority for T16_CRA0 (priority reg offset 6, bits[2:0])
    write_byte(INTC_BASE + 6, 0x05); // P16T0 = 5
    // IEN3 (offset 18) bit 3 = E16TC0 — leave at 0 (disabled)
    intc.raise(InterruptController::IrqSource::T16_CRA0);
    EXPECT_EQ(-1, last_trap_no) << "Should not deliver when IEN bit clear";
}

// ---------------------------------------------------------------------------
// Interrupt delivered when IEN set and priority > 0
// ---------------------------------------------------------------------------
TEST_F(IntcFixture, Delivery_With_IEN_And_Priority) {
    // Set T16_CRA0 priority to 5 (priority reg offset 6, bits[2:0])
    write_byte(INTC_BASE + 6, 0x05);
    // Enable T16_CRA0 in IEN3 (offset 18), bit 3
    write_byte(INTC_BASE + 18, 0x08); // bit 3 = E16TC0
    intc.raise(InterruptController::IrqSource::T16_CRA0);
    EXPECT_EQ(31, last_trap_no)  << "T16_CRA0 trap number should be 31";
    EXPECT_EQ(5,  last_trap_lvl) << "Priority level should be 5";
}

// ---------------------------------------------------------------------------
// Priority 0 = interrupt disabled even if IEN set
// ---------------------------------------------------------------------------
TEST_F(IntcFixture, NoDelivery_Priority_Zero) {
    // Priority for T8_UF0 (reg offset 9, bits[2:0]) = 0
    write_byte(INTC_BASE + 9, 0x00);
    // Enable in IEN6 (offset 21), bit 0
    write_byte(INTC_BASE + 21, 0x01);
    intc.raise(InterruptController::IrqSource::T8_UF0);
    EXPECT_EQ(-1, last_trap_no) << "Priority 0 should prevent delivery";
}

// ---------------------------------------------------------------------------
// T8 underflow channels have correct trap numbers and shared priority
// ---------------------------------------------------------------------------
TEST_F(IntcFixture, T8_UF_TrapNumbers) {
    // Priority = 3 for all 8-bit timers (reg offset 9, bits[2:0])
    write_byte(INTC_BASE + 9, 0x03);
    // Enable all 4 underflow bits in IEN6 (offset 21)
    write_byte(INTC_BASE + 21, 0x0F);

    const int expected_traps[] = {52, 53, 54, 55};
    for (int ch = 0; ch < 4; ch++) {
        reset_trap();
        intc.raise(static_cast<InterruptController::IrqSource>(
            static_cast<int>(InterruptController::IrqSource::T8_UF0) + ch));
        EXPECT_EQ(expected_traps[ch], last_trap_no);
        EXPECT_EQ(3, last_trap_lvl);
    }
}

// ---------------------------------------------------------------------------
// ISR clear in normal mode (RSTONLY=0): write 0 to bit clears it
//
// The kernel uses read-modify-write "*(unsigned char*)0x40282 &= ~mask"
// to clear a specific ISR bit.  RSTONLY=0 means direct write: writing 0
// clears the flag, writing 1 force-sets it.
// ---------------------------------------------------------------------------
TEST_F(IntcFixture, ISR_Clear_NormalMode) {
    // Raise T8_UF0 to set ISR6 bit 0 (ISR byte 37)
    intc.raise(InterruptController::IrqSource::T8_UF0);
    EXPECT_NE(0, intc.reg(37) & 0x01) << "ISR bit should be set";

    // Write 0 to ISR6 bit 0 (read-modify-write pattern used by kernel):
    // ISR6 is the hi byte of halfword at INTC_BASE+36.
    // Reading gives hi byte = 0x01 (bit 0 set).  Clear bit 0 → hi byte = 0x00.
    uint16_t cur = bus.read16(INTC_BASE + 36);
    cur &= ~(1u << 8); // clear bit 0 of ISR6 in the hi byte position
    bus.write16(INTC_BASE + 36, cur);
    EXPECT_EQ(0, intc.reg(37) & 0x01) << "ISR bit should be cleared after write-0";
}

// ---------------------------------------------------------------------------
// ISR clear in RSTONLY mode: write 0 to bit clears it
// ---------------------------------------------------------------------------
TEST_F(IntcFixture, ISR_Clear_RstOnlyMode) {
    // Set RSTONLY bit in rRESET (offset 63, last byte of INTC block)
    // rRESET is at byte 63 = hi byte of the halfword at offset 62.
    bus.write16(INTC_BASE + 62, 0x0100); // hi byte (offset 63) bit 0 = RSTONLY

    // Raise T8_UF0
    intc.raise(InterruptController::IrqSource::T8_UF0);
    EXPECT_NE(0, intc.reg(37) & 0x01);

    // Write 0 to ISR6 byte — clears all set bits
    uint16_t cur = bus.read16(INTC_BASE + 36);
    cur &= 0x00FF; // zero out hi byte (ISR6)
    bus.write16(INTC_BASE + 36, cur);
    EXPECT_EQ(0, intc.reg(37) & 0x01) << "RSTONLY: write-0 should clear ISR";
}

// ---------------------------------------------------------------------------
// Port input trap numbers
// ---------------------------------------------------------------------------
TEST_F(IntcFixture, Port_TrapNumbers) {
    // Enable all port inputs (IEN1 offset 16)
    write_byte(INTC_BASE + 16, 0xFF);

    const struct { InterruptController::IrqSource src; int trap; int pri_byte; int pri_shift; } cases[] = {
        { InterruptController::IrqSource::PORT0, 16, 0, 0 },
        { InterruptController::IrqSource::PORT1, 17, 0, 4 },
        { InterruptController::IrqSource::PORT2, 18, 1, 0 },
        { InterruptController::IrqSource::PORT3, 19, 1, 4 },
    };
    for (auto& c : cases) {
        // Set priority = 2
        uint8_t cur = read_byte(INTC_BASE + c.pri_byte);
        cur = (cur & ~(0x07 << c.pri_shift)) | (2 << c.pri_shift);
        write_byte(INTC_BASE + c.pri_byte, cur);

        reset_trap();
        intc.raise(c.src);
        EXPECT_EQ(c.trap, last_trap_no);
        EXPECT_EQ(2, last_trap_lvl);
    }
}

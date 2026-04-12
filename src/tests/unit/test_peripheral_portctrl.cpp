#include <gtest/gtest.h>
#include "peripheral_portctrl.hpp"
#include "peripheral_intc.hpp"
#include "bus.hpp"

// ============================================================================
// PortCtrl unit tests
// ============================================================================

static constexpr uint32_t KPORT_BASE = 0x0402C0;
static constexpr uint32_t PINT_BASE  = 0x0402C6;
static constexpr uint32_t PPORT_BASE = 0x0402D0;
static constexpr uint32_t INTC_BASE  = 0x040260;

class PortCtrlFixture : public ::testing::Test {
protected:
    Bus bus;
    InterruptController intc;
    PortCtrl port;
    int last_irq = -1;

    PortCtrlFixture() : bus(0x40000, 0x80000) {
        intc.attach(bus, [this](int no, int) { last_irq = no; });
        port.attach(bus, intc);
    }

    // Enable KEY0 interrupt delivery (trap 20):
    //   priority byte 2 (lo byte of halfword at INTC_BASE+2), bits[2:0] = 3
    //   IEN1 byte 16 (lo byte of halfword at INTC_BASE+16), bit 4
    void enable_key0_irq() {
        bus.write16(INTC_BASE + 2,  0x0003); // priority byte 2 = 3
        bus.write16(INTC_BASE + 16, 0x0010); // IEN1[4] = KEY0 enable
    }
};

// ---------------------------------------------------------------------------
// K5D is readable after set_k5()
// ---------------------------------------------------------------------------
TEST_F(PortCtrlFixture, K5D_Readable_Via_Accessor) {
    port.set_k5(0xAB);
    EXPECT_EQ(0xAB, port.k5d());
}

// ---------------------------------------------------------------------------
// K5D is reflected in the hi byte of the bus halfword at KPORT_BASE
// ---------------------------------------------------------------------------
TEST_F(PortCtrlFixture, K5D_Readable_Via_Bus) {
    port.set_k5(0x3C);
    uint16_t hw = bus.read16(KPORT_BASE);
    EXPECT_EQ(0x3C, static_cast<uint8_t>(hw >> 8)) << "K5D in hi byte of 0x0402C0";
}

// ---------------------------------------------------------------------------
// CPU write to the K5D position must not overwrite K5D (input-only)
// ---------------------------------------------------------------------------
TEST_F(PortCtrlFixture, K5D_CPU_Write_Ignored) {
    port.set_k5(0x55);
    // CPU tries to zero the hi byte (K5D position)
    bus.write16(KPORT_BASE, 0x0000);
    EXPECT_EQ(0x55, port.k5d()) << "CPU writes to K5D must be ignored";
}

// ---------------------------------------------------------------------------
// K6D is readable after set_k6()
// ---------------------------------------------------------------------------
TEST_F(PortCtrlFixture, K6D_Readable_Via_Accessor) {
    port.set_k6(0xCD);
    EXPECT_EQ(0xCD, port.k6d());
}

// ---------------------------------------------------------------------------
// K6D is reflected in the lo byte of the bus halfword at KPORT_BASE+4
// ---------------------------------------------------------------------------
TEST_F(PortCtrlFixture, K6D_Readable_Via_Bus) {
    port.set_k6(0x7E);
    uint16_t hw = bus.read16(KPORT_BASE + 4);
    EXPECT_EQ(0x7E, static_cast<uint8_t>(hw)) << "K6D in lo byte of 0x0402C4";
}

// ---------------------------------------------------------------------------
// CPU write to K6D must not overwrite K6D (input-only)
// ---------------------------------------------------------------------------
TEST_F(PortCtrlFixture, K6D_CPU_Write_Ignored) {
    port.set_k6(0xAA);
    bus.write16(KPORT_BASE + 4, 0x0000);
    EXPECT_EQ(0xAA, port.k6d()) << "CPU writes to K6D must be ignored";
}

// ---------------------------------------------------------------------------
// KEY0 interrupt fires when K5D matches SCPK0 (with zero mask)
// ---------------------------------------------------------------------------
TEST_F(PortCtrlFixture, KEY0_Fires_On_K5D_Match) {
    enable_key0_irq();

    // SPPK[1:0]=0 → source = K5D (default, register value 0)
    // SMPK0 = 0x00 (no mask), SCPK0 = 0x01 → match when k5d == 0x01
    // SCPK0 at pint_[6] → offset 6 within PINT block → bus addr PINT_BASE+6
    bus.write16(PINT_BASE + 6, 0x0001); // SCPK0=0x01 (lo), SCPK1=0x00 (hi)
    // SMPK0/SMPK1 at pint_[8..9] → bus addr PINT_BASE+8, default 0x00

    port.set_k5(0x01); // triggers check_key_irq() → KEY0 condition met
    EXPECT_EQ(20, last_irq) << "KEY0 trap number must be 20";
}

// ---------------------------------------------------------------------------
// KEY0 does not fire when K5D does not match SCPK0
// ---------------------------------------------------------------------------
TEST_F(PortCtrlFixture, KEY0_No_Fire_On_Mismatch) {
    enable_key0_irq();
    bus.write16(PINT_BASE + 6, 0x0001); // SCPK0=0x01

    port.set_k5(0x02); // ≠ 0x01 → no match
    EXPECT_EQ(-1, last_irq);
}

// ---------------------------------------------------------------------------
// KEY0 uses mask: masked bits are ignored in comparison
// ---------------------------------------------------------------------------
TEST_F(PortCtrlFixture, KEY0_Mask_Ignores_Bits) {
    enable_key0_irq();

    // SCPK0=0x01, SMPK0=0xFE → only bit 0 is compared (mask hides bits 7..1)
    // Condition: (k5d & ~0xFE) == (0x01 & ~0xFE) → k5d[0] == 1
    bus.write16(PINT_BASE + 6, 0x0001); // SCPK0=0x01
    bus.write16(PINT_BASE + 8, 0x00FE); // SMPK0=0xFE

    port.set_k5(0xFF); // bit 0 = 1 → match despite other bits differing
    EXPECT_EQ(20, last_irq) << "Masked bits must be ignored in KEY0 comparison";
}

// ---------------------------------------------------------------------------
// P port: read/write via bus
// ---------------------------------------------------------------------------
TEST_F(PortCtrlFixture, PPort_ReadWrite) {
    // P0 port: CFP (lo byte) + PD (hi byte) at PPORT_BASE
    bus.write16(PPORT_BASE, 0xAB0C); // CFP=0x0C, PD=0xAB
    uint16_t hw = bus.read16(PPORT_BASE);
    EXPECT_EQ(0x0C, static_cast<uint8_t>(hw))       << "CFP in lo byte";
    EXPECT_EQ(0xAB, static_cast<uint8_t>(hw >> 8))  << "PD in hi byte";
    EXPECT_EQ(0xAB, port.pd(0))                      << "pd(0) accessor";
}

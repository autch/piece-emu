#include <gtest/gtest.h>
#include "peripheral_sif3.hpp"
#include "peripheral_hsdma.hpp"
#include "peripheral_intc.hpp"
#include "bus.hpp"
#include <vector>

// ============================================================================
// Sif3 + Hsdma unit tests
// ============================================================================

// SIF3 register addresses
static constexpr uint32_t SIF3_TXD_ADDR    = 0x0401F5; // odd → high byte of 0x401F4 pair
static constexpr uint32_t SIF3_RXD_ADDR    = 0x0401F6; // low byte
static constexpr uint32_t SIF3_STATUS_ADDR = 0x0401F7; // high byte of 0x401F6 pair
static constexpr uint32_t SIF3_CTL_ADDR    = 0x0401F8; // low byte
static constexpr uint32_t SIF3_IRDA_ADDR   = 0x0401F9; // high byte of 0x401F8 pair

// HSDMA Ch0 register addresses
static constexpr uint32_t HS0_CNT_LO  = 0x048220;
static constexpr uint32_t HS0_CNT_HI  = 0x048222;
static constexpr uint32_t HS0_SADR_LO = 0x048224;
static constexpr uint32_t HS0_SADR_HI = 0x048226;
static constexpr uint32_t HS0_EN      = 0x04822C;

// HSDMA Ch1 register addresses
static constexpr uint32_t HS1_CNT_LO  = 0x048230;
static constexpr uint32_t HS1_SADR_LO = 0x048234;
static constexpr uint32_t HS1_SADR_HI = 0x048236;
static constexpr uint32_t HS1_EN      = 0x04823C;

class Sif3Fixture : public ::testing::Test {
protected:
    Bus bus;
    InterruptController intc;
    Hsdma hsdma;
    Sif3  sif3;
    std::vector<uint8_t> received;

    Sif3Fixture() : bus(0x040000, 0x080000) {
        intc.attach(bus, [](int, int){});
        hsdma.attach(bus);
        sif3.attach(bus, intc, hsdma);
        sif3.set_txd_callback([this](uint8_t b){ received.push_back(b); });
    }
};

// ---------------------------------------------------------------------------
// STATUS register — TDBE always set
// ---------------------------------------------------------------------------

TEST_F(Sif3Fixture, StatusTdbeAlwaysSet) {
    uint8_t status = bus.read8(SIF3_STATUS_ADDR);
    EXPECT_EQ(status & 0x02, 0x02) << "TDBE bit must always be 1";
}

// ---------------------------------------------------------------------------
// CTL / IRDA registers — basic R/W
// ---------------------------------------------------------------------------

TEST_F(Sif3Fixture, CtlIrdaRoundTrip) {
    // Write via 16-bit halfword at 0x401F8: lo=CTL, hi=IRDA
    bus.write16(SIF3_CTL_ADDR, 0xA5C3u); // CTL=0xC3, IRDA=0xA5
    EXPECT_EQ(sif3.ctl(),    0xC3u);
    uint8_t irda_val = static_cast<uint8_t>(bus.read16(SIF3_CTL_ADDR) >> 8);
    EXPECT_EQ(irda_val,      0xA5u);
}

// ---------------------------------------------------------------------------
// Single TXD byte write triggers callback
// ---------------------------------------------------------------------------

TEST_F(Sif3Fixture, SingleTxdByteDelivered) {
    bus.write8(SIF3_TXD_ADDR, 0x42);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0], 0x42u);
}

// ---------------------------------------------------------------------------
// Write to SIF2_IRDA (low byte at 0x401F4) must NOT trigger SIF3 TXD callback
// ---------------------------------------------------------------------------

TEST_F(Sif3Fixture, Sif2IrdaWriteDoesNotTriggerTxd) {
    // Byte write to 0x401F4 (SIF2_IRDA, the low byte of the 0x401F4 pair)
    // must NOT fire the SIF3 TXD callback.
    bus.write8(0x0401F4u, 0x42u);
    EXPECT_TRUE(received.empty()) << "SIF2_IRDA byte write must not trigger SIF3 TXD";
}

// ---------------------------------------------------------------------------
// HSDMA Ch0 inline DMA: TXD write while ch0 EN active drains DMA buffer
// ---------------------------------------------------------------------------

TEST_F(Sif3Fixture, Hsdma0InlineDmaAfterTxdWrite) {
    // Place 3 bytes in SRAM at 0x100010
    bus.write8(0x100010, 0xAA);
    bus.write8(0x100011, 0xBB);
    bus.write8(0x100012, 0xCC);

    // Configure HSDMA Ch0: SADR=0x100010, CNT=3
    bus.write16(HS0_SADR_LO, 0x0010u); // lo halfword of 0x100010
    bus.write16(HS0_SADR_HI, 0x0010u); // hi halfword of 0x100010
    bus.write16(HS0_CNT_LO,  3u);      // CNT lo = 3
    bus.write16(HS0_CNT_HI,  0u);
    // Enable Ch0
    bus.write16(HS0_EN, 0x0001u);
    EXPECT_TRUE(hsdma.ch0_en);

    // TXD write triggers: first byte = 0x11 (written to TXD), then DMA bytes
    bus.write8(SIF3_TXD_ADDR, 0x11u);

    // Expect: 0x11 first, then 0xAA, 0xBB, 0xCC from DMA
    ASSERT_EQ(received.size(), 4u);
    EXPECT_EQ(received[0], 0x11u);
    EXPECT_EQ(received[1], 0xAAu);
    EXPECT_EQ(received[2], 0xBBu);
    EXPECT_EQ(received[3], 0xCCu);

    // After exhaustion, ch0 EN must be cleared
    EXPECT_FALSE(hsdma.ch0_en);
    EXPECT_EQ(hsdma.ch0_cnt, 0u);
    EXPECT_EQ(hsdma.ch0_sadr, 0x100013u);
}

// ---------------------------------------------------------------------------
// HSDMA Ch0: second TXD write after DMA exhaustion does not re-trigger DMA
// ---------------------------------------------------------------------------

TEST_F(Sif3Fixture, Hsdma0DisabledAfterExhaustion) {
    bus.write8(0x100010, 0x55);
    bus.write16(HS0_SADR_LO, 0x0010u);
    bus.write16(HS0_SADR_HI, 0x0010u);
    bus.write16(HS0_CNT_LO,  1u);
    bus.write16(HS0_EN, 0x0001u);

    bus.write8(SIF3_TXD_ADDR, 0xFEu); // triggers: 0xFE then 0x55
    ASSERT_EQ(received.size(), 2u);

    received.clear();
    bus.write8(SIF3_TXD_ADDR, 0x99u); // DMA exhausted, only TXD byte
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0], 0x99u);
}

// ---------------------------------------------------------------------------
// HSDMA register read-back
// ---------------------------------------------------------------------------

TEST_F(Sif3Fixture, HsdmaRegisterReadBack) {
    // Write SADR for Ch1 and read it back
    bus.write16(HS1_SADR_LO, 0x3456u);
    bus.write16(HS1_SADR_HI, 0x0012u);
    EXPECT_EQ(bus.read16(HS1_SADR_LO), 0x3456u);
    EXPECT_EQ(bus.read16(HS1_SADR_HI), 0x0012u);
    EXPECT_EQ(hsdma.ch1_sadr, 0x00123456u);
}

// ---------------------------------------------------------------------------
// HSDMA Ch1 EN rising-edge callback
// ---------------------------------------------------------------------------

TEST_F(Sif3Fixture, Hsdma1RisingEdgeCallback) {
    uint32_t cb_sadr = 0;
    uint32_t cb_cnt  = 0;
    hsdma.on_ch1_start = [&](Bus&, uint32_t sadr, uint32_t cnt) {
        cb_sadr = sadr;
        cb_cnt  = cnt;
    };

    bus.write16(HS1_SADR_LO, 0x0400u);
    bus.write16(HS1_SADR_HI, 0x0010u); // sadr = 0x100400
    bus.write16(HS1_CNT_LO,  256u);
    // Enable Ch1 (0→1 transition)
    bus.write16(HS1_EN, 0x0001u);

    EXPECT_EQ(cb_sadr, 0x100400u);
    EXPECT_EQ(cb_cnt,  256u);
}

// ---------------------------------------------------------------------------
// HSDMA Ch1 EN write of 0 (1→0) does NOT re-fire callback
// ---------------------------------------------------------------------------

TEST_F(Sif3Fixture, Hsdma1NoCallbackOnDisable) {
    int call_count = 0;
    hsdma.on_ch1_start = [&](Bus&, uint32_t, uint32_t) { call_count++; };

    bus.write16(HS1_EN, 0x0001u); // enable (fires callback)
    bus.write16(HS1_EN, 0x0000u); // disable (must NOT fire callback again)
    bus.write16(HS1_EN, 0x0001u); // re-enable (fires callback again)

    EXPECT_EQ(call_count, 2);
}

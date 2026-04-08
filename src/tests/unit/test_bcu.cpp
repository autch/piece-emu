// test_bcu.cpp — Layer 1: BCU address decode and memory access unit tests
// Tests that the Bus correctly routes reads/writes to the right regions,
// and that cycle counts are accumulated correctly.
#include "bus.hpp"
#include <gtest/gtest.h>
#include <cstdint>

class BcuFixture : public ::testing::Test {
protected:
    Bus bus;
    BcuFixture() : bus(0x80000 /* 512 KB flash */) {
        bus.sram_wait  = 3;
        bus.flash_wait = 3;
    }
};

// ---- IRAM region ----

TEST_F(BcuFixture, Iram_Write8_Read8) {
    bus.write8(0x000010, 0xAB);
    EXPECT_EQ(bus.read8(0x000010), 0xAB);
}

TEST_F(BcuFixture, Iram_Write16_Read16) {
    bus.write16(0x000100, 0x1234);
    EXPECT_EQ(bus.read16(0x000100), 0x1234);
}

TEST_F(BcuFixture, Iram_Write32_Read32) {
    bus.write32(0x000200, 0xDEADBEEF);
    EXPECT_EQ(bus.read32(0x000200), 0xDEADBEEFu);
}

TEST_F(BcuFixture, Iram_NoCycles) {
    bus.cycles = 0;
    bus.write8(0x000000, 0x55);
    bus.read8(0x000000);
    EXPECT_EQ(bus.cycles, 0u); // internal RAM = 0 wait, no cycle charge
}

TEST_F(BcuFixture, Iram_Mirror_Read8) {
    // 0x002000–0x002FFF mirrors internal RAM
    bus.write8(0x000010, 0x77);
    EXPECT_EQ(bus.read8(0x002010), 0x77);
}

// ---- SRAM region ----

TEST_F(BcuFixture, Sram_Write8_Read8) {
    bus.write8(0x100000, 0x42);
    EXPECT_EQ(bus.read8(0x100000), 0x42);
}

TEST_F(BcuFixture, Sram_Read8_AccruesCycles) {
    bus.cycles = 0;
    bus.write8(0x100000, 0);
    // write8 to SRAM: sram_wait + 2 = 5 cycles
    uint32_t after_write = bus.cycles;
    bus.read8(0x100000);
    // read8 from SRAM: sram_wait + 1 = 4 cycles
    EXPECT_EQ(bus.cycles, after_write + 4u);
}

TEST_F(BcuFixture, Sram_Read32_AccruesCycles) {
    bus.cycles = 0;
    bus.read32(0x100000);
    // read32 from SRAM: (sram_wait + 1) * 2 = 8 cycles
    EXPECT_EQ(bus.cycles, 8u);
}

TEST_F(BcuFixture, Sram_Write16_Read16) {
    bus.write16(0x100002, 0xBEEF);
    EXPECT_EQ(bus.read16(0x100002), 0xBEEFu);
}

// ---- Flash region ----

TEST_F(BcuFixture, Flash_LoadAndRead8) {
    uint8_t data[] = {0x11, 0x22, 0x33, 0x44};
    bus.load_flash(0, data, 4);
    EXPECT_EQ(bus.read8(0xC00000), 0x11);
    EXPECT_EQ(bus.read8(0xC00001), 0x22);
    EXPECT_EQ(bus.read8(0xC00002), 0x33);
    EXPECT_EQ(bus.read8(0xC00003), 0x44);
}

TEST_F(BcuFixture, Flash_Read16_LittleEndian) {
    uint8_t data[] = {0x34, 0x12};
    bus.load_flash(0, data, 2);
    EXPECT_EQ(bus.read16(0xC00000), 0x1234);
}

TEST_F(BcuFixture, Flash_Read8_AccruesCycles) {
    bus.cycles = 0;
    bus.read8(0xC00000);
    // flash_wait + 1 = 4
    EXPECT_EQ(bus.cycles, 4u);
}

TEST_F(BcuFixture, Flash_Read32_AccruesCycles) {
    bus.cycles = 0;
    bus.read32(0xC00000);
    // (flash_wait + 1) * 2 = 8
    EXPECT_EQ(bus.cycles, 8u);
}

// ---- Fetch (instruction fetch bypasses cycle charge) ----

TEST_F(BcuFixture, Fetch_Iram_NoCycles) {
    bus.write16(0x000000, 0xABCD);
    bus.cycles = 0;
    EXPECT_EQ(bus.fetch16(0x000000), 0xABCD);
    EXPECT_EQ(bus.cycles, 0u);
}

TEST_F(BcuFixture, Fetch_Flash_NoCycles) {
    uint8_t data[] = {0x34, 0x12};
    bus.load_flash(0, data, 2);
    bus.cycles = 0;
    EXPECT_EQ(bus.fetch16(0xC00000), 0x1234);
    EXPECT_EQ(bus.cycles, 0u); // fetch16 does not charge cycles
}

// ---- Unmapped region returns open-bus ----

TEST_F(BcuFixture, Unmapped_Read8_ReturnsFF) {
    EXPECT_EQ(bus.read8(0x800000), 0xFF);
}

TEST_F(BcuFixture, Unmapped_Read16_ReturnsFFFF) {
    EXPECT_EQ(bus.read16(0x800000), 0xFFFFu);
}

// ---- 28-bit address masking ----

TEST_F(BcuFixture, AddressMask_28bit) {
    // 0x10000000 should alias to 0x0000000 (IRAM after masking)
    bus.write8(0x000000, 0xCC);
    EXPECT_EQ(bus.read8(0x10000000), 0xCC);
}

// ---- IO handler registration and dispatch ----

TEST_F(BcuFixture, IO_RegisterAndRead) {
    uint16_t reg_val = 0x5A5A;
    bus.register_io(0x030000, {
        [&](uint32_t) -> uint16_t { return reg_val; },
        nullptr
    });
    EXPECT_EQ(bus.read16(0x030000), 0x5A5A);
}

TEST_F(BcuFixture, IO_RegisterAndWrite) {
    uint16_t written = 0;
    bus.register_io(0x030002, {
        nullptr,
        [&](uint32_t, uint16_t v) { written = v; }
    });
    bus.write16(0x030002, 0x1234);
    EXPECT_EQ(written, 0x1234u);
}

TEST_F(BcuFixture, IO_UnregisteredRead_ReturnsFF) {
    EXPECT_EQ(bus.read16(0x030010), 0xFFFFu);
}

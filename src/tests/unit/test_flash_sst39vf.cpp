#include <gtest/gtest.h>
#include "flash_sst39vf.hpp"

#include <vector>

// ============================================================================
// Sst39vf — SST 39VF400A / 39VF160 unit tests
//
// Drives the device through the same six-cycle command sequences the P/ECE
// kernel uses (see sdk/sysdev/pcekn/fmacc*.c) and verifies state transitions,
// memory mutations, dirty tracking, and CFI / Software-ID readouts.
// ============================================================================

namespace {

// Halfword-byte address helpers.  Real chip is x16, so the "5555" command
// address is byte-offset 0xAAAA in the device.
constexpr uint32_t CMD1 = 0x5555 * 2; // byte address of half-word 0x5555
constexpr uint32_t CMD2 = 0x2AAA * 2;

void unlock(Sst39vf& f) {
    f.write16(CMD1, 0xAA);
    f.write16(CMD2, 0x55);
}

void word_program(Sst39vf& f, uint32_t byte_offset, uint16_t value) {
    unlock(f);
    f.write16(CMD1, 0xA0);
    f.write16(byte_offset, value);
}

void sector_erase(Sst39vf& f, uint32_t sector_byte_addr) {
    unlock(f);
    f.write16(CMD1, 0x80);
    unlock(f);
    f.write16(sector_byte_addr, 0x30);
}

void block_erase(Sst39vf& f, uint32_t block_byte_addr) {
    unlock(f);
    f.write16(CMD1, 0x80);
    unlock(f);
    f.write16(block_byte_addr, 0x50);
}

void chip_erase(Sst39vf& f) {
    unlock(f);
    f.write16(CMD1, 0x80);
    unlock(f);
    f.write16(CMD1, 0x10);
}

void enter_cfi(Sst39vf& f) {
    unlock(f);
    f.write16(CMD1, 0x98);
}

void enter_software_id(Sst39vf& f) {
    unlock(f);
    f.write16(CMD1, 0x90);
}

void exit_short(Sst39vf& f) {
    f.write16(0, 0xF0);
}

} // namespace

// ---- construction ----------------------------------------------------------

TEST(Sst39vf, ConstructDefaultIs2MB) {
    Sst39vf f;
    EXPECT_EQ(f.mem_size(), 0x200000u);
    EXPECT_EQ(f.manufacturer_id(), 0x00BFu);
    EXPECT_EQ(f.device_id(), 0x2782u); // SST39VF160
    EXPECT_EQ(f.sector_count(), 0x200u); // 2 MB / 4 KB = 512 sectors
}

TEST(Sst39vf, Construct512KBVariant) {
    Sst39vf f(0x80000);
    EXPECT_EQ(f.mem_size(), 0x80000u);
    EXPECT_EQ(f.device_id(), 0x2780u); // SST39VF400A
    EXPECT_EQ(f.sector_count(), 128u);
}

TEST(Sst39vf, FactoryForMinBytes) {
    Sst39vf small  = Sst39vf::for_min_bytes(0x40000); // 256 KB needed
    Sst39vf medium = Sst39vf::for_min_bytes(0x80000); // 512 KB
    Sst39vf large  = Sst39vf::for_min_bytes(0x180000); // 1.5 MB
    EXPECT_EQ(small.mem_size(),  0x80000u);  // bumped up to SST39VF400A
    EXPECT_EQ(medium.mem_size(), 0x80000u);
    EXPECT_EQ(large.mem_size(),  0x200000u); // SST39VF160
}

TEST(Sst39vf, ErasedStateIsAllOnes) {
    Sst39vf f(0x80000);
    EXPECT_EQ(f.read16(0),       0xFFFFu);
    EXPECT_EQ(f.read16(0x12000), 0xFFFFu);
}

TEST(Sst39vf, RejectsNonPowerOfTwo) {
    EXPECT_THROW(Sst39vf(0x90000), std::invalid_argument);
}

// ---- bulk image load -------------------------------------------------------

TEST(Sst39vf, LoadImageDoesNotMarkSectorsDirty) {
    Sst39vf f(0x80000);
    std::vector<uint8_t> blob(0x80000, 0xAA);
    f.load_image(0, blob.data(), blob.size());
    EXPECT_FALSE(f.any_dirty());
    EXPECT_EQ(f.read16(0), 0xAAAAu);
}

// ---- word program ----------------------------------------------------------

TEST(Sst39vf, WordProgramWritesAndMarksSectorDirty) {
    Sst39vf f(0x80000);
    word_program(f, 0x10, 0x1234);
    EXPECT_EQ(f.read16(0x10), 0x1234u);
    EXPECT_TRUE(f.is_sector_dirty(0));
    EXPECT_FALSE(f.is_sector_dirty(1));
}

TEST(Sst39vf, WordProgramOverwritesNoAndMask) {
    // Real silicon AND-masks (1→0 only); piemu and we both overwrite.
    // Pin this behaviour so future refactors do not silently flip it.
    Sst39vf f(0x80000);
    word_program(f, 0x20, 0x0F0F);
    EXPECT_EQ(f.read16(0x20), 0x0F0Fu);
    word_program(f, 0x20, 0xFFFF);
    EXPECT_EQ(f.read16(0x20), 0xFFFFu);
}

TEST(Sst39vf, WordProgramReturnsToNormalAfterOneCommit) {
    Sst39vf f(0x80000);
    word_program(f, 0x40, 0xABCD);
    // A bare write at this point should not be interpreted as program data.
    f.write16(0x60, 0x1234);
    EXPECT_EQ(f.read16(0x60), 0xFFFFu); // memory unchanged (state == Normal)
}

TEST(Sst39vf, MultipleProgramsMarkRespectiveSectors) {
    Sst39vf f(0x80000);
    word_program(f, 0x0000, 0x1111);
    word_program(f, 0x1000, 0x2222);
    word_program(f, 0x4000, 0x3333);
    EXPECT_TRUE (f.is_sector_dirty(0));
    EXPECT_TRUE (f.is_sector_dirty(1));
    EXPECT_FALSE(f.is_sector_dirty(2));
    EXPECT_FALSE(f.is_sector_dirty(3));
    EXPECT_TRUE (f.is_sector_dirty(4));
}

// ---- sector erase ----------------------------------------------------------

TEST(Sst39vf, SectorEraseFillsSectorAndMarksDirty) {
    Sst39vf f(0x80000);
    word_program(f, 0x10, 0x1234);
    word_program(f, 0x20, 0x5678);
    EXPECT_EQ(f.read16(0x10), 0x1234u);

    sector_erase(f, 0x800); // any address inside sector 0
    EXPECT_EQ(f.read16(0x10), 0xFFFFu);
    EXPECT_EQ(f.read16(0x20), 0xFFFFu);
    EXPECT_TRUE(f.is_sector_dirty(0));
}

TEST(Sst39vf, SectorEraseDoesNotTouchAdjacentSectors) {
    Sst39vf f(0x80000);
    word_program(f, 0x1000, 0xCAFE); // sector 1
    word_program(f, 0x2000, 0xBEEF); // sector 2

    sector_erase(f, 0x1000); // erase sector 1 only

    EXPECT_EQ(f.read16(0x1000), 0xFFFFu);
    EXPECT_EQ(f.read16(0x2000), 0xBEEFu);
}

// ---- block erase / chip erase ---------------------------------------------

TEST(Sst39vf, BlockEraseFills64KBAndMarksAllSectorsDirty) {
    Sst39vf f(0x80000);
    word_program(f, 0xF0, 0x1234);
    word_program(f, 0xF000, 0xABCD);

    block_erase(f, 0x0); // block 0 → 0x0000–0xFFFF

    EXPECT_EQ(f.read16(0xF0), 0xFFFFu);
    EXPECT_EQ(f.read16(0xF000), 0xFFFFu);
    for (uint32_t s = 0; s < 16; ++s)
        EXPECT_TRUE(f.is_sector_dirty(s)) << "sector " << s;
    // sector 16 (start of block 1) untouched
    EXPECT_FALSE(f.is_sector_dirty(16));
}

TEST(Sst39vf, ChipEraseClearsEverythingAndMarksAllDirty) {
    Sst39vf f(0x80000);
    word_program(f, 0x10,    0x1111);
    word_program(f, 0x10000, 0x2222);
    word_program(f, 0x70000, 0x3333);

    chip_erase(f);

    EXPECT_EQ(f.read16(0x10),    0xFFFFu);
    EXPECT_EQ(f.read16(0x10000), 0xFFFFu);
    EXPECT_EQ(f.read16(0x70000), 0xFFFFu);

    for (std::size_t s = 0; s < f.sector_count(); ++s)
        EXPECT_TRUE(f.is_sector_dirty(static_cast<uint32_t>(s)));
}

// ---- exit / state robustness ----------------------------------------------

TEST(Sst39vf, ShortExitReturnsToNormalFromAnywhere) {
    Sst39vf f(0x80000);
    enter_cfi(f);
    EXPECT_EQ(f.read16(0x10 * 2), 0x0051u); // 'Q'
    exit_short(f);
    EXPECT_EQ(f.read16(0x10 * 2), 0xFFFFu); // back to memory view (erased)
}

TEST(Sst39vf, BrokenUnlockSequenceIsSilentlyIgnored) {
    Sst39vf f(0x80000);
    f.write16(CMD1, 0xAA);
    f.write16(CMD2, 0x99);   // wrong second byte
    // State machine should have reset to Normal; subsequent program attempt
    // without a fresh AA prefix must NOT modify memory.
    f.write16(CMD1, 0xA0);
    f.write16(0x100, 0xDEAD);
    EXPECT_EQ(f.read16(0x100), 0xFFFFu);
    EXPECT_FALSE(f.any_dirty());
}

TEST(Sst39vf, F0BeforeUnlockIsAcceptedAsExit) {
    // Kernel pattern: emit F0 defensively before each program loop iteration.
    Sst39vf f(0x80000);
    f.write16(CMD1, 0xF0);          // accepted as Exit (no-op from Normal)
    word_program(f, 0x10, 0x1234);  // still works
    EXPECT_EQ(f.read16(0x10), 0x1234u);
}

// ---- CFI Query -------------------------------------------------------------

TEST(Sst39vf, CfiQueryReturnsQRY) {
    Sst39vf f(0x80000);
    enter_cfi(f);
    EXPECT_EQ(f.read16(0x10 * 2), 0x0051u); // 'Q'
    EXPECT_EQ(f.read16(0x11 * 2), 0x0052u); // 'R'
    EXPECT_EQ(f.read16(0x12 * 2), 0x0059u); // 'Y'
}

TEST(Sst39vf, CfiDeviceSizeMatchesCapacity) {
    {
        Sst39vf f(0x80000);  // 512 KB
        enter_cfi(f);
        EXPECT_EQ(f.read16(0x27 * 2), 0x13u); // 2^19
    }
    {
        Sst39vf f(0x200000); // 2 MB
        enter_cfi(f);
        EXPECT_EQ(f.read16(0x27 * 2), 0x15u); // 2^21
    }
}

TEST(Sst39vf, CfiRegionDescriptorsMatchGeometry) {
    Sst39vf f(0x200000); // 2 MB
    enter_cfi(f);
    // Region 1: y = 511, z = 0x10 (4 KB)
    EXPECT_EQ(f.read16(0x2D * 2), 511u & 0xFFu);
    EXPECT_EQ(f.read16(0x2E * 2), (511u >> 8) & 0xFFu);
    EXPECT_EQ(f.read16(0x2F * 2), 0x10u);
    EXPECT_EQ(f.read16(0x30 * 2), 0x00u);
    // Region 2: y = 31, z = 256 (64 KB)
    EXPECT_EQ(f.read16(0x31 * 2), 31u & 0xFFu);
    EXPECT_EQ(f.read16(0x32 * 2), 0x00u);
    EXPECT_EQ(f.read16(0x33 * 2), 0x00u);
    EXPECT_EQ(f.read16(0x34 * 2), 0x01u);
}

// ---- Software ID -----------------------------------------------------------

TEST(Sst39vf, SoftwareIdReportsManufacturerAndDevice) {
    Sst39vf f(0x200000);
    enter_software_id(f);
    EXPECT_EQ(f.read16(0), 0x00BFu);
    EXPECT_EQ(f.read16(2), 0x2782u); // SST39VF160
    exit_short(f);
}

// ---- dirty callback --------------------------------------------------------

TEST(Sst39vf, DirtyCallbackFiresOncePerSectorTransition) {
    Sst39vf f(0x80000);
    std::vector<uint32_t> seen;
    f.set_dirty_callback([&](uint32_t s){ seen.push_back(s); });

    word_program(f, 0x10, 0x1111); // sector 0 first time
    word_program(f, 0x20, 0x2222); // sector 0 again — no callback
    word_program(f, 0x4000, 0x3333); // sector 4 first time

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], 0u);
    EXPECT_EQ(seen[1], 4u);
}

TEST(Sst39vf, ClearDirtyResetsTracking) {
    Sst39vf f(0x80000);
    word_program(f, 0x10, 0x1234);
    EXPECT_TRUE(f.any_dirty());
    f.clear_dirty();
    EXPECT_FALSE(f.any_dirty());
    EXPECT_FALSE(f.is_sector_dirty(0));
}

TEST(Sst39vf, ForEachDirtyVisitsAscendingOrder) {
    Sst39vf f(0x80000);
    word_program(f, 0x4000, 0); // sector 4
    word_program(f, 0x0000, 0); // sector 0
    word_program(f, 0x1000, 0); // sector 1

    std::vector<uint32_t> visited;
    f.for_each_dirty_sector([&](uint32_t s){ visited.push_back(s); });
    ASSERT_EQ(visited.size(), 3u);
    EXPECT_EQ(visited[0], 0u);
    EXPECT_EQ(visited[1], 1u);
    EXPECT_EQ(visited[2], 4u);
}

// ---- byte read -------------------------------------------------------------

TEST(Sst39vf, Read8DoesNotAdvanceStateMachine) {
    Sst39vf f(0x80000);
    word_program(f, 0x100, 0xCAFE);
    // Random byte reads at "command" addresses must not consume state.
    f.read8(CMD1);
    f.read8(CMD2);
    word_program(f, 0x102, 0xBEEF);
    EXPECT_EQ(f.read16(0x100), 0xCAFEu);
    EXPECT_EQ(f.read16(0x102), 0xBEEFu);
}

TEST(Sst39vf, Read8ReturnsCorrectByteOfHalfword) {
    Sst39vf f(0x80000);
    word_program(f, 0x100, 0xCAFE);
    EXPECT_EQ(f.read8(0x100), 0xFEu);
    EXPECT_EQ(f.read8(0x101), 0xCAu);
}

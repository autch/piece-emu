#include <gtest/gtest.h>
#include "flash_sst39vf.hpp"
#include "pfi_writeback.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

// ============================================================================
// PfiWriteback — end-to-end unit tests using a temporary PFI file.
//
// These tests construct a synthetic PFI-shaped file (header + flash blob)
// in /tmp, attach a Sst39vf to a PfiWriteback in WriteBack mode, drive
// kernel-style word-program / sector-erase sequences through the device,
// and verify the resulting host file matches the device memory after
// shutdown_flush().
//
// Debounce paths are checked separately by stepping a fake clock.
// ============================================================================

namespace {

// Halfword-byte address helpers (chip is x16; "5555" address = byte 0xAAAA).
constexpr uint32_t CMD1 = 0x5555 * 2;
constexpr uint32_t CMD2 = 0x2AAA * 2;
constexpr uint32_t HEADER_SZ = 0x40; // arbitrary header pad

void unlock(Sst39vf& f) {
    f.write16(CMD1, 0xAA);
    f.write16(CMD2, 0x55);
}

void word_program(Sst39vf& f, uint32_t off, uint16_t v) {
    unlock(f);
    f.write16(CMD1, 0xA0);
    f.write16(off, v);
}

std::string make_temp_pfi(std::size_t flash_bytes) {
    namespace fs = std::filesystem;
    auto path = fs::temp_directory_path() / ("pfi_writeback_test_"
                + std::to_string(::getpid()) + "_"
                + std::to_string(std::random_device{}()) + ".pfi");
    std::vector<uint8_t> blob(HEADER_SZ + flash_bytes, 0xFF);
    // Stamp a recognisable pattern in the header so we can verify it's
    // preserved across writeback (we never touch bytes outside the flash
    // region defined by flash_offset_in_pfi).
    for (std::size_t i = 0; i < HEADER_SZ; ++i)
        blob[i] = static_cast<uint8_t>(i ^ 0x5A);
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(blob.data()),
                static_cast<std::streamsize>(blob.size()));
    }
    return path.string();
}

std::vector<uint8_t> read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    auto sz = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> buf(sz);
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(sz));
    return buf;
}

void cleanup(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace

// ---- ReadOnly: verify host file untouched -----------------------------------

TEST(PfiWriteback, ReadOnlyDoesNotMutateHostFile) {
    constexpr std::size_t FLASH_SZ = 0x80000; // 512 KB
    auto path = make_temp_pfi(FLASH_SZ);
    auto before = read_all(path);

    Sst39vf flash(FLASH_SZ);
    PfiWriteback wb;
    wb.attach(&flash, PfiWriteback::Mode::ReadOnly,
              path, HEADER_SZ, /*debounce_ms=*/0);

    word_program(flash, 0x1000, 0xCAFE);
    EXPECT_TRUE(flash.any_dirty());

    wb.poll(0); // even with debounce_ms=0, ReadOnly never writes
    wb.shutdown_flush();

    auto after = read_all(path);
    EXPECT_EQ(before, after);
    cleanup(path);
}

// ---- WriteBack: shutdown_flush mirrors device into the file -----------------

TEST(PfiWriteback, ShutdownFlushPushesAllDirtySectors) {
    constexpr std::size_t FLASH_SZ = 0x80000;
    auto path = make_temp_pfi(FLASH_SZ);

    Sst39vf flash(FLASH_SZ);
    PfiWriteback wb;
    ASSERT_TRUE(wb.attach(&flash, PfiWriteback::Mode::WriteBack,
                          path, HEADER_SZ, /*debounce_ms=*/2000));

    // Two writes in two different sectors (0 and 4).
    word_program(flash, 0x0010, 0x1234);
    word_program(flash, 0x4020, 0x5678);

    // Without poll() the debounce has not yet elapsed — file untouched.
    {
        auto buf = read_all(path);
        EXPECT_EQ(buf[HEADER_SZ + 0x0010], 0xFFu);
        EXPECT_EQ(buf[HEADER_SZ + 0x4020], 0xFFu);
    }

    wb.shutdown_flush();

    auto buf = read_all(path);
    // Header preserved.
    for (std::size_t i = 0; i < HEADER_SZ; ++i)
        EXPECT_EQ(buf[i], static_cast<uint8_t>(i ^ 0x5A));

    // Sector 0 contents reflect the program.  S1C33 is little-endian.
    EXPECT_EQ(buf[HEADER_SZ + 0x0010], 0x34u);
    EXPECT_EQ(buf[HEADER_SZ + 0x0011], 0x12u);
    // Sector 4 contents reflect the second program.
    EXPECT_EQ(buf[HEADER_SZ + 0x4020], 0x78u);
    EXPECT_EQ(buf[HEADER_SZ + 0x4021], 0x56u);
    // Untouched sectors (e.g. sector 1) remain 0xFF.
    EXPECT_EQ(buf[HEADER_SZ + 0x1000], 0xFFu);

    EXPECT_FALSE(flash.any_dirty()); // cleared after flush
    EXPECT_GE(wb.bytes_written(), 2u * Sst39vf::SECTOR_SIZE);
    EXPECT_GE(wb.flush_count(), 1u);
    cleanup(path);
}

// ---- Debounce: poll respects the debounce window ----------------------------

TEST(PfiWriteback, DebouncePollDelaysWrite) {
    constexpr std::size_t FLASH_SZ = 0x80000;
    auto path = make_temp_pfi(FLASH_SZ);

    Sst39vf flash(FLASH_SZ);
    PfiWriteback wb;
    ASSERT_TRUE(wb.attach(&flash, PfiWriteback::Mode::WriteBack,
                          path, HEADER_SZ, /*debounce_ms=*/100));

    word_program(flash, 0x10, 0xBEEF);
    EXPECT_TRUE(flash.any_dirty());

    // 50 ms is below the 100 ms debounce — must not flush yet.
    // The dirty timestamp is set by the Sst39vf callback to whatever
    // steady_clock returned at write time; we only know it's ≤ now.
    // Immediate poll() should leave the file unchanged.
    wb.poll(/*now_us=*/0);
    EXPECT_TRUE(flash.any_dirty());

    {
        auto buf = read_all(path);
        EXPECT_EQ(buf[HEADER_SZ + 0x10], 0xFFu); // not yet flushed
    }

    // shutdown_flush bypasses the debounce — pin that contract.
    wb.shutdown_flush();
    auto buf = read_all(path);
    EXPECT_EQ(buf[HEADER_SZ + 0x10], 0xEFu);
    cleanup(path);
}

// ---- WriteBack downgrades to ReadOnly if the file is missing ----------------

TEST(PfiWriteback, MissingFileDowngradesToReadOnly) {
    Sst39vf flash(0x80000);
    PfiWriteback wb;
    bool ok = wb.attach(&flash, PfiWriteback::Mode::WriteBack,
                        "/tmp/__definitely_not_a_real_file__.pfi",
                        HEADER_SZ, 1000);
    EXPECT_TRUE(ok); // attach itself returns true; mode falls back
    EXPECT_EQ(wb.mode(), PfiWriteback::Mode::ReadOnly);

    word_program(flash, 0x100, 0x1234);
    wb.shutdown_flush(); // must not crash, no file to write
    SUCCEED();
}

// ---- A FlashDevice without dirty tracking is treated as ReadOnly ------------

TEST(PfiWriteback, RomBackedDeviceForcesReadOnly) {
    FlatFlashRom rom(0x80000);
    constexpr std::size_t FLASH_SZ = 0x80000;
    auto path = make_temp_pfi(FLASH_SZ);

    PfiWriteback wb;
    wb.attach(&rom, PfiWriteback::Mode::WriteBack,
              path, HEADER_SZ, /*debounce_ms=*/0);
    EXPECT_EQ(wb.mode(), PfiWriteback::Mode::ReadOnly);

    auto before = read_all(path);
    wb.shutdown_flush();
    auto after = read_all(path);
    EXPECT_EQ(before, after);
    cleanup(path);
}

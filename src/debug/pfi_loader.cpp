#include "pfi_loader.hpp"
#include "bus.hpp"
#include <cstdio>
#include <cstring>
#include <format>
#include <stdexcept>
#include <vector>

// PFI header on-disk layout (40 bytes total, all fields little-endian)
// See piemu/tools/pfi_types.h and piemu/include/piece_types.h for reference.
static constexpr std::size_t PFI_SYSINFO_SIZE = 32; // expected sys_info.size value

// Read a little-endian uint16 from a byte buffer at the given offset.
static uint16_t read_le16(const uint8_t* buf, std::size_t off) {
    return static_cast<uint16_t>(buf[off]) |
           (static_cast<uint16_t>(buf[off + 1]) << 8);
}

// Read a little-endian uint32 from a byte buffer at the given offset.
static uint32_t read_le32(const uint8_t* buf, std::size_t off) {
    return static_cast<uint32_t>(buf[off])       |
           (static_cast<uint32_t>(buf[off + 1]) <<  8) |
           (static_cast<uint32_t>(buf[off + 2]) << 16) |
           (static_cast<uint32_t>(buf[off + 3]) << 24);
}

PfiInfo pfi_load(Bus& bus, const std::string& path)
{
    // ---- open file ----
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp)
        throw std::runtime_error(std::format("pfi_load: cannot open '{}'", path));

    // ---- read header (40 bytes) ----
    // Layout:
    //   [0..3]   signature  — {'1','I','F','P'} (LE store of multi-char 'PFI1')
    //   [4..7]   offset_to_flash — uint32_t
    //   [8..39]  SYSTEMINFO (32 bytes)
    static constexpr std::size_t HDR_SIZE = 40;
    uint8_t hdr[HDR_SIZE];
    if (std::fread(hdr, 1, HDR_SIZE, fp) != HDR_SIZE) {
        std::fclose(fp);
        throw std::runtime_error(std::format("pfi_load: '{}' too short to read header", path));
    }

    // ---- validate signature: bytes must be '1','I','F','P' ----
    // mkpfi sets pfi.signature = 'PFI1' (C multi-char literal = 0x50464931 on GCC).
    // Stored as LE uint32, the on-disk bytes are: 0x31 0x49 0x46 0x50 = "1IFP".
    if (hdr[0] != '1' || hdr[1] != 'I' || hdr[2] != 'F' || hdr[3] != 'P') {
        std::fclose(fp);
        throw std::runtime_error(std::format(
            "pfi_load: '{}' has invalid signature (expected 0x31494650 '1IFP', "
            "got 0x{:02X}{:02X}{:02X}{:02X})",
            path, hdr[0], hdr[1], hdr[2], hdr[3]));
    }

    uint32_t offset_to_flash = read_le32(hdr, 4);
    if (offset_to_flash < HDR_SIZE) {
        std::fclose(fp);
        throw std::runtime_error(std::format(
            "pfi_load: '{}' offset_to_flash ({}) is smaller than header size ({})",
            path, offset_to_flash, HDR_SIZE));
    }

    // ---- parse SYSTEMINFO ----
    PfiSysInfo si;
    si.size         = read_le16(hdr,  8);
    si.hard_ver     = read_le16(hdr, 10);
    si.bios_ver     = read_le16(hdr, 12);
    si.bios_date    = read_le16(hdr, 14);
    si.sys_clock    = read_le32(hdr, 16);
    si.vdde_voltage = read_le16(hdr, 20);
    si.resv1        = read_le16(hdr, 22);
    si.sram_top     = read_le32(hdr, 24);
    si.sram_end     = read_le32(hdr, 28);
    si.pffs_top     = read_le32(hdr, 32);
    si.pffs_end     = read_le32(hdr, 36);

    if (si.size != PFI_SYSINFO_SIZE) {
        std::fclose(fp);
        throw std::runtime_error(std::format(
            "pfi_load: '{}' SYSTEMINFO size mismatch (expected {}, got {})",
            path, PFI_SYSINFO_SIZE, si.size));
    }

    // ---- determine flash image size from SYSTEMINFO ----
    // pffs_end is the last address of the flash file system, which is
    // FLASH_BASE + flash_size.  Round up to the next power-of-two for
    // the allocation (matching piemu's LoadFlashImage logic).
    static constexpr uint32_t FLASH_BASE = Bus::FLASH_BASE;
    if (si.pffs_end <= FLASH_BASE) {
        std::fclose(fp);
        throw std::runtime_error(std::format(
            "pfi_load: '{}' SYSTEMINFO.pffs_end (0x{:X}) <= FLASH_BASE (0x{:X})",
            path, si.pffs_end, FLASH_BASE));
    }
    uint32_t flash_size = si.pffs_end - FLASH_BASE;

    // ---- seek to flash image ----
    if (std::fseek(fp, static_cast<long>(offset_to_flash), SEEK_SET) != 0) {
        std::fclose(fp);
        throw std::runtime_error(std::format(
            "pfi_load: '{}' seek to flash image at offset {} failed",
            path, offset_to_flash));
    }

    // ---- load flash image in 4 KB chunks ----
    static constexpr std::size_t CHUNK = 0x1000; // 4096 bytes
    std::vector<uint8_t> chunk(CHUNK);
    std::size_t total_loaded = 0;
    while (total_loaded < flash_size) {
        std::size_t to_read = std::min(CHUNK, static_cast<std::size_t>(flash_size) - total_loaded);
        std::size_t n = std::fread(chunk.data(), 1, to_read, fp);
        if (n == 0) break; // EOF — flash image may be shorter than pffs_end indicates
        bus.load_flash(static_cast<uint32_t>(total_loaded), chunk.data(), n);
        total_loaded += n;
    }

    std::fclose(fp);

    if (total_loaded == 0)
        throw std::runtime_error(std::format(
            "pfi_load: '{}' flash image is empty", path));

    std::fprintf(stderr, "Loaded PFI '%s': flash=0x%X bytes, sys_clock=%u Hz, "
                         "hard_ver=0x%04X, bios_ver=0x%04X\n",
                 path.c_str(), flash_size, si.sys_clock, si.hard_ver, si.bios_ver);

    return PfiInfo{si, total_loaded};
}

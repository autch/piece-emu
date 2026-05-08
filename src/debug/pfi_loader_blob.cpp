#include "pfi_loader.hpp"
#include "bus.hpp"
#include <cstdio>
#include <cstring>
#include <format>
#include <stdexcept>

// Memory-blob counterpart of pfi_loader.cpp.  Used by the libretro core
// (retro_load_game receives data + length rather than a path).
//
// Logic mirrors read_pfi_header / pfi_load in pfi_loader.cpp; kept as a
// parallel implementation rather than refactored into a shared core so
// the file-based path stays untouched.  Phase 2 may unify them once the
// libretro path is stable.

static constexpr uint16_t SYSINFO_EXPECTED_SIZE = 32;

// Validate header from blob, copy into *hdr_out.  Throws on any mismatch
// or short blob.
static void read_pfi_header_blob(const std::uint8_t* data, std::size_t len,
                                 PFIHEADER* hdr_out)
{
    if (!data)
        throw std::runtime_error("pfi_blob: null data pointer");
    if (len < sizeof(PFIHEADER))
        throw std::runtime_error(std::format(
            "pfi_blob: blob too short to read header ({} < {})",
            len, sizeof(PFIHEADER)));

    PFIHEADER hdr{};
    std::memcpy(&hdr, data, sizeof(PFIHEADER));

    if ((hdr.signature & 0xffffff00u) !=
        ((uint32_t)'P' << 24 | (uint32_t)'F' << 16 | (uint32_t)'I' << 8))
        throw std::runtime_error(std::format(
            "pfi_blob: invalid PFI signature (got 0x{:08X})", hdr.signature));

    if (hdr.offset < sizeof(PFIHEADER))
        throw std::runtime_error(std::format(
            "pfi_blob: offset_to_flash ({}) < sizeof(PFIHEADER) ({})",
            hdr.offset, sizeof(PFIHEADER)));

    if (hdr.sysinfo.size != SYSINFO_EXPECTED_SIZE)
        throw std::runtime_error(std::format(
            "pfi_blob: SYSTEMINFO.size mismatch (expected {}, got {})",
            SYSINFO_EXPECTED_SIZE, hdr.sysinfo.size));

    *hdr_out = hdr;
}

SYSTEMINFO pfi_read_sysinfo_blob(const std::uint8_t* data, std::size_t len)
{
    PFIHEADER hdr{};
    read_pfi_header_blob(data, len, &hdr);
    return hdr.sysinfo;
}

PfiInfo pfi_load_blob(Bus& bus, const std::uint8_t* data, std::size_t len)
{
    PFIHEADER hdr{};
    read_pfi_header_blob(data, len, &hdr);

    static constexpr uint32_t FLASH_BASE = Bus::FLASH_BASE;
    if (hdr.sysinfo.pffs_end <= FLASH_BASE)
        throw std::runtime_error(std::format(
            "pfi_load_blob: SYSTEMINFO.pffs_end (0x{:X}) <= FLASH_BASE (0x{:X})",
            hdr.sysinfo.pffs_end, FLASH_BASE));
    uint32_t flash_size = hdr.sysinfo.pffs_end - FLASH_BASE;

    if (hdr.offset > len)
        throw std::runtime_error(std::format(
            "pfi_load_blob: flash offset ({}) past blob end ({})",
            hdr.offset, len));

    std::size_t available = len - hdr.offset;
    std::size_t to_load   = std::min<std::size_t>(flash_size, available);
    if (to_load == 0)
        throw std::runtime_error("pfi_load_blob: flash image is empty");

    bus.load_flash(0u, data + hdr.offset, to_load);

    std::fprintf(stderr,
                 "Loaded PFI blob: flash=0x%X bytes, "
                 "sys_clock=%u Hz, hard_ver=0x%04X, bios_ver=0x%04X\n",
                 static_cast<unsigned>(to_load),
                 hdr.sysinfo.sys_clock,
                 hdr.sysinfo.hard_ver,
                 hdr.sysinfo.bios_ver);

    return PfiInfo{hdr.sysinfo, to_load, hdr.offset};
}

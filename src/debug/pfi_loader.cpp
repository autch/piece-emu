#include "pfi_loader.hpp"
#include "bus.hpp"
#include <cstdio>
#include <cstring>
#include <format>
#include <stdexcept>
#include <vector>

// SYSTEMINFO.size must equal sizeof(SYSTEMINFO) == 32.
static constexpr uint16_t SYSINFO_EXPECTED_SIZE = 32;

PfiInfo pfi_load(Bus& bus, const std::string& path)
{
    // ---- open file ----
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp)
        throw std::runtime_error(std::format("pfi_load: cannot open '{}'", path));

    // ---- read header ----
    // PFIHEADER is 40 bytes, no padding (see pfi_format.h).
    // Both host and P/ECE target are little-endian, so a direct fread is safe.
    PFIHEADER hdr{};
    if (std::fread(&hdr, sizeof(PFIHEADER), 1, fp) != 1) {
        std::fclose(fp);
        throw std::runtime_error(std::format("pfi_load: '{}' too short to read header", path));
    }

    // ---- validate signature ----
    // On-disk bytes: '1','I','F','P' → read as LE uint32 = 0x50464931.
    //   bits 31-24: 'P'=0x50, bits 23-16: 'F'=0x46, bits 15-8: 'I'=0x49
    //   bits  7- 0: version byte ('1'=0x31)  ← masked out with 0xffffff00
    if ((hdr.signature & 0xffffff00u) !=
        ((uint32_t)'P' << 24 | (uint32_t)'F' << 16 | (uint32_t)'I' << 8)) {
        std::fclose(fp);
        throw std::runtime_error(std::format(
            "pfi_load: '{}' invalid PFI signature (got 0x{:08X})", path, hdr.signature));
    }

    // ---- validate offset ----
    if (hdr.offset < sizeof(PFIHEADER)) {
        std::fclose(fp);
        throw std::runtime_error(std::format(
            "pfi_load: '{}' offset_to_flash ({}) < sizeof(PFIHEADER) ({})",
            path, hdr.offset, sizeof(PFIHEADER)));
    }

    // ---- validate SYSTEMINFO ----
    if (hdr.sysinfo.size != SYSINFO_EXPECTED_SIZE) {
        std::fclose(fp);
        throw std::runtime_error(std::format(
            "pfi_load: '{}' SYSTEMINFO.size mismatch (expected {}, got {})",
            path, SYSINFO_EXPECTED_SIZE, hdr.sysinfo.size));
    }

    // ---- determine flash image size ----
    // pffs_end is FLASH_TOP + flash_size.  Round up to next power-of-two.
    static constexpr uint32_t FLASH_BASE = Bus::FLASH_BASE;
    if (hdr.sysinfo.pffs_end <= FLASH_BASE) {
        std::fclose(fp);
        throw std::runtime_error(std::format(
            "pfi_load: '{}' SYSTEMINFO.pffs_end (0x{:X}) <= FLASH_BASE (0x{:X})",
            path, hdr.sysinfo.pffs_end, FLASH_BASE));
    }
    uint32_t flash_size = hdr.sysinfo.pffs_end - FLASH_BASE;

    // ---- seek to flash image (uses stored offset, handles any alignment) ----
    if (std::fseek(fp, static_cast<long>(hdr.offset), SEEK_SET) != 0) {
        std::fclose(fp);
        throw std::runtime_error(std::format(
            "pfi_load: '{}' seek to flash image at offset {} failed",
            path, hdr.offset));
    }

    // ---- load flash image in 4 KB chunks ----
    static constexpr std::size_t CHUNK = 0x1000;
    std::vector<uint8_t> chunk(CHUNK);
    std::size_t total = 0;
    while (total < flash_size) {
        std::size_t want = std::min(CHUNK, static_cast<std::size_t>(flash_size) - total);
        std::size_t n    = std::fread(chunk.data(), 1, want, fp);
        if (n == 0) break;
        bus.load_flash(static_cast<uint32_t>(total), chunk.data(), n);
        total += n;
    }
    std::fclose(fp);

    if (total == 0)
        throw std::runtime_error(std::format("pfi_load: '{}' flash image is empty", path));

    std::fprintf(stderr, "Loaded PFI '%s': flash=0x%X bytes, "
                         "sys_clock=%u Hz, hard_ver=0x%04X, bios_ver=0x%04X\n",
                 path.c_str(), static_cast<unsigned>(total),
                 hdr.sysinfo.sys_clock, hdr.sysinfo.hard_ver, hdr.sysinfo.bios_ver);

    return PfiInfo{hdr.sysinfo, total};
}

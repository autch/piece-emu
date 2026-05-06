#include "pfi_loader.hpp"
#include "bus.hpp"
#include <cstdio>
#include <cstring>
#include <format>
#include <stdexcept>
#include <vector>

// SYSTEMINFO.size must equal sizeof(SYSTEMINFO) == 32.
static constexpr uint16_t SYSINFO_EXPECTED_SIZE = 32;

// ---- shared header reader ---------------------------------------------------

// Opens path, reads and validates the PFIHEADER, closes (or leaves open) the
// file.  On success fills *hdr_out; on failure throws std::runtime_error.
// If fp_out is non-null the file is left open and the handle returned via
// *fp_out; otherwise the file is closed before returning.
static void read_pfi_header(const std::string& path,
                             PFIHEADER* hdr_out,
                             std::FILE** fp_out = nullptr)
{
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp)
        throw std::runtime_error(std::format("pfi: cannot open '{}'", path));

    PFIHEADER hdr{};
    if (std::fread(&hdr, sizeof(PFIHEADER), 1, fp) != 1) {
        std::fclose(fp);
        throw std::runtime_error(
            std::format("pfi: '{}' too short to read header", path));
    }

    // Signature check: on-disk bytes '1','I','F','P' → LE uint32 = 0x50464931.
    // Ignore the version byte (bits 7-0) with the 0xffffff00 mask.
    if ((hdr.signature & 0xffffff00u) !=
        ((uint32_t)'P' << 24 | (uint32_t)'F' << 16 | (uint32_t)'I' << 8)) {
        std::fclose(fp);
        throw std::runtime_error(std::format(
            "pfi: '{}' invalid PFI signature (got 0x{:08X})", path, hdr.signature));
    }

    if (hdr.offset < sizeof(PFIHEADER)) {
        std::fclose(fp);
        throw std::runtime_error(std::format(
            "pfi: '{}' offset_to_flash ({}) < sizeof(PFIHEADER) ({})",
            path, hdr.offset, sizeof(PFIHEADER)));
    }

    if (hdr.sysinfo.size != SYSINFO_EXPECTED_SIZE) {
        std::fclose(fp);
        throw std::runtime_error(std::format(
            "pfi: '{}' SYSTEMINFO.size mismatch (expected {}, got {})",
            path, SYSINFO_EXPECTED_SIZE, hdr.sysinfo.size));
    }

    *hdr_out = hdr;
    if (fp_out)
        *fp_out = fp;
    else
        std::fclose(fp);
}

// ---- pfi_read_sysinfo -------------------------------------------------------

SYSTEMINFO pfi_read_sysinfo(const std::string& path)
{
    PFIHEADER hdr{};
    read_pfi_header(path, &hdr);
    return hdr.sysinfo;
}

// ---- pfi_load ---------------------------------------------------------------

PfiInfo pfi_load(Bus& bus, const std::string& path)
{
    PFIHEADER  hdr{};
    std::FILE* fp = nullptr;
    read_pfi_header(path, &hdr, &fp);

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

    return PfiInfo{hdr.sysinfo, total, hdr.offset};
}

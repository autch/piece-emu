#pragma once
#include <cstdint>
#include <string>

class Bus;

// ============================================================================
// PFI (P/ECE Flash Image) loader
//
// PFI file layout (all fields little-endian):
//   offset 0:  signature[4]       — bytes {'1','I','F','P'} (LE encoding of 'PFI1')
//   offset 4:  offset_to_flash    — uint32_t byte offset within file to flash image
//   offset 8:  sys_info           — SYSTEMINFO (32 bytes)
//   offset 40: flash image        — starts at offset_to_flash from file start
//
// The flash image is loaded into bus flash memory starting at offset 0
// (CPU address 0xC00000).
// ============================================================================

struct PfiSysInfo {
    uint16_t size;          // struct size (must == 32)
    uint16_t hard_ver;      // hardware version
    uint16_t bios_ver;      // BIOS version
    uint16_t bios_date;     // BIOS date YYYY(7):MM(4):DD(5)
    uint32_t sys_clock;     // system clock in Hz
    uint16_t vdde_voltage;  // VDDE peripheral voltage in mV
    uint16_t resv1;
    uint32_t sram_top;      // SRAM start address (usually 0x100000)
    uint32_t sram_end;      // SRAM end address + 1 (usually 0x140000)
    uint32_t pffs_top;      // P/ECE Flash File System start address
    uint32_t pffs_end;      // PFFS end address (= FLASH_BASE + flash_size)
};

struct PfiInfo {
    PfiSysInfo sys_info;
    std::size_t flash_size; // bytes loaded into bus flash
};

// Load a PFI flash image into bus flash memory.
// Validates the header signature and SYSTEMINFO size field.
// Throws std::runtime_error on failure.
// Returns a PfiInfo struct with system information parsed from the header.
PfiInfo pfi_load(Bus& bus, const std::string& path);

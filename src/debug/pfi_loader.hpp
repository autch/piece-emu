#pragma once
#include "pfi_format.h"   // SYSTEMINFO, PFIHEADER, FLASH_TOP, PFI_IMAGE_OFFSET
#include <cstddef>
#include <string>

class Bus;

// ============================================================================
// PFI (P/ECE Flash Image) loader
//
// Reads a PFI file from disk and loads the flash image into bus flash memory.
// The header format is defined in pfi_format.h (shared with the host tools).
// Throws std::runtime_error on any failure.
// ============================================================================

struct PfiInfo {
    SYSTEMINFO  sys_info;   // hardware info parsed from the PFI header
    std::size_t flash_size; // bytes loaded into bus flash
};

// Load a PFI flash image into bus flash memory.
// Validates the header signature and SYSTEMINFO.size field.
// Throws std::runtime_error on failure.
PfiInfo pfi_load(Bus& bus, const std::string& path);

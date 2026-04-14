#ifndef PFI_FORMAT_H
#define PFI_FORMAT_H

// Shared P/ECE Flash Image (PFI) on-disk format definitions.
//
// Pure C header — safe to include from C and C++.
// Used by:
//   src/tools/   — mkpfi, pfar, ripper  (host-side PFI file manipulation)
//   src/debug/   — pfi_loader           (emulator PFI loader)

#include <stdint.h>

// Flash ROM base address in the S1C33209 address space.
#ifndef FLASH_TOP
#  define FLASH_TOP 0x0c00000u
#endif

// ---------------------------------------------------------------------------
// P/ECE hardware info block (SYSTEMINFO, from P/ECE SDK piece.h).
// 32 bytes total, no internal padding (all fields are naturally aligned).
// ---------------------------------------------------------------------------
typedef struct SYSTEMINFO
{
    uint16_t size;          //  0  sizeof(SYSTEMINFO) — must be 32
    uint16_t hard_ver;      //  2  hardware version (major<<8 | minor)
    uint16_t bios_ver;      //  4  BIOS version     (major<<8 | minor)
    uint16_t bios_date;     //  6  BIOS date: YY(7):MM(4):DD(5)
    uint32_t sys_clock;     //  8  system clock (Hz)
    uint16_t vdde_voltage;  // 12  VDDE peripheral voltage (mV)
    uint16_t resv1;         // 14  reserved
    uint32_t sram_top;      // 16  SRAM start address
    uint32_t sram_end;      // 20  SRAM end address + 1
    uint32_t pffs_top;      // 24  PFFS start address
    uint32_t pffs_end;      // 28  PFFS end address
} SYSTEMINFO;

// ---------------------------------------------------------------------------
// PFI file header (40 bytes total, no padding).
// All multi-byte fields are little-endian.
// ---------------------------------------------------------------------------
typedef struct PFIHEADER
{
    uint32_t   signature;  // 0x50464931 (LE) — on-disk bytes: '1','I','F','P'
    uint32_t   offset;     // byte offset from file start to the flash image
    SYSTEMINFO sysinfo;    // hardware info (32 bytes)
    // flash image data begins at byte `offset`
} PFIHEADER;

// Minimum valid `offset` field: sizeof(PFIHEADER) == 40 bytes.
// New files written by this project use PFI_IMAGE_OFFSET (48) to align the
// flash image to a 16-byte boundary.  Readers must use the stored `offset`
// value and must not assume any particular alignment.
#define PFI_ALIGN        16u
#define PFI_IMAGE_OFFSET (((sizeof(PFIHEADER) + (PFI_ALIGN - 1u)) / PFI_ALIGN) * PFI_ALIGN)

#endif // PFI_FORMAT_H

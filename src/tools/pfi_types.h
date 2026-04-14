#ifndef PFI_TYPES_H
#define PFI_TYPES_H

// PFI/PFFS on-disk format types — re-exported from the shared format header.
#include "pfi_format.h"
#include "pffs_types.h"

// PFI in-memory handle used by the host-side tools (mkpfi, pfar, etc.).
// Not used by the emulator.
typedef struct PFI
{
    PFIHEADER        header;
    uint8_t         *buffer; // flash image data (malloc'd)
    uint32_t         size;   // size in bytes of buffer
    pffsMASTERBLOCK *msb;    // pointer into buffer at pffs_top
} PFI;

#endif

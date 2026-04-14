/** @file pfi.c
 * PFI (P/ECE Flash Image) container operations.
 *
 * Opens a PFI file from disk, allocates the flash buffer, and provides
 * save/close helpers.  PFFS filesystem manipulation lives in pffs.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pfi.h"
#include "pffs.h"

void PFIInit(PFI *pfi)
{
    memset(pfi, 0, sizeof(PFI));
}

void PFIExit(PFI *pfi)
{
    PFIClose(pfi);
}

int PFIOpen(PFI *pfi, const char *filename)
{
    int       r = 0;
    uint8_t  *p;
    FILE     *fp;
    uint32_t  flashsize;

    fp = fopen(filename, "rb");
    if (fp && fread(&pfi->header, sizeof(PFIHEADER), 1, fp))
    {
        // Multi-char literal 'PFI1' on GCC/LE = 0x50464931: 'P'@bits24,'F'@bits16,'I'@bits8,ver@bits0.
        // Mask bits 0-7 (version) and compare the 'PFI' portion.
        if ((pfi->header.signature & 0xffffff00u) ==
            ((uint32_t)'P' << 24 | (uint32_t)'F' << 16 | (uint32_t)'I' << 8))
        {
            fseek(fp, (long)pfi->header.offset, SEEK_SET);

            flashsize = pfi->header.sysinfo.pffs_end - FLASH_TOP;
            for (pfi->size = 1; pfi->size < flashsize; pfi->size <<= 1)
                ;
            pfi->buffer = malloc(pfi->size);
            if (pfi->buffer)
            {
                for (p = pfi->buffer; p < pfi->buffer + pfi->size; p += 1 << 12)
                    fread(p, 1, 1 << 12, fp);
                r = 1;
            }
        }
    }
    if (fp) fclose(fp);
    PFFSLoadMasterBlock(pfi);

    return r;
}

void PFIClose(PFI *pfi)
{
    if (pfi->buffer)
    {
        free(pfi->buffer);
        pfi->buffer = NULL;
    }
}

int PFISave(PFI *pfi, const char *filename)
{
    uint8_t pad[PFI_IMAGE_OFFSET - sizeof(PFIHEADER)];
    FILE *fp = fopen(filename, "wb");
    if (!fp) return 0;
    pfi->header.offset = PFI_IMAGE_OFFSET;
    fwrite(&pfi->header, sizeof(PFIHEADER), 1, fp);
    memset(pad, 0xff, sizeof(pad));
    fwrite(pad, 1, sizeof(pad), fp);
    fwrite(pfi->buffer, 1, pfi->size, fp);
    fclose(fp);
    return 1;
}

uint8_t *PFIGetPFFSTop(PFI *pfi)
{
    return pfi->buffer + (pfi->header.sysinfo.pffs_top - FLASH_TOP);
}

uint32_t PFIGetPFFSMaxFAT(PFI *pfi)
{
    uint32_t maxfat;
    SYSTEMINFO *si = &pfi->header.sysinfo;

    maxfat = (si->pffs_end - si->pffs_top) >> 12;
    if (maxfat > MAXFAT) maxfat = MAXFAT;
    return maxfat;
}

void PFIDumpSystemInfo(PFI *pfi)
{
    SYSTEMINFO *si = &pfi->header.sysinfo;
    printf("P/ECE H/W v%d.%02d, BIOS v%d.%02d (%d.%02d.%02d), "
           "running at %5.3f MHz, %5.3f V\n",
           si->hard_ver >> 8, si->hard_ver & 255,
           si->bios_ver >> 8, si->bios_ver & 255,
           2000 + (si->bios_date >> 9), (si->bios_date >> 5) & 15,
           si->bios_date & 31,
           si->sys_clock / 1e6, si->vdde_voltage / 1e3);
    printf("SRAM addr 0x%08x - 0x%08x, %4d KiB\n",
           si->sram_top, si->sram_end - 1,
           (si->sram_end - si->sram_top) >> 10);
    printf("PFFS addr 0x%08x - 0x%08x, %4d KiB\n",
           si->pffs_top, si->pffs_end - 1,
           (si->pffs_end - si->pffs_top) >> 10);
}

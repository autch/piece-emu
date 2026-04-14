/** @file pffs.c
 * PFFS (P/ECE Flash File System) operations.
 *
 * Manipulates the PFFS filesystem inside a PFI flash image buffer that has
 * already been loaded by pfi.c.  Call PFIOpen() before any PFFS function.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pffs.h"
#include "pfi.h"

static int        CheckFileName(const char *name);
static DIRECTORY *FindNextDirEntry(PFI *pfi, DIRECTORY **p);
static uint32_t   GetFreeFAT(PFI *pfi, FAT *fat, int start);
static DIRECTORY *FindFreeDir(PFI *pfi);
static int        WriteFileToSectors(PFI *pfi, DIRECTORY *dir,
                                     const char *filename, int nsectors);
static int        AllocateSectors(PFI *pfi, DIRECTORY *dir,
                                  int nsectors, int old_nsectors);

void PFFSLoadMasterBlock(PFI *pfi)
{
    pfi->msb = (pffsMASTERBLOCK *)PFIGetPFFSTop(pfi);
}

// Valid PFFS name: [a-z0-9_]{1,8}([.][a-z0-9_]{1,3})?
static int CheckFileName(const char *name)
{
    int part;
    for (part = 0; part < 2; part++)
    {
        int  length = 0;
        char c;
        while (1)
        {
            c = *name++;
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_'))
                break;
            length++;
        }
        if (!part)
        {
            if (length == 0 || length > 8) return 1;
            if (!c) return 0;
            if (c != '.') return 2;
        }
        else
        {
            if (length == 0 || length > 3) return 3;
            if (!c) return 0;
        }
    }
    return 4;
}

// Return the next in-use directory entry, advancing *p.
static DIRECTORY *FindNextDirEntry(PFI *pfi, DIRECTORY **p)
{
    DIRECTORY *d;
    uint8_t    c;
    while (1)
    {
        d = (*p)++;
        if (d - pfi->msb->dir >= MAXDIR) return NULL;
        c = (uint8_t)d->name[0];
        if (c && c != 0xff) return d;
    }
}

// Find the directory entry matching name, or NULL.
DIRECTORY *PFFSFindFile(PFI *pfi, const char *name)
{
    DIRECTORY *work = pfi->msb->dir;
    DIRECTORY *d;
    if (CheckFileName(name)) return NULL;
    while ((d = FindNextDirEntry(pfi, &work)))
        if (!strncmp((const char *)d->name, name, 24)) return d;
    return NULL;
}

// Return the pointer to sector nSector of file dir.
uint8_t *PFFSGetFilesNthSector(PFI *pfi, DIRECTORY *dir, int sector)
{
    int c = dir->chain;
    while (sector--) c = pfi->msb->fat[c].chain;
    return PFIGetPFFSTop(pfi) + ((uint32_t)c << 12);
}

// Convert a filesystem-relative sector number to a pointer.
uint8_t *PFFSSectorToPointer(PFI *pfi, int sector)
{
    return PFIGetPFFSTop(pfi) + ((uint32_t)sector << 12);
}

// Find the first free FAT entry at index >= start.
static uint32_t GetFreeFAT(PFI *pfi, FAT *fat, int start)
{
    uint32_t maxfat = PFIGetPFFSMaxFAT(pfi);
    for (; (uint32_t)start < maxfat; start++)
        if (fat[start].chain == FAT_FREE) return (uint32_t)start;
    return (uint32_t)-1;
}

uint32_t PFFSDirCount(PFI *pfi)
{
    DIRECTORY *work = pfi->msb->dir;
    uint32_t   n = 0;
    while (FindNextDirEntry(pfi, &work)) n++;
    return n;
}

DIRECTORY *PFFSDir(PFI *pfi, int index)
{
    DIRECTORY *work = pfi->msb->dir;
    DIRECTORY *d = NULL;
    do {
        if (!(d = FindNextDirEntry(pfi, &work))) return NULL;
    } while (index-- > 0);
    return d;
}

uint32_t PFFSFree(PFI *pfi)
{
    uint32_t n = 0, i, maxfat = PFIGetPFFSMaxFAT(pfi);
    for (i = 0; i < maxfat; i++)
        if (pfi->msb->fat[i].chain == FAT_FREE) n++;
    return n;
}

void PFFSDumpDirEntries(PFI *pfi)
{
    uint32_t   i, count = PFFSDirCount(pfi);
    int        total = 0;

    printf("idx: %-24s attr chain %10s\n", "filename", "size");
    for (i = 0; i < count; i++)
    {
        DIRECTORY *d = PFFSDir(pfi, (int)i);
        if (!d) break;
        printf("%3td: %-24s\t%02x  %04x\t%10u\n",
               d - pfi->msb->dir, (const char *)d->name,
               d->attr, d->chain, d->size);
        total += (int)d->size;
    }
    for (i = 0; i < 51; i++) putchar('-');
    printf("\n%3u files %30s %10d bytes\n", count, "", total);
    printf("%u sectors (%u bytes) free\n",
           PFFSFree(pfi), PFFSFree(pfi) << 12);
}

int PFFSExtractFile(PFI *pfi, const char *pffs_name, const char *disk_name)
{
    DIRECTORY *dir;
    uint32_t   remaining;
    int        sector;
    FILE      *fp;

    dir = PFFSFindFile(pfi, pffs_name);
    if (!dir) return 0;

    if (!disk_name) disk_name = pffs_name;

    fp = fopen(disk_name, "wb");
    if (!fp) return 0;

    remaining = dir->size;
    sector    = dir->chain;
    while (remaining && sector != FAT_END)
    {
        uint32_t to_write = (remaining > 4096) ? 4096 : remaining;
        uint32_t written  = (uint32_t)fwrite(PFFSSectorToPointer(pfi, sector),
                                              1, to_write, fp);
        remaining -= written;
        sector = pfi->msb->fat[sector].chain;
    }
    fclose(fp);
    return 1;
}

int PFFSDeleteFile(PFI *pfi, const char *pffs_name)
{
    pffsMASTERBLOCK  new_msb;
    pffsMASTERBLOCK *old_msb = pfi->msb;
    DIRECTORY       *dir;
    int              sector, next;

    new_msb  = *pfi->msb;
    pfi->msb = &new_msb;

    dir = PFFSFindFile(pfi, pffs_name);
    if (!dir) { pfi->msb = old_msb; return 0; }

    sector = dir->chain;
    while (sector != FAT_END)
    {
        next = new_msb.fat[sector].chain;
        new_msb.fat[sector].chain = FAT_FREE;
        sector = next;
    }
    memset(dir, 0xff, sizeof(DIRECTORY));
    *old_msb = new_msb;
    pfi->msb = old_msb;
    return 1;
}

static DIRECTORY *FindFreeDir(PFI *pfi)
{
    DIRECTORY *dir = pfi->msb->dir;
    int        i;
    for (i = 0; i < MAXDIR; i++, dir++)
        if ((uint8_t)dir->name[0] == 0xff) return dir;
    return NULL;
}

int PFFSAddFile(PFI *pfi, const char *filename)
{
    FILE            *fp;
    uint32_t         size;
    pffsMASTERBLOCK  new_msb;
    pffsMASTERBLOCK *old_msb = pfi->msb;
    int              nsectors, old_nsectors, r = 0;
    DIRECTORY       *dir;

    // Strip any leading directory components: PFFS only stores bare filenames.
    // fopen() still receives the original path so relative/absolute paths work.
    const char *pffs_name = strrchr(filename, '/');
    pffs_name = pffs_name ? pffs_name + 1 : filename;

    if (CheckFileName(pffs_name))
    {
        fprintf(stderr, "Invalid filename for pffs: %s\n", pffs_name);
        return 0;
    }

    fp = fopen(filename, "rb");
    if (!fp)
    {
        fprintf(stderr, "Cannot open disk file: %s\n", filename);
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    size = (uint32_t)ftell(fp);
    fclose(fp);

    nsectors     = (int)((size + 4095) >> 12);
    old_nsectors = 0;

    new_msb  = *pfi->msb;
    pfi->msb = &new_msb;

    dir = PFFSFindFile(pfi, pffs_name);
    if (!dir)
    {
        dir = FindFreeDir(pfi);
        if (!dir) goto done;
        memset(dir, 0, sizeof(DIRECTORY));
        strncpy((char *)dir->name, pffs_name, sizeof(dir->name) - 1);
        dir->chain = FAT_FREE;
    }
    else
    {
        old_nsectors = (int)((dir->size + 4095) >> 12);
    }
    dir->size = size;

    if (!AllocateSectors(pfi, dir, nsectors, old_nsectors))
    {
        fprintf(stderr, "Unable to allocate sectors for %s\n", pffs_name);
        goto done;
    }
    if (!WriteFileToSectors(pfi, dir, filename, nsectors))
    {
        fprintf(stderr, "Cannot write file into pffs: %s\n", pffs_name);
        goto done;
    }

    r        = 1;
    *old_msb = new_msb;

done:
    pfi->msb = old_msb;
    return r;
}

static int WriteFileToSectors(PFI *pfi, DIRECTORY *dir,
                               const char *filename, int nsectors)
{
    FILE *fp = fopen(filename, "rb");
    int   sector, i;
    if (!fp) return 0;
    sector = dir->chain;
    for (i = 0; i < nsectors; i++)
    {
        fread(PFFSSectorToPointer(pfi, sector), 1, 4096, fp);
        sector = pfi->msb->fat[sector].chain;
    }
    fclose(fp);
    return 1;
}

static int AllocateSectors(PFI *pfi, DIRECTORY *dir,
                            int nsectors, int old_nsectors)
{
    uint16_t *chain = &dir->chain;
    int       sector, n, nn = 0;
    FAT      *fat   = pfi->msb->fat;

    for (sector = 0; sector < nsectors; sector++)
    {
        n = *chain;
        if (n == FAT_FREE || n == FAT_END)
        {
            n = (int)GetFreeFAT(pfi, fat, nn);
            if (n < 0) return 0;
            *chain = (uint16_t)n;
            nn = n + 1;
        }
        chain = &fat[n].chain;
    }

    nn     = *chain;
    *chain = FAT_END;

    // Release any extra sectors from an old (larger) version of the file.
    for (; sector < old_nsectors; sector++)
    {
        chain = &fat[nn].chain;
        nn    = *chain;
        *chain = FAT_FREE;
        if (nn == FAT_FREE || nn == FAT_END) break;
    }

    return 1;
}

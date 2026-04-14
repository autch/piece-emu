/** @file mkpfi.c
 * Generate a PFI flash image from a raw P/ECE all.bin kernel image.
 *
 * Usage: mkpfi [-512kb|-2mb] all.bin [piece.pfi]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pffs.h"
#include "pfi.h"

#define KERNEL_SIGNATURE "Piece Kernel Program"

// Default SYSTEMINFO values for a standard P/ECE unit.
// pffs_end must be overridden to the correct flash size before use.
static const SYSTEMINFO si_default =
{
    0x0020,      // size
    0x0100,      // hard_ver
    0x011d,      // bios_ver
    0x0aa7,      // bios_date
    0x016e3600,  // sys_clock  (24 MHz)
    0x0ce4,      // vdde_voltage
    0x0000,      // resv1
    0x0100000,   // sram_top
    0x0140000,   // sram_end
    0x0c28000,   // pffs_top
    0x0c00000,   // pffs_end  (placeholder — overwritten below)
};

static int usage(void)
{
    fprintf(stderr, "usage: mkpfi [-512kb|-2mb] all.bin [piece.pfi]\n");
    return -1;
}

static int run(const char *src, const char *dst, const PFIHEADER *hdr);

int main(int argc, char **argv)
{
    const char *src = NULL, *dst = NULL;
    PFIHEADER   hdr;

    // Signature: bytes 'P','F','I','1' stored in little-endian order
    hdr.signature = 0x31u | (0x49u << 8) | (0x46u << 16) | (0x50u << 24);
    hdr.offset    = PFI_IMAGE_OFFSET;
    hdr.sysinfo   = si_default;
    // Default to 512 KB PFFS
    hdr.sysinfo.pffs_end = si_default.pffs_end + (512u << 10);

    (void)argc;
    while (*++argv)
    {
        if (strcmp(*argv, "-512kb") == 0)
            hdr.sysinfo.pffs_end = si_default.pffs_end + (512u << 10);
        else if (strcmp(*argv, "-2mb") == 0)
            hdr.sysinfo.pffs_end = si_default.pffs_end + (2048u << 10);
        else if (!src && **argv != '-')
            src = *argv;
        else if (src && !dst && **argv != '-')
            dst = *argv;
    }

    if (!src)
        return usage();
    if (!dst)
        dst = "piece.pfi";

    return run(src, dst, &hdr);
}

static int run(const char *src, const char *dst, const PFIHEADER *hdr)
{
    char         buf[4096];
    // P/ECE reset vector stored little-endian: address 0x00c02004
    const uint8_t boot_addr[4] = { 0x04, 0x20, 0xc0, 0x00 };
    FILE        *fpi, *fpo;
    int          n, nsectors;

    fpi = fopen(src, "rb");
    if (!fpi)
    {
        fprintf(stderr, "mkpfi: cannot open %s\n", src);
        return usage();
    }
    // Verify all.bin kernel signature at offset 8
    fread(buf, 1, 8 + (int)strlen(KERNEL_SIGNATURE), fpi);
    if (strncmp(buf + 8, KERNEL_SIGNATURE, strlen(KERNEL_SIGNATURE)))
    {
        fprintf(stderr, "mkpfi: %s is not a valid all.bin image\n", src);
        fclose(fpi);
        return usage();
    }
    fseek(fpi, 0, SEEK_SET);

    fpo = fopen(dst, "wb");
    if (!fpo)
    {
        fprintf(stderr, "mkpfi: cannot create %s\n", dst);
        fclose(fpi);
        return usage();
    }
    printf("creating PFI %s from all.bin image %s\n", dst, src);

    fwrite(hdr, 1, sizeof(PFIHEADER), fpo);
    // Pad header to PFI_IMAGE_OFFSET (16-byte boundary) with 0xff.
    {
        uint8_t pad[PFI_IMAGE_OFFSET - sizeof(PFIHEADER)];
        memset(pad, 0xff, sizeof(pad));
        fwrite(pad, 1, sizeof(pad), fpo);
    }

    // Sector 0: boot jump vector + padding
    memset(buf, 0xff, 4096);
    memcpy(buf, boot_addr, 4);
    fwrite(buf, 1, 4096, fpo);

    // Sector 1: empty padding
    memset(buf, 0xff, 4096);
    fwrite(buf, 1, 4096, fpo);

    // Remaining sectors: copy from source image
    nsectors = (int)((hdr->sysinfo.pffs_end - FLASH_TOP) >> 12) - 2;
    while ((n = (int)fread(buf, 1, 4096, fpi)) > 0)
    {
        fwrite(buf, 1, (size_t)n, fpo);
        nsectors--;
    }

    // Pad remaining sectors with 0xff
    memset(buf, 0xff, 4096);
    while (nsectors-- > 0)
        fwrite(buf, 1, 4096, fpo);

    fclose(fpo);
    fclose(fpi);
    return 0;
}

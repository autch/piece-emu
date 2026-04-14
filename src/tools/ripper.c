/** @file ripper.c
 * Read the P/ECE flash image via USB and save it as piece.pfi.
 *
 * Connects to a P/ECE device, queries hardware info, reads the full flash
 * region, and writes a PFI file to disk.
 *
 * Usage: ripper [output.pfi]
 *   Default output filename: piece.pfi
 *
 * Original by nsawa / autch (Windows/pieceif.lib).
 * Linux port: uses pieceif_libusb (libusb-1.0); no Windows dependencies.
 *
 * Note: requires read permission for the USB device.  Either run as root or
 * add a udev rule granting access to VID 0x0e19 / PID 0x1000.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pieceif.h"
#include "pfi.h"      // PFIHEADER, FLASH_TOP

#define READ_UNIT  0x1000u   // 4 KiB per USB read

static int  die(const char *msg);
static void dump_sysinfo(const SYSTEMINFO *si);
static int  read_flash(const SYSTEMINFO *si, const char *outpath);

int main(int argc, char **argv)
{
    SYSTEMINFO si;
    const char *outpath = "piece.pfi";
    int r;

    if (argc >= 2 && argv[1][0] != '-')
        outpath = argv[1];

    if (ismInit())
        return die("cannot connect to P/ECE — is it plugged in?");

    r = ismGetVersion(&si, 1);
    if (r == PIECE_INVALID_VERSION) {
        ismExit();
        return die("P/ECE kernel is too old or unrecognised");
    }

    dump_sysinfo(&si);
    r = read_flash(&si, outpath);

    ismExit();
    return r;
}

static int die(const char *msg)
{
    fprintf(stderr, "ripper: %s\n", msg);
    return 1;
}

static void dump_sysinfo(const SYSTEMINFO *si)
{
    printf("P/ECE H/W v%d.%02d, BIOS v%d.%02d (%d.%02d.%02d), "
           "running at %5.3f MHz, %5.3f V\n",
           si->hard_ver >> 8,  si->hard_ver & 0xff,
           si->bios_ver >> 8,  si->bios_ver & 0xff,
           2000 + (si->bios_date >> 9),
           (si->bios_date >> 5) & 0xf,
           si->bios_date & 0x1f,
           si->sys_clock / 1e6,
           si->vdde_voltage / 1e3);
    printf("SRAM addr 0x%08x - 0x%08x, %4u KiB\n",
           si->sram_top, si->sram_end - 1,
           (si->sram_end - si->sram_top) >> 10);
    printf("PFFS addr 0x%08x - 0x%08x, %4u KiB\n",
           si->pffs_top, si->pffs_end - 1,
           (si->pffs_end - si->pffs_top) >> 10);
}

static int read_flash(const SYSTEMINFO *si, const char *outpath)
{
    PFIHEADER   hdr;
    FILE       *fp;
    uint8_t     buf[READ_UNIT];
    uint32_t    flash_bytes, p;
    unsigned    tick = 0;

    // Round flash size up to the next power of two.
    flash_bytes = si->pffs_end - FLASH_TOP;
    {
        uint32_t size;
        for (size = 1; size < flash_bytes; size <<= 1)
            ;
        flash_bytes = size;
    }

    // Build PFI header: signature 'PFI1' in little-endian.
    hdr.signature = 0x31u | (0x49u << 8) | (0x46u << 16) | (0x50u << 24);
    hdr.offset    = PFI_IMAGE_OFFSET;
    hdr.sysinfo   = *si;

    fp = fopen(outpath, "wb");
    if (!fp) return die("cannot open output file for writing");

    fwrite(&hdr, sizeof(PFIHEADER), 1, fp);
    // Pad header to PFI_IMAGE_OFFSET (16-byte boundary) with 0xff.
    {
        uint8_t pad[PFI_IMAGE_OFFSET - sizeof(PFIHEADER)];
        memset(pad, 0xff, sizeof(pad));
        fwrite(pad, 1, sizeof(pad), fp);
    }

    printf("Reading %u KiB of flash to %s", flash_bytes >> 10, outpath);
    fflush(stdout);

    for (p = FLASH_TOP; p < FLASH_TOP + flash_bytes; p += READ_UNIT) {
        memset(buf, 0xff, READ_UNIT);   // safe default if read fails
        ismReadMem(buf, p, READ_UNIT);
        fwrite(buf, 1, READ_UNIT, fp);

        if (++tick % 16 == 0) {         // progress dot every 64 KiB
            putchar('.');
            fflush(stdout);
        }
    }
    putchar('\n');

    fclose(fp);
    printf("Done — wrote %u bytes to %s\n",
           (unsigned)(sizeof(PFIHEADER) + flash_bytes), outpath);
    return 0;
}

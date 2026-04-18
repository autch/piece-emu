/** @file pieceif_libusb.c
 * P/ECE USB interface — libusb-1.0 port (Linux / macOS / Windows).
 *
 * Original pieceif.dll by MIO.H (OeRSTED), Copyright (C)2001 AQUAPLUS Co.,Ltd.
 * libusb rewrite by autch.
 *
 * Differences from the Windows DLL:
 *   - No DLL or shared-library infrastructure.
 *   - No multi-process mutex (HANDLE / CreateMutex / WaitForSingleObject).
 *     A single libusb context is shared across all device slots; the caller
 *     is expected to be a single-process, single-threaded CLI tool.
 *   - No Windows types (DWORD, LONG, HANDLE).  Replaced with stdint.h types.
 *
 * On Windows the device needs a WinUSB-compatible driver (install via Zadig
 * or libusbK) so libusb can claim the interface.
 *
 * Implemented functions:
 *   Transport  : ismInit, ismExit, ismInitEx, ismExitEx, ismSelect,
 *                ismCmdW, ismCmdR
 *   Memory     : ismGetVersion, ismReadMem, ismWriteMem, ismExec
 *   USBCOM     : ismUCOpen, ismUCClose, ismUCGetStat, ismUCWrite, ismUCRead
 *   App ctrl   : ismAppStop, ismAppStart  (needed by monitor / isd tools)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#  include <time.h>
#endif

#include <libusb.h>

#include "pieceif.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Sleep for n milliseconds.
#if defined(_WIN32)
#  define ismWait(n) Sleep((DWORD)(n))
#else
#  define ismWait(n) usleep((unsigned int)(n) * 1000u)
#endif

// Monotonic millisecond clock (wraps at ~49 days, sufficient for timeouts).
static uint32_t get_tick_ms(void)
{
#if defined(_WIN32)
    return (uint32_t)GetTickCount64();
#elif defined(__APPLE__) && !defined(CLOCK_MONOTONIC)
    // Pre-10.12 macOS fallback (modern SDKs provide clock_gettime).
    return (uint32_t)(clock() * 1000u / CLOCKS_PER_SEC);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
#endif
}

// ---------------------------------------------------------------------------
// USB device state
// ---------------------------------------------------------------------------

#define USB_ID_VENDOR   0x0e19u
#define USB_ID_PRODUCT  0x1000u

#define PIECE_ENDPOINT_IN  0x82u
#define PIECE_ENDPOINT_OUT 0x02u
#define BULK_TIMEOUT_MS    500

static libusb_context *g_ctx = NULL;

typedef struct {
    libusb_device_handle *udev;
} USBHANDLES;

static USBHANDLES g_handles[PIECE_MAX_DEVICES];
static USBHANDLES *g_cur = g_handles;

#define current_dev() (g_cur->udev)

// Length of last ismCmdR response (matches original dlen).
static volatile uint32_t g_dlen;

// Version info buffer filled by ismGetVersion (raw SYSTEMINFO bytes from device).
unsigned char _piece_version_info[32];
int           _piece_version = PIECE_INVALID_VERSION;

// ---------------------------------------------------------------------------
// Low-level bulk transfer wrappers
// ---------------------------------------------------------------------------

static int piece_write(const void *buf, size_t len, size_t *written)
{
    int actual = 0;
    int r = libusb_bulk_transfer(current_dev(), PIECE_ENDPOINT_OUT,
                                 (unsigned char *)buf, (int)len,
                                 &actual, BULK_TIMEOUT_MS);
    if (written) *written = (size_t)actual;
    return r;
}

static int piece_read(void *buf, size_t len, size_t *nread)
{
    int actual = 0;
    int r = libusb_bulk_transfer(current_dev(), PIECE_ENDPOINT_IN,
                                 (unsigned char *)buf, (int)len,
                                 &actual, BULK_TIMEOUT_MS);
    if (nread) *nread = (size_t)actual;
    return r;
}

// ---------------------------------------------------------------------------
// Transport: ismCmdW / ismCmdR
// ---------------------------------------------------------------------------

int ismCmdW(const void *cmd, unsigned cmd_len, const void *data, unsigned data_len)
{
    size_t n;
    piece_write(cmd, cmd_len, &n);
    if (data_len)
        piece_write(data, data_len, &n);
    else if (_piece_version < 0x107)
        ismWait(15);    // older firmware needs a brief pause
    return 0;
}

int ismCmdR(const void *cmd, unsigned cmd_len, void *resp, unsigned resp_len)
{
    size_t n;
    piece_write(cmd, cmd_len, &n);
    piece_read(resp, resp_len, &n);
    g_dlen = (uint32_t)n;
    return 0;
}

// ---------------------------------------------------------------------------
// Basic memory / execution interface
// ---------------------------------------------------------------------------

int ismReadMem(unsigned char *buf, unsigned long addr, unsigned len)
{
    uint8_t tmp[9];
    tmp[0] = 2;
    memcpy(tmp + 1, &addr, 4);  // little-endian; both host and target are LE
    memcpy(tmp + 5, &len,  4);
    return ismCmdR(tmp, 9, buf, len);
}

int ismWriteMem(const unsigned char *buf, unsigned long addr, unsigned len)
{
    uint8_t tmp[9];
    tmp[0] = 3;
    memcpy(tmp + 1, &addr, 4);
    memcpy(tmp + 5, &len,  4);
    return ismCmdW(tmp, 9, buf, len);
}

int ismExec(unsigned long addr)
{
    uint8_t tmp[5];
    tmp[0] = (addr & 1u) ? 8 : 1;
    memcpy(tmp + 1, &addr, 4);
    return ismCmdW(tmp, 5, NULL, 0);
}

int ismGetVersion(void *buf32, int renew)
{
    uint8_t tmp[4];
    int ver = PIECE_INVALID_VERSION;

    if (renew) {
        int retry = 0;
        _piece_version = ver;

        // First handshake: opcode 0x00, expect 8 bytes back.
        while (1) {
            tmp[0] = 0;
            if (ismCmdR(tmp, 1, _piece_version_info, 8))
                return _piece_version;
            if (g_dlen == 8) break;
            fprintf(stderr, "ismGetVersion: short read (%u bytes), resetting\n", g_dlen);
            libusb_reset_device(current_dev());
            if (++retry >= 3) return _piece_version;
        }

        ver = (int)(*(uint16_t *)(_piece_version_info + 4));  // bios_ver field

        // Second handshake for ver >= 21: fetch full SYSTEMINFO (24 or 32 bytes).
        if (ver >= 21) {
            tmp[0] = 0;
            tmp[1] = (*(int16_t *)(_piece_version_info + 4) >= 25) ? 32 : 24;
            if (ismCmdR(tmp, 2, _piece_version_info, tmp[1]))
                return _piece_version;
        }

        _piece_version = ver;
    }

    if (buf32)
        memcpy(buf32, _piece_version_info, 32);

    return _piece_version;
}

// ---------------------------------------------------------------------------
// Device open / close helpers
// ---------------------------------------------------------------------------

static void close_device(USBHANDLES *h)
{
    if (h->udev) {
        libusb_release_interface(h->udev, 0);
        libusb_reset_device(h->udev);
        libusb_close(h->udev);
        h->udev = NULL;
    }
}

// ---------------------------------------------------------------------------
// Init / exit
// ---------------------------------------------------------------------------

int ismInitEx(int devno, int waitn_ms)
{
    USBHANDLES *h;
    uint32_t    t0;

    if (devno >= PIECE_MAX_DEVICES) return ERR_PIECEIF_OVER_DEVICES;
    if (waitn_ms == PIECE_DEF_WAITN) waitn_ms = 500;

    if (!g_ctx) libusb_init(&g_ctx);

    h     = g_handles + devno;
    g_cur = h;
    t0    = get_tick_ms();

    while (1) {
        libusb_device **list;
        int found = -1;

        h->udev = NULL;
        libusb_get_device_list(g_ctx, &list);

        for (libusb_device **p = list; *p; p++) {
            struct libusb_device_descriptor desc;
            if (libusb_get_device_descriptor(*p, &desc) < 0) continue;
            if (desc.idVendor == USB_ID_VENDOR && desc.idProduct == USB_ID_PRODUCT) {
                if (++found == devno) {
                    libusb_open(*p, &h->udev);
                    break;
                }
            }
        }
        libusb_free_device_list(list, 1);

        if (found < 0) return 1;    // no P/ECE devices at all

        if (h->udev) {
            libusb_set_configuration(h->udev, 1);
            libusb_claim_interface(h->udev, 0);

            if (ismGetVersion(NULL, 1) != PIECE_INVALID_VERSION)
                return 0;

            close_device(h);
        }

        if ((get_tick_ms() - t0) > (uint32_t)waitn_ms) return 1;
        ismWait(10);
    }
}

int ismInit(void)
{
    return ismInitEx(0, PIECE_DEF_WAITN);
}

int ismExitEx(int devno)
{
    if (devno >= PIECE_MAX_DEVICES) return ERR_PIECEIF_OVER_DEVICES;
    close_device(g_handles + devno);
    return 0;
}

int ismExit(void)
{
    int i;
    for (i = 0; i < PIECE_MAX_DEVICES; i++)
        ismExitEx(i);
    if (g_ctx) {
        libusb_exit(g_ctx);
        g_ctx = NULL;
    }
    return 0;
}

int ismSelect(int devno)
{
    if (devno >= PIECE_MAX_DEVICES)    return ERR_PIECEIF_OVER_DEVICES;
    if (!g_handles[devno].udev)        return 1;
    g_cur = g_handles + devno;
    return 0;
}

// ---------------------------------------------------------------------------
// USBCOM serial-over-USB interface (for monitor tools)
// ---------------------------------------------------------------------------

int ismUCOpen(USBCOMS *ucs)
{
    uint8_t tmp = 13;
    if (_piece_version < 49) return ERR_PIECEIF_ILL_VER;
    return ismCmdR(&tmp, 1, ucs, 12 + 16);
}

int ismUCClose(void)
{
    uint8_t tmp = 14;
    if (_piece_version < 49) return ERR_PIECEIF_ILL_VER;
    return ismCmdR(&tmp, 1, &tmp, 1);
}

int ismUCWrite(const void *ptr, int len)
{
    uint8_t tmp = 11;
    if (_piece_version < 49) return ERR_PIECEIF_ILL_VER;
    return ismCmdW(&tmp, 1, ptr, (unsigned)len);
}

int ismUCRead(void *ptr, int len)
{
    uint8_t tmp = 10;
    if (_piece_version < 49) return ERR_PIECEIF_ILL_VER;
    return ismCmdR(&tmp, 1, ptr, (unsigned)len);
}

int ismUCGetStat(USBCOMS *ucs)
{
    uint8_t tmp = 12;
    if (_piece_version < 49) return ERR_PIECEIF_ILL_VER;
    return ismCmdR(&tmp, 1, ucs, 12);
}

// ---------------------------------------------------------------------------
// Application control (needed by monitor / isd-level tools)
// ---------------------------------------------------------------------------

static int ismSetAppStat(int stat)
{
    uint8_t tmp[2] = { 4, (uint8_t)stat };
    return ismCmdW(tmp, 2, NULL, 0);
}

static int ismGetAppStat(int *ret)
{
    uint8_t tmp[2] = { 5, 0 };
    if (ismCmdR(tmp, 1, tmp, 2)) return -1;
    *ret = (int)(int16_t)((uint16_t)tmp[0] | ((uint16_t)tmp[1] << 8));
    return 0;
}

static int ismAppCtrl(int stat)
{
    if (_piece_version >= 22) {
        int target = (stat == 3) ? 0 : (stat == 1) ? 2 : -1;
        if (target < 0) return 1;
        if (ismSetAppStat(stat)) return 1;
        for (int i = 0; i < 100; i++) {
            int a;
            if (ismGetAppStat(&a)) return 1;
            if (a == target) return 0;
            ismWait(20);
        }
        fprintf(stderr, "ismAppCtrl: timed out waiting for state %d\n", target);
        return 1;
    } else {
        if (ismSetAppStat(stat)) return 1;
        ismWait(20);
        return 0;
    }
}

int ismAppStop(void)  { return ismAppCtrl(3); }
int ismAppStart(void) { return ismAppCtrl(1); }

#ifndef PIECEIF_H
#define PIECEIF_H

// P/ECE USB interface (pieceif) — Linux/libusb port.
// Original: Copyright (C)2001 AQUAPLUS Co., Ltd. / OeRSTED, Inc.
// libusb port: Copyright (C) autch.  Linux port strips the Windows
// DLL/mutex infrastructure; the USB protocol itself is unchanged.

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Status structure used by the USBCOM serial-over-USB interface.
typedef struct {
    uint8_t  rxstat;       // receive status
    uint8_t  txstat;       // transmit status
    uint8_t  pistat;       // P/ECE open flag
    uint8_t  mystat;       // PC open flag
    uint32_t rxlen;        // receive request size
    uint32_t txlen;        // transmit request size
    char     signature[16];
} USBCOMS;

#define USBCOM_STAT_RXWAIT  1
#define USBCOM_STAT_TXWAIT  1

#define PIECE_INVALID_VERSION  (-1)
#define PIECE_DEF_WAITN        (-1)   // default connect timeout (500 ms)
#define PIECE_MAX_DEVICES       22

// ---------------------------------------------------------------------------
// Low-level USB transport
// ---------------------------------------------------------------------------
int ismInit(void);
int ismExit(void);
int ismInitEx(int devno, int waitn_ms);
int ismExitEx(int devno);
int ismSelect(int devno);
int ismCmdW(const void *cmd, unsigned cmd_len, const void *data, unsigned data_len);
int ismCmdR(const void *cmd, unsigned cmd_len, void *resp, unsigned resp_len);

// ---------------------------------------------------------------------------
// Basic memory/execution interface
// ---------------------------------------------------------------------------
int ismGetVersion(void *buf32, int renew);
int ismReadMem(unsigned char *buf, unsigned long addr, unsigned len);
int ismWriteMem(const unsigned char *buf, unsigned long addr, unsigned len);
int ismExec(unsigned long addr);

// ---------------------------------------------------------------------------
// USBCOM serial-over-USB interface (used by monitor tools)
// ---------------------------------------------------------------------------
int ismUCOpen(USBCOMS *ucs);
int ismUCClose(void);
int ismUCGetStat(USBCOMS *ucs);
int ismUCWrite(const void *ptr, int len);
int ismUCRead(void *ptr, int len);

// ---------------------------------------------------------------------------
// Application control (needed by monitor / isd-level tools)
// ---------------------------------------------------------------------------
int ismAppStop(void);
int ismAppStart(void);

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------
#define ERR_PIECEIF_TIMEOUT            1001  // USB timeout
#define ERR_PIECEIF_ILL_VER            1002  // BIOS version too old
#define ERR_PIECEIF_PFFS_EMPTY         1005  // PFFS full
#define ERR_PIECEIF_PFFS_DIR_EMPTY     1006  // PFFS directory full
#define ERR_PIECEIF_PFFS_BAD_FILENAME  1007  // Invalid PFFS filename
#define ERR_PIECEIF_ALREADY_RUNNING    1010  // Already in use
#define ERR_PIECEIF_OVER_DEVICES       1012  // devno out of range

#ifdef __cplusplus
}
#endif

#endif // PIECEIF_H

#ifndef PFI_H
#define PFI_H

#include "pfi_types.h"

void      PFIInit(PFI *pfi);
void      PFIExit(PFI *pfi);
int       PFIOpen(PFI *pfi, const char *filename);
void      PFIClose(PFI *pfi);
int       PFISave(PFI *pfi, const char *filename);
uint8_t  *PFIGetPFFSTop(PFI *pfi);
uint32_t  PFIGetPFFSMaxFAT(PFI *pfi);
void      PFIDumpSystemInfo(PFI *pfi);

#endif

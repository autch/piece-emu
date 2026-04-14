#ifndef PFFS_H
#define PFFS_H

#include "pfi.h"
#include "pffs_types.h"

void       PFFSLoadMasterBlock(PFI *pfi);
uint32_t   PFFSDirCount(PFI *pfi);
DIRECTORY *PFFSDir(PFI *pfi, int index);
DIRECTORY *PFFSFindFile(PFI *pfi, const char *filename);
uint8_t   *PFFSGetFilesNthSector(PFI *pfi, DIRECTORY *dir, int sector);
uint8_t   *PFFSSectorToPointer(PFI *pfi, int sector);
uint32_t   PFFSFree(PFI *pfi);
void       PFFSDumpDirEntries(PFI *pfi);
int        PFFSExtractFile(PFI *pfi, const char *pffs_name, const char *disk_name);
int        PFFSDeleteFile(PFI *pfi, const char *pffs_name);
int        PFFSAddFile(PFI *pfi, const char *filename);

#endif

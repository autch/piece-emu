#ifndef PFFS_TYPES_H
#define PFFS_TYPES_H

#include <stdint.h>

// P/ECE flash file system types (derived from P/ECE SDK piece.h)

#define MAXDIR 96
#define MAXFAT 496

typedef struct pffsMARK
{
    uint32_t ptr;
    uint32_t resv;
    int8_t   signature[24];
} pffsMARK;

typedef struct DIRECTORY
{
    int8_t   name[24];
    uint8_t  attr;
    uint8_t  resv;
    uint16_t chain;
    uint32_t size;
} DIRECTORY;

typedef struct FAT
{
    uint16_t chain;
} FAT;

typedef struct pffsMASTERBLOCK
{
    pffsMARK   mark;
    DIRECTORY  dir[MAXDIR];
    FAT        fat[MAXFAT];
} pffsMASTERBLOCK;

#define FAT_FREE    0xffff
#define FAT_END     0xeeee
#define FAT_INVALID 0xdddd
#define FAT_SYSTEM  0xcccc

#endif

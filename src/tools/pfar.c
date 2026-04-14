/** @file pfar.c
 * P/ECE Flash image ARchiver.
 *
 * Manage files inside a PFI flash image's PFFS filesystem.
 *
 * Usage: pfar piece.pfi -a|-d|-e|-l|-v [file [...]]
 *
 *   -a  Add file(s) to PFFS
 *   -d  Delete file(s) from PFFS
 *   -e  Extract file(s) from PFFS to disk
 *   -l  List PFFS directory (default)
 *   -v  Show PFI system info
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pffs.h"
#include "pfi.h"

static int usage(void);
static int run(PFI *pfi, const char *pfi_path, char action, char **args);

int main(int argc, char **argv)
{
    PFI         pfi;
    const char *pfi_path = NULL;
    char        action   = 'l';
    char      **args     = NULL;

    (void)argc;
    while (*++argv)
    {
        char *p = *argv;
        if (!pfi_path && *p != '-')
        {
            pfi_path = p;
            continue;
        }
        if (*p != '-' && !args)
        {
            args = argv;
            break;
        }
        switch (*(p + 1))
        {
        case 'a': case 'd': case 'e': case 'l': case 'v':
            action = *(p + 1);
            break;
        default:
            fprintf(stderr, "invalid option -%c, skipping.\n", *(p + 1));
        }
    }

    if (!pfi_path)
        return usage();

    PFIInit(&pfi);
    if (!PFIOpen(&pfi, pfi_path))
    {
        fprintf(stderr, "pfar: cannot open %s\n", pfi_path);
        PFIExit(&pfi);
        return EXIT_FAILURE;
    }

    run(&pfi, pfi_path, action, args);

    PFIClose(&pfi);
    PFIExit(&pfi);
    return EXIT_SUCCESS;
}

static int usage(void)
{
    fprintf(stderr, "Usage: pfar piece.pfi [-adelv] [file [...]]\n");
    return EXIT_FAILURE;
}

static int run(PFI *pfi, const char *pfi_path, char action, char **args)
{
    int nfiles = 0;

    switch (action)
    {
    case 'a':
        if (!args) return usage();
        for (; *args; args++)
        {
            // Show the PFFS name (basename), not the raw path supplied by the user.
            const char *pffs_name = strrchr(*args, '/');
            pffs_name = pffs_name ? pffs_name + 1 : *args;
            printf("Adding %s...", pffs_name);
            fflush(stdout);
            if (PFFSAddFile(pfi, *args))
            {
                printf("ok\n");
                nfiles++;
            }
            else
            {
                printf("failed, aborting\n");
                return -1;
            }
        }
        if (nfiles > 0)
        {
            printf("Writing modifications to %s...", pfi_path);
            fflush(stdout);
            PFISave(pfi, pfi_path);
            printf("done\n");
        }
        break;

    case 'd':
        if (!args) return usage();
        for (; *args; args++)
        {
            printf("Deleting %s...", *args);
            fflush(stdout);
            if (PFFSDeleteFile(pfi, *args))
            {
                printf("ok\n");
                nfiles++;
            }
            else
            {
                printf("failed, aborting\n");
                return -1;
            }
        }
        if (nfiles > 0)
        {
            printf("Writing modifications to %s...", pfi_path);
            fflush(stdout);
            PFISave(pfi, pfi_path);
            printf("done\n");
        }
        break;

    case 'e':
        if (!args) return usage();
        for (; *args; args++)
        {
            printf("Extracting %s...", *args);
            fflush(stdout);
            printf("%s\n", PFFSExtractFile(pfi, *args, *args) ? "ok" : "failed, skipping");
        }
        break;

    case 'l':
        PFIDumpSystemInfo(pfi);
        printf("\n");
        PFFSDumpDirEntries(pfi);
        break;

    case 'v':
        PFIDumpSystemInfo(pfi);
        break;

    default:
        return usage();
    }
    return 0;
}

/** @file pfar.c
 * P/ECE Flash image ARchiver.
 *
 * Manage files inside a PFI flash image's PFFS filesystem.
 *
 * Usage: pfar piece.pfi -a|-A|-d|-e|-l|-v [file [...]]
 *
 *   -a  Add file(s) to PFFS (PFFS name = disk basename)
 *   -A  Add one file under a chosen PFFS name:
 *         pfar piece.pfi -A <local_path> <pffs_name>
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
        case 'a': case 'A': case 'd': case 'e': case 'l': case 'v':
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
    fprintf(stderr,
        "pfar — P/ECE Flash Image ARchiver\n"
        "\n"
        "Manage files inside the PFFS filesystem of a P/ECE flash image.\n"
        "Without an action flag, the directory is listed (same as -l).\n"
        "\n"
        "Usage:\n"
        "  pfar <piece.pfi> [action] [args...]\n"
        "\n"
        "Actions:\n"
        "  -l                          List PFFS directory entries (default)\n"
        "  -v                          Show only PFI system info header\n"
        "  -a <file> [file ...]        Add file(s) to PFFS using each disk\n"
        "                              file's basename as the PFFS name\n"
        "  -A <local_path> <pffs_name> Add exactly one file under a chosen\n"
        "                              PFFS name (no '/' allowed in name)\n"
        "  -e <pffs_name> [name ...]   Extract file(s) from PFFS to the\n"
        "                              current directory (same name)\n"
        "  -d <pffs_name> [name ...]   Delete file(s) from PFFS\n"
        "\n"
        "Notes:\n"
        "  * Add / delete actions rewrite <piece.pfi> in place.\n"
        "  * PFFS filenames are limited to the on-disk DIRECTORY format\n"
        "    (bare filenames; no leading directory components).\n"
        "  * The action flag may appear before or after <piece.pfi>; the\n"
        "    first non-flag argument is treated as the PFI path.\n"
        "\n"
        "Examples:\n"
        "  pfar piece.pfi                          # list directory\n"
        "  pfar piece.pfi -v                       # show SYSTEMINFO only\n"
        "  pfar piece.pfi -a build/bwings.pex      # add as 'bwings.pex'\n"
        "  pfar piece.pfi -A build/bwings.pex startup.pex\n"
        "                                          # add under 'startup.pex'\n"
        "  pfar piece.pfi -e startup.pex           # extract one file\n"
        "  pfar piece.pfi -d old.pex bad.sco       # delete two files\n");
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

    case 'A':
        // -A <local_path> <pffs_name>: exactly two args, one file added
        // under a caller-chosen PFFS name.  Reject malformed counts up
        // front so the PFI never gets a half-written DIRECTORY entry.
        if (!args || !args[0] || !args[1] || args[2])
        {
            fprintf(stderr,
                "-A requires exactly two arguments: <local_path> <pffs_name>\n");
            return usage();
        }
        printf("Adding %s as %s...", args[0], args[1]);
        fflush(stdout);
        if (PFFSAddFileAs(pfi, args[0], args[1]))
        {
            printf("ok\n");
            printf("Writing modifications to %s...", pfi_path);
            fflush(stdout);
            PFISave(pfi, pfi_path);
            printf("done\n");
        }
        else
        {
            printf("failed, aborting\n");
            return -1;
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

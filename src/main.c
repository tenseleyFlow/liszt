#include <stdio.h>
#include <string.h>

#include "config.h"
#include "colors.h"
#include "dirread.h"
#include "emit.h"
#include "entry.h"
#include "idcache.h"
#include "layout.h"
#include "options.h"
#include "plan.h"
#include "quote.h"
#include "recurse.h"
#include "sortkey.h"
#include "sys/dir.h"
#include "sys/scan.h"
#include "sys/thread.h"
#include "sys/xstat.h"
#include "timefmt.h"
#include "util.h"

static int
print_help(void)
{
    printf("Usage: %s [OPTION]... [FILE]...\n", liszt_prog);
    printf("List information about the FILEs (the current directory by default).\n");
    printf("\n");
    printf("liszt is a from-scratch reimplementation of GNU ls; 'lz' is the same\n");
    printf("binary under a shorter name.\n");
    printf("\n");
    printf("This is a skeleton build: only --help and --version are implemented.\n");
    return LISZT_STATUS_OK;
}

static int
print_version(void)
{
    printf("liszt %s\n", LISZT_VERSION);
    return LISZT_STATUS_OK;
}

int
main(int argc, char **argv)
{
    liszt_set_program(argv[0]);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0)
            return print_help();
        if (strcmp(argv[i], "--version") == 0)
            return print_version();
    }

    liszt_error(0, "no ls functionality is implemented yet");
    fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
    return LISZT_STATUS_SERIOUS;
}

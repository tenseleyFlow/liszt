#ifndef LISZT_HUMAN_H
#define LISZT_HUMAN_H

#include <stdint.h>

/* Port of gnulib's human-readable rendering (lib/human.c, pinned 9.11
   tree) - the semantics behind -l sizes, -s block counts, the total
   line, -h/--si, and the --block-size/BLOCK_SIZE family. A separate TU
   rather than part of util/timefmt because it is a self-contained
   gnulib-semantics port shared by several renderers (documented
   deviation from the overview's module list). */

enum {
    LISZT_HUMAN_CEILING = 0,
    LISZT_HUMAN_ROUND_TO_NEAREST = 1,
    LISZT_HUMAN_FLOOR = 2,
    LISZT_HUMAN_GROUP_DIGITS = 4,
    LISZT_HUMAN_SUPPRESS_POINT_ZERO = 8,
    LISZT_HUMAN_AUTOSCALE = 16,
    LISZT_HUMAN_BASE_1024 = 32,
    LISZT_HUMAN_SPACE_BEFORE_UNIT = 64,
    LISZT_HUMAN_SI = 128,
    LISZT_HUMAN_B = 256
};

/* Longest sensible rendering, sized as gnulib does. */
#define LISZT_LONGEST_HUMAN_READABLE \
    ((2 * sizeof (uintmax_t) * 8 * 146 / 485 + 1) * 2 + 1 + 3 + 2 + 2)

/* Render N (in FROM units) into BUF (at least
   LISZT_LONGEST_HUMAN_READABLE + 1 bytes) using TO units; returns a
   pointer into BUF. */
char *liszt_human_readable(uintmax_t n, char *buf, int opts,
                           uintmax_t from_block_size,
                           uintmax_t to_block_size);

enum liszt_strtol_error {
    LISZT_LONGINT_OK = 0,
    LISZT_LONGINT_OVERFLOW,
    LISZT_LONGINT_INVALID_SUFFIX_CHAR,
    LISZT_LONGINT_INVALID
};

/* Parse SPEC as a block size (NULL consults BLOCK_SIZE/BLOCKSIZE env,
   default 1024 or 512 under POSIXLY_CORRECT). On error *block_size is
   the default and the error is returned; callers decide fatality (GNU:
   --block-size fatal exit 2, env silently ignored). */
enum liszt_strtol_error liszt_human_options(const char *spec, int *opts,
                                            uintmax_t *block_size);

#endif

#include "human.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Faithful port of gnulib human.c's state machine, including the exact
   rounding-remainder encoding (0: exact; 1: below half-tenth; 2: exactly
   half-tenth; 3: above) and the long-double fallback. Byte parity
   depends on all of it. */

static const char power_letter[] =
    { 0, 'K', 'M', 'G', 'T', 'P', 'E', 'Z', 'Y', 'R', 'Q' };

#define SUFFIX_MAX 3    /* longest suffix: " KiB" pieces, sans space */

static long double
adjust_value(int inexact_style, long double value)
{
    if (inexact_style != LISZT_HUMAN_ROUND_TO_NEAREST
        && value < (long double)UINTMAX_MAX) {
        uintmax_t u = (uintmax_t)value;
        value = (long double)u
            + (inexact_style == LISZT_HUMAN_CEILING
               && (long double)u != value);
    }
    return value;
}

static char *
group_number(char *number, size_t numberlen, const char *grouping,
             const char *thousands_sep)
{
    size_t thousands_seplen = strlen(thousands_sep);
    char buf[2 * (sizeof (uintmax_t) * CHAR_BIT / 3 + 2) + 1];
    char *d = number + numberlen;
    size_t grouplen = (size_t)-1;
    size_t i = numberlen;

    memcpy(buf, number, numberlen);
    for (;;) {
        unsigned char g = (unsigned char)*grouping;

        if (g) {
            grouplen = g < CHAR_MAX ? g : i;
            grouping++;
        }
        if (i < grouplen)
            grouplen = i;
        d -= grouplen;
        i -= grouplen;
        memcpy(d, buf + i, grouplen);
        if (i == 0)
            return d;
        d -= thousands_seplen;
        memcpy(d, thousands_sep, thousands_seplen);
    }
}

char *
liszt_human_readable(uintmax_t n, char *buf, int opts,
                     uintmax_t from_block_size, uintmax_t to_block_size)
{
    int inexact_style = opts
        & (LISZT_HUMAN_ROUND_TO_NEAREST | LISZT_HUMAN_FLOOR
           | LISZT_HUMAN_CEILING);
    unsigned int base = (opts & LISZT_HUMAN_BASE_1024) ? 1024 : 1000;
    /* localeconv() was 5% of the -l lane when consulted per call; the
       locale cannot change mid-run, so resolve the pieces once. */
    static const char *decimal_point;
    static size_t decimal_pointlen;
    static const char *grouping;
    static const char *thousands_sep;

    if (decimal_point == NULL) {
        struct lconv *l = localeconv();
        size_t pointlen = strlen(l->decimal_point);

        decimal_point = ".";
        decimal_pointlen = 1;
        grouping = l->grouping;
        thousands_sep = "";
        if (0 < pointlen && pointlen <= MB_LEN_MAX) {
            decimal_point = l->decimal_point;
            decimal_pointlen = pointlen;
        }
        if (strlen(l->thousands_sep) <= MB_LEN_MAX)
            thousands_sep = l->thousands_sep;
    }

    char *psuffix = buf + LISZT_LONGEST_HUMAN_READABLE - 1 - SUFFIX_MAX;
    char *p = psuffix;

    uintmax_t amt;
    int tenths;
    int rounding;
    int exponent = -1;
    int exponent_max = sizeof power_letter - 1;
    const char *integerlim;

    if (to_block_size <= from_block_size) {
        if (from_block_size % to_block_size == 0) {
            uintmax_t multiplier = from_block_size / to_block_size;
            amt = n * multiplier;
            if (amt / multiplier == n) {
                tenths = 0;
                rounding = 0;
                goto use_integer_arithmetic;
            }
        }
    } else if (from_block_size != 0
               && to_block_size % from_block_size == 0) {
        uintmax_t divisor = to_block_size / from_block_size;
        uintmax_t r10 = (n % divisor) * 10;
        uintmax_t r2 = (r10 % divisor) * 2;
        amt = n / divisor;
        tenths = (int)(r10 / divisor);
        rounding = r2 < divisor ? (0 < r2) : 2 + (divisor < r2);
        goto use_integer_arithmetic;
    }

    {
        long double dto_block_size = (long double)to_block_size;
        long double damt =
            (long double)n * ((long double)from_block_size / dto_block_size);
        size_t buflen;
        size_t nonintegerlen;

        if (!(opts & LISZT_HUMAN_AUTOSCALE)) {
            snprintf(buf, LISZT_LONGEST_HUMAN_READABLE + 1, "%.0Lf",
                     adjust_value(inexact_style, damt));
            buflen = strlen(buf);
            nonintegerlen = 0;
        } else {
            long double e = 1;
            exponent = 0;
            do {
                e *= base;
                exponent++;
            } while (e * base <= damt && exponent < exponent_max);
            damt /= e;
            snprintf(buf, LISZT_LONGEST_HUMAN_READABLE + 1, "%.1Lf",
                     adjust_value(inexact_style, damt));
            buflen = strlen(buf);
            nonintegerlen = decimal_pointlen + 1;
            if (1 + nonintegerlen + !(opts & LISZT_HUMAN_BASE_1024) < buflen
                || ((opts & LISZT_HUMAN_SUPPRESS_POINT_ZERO)
                    && buf[buflen - 1] == '0')) {
                snprintf(buf, LISZT_LONGEST_HUMAN_READABLE + 1, "%.0Lf",
                         adjust_value(inexact_style, damt * 10) / 10);
                buflen = strlen(buf);
                nonintegerlen = 0;
            }
        }
        p = psuffix - buflen;
        memmove(p, buf, buflen);
        integerlim = p + buflen - nonintegerlen;
    }
    goto do_grouping;

use_integer_arithmetic:
    {
        if (opts & LISZT_HUMAN_AUTOSCALE) {
            exponent = 0;
            if (base <= amt) {
                do {
                    unsigned int r10 =
                        (unsigned int)((amt % base) * 10 + (uintmax_t)tenths);
                    unsigned int r2 =
                        (r10 % base) * 2 + ((unsigned int)rounding >> 1);
                    amt /= base;
                    tenths = (int)(r10 / base);
                    rounding = (r2 < base
                                ? (r2 + (unsigned int)rounding) != 0
                                : (int)(2 + (base < r2 + (unsigned int)rounding)));
                    exponent++;
                } while (base <= amt && exponent < exponent_max);

                if (amt < 10) {
                    if (inexact_style == LISZT_HUMAN_ROUND_TO_NEAREST
                        ? 2 < rounding + (tenths & 1)
                        : inexact_style == LISZT_HUMAN_CEILING
                          && 0 < rounding) {
                        tenths++;
                        rounding = 0;
                        if (tenths == 10) {
                            amt++;
                            tenths = 0;
                        }
                    }
                    if (amt < 10
                        && (tenths
                            || !(opts & LISZT_HUMAN_SUPPRESS_POINT_ZERO))) {
                        *--p = (char)('0' + tenths);
                        p -= decimal_pointlen;
                        memcpy(p, decimal_point, decimal_pointlen);
                        tenths = rounding = 0;
                    }
                }
            }
        }

        if (inexact_style == LISZT_HUMAN_ROUND_TO_NEAREST
            ? 5 < tenths + (0 < rounding + (int)(amt & 1))
            : inexact_style == LISZT_HUMAN_CEILING
              && 0 < tenths + rounding) {
            amt++;
            if ((opts & LISZT_HUMAN_AUTOSCALE) && amt == base
                && exponent < exponent_max) {
                exponent++;
                if (!(opts & LISZT_HUMAN_SUPPRESS_POINT_ZERO)) {
                    *--p = '0';
                    p -= decimal_pointlen;
                    memcpy(p, decimal_point, decimal_pointlen);
                }
                amt = 1;
            }
        }

        integerlim = p;
        do {
            int digit = (int)(amt % 10);
            *--p = (char)(digit + '0');
        } while ((amt /= 10) != 0);
    }

do_grouping:
    if (opts & LISZT_HUMAN_GROUP_DIGITS)
        p = group_number(p, (size_t)(integerlim - p), grouping,
                         thousands_sep);

    if (opts & LISZT_HUMAN_SI) {
        if (exponent < 0) {
            exponent = 0;
            for (uintmax_t power = 1; power < to_block_size; power *= base)
                if (++exponent == exponent_max)
                    break;
        }
        if ((exponent | (opts & LISZT_HUMAN_B))
            && (opts & LISZT_HUMAN_SPACE_BEFORE_UNIT))
            *psuffix++ = ' ';
        if (exponent)
            *psuffix++ = (!(opts & LISZT_HUMAN_BASE_1024) && exponent == 1
                          ? 'k'
                          : power_letter[exponent]);
        if (opts & LISZT_HUMAN_B) {
            if ((opts & LISZT_HUMAN_BASE_1024) && exponent)
                *psuffix++ = 'i';
            *psuffix++ = 'B';
        }
    }
    *psuffix = '\0';
    return p;
}

/* --- block-size parsing (humblock/xstrtoumax subset) ------------------ */

static int
suffix_power(char c)
{
    switch (c) {
    case 'k': case 'K': return 1;
    case 'm': case 'M': return 2;
    case 'g': case 'G': return 3;
    case 't': case 'T': return 4;
    case 'p': case 'P': return 5;
    case 'e': case 'E': return 6;
    case 'z': case 'Z': return 7;
    case 'y': case 'Y': return 8;
    default: return 0;
    }
}

static enum liszt_strtol_error
bkm_scale(uintmax_t *x, unsigned int scale, int power)
{
    while (power-- > 0) {
        if (*x > UINTMAX_MAX / scale)
            return LISZT_LONGINT_OVERFLOW;
        *x *= scale;
    }
    return LISZT_LONGINT_OK;
}

/* xstrtoumax over the "eEgGkKmMpPtTyYzZ0" suffix set: NUM[SUFFIX[B|iB]];
   bare suffix scales by 1024, "MB" by 1000, "MiB" by 1024. */
static enum liszt_strtol_error
parse_size(const char *spec, const char **endp, uintmax_t *out)
{
    char *num_end;
    uintmax_t v = 0;
    int have_digits = 0;

    if (*spec >= '0' && *spec <= '9') {
        errno = 0;
        v = strtoumax(spec, &num_end, 0);
        if (errno == ERANGE)
            return LISZT_LONGINT_OVERFLOW;
        have_digits = 1;
        spec = num_end;
    } else {
        v = 1;
    }

    if (*spec != '\0') {
        int power = suffix_power(*spec);
        if (power == 0)
            return LISZT_LONGINT_INVALID;
        unsigned int scale = 1024;
        const char *after = spec + 1;
        if (after[0] == 'B' && after[1] == '\0') {
            scale = 1000;
            after++;
        } else if (after[0] == 'i' && after[1] == 'B' && after[2] == '\0') {
            after += 2;
        } else if (after[0] != '\0') {
            return LISZT_LONGINT_INVALID;
        }
        if (bkm_scale(&v, scale, power) != LISZT_LONGINT_OK)
            return have_digits ? LISZT_LONGINT_OVERFLOW
                               : LISZT_LONGINT_OVERFLOW;
        spec = after;
    } else if (!have_digits) {
        return LISZT_LONGINT_INVALID;
    }

    *endp = spec;
    *out = v;
    return LISZT_LONGINT_OK;
}

static uintmax_t
default_block_size(void)
{
    return getenv("POSIXLY_CORRECT") ? 512 : 1024;
}

static enum liszt_strtol_error
humblock(const char *spec, uintmax_t *block_size, int *options)
{
    int opts = 0;

    if (!spec && !(spec = getenv("BLOCK_SIZE"))
        && !(spec = getenv("BLOCKSIZE"))) {
        *block_size = default_block_size();
    } else {
        if (*spec == '\'') {
            opts |= LISZT_HUMAN_GROUP_DIGITS;
            spec++;
        }
        if (strcmp(spec, "human-readable") == 0) {
            opts |= LISZT_HUMAN_AUTOSCALE | LISZT_HUMAN_SI
                | LISZT_HUMAN_BASE_1024;
            *block_size = 1;
        } else if (strcmp(spec, "si") == 0) {
            opts |= LISZT_HUMAN_AUTOSCALE | LISZT_HUMAN_SI;
            *block_size = 1;
        } else {
            const char *end;
            enum liszt_strtol_error e = parse_size(spec, &end, block_size);
            if (e != LISZT_LONGINT_OK) {
                *options = 0;
                return e;
            }
            /* Suffix-only specs ("K", "MB", "KiB") imply SI display
               opts; specs with digits ("1K") do not - gnulib walks the
               leading non-digits and only fires when it reaches the
               parse end. */
            for (const char *q = spec; !(*q >= '0' && *q <= '9'); q++) {
                if (q == end) {
                    opts |= LISZT_HUMAN_SI;
                    if (end[-1] == 'B')
                        opts |= LISZT_HUMAN_B;
                    if (end[-1] != 'B'
                        || (end - spec >= 2 && end[-2] == 'i'))
                        opts |= LISZT_HUMAN_BASE_1024;
                    break;
                }
            }
        }
    }
    *options = opts;
    return LISZT_LONGINT_OK;
}

enum liszt_strtol_error
liszt_human_options(const char *spec, int *opts, uintmax_t *block_size)
{
    enum liszt_strtol_error e = humblock(spec, block_size, opts);
    if (*block_size == 0) {
        *block_size = default_block_size();
        e = LISZT_LONGINT_INVALID;
    }
    return e;
}

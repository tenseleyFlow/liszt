#include "util.h"

#include <errno.h>
#include <langinfo.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *liszt_prog = "liszt";
const char *liszt_argv0 = "liszt";

void
liszt_set_program(const char *argv0)
{
    const char *slash = strrchr(argv0, '/');
    liszt_argv0 = argv0;
    liszt_prog = slash ? slash + 1 : argv0;
}

static const char *quote_left = "'";
static const char *quote_right = "'";

void
liszt_diag_init(void)
{
    const char *cs = nl_langinfo(CODESET);
    if (cs && strcmp(cs, "UTF-8") == 0) {
        quote_left = "\342\200\230";    /* U+2018 */
        quote_right = "\342\200\231";   /* U+2019 */
    }
}

const char *
liszt_qL(void)
{
    return quote_left;
}

const char *
liszt_qR(void)
{
    return quote_right;
}

void
liszt_try_help_print(void)
{
    fprintf(stderr, "Try '%s --help' for more information.\n", liszt_argv0);
}

_Noreturn void
liszt_try_help_and_die(void)
{
    liszt_try_help_print();
    exit(LISZT_STATUS_SERIOUS);
}

static void
verror(int errnum, const char *fmt, va_list ap)
{
    fprintf(stderr, "%s: ", liszt_prog);
    vfprintf(stderr, fmt, ap);
    if (errnum != 0)
        fprintf(stderr, ": %s", strerror(errnum));
    fputc('\n', stderr);
}

void
liszt_error(int errnum, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verror(errnum, fmt, ap);
    va_end(ap);
}

_Noreturn void
liszt_die(int status, int errnum, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verror(errnum, fmt, ap);
    va_end(ap);
    exit(status);
}

void *
liszt_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        liszt_die(LISZT_STATUS_SERIOUS, errno, "memory exhausted");
    return p;
}

void *
liszt_xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        liszt_die(LISZT_STATUS_SERIOUS, errno, "memory exhausted");
    return q;
}

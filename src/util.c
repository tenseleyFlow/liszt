#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *liszt_prog = "liszt";

void
liszt_set_program(const char *argv0)
{
    const char *slash = strrchr(argv0, '/');
    liszt_prog = slash ? slash + 1 : argv0;
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

void
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

#include "util.h"

#include <errno.h>
#include <langinfo.h>
#include <wchar.h>
#include <wctype.h>
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

const char *
liszt_quote_diag(const char *name)
{
    static char *slots[2];
    static size_t caps[2];
    static int turn;

    turn = 1 - turn;
    size_t len = strlen(name);
    size_t want = len * 4 + 1;
    if (caps[turn] < want) {
        slots[turn] = liszt_xrealloc(slots[turn], want);
        caps[turn] = want;
    }
    char *out = slots[turn];
    size_t o = 0;
    size_t i = 0;
    mbstate_t st;
    memset(&st, 0, sizeof st);

    while (i < len) {
        wchar_t wc;
        size_t r = mbrtowc(&wc, name + i, len - i, &st);
        if (r != (size_t)-1 && r != (size_t)-2 && r != 0
            && iswprint((wint_t)wc) && wc != L'\\') {
            memcpy(out + o, name + i, r);
            o += r;
            i += r;
            continue;
        }
        if (r == (size_t)-1 || r == (size_t)-2)
            memset(&st, 0, sizeof st);
        unsigned char c = (unsigned char)name[i];
        i++;
        switch (c) {
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\a': out[o++] = '\\'; out[o++] = 'a'; break;
        case '\b': out[o++] = '\\'; out[o++] = 'b'; break;
        case '\t': out[o++] = '\\'; out[o++] = 't'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
        case '\v': out[o++] = '\\'; out[o++] = 'v'; break;
        case '\f': out[o++] = '\\'; out[o++] = 'f'; break;
        case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
        default:
            out[o++] = '\\';
            out[o++] = (char)('0' + ((c >> 6) & 7));
            out[o++] = (char)('0' + ((c >> 3) & 7));
            out[o++] = (char)('0' + (c & 7));
            break;
        }
    }
    out[o] = '\0';
    return out;
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

static int exit_status = LISZT_STATUS_OK;

void
liszt_set_exit_status(bool serious)
{
    if (serious)
        exit_status = LISZT_STATUS_SERIOUS;
    else if (exit_status == LISZT_STATUS_OK)
        exit_status = LISZT_STATUS_MINOR;
}

int
liszt_exit_status(void)
{
    return exit_status;
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

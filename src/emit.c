#include "emit.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

/* 256 KiB flush threshold (ferret's measured choice; re-measure in 09). */
#define EMIT_FLUSH_THRESHOLD (256u * 1024u)

static unsigned char *buf;
static size_t buf_len;
static size_t buf_cap;
static int write_errno;     /* sticky first failure; 0 = healthy */

static void
drain(void)
{
    size_t off = 0;

    if (write_errno) {
        buf_len = 0;
        return;
    }
    while (off < buf_len) {
        ssize_t w = write(STDOUT_FILENO, buf + off, buf_len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            write_errno = errno;
            break;
        }
        off += (size_t)w;
    }
    buf_len = 0;
}

static off_t emit_total;

off_t
liszt_emit_total(void)
{
    return emit_total;
}

void
liszt_emit_bytes(const void *p, size_t n)
{
    emit_total += (off_t)n;
    if (buf_len + n > buf_cap) {
        size_t cap = buf_cap ? buf_cap : 64 * 1024;
        while (cap < buf_len + n)
            cap += cap / 2;
        buf = liszt_xrealloc(buf, cap);
        buf_cap = cap;
    }
    memcpy(buf + buf_len, p, n);
    buf_len += n;
    if (buf_len >= EMIT_FLUSH_THRESHOLD)
        drain();
}

void
liszt_emit_byte(int c)
{
    unsigned char b = (unsigned char)c;
    liszt_emit_bytes(&b, 1);
}

void
liszt_emit_str(const char *s)
{
    liszt_emit_bytes(s, strlen(s));
}

void
liszt_emit_flush(void)
{
    drain();
}

int
liszt_emit_finish(int *errnum)
{
    drain();
    if (write_errno) {
        *errnum = write_errno;
        return -1;
    }
    return 0;
}

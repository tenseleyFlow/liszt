#include "sys/xstat.h"

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "util.h"

#if LISZT_HAVE_ST_MTIM
#define ST_MTIMESPEC(st) ((st)->st_mtim)
#elif LISZT_HAVE_ST_MTIMESPEC
#define ST_MTIMESPEC(st) ((st)->st_mtimespec)
#else
#error "no nanosecond mtime member probed"
#endif

static char *pathbuf;
static size_t pathbuf_cap;

const char *
liszt_join_path(const char *dir, const char *name)
{
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    int need_slash = dlen > 0 && dir[dlen - 1] != '/';
    size_t want = dlen + (size_t)need_slash + nlen + 1;

    if (want > pathbuf_cap) {
        size_t cap = pathbuf_cap ? pathbuf_cap : 256;
        while (cap < want)
            cap += cap / 2;
        pathbuf = liszt_xrealloc(pathbuf, cap);
        pathbuf_cap = cap;
    }
    memcpy(pathbuf, dir, dlen);
    if (need_slash)
        pathbuf[dlen] = '/';
    memcpy(pathbuf + dlen + (size_t)need_slash, name, nlen + 1);
    return pathbuf;
}

static void
fill(const struct stat *st, struct liszt_statinfo *out)
{
    out->size = st->st_size;
    out->mtime = ST_MTIMESPEC(st);
    out->mode = st->st_mode;
}

int
liszt_lstat_join(const char *dir, const char *name,
                 struct liszt_statinfo *out)
{
    struct stat st;
    if (lstat(liszt_join_path(dir, name), &st) < 0)
        return -1;
    fill(&st, out);
    return 0;
}

int
liszt_stat_join(const char *dir, const char *name,
                struct liszt_statinfo *out)
{
    struct stat st;
    if (stat(liszt_join_path(dir, name), &st) < 0)
        return -1;
    fill(&st, out);
    return 0;
}

int
liszt_stat_path(const char *path, struct liszt_statinfo *out)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return -1;
    fill(&st, out);
    return 0;
}

int
liszt_lstat_path(const char *path, struct liszt_statinfo *out)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return -1;
    fill(&st, out);
    return 0;
}

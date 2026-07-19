#include "dirread.h"

#include <errno.h>

static int
ignored(enum liszt_ignore_mode mode, const char *name, size_t len)
{
    switch (mode) {
    case LISZT_IGNORE_DEFAULT:
        return len > 0 && name[0] == '.';
    case LISZT_IGNORE_DOT_AND_DOTDOT:
        return len > 0 && name[0] == '.'
            && (len == 1 || (len == 2 && name[1] == '.'));
    case LISZT_IGNORE_MINIMAL:
    default:
        return 0;
    }
}

int
liszt_dirread_collect(const char *path, enum liszt_ignore_mode mode,
                      struct liszt_entries *out,
                      liszt_dirread_diag diag, void *ctx)
{
    struct liszt_dir *d;

    if (liszt_diropen(path, &d) < 0)
        return -1;
    liszt_dirread_collect_from(d, mode, out, diag, ctx);
    return 0;
}

void
liszt_dirread_collect_from(struct liszt_dir *d, enum liszt_ignore_mode mode,
                           struct liszt_entries *out,
                           liszt_dirread_diag diag, void *ctx)
{
    liszt_entries_clear(out);
    for (;;) {
        struct liszt_dirent e;
        int r = liszt_dirread(d, &e);
        if (r == 0)
            break;
        if (r < 0) {
            int err = errno;
            diag(ctx, LISZT_DIRFAIL_READ, err);
            /* GNU ls stops on read errors except EOVERFLOW. */
            if (err != EOVERFLOW)
                break;
            continue;
        }
        if (!ignored(mode, e.name, e.namelen))
            liszt_entries_add(out, e.name, e.namelen, e.type);
    }

    /* No fclosedir-style error path in our reader today; close and keep
       whatever was collected, as GNU does after diagnosing. */
    liszt_dirclose(d);
}

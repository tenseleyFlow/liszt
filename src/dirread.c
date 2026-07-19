#include "dirread.h"

#include <errno.h>
#include <fnmatch.h>

static int
patterns_match(char *const *pats, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (fnmatch(pats[i], name, FNM_PERIOD) == 0)
            return 1;
    return 0;
}

/* GNU file_ignored: mode dots, then --hide (default mode only), then
   -I/-B unconditionally. */
static int
ignored(const struct liszt_ignore_spec *spec, const char *name, size_t len)
{
    switch (spec->mode) {
    case LISZT_IGNORE_DEFAULT:
        if (len > 0 && name[0] == '.')
            return 1;
        if (patterns_match(spec->hide, spec->n_hide, name))
            return 1;
        break;
    case LISZT_IGNORE_DOT_AND_DOTDOT:
        if (len > 0 && name[0] == '.'
            && (len == 1 || (len == 2 && name[1] == '.')))
            return 1;
        break;
    case LISZT_IGNORE_MINIMAL:
    default:
        break;
    }
    return patterns_match(spec->ignore, spec->n_ignore, name);
}

int
liszt_dirread_collect(const char *path,
                      const struct liszt_ignore_spec *spec,
                      struct liszt_entries *out,
                      liszt_dirread_diag diag, void *ctx)
{
    struct liszt_dir *d;

    if (liszt_diropen(path, &d) < 0)
        return -1;
    liszt_dirread_collect_from(d, spec, out, diag, ctx);
    return 0;
}

void
liszt_dirread_collect_from(struct liszt_dir *d,
                           const struct liszt_ignore_spec *spec,
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
        if (!ignored(spec, e.name, e.namelen))
            liszt_entries_add(out, e.name, e.namelen, e.type);
    }

    /* No fclosedir-style error path in our reader today; close and keep
       whatever was collected, as GNU does after diagnosing. */
    liszt_dirclose(d);
}

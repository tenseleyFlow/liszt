#ifndef LISZT_DIRREAD_H
#define LISZT_DIRREAD_H

#include "entry.h"
#include "sys/dir.h"

/* The three ignore modes of GNU ls (ls.c enum ignore_mode). */
enum liszt_ignore_mode {
    LISZT_IGNORE_DEFAULT = 0,       /* skip names starting with '.' */
    LISZT_IGNORE_DOT_AND_DOTDOT,    /* -A: skip only "." and ".." */
    LISZT_IGNORE_MINIMAL            /* -a: skip nothing */
};

/* Mid-read failures reported through the callback, mirroring GNU's
   diagnostics: "reading directory %s" and "closing directory %s". */
enum liszt_dirread_fail {
    LISZT_DIRFAIL_READ,
    LISZT_DIRFAIL_CLOSE
};

typedef void (*liszt_dirread_diag)(void *ctx, enum liszt_dirread_fail how,
                                   int errnum);

/* Collect PATH's entries in readdir order, filtered per MODE, into OUT
   (cleared first). Returns 0 on open success (even if a read error cut
   the listing short - GNU prints what it got), -1 if the directory could
   not be opened (errno set). Read errors stop collection unless
   EOVERFLOW, exactly as GNU ls behaves; each failure is reported through
   DIAG before the policy applies. */
int liszt_dirread_collect(const char *path, enum liszt_ignore_mode mode,
                          struct liszt_entries *out,
                          liszt_dirread_diag diag, void *ctx);

/* Same, over an already-open handle (consumed and closed). Lets callers
   order opendir failure before loop detection, as GNU does. */
void liszt_dirread_collect_from(struct liszt_dir *d,
                                enum liszt_ignore_mode mode,
                                struct liszt_entries *out,
                                liszt_dirread_diag diag, void *ctx);

#endif

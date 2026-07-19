#ifndef LISZT_SORTKEY_H
#define LISZT_SORTKEY_H

#include <stdbool.h>

#include <sys/types.h>
#include <time.h>

#include "entry.h"
#include "options.h"
#include "plan.h"

/* A comparator's view of one element: the chain reads only this. */
struct liszt_item {
    const char *name;
    off_t size;
    struct timespec mtime;
    bool group_dir;     /* counts as a directory for grouping */
};

/* Collation identity probe, cached process-wide (rank's port): true when
   strcoll order provably equals byte order. Conservative: multibyte
   locales (including C.UTF-8) are never identity; they ride the
   transformed engine instead. */
bool liszt_locale_collation_identity(void);

/* Bind the sorter to the resolved options and selected plan. */
void liszt_sort_init(const struct liszt_options *o,
                     const struct liszt_plan *p);

/* Sort a directory's entries in place per the plan. Under
   LISZT_DEBUG_VERIFY every engine-sorted result is re-checked against
   the scalar comparator chain; a mismatch is fatal (exit 2). */
void liszt_sort_entries(struct liszt_entries *es);

/* Stable sort of the operand array (any element type; GET_ITEM fills
   the comparator's view). setjmp-protected like liszt_sort_entries. */
void liszt_sort_operands(void *base, size_t n, size_t size,
                         void (*get_item)(const void *,
                                          struct liszt_item *));

#endif

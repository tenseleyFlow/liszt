#ifndef LISZT_SORTKEY_H
#define LISZT_SORTKEY_H

#include <stdbool.h>

#include "entry.h"
#include "options.h"
#include "plan.h"

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

/* The scalar comparator chain over two names (also the verification
   oracle). Includes -r. May report a strcoll failure per GNU semantics -
   only call while a sort entry point is active. */
int liszt_sort_cmp_names(const char *a, const char *b);

/* Stable sort of the operand array (any element type; GET_NAME extracts
   the name). setjmp-protected like liszt_sort_entries. */
void liszt_sort_operands(void *base, size_t n, size_t size,
                         const char *(*get_name)(const void *));

#endif

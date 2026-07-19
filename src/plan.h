#ifndef LISZT_PLAN_H
#define LISZT_PLAN_H

#include "options.h"

/* The plan is the center of liszt: it derives, from the resolved options
   (and later the parsed color scheme), the per-entry data set and the
   engines. Sprint 02 owns the sort-engine half; the fetch-set half lands
   with sprint 03's statx masks. */

enum liszt_sort_plan {
    LISZT_PLAN_SORT_NONE = 0,
    LISZT_PLAN_SORT_SCALAR,         /* comparator chain, the oracle */
    LISZT_PLAN_SORT_RADIX_BYTES,    /* identity collation, name bytes */
    LISZT_PLAN_SORT_RADIX_TRANSFORMED, /* strxfrm once, radix transforms */
    LISZT_PLAN_SORT_RADIX_NUMERIC   /* 64-bit keys (-S/-t) */
};

struct liszt_plan {
    enum liszt_sort_plan sort_engine;
    const char *reason;
    /* The per-entry fetch set (GNU calc_req_mask semantics plus liszt's
       explicit BLOCKS request for the total line). */
    bool needs_stat;
    unsigned stat_wants;        /* LISZT_WANT_* */
    bool needs_link_target;     /* readlink for -l symlinks */
    bool needs_xattr;           /* ACL/context mode suffix */
};

void liszt_plan_select(const struct liszt_options *o, struct liszt_plan *p);

/* Honors LISZT_DEBUG_PLAN. */
void liszt_plan_debug_print(const struct liszt_plan *p);

#endif

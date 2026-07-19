#include "plan.h"

#include <stdio.h>
#include <stdlib.h>

#include "sortkey.h"
#include "sys/xstat.h"
#include "util.h"

static const char *const engine_names[] = {
    "none", "scalar", "radix-bytes", "radix-transformed", "radix-numeric"
};

static void
select_fetch_set(const struct liszt_options *o, struct liszt_plan *p)
{
    unsigned wants = 0;
    bool long_fmt = o->format == LISZT_FMT_LONG;

    if (long_fmt || o->print_block_size || o->print_inode
        || o->sort == LISZT_SORT_SIZE || o->sort == LISZT_SORT_TIME) {
        wants |= LISZT_WANT_MODE;
        if (o->print_inode)
            wants |= LISZT_WANT_INO;
        if (o->print_block_size || long_fmt)
            wants |= LISZT_WANT_BLOCKS;     /* total line needs blocks */
        if (long_fmt) {
            wants |= LISZT_WANT_NLINK | LISZT_WANT_SIZE
                | LISZT_WANT_MTIME;
            if (o->print_owner || o->print_author)
                wants |= LISZT_WANT_UID;
            if (o->print_group)
                wants |= LISZT_WANT_GID;
        }
        if (o->sort == LISZT_SORT_TIME)
            wants |= LISZT_WANT_MTIME;
        if (o->sort == LISZT_SORT_SIZE)
            wants |= LISZT_WANT_SIZE;
    }
    p->needs_stat = wants != 0;
    p->stat_wants = wants;
    p->needs_link_target = long_fmt;
    p->needs_xattr = long_fmt;
}

void
liszt_plan_select(const struct liszt_options *o, struct liszt_plan *p)
{
    select_fetch_set(o, p);
    if (o->sort == LISZT_SORT_NONE) {
        p->sort_engine = LISZT_PLAN_SORT_NONE;
        p->reason = "sort disabled";
        return;
    }
    if (getenv("LISZT_FORCE_SCALAR") != NULL) {
        p->sort_engine = LISZT_PLAN_SORT_SCALAR;
        p->reason = "forced scalar";
        return;
    }
    if (o->sort == LISZT_SORT_NAME) {
        if (liszt_locale_collation_identity()) {
            p->sort_engine = LISZT_PLAN_SORT_RADIX_BYTES;
            p->reason = "identity collation";
            return;
        }
        p->sort_engine = LISZT_PLAN_SORT_RADIX_TRANSFORMED;
        p->reason = "hard locale, transform once";
        return;
    }
    if (o->sort == LISZT_SORT_SIZE || o->sort == LISZT_SORT_TIME) {
        p->sort_engine = LISZT_PLAN_SORT_RADIX_NUMERIC;
        p->reason = "64-bit key radix";
        return;
    }
    /* Version, extension, width: scalar comparators until an exact
       transformed form is proven. */
    p->sort_engine = LISZT_PLAN_SORT_SCALAR;
    p->reason = "comparator-only sort word";
}

void
liszt_plan_debug_print(const struct liszt_plan *p)
{
    if (getenv("LISZT_DEBUG_PLAN") != NULL)
        fprintf(stderr, "%s: plan=%s reason=%s\n", liszt_prog,
                engine_names[p->sort_engine], p->reason);
}

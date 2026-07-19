#include "plan.h"

#include <stdio.h>
#include <stdlib.h>

#include "sortkey.h"
#include "util.h"

static const char *const engine_names[] = {
    "none", "scalar", "radix-bytes", "radix-transformed", "radix-numeric"
};

void
liszt_plan_select(const struct liszt_options *o, struct liszt_plan *p)
{
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
    /* Version, extension: scalar comparators until an exact transformed
       form is proven. Size/time move to radix-numeric in 02E. */
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

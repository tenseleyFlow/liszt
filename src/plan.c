#include "plan.h"

#include <stdio.h>
#include <stdlib.h>

#include "colors.h"
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
                | LISZT_WANT_TIME;
            if (o->print_owner || o->print_author)
                wants |= LISZT_WANT_UID;
            if (o->print_group)
                wants |= LISZT_WANT_GID;
        }
        if (o->sort == LISZT_SORT_TIME)
            wants |= LISZT_WANT_TIME;
        if (o->sort == LISZT_SORT_SIZE)
            wants |= LISZT_WANT_SIZE;
    }
    p->needs_stat = wants != 0;
    p->stat_wants = wants;
    p->needs_link_target = long_fmt;
    p->needs_xattr = long_fmt || o->print_scontext;
    p->stat_dirs_for_color = false;
    p->stat_exec = false;
    p->stat_links = false;
    p->check_symlink_mode = false;
    p->link_target_mode = false;
    p->cap_probe = false;
}

void
liszt_plan_color_update(const struct liszt_options *o, struct liszt_plan *p)
{
    bool color = o->print_with_color;
    bool long_fmt = o->format == LISZT_FMT_LONG;

    /* GNU main 1688-1697. */
    p->check_symlink_mode = o->group_directories_first
        || (color
            && (liszt_color_is_colored(LISZT_C_ORPHAN)
                || (liszt_color_is_colored(LISZT_C_EXEC)
                    && liszt_color_symlink_as_referent())
                || (liszt_color_is_colored(LISZT_C_MISSING) && long_fmt)));

    /* gobble_file check_stat color terms (ls.c 3342). */
    p->stat_dirs_for_color = color
        && (liszt_color_is_colored(LISZT_C_OTHER_WRITABLE)
            || liszt_color_is_colored(LISZT_C_STICKY)
            || liszt_color_is_colored(LISZT_C_STICKY_OTHER_WRITABLE));
    p->stat_exec = o->indicator_style == LISZT_IND_CLASSIFY
        || (color
            && (liszt_color_is_colored(LISZT_C_EXEC)
                || liszt_color_is_colored(LISZT_C_SETUID)
                || liszt_color_is_colored(LISZT_C_SETGID)));
    p->stat_links = (o->print_inode || color
                     || o->indicator_style != LISZT_IND_NONE
                     || o->group_directories_first || o->recursive)
        && (o->deref == LISZT_DEREF_ALWAYS
            || liszt_color_symlink_as_referent()
            || p->check_symlink_mode);
    /* Targets need mode when an @-capable indicator or symlink color
       logic is active (ls.c 3533: file_type <= indicator_style). */
    p->link_target_mode = o->indicator_style >= LISZT_IND_FILE_TYPE
        || p->check_symlink_mode;
    p->needs_link_target = p->needs_link_target || p->check_symlink_mode;
    p->cap_probe = color && liszt_color_is_colored(LISZT_C_CAP);
    /* Note: mh coloring gets nlink only when a stat happens anyway -
       GNU's check_stat has no mh term, so an mh-only scheme silently
       leaves plain files unstatted. Mirrored, not fixed. */
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

#ifndef LISZT_COLORS_H
#define LISZT_COLORS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "sys/dir.h"

/* GNU ls color machinery: LS_COLORS parsing (parse_ls_color +
   get_funky_string ports), the builtin indicator table, classification
   (get_color_indicator), and escape emission. All output flows through
   the emit module so dired accounting stays coherent. */

/* GNU order (enum indicator_no). */
enum liszt_cind {
    LISZT_C_LEFT = 0, LISZT_C_RIGHT, LISZT_C_END, LISZT_C_RESET,
    LISZT_C_NORM, LISZT_C_FILE, LISZT_C_DIR, LISZT_C_LINK, LISZT_C_FIFO,
    LISZT_C_SOCK, LISZT_C_BLK, LISZT_C_CHR, LISZT_C_MISSING,
    LISZT_C_ORPHAN, LISZT_C_EXEC, LISZT_C_DOOR, LISZT_C_SETUID,
    LISZT_C_SETGID, LISZT_C_STICKY, LISZT_C_OTHER_WRITABLE,
    LISZT_C_STICKY_OTHER_WRITABLE, LISZT_C_CAP, LISZT_C_MULTIHARDLINK,
    LISZT_C_CLR_TO_EOL
};

struct liszt_binstr {
    size_t len;
    const char *string;
};

/* Parse LS_COLORS. May clear *color_enabled: empty/unset LS_COLORS with
   no COLORTERM and unknown TERM, or an unparsable value (with GNU's
   diagnostics). Call once, after options resolution, when color might
   be on. */
void liszt_colors_parse(bool *color_enabled);

bool liszt_color_is_colored(enum liszt_cind ind);
bool liszt_color_symlink_as_referent(void);

/* The classification input: a file or a symlink target. linkok follows
   GNU: 1 target exists, 0 dangling, -1 missing (target view). */
struct liszt_colorable {
    const char *name;
    mode_t mode;
    enum liszt_ftype ftype;
    int linkok;
    bool stat_ok;
    bool has_capability;
    bool multi_hardlink;
};

/* get_color_indicator: the escape to paint with, or NULL. */
const struct liszt_binstr *liszt_color_for(const struct liszt_colorable *c);

/* Emission (put_indicator and friends); track first use for teardown. */
void liszt_color_put(const struct liszt_binstr *s);
void liszt_color_put_ind(enum liszt_cind ind);
void liszt_color_start(const struct liszt_binstr *seq);  /* LEFT seq RIGHT
    with the C_NORM reset dance */
void liszt_color_prep_non_filename(void);   /* C_END or LEFT RESET RIGHT */
void liszt_color_set_normal(void);
void liszt_color_restore_default(void);
bool liszt_color_used(void);
bool liszt_color_restore_is_noop(void);

#endif

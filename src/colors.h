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

/* LS_COLORS-style value decoder (get_funky_string port): decodes one
   \-escaped value from *SRC into *DEST, advancing both; stops at ':'
   or NUL (or '=' when EQUALS_END). For extension env parsers
   (LS_ICONS). */
bool liszt_funky_decode(char **dest, const char **src, bool equals_end,
                        size_t *output_count);

/* The classification half of get_color_indicator: which indicator
   class this file belongs to. Shared vocabulary for color and icon
   resolution (sprint 11 extraction; byte-identical behavior). */
enum liszt_cind liszt_file_class(const struct liszt_colorable *c);

/* get_color_indicator: the escape to paint with, or NULL. */
const struct liszt_binstr *liszt_color_for(const struct liszt_colorable *c);

/* Theme system (v0.3): the 53-key style surface. Keys follow the
   EZA_COLORS two-letter vocabulary; the built-in default is the
   16-color eza theme. liszt_theme_style() is the single accessor -
   selection and LISZT_COLORS overlays mutate the table at init only. */
enum liszt_theme_key {
    LISZT_TK_FI, LISZT_TK_DI, LISZT_TK_LN, LISZT_TK_PI, LISZT_TK_BD,
    LISZT_TK_CD, LISZT_TK_SO, LISZT_TK_EX, LISZT_TK_OR,
    LISZT_TK_UR, LISZT_TK_UW, LISZT_TK_UX, LISZT_TK_UE, LISZT_TK_GR,
    LISZT_TK_GW, LISZT_TK_GX, LISZT_TK_TR, LISZT_TK_TW, LISZT_TK_TX,
    LISZT_TK_SU, LISZT_TK_SF,
    LISZT_TK_LC, LISZT_TK_LM, LISZT_TK_UU, LISZT_TK_UN, LISZT_TK_GU,
    LISZT_TK_GN, LISZT_TK_NB, LISZT_TK_NK, LISZT_TK_NM, LISZT_TK_NG,
    LISZT_TK_NT, LISZT_TK_DF, LISZT_TK_DS,
    LISZT_TK_DA, LISZT_TK_IN, LISZT_TK_BL, LISZT_TK_XX,
    LISZT_TK_GA, LISZT_TK_GM, LISZT_TK_GD, LISZT_TK_GV, LISZT_TK_GT,
    LISZT_TK_GI, LISZT_TK_GC,
    LISZT_TK_IM, LISZT_TK_VI, LISZT_TK_MU, LISZT_TK_LO, LISZT_TK_CR,
    LISZT_TK_DO, LISZT_TK_CO, LISZT_TK_TM, LISZT_TK_CM, LISZT_TK_BU,
    LISZT_TK_SC,
    LISZT_TK_N
};

/* Select a preset ("default" = built-ins). False = unknown name. */
bool liszt_theme_select(const char *name);
/* All preset names, for the error listing. */
const char *const *liszt_theme_names(size_t *n);
/* Overlay LISZT_COLORS (LS_COLORS grammar over the key vocabulary);
   unknown keys or parse failure diagnose and drop the variable. */
void liszt_theme_env_overlay(void);
/* NULL when the key has no style (uncolored). */
const struct liszt_binstr *liszt_theme_style(enum liszt_theme_key k);
/* Replace the LS_COLORS-default filekind styles with the theme's
   (called by liszt_colors_parse when the scheme was empty). */
void liszt_theme_apply_filekinds(void);
bool liszt_theme_active(void);

/* --color=full filename-class fallback (v0.3): eza's filetype chain
   (readme prefix, exact name, lowercased last-dot extension, temp
   patterns) for regular files LS_COLORS left unstyled. NULL = none. */
const struct liszt_binstr *liszt_colorclass_for(const char *name,
                                                size_t len);

/* Bytes of escape sequences emitted so far: color output is invisible
   to dired offsets, which count everything else. */
off_t liszt_color_bytes(void);

/* Emission (put_indicator and friends); track first use for teardown. */
void liszt_color_put(const struct liszt_binstr *s);
void liszt_color_put_run(const struct liszt_binstr *const *seqs,
                         const char *chars, size_t n);
void liszt_color_put_token(const struct liszt_binstr *seq,
                           const char *bytes, size_t blen);
size_t liszt_color_build_run(const struct liszt_binstr *const *seqs,
                             const char *chars, size_t n, char *out,
                             size_t cap, off_t *esc);
void liszt_color_put_prebuilt(const char *bytes, size_t len, off_t esc);
void liszt_color_put_ind(enum liszt_cind ind);
void liszt_color_start(const struct liszt_binstr *seq);  /* LEFT seq RIGHT
    with the C_NORM reset dance */
void liszt_color_prep_non_filename(void);   /* C_END or LEFT RESET RIGHT */
void liszt_color_set_normal(void);
void liszt_color_restore_default(void);
bool liszt_color_used(void);
bool liszt_color_restore_is_noop(void);

#endif

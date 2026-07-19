#ifndef LISZT_ICONS_H
#define LISZT_ICONS_H

#include <stdbool.h>
#include <stddef.h>

#include "colors.h"

struct liszt_options;

/* Nerd-Font icon resolution (v0.2 extension): compiled-in curated
   tables (src/icons_tab.h) with LS_ICONS env overrides layered on
   top. Chain per tier: exact filename -> dirname (directories) ->
   suffix -> filetype default. Never touches the filesystem; builtin
   tables observe only name + classification, so icons ride the
   statless plan. */

struct liszt_icon {
    const char *bytes;      /* UTF-8 glyph, not NUL-terminated */
    unsigned len;           /* 0 = blank cell (spaces) */
};

/* Parse LS_ICONS / spacing / OSC 66 env once. Call after
   liszt_colors_parse when icons resolved on. */
void liszt_icons_init(const struct liszt_options *o);

/* Resolve the icon for one entry. */
void liszt_icon_for(const struct liszt_colorable *c, size_t name_len,
                    struct liszt_icon *out);

/* Spacing cells after the glyph (LISZT_ICON_SPACING, default 1). */
unsigned liszt_icon_spacing(void);

/* Whether to wrap glyphs in OSC 66 w=1 (LISZT_ICONS_OSC66=1). */
bool liszt_icon_osc66(void);

/* Whether the LS_ICONS scheme observes stat-needing filetype labels
   (ex/su/sg/ow/st/tw) - feeds liszt_plan_icons_update. */
bool liszt_icons_observe_stat(void);

#endif

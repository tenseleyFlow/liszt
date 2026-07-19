#ifndef LISZT_TIMEFMT_H
#define LISZT_TIMEFMT_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Long-format timestamp rendering with GNU's recent/older six-month
   cutoff and a per-(minute, side) memo - both formats resolve at minute
   granularity, so equal keys render identically by construction.
   Styles are a parameter from day one; sprint 07 plugs the --time-style
   word table and +FORMAT into liszt_timefmt_set_style. */

enum liszt_time_style {
    LISZT_TSTYLE_LOCALE = 0     /* "%b %e %H:%M" / "%b %e  %Y" */
};

void liszt_timefmt_set_style(enum liszt_time_style style);

/* Render WHEN into BUF (>= LISZT_TIME_BUFSZ). Returns the length, or 0
   when the time cannot be converted (caller prints the raw-seconds
   fallback). Lazily reads the clock on first use; a timestamp in the
   future refreshes it, as GNU does. */
#define LISZT_TIME_BUFSZ 64
size_t liszt_timefmt_render(char *buf, struct timespec when);

/* Width of a non-recent rendering of epoch 0 (GNU
   long_time_expected_width), for the raw-seconds fallback padding. */
int liszt_timefmt_expected_width(void);

#endif

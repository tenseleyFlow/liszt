#ifndef LISZT_TIMEFMT_H
#define LISZT_TIMEFMT_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Long-format timestamp rendering: GNU's recent/older six-month cutoff,
   the abformat aligned-month table (abformat_init), an nstrftime-subset
   %N expansion, and a memo whose granularity follows the format (minute
   for the default style, second/none for finer ones). */

/* Install the resolved --time-style formats (older side, recent side).
   Both strings must outlive the run. Without a call, GNU's untranslated
   "locale" defaults apply. Rebuilds the abformat table and the memo
   granularity. */
void liszt_timefmt_set_formats(const char *older, const char *recent);

/* Render WHEN into BUF (>= LISZT_TIME_BUFSZ). Returns the length, or
   (size_t)-1 when the time cannot be converted to a struct tm - only
   then does the caller print the raw-seconds fallback. A format that
   renders empty or overflows yields 0, which GNU prints as zero bytes
   plus the column space (ls.c: s stays >= 0). Lazily reads the clock on
   first use; a timestamp in the future refreshes it, as GNU does. */
#define LISZT_TIME_BUFSZ 1024
size_t liszt_timefmt_render(char *buf, struct timespec when);

/* Display columns of a non-recent rendering of epoch 0 (GNU
   long_time_expected_width), for the raw-seconds fallback padding. */
int liszt_timefmt_expected_width(void);

#endif

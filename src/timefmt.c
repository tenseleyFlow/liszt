#include "timefmt.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "util.h"

/* GNU's default "locale" style formats in non-hard LC_TIME locales. */
static const char *fmt_older = "%b %e  %Y";
static const char *fmt_recent = "%b %e %H:%M";

static struct timespec current_time = { 0, 0 };
static bool have_time;

/* Memo: open-addressed table keyed by (minute, recent-side). Cleared
   never - keys are absolute minutes, valid for the whole run. */
struct memo_ent {
    int64_t key;        /* minute * 2 + recent; 0 = empty (minute 0 old
                           collides only for the 1970-01-01 00:00 slot -
                           use +1 bias to keep 0 free) */
    char text[LISZT_TIME_BUFSZ];
    unsigned char len;
};

#define MEMO_SLOTS 1024
static struct memo_ent memo[MEMO_SLOTS];

void
liszt_timefmt_set_style(enum liszt_time_style style)
{
    (void)style;    /* single style until sprint 07 */
}

static int
timespec_cmp(struct timespec a, struct timespec b)
{
    if (a.tv_sec != b.tv_sec)
        return a.tv_sec < b.tv_sec ? -1 : 1;
    if (a.tv_nsec != b.tv_nsec)
        return a.tv_nsec < b.tv_nsec ? -1 : 1;
    return 0;
}

size_t
liszt_timefmt_render(char *buf, struct timespec when)
{
    if (!have_time) {
        clock_gettime(CLOCK_REALTIME, &current_time);
        have_time = true;
    }
    /* A future timestamp may mean the file changed since we read the
       clock; re-read it, as GNU does. */
    if (timespec_cmp(current_time, when) < 0) {
        clock_gettime(CLOCK_REALTIME, &current_time);
    }

    struct timespec six_months_ago = {
        current_time.tv_sec - 31556952 / 2,
        current_time.tv_nsec
    };
    bool recent = timespec_cmp(six_months_ago, when) < 0
        && timespec_cmp(when, current_time) < 0;

    int64_t key = ((int64_t)(when.tv_sec >= 0 ? when.tv_sec / 60
                             : (when.tv_sec - 59) / 60) * 2
                   + (recent ? 1 : 0)) + 1;
    size_t slot = (size_t)((uint64_t)key * 0x9E3779B97F4A7C15ull
                           % MEMO_SLOTS);
    for (size_t probe = 0; probe < MEMO_SLOTS; probe++) {
        struct memo_ent *m = &memo[(slot + probe) % MEMO_SLOTS];
        if (m->key == key) {
            memcpy(buf, m->text, m->len);
            buf[m->len] = '\0';
            return m->len;
        }
        if (m->key == 0) {
            struct tm tm;
            if (!localtime_r(&when.tv_sec, &tm))
                return 0;
            size_t n = strftime(buf, LISZT_TIME_BUFSZ,
                                recent ? fmt_recent : fmt_older, &tm);
            if (n == 0)
                return 0;
            m->key = key;
            m->len = (unsigned char)n;
            memcpy(m->text, buf, n + 1);
            return n;
        }
    }
    /* Table full (pathological): render uncached. */
    struct tm tm;
    if (!localtime_r(&when.tv_sec, &tm))
        return 0;
    return strftime(buf, LISZT_TIME_BUFSZ, recent ? fmt_recent : fmt_older,
                    &tm);
}

int
liszt_timefmt_expected_width(void)
{
    static int width = -1;

    if (width < 0) {
        time_t epoch = 0;
        struct tm tm;
        char buf[LISZT_TIME_BUFSZ];
        width = 0;
        if (localtime_r(&epoch, &tm)) {
            size_t n = strftime(buf, sizeof buf, fmt_older, &tm);
            if (n > 0)
                width = (int)n;     /* mbswidth for ASCII == bytes */
        }
    }
    return width;
}

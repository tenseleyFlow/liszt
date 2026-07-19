#include "timefmt.h"

#include <langinfo.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "quote.h"

/* GNU's untranslated "locale" style formats; --time-style swaps them. */
static const char *fmt_older = "%b %e  %Y";
static const char *fmt_recent = "%b %e %H:%M";

/* abformat port (ls.c abformat_init): when a format's first %b applies,
   precompute per-month formats with the abbreviation aligned to the
   widest month, so variable-width month names stay columnar. */
enum { ABFORMAT_SIZE = 128 };
static char abformat[2][12][ABFORMAT_SIZE];
static bool use_abformat;

/* Memo granularity, derived from the finest specifier in the formats.
   The default style resolves at minute granularity; %S/%T/%c/%s/%X/%r
   at second; %N (or anything unrecognized) disables the memo. */
enum granularity { GRAN_MINUTE, GRAN_SECOND, GRAN_NONE };
static enum granularity gran = GRAN_MINUTE;

static bool inited;

static struct timespec current_time = { 0, 0 };
static bool have_time;

struct memo_ent {
    int64_t key;        /* unit * 2 + recent, +1 bias so 0 = empty */
    char text[128];
    unsigned char len;
};

#define MEMO_SLOTS 1024
static struct memo_ent memo[MEMO_SLOTS];
static size_t memo_hits;
static size_t memo_misses;

static const char *
first_percent_b(const char *fmt)
{
    for (; *fmt; fmt++)
        if (fmt[0] == '%') {
            if (fmt[1] == 'b')
                return fmt;
            if (fmt[1] == '%')
                fmt++;
        }
    return NULL;
}

static bool
abmon_init(char abmon[12][ABFORMAT_SIZE])
{
    int max_mon_width = 0;
    int mon_width[12];
    size_t mon_len[12];

    for (int i = 0; i < 12; i++) {
        const char *abbr = nl_langinfo(ABMON_1 + i);
        mon_len[i] = strnlen(abbr, ABFORMAT_SIZE);
        if (mon_len[i] == ABFORMAT_SIZE)
            return false;
        if (strchr(abbr, '%'))
            return false;
        strcpy(abmon[i], abbr);
        mon_width[i] = liszt_mbsnwidth(abmon[i], mon_len[i]);
        if (mon_width[i] < 0)
            return false;
        if (mon_width[i] > max_mon_width)
            max_mon_width = mon_width[i];
    }

    for (int i = 0; i < 12; i++) {
        size_t fill = (size_t)(max_mon_width - mon_width[i]);
        if (ABFORMAT_SIZE - mon_len[i] <= fill)
            return false;
        bool align_left = !(abmon[i][0] >= '0' && abmon[i][0] <= '9');
        size_t fill_offset;
        if (align_left)
            fill_offset = mon_len[i];
        else {
            memmove(abmon[i] + fill, abmon[i], mon_len[i]);
            fill_offset = 0;
        }
        memset(abmon[i] + fill_offset, ' ', fill);
        abmon[i][mon_len[i] + fill] = '\0';
    }
    return true;
}

static void
abformat_init(void)
{
    const char *fmts[2] = { fmt_older, fmt_recent };
    const char *pb[2];

    use_abformat = false;
    for (int recent = 0; recent < 2; recent++)
        pb[recent] = first_percent_b(fmts[recent]);
    if (!pb[0] && !pb[1])
        return;

    char abmon[12][ABFORMAT_SIZE];
    if (!abmon_init(abmon))
        return;

    for (int recent = 0; recent < 2; recent++) {
        const char *fmt = fmts[recent];
        for (int i = 0; i < 12; i++) {
            char *nfmt = abformat[recent][i];
            int nbytes;

            if (!pb[recent])
                nbytes = snprintf(nfmt, ABFORMAT_SIZE, "%s", fmt);
            else {
                if (pb[recent] - fmt > ABFORMAT_SIZE)
                    return;
                int prefix_len = (int)(pb[recent] - fmt);
                nbytes = snprintf(nfmt, ABFORMAT_SIZE, "%.*s%s%s",
                                  prefix_len, fmt, abmon[i],
                                  pb[recent] + 2);
            }
            if (nbytes < 0 || nbytes >= ABFORMAT_SIZE)
                return;
        }
    }
    use_abformat = true;
}

static enum granularity
fmt_granularity(const char *fmt)
{
    enum granularity g = GRAN_MINUTE;

    for (; *fmt; fmt++) {
        if (fmt[0] != '%')
            continue;
        fmt++;
        /* Skip nstrftime flags and width; keep E/O modifiers' base. */
        while (*fmt == '-' || *fmt == '_' || *fmt == '0' || *fmt == '^'
               || *fmt == '#')
            fmt++;
        while (*fmt >= '0' && *fmt <= '9')
            fmt++;
        if (*fmt == 'E' || *fmt == 'O')
            fmt++;
        switch (*fmt) {
        case '\0':
            return g;
        case 'N':
            return GRAN_NONE;
        case 'c': case 'r': case 's': case 'S': case 'T': case 'X':
        case '+':
            if (g < GRAN_SECOND)
                g = GRAN_SECOND;
            break;
        case '%': case 'a': case 'A': case 'b': case 'B': case 'C':
        case 'd': case 'D': case 'e': case 'F': case 'g': case 'G':
        case 'h': case 'H': case 'I': case 'j': case 'k': case 'l':
        case 'm': case 'M': case 'n': case 'p': case 'P': case 'q':
        case 't': case 'u': case 'U': case 'V': case 'w': case 'W':
        case 'x': case 'y': case 'Y': case 'z': case 'Z':
            break;
        default:
            /* Unknown conversion: never memo what we can't classify. */
            return GRAN_NONE;
        }
    }
    return g;
}

static void
init(void)
{
    abformat_init();
    enum granularity a = fmt_granularity(fmt_older);
    enum granularity b = fmt_granularity(fmt_recent);
    gran = a > b ? a : b;
    inited = true;
}

void
liszt_timefmt_set_formats(const char *older, const char *recent)
{
    fmt_older = older;
    fmt_recent = recent;
    init();
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

/* nstrftime subset: expand %[width]N (nanoseconds; %3N = first three
   digits, gnulib-style) into digits, then let strftime do the rest.
   Returns 0 when the expansion or the rendering does not fit. */
static size_t
render_fmt(char *buf, size_t bufsz, const char *fmt, const struct tm *tm,
           long nsec)
{
    char expanded[LISZT_TIME_BUFSZ];
    size_t o = 0;

    for (const char *p = fmt; *p; p++) {
        if (o + 16 >= sizeof expanded)
            return 0;
        if (p[0] != '%') {
            expanded[o++] = *p;
            continue;
        }
        if (p[1] == '%') {
            expanded[o++] = '%';
            expanded[o++] = '%';
            p++;
            continue;
        }
        const char *spec = p + 1;
        size_t width = 0;
        bool have_width = false;
        while (*spec == '-' || *spec == '_' || *spec == '0' || *spec == '^'
               || *spec == '#')
            spec++;
        while (*spec >= '0' && *spec <= '9') {
            width = width * 10 + (size_t)(*spec - '0');
            have_width = true;
            spec++;
        }
        if (*spec == 'N') {
            char digits[10];
            snprintf(digits, sizeof digits, "%09ld", nsec);
            size_t w = have_width ? width : 9;
            if (o + w >= sizeof expanded)
                return 0;
            for (size_t i = 0; i < w; i++)
                expanded[o++] = i < 9 ? digits[i] : '0';
            p = spec;
            continue;
        }
        /* Any other conversion passes through to strftime verbatim. */
        expanded[o++] = '%';
        continue;
    }
    expanded[o] = '\0';

    /* strftime's 0 covers both "renders empty" and "does not fit";
       GNU treats either as a zero-length success (ls.c keeps s >= 0),
       so 0 propagates as a valid length here. */
    return strftime(buf, bufsz, expanded, tm);
}

static size_t
render_at(char *buf, bool recent, const struct tm *tm, long nsec)
{
    const char *fmt = use_abformat
        ? abformat[recent][tm->tm_mon]
        : (recent ? fmt_recent : fmt_older);
    return render_fmt(buf, LISZT_TIME_BUFSZ, fmt, tm, nsec);
}

size_t
liszt_timefmt_render(char *buf, struct timespec when)
{
    if (!inited)
        init();
    if (!have_time) {
        clock_gettime(CLOCK_REALTIME, &current_time);
        have_time = true;
    }
    /* A future timestamp may mean the file changed since we read the
       clock; re-read it, as GNU does. */
    if (timespec_cmp(current_time, when) < 0)
        clock_gettime(CLOCK_REALTIME, &current_time);

    struct timespec six_months_ago = {
        current_time.tv_sec - 31556952 / 2,
        current_time.tv_nsec
    };
    bool recent = timespec_cmp(six_months_ago, when) < 0
        && timespec_cmp(when, current_time) < 0;

    if (gran == GRAN_NONE) {
        struct tm tm;
        if (!localtime_r(&when.tv_sec, &tm))
            return (size_t)-1;
        return render_at(buf, recent, &tm, when.tv_nsec);
    }

    int64_t unit = gran == GRAN_MINUTE
        ? (when.tv_sec >= 0 ? when.tv_sec / 60 : (when.tv_sec - 59) / 60)
        : (int64_t)when.tv_sec;
    int64_t key = (unit * 2 + (recent ? 1 : 0)) + 1;
    size_t slot = (size_t)((uint64_t)key * 0x9E3779B97F4A7C15ull
                           % MEMO_SLOTS);
    for (size_t probe = 0; probe < MEMO_SLOTS; probe++) {
        struct memo_ent *m = &memo[(slot + probe) % MEMO_SLOTS];
        if (m->key == key) {
            memo_hits++;
            memcpy(buf, m->text, m->len);
            buf[m->len] = '\0';
            return m->len;
        }
        if (m->key == 0) {
            struct tm tm;
            memo_misses++;
            if (!localtime_r(&when.tv_sec, &tm))
                return (size_t)-1;
            size_t n = render_at(buf, recent, &tm, when.tv_nsec);
            if (n < sizeof m->text) {
                m->key = key;
                m->len = (unsigned char)n;
                memcpy(m->text, buf, n + 1);
            }
            return n;
        }
    }
    /* Table full (pathological): render uncached. */
    struct tm tm;
    if (!localtime_r(&when.tv_sec, &tm))
        return (size_t)-1;
    return render_at(buf, recent, &tm, when.tv_nsec);
}

void
liszt_timefmt_stats(void)
{
    fprintf(stderr, "timefmt: memo hits=%zu misses=%zu gran=%d\n",
            memo_hits, memo_misses, (int)gran);
}

int
liszt_timefmt_expected_width(void)
{
    static int width = -1;

    if (width < 0) {
        if (!inited)
            init();
        time_t epoch = 0;
        struct tm tm;
        char buf[LISZT_TIME_BUFSZ];
        width = 0;
        if (localtime_r(&epoch, &tm)) {
            size_t n = render_at(buf, false, &tm, 0);
            if (n > 0) {
                int w = liszt_mbsnwidth(buf, n);
                if (w > 0)
                    width = w;
            }
        }
    }
    return width;
}

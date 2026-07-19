#include "quote.h"

#include <ctype.h>
#include <langinfo.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "config.h"
#include "uniwidth.h"
#include "util.h"

/* wchar_t is UTF-32 on every supported platform (Linux glibc/musl,
   macOS, FreeBSD), so mbrtowc/iswprint/wcwidth stand in for the
   mbrtoc32/c32isprint/c32width calls in the pinned tree. Width follows
   gnulib's per-platform decision: where libc wcwidth fails the
   conformance probe (LISZT_REPLACE_WCWIDTH - macOS), UTF-8 locales
   route through the ported uniwidth tables like rpl_wcwidth does;
   printability stays libc iswprint everywhere (c32isprint is
   WCHAR_FUNC on both glibc and BSD paths). */

#if LISZT_REPLACE_WCWIDTH
static int
locale_is_utf8(void)
{
    static int cached = -1;

    if (cached < 0)
        cached = strcmp(nl_langinfo(CODESET), "UTF-8") == 0;
    return cached;
}
#endif

static int
backend_wcwidth(wchar_t wc)
{
#if LISZT_REPLACE_WCWIDTH
    if (locale_is_utf8())
        return liszt_uc_width((uint32_t)wc);
#endif
    return wcwidth(wc);
}

enum { INT_BITS = (int)(sizeof (int) * CHAR_BIT) };

void
liszt_set_char_quoting(struct liszt_qopts *o, char c, int on)
{
    unsigned char uc = (unsigned char)c;
    unsigned int *w = &o->quote_these_too[uc / INT_BITS];
    unsigned int bit = 1u << (uc % INT_BITS);

    if (on)
        *w |= bit;
    else
        *w &= ~bit;
}

/* tally's lazy BMP cache: one wcwidth call per code point per run. */
static int
cached_wcwidth(wchar_t wc)
{
    static signed char *tab;    /* wcwidth + 2, 0 = unfilled */

    if ((unsigned long)wc >= 0x10000ul)
        return backend_wcwidth(wc);
    if (!tab) {
        tab = liszt_xmalloc(0x10000);
        memset(tab, 0, 0x10000);
    }
    if (tab[wc] == 0) {
        int w = backend_wcwidth(wc);
        tab[wc] = (signed char)(w + 2);
    }
    return tab[wc] - 2;
}

static int
cached_iswprint(wchar_t wc)
{
    static signed char *tab;    /* iswprint + 1, 0 = unfilled */

    /* Printability mirrors c32isprint, which dispatches to libc
       iswprint on glibc and BSD alike - never derive it from width:
       Darwin's wcwidth(ZWJ) is 0 while its iswprint says no, and the
       uniwidth backend gives unassigned code points width 1. */
    if ((unsigned long)wc >= 0x10000ul)
        return iswprint((wint_t)wc);
    if (!tab) {
        tab = liszt_xmalloc(0x10000);
        memset(tab, 0, 0x10000);
    }
    if (tab[wc] == 0)
        tab[wc] = (signed char)(iswprint((wint_t)wc) ? 2 : 1);
    return tab[wc] - 1;
}

/* gettext_quote reduced: no message catalogs; UTF-8 locales get curly
   quotes, else clocale quotes with '"' and locale with "'". */
static const char *
gettext_quote(bool left, enum liszt_qstyle s)
{
    const char *cs = nl_langinfo(CODESET);

    if (cs && strcmp(cs, "UTF-8") == 0)
        return left ? "\342\200\230" : "\342\200\231";
    return s == LISZT_QS_CLOCALE ? "\"" : "'";
}

/* Faithful port of quotearg_buffer_restyled (lib/quotearg.c, pinned
   tree): same control flow, same gotos, same store discipline. */
static size_t
quotearg_restyled(char *buffer, size_t buffersize, const char *arg,
                  size_t argsize, enum liszt_qstyle quoting_style,
                  const unsigned int *quote_these_too)
{
    bool unibyte_locale = MB_CUR_MAX == 1;
    size_t len = 0;
    size_t orig_buffersize = 0;
    const char *quote_string = NULL;
    size_t quote_string_len = 0;
    bool backslash_escapes = false;
    bool elide_outer_quotes = false;
    bool encountered_single_quote = false;
    bool all_c_and_shell_quote_compat = true;
    const char *left_quote = NULL;
    const char *right_quote = NULL;
    bool pending_shell_escape_end;

#define STORE(ch) \
    do { \
        if (len < buffersize) \
            buffer[len] = (ch); \
        len++; \
    } while (0)

#define START_ESC() \
    do { \
        if (elide_outer_quotes) \
            goto force_outer_quoting_style; \
        escaping = true; \
        if (quoting_style == LISZT_QS_SHELL_ALWAYS \
            && !pending_shell_escape_end) { \
            STORE('\''); \
            STORE('$'); \
            STORE('\''); \
            pending_shell_escape_end = true; \
        } \
        STORE('\\'); \
    } while (0)

#define END_ESC() \
    do { \
        if (pending_shell_escape_end && !escaping) { \
            STORE('\''); \
            STORE('\''); \
            pending_shell_escape_end = false; \
        } \
    } while (0)

process_input:
    pending_shell_escape_end = false;

    switch (quoting_style) {
    case LISZT_QS_C_MAYBE:
        quoting_style = LISZT_QS_C;
        elide_outer_quotes = true;
        /* fall through */
    case LISZT_QS_C:
        if (!elide_outer_quotes)
            STORE('"');
        backslash_escapes = true;
        quote_string = "\"";
        quote_string_len = 1;
        break;

    case LISZT_QS_ESCAPE:
        backslash_escapes = true;
        elide_outer_quotes = false;
        break;

    case LISZT_QS_LOCALE:
    case LISZT_QS_CLOCALE:
        left_quote = gettext_quote(true, quoting_style);
        right_quote = gettext_quote(false, quoting_style);
        if (!elide_outer_quotes)
            for (const char *lq = left_quote; *lq; lq++)
                STORE(*lq);
        backslash_escapes = true;
        quote_string = right_quote;
        quote_string_len = strlen(quote_string);
        break;

    case LISZT_QS_SHELL_ESCAPE:
        backslash_escapes = true;
        /* fall through */
    case LISZT_QS_SHELL:
        elide_outer_quotes = true;
        /* fall through */
    case LISZT_QS_SHELL_ESCAPE_ALWAYS:
        if (!elide_outer_quotes)
            backslash_escapes = true;
        /* fall through */
    case LISZT_QS_SHELL_ALWAYS:
        quoting_style = LISZT_QS_SHELL_ALWAYS;
        if (!elide_outer_quotes)
            STORE('\'');
        quote_string = "'";
        quote_string_len = 1;
        break;

    case LISZT_QS_LITERAL:
    default:
        elide_outer_quotes = false;
        break;
    }

    for (size_t i = 0;
         !(argsize == (size_t)-1 ? arg[i] == '\0' : i == argsize); i++) {
        bool is_right_quote = false;
        bool escaping = false;
        bool c_and_shell_quote_compat = false;
        unsigned char c;
        unsigned char esc;

        if (backslash_escapes && quoting_style != LISZT_QS_SHELL_ALWAYS
            && quote_string_len
            && (i + quote_string_len
                <= (argsize == (size_t)-1 && 1 < quote_string_len
                    ? (argsize = strlen(arg)) : argsize))
            && memcmp(arg + i, quote_string, quote_string_len) == 0) {
            if (elide_outer_quotes)
                goto force_outer_quoting_style;
            is_right_quote = true;
        }

        c = (unsigned char)arg[i];
        switch (c) {
        case '\0':
            if (backslash_escapes) {
                START_ESC();
                if (quoting_style != LISZT_QS_SHELL_ALWAYS
                    && i + 1 < argsize && '0' <= arg[i + 1]
                    && arg[i + 1] <= '9') {
                    STORE('0');
                    STORE('0');
                }
                c = '0';
            }
            break;

        case '?':
            if (quoting_style == LISZT_QS_SHELL_ALWAYS
                && elide_outer_quotes)
                goto force_outer_quoting_style;
            break;

        case '\a': esc = 'a'; goto c_escape;
        case '\b': esc = 'b'; goto c_escape;
        case '\f': esc = 'f'; goto c_escape;
        case '\n': esc = 'n'; goto c_and_shell_escape;
        case '\r': esc = 'r'; goto c_and_shell_escape;
        case '\t': esc = 't'; goto c_and_shell_escape;
        case '\v': esc = 'v'; goto c_escape;
        case '\\':
            esc = c;
            if (quoting_style == LISZT_QS_SHELL_ALWAYS) {
                if (elide_outer_quotes)
                    goto force_outer_quoting_style;
                goto store_c;
            }
            if (backslash_escapes && elide_outer_quotes
                && quote_string_len)
                goto store_c;
        c_and_shell_escape:
            if (quoting_style == LISZT_QS_SHELL_ALWAYS
                && elide_outer_quotes)
                goto force_outer_quoting_style;
            /* fall through */
        c_escape:
            if (backslash_escapes) {
                c = esc;
                goto store_escape;
            }
            break;

        case '{': case '}':     /* sometimes special if isolated */
            if (!(argsize == (size_t)-1 ? arg[1] == '\0' : argsize == 1))
                break;
            /* fall through */
        case '#': case '~':
            if (i != 0)
                break;
            /* fall through */
        case ' ':
            c_and_shell_quote_compat = true;
            /* fall through */
        case '!':
        case '"': case '$': case '&':
        case '(': case ')': case '*': case ';':
        case '<':
        case '=':
        case '>': case '[':
        case '^':
        case '`': case '|':
            if (quoting_style == LISZT_QS_SHELL_ALWAYS
                && elide_outer_quotes)
                goto force_outer_quoting_style;
            break;

        case '\'':
            encountered_single_quote = true;
            c_and_shell_quote_compat = true;
            if (quoting_style == LISZT_QS_SHELL_ALWAYS) {
                if (elide_outer_quotes)
                    goto force_outer_quoting_style;
                if (buffersize && !orig_buffersize) {
                    orig_buffersize = buffersize;
                    buffersize = 0;
                }
                STORE('\'');
                STORE('\\');
                STORE('\'');
                pending_shell_escape_end = false;
            }
            break;

        case '%': case '+': case ',': case '-': case '.': case '/':
        case '0': case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9': case ':':
        case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
        case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
        case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
        case 'S': case 'T': case 'U': case 'V': case 'W': case 'X':
        case 'Y': case 'Z': case ']': case '_': case 'a': case 'b':
        case 'c': case 'd': case 'e': case 'f': case 'g': case 'h':
        case 'i': case 'j': case 'k': case 'l': case 'm': case 'n':
        case 'o': case 'p': case 'q': case 'r': case 's': case 't':
        case 'u': case 'v': case 'w': case 'x': case 'y': case 'z':
            c_and_shell_quote_compat = true;
            break;

        default: {
            size_t m;
            bool printable;

            if (unibyte_locale) {
                m = 1;
                printable = isprint(c) != 0;
            } else {
                mbstate_t mbs;
                memset(&mbs, 0, sizeof mbs);
                m = 0;
                printable = true;
                if (argsize == (size_t)-1)
                    argsize = strlen(arg);
                for (;;) {
                    wchar_t w;
                    size_t bytes = mbrtowc(&w, &arg[i + m],
                                           argsize - (i + m), &mbs);
                    if (bytes == 0) {
                        break;
                    } else if (bytes == (size_t)-1) {
                        printable = false;
                        break;
                    } else if (bytes == (size_t)-2) {
                        printable = false;
                        while (i + m < argsize && arg[i + m])
                            m++;
                        break;
                    } else {
                        if (elide_outer_quotes
                            && quoting_style == LISZT_QS_SHELL_ALWAYS) {
                            for (size_t j = 1; j < bytes; j++)
                                switch (arg[i + m + j]) {
                                case '[': case '\\': case '^':
                                case '`': case '|':
                                    goto force_outer_quoting_style;
                                }
                        }
                        if (!cached_iswprint(w))
                            printable = false;
                        m += bytes;
                    }
                    break;
                }
            }
            c_and_shell_quote_compat = printable;

            if (1 < m || (backslash_escapes && !printable)) {
                size_t ilim = i + m;

                for (;;) {
                    if (backslash_escapes && !printable) {
                        START_ESC();
                        STORE((char)('0' + (c >> 6)));
                        STORE((char)('0' + ((c >> 3) & 7)));
                        c = (unsigned char)('0' + (c & 7));
                    } else if (is_right_quote) {
                        STORE('\\');
                        is_right_quote = false;
                    }
                    if (ilim <= i + 1)
                        break;
                    END_ESC();
                    STORE((char)c);
                    c = (unsigned char)arg[++i];
                }
                goto store_c;
            }
        }
        }

        if (!(((backslash_escapes
                && quoting_style != LISZT_QS_SHELL_ALWAYS)
               || elide_outer_quotes)
              && quote_these_too
              && quote_these_too[c / INT_BITS] >> (c % INT_BITS) & 1)
            && !is_right_quote)
            goto store_c;

    store_escape:
        START_ESC();

    store_c:
        END_ESC();
        STORE((char)c);

        if (!c_and_shell_quote_compat)
            all_c_and_shell_quote_compat = false;
    }

    if (len == 0 && quoting_style == LISZT_QS_SHELL_ALWAYS
        && elide_outer_quotes)
        goto force_outer_quoting_style;

    if (quoting_style == LISZT_QS_SHELL_ALWAYS && !elide_outer_quotes
        && encountered_single_quote) {
        if (all_c_and_shell_quote_compat)
            return quotearg_restyled(buffer, orig_buffersize, arg, argsize,
                                     LISZT_QS_C, quote_these_too);
        else if (!buffersize && orig_buffersize) {
            buffersize = orig_buffersize;
            len = 0;
            goto process_input;
        }
    }

    if (quote_string && !elide_outer_quotes)
        for (; *quote_string; quote_string++)
            STORE(*quote_string);

    if (len < buffersize)
        buffer[len] = '\0';
    return len;

force_outer_quoting_style:
    if (quoting_style == LISZT_QS_SHELL_ALWAYS && backslash_escapes)
        quoting_style = LISZT_QS_SHELL_ESCAPE_ALWAYS;
    return quotearg_restyled(buffer, buffersize, arg, argsize,
                             quoting_style, NULL);

#undef STORE
#undef START_ESC
#undef END_ESC
}

size_t
liszt_quotearg_buffer(char *buf, size_t bufsize, const char *arg,
                      size_t argsize, const struct liszt_qopts *o)
{
    return quotearg_restyled(buf, bufsize, arg, argsize, o->style,
                             o->quote_these_too);
}

/* mbsnwidth port with both reject flags hard-enabled (ls's
   MBSWIDTH_FLAGS). */
int
liszt_mbsnwidth(const char *s, size_t n)
{
    const char *p = s;
    const char *plimit = s + n;
    int width = 0;

    if (MB_CUR_MAX > 1) {
        while (p < plimit) {
            unsigned char c = (unsigned char)*p;
            if (c >= 0x20 && c < 0x7f) {
                p++;
                width++;
                continue;
            }
            {
                mbstate_t mbs;
                memset(&mbs, 0, sizeof mbs);
                wchar_t wc;
                size_t bytes = mbrtowc(&wc, p, (size_t)(plimit - p), &mbs);
                if (bytes == (size_t)-1 || bytes == (size_t)-2)
                    return -1;
                if (bytes == 0)
                    bytes = 1;
                int w = cached_wcwidth(wc);
                if (w < 0)
                    return -1;
                if (w > INT_MAX - width)
                    return -1;
                width += w;
                p += bytes;
            }
        }
        return width;
    }

    while (p < plimit) {
        unsigned char c = (unsigned char)*p++;
        if (!isprint(c))
            return -1;
        width++;
    }
    return width;
}

/* --- the ls quote_name pipeline --------------------------------------- */

static char *qbuf;
static size_t qbuf_cap;

static void
qbuf_reserve(size_t want)
{
    if (want > qbuf_cap) {
        size_t cap = qbuf_cap ? qbuf_cap : 256;
        while (cap < want)
            cap += cap / 2;
        qbuf = liszt_xrealloc(qbuf, cap);
        qbuf_cap = cap;
    }
}

const char *
liszt_quote_name(const char *name, const struct liszt_qopts *opts,
                 bool qmark_funny, bool want_width, size_t *len_out,
                 int *width_out, bool *quoted_out)
{
    size_t len;
    bool quoted;
    int displayed_width = 0;
    bool needs_general = opts->style != LISZT_QS_LITERAL;
    bool needs_further = qmark_funny
        && (opts->style == LISZT_QS_SHELL
            || opts->style == LISZT_QS_SHELL_ALWAYS
            || opts->style == LISZT_QS_LITERAL);

    if (!needs_general)
        for (int i = 0; i < 8 && !needs_general; i++)
            if (opts->quote_these_too[i])
                needs_general = true;

    if (needs_general) {
        len = liszt_quotearg_buffer(qbuf, qbuf_cap, name, (size_t)-1,
                                    opts);
        if (qbuf_cap <= len) {
            qbuf_reserve(len + 1);
            liszt_quotearg_buffer(qbuf, qbuf_cap, name, (size_t)-1, opts);
        }
        quoted = (*name != *qbuf) || strlen(name) != len;
    } else {
        len = strlen(name);
        qbuf_reserve(len + 1);
        memcpy(qbuf, name, len + 1);
        quoted = false;
    }

    if (needs_further) {
        if (MB_CUR_MAX > 1) {
            const char *p = qbuf;
            const char *plimit = qbuf + len;
            char *q = qbuf;
            displayed_width = 0;

            while (p < plimit) {
                unsigned char c = (unsigned char)*p;
                if (c >= 0x20 && c < 0x7f) {
                    *q++ = (char)c;
                    p++;
                    displayed_width += 1;
                    continue;
                }
                {
                    mbstate_t mbstate;
                    memset(&mbstate, 0, sizeof mbstate);
                    do {
                        wchar_t wc;
                        size_t bytes;
                        int w;

                        bytes = mbrtowc(&wc, p, (size_t)(plimit - p),
                                        &mbstate);
                        if (bytes == (size_t)-1) {
                            p++;
                            *q++ = '?';
                            displayed_width += 1;
                            break;
                        }
                        if (bytes == (size_t)-2) {
                            p = plimit;
                            *q++ = '?';
                            displayed_width += 1;
                            break;
                        }
                        if (bytes == 0)
                            bytes = 1;
                        w = cached_wcwidth(wc);
                        if (w >= 0) {
                            for (; bytes > 0; --bytes)
                                *q++ = *p++;
                            displayed_width += w;
                        } else {
                            p += bytes;
                            *q++ = '?';
                            displayed_width += 1;
                        }
                    } while (!mbsinit(&mbstate));
                }
            }
            len = (size_t)(q - qbuf);
        } else {
            char *p = qbuf;
            const char *plimit = qbuf + len;

            while (p < plimit) {
                if (!isprint((unsigned char)*p))
                    *p = '?';
                p++;
            }
            displayed_width = (int)len;
        }
    } else if (want_width) {
        if (MB_CUR_MAX > 1) {
            /* Deviation D1 (.docs/deviations.md): GNU assigns
               mbsnwidth's -1 into a size_t before its MAX(0,..) clamp,
               so rejected names carry SIZE_MAX and wrap through layout
               arithmetic (broken alignment near control-byte and
               invalid-multibyte names). liszt honors the clamp GNU's
               dead code intended. */
            displayed_width = liszt_mbsnwidth(qbuf, len);
            if (displayed_width < 0)
                displayed_width = 0;
        } else {
            const char *p = qbuf;
            const char *plimit = qbuf + len;

            displayed_width = 0;
            while (p < plimit) {
                if (isprint((unsigned char)*p))
                    displayed_width++;
                p++;
            }
        }
    }

    *len_out = len;
    if (width_out)
        *width_out = displayed_width;
    *quoted_out = quoted;
    return qbuf;
}

#ifndef LISZT_QUOTE_H
#define LISZT_QUOTE_H

#include <stdbool.h>
#include <stddef.h>

/* Ports from the pinned tree: gnulib quotearg (the full quoting-style
   state machine), gnulib mbswidth (display-width measurement with
   reject-invalid/unprintable semantics), and ls.c's quote_name pipeline
   (style rendering, -q qmark replacement, width caching, outer-quote
   pad). */

/* GNU order (quoting_style_args); argmatch values for --quoting-style. */
enum liszt_qstyle {
    LISZT_QS_LITERAL = 0,
    LISZT_QS_SHELL,
    LISZT_QS_SHELL_ALWAYS,
    LISZT_QS_SHELL_ESCAPE,
    LISZT_QS_SHELL_ESCAPE_ALWAYS,
    LISZT_QS_C,
    LISZT_QS_C_MAYBE,
    LISZT_QS_ESCAPE,
    LISZT_QS_LOCALE,
    LISZT_QS_CLOCALE
};

struct liszt_qopts {
    enum liszt_qstyle style;
    unsigned int quote_these_too[8];    /* 256-bit set */
};

void liszt_set_char_quoting(struct liszt_qopts *o, char c, int on);

/* quotearg_buffer semantics: writes at most BUFSIZE bytes (always
   NUL-terminating when it fits), returns the full quoted length. */
size_t liszt_quotearg_buffer(char *buf, size_t bufsize, const char *arg,
                             size_t argsize, const struct liszt_qopts *o);

/* mbsnwidth with MBSW_REJECT_INVALID|MBSW_REJECT_UNPRINTABLE: -1 when
   any invalid or unprintable character appears. */
int liszt_mbsnwidth(const char *s, size_t n);

/* The ls quote_name pipeline over NAME: general quoting per OPTS, then
   -q qmark replacement when QMARK_FUNNY (only meaningful for literal
   and shell styles, per GNU), display width when WANT_WIDTH. Returns a
   pointer to an internal buffer valid until the next call; *LEN_OUT is
   its byte length, *QUOTED_OUT whether outer quotes/escapes changed the
   name (drives pad alignment). */
const char *liszt_quote_name(const char *name,
                             const struct liszt_qopts *opts,
                             bool qmark_funny, bool want_width,
                             size_t *len_out, int *width_out,
                             bool *quoted_out);

#endif

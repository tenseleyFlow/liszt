#ifndef LISZT_UTIL_H
#define LISZT_UTIL_H

#include <stddef.h>

/* Exit codes shared with GNU ls: 0 ok, 1 minor (e.g. unreadable
   subdirectory), 2 serious (bad option, inaccessible operand). */
enum {
    LISZT_STATUS_OK = 0,
    LISZT_STATUS_MINOR = 1,
    LISZT_STATUS_SERIOUS = 2
};

#if defined(__GNUC__) || defined(__clang__)
#define LISZT_PRINTF(fmt_idx, arg_idx) \
    __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define LISZT_PRINTF(fmt_idx, arg_idx)
#endif

/* Two program-name layers, matching GNU ls exactly (verified vs 9.11):
   error-layer diagnostics ("cannot access ...") prefix the BASENAME;
   getopt-layer diagnostics ("unrecognized option") and the Try/usage
   lines use argv[0] VERBATIM. */
extern const char *liszt_prog;      /* basename */
extern const char *liszt_argv0;     /* verbatim argv[0] */
void liszt_set_program(const char *argv0);

/* Locale-dependent quote marks for argmatch-class diagnostics: U+2018/19
   under UTF-8, ASCII apostrophes otherwise. Call after setlocale. The
   shell-escape quoting used by "cannot access"-class messages is ASCII
   quotes in every locale (that one is liszt_quote_sh below). */
void liszt_diag_init(void);
const char *liszt_qL(void);
const char *liszt_qR(void);

/* "Try 'ARGV0 --help' for more information." to stderr. The _die form
   exits 2 (getopt-layer). argmatch-layer errors print the same line but
   exit 1 - GNU's exit_failure default, pinned against 9.11. */
void liszt_try_help_print(void);
_Noreturn void liszt_try_help_and_die(void);

/* Diagnostic to stderr: "prog: msg" plus ": strerror" when errnum != 0. */
void liszt_error(int errnum, const char *fmt, ...) LISZT_PRINTF(2, 3);
_Noreturn void liszt_die(int status, int errnum, const char *fmt, ...)
    LISZT_PRINTF(3, 4);

void *liszt_xmalloc(size_t n);
void *liszt_xrealloc(void *p, size_t n);

#endif

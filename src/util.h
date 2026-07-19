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

/* Basename of argv[0]; used as the diagnostic prefix so lz reports as lz. */
extern const char *liszt_prog;
void liszt_set_program(const char *argv0);

/* Diagnostic to stderr: "prog: msg" plus ": strerror" when errnum != 0. */
void liszt_error(int errnum, const char *fmt, ...) LISZT_PRINTF(2, 3);
void liszt_die(int status, int errnum, const char *fmt, ...) LISZT_PRINTF(3, 4);

void *liszt_xmalloc(size_t n);
void *liszt_xrealloc(void *p, size_t n);

#endif

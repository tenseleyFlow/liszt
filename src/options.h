#ifndef LISZT_OPTIONS_H
#define LISZT_OPTIONS_H

#include <stdbool.h>

#include "dirread.h"

/* Values mirror GNU ls's enums where order matters (format/sort words). */
enum liszt_format {
    LISZT_FMT_MANY = 0,
    LISZT_FMT_HORIZONTAL,
    LISZT_FMT_ONE,
    LISZT_FMT_COMMAS,
    LISZT_FMT_LONG
};

enum liszt_sortword {
    LISZT_SORT_NAME = 0,
    LISZT_SORT_EXTENSION,
    LISZT_SORT_WIDTH,
    LISZT_SORT_SIZE,
    LISZT_SORT_VERSION,
    LISZT_SORT_TIME,
    LISZT_SORT_NONE
};

enum liszt_deref {
    LISZT_DEREF_UNDEFINED = 0,
    LISZT_DEREF_NEVER,
    LISZT_DEREF_COMMAND_LINE_ARGUMENTS,
    LISZT_DEREF_COMMAND_LINE_SYMLINK_TO_DIR,
    LISZT_DEREF_ALWAYS
};

struct liszt_options {
    enum liszt_format format;
    enum liszt_sortword sort;
    enum liszt_ignore_mode ignore;
    enum liszt_deref deref;
    bool reverse;
    bool group_directories_first;
    bool immediate_dirs;
    char **operands;    /* argv-order pointers into argv; xmalloc'd array */
    int n_operands;
};

/* Parse argv with GNU getopt permutation semantics (POSIXLY_CORRECT
   stops at the first non-option). Exits directly for --help/--version
   (0), parse errors (2, GNU-shaped diagnostics), and options whose
   sprint has not landed yet (2). On return *o is fully resolved. */
void liszt_options_parse(int argc, char **argv, struct liszt_options *o);

#endif

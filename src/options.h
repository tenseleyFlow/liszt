#ifndef LISZT_OPTIONS_H
#define LISZT_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dirread.h"
#include "quote.h"
#include "sys/xstat.h"

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

enum liszt_indicator_style {
    LISZT_IND_NONE = 0,
    LISZT_IND_SLASH,        /* -p */
    LISZT_IND_FILE_TYPE,    /* --file-type */
    LISZT_IND_CLASSIFY      /* -F */
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
    enum liszt_timetype time_type;
    enum liszt_ignore_mode ignore;
    /* --hide and -I/-B pattern lists (xmalloc'd, argv-backed strings);
       hide is inert outside the default ignore mode. */
    char **hide_patterns;
    int n_hide_patterns;
    char **ignore_patterns;
    int n_ignore_patterns;
    enum liszt_deref deref;
    bool reverse;
    bool group_directories_first;
    bool immediate_dirs;
    bool recursive;
    /* Long-format field selection. */
    bool print_owner;
    bool print_group;
    bool print_author;
    bool print_scontext;        /* -Z: context column/field */
    bool numeric_ids;
    /* Columns valid in every format. */
    bool print_block_size;      /* -s */
    bool print_inode;           /* -i */
    /* Block-size family: (opts, size) pairs for block counts (totals,
       -s) and for -l file sizes, resolved per GNU's env chain. */
    int human_output_opts;
    uintmax_t output_block_size;
    int file_human_output_opts;
    uintmax_t file_output_block_size;
    /* Layout and quoting (sprint 04). */
    bool dired;                 /* -D after self-disable rules */
    char eolbyte;               /* '\n'; --zero makes it NUL */
    size_t line_length;         /* 0 = unlimited */
    size_t max_idx;
    size_t tabsize;
    enum liszt_qstyle quoting_style;
    bool print_with_color;
    bool print_hyperlink;
    bool print_icons;           /* --icons extension (v0.2) */
    bool tree;                  /* --tree structural mode (v0.2) */
    size_t tree_level;          /* 0 = unlimited */
    size_t tree_limit;          /* 0 = uncapped */
    bool tree_unicode;          /* branch glyph charset */
    bool show_git;              /* --git column (long only, v0.2) */
    bool git_ignore;            /* hide ignored entries (v0.2) */
    bool color_full;            /* --color=full metadata theme (v0.3) */
    const char *theme;          /* preset name or NULL (v0.3) */
    bool theme_from_flag;       /* flag = hard error; env = soft */
    enum liszt_indicator_style indicator_style;
    bool qmark_funny_chars;
    bool align_variable_outer_quotes;
    struct liszt_qopts filename_qopts;
    struct liszt_qopts dirname_qopts;
    char **operands;    /* argv-order pointers into argv; xmalloc'd array */
    int n_operands;
};

/* Parse argv with GNU getopt permutation semantics (POSIXLY_CORRECT
   stops at the first non-option). Exits directly for --help/--version
   (0), parse errors (2, GNU-shaped diagnostics), and options whose
   sprint has not landed yet (2). On return *o is fully resolved. */
void liszt_options_parse(int argc, char **argv, struct liszt_options *o);

/* The --quoting-style word for STYLE (GNU quoting_style_args order),
   for the //DIRED-OPTIONS// line. */
const char *liszt_quoting_style_word(enum liszt_qstyle style);

#endif

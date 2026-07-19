#include "options.h"

#include <errno.h>
#include <limits.h>
#include <langinfo.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "config.h"
#include "human.h"
#include "timefmt.h"
#include "util.h"

/* Hand-rolled parser producing byte-identical diagnostics to GNU ls's
   getopt/argmatch stack (verified against coreutils 9.11):
   - getopt-layer messages prefix argv[0] verbatim;
   - long options match by unambiguous abbreviation, ambiguity listing
     candidates in table order;
   - argmatch words match by abbreviation too, with the locale-quoted
     "invalid/ambiguous argument" + "Valid arguments are:" block.
   The full 9.11 option surface is recognized; options owned by later
   sprints exit 2 with a clear not-supported diagnostic. */

enum {
    KEY_AUTHOR = 256,
    KEY_BLOCK_SIZE,
    KEY_COLOR,
    KEY_DEREF_CL_SYMLINK_TO_DIR,
    KEY_FILE_TYPE,
    KEY_FORMAT,
    KEY_FULL_TIME,
    KEY_GROUP_DIRS_FIRST,
    KEY_HIDE,
    KEY_HYPERLINK,
    KEY_INDICATOR_STYLE,
    KEY_QUOTING_STYLE,
    KEY_SHOW_CONTROL_CHARS,
    KEY_SI,
    KEY_SORT,
    KEY_TIME,
    KEY_TIME_STYLE,
    KEY_ZERO,
    KEY_HELP,
    KEY_VERSION
};

enum argkind { ARG_NO, ARG_REQ, ARG_OPT };

struct longopt {
    const char *name;
    enum argkind arg;
    int key;
};

/* Same order as GNU ls's long_options[]: ambiguity diagnostics list
   possibilities in table order. */
static const struct longopt longopts[] = {
    {"all", ARG_NO, 'a'},
    {"escape", ARG_NO, 'b'},
    {"directory", ARG_NO, 'd'},
    {"dired", ARG_NO, 'D'},
    {"full-time", ARG_NO, KEY_FULL_TIME},
    {"group-directories-first", ARG_NO, KEY_GROUP_DIRS_FIRST},
    {"human-readable", ARG_NO, 'h'},
    {"inode", ARG_NO, 'i'},
    {"kibibytes", ARG_NO, 'k'},
    {"numeric-uid-gid", ARG_NO, 'n'},
    {"no-group", ARG_NO, 'G'},
    {"hide-control-chars", ARG_NO, 'q'},
    {"reverse", ARG_NO, 'r'},
    {"size", ARG_NO, 's'},
    {"width", ARG_REQ, 'w'},
    {"almost-all", ARG_NO, 'A'},
    {"ignore-backups", ARG_NO, 'B'},
    {"classify", ARG_OPT, 'F'},
    {"file-type", ARG_NO, KEY_FILE_TYPE},
    {"si", ARG_NO, KEY_SI},
    {"dereference-command-line", ARG_NO, 'H'},
    {"dereference-command-line-symlink-to-dir", ARG_NO,
     KEY_DEREF_CL_SYMLINK_TO_DIR},
    {"hide", ARG_REQ, KEY_HIDE},
    {"ignore", ARG_REQ, 'I'},
    {"indicator-style", ARG_REQ, KEY_INDICATOR_STYLE},
    {"dereference", ARG_NO, 'L'},
    {"literal", ARG_NO, 'N'},
    {"quote-name", ARG_NO, 'Q'},
    {"quoting-style", ARG_REQ, KEY_QUOTING_STYLE},
    {"recursive", ARG_NO, 'R'},
    {"format", ARG_REQ, KEY_FORMAT},
    {"show-control-chars", ARG_NO, KEY_SHOW_CONTROL_CHARS},
    {"sort", ARG_REQ, KEY_SORT},
    {"tabsize", ARG_REQ, 'T'},
    {"time", ARG_REQ, KEY_TIME},
    {"time-style", ARG_REQ, KEY_TIME_STYLE},
    {"zero", ARG_NO, KEY_ZERO},
    {"color", ARG_OPT, KEY_COLOR},
    {"hyperlink", ARG_OPT, KEY_HYPERLINK},
    {"block-size", ARG_REQ, KEY_BLOCK_SIZE},
    {"context", ARG_NO, 'Z'},
    {"author", ARG_NO, KEY_AUTHOR},
    {"help", ARG_NO, KEY_HELP},
    {"version", ARG_NO, KEY_VERSION},
};
enum { N_LONGOPTS = sizeof longopts / sizeof longopts[0] };

/* Extension options (v0.2+): eza-compatible names beyond the frozen
   GNU 9.11 surface. EXACT-MATCH ONLY, consulted before the GNU
   matcher - abbreviation and ambiguity diagnostics come exclusively
   from the GNU table, so every GNU-surface parse and error stays
   byte-identical ("--t" and "--i" listings, "--tre" unrecognized).
   No abbreviation for extension names, by design. Keys from 512. */
enum {
    KEY_EXT_BASE = 512,
    KEY_ICONS = KEY_EXT_BASE,
    KEY_TREE,
    KEY_LEVEL,
    KEY_TREE_LIMIT,
    KEY_TREE_GLYPHS
};

static const struct longopt ext_longopts[] = {
    {"icons", ARG_OPT, KEY_ICONS},
    {"tree", ARG_NO, KEY_TREE},
    {"level", ARG_REQ, KEY_LEVEL},
    {"tree-limit", ARG_REQ, KEY_TREE_LIMIT},
    {"tree-glyphs", ARG_REQ, KEY_TREE_GLYPHS},
    /* Rows land with their sprints (14: git). */
    {NULL, ARG_NO, 0},
};
enum { N_EXT_LONGOPTS = sizeof ext_longopts / sizeof ext_longopts[0] - 1 };

static const struct longopt *
match_ext_long(const char *text, size_t namelen)
{
    for (int i = 0; i < N_EXT_LONGOPTS; i++) {
        const struct longopt *lo = &ext_longopts[i];
        if (strlen(lo->name) == namelen
            && strncmp(lo->name, text, namelen) == 0)
            return lo;
    }
    return NULL;
}

/* GNU's short-option string "abcdfghiklmnopqrstuvw:xABCDFGHI:LNQRST:UXZ1". */
static const char short_accept[] = "abcdfghiklmnopqrstuvwxABCDFGHILNQRSTUXZ1";
static const char short_witharg[] = "wIT";

/* --format words, GNU order; values live at the use site (argmatch takes
   plain ints). */
static const char *const format_words[] = {
    "verbose", "long", "commas", "horizontal", "across",
    "vertical", "single-column"
};
enum { N_FORMAT_WORDS = sizeof format_words / sizeof format_words[0] };

/* --sort words, GNU order (ls.c sort_args); "width" parses but stays
   gated until sprint 04. */
static const char *const sort_words[] = {
    "none", "size", "time", "version", "extension", "name", "width"
};
enum { N_SORT_WORDS = sizeof sort_words / sizeof sort_words[0] };

/* Parse-time staging, resolved after the loop (GNU's last-wins model). */
struct staging {
    int format_opt;     /* -1 or enum liszt_format */
    int sort_opt;       /* -1 or enum liszt_sortword */
    enum liszt_ignore_mode ignore;
    bool reverse;
    bool group_directories_first;
    bool immediate_dirs;
    bool recursive;
    int deref_opt;      /* -1 unset, else enum liszt_deref */
    int time_type;      /* enum liszt_timetype; mtime unless overridden */
    bool explicit_time; /* -c/-u/--time seen; feeds the sort rule */
    const char *time_style_opt;     /* --time-style/--full-time value */
    char **hide_patterns;           /* --hide accumulation */
    int n_hide_patterns;
    char **ignore_patterns;         /* -I and -B accumulation */
    int n_ignore_patterns;
    bool print_owner;
    bool print_group;
    bool print_author;
    bool print_scontext;
    bool numeric_ids;
    bool print_block_size;
    bool print_inode;
    bool kibibytes_specified;
    int human_output_opts;
    uintmax_t output_block_size;    /* 0 = unset, resolve from env */
    int file_human_output_opts;
    uintmax_t file_output_block_size;
    bool print_with_color;
    int indicator_style;            /* enum liszt_indicator_style */
    int quoting_style_opt;          /* -1 unset */
    int eolbyte;                    /* '\n'; --zero stages 0 */
    bool dired;                     /* -D seen */
    bool print_hyperlink;           /* resolved WHEN, positional */
    int print_icons_opt;            /* -1 unset, 0 off, 1 on */
    bool tree;                      /* --tree structural mode */
    long tree_level;                /* -1 unset; 0 = unlimited */
    long tree_limit;                /* -1 unset; 0 = uncapped */
    int tree_glyphs;                /* -1 auto, 0 ascii, 1 unicode */
    int hide_control_chars_opt;     /* -1 unset */
    long width_opt;                 /* -1 unset */
    long tabsize_opt;               /* -1 unset */
};

/* WHEN words (--color/--classify, later --hyperlink), GNU order. */
static const char *const when_words[] = {
    "always", "yes", "force", "never", "no", "none", "auto", "tty",
    "if-tty"
};
enum { N_WHEN_WORDS = sizeof when_words / sizeof when_words[0] };
enum { WHEN_ALWAYS = 0, WHEN_NEVER = 1, WHEN_IF_TTY = 2 };
static const int when_vals[] = {
    WHEN_ALWAYS, WHEN_ALWAYS, WHEN_ALWAYS,
    WHEN_NEVER, WHEN_NEVER, WHEN_NEVER,
    WHEN_IF_TTY, WHEN_IF_TTY, WHEN_IF_TTY
};

/* --indicator-style words, GNU order. */
static const char *const indstyle_words[] = {
    "none", "slash", "file-type", "classify"
};
enum { N_INDSTYLE_WORDS = sizeof indstyle_words / sizeof indstyle_words[0] };
static const int indstyle_vals[] = {
    LISZT_IND_NONE, LISZT_IND_SLASH, LISZT_IND_FILE_TYPE,
    LISZT_IND_CLASSIFY
};

/* --quoting-style words, GNU order. */
static const char *const qstyle_words[] = {
    "literal", "shell", "shell-always", "shell-escape",
    "shell-escape-always", "c", "c-maybe", "escape", "locale", "clocale"
};
enum { N_QSTYLE_WORDS = sizeof qstyle_words / sizeof qstyle_words[0] };
static const int qstyle_vals[] = {
    LISZT_QS_LITERAL, LISZT_QS_SHELL, LISZT_QS_SHELL_ALWAYS,
    LISZT_QS_SHELL_ESCAPE, LISZT_QS_SHELL_ESCAPE_ALWAYS, LISZT_QS_C,
    LISZT_QS_C_MAYBE, LISZT_QS_ESCAPE, LISZT_QS_LOCALE, LISZT_QS_CLOCALE
};

/* GNU decode_line_length: base-0 integer, full-string, capped; -1 on
   any parse failure or overflow-to-invalid. */
static long
decode_line_length(const char *spec)
{
    char *end;
    unsigned long long v;

    if (!(*spec >= '0' && *spec <= '9') && *spec != '0')
        if (!(*spec >= '0' && *spec <= '9'))
            return -1;
    errno = 0;
    v = strtoull(spec, &end, 0);
    if (end == spec || *end != '\0')
        return -1;
    if (errno == ERANGE || v > (unsigned long long)LONG_MAX / 2)
        return 0;   /* huge widths mean unlimited, matching GNU's cap */
    return (long)v;
}

static void
print_valid_words(const char *const *words, const int *vals, int n)
{
    fprintf(stderr, "Valid arguments are:\n");
    for (int i = 0; i < n; i++) {
        if (i > 0 && vals[i] == vals[i - 1])
            fprintf(stderr, ", %s%s%s", liszt_qL(), words[i], liszt_qR());
        else
            fprintf(stderr, "%s  - %s%s%s", i ? "\n" : "",
                    liszt_qL(), words[i], liszt_qR());
    }
    fputc('\n', stderr);
}

/* gnulib-argmatch semantics: exact match wins; else unambiguous
   abbreviation (several matches agreeing on the value are fine). */
static int
argmatch_die(const char *context, const char *arg,
             const char *const *words, const int *vals, int n)
{
    int match = -1;
    bool ambiguous = false;

    for (int i = 0; i < n; i++) {
        if (strcmp(words[i], arg) == 0)
            return vals[i];
        if (strncmp(words[i], arg, strlen(arg)) == 0) {
            if (match < 0)
                match = i;
            else if (vals[i] != vals[match])
                ambiguous = true;
        }
    }
    if (match >= 0 && !ambiguous)
        return vals[match];

    liszt_error(0, "%s argument %s%s%s for %s%s%s",
                ambiguous ? "ambiguous" : "invalid",
                liszt_qL(), arg, liszt_qR(),
                liszt_qL(), context, liszt_qR());
    print_valid_words(words, vals, n);
    /* GNU quirk (pinned vs 9.11): argmatch-class errors exit 1, not 2 -
       gnulib exit_failure keeps its EXIT_FAILURE default in ls. */
    liszt_try_help_print();
    exit(LISZT_STATUS_MINOR);
}

static void
add_pattern(char ***list, int *n, const char *pat)
{
    *list = liszt_xrealloc(*list, (size_t)(*n + 1) * sizeof **list);
    /* Argv and literal strings both outlive the run; no copy. */
    (*list)[(*n)++] = (char *)(uintptr_t)pat;
}

/* gnulib hard_locale for LC_TIME: anything but C/POSIX. */
static bool
hard_time_locale(void)
{
    const char *name = setlocale(LC_TIME, NULL);
    return name != NULL && strcmp(name, "C") != 0
        && strcmp(name, "POSIX") != 0;
}

/* x_timestyle_match (GNU system.h): word matching like argmatch, but
   failure lists the [posix-] variants plus the +FORMAT hint and exits
   2 (LS_FAILURE) - unlike ls's other argmatch errors, which exit 1. */
static int
timestyle_match(const char *arg)
{
    static const char *const words[] = {
        "full-iso", "long-iso", "iso", "locale"
    };
    int match = -1;
    bool ambiguous = false;

    for (int i = 0; i < 4; i++) {
        if (strcmp(words[i], arg) == 0)
            return i;
        if (strncmp(words[i], arg, strlen(arg)) == 0) {
            if (match < 0)
                match = i;
            else
                ambiguous = true;   /* all four values are distinct */
        }
    }
    if (match >= 0 && !ambiguous)
        return match;

    liszt_error(0, "%s argument %s%s%s for %s%s%s",
                ambiguous ? "ambiguous" : "invalid",
                liszt_qL(), arg, liszt_qR(),
                liszt_qL(), "time style", liszt_qR());
    fprintf(stderr, "Valid arguments are:\n");
    for (int i = 0; i < 4; i++)
        fprintf(stderr, "  - [posix-]%s\n", words[i]);
    fprintf(stderr,
            "  - +FORMAT (e.g., +%%H:%%M) for a 'date'-style format\n");
    liszt_try_help_print();
    exit(LISZT_STATUS_SERIOUS);
}

static void
unsupported(const char *display)
{
    liszt_die(LISZT_STATUS_SERIOUS, 0,
              "option '%s' is not supported yet", display);
}

static void
print_help(void)
{
    printf("Usage: %s [OPTION]... [FILE]...\n", liszt_argv0);
    printf("List information about the FILEs (the current directory by default).\n");
    printf("\n");
    printf("liszt is a from-scratch reimplementation of GNU ls; 'lz' is the same\n");
    printf("binary under a shorter name. The full GNU ls 9.11 option surface is\n");
    printf("supported with byte-identical output under a pinned environment;\n");
    printf("see ls(1) for option semantics.\n");
    printf("\n");
    printf("Extensions (exact spelling, off by default; see liszt(1)):\n");
    printf("      --icons[=WHEN]      Nerd-Font icons (always, auto, never);\n");
    printf("                          LS_ICONS overrides, LISZT_ICON_SPACING\n");
    printf("      --tree              list contents as a tree (implies recursion;\n");
    printf("                          one-per-line unless -l)\n");
    printf("      --level=N           descend at most N levels (0 = unlimited)\n");
    printf("      --tree-limit=N      at most N entries per dir, then '... K more'\n");
    printf("      --tree-glyphs=WORD  unicode, ascii, or auto (locale codeset)\n");
    exit(LISZT_STATUS_OK);
}

static void
print_version(void)
{
    printf("liszt %s\n", LISZT_VERSION);
    exit(LISZT_STATUS_OK);
}

static void
handle(int key, const char *value, const char *display, struct staging *st)
{
    switch (key) {
    case 'a':
        st->ignore = LISZT_IGNORE_MINIMAL;
        break;
    case 'A':
        st->ignore = LISZT_IGNORE_DOT_AND_DOTDOT;
        break;
    case 'U':
        st->sort_opt = LISZT_SORT_NONE;
        break;
    case 'S':
        st->sort_opt = LISZT_SORT_SIZE;
        break;
    case 't':
        st->sort_opt = LISZT_SORT_TIME;
        break;
    case 'c':
        st->time_type = LISZT_TIME_CTIME;
        st->explicit_time = true;
        break;
    case 'u':
        st->time_type = LISZT_TIME_ATIME;
        st->explicit_time = true;
        break;
    case KEY_TIME: {
        /* GNU time_args order: three atime synonyms, two ctime, two
           mtime, two btime. */
        static const char *const time_words[] = {
            "atime", "access", "use",
            "ctime", "status",
            "mtime", "modification",
            "birth", "creation"
        };
        static const int vals[] = {
            LISZT_TIME_ATIME, LISZT_TIME_ATIME, LISZT_TIME_ATIME,
            LISZT_TIME_CTIME, LISZT_TIME_CTIME,
            LISZT_TIME_MTIME, LISZT_TIME_MTIME,
            LISZT_TIME_BTIME, LISZT_TIME_BTIME
        };
        st->time_type = argmatch_die("--time", value, time_words, vals,
                                     (int)(sizeof vals / sizeof vals[0]));
        st->explicit_time = true;
        break;
    }
    case KEY_FULL_TIME:
        st->format_opt = LISZT_FMT_LONG;
        st->time_style_opt = "full-iso";
        break;
    case KEY_TIME_STYLE:
        st->time_style_opt = value;
        break;
    case 'B':
        /* GNU adds both to the -I list; FNM_PERIOD makes the second
           necessary for dotted backups. */
        add_pattern(&st->ignore_patterns, &st->n_ignore_patterns, "*~");
        add_pattern(&st->ignore_patterns, &st->n_ignore_patterns, ".*~");
        break;
    case 'I':
        add_pattern(&st->ignore_patterns, &st->n_ignore_patterns, value);
        break;
    case KEY_HIDE:
        add_pattern(&st->hide_patterns, &st->n_hide_patterns, value);
        break;
    case 'Z':
        st->print_scontext = true;
        break;
    case 'D':
        /* GNU: -D stages long format and drops --hyperlink; both are
           positional, later options re-override. Icons follow the
           same positional rule. */
        st->format_opt = LISZT_FMT_LONG;
        st->print_hyperlink = false;
        st->print_icons_opt = 0;
        st->dired = true;
        break;
    case KEY_TREE:
        st->tree = true;
        break;
    case KEY_LEVEL:
    case KEY_TREE_LIMIT: {
        char *end;
        errno = 0;
        long v = strtol(value, &end, 10);
        if (end == value || *end != '\0' || errno == ERANGE || v < 0) {
            fprintf(stderr, "%s: invalid %s argument '%s'\n",
                    liszt_argv0, display, value);
            liszt_try_help_and_die();
        }
        if (key == KEY_LEVEL)
            st->tree_level = v;
        else
            st->tree_limit = v;
        break;
    }
    case KEY_TREE_GLYPHS: {
        static const char *const glyph_words[] = {
            "unicode", "ascii", "auto"
        };
        static const int glyph_vals[] = { 1, 0, -1 };
        st->tree_glyphs = argmatch_die("--tree-glyphs", value,
                                       glyph_words, glyph_vals, 3);
        break;
    }
    case KEY_ICONS: {
        /* eza semantics: bare --icons means auto (TTY-gated), a
           deliberate documented divergence from the GNU bare-WHEN
           convention. */
        int v = value ? argmatch_die("--icons", value, when_words,
                                     when_vals, N_WHEN_WORDS)
                      : WHEN_IF_TTY;
        st->print_icons_opt = v == WHEN_ALWAYS
            || (v == WHEN_IF_TTY && isatty(STDOUT_FILENO));
        break;
    }
    case KEY_HYPERLINK: {
        int v = WHEN_ALWAYS;
        if (value)
            v = argmatch_die("--hyperlink", value, when_words, when_vals,
                             N_WHEN_WORDS);
        st->print_hyperlink = v == WHEN_ALWAYS
            || (v == WHEN_IF_TTY && isatty(STDOUT_FILENO));
        break;
    }
    case KEY_ZERO:
        /* GNU's staging effects are positional last-wins: a later -l,
           -q, -Q, -C or --color re-overrides the individual pieces.
           Icons stage off like color. */
        st->print_icons_opt = 0;
        st->eolbyte = 0;
        st->hide_control_chars_opt = 0;
        if (st->format_opt != LISZT_FMT_LONG)
            st->format_opt = LISZT_FMT_ONE;
        st->print_with_color = false;
        st->quoting_style_opt = LISZT_QS_LITERAL;
        break;
    case 'v':
        st->sort_opt = LISZT_SORT_VERSION;
        break;
    case 'X':
        st->sort_opt = LISZT_SORT_EXTENSION;
        break;
    case 'r':
        st->reverse = true;
        break;
    case 'l':
        st->format_opt = LISZT_FMT_LONG;
        break;
    case 'g':
        st->format_opt = LISZT_FMT_LONG;
        st->print_owner = false;
        break;
    case 'o':
        st->format_opt = LISZT_FMT_LONG;
        st->print_group = false;
        break;
    case 'n':
        st->numeric_ids = true;
        st->format_opt = LISZT_FMT_LONG;
        break;
    case 'G':
        st->print_group = false;
        break;
    case KEY_AUTHOR:
        st->print_author = true;
        break;
    case 's':
        st->print_block_size = true;
        break;
    case 'i':
        st->print_inode = true;
        break;
    case 'h':
        st->file_human_output_opts = st->human_output_opts =
            LISZT_HUMAN_AUTOSCALE | LISZT_HUMAN_SI | LISZT_HUMAN_BASE_1024;
        st->file_output_block_size = st->output_block_size = 1;
        break;
    case KEY_SI:
        st->file_human_output_opts = st->human_output_opts =
            LISZT_HUMAN_AUTOSCALE | LISZT_HUMAN_SI;
        st->file_output_block_size = st->output_block_size = 1;
        break;
    case 'k':
        st->kibibytes_specified = true;
        break;
    case 'C':
        st->format_opt = LISZT_FMT_MANY;
        break;
    case 'x':
        st->format_opt = LISZT_FMT_HORIZONTAL;
        break;
    case 'm':
        st->format_opt = LISZT_FMT_COMMAS;
        break;
    case 'b':
        st->quoting_style_opt = LISZT_QS_ESCAPE;
        break;
    case 'N':
        st->quoting_style_opt = LISZT_QS_LITERAL;
        break;
    case 'Q':
        st->quoting_style_opt = LISZT_QS_C;
        break;
    case KEY_QUOTING_STYLE:
        st->quoting_style_opt =
            argmatch_die("--quoting-style", value, qstyle_words,
                         qstyle_vals, N_QSTYLE_WORDS);
        break;
    case 'q':
        st->hide_control_chars_opt = 1;
        break;
    case KEY_COLOR: {
        int when = value
            ? argmatch_die("--color", value, when_words, when_vals,
                           N_WHEN_WORDS)
            : WHEN_ALWAYS;
        st->print_with_color = when == WHEN_ALWAYS
            || (when == WHEN_IF_TTY && isatty(STDOUT_FILENO));
        break;
    }
    case 'F': {
        int when = value
            ? argmatch_die("--classify", value, when_words, when_vals,
                           N_WHEN_WORDS)
            : WHEN_ALWAYS;
        if (when == WHEN_ALWAYS
            || (when == WHEN_IF_TTY && isatty(STDOUT_FILENO)))
            st->indicator_style = LISZT_IND_CLASSIFY;
        break;
    }
    case 'p':
        st->indicator_style = LISZT_IND_SLASH;
        break;
    case 'R':
        st->recursive = true;
        break;
    case 'd':
        st->immediate_dirs = true;
        break;
    case 'H':
        st->deref_opt = LISZT_DEREF_COMMAND_LINE_ARGUMENTS;
        break;
    case 'L':
        st->deref_opt = LISZT_DEREF_ALWAYS;
        break;
    case KEY_DEREF_CL_SYMLINK_TO_DIR:
        st->deref_opt = LISZT_DEREF_COMMAND_LINE_SYMLINK_TO_DIR;
        break;
    case KEY_FILE_TYPE:
        st->indicator_style = LISZT_IND_FILE_TYPE;
        break;
    case KEY_INDICATOR_STYLE:
        st->indicator_style =
            argmatch_die("--indicator-style", value, indstyle_words,
                         indstyle_vals, N_INDSTYLE_WORDS);
        break;
    case KEY_SHOW_CONTROL_CHARS:
        st->hide_control_chars_opt = 0;
        break;
    case 'w': {
        long ll = decode_line_length(value);
        if (ll < 0) {
            liszt_error(0, "invalid line width: %s%s%s", liszt_qL(),
                        liszt_quote_diag(value), liszt_qR());
            exit(LISZT_STATUS_SERIOUS);
        }
        st->width_opt = ll;
        break;
    }
    case 'T': {
        char *end;
        unsigned long long v;
        errno = 0;
        v = strtoull(value, &end, 0);
        if (end == value || *end != '\0' || errno == ERANGE
            || v > (unsigned long long)LONG_MAX
            || !(*value >= '0' && *value <= '9')) {
            liszt_error(0, "invalid tab size: %s%s%s", liszt_qL(),
                        liszt_quote_diag(value), liszt_qR());
            exit(LISZT_STATUS_SERIOUS);
        }
        st->tabsize_opt = (long)v;
        break;
    }
    case KEY_BLOCK_SIZE: {
        enum liszt_strtol_error e =
            liszt_human_options(value, &st->human_output_opts,
                                &st->output_block_size);
        if (e != LISZT_LONGINT_OK) {
            /* GNU xstrtol_fatal shape: no Try line, exit 2. */
            liszt_error(0, "%s --block-size argument %s%s%s%s",
                        e == LISZT_LONGINT_OVERFLOW ? "" : "invalid",
                        liszt_qL(), liszt_quote_diag(value), liszt_qR(),
                        e == LISZT_LONGINT_OVERFLOW ? " too large" : "");
            exit(LISZT_STATUS_SERIOUS);
        }
        st->file_human_output_opts = st->human_output_opts;
        st->file_output_block_size = st->output_block_size;
        break;
    }
    case 'f':
        /* 9.11: -f is exactly -a -U, last-wins (it no longer disables
           -l or color as ancient ls did). */
        st->ignore = LISZT_IGNORE_MINIMAL;
        st->sort_opt = LISZT_SORT_NONE;
        break;
    case KEY_GROUP_DIRS_FIRST:
        st->group_directories_first = true;
        break;
    case KEY_SORT: {
        static const int vals[] = {
            LISZT_SORT_NONE, LISZT_SORT_SIZE, LISZT_SORT_TIME,
            LISZT_SORT_VERSION, LISZT_SORT_EXTENSION, LISZT_SORT_NAME,
            LISZT_SORT_WIDTH
        };
        st->sort_opt =
            argmatch_die("--sort", value, sort_words, vals, N_SORT_WORDS);
        break;
    }
    case '1':
        if (st->format_opt != LISZT_FMT_LONG)
            st->format_opt = LISZT_FMT_ONE;
        break;
    case KEY_FORMAT: {
        static const int vals[] = {
            LISZT_FMT_LONG, LISZT_FMT_LONG, LISZT_FMT_COMMAS,
            LISZT_FMT_HORIZONTAL, LISZT_FMT_HORIZONTAL, LISZT_FMT_MANY,
            LISZT_FMT_ONE
        };
        st->format_opt =
            argmatch_die("--format", value, format_words, vals,
                         N_FORMAT_WORDS);
        break;
    }
    case KEY_HELP:
        print_help();
        break;
    case KEY_VERSION:
        print_version();
        break;
    default:
        unsupported(display);
    }
}

static const struct longopt *
match_long(const char *text, size_t namelen, const char *whole_arg)
{
    const struct longopt *exact = NULL;
    const struct longopt *match = NULL;
    bool ambiguous = false;

    for (int i = 0; i < N_LONGOPTS; i++) {
        const struct longopt *lo = &longopts[i];
        if (strlen(lo->name) == namelen
            && strncmp(lo->name, text, namelen) == 0) {
            exact = lo;
            break;
        }
        if (strlen(lo->name) > namelen
            && strncmp(lo->name, text, namelen) == 0) {
            if (!match)
                match = lo;
            else
                ambiguous = true;
        }
    }
    if (exact)
        return exact;
    if (match && !ambiguous)
        return match;

    if (ambiguous) {
        fprintf(stderr, "%s: option '--%.*s' is ambiguous; possibilities:",
                liszt_argv0, (int)namelen, text);
        for (int i = 0; i < N_LONGOPTS; i++)
            if (strlen(longopts[i].name) >= namelen
                && strncmp(longopts[i].name, text, namelen) == 0)
                fprintf(stderr, " '--%s'", longopts[i].name);
        fputc('\n', stderr);
        liszt_try_help_and_die();
    }
    fprintf(stderr, "%s: unrecognized option '%s'\n", liszt_argv0, whole_arg);
    liszt_try_help_and_die();
    return NULL;
}

static void
parse_long(const char *arg, int argc, char **argv, int *i,
           struct staging *st)
{
    const char *text = arg + 2;
    const char *eq = strchr(text, '=');
    size_t namelen = eq ? (size_t)(eq - text) : strlen(text);
    const struct longopt *lo = match_ext_long(text, namelen);
    if (lo == NULL)
        lo = match_long(text, namelen, arg);
    char display[64];

    snprintf(display, sizeof display, "--%s", lo->name);

    const char *value = NULL;
    switch (lo->arg) {
    case ARG_NO:
        if (eq) {
            fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n",
                    liszt_argv0, lo->name);
            liszt_try_help_and_die();
        }
        break;
    case ARG_REQ:
        if (eq) {
            value = eq + 1;
        } else if (*i + 1 < argc) {
            value = argv[++*i];
        } else {
            fprintf(stderr, "%s: option '--%s' requires an argument\n",
                    liszt_argv0, lo->name);
            liszt_try_help_and_die();
        }
        break;
    case ARG_OPT:
        value = eq ? eq + 1 : NULL;
        break;
    }
    handle(lo->key, value, display, st);
}

static void
parse_shorts(const char *arg, int argc, char **argv, int *i,
             struct staging *st)
{
    for (const char *p = arg + 1; *p; p++) {
        char c = *p;
        char display[3] = { '-', c, '\0' };

        if (!strchr(short_accept, c)) {
            fprintf(stderr, "%s: invalid option -- '%c'\n", liszt_argv0, c);
            liszt_try_help_and_die();
        }
        if (strchr(short_witharg, c)) {
            const char *value;
            if (p[1] != '\0') {
                value = p + 1;
            } else if (*i + 1 < argc) {
                value = argv[++*i];
            } else {
                fprintf(stderr,
                        "%s: option requires an argument -- '%c'\n",
                        liszt_argv0, c);
                liszt_try_help_and_die();
            }
            handle(c, value, display, st);
            return;     /* value consumed the rest of the cluster */
        }
        handle(c, NULL, display, st);
    }
}

void
liszt_options_parse(int argc, char **argv, struct liszt_options *o)
{
    struct staging st = {
        .format_opt = -1,
        .sort_opt = -1,
        .ignore = LISZT_IGNORE_DEFAULT,
        .reverse = false,
        .group_directories_first = false,
        .immediate_dirs = false,
        .recursive = false,
        .deref_opt = -1,
        .time_type = LISZT_TIME_MTIME,
        .explicit_time = false,
        .time_style_opt = NULL,
        .hide_patterns = NULL,
        .n_hide_patterns = 0,
        .ignore_patterns = NULL,
        .n_ignore_patterns = 0,
        .print_owner = true,
        .print_group = true,
        .print_author = false,
        .print_scontext = false,
        .numeric_ids = false,
        .print_block_size = false,
        .print_inode = false,
        .kibibytes_specified = false,
        .human_output_opts = 0,
        .output_block_size = 0,
        .file_human_output_opts = 0,
        .file_output_block_size = 0,
        .print_with_color = false,
        .indicator_style = LISZT_IND_NONE,
        .quoting_style_opt = -1,
        .eolbyte = '\n',
        .dired = false,
        .print_hyperlink = false,
        .print_icons_opt = -1,
        .tree = false,
        .tree_level = -1,
        .tree_limit = -1,
        .tree_glyphs = -1,
        .hide_control_chars_opt = -1,
        .width_opt = -1,
        .tabsize_opt = -1
    };
    bool posixly = getenv("POSIXLY_CORRECT") != NULL;
    bool no_more_options = false;

    o->operands = liszt_xmalloc((size_t)(argc > 1 ? argc : 1)
                                * sizeof *o->operands);
    o->n_operands = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (no_more_options || arg[0] != '-' || arg[1] == '\0') {
            o->operands[o->n_operands++] = argv[i];
            /* GNU getopt permutes; POSIXLY_CORRECT stops at the first
               non-option instead. */
            if (posixly)
                no_more_options = true;
            continue;
        }
        if (arg[1] == '-' && arg[2] == '\0') {
            no_more_options = true;
            continue;
        }
        if (arg[1] == '-')
            parse_long(arg, argc, argv, &i, &st);
        else
            parse_shorts(arg, argc, argv, &i, &st);
    }

    /* Resolution, mirroring decode_switches order. The sort-resolution
       table is THE one place effective sort is computed; sprint 07's
       -c/-u rule is the explicit_time input below, nothing else. */
    o->ignore = st.ignore;
    o->reverse = st.reverse;
    o->group_directories_first = st.group_directories_first;
    o->immediate_dirs = st.immediate_dirs;
    o->recursive = st.recursive;
    o->print_owner = st.print_owner;
    o->print_group = st.print_group;
    o->print_author = st.print_author;
    o->numeric_ids = st.numeric_ids;
    o->print_block_size = st.print_block_size;
    o->print_inode = st.print_inode;

    /* GNU's BLOCK_SIZE env chain (decode_switches 2249-2264): flags win;
       else LS_BLOCK_SIZE (falling through to BLOCK_SIZE/BLOCKSIZE inside
       the parser); env values also bind the -l file pair; -k then forces
       1024 block counts, leaving file sizes alone. Env errors are
       silently ignored. */
    if (st.output_block_size == 0) {
        const char *ls_bs = getenv("LS_BLOCK_SIZE");
        liszt_human_options(ls_bs, &st.human_output_opts,
                            &st.output_block_size);
        if (ls_bs || getenv("BLOCK_SIZE")) {
            st.file_human_output_opts = st.human_output_opts;
            st.file_output_block_size = st.output_block_size;
        }
        if (st.kibibytes_specified) {
            st.human_output_opts = 0;
            st.output_block_size = 1024;
        }
    }
    if (st.file_output_block_size == 0) {
        st.file_human_output_opts = 0;
        st.file_output_block_size = 1;
    }
    o->human_output_opts = st.human_output_opts;
    o->output_block_size = st.output_block_size;
    o->file_human_output_opts = st.file_human_output_opts;
    o->file_output_block_size = st.file_output_block_size;
    o->format = st.format_opt >= 0
        ? (enum liszt_format)st.format_opt
        : (isatty(STDOUT_FILENO) ? LISZT_FMT_MANY : LISZT_FMT_ONE);
    if (st.sort_opt >= 0)
        o->sort = (enum liszt_sortword)st.sort_opt;
    else if (st.explicit_time && o->format != LISZT_FMT_LONG)
        o->sort = LISZT_SORT_TIME;
    else
        o->sort = LISZT_SORT_NAME;
    o->time_type = (enum liszt_timetype)st.time_type;
    o->eolbyte = (char)st.eolbyte;
    /* --dired implies long format; silently self-disables if a later
       format word overrode that (GNU: dired &= format == long). The
       --zero clash is fatal only when dired survives. */
    o->print_scontext = st.print_scontext;
    o->print_hyperlink = st.print_hyperlink;
    if (st.print_icons_opt < 0) {
        const char *ie = getenv("LISZT_ICONS");
        int v = -1;
        if (ie != NULL && *ie != '\0') {
            for (int wi = 0; wi < N_WHEN_WORDS; wi++)
                if (strcmp(when_words[wi], ie) == 0) {
                    v = when_vals[wi];
                    break;
                }
        }
        st.print_icons_opt = v == WHEN_ALWAYS
            || (v == WHEN_IF_TTY && isatty(STDOUT_FILENO));
    }
    o->print_icons = st.print_icons_opt == 1;
    /* --tree resolution (locked, sprint 13): implies recursion-like
       traversal via its own walker; forces one-per-line unless long;
       -d wins by leaving dir operands unextracted; the child flags
       demand --tree. Glyph auto = UTF-8 codeset. */
    if (!st.tree
        && (st.tree_level >= 0 || st.tree_limit >= 0
            || st.tree_glyphs != -1)) {
        const char *culprit = st.tree_level >= 0 ? "--level"
            : st.tree_limit >= 0 ? "--tree-limit" : "--tree-glyphs";
        fprintf(stderr, "%s: %s requires --tree\n", liszt_argv0,
                culprit);
        liszt_try_help_and_die();
    }
    o->tree = st.tree;
    o->tree_level = st.tree_level > 0 ? (size_t)st.tree_level : 0;
    o->tree_limit = st.tree_limit > 0 ? (size_t)st.tree_limit : 0;
    if (st.tree_glyphs == -1) {
        const char *cs = nl_langinfo(CODESET);
        o->tree_unicode = cs != NULL && strcmp(cs, "UTF-8") == 0;
    } else {
        o->tree_unicode = st.tree_glyphs == 1;
    }
    if (o->tree && o->format != LISZT_FMT_LONG)
        o->format = LISZT_FMT_ONE;
    o->dired = st.dired && o->format == LISZT_FMT_LONG
        && !o->print_hyperlink && !o->print_icons && !o->tree;
    if (o->eolbyte == 0 && o->dired)
        liszt_die(LISZT_STATUS_SERIOUS, 0,
                  "--dired and --zero are incompatible");
    o->hide_patterns = st.hide_patterns;
    o->n_hide_patterns = st.n_hide_patterns;
    o->ignore_patterns = st.ignore_patterns;
    o->n_ignore_patterns = st.n_ignore_patterns;
    if (st.deref_opt >= 0)
        o->deref = (enum liszt_deref)st.deref_opt;
    else
        o->deref = (o->immediate_dirs
                    || o->indicator_style == LISZT_IND_CLASSIFY
                    || o->format == LISZT_FMT_LONG)
            ? LISZT_DEREF_NEVER
            : LISZT_DEREF_COMMAND_LINE_SYMLINK_TO_DIR;

    /* --time-style resolves only under long format (GNU decode_switches
       gates the whole block): a bogus style word without -l is never
       even validated. TIME_STYLE env fills an absent option; posix-
       prefixes strip repeatedly in a hard LC_TIME locale and otherwise
       pin the locale default outright (GNU returns early). */
    if (o->format == LISZT_FMT_LONG) {
        const char *style = st.time_style_opt;
        if (style == NULL)
            style = getenv("TIME_STYLE");
        bool use_default = style == NULL;
        while (!use_default && strncmp(style, "posix-", 6) == 0) {
            if (!hard_time_locale()) {
                use_default = true;
                break;
            }
            style += 6;
        }
        if (!use_default) {
            if (style[0] == '+') {
                const char *p0 = style + 1;
                const char *nl = strchr(p0, '\n');
                if (nl != NULL) {
                    if (strchr(nl + 1, '\n') != NULL)
                        liszt_die(LISZT_STATUS_SERIOUS, 0,
                                  "invalid time style format %s%s%s",
                                  liszt_qL(), liszt_quote_diag(p0),
                                  liszt_qR());
                    size_t n0 = (size_t)(nl - p0);
                    char *older = liszt_xmalloc(n0 + 1);
                    memcpy(older, p0, n0);
                    older[n0] = '\0';
                    liszt_timefmt_set_formats(older, nl + 1);
                } else {
                    liszt_timefmt_set_formats(p0, p0);
                }
            } else {
                switch (timestyle_match(style)) {
                case 0:
                    liszt_timefmt_set_formats("%Y-%m-%d %H:%M:%S.%N %z",
                                              "%Y-%m-%d %H:%M:%S.%N %z");
                    break;
                case 1:
                    liszt_timefmt_set_formats("%Y-%m-%d %H:%M",
                                              "%Y-%m-%d %H:%M");
                    break;
                case 2:
                    liszt_timefmt_set_formats("%Y-%m-%d ",
                                              "%m-%d %H:%M");
                    break;
                default:
                    /* locale: GNU dcgettext-translates the defaults in
                       hard LC_TIME locales; the pinned oracle runs
                       without message catalogs, so the untranslated
                       formats are the parity target either way. */
                    break;
                }
            }
        }
    }

    /* Line length (GNU 2272-2303): -w wins; else tty winsize; else
       COLUMNS (invalid warns and falls through); else 80. -w0 and huge
       values mean unlimited. */
    o->print_with_color = st.print_with_color;
    o->indicator_style = (enum liszt_indicator_style)st.indicator_style;

    long linelen = st.width_opt;
    bool multi = o->format == LISZT_FMT_MANY
        || o->format == LISZT_FMT_HORIZONTAL
        || o->format == LISZT_FMT_COMMAS;
    /* Color forces line-length acquisition (GNU 2276). */
    if (multi || o->print_with_color) {
        if (linelen < 0 && isatty(STDOUT_FILENO)) {
            struct winsize ws;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) >= 0
                && 0 < ws.ws_col)
                linelen = ws.ws_col;
        }
        if (linelen < 0) {
            const char *p = getenv("COLUMNS");
            if (p && *p) {
                linelen = decode_line_length(p);
                if (linelen < 0)
                    liszt_error(0, "ignoring invalid width in environment"
                                " variable COLUMNS: %s%s%s", liszt_qL(),
                                liszt_quote_diag(p), liszt_qR());
            }
        }
    }
    o->line_length = linelen < 0 ? 80 : (size_t)linelen;
    o->max_idx = o->line_length / 3 + (o->line_length % 3 != 0);

    o->tabsize = 8;
    if (multi) {
        if (st.tabsize_opt >= 0) {
            o->tabsize = (size_t)st.tabsize_opt;
        } else {
            const char *p = getenv("TABSIZE");
            if (p) {
                char *end;
                errno = 0;
                unsigned long long v = strtoull(p, &end, 0);
                if (end != p && *end == '\0' && errno != ERANGE
                    && (*p >= '0' && *p <= '9'))
                    o->tabsize = (size_t)v;
                else
                    liszt_error(0, "ignoring invalid tab size in"
                                " environment variable TABSIZE: %s%s%s",
                                liszt_qL(), liszt_quote_diag(p),
                                liszt_qR());
            }
        }
    }

    /* -q resolution: flag wins, else tty default. */
    o->qmark_funny_chars = st.hide_control_chars_opt >= 0
        ? st.hide_control_chars_opt != 0
        : isatty(STDOUT_FILENO);

    /* Quoting style: flag > QUOTING_STYLE env (invalid warns) > tty
       shell-escape / piped literal. */
    int qs = st.quoting_style_opt;
    if (qs < 0) {
        const char *p = getenv("QUOTING_STYLE");
        if (p) {
            int found = -1;
            for (int i = 0; i < N_QSTYLE_WORDS; i++)
                if (strcmp(p, qstyle_words[i]) == 0) {
                    found = qstyle_vals[i];
                    break;
                }
            if (found >= 0)
                qs = found;
            else
                liszt_error(0, "ignoring invalid value of environment"
                            " variable QUOTING_STYLE: %s%s%s", liszt_qL(),
                            liszt_quote_diag(p), liszt_qR());
        }
    }
    if (qs < 0)
        qs = isatty(STDOUT_FILENO) ? LISZT_QS_SHELL_ESCAPE
                                   : LISZT_QS_LITERAL;
    o->quoting_style = (enum liszt_qstyle)qs;

    o->align_variable_outer_quotes =
        (o->format == LISZT_FMT_LONG
         || ((o->format == LISZT_FMT_MANY
              || o->format == LISZT_FMT_HORIZONTAL)
             && o->line_length))
        && (o->quoting_style == LISZT_QS_SHELL
            || o->quoting_style == LISZT_QS_SHELL_ESCAPE
            || o->quoting_style == LISZT_QS_C_MAYBE);

    memset(&o->filename_qopts, 0, sizeof o->filename_qopts);
    o->filename_qopts.style = o->quoting_style;
    if (o->quoting_style == LISZT_QS_ESCAPE)
        liszt_set_char_quoting(&o->filename_qopts, ' ', 1);
    if (o->indicator_style >= LISZT_IND_FILE_TYPE) {
        /* GNU: &"*=>@|"[indicator_style - file_type] - file-type
           force-quotes all five, classify skips '*'. */
        const char *incompat = "*=>@|";
        for (const char *pc = incompat
                 + (o->indicator_style - LISZT_IND_FILE_TYPE); *pc; pc++)
            liszt_set_char_quoting(&o->filename_qopts, *pc, 1);
    }
    o->dirname_qopts = o->filename_qopts;
    liszt_set_char_quoting(&o->dirname_qopts, ':', 1);

    /* Capability gate: sorts fully covered except width (04E wires it
       against the quoted-width cache). */
    switch (o->sort) {
    case LISZT_SORT_NONE:
    case LISZT_SORT_NAME:
    case LISZT_SORT_EXTENSION:
    case LISZT_SORT_VERSION:
    case LISZT_SORT_SIZE:
    case LISZT_SORT_TIME:
    case LISZT_SORT_WIDTH:
        break;
    default:
        liszt_die(LISZT_STATUS_SERIOUS, 0,
                  "this sort is not supported yet");
    }
}

const char *
liszt_quoting_style_word(enum liszt_qstyle style)
{
    for (int i = 0; i < N_QSTYLE_WORDS; i++)
        if (qstyle_vals[i] == (int)style)
            return qstyle_words[i];
    return "literal";
}

#include "options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
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

/* Parse-time staging, resolved after the loop (GNU's last-wins model). */
struct staging {
    int format_opt;     /* -1 or enum liszt_format */
    int sort_opt;       /* -1 or enum liszt_sortword */
    enum liszt_ignore_mode ignore;
    bool immediate_dirs;
};

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
    printf("binary under a shorter name.\n");
    printf("\n");
    printf("Implemented so far: -1 -U -a -A --all --almost-all\n");
    printf("--format=single-column --help --version. Other GNU ls options are\n");
    printf("recognized but exit with a not-supported diagnostic until their\n");
    printf("sprint lands.\n");
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
    const struct longopt *lo = match_long(text, namelen, arg);
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
        .immediate_dirs = false
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

    /* Resolution, mirroring decode_switches order. */
    o->ignore = st.ignore;
    o->immediate_dirs = st.immediate_dirs;
    o->format = st.format_opt >= 0
        ? (enum liszt_format)st.format_opt
        : (isatty(STDOUT_FILENO) ? LISZT_FMT_MANY : LISZT_FMT_ONE);
    o->sort = st.sort_opt >= 0
        ? (enum liszt_sortword)st.sort_opt
        : LISZT_SORT_NAME;
    o->deref = (o->immediate_dirs || o->format == LISZT_FMT_LONG)
        ? LISZT_DEREF_NEVER
        : LISZT_DEREF_COMMAND_LINE_SYMLINK_TO_DIR;

    /* Sprint 01 capability gate: clear exits, never wrong output. */
    if (o->format != LISZT_FMT_ONE)
        liszt_die(LISZT_STATUS_SERIOUS, 0,
                  "only single-column output is supported yet"
                  " (use -1 or pipe stdout)");
    if (o->sort != LISZT_SORT_NONE)
        liszt_die(LISZT_STATUS_SERIOUS, 0,
                  "sorted listings are not supported yet (use -U)");
}

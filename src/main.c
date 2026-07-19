#include <errno.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "dirread.h"
#include "emit.h"
#include "entry.h"
#include "options.h"
#include "plan.h"
#include "sortkey.h"
#include "util.h"

static void
file_failure(bool serious, const char *fmt_with_name, const char *name,
             int errnum)
{
    /* GNU quotes the name shell-escape-always style; the simple-name
       subset ('name') is what sprint 01 fixtures exercise. Sprint 04's
       quoting module takes this over. */
    fprintf(stderr, "%s: ", liszt_prog);
    fprintf(stderr, fmt_with_name, name);
    fprintf(stderr, ": %s\n", strerror(errnum));
    liszt_set_exit_status(serious);
}

/* One classified command-line operand. */
struct operand {
    const char *name;
    bool is_dir;
};

static const char *
operand_name(const void *p)
{
    return ((const struct operand *)p)->name;
}

/* GNU gobble_file's dereference chain for command-line operands under
   DEREF_COMMAND_LINE_SYMLINK_TO_DIR / _ARGUMENTS / NEVER, including the
   quirk that a successful stat of a non-directory is followed by an
   lstat whose result (and errno) wins. Returns false when the operand
   was diagnosed and discarded. */
static bool
classify_operand(const char *name, const struct liszt_options *o,
                 struct operand *out)
{
    struct stat st;
    int err;

    switch (o->deref) {
    case LISZT_DEREF_ALWAYS:
        err = stat(name, &st);
        break;
    case LISZT_DEREF_COMMAND_LINE_ARGUMENTS:
        err = stat(name, &st);
        break;
    case LISZT_DEREF_COMMAND_LINE_SYMLINK_TO_DIR:
        err = stat(name, &st);
        if (err < 0 ? (errno == ENOENT || errno == ELOOP)
                    : !S_ISDIR(st.st_mode))
            err = lstat(name, &st);
        break;
    case LISZT_DEREF_NEVER:
    case LISZT_DEREF_UNDEFINED:
    default:
        err = lstat(name, &st);
        break;
    }

    if (err != 0) {
        file_failure(true, "cannot access '%s'", name, errno);
        return false;
    }
    out->name = name;
    out->is_dir = S_ISDIR(st.st_mode) && !o->immediate_dirs;
    return true;
}

struct dir_diag_ctx {
    const char *name;
    bool command_line;
};

static void
on_dirread_fail(void *ctx, enum liszt_dirread_fail how, int errnum)
{
    struct dir_diag_ctx *dc = ctx;
    file_failure(dc->command_line,
                 how == LISZT_DIRFAIL_READ ? "reading directory '%s'"
                                           : "closing directory '%s'",
                 dc->name, errnum);
}

static void
emit_entries(const struct liszt_entries *es)
{
    for (size_t i = 0; i < es->len; i++) {
        const struct liszt_entry *e = &es->v[i];
        liszt_emit_bytes(liszt_entry_name(es, e), e->name_len);
        liszt_emit_byte('\n');
    }
}

/* GNU print_dir: header when print_dir_name, blank line before every
   header but the first output block (static first). */
static void
print_dir(const char *name, bool command_line, bool print_dir_name,
          bool *first, const struct liszt_options *o,
          struct liszt_entries *es)
{
    struct dir_diag_ctx dc = { name, command_line };

    if (liszt_dirread_collect(name, o->ignore, es, on_dirread_fail, &dc)
        < 0) {
        file_failure(command_line, "cannot open directory '%s'", name,
                     errno);
        return;
    }

    liszt_sort_entries(es);

    if (print_dir_name) {
        if (!*first)
            liszt_emit_byte('\n');
        *first = false;
        liszt_emit_bytes(name, strlen(name));
        liszt_emit_str(":\n");
    }
    emit_entries(es);
}

int
main(int argc, char **argv)
{
    struct liszt_options o;

    liszt_set_program(argv[0]);
    setlocale(LC_ALL, "");
    liszt_diag_init();

    liszt_options_parse(argc, argv, &o);

    struct liszt_plan plan;
    liszt_plan_select(&o, &plan);
    liszt_plan_debug_print(&plan);
    liszt_sort_init(&o, &plan);

    /* Classify operands in argv order; failures diagnose and discard
       (GNU gobble_file returning 0 for command-line args). The implicit
       "." goes straight to the directory queue without classification,
       exactly as GNU queues it unstatted. */
    struct operand *ops = NULL;
    int n_ops = 0;
    bool implicit_dot = o.n_operands == 0;

    if (!implicit_dot) {
        ops = liszt_xmalloc((size_t)o.n_operands * sizeof *ops);
        for (int i = 0; i < o.n_operands; i++)
            if (classify_operand(o.operands[i], &o, &ops[n_ops]))
                n_ops++;
    }

    /* GNU sorts the whole command-line batch (files and dirs together),
       then extracts dirs preserving that order. */
    if (o.sort != LISZT_SORT_NONE && n_ops > 1)
        liszt_sort_operands(ops, (size_t)n_ops, sizeof *ops, operand_name);

    int n_files = 0;
    int n_dirs = implicit_dot ? 1 : 0;
    for (int i = 0; i < n_ops; i++)
        n_files += !ops[i].is_dir;
    for (int i = 0; i < n_ops; i++)
        n_dirs += ops[i].is_dir;

    /* Non-directory operands print first, in operand order (sort is
       none). */
    for (int i = 0; i < n_ops; i++) {
        if (ops[i].is_dir)
            continue;
        liszt_emit_bytes(ops[i].name, strlen(ops[i].name));
        liszt_emit_byte('\n');
    }
    if (n_files > 0 && n_dirs > 0)
        liszt_emit_byte('\n');

    /* GNU: headers unless nothing was printed, at most one operand was
       given, and exactly one directory is pending. */
    bool print_dir_name = true;
    if (n_files == 0 && o.n_operands <= 1 && n_dirs == 1)
        print_dir_name = false;

    struct liszt_entries es;
    liszt_entries_init(&es);
    bool first = true;

    if (implicit_dot) {
        print_dir(".", true, print_dir_name, &first, &o, &es);
    } else {
        for (int i = 0; i < n_ops; i++)
            if (ops[i].is_dir)
                print_dir(ops[i].name, true, print_dir_name, &first, &o,
                          &es);
    }

    liszt_entries_free(&es);
    free(ops);
    free(o.operands);

    int werr;
    if (liszt_emit_finish(&werr) < 0) {
        liszt_error(werr, "write error");
        return LISZT_STATUS_SERIOUS;
    }
    return liszt_exit_status();
}

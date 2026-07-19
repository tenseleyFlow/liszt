#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __linux__
#include <sys/sysmacros.h>
#endif

#include "config.h"
#include "dirread.h"
#include "emit.h"
#include "entry.h"
#include "human.h"
#include "idcache.h"
#include "options.h"
#include "plan.h"
#include "sortkey.h"
#include "sys/xstat.h"
#include "timefmt.h"
#include "util.h"

#define ST_NBLOCKSIZE 512

static struct liszt_plan plan;

static void
file_failure(bool serious, const char *fmt_with_name, const char *name,
             int errnum)
{
    /* GNU quotes shell-escape-always style; the simple-name subset is
       what current fixtures exercise (sprint 04 owns full quoting). */
    fprintf(stderr, "%s: ", liszt_prog);
    fprintf(stderr, fmt_with_name, name);
    fprintf(stderr, ": %s\n", strerror(errnum));
    liszt_set_exit_status(serious);
}

/* --- per-listing rendering state ------------------------------------- */

/* Column width maxima for one listing batch (a directory or the
   command-line file batch), GNU's two-pass model: accumulate from
   rendered fields, then emit against the maxima. */
struct lwidths {
    int inode;
    int blocks;
    int nlink;
    int owner;
    int group;
    int author;
    int size;
    int major;
    int minor;
    bool any_acl;
};

/* The renderer's view of one listable item; both directory entries and
   command-line operands compile into this. */
struct litem {
    const char *name;
    const struct liszt_statinfo *st;
    const char *linkname;       /* NULL = none */
    mode_t linkmode;
    enum liszt_ftype ftype;
    unsigned char stat_ok;
    unsigned char acl;          /* 0 none, 1 '.', 2 '+' */
};

static int
digits_umax(uintmax_t v)
{
    int n = 1;
    while (v >= 10) {
        v /= 10;
        n++;
    }
    return n;
}

static void
widths_add(struct lwidths *w, const struct liszt_options *o,
           const struct litem *it)
{
    char hbuf[LISZT_LONGEST_HUMAN_READABLE + 1];

    if (it->acl)
        w->any_acl = true;
    if (!it->stat_ok)
        return;     /* GNU: failed stats contribute no widths */

    const struct liszt_statinfo *st = it->st;

    if (o->print_inode) {
        int len = digits_umax((uintmax_t)st->ino);
        if (w->inode < len)
            w->inode = len;
    }
    if (o->print_block_size || o->format == LISZT_FMT_LONG) {
        int len = (int)strlen(liszt_human_readable(
            (uintmax_t)st->blocks, hbuf, o->human_output_opts,
            ST_NBLOCKSIZE, o->output_block_size));
        if (w->blocks < len)
            w->blocks = len;
    }
    if (o->format != LISZT_FMT_LONG)
        return;

    int len = digits_umax((uintmax_t)st->nlink);
    if (w->nlink < len)
        w->nlink = len;

    if (o->print_owner) {
        const char *nm = o->numeric_ids ? NULL : liszt_getuser(st->uid);
        len = nm ? (int)strlen(nm) : digits_umax((uintmax_t)st->uid);
        if (w->owner < len)
            w->owner = len;
    }
    if (o->print_group) {
        const char *nm = o->numeric_ids ? NULL : liszt_getgroup(st->gid);
        len = nm ? (int)strlen(nm) : digits_umax((uintmax_t)st->gid);
        if (w->group < len)
            w->group = len;
    }
    if (o->print_author) {
        const char *nm = o->numeric_ids ? NULL : liszt_getuser(st->uid);
        len = nm ? (int)strlen(nm) : digits_umax((uintmax_t)st->uid);
        if (w->author < len)
            w->author = len;
    }

    if (S_ISCHR(st->mode) || S_ISBLK(st->mode)) {
        len = digits_umax((uintmax_t)major(st->rdev));
        if (w->major < len)
            w->major = len;
        len = digits_umax((uintmax_t)minor(st->rdev));
        if (w->minor < len)
            w->minor = len;
        len = w->major + 2 + w->minor;
        if (w->size < len)
            w->size = len;
    } else {
        len = (int)strlen(liszt_human_readable(
            (uintmax_t)st->size, hbuf, o->file_human_output_opts, 1,
            o->file_output_block_size));
        if (w->size < len)
            w->size = len;
    }
}

/* Inode and blocks prefixes shared by every format (-i, -s). */
static void
emit_frills(const struct liszt_options *o, const struct lwidths *w,
            const struct litem *it)
{
    char buf[64 + LISZT_LONGEST_HUMAN_READABLE];
    char hbuf[LISZT_LONGEST_HUMAN_READABLE + 1];

    if (o->print_inode) {
        int n = it->stat_ok
            ? snprintf(buf, sizeof buf, "%*ju ", w->inode,
                       (uintmax_t)it->st->ino)
            : snprintf(buf, sizeof buf, "%*s ", w->inode, "?");
        liszt_emit_bytes(buf, (size_t)n);
    }
    if (o->print_block_size) {
        const char *blocks = !it->stat_ok
            ? "?"
            : liszt_human_readable((uintmax_t)it->st->blocks, hbuf,
                                   o->human_output_opts, ST_NBLOCKSIZE,
                                   o->output_block_size);
        int n = snprintf(buf, sizeof buf, "%*s ", w->blocks, blocks);
        liszt_emit_bytes(buf, (size_t)n);
    }
}

/* GNU format_user_or_group: a name gets gap+1 trailing spaces; numeric
   ids render right-aligned with one trailing space. */
static void
emit_id_field(const char *name, uintmax_t id, int width)
{
    char buf[64];

    if (name) {
        int gap = width - (int)strlen(name);
        int pad = gap > 0 ? gap : 0;
        liszt_emit_str(name);
        do
            liszt_emit_byte(' ');
        while (pad--);
    } else {
        int n = snprintf(buf, sizeof buf, "%*ju ", width, id);
        liszt_emit_bytes(buf, (size_t)n);
    }
}

static void
emit_long_entry(const struct liszt_options *o, const struct lwidths *w,
                const struct litem *it)
{
    char modebuf[12];
    char buf[256 + LISZT_LONGEST_HUMAN_READABLE];
    char hbuf[LISZT_LONGEST_HUMAN_READABLE + 1];
    const struct liszt_statinfo *st = it->st;

    emit_frills(o, w, it);

    static const char type_letter[] = {
        '?', 'p', 'c', 'd', 'b', '-', 'l', 's', 'w'
    };
    if (it->stat_ok) {
        liszt_filemodestring(st->mode, modebuf);
    } else {
        modebuf[0] = type_letter[it->ftype];
        memset(modebuf + 1, '?', 10);
        modebuf[11] = '\0';
    }
    if (!w->any_acl)
        modebuf[10] = '\0';
    else if (it->acl == 1)
        modebuf[10] = '.';
    else if (it->acl == 2)
        modebuf[10] = '+';
    /* else: strmode's ' ' stands, aligning with suffixed neighbors */

    int n;
    if (it->stat_ok)
        n = snprintf(buf, sizeof buf, "%s %*ju ", modebuf, w->nlink,
                     (uintmax_t)st->nlink);
    else
        n = snprintf(buf, sizeof buf, "%s %*s ", modebuf, w->nlink, "?");
    liszt_emit_bytes(buf, (size_t)n);

    if (o->print_owner)
        emit_id_field(!it->stat_ok ? "?"
                      : o->numeric_ids ? NULL : liszt_getuser(st->uid),
                      (uintmax_t)st->uid, w->owner);
    if (o->print_group)
        emit_id_field(!it->stat_ok ? "?"
                      : o->numeric_ids ? NULL : liszt_getgroup(st->gid),
                      (uintmax_t)st->gid, w->group);
    if (o->print_author)
        emit_id_field(!it->stat_ok ? "?"
                      : o->numeric_ids ? NULL : liszt_getuser(st->uid),
                      (uintmax_t)st->uid, w->author);

    if (it->stat_ok && (S_ISCHR(st->mode) || S_ISBLK(st->mode))) {
        int blanks = w->size - (w->major + 2 + w->minor);
        n = snprintf(buf, sizeof buf, "%*ju, %*ju ",
                     w->major + (blanks > 0 ? blanks : 0),
                     (uintmax_t)major(st->rdev), w->minor,
                     (uintmax_t)minor(st->rdev));
        liszt_emit_bytes(buf, (size_t)n);
    } else {
        const char *size = !it->stat_ok
            ? "?"
            : liszt_human_readable((uintmax_t)st->size, hbuf,
                                   o->file_human_output_opts, 1,
                                   o->file_output_block_size);
        n = snprintf(buf, sizeof buf, "%*s ", w->size, size);
        liszt_emit_bytes(buf, (size_t)n);
    }

    char tbuf[LISZT_TIME_BUFSZ];
    size_t tlen = it->stat_ok ? liszt_timefmt_render(tbuf, st->mtime) : 0;
    if (tlen > 0) {
        liszt_emit_bytes(tbuf, tlen);
        liszt_emit_byte(' ');
    } else {
        char sbuf[32];
        if (!it->stat_ok) {
            n = snprintf(buf, sizeof buf, "%*s ",
                         liszt_timefmt_expected_width(), "?");
        } else {
            snprintf(sbuf, sizeof sbuf, "%jd",
                     (intmax_t)st->mtime.tv_sec);
            n = snprintf(buf, sizeof buf, "%*s ",
                         liszt_timefmt_expected_width(), sbuf);
        }
        liszt_emit_bytes(buf, (size_t)n);
    }

    liszt_emit_str(it->name);
    if (it->ftype == LISZT_T_LNK && it->linkname) {
        liszt_emit_str(" -> ");
        liszt_emit_str(it->linkname);
    }
    liszt_emit_byte('\n');
}

static void
emit_item(const struct liszt_options *o, const struct lwidths *w,
          const struct litem *it)
{
    if (o->format == LISZT_FMT_LONG) {
        emit_long_entry(o, w, it);
    } else {
        emit_frills(o, w, it);
        liszt_emit_str(it->name);
        liszt_emit_byte('\n');
    }
}

/* --- command-line operands ------------------------------------------- */

struct operand {
    const char *name;
    struct liszt_statinfo st;
    char *linkname;
    mode_t linkmode;
    enum liszt_ftype ftype;
    unsigned char stat_ok;
    unsigned char acl;
    bool is_dir;
};

static void
operand_item(const void *p, struct liszt_item *out)
{
    const struct operand *op = p;
    out->name = op->name;
    out->size = op->st.size;
    out->mtime = op->st.mtime;
    out->group_dir = false;     /* grouping cannot affect operand output */
}

static enum liszt_ftype
ftype_from_mode(mode_t mode)
{
    if (S_ISDIR(mode))
        return LISZT_T_DIR;
    if (S_ISLNK(mode))
        return LISZT_T_LNK;
    if (S_ISFIFO(mode))
        return LISZT_T_FIFO;
    if (S_ISSOCK(mode))
        return LISZT_T_SOCK;
    if (S_ISCHR(mode))
        return LISZT_T_CHR;
    if (S_ISBLK(mode))
        return LISZT_T_BLK;
    if (S_ISREG(mode))
        return LISZT_T_REG;
    return LISZT_T_UNKNOWN;
}

/* One llistxattr answers ACL and security-context presence together
   (GNU file_has_aclinfo's syscall shape - measured 2x cheaper than
   per-attribute probes on the 100k -l lane). */
static unsigned char
probe_acl(const char *dir, const char *name, bool is_dir)
{
    char buf[4096];
    long n = liszt_xattr_list_join(dir, name, buf, sizeof buf);

    if (n <= 0)
        return 0;
    bool acl = false;
    bool ctx = false;
    for (long off = 0; off < n; off += (long)strlen(buf + off) + 1) {
        const char *attr = buf + off;
        if (strcmp(attr, "system.posix_acl_access") == 0
            || (is_dir && strcmp(attr, "system.posix_acl_default") == 0))
            acl = true;
        else if (strcmp(attr, "security.selinux") == 0)
            ctx = true;
    }
    return acl ? 2 : ctx ? 1 : 0;
}

/* GNU gobble_file's dereference chain for command-line operands,
   including the successful-stat-then-lstat fall-through. */
static bool
classify_operand(const char *name, const struct liszt_options *o,
                 struct operand *out)
{
    struct liszt_statinfo st;
    int err;

    memset(out, 0, sizeof *out);
    switch (o->deref) {
    case LISZT_DEREF_ALWAYS:
    case LISZT_DEREF_COMMAND_LINE_ARGUMENTS:
        err = liszt_stat_path(name, &st);
        break;
    case LISZT_DEREF_COMMAND_LINE_SYMLINK_TO_DIR:
        err = liszt_stat_path(name, &st);
        if (err < 0 ? (errno == ENOENT || errno == ELOOP)
                    : !S_ISDIR(st.mode))
            err = liszt_lstat_path(name, &st);
        break;
    case LISZT_DEREF_NEVER:
    case LISZT_DEREF_UNDEFINED:
    default:
        err = liszt_lstat_path(name, &st);
        break;
    }

    if (err != 0) {
        file_failure(true, "cannot access '%s'", name, errno);
        return false;
    }
    out->name = name;
    out->st = st;
    out->stat_ok = 1;
    out->ftype = ftype_from_mode(st.mode);
    out->is_dir = S_ISDIR(st.mode) && !o->immediate_dirs;

    /* GNU decorates every command-line operand during gobble - dirs
       included, since widths and any_has_acl accumulate before dirs are
       extracted from the batch. */
    if (plan.needs_link_target && out->ftype == LISZT_T_LNK) {
        out->linkname = liszt_readlink_join("", name);
        if (!out->linkname)
            file_failure(false, "cannot read symbolic link '%s'",
                         name, errno);
    }
    if (plan.needs_xattr)
        out->acl = probe_acl("", name, out->is_dir);
    return true;
}

/* --- directories ------------------------------------------------------ */

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

/* The plan's per-entry fetch pass: one statx with the derived mask,
   plus readlink/xattr when the long format needs them, plus the
   grouping resolutions carried over from sprint 02. */
static void
fill_meta(const char *dirname, const struct liszt_options *o,
          struct liszt_entries *es)
{
    bool group = o->group_directories_first && o->sort != LISZT_SORT_NONE;

    if (!plan.needs_stat && !group)
        return;
    liszt_entries_ensure_meta(es);

    for (size_t i = 0; i < es->len; i++) {
        struct liszt_entry *e = &es->v[i];
        struct liszt_entrymeta *m = &es->meta[e->meta_idx];
        const char *nm = liszt_entry_name(es, e);

        if (plan.needs_stat
            || (group && e->ftype == LISZT_T_UNKNOWN)) {
            if (liszt_statx_join(dirname, nm, plan.stat_wants, false,
                                 &m->st) == 0) {
                m->stat_ok = 1;
                if (plan.stat_wants & LISZT_WANT_MODE)
                    e->ftype = (uint8_t)ftype_from_mode(m->st.mode);
            } else {
                file_failure(false, "cannot access '%s'",
                             liszt_join_path(dirname, nm), errno);
                continue;
            }
        }
        if (e->ftype == LISZT_T_LNK) {
            if (plan.needs_link_target) {
                char *target = liszt_readlink_join(dirname, nm);
                if (target) {
                    m->link_off = liszt_entries_add_bytes(es, target,
                                                          strlen(target));
                    free(target);
                } else {
                    file_failure(false, "cannot read symbolic link '%s'",
                                 liszt_join_path(dirname, nm), errno);
                }
            }
            if (group) {
                struct liszt_statinfo ti;
                if (liszt_stat_join(dirname, nm, &ti) == 0)
                    m->linkmode = ti.mode;
            }
        }
        if (plan.needs_xattr)
            m->acl = probe_acl(dirname, nm, e->ftype == LISZT_T_DIR);
    }
}

static void
entry_to_item(const struct liszt_entries *es, const struct liszt_entry *e,
              struct litem *it)
{
    static const struct liszt_statinfo zero_st;
    const struct liszt_entrymeta *m =
        es->meta ? &es->meta[e->meta_idx] : NULL;

    it->name = liszt_entry_name(es, e);
    it->st = m ? &m->st : &zero_st;
    it->linkname = (m && m->link_off != UINT32_MAX)
        ? (const char *)es->arena + m->link_off
        : NULL;
    it->linkmode = m ? m->linkmode : 0;
    it->ftype = e->ftype;
    it->stat_ok = m ? m->stat_ok : 0;
    it->acl = m ? m->acl : 0;
}

static bool
needs_columns(const struct liszt_options *o)
{
    return o->format == LISZT_FMT_LONG || o->print_block_size
        || o->print_inode;
}

static void
emit_entries(const struct liszt_options *o, struct liszt_entries *es,
             bool with_total)
{
    struct lwidths w = { 0 };
    struct litem it;

    if (needs_columns(o)) {
        for (size_t i = 0; i < es->len; i++) {
            entry_to_item(es, &es->v[i], &it);
            widths_add(&w, o, &it);
        }
    }

    if (with_total
        && (o->format == LISZT_FMT_LONG || o->print_block_size)) {
        uintmax_t total = 0;
        for (size_t i = 0; i < es->len; i++) {
            entry_to_item(es, &es->v[i], &it);
            if (it.stat_ok)
                total += (uintmax_t)it.st->blocks;
        }
        char hbuf[LISZT_LONGEST_HUMAN_READABLE + 1];
        liszt_emit_str("total ");
        liszt_emit_str(liszt_human_readable(total, hbuf,
                                            o->human_output_opts,
                                            ST_NBLOCKSIZE,
                                            o->output_block_size));
        liszt_emit_byte('\n');
    }

    for (size_t i = 0; i < es->len; i++) {
        entry_to_item(es, &es->v[i], &it);
        emit_item(o, &w, &it);
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

    fill_meta(name, o, es);
    liszt_sort_entries(es);

    if (print_dir_name) {
        if (!*first)
            liszt_emit_byte('\n');
        *first = false;
        liszt_emit_bytes(name, strlen(name));
        liszt_emit_str(":\n");
    }
    emit_entries(o, es, true);
}

int
main(int argc, char **argv)
{
    struct liszt_options o;

    liszt_set_program(argv[0]);
    setlocale(LC_ALL, "");
    liszt_diag_init();

    liszt_options_parse(argc, argv, &o);

    liszt_plan_select(&o, &plan);
    liszt_plan_debug_print(&plan);
    liszt_sort_init(&o, &plan);

    /* Classify operands in argv order; failures diagnose and discard.
       The implicit "." goes straight to the directory queue without
       classification, exactly as GNU queues it unstatted. */
    struct operand *ops = NULL;
    int n_ops = 0;
    bool implicit_dot = o.n_operands == 0;

    if (!implicit_dot) {
        ops = liszt_xmalloc((size_t)o.n_operands * sizeof *ops);
        for (int i = 0; i < o.n_operands; i++)
            if (classify_operand(o.operands[i], &o, &ops[n_ops]))
                n_ops++;
    }

    if (o.sort != LISZT_SORT_NONE && n_ops > 1)
        liszt_sort_operands(ops, (size_t)n_ops, sizeof *ops, operand_item);

    int n_files = 0;
    int n_dirs = implicit_dot ? 1 : 0;
    for (int i = 0; i < n_ops; i++)
        n_files += !ops[i].is_dir;
    for (int i = 0; i < n_ops; i++)
        n_dirs += ops[i].is_dir;

    /* Non-directory operands print first (no total line). Widths span
       the WHOLE operand batch, dirs included: GNU accumulates widths in
       gobble_file before extract_dirs_from_files removes the dirs
       (pinned by fuzz - a dir operand's size widens the file batch's
       size column). */
    if (n_files > 0) {
        struct lwidths w = { 0 };
        struct litem it;
        if (needs_columns(&o)) {
            for (int i = 0; i < n_ops; i++) {
                it = (struct litem){ ops[i].name, &ops[i].st,
                                     ops[i].linkname, ops[i].linkmode,
                                     ops[i].ftype, ops[i].stat_ok,
                                     ops[i].acl };
                widths_add(&w, &o, &it);
            }
        }
        for (int i = 0; i < n_ops; i++) {
            if (ops[i].is_dir)
                continue;
            it = (struct litem){ ops[i].name, &ops[i].st,
                                 ops[i].linkname, ops[i].linkmode,
                                 ops[i].ftype, ops[i].stat_ok,
                                 ops[i].acl };
            emit_item(&o, &w, &it);
        }
    }
    if (n_files > 0 && n_dirs > 0)
        liszt_emit_byte('\n');

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
    for (int i = 0; i < n_ops; i++)
        free(ops[i].linkname);
    free(ops);
    free(o.operands);

    int werr;
    if (liszt_emit_finish(&werr) < 0) {
        liszt_error(werr, "write error");
        return LISZT_STATUS_SERIOUS;
    }
    return liszt_exit_status();
}

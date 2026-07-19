#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
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
#include "layout.h"
#include "options.h"
#include "plan.h"
#include "quote.h"
#include "sortkey.h"
#include "sys/xstat.h"
#include "timefmt.h"
#include "util.h"

#define ST_NBLOCKSIZE 512

static struct liszt_plan plan;
static const struct liszt_options *cur_opts;
static bool cur_some_quoted;

/* GNU quoteaf: shell-escape-always rendering for diagnostics. Static
   rotating buffer, two slots. */
static const char *
quote_af(const char *name)
{
    static char *slots[2];
    static size_t caps[2];
    static int turn;
    static const struct liszt_qopts af_opts = {
        .style = LISZT_QS_SHELL_ESCAPE_ALWAYS
    };

    turn = 1 - turn;
    size_t need = liszt_quotearg_buffer(slots[turn], caps[turn], name,
                                        (size_t)-1, &af_opts);
    if (need >= caps[turn]) {
        caps[turn] = need + 1;
        slots[turn] = liszt_xrealloc(slots[turn], caps[turn]);
        liszt_quotearg_buffer(slots[turn], caps[turn], name, (size_t)-1,
                              &af_opts);
    }
    return slots[turn];
}

static void
file_failure(bool serious, const char *fmt_with_name, const char *name,
             int errnum)
{
    fprintf(stderr, "%s: ", liszt_prog);
    fprintf(stderr, fmt_with_name, quote_af(name));
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
    const char *qname;          /* display form (may equal name) */
    size_t qlen;
    int width;                  /* display width, no pad */
    const struct liszt_statinfo *st;
    const char *linkname;       /* NULL = none */
    mode_t linkmode;
    enum liszt_ftype ftype;
    unsigned char stat_ok;
    unsigned char acl;          /* 0 none, 1 '.', 2 '+' */
    unsigned char quoted;
    unsigned char padded;
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

/* Whether this run needs display widths (columns, commas, width sort). */
static bool
needs_widths(const struct liszt_options *o)
{
    return o->format == LISZT_FMT_MANY
        || o->format == LISZT_FMT_HORIZONTAL
        || o->format == LISZT_FMT_COMMAS
        || o->sort == LISZT_SORT_WIDTH;
}

/* The align pad byte, then the display bytes. */
static void
emit_name(const struct litem *it)
{
    if (it->padded)
        liszt_emit_byte(' ');
    liszt_emit_bytes(it->qname, it->qlen);
}

/* Inode and blocks prefixes shared by every format (-i, -s). */
static void
emit_frills(const struct liszt_options *o, const struct lwidths *w,
            const struct litem *it)
{
    char buf[64 + LISZT_LONGEST_HUMAN_READABLE];
    char hbuf[LISZT_LONGEST_HUMAN_READABLE + 1];
    bool commas = o->format == LISZT_FMT_COMMAS;
    int iw = commas ? 0 : w->inode;
    int bw = commas ? 0 : w->blocks;

    if (o->print_inode) {
        int n = it->stat_ok
            ? snprintf(buf, sizeof buf, "%*ju ", iw,
                       (uintmax_t)it->st->ino)
            : snprintf(buf, sizeof buf, "%*s ", iw, "?");
        liszt_emit_bytes(buf, (size_t)n);
    }
    if (o->print_block_size) {
        const char *blocks = !it->stat_ok
            ? "?"
            : liszt_human_readable((uintmax_t)it->st->blocks, hbuf,
                                   o->human_output_opts, ST_NBLOCKSIZE,
                                   o->output_block_size);
        int n = snprintf(buf, sizeof buf, "%*s ", bw, blocks);
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

    emit_name(it);
    if (it->ftype == LISZT_T_LNK && it->linkname) {
        liszt_emit_str(" -> ");
        /* Targets always take the general-quoting path, but never the
           align pad (both pinned by fuzz vs the oracle). */
        size_t lt_len;
        int lt_width;
        bool lt_quoted;
        const char *tq = liszt_quote_name(it->linkname,
                                          &cur_opts->filename_qopts,
                                          cur_opts->qmark_funny_chars,
                                          false, &lt_len, &lt_width,
                                          &lt_quoted);
        liszt_emit_bytes(tq, lt_len);
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
        emit_name(it);
        liszt_emit_byte('\n');
    }
}

/* --- command-line operands ------------------------------------------- */

struct operand {
    const char *name;
    char *qname;                /* malloc'd display form, or NULL = raw */
    size_t qlen;
    int disp_width;
    struct liszt_statinfo st;
    char *linkname;
    mode_t linkmode;
    enum liszt_ftype ftype;
    unsigned char stat_ok;
    unsigned char acl;
    unsigned char quoted;
    unsigned char padded;
    bool is_dir;
};

static void
operand_item(const void *p, struct liszt_item *out)
{
    const struct operand *op = p;
    out->name = op->name;
    out->size = op->st.size;
    out->mtime = op->st.mtime;
    out->width = op->disp_width + op->padded;
    out->group_dir = false;     /* grouping cannot affect operand output */
}

/* Operand decoration mirrors the entry pass; the quoted probe spans the
   whole batch (dirs included) for the align pad, exactly as gobble
   accumulates cwd_some_quoted before extraction. */
static void
decorate_operands(const struct liszt_options *o, struct operand *ops,
                  int n_ops)
{
    bool bytes_can_change = o->quoting_style != LISZT_QS_LITERAL
        || o->qmark_funny_chars;
    bool want_width = needs_widths(o);

    cur_some_quoted = false;
    if (!bytes_can_change && !want_width
        && !o->align_variable_outer_quotes)
        return;
    for (int i = 0; i < n_ops; i++) {
        size_t qlen;
        int width = 0;
        bool quoted;
        const char *q = liszt_quote_name(ops[i].name,
                                         &o->filename_qopts,
                                         o->qmark_funny_chars, want_width,
                                         &qlen, &width, &quoted);
        if (qlen != strlen(ops[i].name)
            || memcmp(q, ops[i].name, qlen) != 0) {
            ops[i].qname = liszt_xmalloc(qlen + 1);
            memcpy(ops[i].qname, q, qlen + 1);
        }
        ops[i].qlen = qlen;
        ops[i].disp_width = width;
        ops[i].quoted = quoted;
        if (quoted)
            cur_some_quoted = true;
    }
    for (int i = 0; i < n_ops; i++)
        ops[i].padded = o->align_variable_outer_quotes && cur_some_quoted
            && !ops[i].quoted;
}

static void
operand_to_litem(const struct operand *op, struct litem *it)
{
    it->name = op->name;
    it->qname = op->qname ? op->qname : op->name;
    it->qlen = op->qname ? op->qlen : strlen(op->name);
    it->width = op->disp_width;
    it->quoted = op->quoted;
    it->padded = op->padded;
    it->st = &op->st;
    it->linkname = op->linkname;
    it->linkmode = op->linkmode;
    it->ftype = op->ftype;
    it->stat_ok = op->stat_ok;
    it->acl = op->acl;
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
        file_failure(true, "cannot access %s", name, errno);
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
            file_failure(false, "cannot read symbolic link %s",
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
                 how == LISZT_DIRFAIL_READ ? "reading directory %s"
                                           : "closing directory %s",
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
                file_failure(false, "cannot access %s",
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
                    file_failure(false, "cannot read symbolic link %s",
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
    if (m && m->quoted_off != UINT32_MAX) {
        it->qname = (const char *)es->arena + m->quoted_off;
        it->qlen = m->quoted_len;
    } else {
        it->qname = it->name;
        it->qlen = e->name_len;
    }
    it->width = m ? m->disp_width : 0;
    it->quoted = m ? m->quoted : 0;
    it->padded = m ? m->padded : 0;
    it->st = m ? &m->st : &zero_st;
    it->linkname = (m && m->link_off != UINT32_MAX)
        ? (const char *)es->arena + m->link_off
        : NULL;
    it->linkmode = m ? m->linkmode : 0;
    it->ftype = e->ftype;
    it->stat_ok = m ? m->stat_ok : 0;
    it->acl = m ? m->acl : 0;
}

/* The decoration pass: quoted display form, width, quoted flag - once
   per entry, before sorting (width sort reads the cache). The plain
   piped path (literal style, no qmark, no widths) skips everything. */
static void
decorate_entries(const struct liszt_options *o, struct liszt_entries *es)
{
    bool bytes_can_change = o->quoting_style != LISZT_QS_LITERAL
        || o->qmark_funny_chars;
    bool want_width = needs_widths(o);

    cur_some_quoted = false;
    if (!bytes_can_change && !want_width
        && !o->align_variable_outer_quotes)
        return;
    liszt_entries_ensure_meta(es);

    for (size_t i = 0; i < es->len; i++) {
        struct liszt_entry *e = &es->v[i];
        struct liszt_entrymeta *m = &es->meta[e->meta_idx];
        size_t qlen;
        int width = 0;
        bool quoted;
        const char *q = liszt_quote_name(liszt_entry_name(es, e),
                                         &o->filename_qopts,
                                         o->qmark_funny_chars, want_width,
                                         &qlen, &width, &quoted);
        if (qlen != e->name_len
            || memcmp(q, liszt_entry_name(es, e), qlen) != 0) {
            m->quoted_off = liszt_entries_add_bytes(es, q, qlen);
            m->quoted_len = (uint32_t)qlen;
        }
        m->disp_width = width;
        m->quoted = quoted;
        if (quoted)
            cur_some_quoted = true;
    }
    for (size_t i = 0; i < es->len; i++) {
        struct liszt_entrymeta *m = &es->meta[es->v[i].meta_idx];
        m->padded = o->align_variable_outer_quotes && cur_some_quoted
            && !m->quoted;
    }
}

static bool
needs_columns(const struct liszt_options *o)
{
    return o->format == LISZT_FMT_LONG || o->print_block_size
        || o->print_inode;
}

struct batch_ctx {
    const struct liszt_options *o;
    const struct litem *items;
    const struct lwidths *w;
};

static void
layout_emit_cb(size_t idx, size_t start_col, void *vctx)
{
    struct batch_ctx *c = vctx;
    (void)start_col;
    emit_frills(c->o, c->w, &c->items[idx]);
    emit_name(&c->items[idx]);
}

/* GNU length_of_file_name_and_frills: frills use column widths for
   -C/-x but natural widths for -m; name length is quoted width plus the
   align pad. */
static size_t
item_length(const struct liszt_options *o, const struct lwidths *w,
            const struct litem *it)
{
    size_t len = 0;
    char hbuf[LISZT_LONGEST_HUMAN_READABLE + 1];

    if (o->print_inode)
        len += 1 + (o->format == LISZT_FMT_COMMAS
                    ? (it->stat_ok
                       ? (size_t)digits_umax((uintmax_t)it->st->ino)
                       : 1)
                    : (size_t)w->inode);
    if (o->print_block_size)
        len += 1 + (o->format == LISZT_FMT_COMMAS
                    ? (it->stat_ok
                       ? strlen(liszt_human_readable(
                             (uintmax_t)it->st->blocks, hbuf,
                             o->human_output_opts, ST_NBLOCKSIZE,
                             o->output_block_size))
                       : 1)
                    : (size_t)w->blocks);
    len += (size_t)(it->width + it->padded);
    return len;
}

static void
emit_batch(const struct liszt_options *o, const struct litem *items,
           size_t n, bool with_total)
{
    struct lwidths w = { 0 };

    if (needs_columns(o))
        for (size_t i = 0; i < n; i++)
            widths_add(&w, o, &items[i]);

    if (with_total
        && (o->format == LISZT_FMT_LONG || o->print_block_size)) {
        uintmax_t total = 0;
        for (size_t i = 0; i < n; i++)
            if (items[i].stat_ok)
                total += (uintmax_t)items[i].st->blocks;
        char hbuf[LISZT_LONGEST_HUMAN_READABLE + 1];
        liszt_emit_str("total ");
        liszt_emit_str(liszt_human_readable(total, hbuf,
                                            o->human_output_opts,
                                            ST_NBLOCKSIZE,
                                            o->output_block_size));
        liszt_emit_byte('\n');
    }

    if (n == 0)
        return;

    switch (o->format) {
    case LISZT_FMT_MANY:
    case LISZT_FMT_HORIZONTAL: {
        struct batch_ctx ctx = { o, items, &w };
        if (!o->line_length) {
            size_t *lengths = liszt_xmalloc(n * sizeof *lengths);
            for (size_t i = 0; i < n; i++)
                lengths[i] = item_length(o, &w, &items[i]);
            liszt_layout_separated(n, lengths, ' ', 0, layout_emit_cb,
                                   &ctx);
            free(lengths);
            break;
        }
        size_t *lengths = liszt_xmalloc(n * sizeof *lengths);
        for (size_t i = 0; i < n; i++)
            lengths[i] = item_length(o, &w, &items[i]);
        liszt_layout_columns(n, lengths, o->format == LISZT_FMT_MANY,
                             o->line_length, o->max_idx, o->tabsize,
                             layout_emit_cb, &ctx);
        free(lengths);
        break;
    }
    case LISZT_FMT_COMMAS: {
        struct batch_ctx ctx = { o, items, &w };
        size_t *lengths = liszt_xmalloc(n * sizeof *lengths);
        for (size_t i = 0; i < n; i++)
            lengths[i] = item_length(o, &w, &items[i]);
        liszt_layout_separated(n, lengths, ',', o->line_length,
                               layout_emit_cb, &ctx);
        free(lengths);
        break;
    }
    case LISZT_FMT_LONG:
    case LISZT_FMT_ONE:
    default:
        for (size_t i = 0; i < n; i++)
            emit_item(o, &w, &items[i]);
        break;
    }
}

static void
emit_entries(const struct liszt_options *o, struct liszt_entries *es,
             bool with_total)
{
    struct litem *items = liszt_xmalloc(es->len * sizeof *items);

    for (size_t i = 0; i < es->len; i++)
        entry_to_item(es, &es->v[i], &items[i]);
    emit_batch(o, items, es->len, with_total);
    free(items);
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
        file_failure(command_line, "cannot open directory %s", name,
                     errno);
        return;
    }

    fill_meta(name, o, es);
    decorate_entries(o, es);
    liszt_sort_entries(es);

    if (print_dir_name) {
        if (!*first)
            liszt_emit_byte('\n');
        *first = false;
        size_t hlen;
        int hwidth;
        bool hquoted;
        const char *hq = liszt_quote_name(name, &o->dirname_qopts,
                                          o->qmark_funny_chars, false,
                                          &hlen, &hwidth, &hquoted);
        liszt_emit_bytes(hq, hlen);
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
    cur_opts = &o;

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

    decorate_operands(&o, ops, n_ops);
    bool ops_some_quoted = cur_some_quoted;

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
       size column). The file batch itself lays out through emit_batch,
       but its column widths still come from the full-batch pass. */
    if (n_files > 0) {
        cur_some_quoted = ops_some_quoted;
        struct lwidths w = { 0 };
        struct litem it;
        struct litem *fitems = liszt_xmalloc((size_t)n_files
                                             * sizeof *fitems);
        int nf = 0;
        if (needs_columns(&o)) {
            for (int i = 0; i < n_ops; i++) {
                operand_to_litem(&ops[i], &it);
                widths_add(&w, &o, &it);
            }
        }
        for (int i = 0; i < n_ops; i++)
            if (!ops[i].is_dir)
                operand_to_litem(&ops[i], &fitems[nf++]);

        if (o.format == LISZT_FMT_LONG || o.format == LISZT_FMT_ONE) {
            for (int i = 0; i < nf; i++)
                emit_item(&o, &w, &fitems[i]);
        } else {
            struct batch_ctx ctx = { &o, fitems, &w };
            size_t *lengths = liszt_xmalloc((size_t)nf * sizeof *lengths);
            for (int i = 0; i < nf; i++)
                lengths[i] = item_length(&o, &w, &fitems[i]);
            if (o.format == LISZT_FMT_COMMAS)
                liszt_layout_separated((size_t)nf, lengths, ',',
                                       o.line_length, layout_emit_cb,
                                       &ctx);
            else if (!o.line_length)
                liszt_layout_separated((size_t)nf, lengths, ' ', 0,
                                       layout_emit_cb, &ctx);
            else
                liszt_layout_columns((size_t)nf, lengths,
                                     o.format == LISZT_FMT_MANY,
                                     o.line_length, o.max_idx, o.tabsize,
                                     layout_emit_cb, &ctx);
            free(lengths);
        }
        free(fitems);
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
    for (int i = 0; i < n_ops; i++) {
        free(ops[i].linkname);
        free(ops[i].qname);
    }
    free(ops);
    free(o.operands);

    int werr;
    if (liszt_emit_finish(&werr) < 0) {
        liszt_error(werr, "write error");
        return LISZT_STATUS_SERIOUS;
    }
    return liszt_exit_status();
}

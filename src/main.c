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

#include "colors.h"
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

/* --dired accounting: offsets count every emitted byte except escape
   sequences (color, later hyperlink), exactly GNU's dired_pos, whose
   wrappers skip put_indicator output. Pairs bracket long-format names
   (dired) and header names (subdired). */
static bool dired_on;
static off_t dired_escapes_extra;   /* non-color uncounted bytes */

static off_t
dired_pos_now(void)
{
    return liszt_emit_total() - liszt_color_bytes() - dired_escapes_extra;
}

struct dired_pairs {
    off_t *v;
    size_t n, cap;
};
static struct dired_pairs dired_names;
static struct dired_pairs dired_subdirs;

static void
dired_push(struct dired_pairs *p)
{
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 64;
        p->v = liszt_xrealloc(p->v, p->cap * sizeof *p->v);
    }
    p->v[p->n++] = dired_pos_now();
}

static void
dired_dump(const char *prefix, const struct dired_pairs *p)
{
    if (p->n == 0)
        return;
    char buf[32];
    liszt_emit_str(prefix);
    for (size_t i = 0; i < p->n; i++) {
        int k = snprintf(buf, sizeof buf, " %jd", (intmax_t)p->v[i]);
        liszt_emit_bytes(buf, (size_t)k);
    }
    liszt_emit_byte('\n');
}

/* GNU quoteaf/quotef: shell-escape-always vs shell-escape rendering for
   diagnostics. Static rotating buffers, two slots. */
static const char *
quote_style(const char *name, enum liszt_qstyle style)
{
    static char *slots[2];
    static size_t caps[2];
    static int turn;
    struct liszt_qopts opts = { .style = style };

    turn = 1 - turn;
    size_t need = liszt_quotearg_buffer(slots[turn], caps[turn], name,
                                        (size_t)-1, &opts);
    if (need >= caps[turn]) {
        caps[turn] = need + 1;
        slots[turn] = liszt_xrealloc(slots[turn], caps[turn]);
        liszt_quotearg_buffer(slots[turn], caps[turn], name, (size_t)-1,
                              &opts);
    }
    return slots[turn];
}

static const char *
quote_af(const char *name)
{
    return quote_style(name, LISZT_QS_SHELL_ESCAPE_ALWAYS);
}

static const char *
quote_f(const char *name)
{
    return quote_style(name, LISZT_QS_SHELL_ESCAPE);
}

/* GNU file_escape: RFC3986 unreserved set (alnum + ~-._) passes, path
   mode keeps slashes, everything else lowercase %xx. */
static char *
file_escape(const char *str, bool path)
{
    size_t n = strlen(str);
    char *esc = liszt_xmalloc(3 * n + 1);
    char *p = esc;

    for (; *str; str++) {
        unsigned char b = (unsigned char)*str;
        if (path && b == '/')
            *p++ = '/';
        else if ((b >= '0' && b <= '9') || (b >= 'A' && b <= 'Z')
                 || (b >= 'a' && b <= 'z') || b == '~' || b == '-'
                 || b == '.' || b == '_')
            *p++ = (char)b;
        else
            p += sprintf(p, "%%%02x", b);
    }
    *p = '\0';
    return esc;
}

/* OSC 8 open/close around a name; escape bytes never reach dired
   offsets (dired disables hyperlink, but count them uncounted anyway). */
static void
hyperlink_open(const char *absolute_name)
{
    char *h = file_escape(liszt_hostname(), false);
    char *n = file_escape(absolute_name, true);
    off_t before = liszt_emit_total();

    liszt_emit_str("\033]8;;file://");
    liszt_emit_str(h);
    if (n[0] != '/')
        liszt_emit_byte('/');
    liszt_emit_str(n);
    liszt_emit_str("\033\\");
    dired_escapes_extra += liszt_emit_total() - before;
    free(h);
    free(n);
}

static void
hyperlink_close(void)
{
    off_t before = liszt_emit_total();

    liszt_emit_str("\033]8;;\033\\");
    dired_escapes_extra += liszt_emit_total() - before;
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
    const char *absolute_name;  /* --hyperlink canonical path or NULL */
    mode_t linkmode;
    enum liszt_ftype ftype;
    unsigned char stat_ok;
    unsigned char acl;          /* 0 none, 1 '.', 2 '+' */
    unsigned char quoted;
    unsigned char padded;
    unsigned char linkok;
    unsigned char has_capability;
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

/* GNU file_or_link_mode: the mode coloring reads. */
static mode_t
file_or_link_mode(const struct litem *it)
{
    return (liszt_color_symlink_as_referent() && it->linkok)
        ? it->linkmode : it->st->mode;
}

/* print_name_with_quoting's emission: pad, color start, display bytes,
   color end, clear-to-EOL on possible wrap. Returns the byte length of
   the name as printed (indicator excluded). For symlink targets the
   display form is quoted on the fly. */
static size_t
emit_name_colored(const struct litem *it, bool symlink_target,
                  size_t start_col)
{
    const char *bytes;
    size_t blen;
    bool padded;

    if (symlink_target) {
        int tw;
        bool tq;
        bytes = liszt_quote_name(it->linkname, &cur_opts->filename_qopts,
                                 cur_opts->qmark_funny_chars, false,
                                 &blen, &tw, &tq);
        padded = false;
    } else {
        bytes = it->qname;
        blen = it->qlen;
        padded = it->padded;
    }

    const struct liszt_binstr *color = NULL;
    bool used_this = false;
    if (cur_opts->print_with_color) {
        struct liszt_colorable c;
        if (symlink_target) {
            c.name = it->linkname;
            c.mode = it->linkmode;
            c.linkok = it->linkok ? 0 : -1;
        } else {
            c.name = it->name;
            c.mode = file_or_link_mode(it);
            c.linkok = it->linkok;
        }
        c.ftype = it->ftype;
        c.stat_ok = it->stat_ok;
        c.has_capability = it->has_capability;
        c.multi_hardlink = it->st->nlink > 1;
        color = liszt_color_for(&c);
        used_this = color || liszt_color_is_colored(LISZT_C_NORM);
    }

    if (padded)
        liszt_emit_byte(' ');
    if (color)
        liszt_color_start(color);
    /* Hyperlink: outer quote outside the OSC 8 escape when alignment
       quoting is on (GNU skip_quotes), so links line up. */
    bool skip_quotes = false;
    if (it->absolute_name) {
        if (cur_opts->align_variable_outer_quotes && cur_some_quoted
            && !padded) {
            skip_quotes = true;
            liszt_emit_byte(bytes[0]);
        }
        hyperlink_open(it->absolute_name);
    }
    if (dired_on && !symlink_target)
        dired_push(&dired_names);
    liszt_emit_bytes(bytes + (skip_quotes ? 1 : 0),
                     blen - (skip_quotes ? 2 : 0));
    if (dired_on && !symlink_target)
        dired_push(&dired_names);
    if (it->absolute_name) {
        hyperlink_close();
        if (skip_quotes)
            liszt_emit_byte(bytes[blen - 1]);
    }
    if (used_this) {
        liszt_color_prep_non_filename();
        if (cur_opts->line_length
            && (start_col / cur_opts->line_length
                != (start_col + blen - 1) / cur_opts->line_length))
            liszt_color_put_ind(LISZT_C_CLR_TO_EOL);
    }
    return blen;
}

/* The type indicator character, GNU get_type_indicator. */
static char
type_indicator_char(bool stat_ok, mode_t mode, enum liszt_ftype ftype,
                    enum liszt_indicator_style style)
{
    char c;

    if (stat_ok ? S_ISREG(mode) : ftype == LISZT_T_REG) {
        if (stat_ok && style == LISZT_IND_CLASSIFY
            && (mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
            c = '*';
        else
            c = 0;
    } else {
        if (stat_ok ? S_ISDIR(mode) : ftype == LISZT_T_DIR)
            c = '/';
        else if (style == LISZT_IND_SLASH)
            c = 0;
        else if (stat_ok ? S_ISLNK(mode) : ftype == LISZT_T_LNK)
            c = '@';
        else if (stat_ok ? S_ISFIFO(mode) : ftype == LISZT_T_FIFO)
            c = '|';
        else if (stat_ok ? S_ISSOCK(mode) : ftype == LISZT_T_SOCK)
            c = '=';
        else
            c = 0;
    }
    return c;
}

/* Inode and blocks prefixes shared by every format (-i, -s); returns
   the byte count for start-column accounting. */
static size_t
emit_frills_count(const struct liszt_options *o, const struct lwidths *w,
                  const struct litem *it)
{
    char buf[64 + LISZT_LONGEST_HUMAN_READABLE];
    char hbuf[LISZT_LONGEST_HUMAN_READABLE + 1];
    bool commas = o->format == LISZT_FMT_COMMAS;
    int iw = commas ? 0 : w->inode;
    int bw = commas ? 0 : w->blocks;
    size_t total = 0;

    if (o->print_inode) {
        int n = it->stat_ok
            ? snprintf(buf, sizeof buf, "%*ju ", iw,
                       (uintmax_t)it->st->ino)
            : snprintf(buf, sizeof buf, "%*s ", iw, "?");
        liszt_emit_bytes(buf, (size_t)n);
        total += (size_t)n;
    }
    if (o->print_block_size) {
        const char *blocks = !it->stat_ok
            ? "?"
            : liszt_human_readable((uintmax_t)it->st->blocks, hbuf,
                                   o->human_output_opts, ST_NBLOCKSIZE,
                                   o->output_block_size);
        int n = snprintf(buf, sizeof buf, "%*s ", bw, blocks);
        liszt_emit_bytes(buf, (size_t)n);
        total += (size_t)n;
    }
    return total;
}

/* GNU format_user_or_group: a name gets gap+1 trailing spaces; numeric
   ids render right-aligned with one trailing space. Returns bytes. */
static size_t
emit_id_field(const char *name, uintmax_t id, int width)
{
    char buf[64];

    if (name) {
        int gap = width - (int)strlen(name);
        int pad = gap > 0 ? gap : 0;
        size_t total = strlen(name) + (size_t)pad + 1;
        liszt_emit_str(name);
        do
            liszt_emit_byte(' ');
        while (pad--);
        return total;
    }
    int n = snprintf(buf, sizeof buf, "%*ju ", width, id);
    liszt_emit_bytes(buf, (size_t)n);
    return (size_t)n;
}

static size_t emit_frills_count(const struct liszt_options *o,
                                const struct lwidths *w,
                                const struct litem *it);

static void
emit_long_entry(const struct liszt_options *o, const struct lwidths *w,
                const struct litem *it)
{
    char modebuf[12];
    char buf[256 + LISZT_LONGEST_HUMAN_READABLE];
    char hbuf[LISZT_LONGEST_HUMAN_READABLE + 1];
    const struct liszt_statinfo *st = it->st;
    size_t prefix_len = 0;

    /* dired_indent: two counted spaces, outside the wrap-check prefix
       (GNU writes them past the buffer that start_col measures). */
    if (dired_on)
        liszt_emit_str("  ");

    prefix_len += emit_frills_count(o, w, it);

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
    prefix_len += (size_t)n;

    if (o->print_owner)
        prefix_len += emit_id_field(!it->stat_ok ? "?"
                      : o->numeric_ids ? NULL : liszt_getuser(st->uid),
                      (uintmax_t)st->uid, w->owner);
    if (o->print_group)
        prefix_len += emit_id_field(!it->stat_ok ? "?"
                      : o->numeric_ids ? NULL : liszt_getgroup(st->gid),
                      (uintmax_t)st->gid, w->group);
    if (o->print_author)
        prefix_len += emit_id_field(!it->stat_ok ? "?"
                      : o->numeric_ids ? NULL : liszt_getuser(st->uid),
                      (uintmax_t)st->uid, w->author);
    /* GNU resets its assembly buffer after flushing the id fields, so
       the wrap-check start column counts only what follows (a p - buf
       artifact print_name_with_quoting inherits; fuzz-pinned). */
    if (o->print_owner || o->print_group || o->print_author)
        prefix_len = 0;

    if (it->stat_ok && (S_ISCHR(st->mode) || S_ISBLK(st->mode))) {
        int blanks = w->size - (w->major + 2 + w->minor);
        n = snprintf(buf, sizeof buf, "%*ju, %*ju ",
                     w->major + (blanks > 0 ? blanks : 0),
                     (uintmax_t)major(st->rdev), w->minor,
                     (uintmax_t)minor(st->rdev));
        liszt_emit_bytes(buf, (size_t)n);
        prefix_len += (size_t)n;
    } else {
        const char *size = !it->stat_ok
            ? "?"
            : liszt_human_readable((uintmax_t)st->size, hbuf,
                                   o->file_human_output_opts, 1,
                                   o->file_output_block_size);
        n = snprintf(buf, sizeof buf, "%*s ", w->size, size);
        liszt_emit_bytes(buf, (size_t)n);
        prefix_len += (size_t)n;
    }

    /* GNU's btime_ok: birth time selected but the fs has none - the
       (-1,-1) sentinel renders as "?" through the fallback lane. */
    bool btime_ok = !(o->time_type == LISZT_TIME_BTIME
                      && st->time.tv_sec == -1 && st->time.tv_nsec == -1);

    char tbuf[LISZT_TIME_BUFSZ];
    size_t tlen = it->stat_ok && btime_ok
        ? liszt_timefmt_render(tbuf, st->time) : (size_t)-1;
    if (tlen != (size_t)-1) {
        /* Zero-length renders (empty +FORMAT, overflow) still emit the
           column space - GNU's s stays >= 0 for them. */
        liszt_emit_bytes(tbuf, tlen);
        liszt_emit_byte(' ');
        prefix_len += tlen + 1;
    } else {
        char sbuf[32];
        if (!it->stat_ok || !btime_ok) {
            n = snprintf(buf, sizeof buf, "%*s ",
                         liszt_timefmt_expected_width(), "?");
        } else {
            snprintf(sbuf, sizeof sbuf, "%jd",
                     (intmax_t)st->time.tv_sec);
            n = snprintf(buf, sizeof buf, "%*s ",
                         liszt_timefmt_expected_width(), sbuf);
        }
        liszt_emit_bytes(buf, (size_t)n);
        prefix_len += (size_t)n;
    }

    size_t nlen = emit_name_colored(it, false, prefix_len);
    /* GNU branches on filetype==symbolic_link FIRST: a symlink with no
       readable target prints neither arrow nor indicator (fuzz-pinned
       via failed -L stats). */
    if (it->ftype == LISZT_T_LNK) {
        if (it->linkname) {
            liszt_emit_str(" -> ");
            emit_name_colored(it, true, prefix_len + nlen + 4);
            if (cur_opts->indicator_style != LISZT_IND_NONE) {
                char ic = type_indicator_char(true, it->linkmode,
                                              LISZT_T_UNKNOWN,
                                              cur_opts->indicator_style);
                if (ic)
                    liszt_emit_byte(ic);
            }
        }
    } else if (cur_opts->indicator_style != LISZT_IND_NONE) {
        char ic = type_indicator_char(it->stat_ok, it->st->mode,
                                      it->ftype,
                                      cur_opts->indicator_style);
        if (ic)
            liszt_emit_byte(ic);
    }
    liszt_emit_byte(o->eolbyte);
}

/* print_file_name_and_frills: normal color, frills, name, indicator. */
static void
emit_short_item(const struct liszt_options *o, const struct lwidths *w,
                const struct litem *it, size_t start_col)
{
    if (o->print_with_color)
        liszt_color_set_normal();
    size_t fr = emit_frills_count(o, w, it);
    emit_name_colored(it, false, start_col + fr);
    if (o->indicator_style != LISZT_IND_NONE) {
        char ic = type_indicator_char(it->stat_ok, it->st->mode,
                                      it->ftype, o->indicator_style);
        if (ic)
            liszt_emit_byte(ic);
    }
}

static void
emit_item(const struct liszt_options *o, const struct lwidths *w,
          const struct litem *it)
{
    if (o->format == LISZT_FMT_LONG) {
        if (o->print_with_color)
            liszt_color_set_normal();
        emit_long_entry(o, w, it);
    } else {
        emit_short_item(o, w, it, 0);
        liszt_emit_byte(o->eolbyte);
    }
}

/* --- command-line operands ------------------------------------------- */

struct operand {
    const char *name;
    char *absolute_name;        /* --hyperlink canonical path or NULL */
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
    unsigned char linkok;
    unsigned char has_capability;
    bool is_dir;
};

static void
operand_item(const void *p, struct liszt_item *out)
{
    const struct operand *op = p;
    out->name = op->name;
    out->size = op->st.size;
    out->time = op->st.time;
    out->width = op->disp_width + op->padded;
    /* Under -d, dir operands stay in the batch and GNU's dirs-first
       prefix groups them (fuzz-pinned); with extraction active the dirs
       leave the batch anyway, so the truthful value is always right. */
    out->group_dir = S_ISDIR(op->st.mode) || S_ISDIR(op->linkmode);
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
    it->absolute_name = op->absolute_name;
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
    it->linkok = op->linkok;
    it->has_capability = op->has_capability;
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
    if (o->print_hyperlink) {
        out->absolute_name = liszt_canonicalize_missing(name);
        if (!out->absolute_name)
            file_failure(true, "error canonicalizing %s", name, errno);
    }
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
    if (out->ftype == LISZT_T_LNK
        && (plan.needs_link_target || plan.check_symlink_mode)) {
        out->linkname = liszt_readlink_join("", name);
        if (!out->linkname)
            file_failure(false, "cannot read symbolic link %s",
                         name, errno);
        if (out->linkname && plan.link_target_mode) {
            struct liszt_statinfo ti;
            if (liszt_stat_path(name, &ti) == 0) {
                out->linkmode = ti.mode;
                out->linkok = 1;
            }
        }
    }
    if (plan.cap_probe && out->ftype == LISZT_T_REG)
        out->has_capability =
            liszt_xattr_list_has("", name, "security.capability");
    if (plan.needs_xattr)
        out->acl = probe_acl("", name, out->is_dir);
    return true;
}

/* --- recursion queue and loop detection ------------------------------- */

/* GNU's pending_dirs LIFO. Marker entries (name == NULL) pop the
   active-directory set when a directory's subtree is done, keeping the
   set ancestors-only. */
struct pending {
    char *name;         /* NULL = marker */
    bool command_line;
    struct pending *next;
};

static struct pending *pending_dirs;

static void
queue_directory(const char *name, bool command_line)
{
    struct pending *p = liszt_xmalloc(sizeof *p);

    p->name = name ? liszt_xstrdup(name) : NULL;
    p->command_line = command_line;
    p->next = pending_dirs;
    pending_dirs = p;
}

/* Active (dev,ino) set: linear array - depth-bounded (ancestors only),
   and lookups walk the whole visited set as GNU's hash does. */
struct dev_ino {
    dev_t dev;
    ino_t ino;
};

static struct dev_ino *active_dirs;
static size_t active_len;
static size_t active_cap;

static bool
visit_dir(dev_t dev, ino_t ino)
{
    for (size_t i = 0; i < active_len; i++)
        if (active_dirs[i].dev == dev && active_dirs[i].ino == ino)
            return true;
    if (active_len == active_cap) {
        active_cap = active_cap ? active_cap * 2 : 32;
        active_dirs = liszt_xrealloc(active_dirs,
                                     active_cap * sizeof *active_dirs);
    }
    active_dirs[active_len].dev = dev;
    active_dirs[active_len].ino = ino;
    active_len++;
    return false;
}

static void
pop_active_dir(void)
{
    if (active_len > 0)
        active_len--;
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

/* The plan's per-entry fetch pass, mirroring gobble_file's check_stat
   terms: stat only what the format, sort, grouping, color scheme, or
   indicator style can observe. */
static void
fill_meta(const char *dirname, const struct liszt_options *o,
          struct liszt_entries *es)
{
    bool group = o->group_directories_first && o->sort != LISZT_SORT_NONE;
    bool any = plan.needs_stat || group || plan.stat_dirs_for_color
        || plan.stat_exec || plan.stat_links || plan.needs_link_target
        || plan.needs_xattr || plan.cap_probe || o->print_hyperlink;

    if (!any)
        return;
    liszt_entries_ensure_meta(es);

    for (size_t i = 0; i < es->len; i++) {
        struct liszt_entry *e = &es->v[i];
        struct liszt_entrymeta *m = &es->meta[e->meta_idx];
        const char *nm = liszt_entry_name(es, e);
        enum liszt_ftype t = e->ftype;

        /* GNU gobble_file canonicalizes before the stat, per file. */
        if (o->print_hyperlink) {
            char *abs =
                liszt_canonicalize_missing(liszt_join_path(dirname, nm));
            if (abs) {
                m->abs_off = liszt_entries_add_bytes(es, abs, strlen(abs));
                free(abs);
                nm = liszt_entry_name(es, e);   /* arena may have moved */
            } else {
                file_failure(false, "error canonicalizing %s",
                             liszt_join_path(dirname, nm), errno);
            }
        }

        bool check_stat = plan.needs_stat
            || (group && t == LISZT_T_UNKNOWN)
            || ((t == LISZT_T_DIR || t == LISZT_T_UNKNOWN)
                && plan.stat_dirs_for_color)
            || ((t == LISZT_T_LNK || t == LISZT_T_UNKNOWN)
                && plan.stat_links)
            || ((t == LISZT_T_REG || t == LISZT_T_UNKNOWN)
                && plan.stat_exec);

        if (check_stat) {
            bool follow = o->deref == LISZT_DEREF_ALWAYS;
            /* Any stat implies MODE: conditional fetches (symlink
               resolution under -L/-R, exec bits, dir color bits) exist
               to read it, and the ftype re-derivation depends on it. */
            if (liszt_statx_join(dirname, nm,
                                 plan.stat_wants | LISZT_WANT_MODE,
                                 follow, &m->st) == 0) {
                m->stat_ok = 1;
                e->ftype = (uint8_t)ftype_from_mode(m->st.mode);
            } else {
                file_failure(false, "cannot access %s",
                             liszt_join_path(dirname, nm), errno);
                continue;
            }
        }
        if (e->ftype == LISZT_T_LNK
            && (plan.needs_link_target || group)) {
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
            if ((m->link_off != UINT32_MAX
                 && (plan.link_target_mode || group))
                || (group && m->link_off == UINT32_MAX)) {
                struct liszt_statinfo ti;
                if (liszt_stat_join(dirname, nm, &ti) == 0) {
                    m->linkmode = ti.mode;
                    m->linkok = 1;
                }
            }
        }
        if (plan.cap_probe
            && (e->ftype == LISZT_T_REG || e->ftype == LISZT_T_UNKNOWN))
            m->has_capability =
                liszt_xattr_list_has(dirname, nm, "security.capability");
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
    it->absolute_name = (m && m->abs_off != UINT32_MAX)
        ? (const char *)es->arena + m->abs_off
        : NULL;
    it->linkmode = m ? m->linkmode : 0;
    it->ftype = e->ftype;
    it->stat_ok = m ? m->stat_ok : 0;
    it->acl = m ? m->acl : 0;
    it->linkok = m ? m->linkok : 0;
    it->has_capability = m ? m->has_capability : 0;
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
    emit_short_item(c->o, c->w, &c->items[idx], start_col);
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
    if (o->indicator_style != LISZT_IND_NONE
        && type_indicator_char(it->stat_ok, it->st->mode, it->ftype,
                               o->indicator_style))
        len += 1;
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
        if (dired_on)
            liszt_emit_str("  ");
        liszt_emit_str("total ");
        liszt_emit_str(liszt_human_readable(total, hbuf,
                                            o->human_output_opts,
                                            ST_NBLOCKSIZE,
                                            o->output_block_size));
        liszt_emit_byte(o->eolbyte);
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
            liszt_layout_separated(n, lengths, ' ', 0, o->eolbyte,
                                   layout_emit_cb,
                                   &ctx);
            free(lengths);
            break;
        }
        size_t *lengths = liszt_xmalloc(n * sizeof *lengths);
        for (size_t i = 0; i < n; i++)
            lengths[i] = item_length(o, &w, &items[i]);
        liszt_layout_columns(n, lengths, o->format == LISZT_FMT_MANY,
                             o->line_length, o->max_idx, o->tabsize,
                             o->eolbyte, layout_emit_cb, &ctx);
        free(lengths);
        break;
    }
    case LISZT_FMT_COMMAS: {
        struct batch_ctx ctx = { o, items, &w };
        size_t *lengths = liszt_xmalloc(n * sizeof *lengths);
        for (size_t i = 0; i < n; i++)
            lengths[i] = item_length(o, &w, &items[i]);
        liszt_layout_separated(n, lengths, ',', o->line_length,
                               o->eolbyte, layout_emit_cb, &ctx);
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

/* GNU print_dir: loop detection before reading, header when recursive
   or print_dir_name (blank line before every header but the first),
   subdirectory extraction in sorted order after sorting. */
static void
print_dir(const char *name, bool command_line, bool print_dir_name,
          bool *first, const struct liszt_options *o,
          struct liszt_entries *es)
{
    struct dir_diag_ctx dc = { name, command_line };
    struct liszt_dir *dh;

    /* GNU's ordering: opendir failure diagnoses first; loop detection
       runs on the open handle; only then are entries read. */
    if (liszt_diropen(name, &dh) < 0) {
        file_failure(command_line, "cannot open directory %s", name,
                     errno);
        return;
    }
    if (o->recursive) {
        struct liszt_statinfo di;
        if (liszt_fstat(liszt_dirfd(dh), &di) < 0) {
            file_failure(command_line,
                         "cannot determine device and inode of %s", name,
                         errno);
            liszt_dirclose(dh);
            return;
        }
        if (visit_dir(di.dev, di.ino)) {
            fprintf(stderr, "%s: %s: not listing already-listed"
                    " directory\n", liszt_prog, quote_f(name));
            liszt_dirclose(dh);
            liszt_set_exit_status(true);
            return;
        }
    }
    struct liszt_ignore_spec ig = {
        .mode = o->ignore,
        .hide = o->hide_patterns,
        .n_hide = o->n_hide_patterns,
        .ignore = o->ignore_patterns,
        .n_ignore = o->n_ignore_patterns
    };
    liszt_dirread_collect_from(dh, &ig, es, on_dirread_fail, &dc);

    fill_meta(name, o, es);
    decorate_entries(o, es);
    liszt_sort_entries(es);

    if (o->recursive || print_dir_name) {
        if (!*first)
            liszt_emit_byte('\n');
        *first = false;
        size_t hlen;
        int hwidth;
        bool hquoted;
        if (dired_on)
            liszt_emit_str("  ");
        char *habs = NULL;
        if (o->print_hyperlink) {
            habs = liszt_canonicalize_missing(name);
            if (!habs)
                file_failure(command_line, "error canonicalizing %s",
                             name, errno);
        }
        const char *hq = liszt_quote_name(name, &o->dirname_qopts,
                                          o->qmark_funny_chars, false,
                                          &hlen, &hwidth, &hquoted);
        if (habs)
            hyperlink_open(habs);
        if (dired_on)
            dired_push(&dired_subdirs);
        liszt_emit_bytes(hq, hlen);
        if (dired_on)
            dired_push(&dired_subdirs);
        if (habs) {
            hyperlink_close();
            free(habs);
        }
        liszt_emit_str(":\n");
    }

    if (o->recursive) {
        /* Marker first, then subdirs in reverse sorted order: the LIFO
           pops them forward, depth-first (GNU extract_dirs_from_files). */
        queue_directory(NULL, false);
        for (size_t i = es->len; i > 0; i--) {
            const struct liszt_entry *e = &es->v[i - 1];
            if (e->ftype != LISZT_T_DIR)
                continue;
            const char *en = liszt_entry_name(es, e);
            if (strcmp(en, ".") == 0 || strcmp(en, "..") == 0)
                continue;
            queue_directory(liszt_join_path(name, en), false);
        }
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
    liszt_xstat_time_type(o.time_type);
    dired_on = o.dired;

    if (o.print_with_color) {
        liszt_colors_parse(&o.print_with_color);
        /* Color forces spaces-only padding (GNU main 1685). */
        if (o.print_with_color)
            o.tabsize = 0;
    }

    liszt_plan_select(&o, &plan);
    liszt_plan_color_update(&o, &plan);
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
                                       o.line_length, o.eolbyte,
                                       layout_emit_cb, &ctx);
            else if (!o.line_length)
                liszt_layout_separated((size_t)nf, lengths, ' ', 0,
                                       o.eolbyte, layout_emit_cb, &ctx);
            else
                liszt_layout_columns((size_t)nf, lengths,
                                     o.format == LISZT_FMT_MANY,
                                     o.line_length, o.max_idx, o.tabsize,
                                     o.eolbyte, layout_emit_cb, &ctx);
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

    /* Seed the pending queue with command-line directories in reverse
       (the LIFO pops them forward), then drain depth-first; markers pop
       the active-ancestor set. */
    if (implicit_dot) {
        queue_directory(".", true);
    } else {
        for (int i = n_ops; i > 0; i--)
            if (ops[i - 1].is_dir)
                queue_directory(ops[i - 1].name, true);
    }
    while (pending_dirs) {
        struct pending *p = pending_dirs;
        pending_dirs = p->next;
        if (p->name == NULL) {
            pop_active_dir();
        } else {
            print_dir(p->name, p->command_line, print_dir_name, &first,
                      &o, &es);
        }
        free(p->name);
        free(p);
    }

    liszt_entries_free(&es);
    for (int i = 0; i < n_ops; i++) {
        free(ops[i].linkname);
        free(ops[i].qname);
    }
    free(ops);
    free(o.operands);
    free(o.hide_patterns);
    free(o.ignore_patterns);

    if (o.print_with_color && liszt_color_used()
        && !liszt_color_restore_is_noop())
        liszt_color_restore_default();

    if (dired_on) {
        dired_dump("//DIRED//", &dired_names);
        dired_dump("//SUBDIRED//", &dired_subdirs);
        liszt_emit_str("//DIRED-OPTIONS// --quoting-style=");
        liszt_emit_str(liszt_quoting_style_word(o.quoting_style));
        liszt_emit_byte('\n');
    }

    int werr;
    if (liszt_emit_finish(&werr) < 0) {
        liszt_error(werr, "write error");
        return LISZT_STATUS_SERIOUS;
    }
    if (getenv("LISZT_DEBUG_STATS") != NULL)
        liszt_timefmt_stats();
    return liszt_exit_status();
}

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include "colors.h"
#include "config.h"
#include "dirread.h"
#include "emit.h"
#include "entry.h"
#include "human.h"
#include "git.h"
#include "gitignore.h"
#include "icons.h"
#include "idcache.h"
#include "layout.h"
#include "options.h"
#include "plan.h"
#include "quote.h"
#include "sortkey.h"
#include "sys/thread.h"
#include "sys/xstat.h"
#include "timefmt.h"
#include "util.h"

#define ST_NBLOCKSIZE 512

static struct liszt_plan plan;
static const struct liszt_options *cur_opts;
static bool cur_some_quoted;

/* Tree-mode branch column, published by the walker and emitted
   immediately before the name in both long and short entries. Empty in
   GNU mode and on tree root lines - a no-op there. */
static struct {
    const char *bytes;
    size_t len;                 /* 0 = no column */
    size_t width;               /* display cells */
} tree_prefix;

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
            p += snprintf(p, 4, "%%%02x", b);
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

/* --- --color=full metadata theme (v0.3 extension) ---------------------

   eza's default theme (default_theme.rs, nu-ansi-term -> SGR), byte
   content untouched: only SGR wrapping is added, padding stays outside
   every color region, and everything rides the color funnel so dired
   accounting holds. Stripping SGR must reproduce --color=always bytes
   exactly (the fuzz oracle). */

#define CF_STYLE(name, seq) \
    static const struct liszt_binstr name = { sizeof seq - 1, seq }
CF_STYLE(cf_punct, "1;90");
CF_STYLE(cf_ur, "1;33");
CF_STYLE(cf_uw, "1;31");
CF_STYLE(cf_ux_file, "1;4;32");
CF_STYLE(cf_ux, "1;32");
CF_STYLE(cf_gr, "33");
CF_STYLE(cf_gw, "31");
CF_STYLE(cf_gx, "32");
CF_STYLE(cf_special, "35");
CF_STYLE(cf_dir, "1;34");
CF_STYLE(cf_lnk, "36");
CF_STYLE(cf_fifo, "33");
CF_STYLE(cf_dev, "1;33");
CF_STYLE(cf_sock, "1;31");
CF_STYLE(cf_nlink, "1;31");
CF_STYLE(cf_mhlink, "31;43");
CF_STYLE(cf_yours, "1;33");
CF_STYLE(cf_sz_b, "32");
CF_STYLE(cf_sz_k, "1;32");
CF_STYLE(cf_sz_m, "33");
CF_STYLE(cf_sz_g, "31");
CF_STYLE(cf_sz_h, "35");
CF_STYLE(cf_major, "1;32");
CF_STYLE(cf_minor, "32");
CF_STYLE(cf_date, "34");
CF_STYLE(cf_inode, "35");
CF_STYLE(cf_blocks, "36");
CF_STYLE(cf_dim, "2");
CF_STYLE(cf_se_user, "34");
CF_STYLE(cf_se_role, "32");
CF_STYLE(cf_se_type, "33");
CF_STYLE(cf_se_range, "36");

static uid_t cf_uid;
static gid_t cf_gids[64];
static int cf_n_gids = -1;

static void
cf_init_identity(void)
{
    cf_uid = getuid();
    cf_n_gids = getgroups(64, cf_gids);
    if (cf_n_gids < 0)
        cf_n_gids = 0;
}

static bool
cf_my_group(gid_t g)
{
    if (g == getgid())
        return true;
    for (int i = 0; i < cf_n_gids; i++)
        if (cf_gids[i] == g)
            return true;
    return false;
}

/* One styled token through the funnel; NULL style = plain bytes. */
static void
cf_put(const struct liszt_binstr *sty, const char *bytes, size_t len)
{
    if (sty == NULL) {
        liszt_emit_bytes(bytes, len);
        return;
    }
    liszt_color_start(sty);
    liszt_emit_bytes(bytes, len);
    liszt_color_prep_non_filename();
}

static const struct liszt_binstr *
cf_mode_char_style(size_t idx, char ch, bool reg)
{
    if (ch == '-')
        return idx == 0 ? NULL : &cf_punct;
    if (idx == 0)
        switch (ch) {
        case 'd': return &cf_dir;
        case 'l': return &cf_lnk;
        case 'p': return &cf_fifo;
        case 'b':
        case 'c': return &cf_dev;
        case 's': return &cf_sock;
        default: return NULL;
        }
    switch (ch) {
    case 'r': return idx <= 3 ? &cf_ur : idx <= 6 ? &cf_gr : &cf_gr;
    case 'w': return idx <= 3 ? &cf_uw : &cf_gw;
    case 'x': return idx <= 3 ? (reg ? &cf_ux_file : &cf_ux) : &cf_gx;
    case 's':
    case 'S':
    case 't':
    case 'T': return &cf_special;
    default: return NULL;    /* '?' rows, acl suffix */
    }
}

static size_t
cf_emit_mode(const char *modebuf, bool reg)
{
    size_t n = strlen(modebuf);
    for (size_t i = 0; i < n; i++)
        cf_put(cf_mode_char_style(i, modebuf[i], reg), modebuf + i, 1);
    return n;
}

static const struct liszt_binstr *
cf_size_style(uintmax_t v)
{
    if (v < 1000u) return &cf_sz_b;
    if (v < 1000000u) return &cf_sz_k;
    if (v < 1000000000u) return &cf_sz_m;
    if (v < 1000000000000u) return &cf_sz_g;
    return &cf_sz_h;
}

/* Right-aligned styled token: pad plain, token colored. Returns the
   visible length including the trailing space. */
static size_t
cf_emit_field(const struct liszt_binstr *sty, int width,
              const char *token)
{
    size_t tlen = strlen(token);
    int pad = width - (int)tlen;
    for (int i = 0; i < pad; i++)
        liszt_emit_byte(' ');
    cf_put(sty, token, tlen);
    liszt_emit_byte(' ');
    return (size_t)(pad > 0 ? pad : 0) + tlen + 1;
}

/* SELinux context split: user:role:type:range with dimmed colons;
   contextless "?" stays plain. */
static size_t
cf_emit_scontext(const char *ctx, int width)
{
    static const struct liszt_binstr *const part_sty[4] = {
        &cf_se_user, &cf_se_role, &cf_se_type, &cf_se_range
    };
    size_t total = strlen(ctx);
    if (strchr(ctx, ':') == NULL)
        return cf_emit_field(NULL, width, ctx);
    int pad = width - (int)total;
    for (int i = 0; i < pad; i++)
        liszt_emit_byte(' ');
    size_t part = 0;
    const char *p = ctx;
    while (*p != '\0') {
        size_t seg = strcspn(p, ":");
        cf_put(part_sty[part < 3 ? part : 3], p, seg);
        p += seg;
        if (*p == ':') {
            cf_put(&cf_dim, p, 1);
            p++;
        }
        part++;
    }
    liszt_emit_byte(' ');
    return (size_t)(pad > 0 ? pad : 0) + total + 1;
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
    int scontext;
    bool any_acl;
};

/* The renderer's view of one listable item; both directory entries and
   command-line operands compile into this. */
struct litem {
    const char *name;
    size_t name_len;
    const char *qname;          /* display form (may equal name) */
    size_t qlen;
    int width;                  /* display width, no pad */
    const struct liszt_statinfo *st;
    const char *linkname;       /* NULL = none */
    const char *absolute_name;  /* --hyperlink canonical path or NULL */
    const char *scontext;       /* -Z context, "?" when absent */
    mode_t linkmode;
    enum liszt_ftype ftype;
    unsigned char stat_ok;
    unsigned char acl;          /* 0 none, 1 '.', 2 '+' */
    unsigned char quoted;
    unsigned char padded;
    unsigned char linkok;
    unsigned char has_capability;
    unsigned char git_status;   /* 0 none, ' ' blank, else letter */
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
    if (o->print_scontext) {
        int slen = (int)strlen(it->scontext);
        if (w->scontext < slen)
            w->scontext = slen;
    }
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
    bool would_pad;

    if (symlink_target) {
        int tw;
        bool tq;
        bytes = liszt_quote_name(it->linkname, &cur_opts->filename_qopts,
                                 cur_opts->qmark_funny_chars, false,
                                 &blen, &tw, &tq);
        /* Targets never emit the alignment pad, but GNU's skip_quotes
           decision still uses the would-pad value from the target's
           own quotedness. */
        padded = false;
        would_pad = cur_opts->align_variable_outer_quotes
            && cur_some_quoted && !tq;
    } else {
        bytes = it->qname;
        blen = it->qlen;
        padded = it->padded;
        would_pad = padded;
    }

    const struct liszt_binstr *color = NULL;
    bool used_this = false;
    bool want_icon = cur_opts->print_icons && !symlink_target;
    struct liszt_colorable cinfo;
    if (cur_opts->print_with_color || want_icon) {
        if (symlink_target) {
            cinfo.name = it->linkname;
            cinfo.mode = it->linkmode;
            cinfo.linkok = it->linkok ? 0 : -1;
        } else {
            cinfo.name = it->name;
            cinfo.mode = file_or_link_mode(it);
            cinfo.linkok = it->linkok;
        }
        cinfo.ftype = it->ftype;
        cinfo.stat_ok = it->stat_ok;
        cinfo.has_capability = it->has_capability;
        cinfo.multi_hardlink = it->st->nlink > 1;
    }
    if (cur_opts->print_with_color) {
        color = liszt_color_for(&cinfo);
        /* Full theme: eza's filename classes catch what LS_COLORS
           left plain, regular files only, never symlink targets. */
        if (color == NULL && cur_opts->color_full && !symlink_target
            && liszt_file_class(&cinfo) == LISZT_C_FILE)
            color = liszt_colorclass_for(it->name, it->name_len);
        used_this = color || liszt_color_is_colored(LISZT_C_NORM);
    }

    /* Icon prefix (v0.2): own color region so background schemes never
       bleed into the spacing; outside the OSC 8 hyperlink; never on
       symlink targets. A blank resolution keeps the cell with spaces. */
    size_t icon_cells = 0;
    if (want_icon) {
        struct liszt_icon ic;
        unsigned sp = liszt_icon_spacing();

        liszt_icon_for(&cinfo, it->name_len, &ic);
        icon_cells = 1 + sp;
        if (ic.len == 0) {
            liszt_emit_bytes("         ", 1 + sp);   /* sp <= 8 */
        } else if (color == NULL && !liszt_icon_osc66()) {
            char ibuf[16];
            memcpy(ibuf, ic.bytes, ic.len);
            memset(ibuf + ic.len, ' ', sp);
            liszt_emit_bytes(ibuf, ic.len + sp);
        } else {
            if (color)
                liszt_color_start(color);
            if (liszt_icon_osc66())
                liszt_emit_str("\033]66;w=1;");
            liszt_emit_bytes(ic.bytes, ic.len);
            if (liszt_icon_osc66())
                liszt_emit_str("\033\\");
            if (color)
                liszt_color_prep_non_filename();
            liszt_emit_bytes("        ", sp);
        }
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
            && !would_pad) {
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
        /* GNU's wrap check uses quote_name's return, which includes
           the alignment pad byte (fuzz-pinned, seed 1337). Icon cells
           join it under --icons. */
        size_t wlen = icon_cells + blen + (padded ? 1u : 0u);
        if (cur_opts->line_length
            && (start_col / cur_opts->line_length
                != (start_col + wlen - 1) / cur_opts->line_length))
            liszt_color_put_ind(LISZT_C_CLR_TO_EOL);
    }
    return icon_cells + blen;
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
        if (o->color_full) {
            char nb[32];
            if (it->stat_ok)
                snprintf(nb, sizeof nb, "%ju", (uintmax_t)it->st->ino);
            else
                snprintf(nb, sizeof nb, "?");
            total += cf_emit_field(it->stat_ok ? &cf_inode : NULL, iw,
                                   nb);
        } else {
        int n = it->stat_ok
            ? snprintf(buf, sizeof buf, "%*ju ", iw,
                       (uintmax_t)it->st->ino)
            : snprintf(buf, sizeof buf, "%*s ", iw, "?");
        liszt_emit_bytes(buf, (size_t)n);
        total += (size_t)n;
        }
    }
    if (o->print_block_size) {
        const char *blocks = !it->stat_ok
            ? "?"
            : liszt_human_readable((uintmax_t)it->st->blocks, hbuf,
                                   o->human_output_opts, ST_NBLOCKSIZE,
                                   o->output_block_size);
        if (o->color_full) {
            total += cf_emit_field(it->stat_ok ? &cf_blocks : NULL,
                                   bw, blocks);
        } else {
        int n = snprintf(buf, sizeof buf, "%*s ", bw, blocks);
        liszt_emit_bytes(buf, (size_t)n);
        total += (size_t)n;
        }
    }
    return total;
}

/* GNU format_user_or_group: a name gets gap+1 trailing spaces; numeric
   ids render right-aligned with one trailing space. Returns visible
   bytes; STY (may be NULL) colors the value, never the padding. */
static size_t
emit_id_field(const char *name, uintmax_t id, int width,
              const struct liszt_binstr *sty)
{
    char buf[64];

    if (name) {
        int gap = width - (int)strlen(name);
        int pad = gap > 0 ? gap : 0;
        size_t total = strlen(name) + (size_t)pad + 1;
        cf_put(sty, name, strlen(name));
        do
            liszt_emit_byte(' ');
        while (pad--);
        return total;
    }
    int n = snprintf(buf, sizeof buf, "%ju", id);
    if (sty == NULL) {
        int r = snprintf(buf, sizeof buf, "%*ju ", width, id);
        liszt_emit_bytes(buf, (size_t)r);
        return (size_t)r;
    }
    (void)n;
    snprintf(buf, sizeof buf, "%ju", id);
    return cf_emit_field(sty, width, buf);
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
    if (o->color_full) {
        bool reg = it->stat_ok ? S_ISREG(st->mode)
                               : it->ftype == LISZT_T_REG;
        prefix_len += cf_emit_mode(modebuf, reg);
        liszt_emit_byte(' ');
        prefix_len += 1;
        if (it->stat_ok) {
            const struct liszt_binstr *ls =
                reg && st->nlink > 1 ? &cf_mhlink : &cf_nlink;
            char nb[32];
            snprintf(nb, sizeof nb, "%ju", (uintmax_t)st->nlink);
            prefix_len += cf_emit_field(ls, w->nlink, nb);
        } else {
            prefix_len += cf_emit_field(NULL, w->nlink, "?");
        }
    } else {
    if (it->stat_ok)
        n = snprintf(buf, sizeof buf, "%s %*ju ", modebuf, w->nlink,
                     (uintmax_t)st->nlink);
    else
        n = snprintf(buf, sizeof buf, "%s %*s ", modebuf, w->nlink, "?");
    liszt_emit_bytes(buf, (size_t)n);
    prefix_len += (size_t)n;
    }

    const struct liszt_binstr *ust = o->color_full && it->stat_ok
        && st->uid == cf_uid ? &cf_yours : NULL;
    const struct liszt_binstr *gst = o->color_full && it->stat_ok
        && cf_my_group(st->gid) ? &cf_yours : NULL;
    if (o->print_owner)
        prefix_len += emit_id_field(!it->stat_ok ? "?"
                      : o->numeric_ids ? NULL : liszt_getuser(st->uid),
                      (uintmax_t)st->uid, w->owner, ust);
    if (o->print_group)
        prefix_len += emit_id_field(!it->stat_ok ? "?"
                      : o->numeric_ids ? NULL : liszt_getgroup(st->gid),
                      (uintmax_t)st->gid, w->group, gst);
    if (o->print_author)
        prefix_len += emit_id_field(!it->stat_ok ? "?"
                      : o->numeric_ids ? NULL : liszt_getuser(st->uid),
                      (uintmax_t)st->uid, w->author, ust);
    if (o->print_scontext)
        prefix_len += o->color_full
            ? cf_emit_scontext(it->scontext, w->scontext)
            : emit_id_field(it->scontext, 0, w->scontext, NULL);
    /* GNU resets its assembly buffer after flushing the id fields, so
       the wrap-check start column counts only what follows (a p - buf
       artifact print_name_with_quoting inherits; fuzz-pinned). */
    if (o->print_owner || o->print_group || o->print_author)
        prefix_len = 0;

    if (it->stat_ok && (S_ISCHR(st->mode) || S_ISBLK(st->mode))) {
        if (o->color_full) {
            int blanks = w->size - (w->major + 2 + w->minor);
            char db[32];
            snprintf(db, sizeof db, "%ju", (uintmax_t)major(st->rdev));
            size_t dl = strlen(db);
            int mw = w->major + (blanks > 0 ? blanks : 0);
            for (int i = 0; i < mw - (int)dl; i++)
                liszt_emit_byte(' ');
            cf_put(&cf_major, db, dl);
            liszt_emit_byte(',');
            liszt_emit_byte(' ');
            prefix_len += (size_t)(mw > (int)dl ? mw - (int)dl : 0)
                + dl + 2;
            snprintf(db, sizeof db, "%ju", (uintmax_t)minor(st->rdev));
            prefix_len += cf_emit_field(&cf_minor, w->minor, db);
        } else {
        int blanks = w->size - (w->major + 2 + w->minor);
        n = snprintf(buf, sizeof buf, "%*ju, %*ju ",
                     w->major + (blanks > 0 ? blanks : 0),
                     (uintmax_t)major(st->rdev), w->minor,
                     (uintmax_t)minor(st->rdev));
        liszt_emit_bytes(buf, (size_t)n);
        prefix_len += (size_t)n;
        }
    } else {
        const char *size = !it->stat_ok
            ? "?"
            : liszt_human_readable((uintmax_t)st->size, hbuf,
                                   o->file_human_output_opts, 1,
                                   o->file_output_block_size);
        if (o->color_full) {
            const struct liszt_binstr *ss = it->stat_ok
                ? cf_size_style((uintmax_t)st->size) : NULL;
            prefix_len += cf_emit_field(ss, w->size, size);
        } else {
        n = snprintf(buf, sizeof buf, "%*s ", w->size, size);
        liszt_emit_bytes(buf, (size_t)n);
        prefix_len += (size_t)n;
        }
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
        if (o->color_full && tlen > 0)
            cf_put(&cf_date, tbuf, tlen);
        else
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

    if (it->git_status) {
        if (it->git_status == ' ') {
            liszt_emit_str("   ");
        } else {
            static const struct liszt_binstr git_green =
                { 2, "32" };
            static const struct liszt_binstr git_blue = { 2, "34" };
            static const struct liszt_binstr git_purple =
                { 2, "35" };
            static const struct liszt_binstr git_red = { 2, "31" };
            char wc = (char)it->git_status;
            const struct liszt_binstr *seq =
                wc == 'N' ? &git_green
                : wc == 'M' ? &git_blue
                : wc == 'T' ? &git_purple
                : wc == 'U' ? &git_red : NULL;
            liszt_emit_byte('-');
            if (o->print_with_color && seq != NULL) {
                liszt_color_start(seq);
                liszt_emit_byte(wc);
                liszt_color_prep_non_filename();
            } else {
                liszt_emit_byte(wc);
            }
            liszt_emit_byte(' ');
        }
        prefix_len += 3;
    }
    if (tree_prefix.len) {
        liszt_emit_bytes(tree_prefix.bytes, tree_prefix.len);
        prefix_len += tree_prefix.width;
    }
    size_t nlen = emit_name_colored(it, false, prefix_len);
    /* GNU branches on filetype==symbolic_link FIRST: a symlink with no
       readable target prints neither arrow nor indicator (fuzz-pinned
       via failed -L stats). */
    if (it->ftype == LISZT_T_LNK) {
        if (it->linkname) {
            if (cur_opts->color_full) {
                liszt_emit_byte(' ');
                cf_put(&cf_punct, "->", 2);
                liszt_emit_byte(' ');
            } else
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
    if (o->print_scontext) {
        if (o->color_full) {
            fr += cf_emit_scontext(it->scontext,
                                   o->format == LISZT_FMT_COMMAS
                                       ? 0 : w->scontext);
        } else {
        char sbuf[64];
        int n = snprintf(sbuf, sizeof sbuf, "%*s ",
                         o->format == LISZT_FMT_COMMAS ? 0 : w->scontext,
                         it->scontext);
        liszt_emit_bytes(sbuf, (size_t)n);
        fr += (size_t)n;
        }
    }
    if (tree_prefix.len) {
        liszt_emit_bytes(tree_prefix.bytes, tree_prefix.len);
        fr += tree_prefix.width;
    }
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
    char *scontext;             /* -Z context; NULL = "?" */
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
    unsigned char git_status;
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
    it->name_len = strlen(op->name);
    it->absolute_name = op->absolute_name;
    it->scontext = op->scontext ? op->scontext : "?";
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
    it->git_status = op->git_status;
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

/* getfilecon shape: security.selinux value or "?"; failures other
   than the unsupported class are diagnosed without touching the exit
   status (GNU error(0,...)). Returns malloc'd context or NULL. */
static char *
fetch_scontext(const char *dir, const char *name, bool follow)
{
    char cbuf[256];
    long cn = liszt_xattr_value_join(dir, name, "security.selinux",
                                     follow, cbuf, sizeof cbuf);
    if (cn > 0) {
        if (cbuf[cn - 1] == '\0')
            cn--;
        if (cn > 0) {
            char *s = liszt_xmalloc((size_t)cn + 1);
            memcpy(s, cbuf, (size_t)cn);
            s[cn] = '\0';
            return s;
        }
        return NULL;
    }
    if (cn < 0 && errno != ENOTSUP && errno != EOPNOTSUPP
#ifdef ENODATA
        && errno != ENODATA     /* Linux's missing-attribute errno */
#endif
#ifdef ENOATTR
        && errno != ENOATTR     /* BSD's missing-attribute errno */
#endif
        )
        fprintf(stderr, "%s: %s: %s\n", liszt_prog,
                quote_f(*dir ? liszt_join_path(dir, name) : name),
                strerror(errno));
    return NULL;
}

/* --- parallel stat phase (09D) ---------------------------------------

   Determinism argument: each task touches only its own entry's meta
   slot and ftype byte (disjoint pre-assigned slots), reads the entry
   arena immutably (no appends until the serial pass), and never emits
   bytes or diagnostics. The ordered serial pass afterwards replays
   failure diagnostics from recorded errnos and performs all
   arena-appending work, so output and stderr are byte-identical to the
   serial path by construction. Path joining is safe because xstat's
   join buffer is thread-local. */

/* --- git status pipeline (v0.2 extension) -----------------------------

   Serial per listed directory: resolve the repo context, build the
   lookup window and the gitignore chain (deepest .gitignore first,
   info/exclude last), precompute whether an ancestor directory is
   itself ignored. The parallel phase then reads all of it immutably
   and writes only its own meta slot. */

struct gi_cache_node {
    char *key;                  /* absolute .gitignore file path */
    struct liszt_gi_file f;
    bool present;
    struct gi_cache_node *next;
};
static struct gi_cache_node *gi_cache;

static struct {
    bool active;                /* flags on and inside a repo */
    bool column;                /* show_git: letters into meta */
    bool blank;                 /* degraded repo: blank cells */
    struct liszt_git_ctx *ctx;
    const struct liszt_gi_file **chain;
    size_t n_chain, chain_cap;
    bool ancestor_ignored;
    char rel[4096];             /* repo-relative listed dir, "" root */
    size_t rel_len;
} gd;

/* Parse (or fetch cached) one ignore file; BASE is its repo-relative
   directory. Returns NULL when the file does not exist. */
static const struct liszt_gi_file *
gi_load(const char *abspath, const char *base, size_t base_len)
{
    for (struct gi_cache_node *n = gi_cache; n != NULL; n = n->next)
        if (strcmp(n->key, abspath) == 0)
            return n->present ? &n->f : NULL;
    struct gi_cache_node *n = liszt_xmalloc(sizeof *n);
    n->key = liszt_xstrdup(abspath);
    n->present = false;
    memset(&n->f, 0, sizeof n->f);
    FILE *fp = fopen(abspath, "r");
    if (fp != NULL) {
        char *buf = NULL;
        size_t cap = 0, len = 0;
        for (;;) {
            if (len == cap) {
                cap = cap ? cap * 2 : 4096;
                buf = liszt_xrealloc(buf, cap);
            }
            size_t r = fread(buf + len, 1, cap - len, fp);
            len += r;
            if (r == 0)
                break;
        }
        fclose(fp);
        liszt_gi_file_parse(&n->f, buf ? buf : "", len, base, base_len);
        free(buf);
        n->present = true;
    }
    n->next = gi_cache;
    gi_cache = n;
    return n->present ? &n->f : NULL;
}

static void
gd_chain_push(const struct liszt_gi_file *f)
{
    if (gd.n_chain == gd.chain_cap) {
        gd.chain_cap = gd.chain_cap ? gd.chain_cap * 2 : 8;
        gd.chain = liszt_xrealloc(gd.chain,
                                  gd.chain_cap * sizeof *gd.chain);
    }
    gd.chain[gd.n_chain++] = f;
}

/* Is repo-relative PATH excluded by the current chain? Ancestors
   already decided win ("cannot re-include inside an excluded dir"). */
static bool
gi_chain_excluded(const struct liszt_gi_file *const *chain, size_t n,
                  const char *path, size_t len, size_t base_off,
                  bool is_dir)
{
    for (size_t i = 0; i < n; i++) {
        int r = liszt_gi_file_match(chain[i], path, len, base_off,
                                    is_dir);
        if (r >= 0)
            return r == 1;
    }
    return false;
}

static void
git_begin_dir(const char *dirname, const struct liszt_options *o)
{
    gd.active = false;
    gd.column = false;
    gd.blank = false;
    gd.ctx = NULL;
    gd.n_chain = 0;
    gd.ancestor_ignored = false;
    gd.rel_len = 0;
    if (!o->show_git && !o->git_ignore)
        return;
    char *abs = liszt_canonicalize_missing(dirname);
    if (abs == NULL)
        return;
    struct liszt_git_ctx *c = liszt_git_ctx_for(abs);
    if (c == NULL) {
        free(abs);
        return;
    }
    gd.active = true;
    gd.column = o->show_git;
    gd.ctx = c;
    if (c->degraded) {
        gd.blank = true;
        free(abs);
        return;
    }
    liszt_git_window(c, abs);

    size_t alen = strlen(abs);
    while (alen > 1 && abs[alen - 1] == '/')
        alen--;
    if (alen > c->root_len && alen - c->root_len - 1 < sizeof gd.rel) {
        gd.rel_len = alen - c->root_len - 1;
        memcpy(gd.rel, abs + c->root_len + 1, gd.rel_len);
    }
    gd.rel[gd.rel_len] = '\0';

    /* Root-to-dir ignore files; the entry chain wants deepest first,
       the ancestor probe wants them in loading order. */
    const struct liszt_gi_file *files[64];
    size_t nf = 0;
    char pathbuf[8192];
    size_t comp = 0;    /* consumed bytes of rel */
    for (;;) {
        size_t dlen = comp == 0 ? 0 : comp;
        int n = snprintf(pathbuf, sizeof pathbuf, "%s%s%.*s/.gitignore",
                         c->root, dlen ? "/" : "", (int)dlen, gd.rel);
        const struct liszt_gi_file *f = NULL;
        if (n > 0 && (size_t)n < sizeof pathbuf && nf < 64)
            f = gi_load(pathbuf, gd.rel, dlen);
        if (f != NULL)
            files[nf++] = f;
        if (comp >= gd.rel_len)
            break;
        /* Next component: is the child directory itself ignored? */
        size_t next = comp;
        if (next > 0)
            next++;             /* skip '/' */
        while (next < gd.rel_len && gd.rel[next] != '/')
            next++;
        if (!gd.ancestor_ignored) {
            /* Deepest-loaded first, then info/exclude below - the
               exclude file is weaker than every .gitignore, probe it
               after. */
            bool exc = false;
            for (size_t i = nf; i > 0 && !exc; i--) {
                int r = liszt_gi_file_match(files[i - 1], gd.rel, next,
                                            comp ? comp + 1 : 0, true);
                if (r >= 0) {
                    exc = r == 1;
                    break;
                }
            }
            if (!exc) {
                int en = snprintf(pathbuf, sizeof pathbuf,
                                  "%s/info/exclude", c->common);
                const struct liszt_gi_file *ef = NULL;
                if (en > 0 && (size_t)en < sizeof pathbuf)
                    ef = gi_load(pathbuf, "", 0);
                if (ef != NULL) {
                    int r = liszt_gi_file_match(ef, gd.rel, next,
                                                comp ? comp + 1 : 0,
                                                true);
                    exc = r == 1;
                }
            }
            if (exc)
                gd.ancestor_ignored = true;
        }
        comp = next;
    }
    for (size_t i = nf; i > 0; i--)
        gd_chain_push(files[i - 1]);
    int en = snprintf(pathbuf, sizeof pathbuf, "%s/info/exclude",
                      c->common);
    if (en > 0 && (size_t)en < sizeof pathbuf) {
        const struct liszt_gi_file *ef = gi_load(pathbuf, "", 0);
        if (ef != NULL)
            gd_chain_push(ef);
    }
    free(abs);
}

/* Repo-relative exclusion probe for one entry NAME in the listed dir.
   Thread-safe: stack buffer, immutable chain. */
static bool
git_name_ignored(const char *name, size_t len, bool is_dir)
{
    if (gd.ancestor_ignored)
        return true;
    char buf[4096];
    size_t plen = gd.rel_len ? gd.rel_len + 1 : 0;
    if (plen + len + 1 > sizeof buf)
        return false;
    if (plen) {
        memcpy(buf, gd.rel, gd.rel_len);
        buf[gd.rel_len] = '/';
    }
    memcpy(buf + plen, name, len);
    buf[plen + len] = '\0';
    return gi_chain_excluded(gd.chain, gd.n_chain, buf, plen + len,
                             plen, is_dir);
}

/* The status letter for one entry (assumes gd.active). */
static char
git_entry_letter(const char *name, size_t len, bool is_dir,
                 const struct liszt_statinfo *st, bool stat_ok)
{
    if (gd.blank)
        return ' ';
    if ((len == 4 && memcmp(name, ".git", 4) == 0)
        || (name[0] == '.'
            && (len == 1 || (len == 2 && name[1] == '.'))))
        return '-';
    struct liszt_git_wtstat ws;
    const struct liszt_git_wtstat *wsp = NULL;
    if (stat_ok) {
        ws.mtime_sec = (int64_t)st->mtime.tv_sec;
        ws.mtime_nsec = (int32_t)st->mtime.tv_nsec;
        ws.size = (uint64_t)st->size;
        ws.mode = (uint32_t)st->mode;
        wsp = &ws;
    }
    char cst = liszt_git_status(gd.ctx, name, len, is_dir, wsp);
    if (cst != 0)
        return cst;
    return git_name_ignored(name, len, is_dir) ? 'I' : 'N';
}

/* --git-ignore: compact the entry list in place before meta, widths,
   and sort ever see it. Tracked names never drop (git's order). */
static void
git_filter_entries(struct liszt_entries *es)
{
    if (!gd.active || !cur_opts->git_ignore || gd.blank)
        return;
    size_t w = 0;
    for (size_t i = 0; i < es->len; i++) {
        struct liszt_entry *e = &es->v[i];
        const char *nm = liszt_entry_name(es, e);
        size_t len = e->name_len;
        bool drop = false;
        bool special = (len == 4 && memcmp(nm, ".git", 4) == 0)
            || (nm[0] == '.'
                && (len == 1 || (len == 2 && nm[1] == '.')));
        if (!special
            && liszt_git_status(gd.ctx, nm, len,
                                e->ftype == LISZT_T_DIR, NULL) == 0)
            drop = git_name_ignored(nm, len, e->ftype == LISZT_T_DIR);
        if (!drop) {
            es->v[w] = *e;
            /* Meta slots are assigned per surviving entry: ensure_meta
               sizes its array to the compacted length. */
            es->v[w].meta_idx = (uint32_t)w;
            w++;
        }
    }
    es->len = w;
}

struct meta_par_ctx {
    const char *dirname;
    const struct liszt_options *o;
    struct liszt_entries *es;
    bool group;
};

static void
meta_par_task(void *vctx, size_t i)
{
    struct meta_par_ctx *ctx = vctx;
    struct liszt_entries *es = ctx->es;
    struct liszt_entry *e = &es->v[i];
    struct liszt_entrymeta *m = &es->meta[e->meta_idx];
    const char *nm = liszt_entry_name(es, e);
    enum liszt_ftype t = e->ftype;

    bool check_stat = plan.needs_stat
        || (ctx->group && t == LISZT_T_UNKNOWN)
        || (t == LISZT_T_UNKNOWN && plan.stat_unknown_type)
        || ((t == LISZT_T_DIR || t == LISZT_T_UNKNOWN)
            && plan.stat_dirs_for_color)
        || ((t == LISZT_T_LNK || t == LISZT_T_UNKNOWN)
            && plan.stat_links)
        || ((t == LISZT_T_REG || t == LISZT_T_UNKNOWN)
            && plan.stat_exec);

    if (check_stat) {
        bool follow = ctx->o->deref == LISZT_DEREF_ALWAYS;
        m->stat_tried = 1;
        if (liszt_statx_join(ctx->dirname, nm,
                             plan.stat_wants | LISZT_WANT_MODE,
                             follow, &m->st) == 0) {
            m->stat_ok = 1;
            e->ftype = (uint8_t)ftype_from_mode(m->st.mode);
        } else {
            m->stat_errno = errno;
            if (gd.column)
                m->git_status = (unsigned char)
                    git_entry_letter(nm, e->name_len,
                                     e->ftype == LISZT_T_DIR, &m->st,
                                     false);
            return;     /* serial pass diagnoses and skips, in order */
        }
    }
    if (gd.column)
        m->git_status = (unsigned char)
            git_entry_letter(nm, e->name_len, e->ftype == LISZT_T_DIR,
                             &m->st, m->stat_ok);
    if (plan.cap_probe
        && (e->ftype == LISZT_T_REG || e->ftype == LISZT_T_UNKNOWN))
        m->has_capability =
            liszt_xattr_list_has(ctx->dirname, nm, "security.capability");
    if (plan.needs_xattr)
        m->acl = probe_acl(ctx->dirname, nm, e->ftype == LISZT_T_DIR);
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
        free(out->absolute_name);   /* allocated before the stat, GNU order */
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
    if (o->print_scontext)
        out->scontext = fetch_scontext("", name,
                                       o->deref == LISZT_DEREF_ALWAYS
                                       || o->deref
                                          == LISZT_DEREF_COMMAND_LINE_ARGUMENTS
                                       || out->is_dir);
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

    /* Parallel phase: syscall-bound per-entry work (statx, xattr
       probes) farmed across the pool; everything ordered or
       arena-appending stays in the serial pass below. */
    bool par = false;
    bool par_work = plan.needs_stat || group || plan.stat_dirs_for_color
        || plan.stat_exec || plan.stat_links || plan.needs_xattr
        || plan.cap_probe;
    if (par_work) {
        static long par_min = -1;
        if (par_min < 0) {
            const char *pm = getenv("LISZT_PARALLEL_MIN");
            par_min = pm ? atol(pm) : 400;
            if (par_min < 1)
                par_min = 1;
        }
        if (es->len >= (size_t)par_min) {
            static long wcap = -1;
            if (wcap < 0) {
                const char *wc = getenv("LISZT_PARALLEL_WORKERS");
                wcap = wc ? atol(wc) : 0;   /* 0 = no override */
            }
            long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
#ifdef __APPLE__
            /* APFS metadata calls serialize past the performance
               cores: the nomad-1 sweep bottomed at P-core count (6),
               with E-cores adding contention (210ms at 6 workers vs
               286ms uncapped, gls 239ms). Cap at perflevel0. */
            {
                int pcores = 0;
                size_t plen = sizeof pcores;
                if (sysctlbyname("hw.perflevel0.logicalcpu", &pcores,
                                 &plen, NULL, 0) == 0 && pcores > 0
                    && (long)pcores < ncpu)
                    ncpu = pcores;
            }
#endif
            size_t workers = es->len / 256 + 1;
            if (ncpu > 0 && workers > (size_t)ncpu)
                workers = (size_t)ncpu;
            if (wcap > 0 && workers > (size_t)wcap)
                workers = (size_t)wcap;
            if (workers > 1) {
                struct meta_par_ctx ctx = { dirname, o, es, group };
                liszt_run_tasks(meta_par_task, &ctx, es->len, workers);
                par = true;
            }
        }
    }

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
            || (t == LISZT_T_UNKNOWN && plan.stat_unknown_type)
            || ((t == LISZT_T_DIR || t == LISZT_T_UNKNOWN)
                && plan.stat_dirs_for_color)
            || ((t == LISZT_T_LNK || t == LISZT_T_UNKNOWN)
                && plan.stat_links)
            || ((t == LISZT_T_REG || t == LISZT_T_UNKNOWN)
                && plan.stat_exec);

        if (check_stat) {
            if (par && m->stat_tried) {
                if (!m->stat_ok) {
                    file_failure(false, "cannot access %s",
                                 liszt_join_path(dirname, nm),
                                 m->stat_errno);
                    continue;
                }
                /* worker already re-derived ftype */
            } else {
                bool follow = o->deref == LISZT_DEREF_ALWAYS;
                /* Any stat implies MODE: conditional fetches (symlink
                   resolution under -L/-R, exec bits, dir color bits)
                   exist to read it, and the ftype re-derivation
                   depends on it. */
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
        if (!par && plan.cap_probe
            && (e->ftype == LISZT_T_REG || e->ftype == LISZT_T_UNKNOWN))
            m->has_capability =
                liszt_xattr_list_has(dirname, nm, "security.capability");
        if (!par && plan.needs_xattr)
            m->acl = probe_acl(dirname, nm, e->ftype == LISZT_T_DIR);
        if (o->print_scontext) {
            char *sc = fetch_scontext(dirname, nm,
                                      o->deref == LISZT_DEREF_ALWAYS);
            if (sc) {
                m->scontext_off =
                    liszt_entries_add_bytes(es, sc, strlen(sc));
                free(sc);
                nm = liszt_entry_name(es, e);
            }
        }
    }
    /* Serial mode: the parallel phase filled letters; sweep the rest
       (0 marks unset - active letters are never 0). */
    if (gd.column)
        for (size_t i = 0; i < es->len; i++) {
            struct liszt_entry *e = &es->v[i];
            struct liszt_entrymeta *m = &es->meta[e->meta_idx];
            if (m->git_status == 0)
                m->git_status = (unsigned char)
                    git_entry_letter(liszt_entry_name(es, e),
                                     e->name_len,
                                     e->ftype == LISZT_T_DIR, &m->st,
                                     m->stat_ok);
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
    it->name_len = e->name_len;
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
    it->scontext = (m && m->scontext_off != UINT32_MAX)
        ? (const char *)es->arena + m->scontext_off
        : "?";
    it->linkmode = m ? m->linkmode : 0;
    it->ftype = e->ftype;
    it->stat_ok = m ? m->stat_ok : 0;
    it->acl = m ? m->acl : 0;
    it->linkok = m ? m->linkok : 0;
    it->has_capability = m ? m->has_capability : 0;
    it->git_status = m ? m->git_status : 0;
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
        || o->print_inode || o->print_scontext;
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
    if (o->print_scontext)
        len += 1 + (o->format == LISZT_FMT_COMMAS
                    ? strlen(it->scontext) : (size_t)w->scontext);
    if (o->print_icons)
        len += 1 + liszt_icon_spacing();
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

    git_begin_dir(name, o);
    git_filter_entries(es);
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

/* --- tree mode (v0.2 extension) --------------------------------------

   Dedicated recursive walker: the pending queue emits batch-per-dir,
   tree interleaves children between siblings. Per directory: diropen ->
   fstat loop-detect -> collect (closes the fd) -> fill/decorate/sort ->
   emit+recurse. One dirfd is live at any moment, at any depth. */

struct tree_glyphs {
    const char *branch, *last, *run, *blank;    /* all 4 cells wide */
    size_t branch_len, last_len, run_len, blank_len;
};

static const struct tree_glyphs tree_glyphs_unicode = {
    "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 ",
    "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 ",
    "\xE2\x94\x82   ",
    "    ",
    10, 10, 6, 4
};

static const struct tree_glyphs tree_glyphs_ascii = {
    "|-- ", "`-- ", "|   ", "    ",
    4, 4, 4, 4
};

/* All walker buffers: per-depth entries pools (cleared and reused
   across same-depth directories), the walked path, and the single
   ancestor-run prefix. Peak memory is the active path only. */
struct tree_ctx {
    const struct liszt_options *o;
    const struct tree_glyphs *g;
    struct liszt_entries *levels;
    size_t n_levels;
    char *path;
    size_t path_len, path_cap;
    char *prefix;
    size_t prefix_len, prefix_cap;
    size_t prefix_width;
    bool ops_some_quoted;       /* operand-batch snapshot for root lines */
};

static void
tree_prefix_append(struct tree_ctx *tc, const char *g, size_t glen)
{
    if (tc->prefix_len + glen > tc->prefix_cap) {
        tc->prefix_cap = (tc->prefix_len + glen) * 2 + 64;
        tc->prefix = liszt_xrealloc(tc->prefix, tc->prefix_cap);
    }
    memcpy(tc->prefix + tc->prefix_len, g, glen);
    tc->prefix_len += glen;
    tc->prefix_width += 4;
}

/* Republished before every emit: recursion can realloc the buffer. */
static void
tree_publish_prefix(const struct tree_ctx *tc)
{
    tree_prefix.bytes = tc->prefix;
    tree_prefix.len = tc->prefix_len;
    tree_prefix.width = tc->prefix_width;
}

static struct liszt_entries *
tree_level_pool(struct tree_ctx *tc, size_t depth)
{
    if (depth >= tc->n_levels) {
        tc->levels = liszt_xrealloc(tc->levels,
                                    (depth + 1) * sizeof *tc->levels);
        while (tc->n_levels <= depth)
            liszt_entries_init(&tc->levels[tc->n_levels++]);
    }
    return &tc->levels[depth];
}

static void
tree_path_push(struct tree_ctx *tc, const char *name, size_t len,
               size_t *save)
{
    *save = tc->path_len;
    if (tc->path_len + len + 2 > tc->path_cap) {
        tc->path_cap = (tc->path_len + len + 2) * 2;
        tc->path = liszt_xrealloc(tc->path, tc->path_cap);
    }
    if (tc->path_len && tc->path[tc->path_len - 1] != '/')
        tc->path[tc->path_len++] = '/';
    memcpy(tc->path + tc->path_len, name, len);
    tc->path_len += len;
    tc->path[tc->path_len] = '\0';
}

/* Child sibling lists inherit the parent's column maxima: a one-pass
   walk cannot know future widths, but monotone widths keep the glyph
   column steady unless a subtree genuinely outgrows its parent (whole-
   tree alignment stays deferred: --tree-align=global). */
static void
widths_merge(struct lwidths *w, const struct lwidths *pw)
{
    if (pw->inode > w->inode) w->inode = pw->inode;
    if (pw->blocks > w->blocks) w->blocks = pw->blocks;
    if (pw->nlink > w->nlink) w->nlink = pw->nlink;
    if (pw->owner > w->owner) w->owner = pw->owner;
    if (pw->group > w->group) w->group = pw->group;
    if (pw->author > w->author) w->author = pw->author;
    if (pw->size > w->size) w->size = pw->size;
    if (pw->major > w->major) w->major = pw->major;
    if (pw->minor > w->minor) w->minor = pw->minor;
    if (pw->scontext > w->scontext) w->scontext = pw->scontext;
    w->any_acl = w->any_acl || pw->any_acl;
}

/* List the directory at tc->path, whose entries sit at DEPTH (root
   operand line = depth 0). Entered iff --level is 0 or depth < level:
   boundary dirs at the level are shown, never entered. */
static void
tree_walk(struct tree_ctx *tc, size_t depth, bool command_line,
          const struct lwidths *pw)
{
    const struct liszt_options *o = tc->o;
    struct liszt_dir *dh;

    if (liszt_diropen(tc->path, &dh) < 0) {
        file_failure(command_line, "cannot open directory %s", tc->path,
                     errno);
        return;
    }
    /* Loop detection is unconditional in tree mode: bind-mount cycles
       need no -L to recurse forever. */
    struct liszt_statinfo di;
    if (liszt_fstat(liszt_dirfd(dh), &di) < 0) {
        file_failure(command_line,
                     "cannot determine device and inode of %s",
                     tc->path, errno);
        liszt_dirclose(dh);
        return;
    }
    if (visit_dir(di.dev, di.ino)) {
        fprintf(stderr, "%s: %s: not listing already-listed"
                " directory\n", liszt_prog, quote_f(tc->path));
        liszt_dirclose(dh);
        liszt_set_exit_status(true);
        return;
    }

    struct dir_diag_ctx dc = { tc->path, command_line };
    struct liszt_ignore_spec ig = {
        .mode = o->ignore,
        .hide = o->hide_patterns,
        .n_hide = o->n_hide_patterns,
        .ignore = o->ignore_patterns,
        .n_ignore = o->n_ignore_patterns
    };
    struct liszt_entries *es = tree_level_pool(tc, depth);
    liszt_dirread_collect_from(dh, &ig, es, on_dirread_fail, &dc);

    git_begin_dir(tc->path, o);
    git_filter_entries(es);
    fill_meta(tc->path, o, es);
    decorate_entries(o, es);
    /* HAZARD (locked): cur_some_quoted is written at decorate and read
       at emit, and child decoration runs between parent emissions -
       snapshot here, restore before every emit and after every
       recursion. */
    bool some_quoted = cur_some_quoted;
    liszt_sort_entries(es);

    /* --tree-limit caps AFTER sorting; hidden entries neither widen
       columns nor get walked. The summary is the final sibling for
       glyph purposes. */
    size_t n_show = es->len;
    bool capped = false;
    if (o->tree_limit && es->len > o->tree_limit) {
        n_show = o->tree_limit;
        capped = true;
    }

    struct lwidths w = { 0 };
    if (needs_columns(o)) {
        struct litem wit;
        for (size_t i = 0; i < n_show; i++) {
            entry_to_item(es, &es->v[i], &wit);
            widths_add(&w, o, &wit);
        }
        if (pw != NULL)
            widths_merge(&w, pw);
    }

    size_t psave = tc->prefix_len;
    size_t pwsave = tc->prefix_width;
    for (size_t i = 0; i < n_show; i++) {
        struct litem it;
        entry_to_item(es, &es->v[i], &it);
        bool last_sib = !capped && i == n_show - 1;

        tree_prefix_append(tc, last_sib ? tc->g->last : tc->g->branch,
                           last_sib ? tc->g->last_len
                                    : tc->g->branch_len);
        tree_publish_prefix(tc);
        cur_some_quoted = some_quoted;
        emit_item(o, &w, &it);
        tc->prefix_len = psave;
        tc->prefix_width = pwsave;

        const char *nm = it.name;
        bool dot_entry = nm[0] == '.'
            && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'));
        if (es->v[i].ftype == LISZT_T_DIR && !dot_entry
            && (o->tree_level == 0 || depth < o->tree_level)) {
            tree_prefix_append(tc, last_sib ? tc->g->blank : tc->g->run,
                               last_sib ? tc->g->blank_len
                                        : tc->g->run_len);
            size_t sv;
            tree_path_push(tc, nm, es->v[i].name_len, &sv);
            tree_walk(tc, depth + 1, false,
                      needs_columns(o) ? &w : NULL);
            /* Recursion can grow the levels array; the pool's own
               buffers never move, but the struct does. */
            es = &tc->levels[depth];
            tc->path_len = sv;
            tc->path[sv] = '\0';
            tc->prefix_len = psave;
            tc->prefix_width = pwsave;
            cur_some_quoted = some_quoted;
        }
    }
    if (capped) {
        /* Plain bytes: no color, no indicator, no icon. */
        tree_prefix_append(tc, tc->g->last, tc->g->last_len);
        char sbuf[32];
        int sn = snprintf(sbuf, sizeof sbuf, "... %zu more",
                          es->len - n_show);
        liszt_emit_bytes(tc->prefix, tc->prefix_len);
        liszt_emit_bytes(sbuf, (size_t)sn);
        liszt_emit_byte(o->eolbyte);
        tc->prefix_len = psave;
        tc->prefix_width = pwsave;
    }
    pop_active_dir();
}

/* Root line via the normal operand->litem path: empty prefix, its own
   single-item widths. */
static void
tree_root(struct tree_ctx *tc, const struct operand *op, bool *first)
{
    const struct liszt_options *o = tc->o;

    if (!*first)
        liszt_emit_byte('\n');
    *first = false;

    struct litem it;
    operand_to_litem(op, &it);
    struct lwidths rw = { 0 };
    if (needs_columns(o))
        widths_add(&rw, o, &it);
    tree_prefix.bytes = NULL;
    tree_prefix.len = 0;
    tree_prefix.width = 0;
    cur_some_quoted = tc->ops_some_quoted;
    emit_item(o, &rw, &it);

    size_t nlen = strlen(op->name);
    if (nlen + 1 > tc->path_cap) {
        tc->path_cap = nlen + 64;
        tc->path = liszt_xrealloc(tc->path, tc->path_cap);
    }
    memcpy(tc->path, op->name, nlen + 1);
    tc->path_len = nlen;
    tc->prefix_len = 0;
    tc->prefix_width = 0;
    tree_walk(tc, 1, true, needs_columns(o) ? &rw : NULL);
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

    if (o.print_icons)
        liszt_icons_init(&o);
    if (o.color_full)
        cf_init_identity();
    liszt_plan_select(&o, &plan);
    liszt_plan_color_update(&o, &plan);
    liszt_plan_icons_update(&o, &plan);
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

    /* --git for command-line operands: resolve serially via each
       operand's dirname context; non-repo files pad blank in a mixed
       batch, a repo-free batch omits the column entirely. */
    if (o.show_git) {
        bool any = false;
        for (int i = 0; i < n_ops; i++) {
            /* Extracted directories never print in the batch; their
               listing resolves its own context. */
            if (ops[i].is_dir)
                continue;
            const char *nm = ops[i].name;
            const char *sl = strrchr(nm, '/');
            char dbuf[4096];
            const char *dnm = ".";
            if (sl != NULL) {
                size_t dl = sl == nm ? 1 : (size_t)(sl - nm);
                if (dl >= sizeof dbuf)
                    continue;
                memcpy(dbuf, nm, dl);
                dbuf[dl] = '\0';
                dnm = dbuf;
            }
            git_begin_dir(dnm, &o);
            if (gd.active) {
                const char *base = sl ? sl + 1 : nm;
                ops[i].git_status = (unsigned char)
                    git_entry_letter(base, strlen(base),
                                     ops[i].ftype == LISZT_T_DIR,
                                     &ops[i].st, ops[i].stat_ok);
                any = true;
            }
        }
        if (any)
            for (int i = 0; i < n_ops; i++)
                if (!ops[i].is_dir && ops[i].git_status == 0)
                    ops[i].git_status = ' ';
    }

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

    if (o.tree) {
        /* Tree mode replaces the queue seed/drain; -d wins upstream by
           never classifying dir operands as extractable. The implicit
           "." is classified explicitly - the root line renders it. */
        struct tree_ctx tc = {
            .o = &o,
            .g = o.tree_unicode ? &tree_glyphs_unicode
                                : &tree_glyphs_ascii,
            .ops_some_quoted = ops_some_quoted,
        };
        struct operand dot;
        bool have_dot = false;
        if (implicit_dot && classify_operand(".", &o, &dot)) {
            decorate_operands(&o, &dot, 1);
            tc.ops_some_quoted = cur_some_quoted;
            have_dot = true;
        }
        if (have_dot) {
            tree_root(&tc, &dot, &first);
        } else {
            for (int i = 0; i < n_ops; i++)
                if (ops[i].is_dir)
                    tree_root(&tc, &ops[i], &first);
        }
        tree_prefix.bytes = NULL;
        tree_prefix.len = 0;
        tree_prefix.width = 0;
        for (size_t i = 0; i < tc.n_levels; i++)
            liszt_entries_free(&tc.levels[i]);
        free(tc.levels);
        free(tc.path);
        free(tc.prefix);
        if (have_dot) {
            free(dot.linkname);
            free(dot.qname);
            free(dot.absolute_name);
            free(dot.scontext);
        }
    } else {
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
    }

    liszt_entries_free(&es);
    for (int i = 0; i < n_ops; i++) {
        free(ops[i].linkname);
        free(ops[i].qname);
        free(ops[i].absolute_name);
        free(ops[i].scontext);
    }
    free(ops);
    for (struct gi_cache_node *n = gi_cache; n != NULL;) {
        struct gi_cache_node *nx = n->next;
        free(n->key);
        if (n->present)
            liszt_gi_file_free(&n->f);
        free(n);
        n = nx;
    }
    free(gd.chain);
    liszt_git_shutdown();
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

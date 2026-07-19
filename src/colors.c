#include "colors.h"

#include <fnmatch.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "emit.h"
#include "util.h"

#include "colors_terms.h"

/* Builtin defaults, byte-for-byte GNU's color_indicator[] (ls.c 626). */
static struct liszt_binstr color_indicator[] = {
    { 2, "\033[" },     /* lc */
    { 1, "m" },         /* rc */
    { 0, NULL },        /* ec */
    { 1, "0" },         /* rs */
    { 0, NULL },        /* no */
    { 0, NULL },        /* fi */
    { 5, "01;34" },     /* di */
    { 5, "01;36" },     /* ln */
    { 2, "33" },        /* pi */
    { 5, "01;35" },     /* so */
    { 5, "01;33" },     /* bd */
    { 5, "01;33" },     /* cd */
    { 0, NULL },        /* mi */
    { 0, NULL },        /* or */
    { 5, "01;32" },     /* ex */
    { 5, "01;35" },     /* do */
    { 5, "37;41" },     /* su */
    { 5, "30;43" },     /* sg */
    { 5, "37;44" },     /* st */
    { 5, "34;42" },     /* ow */
    { 5, "30;42" },     /* tw */
    { 0, NULL },        /* ca */
    { 0, NULL },        /* mh */
    { 3, "\033[K" },    /* cl */
};

static const char indicator_name[][2] = {
    { 'l', 'c' }, { 'r', 'c' }, { 'e', 'c' }, { 'r', 's' }, { 'n', 'o' },
    { 'f', 'i' }, { 'd', 'i' }, { 'l', 'n' }, { 'p', 'i' }, { 's', 'o' },
    { 'b', 'd' }, { 'c', 'd' }, { 'm', 'i' }, { 'o', 'r' }, { 'e', 'x' },
    { 'd', 'o' }, { 's', 'u' }, { 's', 'g' }, { 's', 't' }, { 'o', 'w' },
    { 't', 'w' }, { 'c', 'a' }, { 'm', 'h' }, { 'c', 'l' },
};
enum { N_INDICATORS = sizeof indicator_name / sizeof indicator_name[0] };

struct color_ext_type {
    struct liszt_binstr ext;
    struct liszt_binstr seq;
    bool exact_match;
    struct color_ext_type *next;
};

static struct color_ext_type *color_ext_list;
static char *color_buf;
static bool color_symlink_referent;
static bool used_color;

/* Suffix table (09C): candidates bucketed by case-folded final byte,
   in list order. Sound because any match - exact or case-insensitive -
   requires the suffix's folded last byte to equal the name's; entries
   in other buckets can never match, so first-hit-in-bucket equals
   first-hit-in-list. Shadowed entries (len SIZE_MAX) are dropped at
   build. A zero-length suffix has no final byte; such degenerate
   schemes fall back to the linear walk. */
struct ext_bucket {
    struct color_ext_type **v;
    size_t n, cap;
};
static struct ext_bucket ext_buckets[256];
static bool ext_table_ok;

static unsigned char
fold_byte(unsigned char b)
{
    return (unsigned char)(b >= 'A' && b <= 'Z' ? b + 32 : b);
}

static void
ext_table_build(void)
{
    for (int i = 0; i < 256; i++)
        ext_buckets[i].n = 0;
    ext_table_ok = true;
    for (struct color_ext_type *e = color_ext_list; e != NULL;
         e = e->next) {
        if (e->ext.len == (size_t)-1)
            continue;
        if (e->ext.len == 0) {
            ext_table_ok = false;
            return;
        }
        unsigned char last =
            fold_byte((unsigned char)e->ext.string[e->ext.len - 1]);
        struct ext_bucket *b = &ext_buckets[last];
        if (b->n == b->cap) {
            b->cap = b->cap ? b->cap * 2 : 4;
            b->v = liszt_xrealloc(b->v, b->cap * sizeof *b->v);
        }
        b->v[b->n++] = e;
    }
}

bool
liszt_color_is_colored(enum liszt_cind ind)
{
    size_t len = color_indicator[ind].len;
    const char *s = color_indicator[ind].string;

    return !(len == 0
             || (len == 1 && s[0] == '0')
             || (len == 2 && s[0] == '0' && s[1] == '0'));
}

bool
liszt_color_symlink_as_referent(void)
{
    return color_symlink_referent;
}

static bool
known_term_type(void)
{
    const char *term = getenv("TERM");

    if (!term || !*term)
        return false;
    for (size_t i = 0; i < sizeof known_terms / sizeof known_terms[0]; i++)
        if (fnmatch(known_terms[i], term, 0) == 0)
            return true;
    return false;
}

/* get_funky_string port (ls.c 2470): decode one LS_COLORS value into
   *DEST, stopping at ':' or NUL (or '=' when EQUALS_END). Exported as
   liszt_funky_decode for LS_ICONS (sprint 12); the wrapper keeps this
   internal name for the parser below. */
static bool
get_funky_string(char **dest, const char **src, bool equals_end,
                 size_t *output_count)
{
    char num = 0;
    size_t count = 0;
    enum {
        ST_GND, ST_BACKSLASH, ST_OCTAL, ST_HEX, ST_CARET, ST_END, ST_ERROR
    } state = ST_GND;
    const char *p = *src;
    char *q = *dest;

    while (state < ST_END) {
        switch (state) {
        case ST_GND:
            switch (*p) {
            case ':':
            case '\0':
                state = ST_END;
                break;
            case '\\':
                state = ST_BACKSLASH;
                ++p;
                break;
            case '^':
                state = ST_CARET;
                ++p;
                break;
            case '=':
                if (equals_end) {
                    state = ST_END;
                    break;
                }
                /* fall through */
            default:
                *(q++) = *(p++);
                ++count;
                break;
            }
            break;

        case ST_BACKSLASH:
            switch (*p) {
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7':
                state = ST_OCTAL;
                num = (char)(*p - '0');
                break;
            case 'x': case 'X':
                state = ST_HEX;
                num = 0;
                break;
            case 'a': num = '\a'; break;
            case 'b': num = '\b'; break;
            case 'e': num = 27; break;
            case 'f': num = '\f'; break;
            case 'n': num = '\n'; break;
            case 'r': num = '\r'; break;
            case 't': num = '\t'; break;
            case 'v': num = '\v'; break;
            case '?': num = 127; break;
            case '_': num = ' '; break;
            case '\0':
                state = ST_ERROR;
                break;
            default:
                num = *p;
                break;
            }
            if (state == ST_BACKSLASH) {
                *(q++) = num;
                ++count;
                state = ST_GND;
            }
            ++p;
            break;

        case ST_OCTAL:
            if (*p < '0' || *p > '7') {
                *(q++) = num;
                ++count;
                state = ST_GND;
            } else {
                num = (char)((num << 3) + (*(p++) - '0'));
            }
            break;

        case ST_HEX:
            switch (*p) {
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                num = (char)((num << 4) + (*(p++) - '0'));
                break;
            case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
                num = (char)((num << 4) + (*(p++) - 'a') + 10);
                break;
            case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
                num = (char)((num << 4) + (*(p++) - 'A') + 10);
                break;
            default:
                *(q++) = num;
                ++count;
                state = ST_GND;
                break;
            }
            break;

        case ST_CARET:
            state = ST_GND;
            if (*p >= '@' && *p <= '~') {
                *(q++) = (char)(*(p++) & 037);
                ++count;
            } else if (*p == '?') {
                *(q++) = 127;
                ++count;
            } else {
                state = ST_ERROR;
            }
            break;

        case ST_END: case ST_ERROR: default:
            break;
        }
    }

    *dest = q;
    *src = p;
    *output_count = count;
    return state != ST_ERROR;
}

void
liszt_colors_parse(bool *color_enabled)
{
    const char *p;
    char *buf;
    char label0 = 0, label1 = 0;
    struct color_ext_type *ext = NULL;

    if ((p = getenv("LS_COLORS")) == NULL || *p == '\0') {
        const char *colorterm = getenv("COLORTERM");
        if (!(colorterm && *colorterm) && !known_term_type())
            *color_enabled = false;
        return;
    }

    buf = color_buf = liszt_xstrdup(p);

    enum { PS_START = 1, PS_2, PS_3, PS_4, PS_FAIL, PS_DONE } state =
        PS_START;
    while (true) {
        switch (state) {
        case PS_START:
            switch (*p) {
            case ':':
                ++p;
                break;
            case '*':
                ext = liszt_xmalloc(sizeof *ext);
                ext->next = color_ext_list;
                color_ext_list = ext;
                ext->exact_match = false;
                ++p;
                ext->ext.string = buf;
                state = get_funky_string(&buf, &p, true, &ext->ext.len)
                    ? PS_4 : PS_FAIL;
                break;
            case '\0':
                state = PS_DONE;
                goto done;
            default:
                label0 = *p++;
                state = PS_2;
                break;
            }
            break;

        case PS_2:
            if (*p) {
                label1 = *p++;
                state = PS_3;
            } else {
                state = PS_FAIL;
            }
            break;

        case PS_3:
            state = PS_FAIL;
            if (*(p++) == '=') {
                for (int i = 0; i < N_INDICATORS; i++) {
                    if (label0 == indicator_name[i][0]
                        && label1 == indicator_name[i][1]) {
                        color_indicator[i].string = buf;
                        state = get_funky_string(&buf, &p, false,
                                                 &color_indicator[i].len)
                            ? PS_START : PS_FAIL;
                        break;
                    }
                }
                if (state == PS_FAIL) {
                    char lbl[3] = { label0, label1, '\0' };
                    liszt_error(0, "unrecognized prefix: %s%s%s",
                                liszt_qL(), liszt_quote_diag(lbl),
                                liszt_qR());
                }
            }
            break;

        case PS_4:
            if (*(p++) == '=') {
                ext->seq.string = buf;
                state = get_funky_string(&buf, &p, false, &ext->seq.len)
                    ? PS_START : PS_FAIL;
            } else {
                state = PS_FAIL;
            }
            break;

        case PS_FAIL:
            goto done;

        case PS_DONE: default:
            goto done;
        }
    }
done:

    if (state == PS_FAIL) {
        liszt_error(0,
                    "unparsable value for LS_COLORS environment variable");
        free(color_buf);
        for (struct color_ext_type *e = color_ext_list; e != NULL;) {
            struct color_ext_type *e2 = e;
            e = e->next;
            free(e2);
        }
        color_ext_list = NULL;
        *color_enabled = false;
        return;
    }

    /* Postprocess: exact_match for case-distinct duplicates, SIZE_MAX
       len for entries shadowed by precedence (ls.c 2836). */
    for (struct color_ext_type *e1 = color_ext_list; e1 != NULL;
         e1 = e1->next) {
        bool case_ignored = false;

        for (struct color_ext_type *e2 = e1->next; e2 != NULL;
             e2 = e2->next) {
            if (e2->ext.len < (size_t)-1 && e1->ext.len == e2->ext.len) {
                if (memcmp(e1->ext.string, e2->ext.string, e1->ext.len)
                    == 0) {
                    e2->ext.len = (size_t)-1;
                } else if (liszt_strncasecmp_c(e1->ext.string,
                                               e2->ext.string,
                                               e1->ext.len) == 0) {
                    if (case_ignored) {
                        e2->ext.len = (size_t)-1;
                    } else if (e1->seq.len == e2->seq.len
                               && memcmp(e1->seq.string, e2->seq.string,
                                         e1->seq.len) == 0) {
                        e2->ext.len = (size_t)-1;
                        case_ignored = true;
                    } else {
                        e1->exact_match = true;
                        e2->exact_match = true;
                    }
                }
            }
        }
    }

    if (color_indicator[LISZT_C_LINK].len == 6
        && strncmp(color_indicator[LISZT_C_LINK].string, "target", 6) == 0)
        color_symlink_referent = true;

    ext_table_build();
}

enum liszt_cind
liszt_file_class(const struct liszt_colorable *c)
{
    enum liszt_cind type;

    if (c->linkok == -1 && liszt_color_is_colored(LISZT_C_MISSING)) {
        type = LISZT_C_MISSING;
    } else if (!c->stat_ok) {
        /* GNU filetype_indicator, indexed by our compact ftype enum:
           unknown fifo chr dir blk reg lnk sock wht. */
        static const enum liszt_cind filetype_indicator[] = {
            LISZT_C_ORPHAN, LISZT_C_FIFO, LISZT_C_CHR, LISZT_C_DIR,
            LISZT_C_BLK, LISZT_C_FILE, LISZT_C_LINK, LISZT_C_SOCK,
            LISZT_C_FILE
        };
        type = filetype_indicator[c->ftype];
    } else {
        mode_t mode = c->mode;
        if (S_ISREG(mode)) {
            type = LISZT_C_FILE;
            if ((mode & S_ISUID) != 0
                && liszt_color_is_colored(LISZT_C_SETUID))
                type = LISZT_C_SETUID;
            else if ((mode & S_ISGID) != 0
                     && liszt_color_is_colored(LISZT_C_SETGID))
                type = LISZT_C_SETGID;
            else if (c->has_capability)
                type = LISZT_C_CAP;
            else if ((mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0
                     && liszt_color_is_colored(LISZT_C_EXEC))
                type = LISZT_C_EXEC;
            else if (c->multi_hardlink
                     && liszt_color_is_colored(LISZT_C_MULTIHARDLINK))
                type = LISZT_C_MULTIHARDLINK;
        } else if (S_ISDIR(mode)) {
            type = LISZT_C_DIR;
            if ((mode & S_ISVTX) && (mode & S_IWOTH)
                && liszt_color_is_colored(LISZT_C_STICKY_OTHER_WRITABLE))
                type = LISZT_C_STICKY_OTHER_WRITABLE;
            else if ((mode & S_IWOTH) != 0
                     && liszt_color_is_colored(LISZT_C_OTHER_WRITABLE))
                type = LISZT_C_OTHER_WRITABLE;
            else if ((mode & S_ISVTX) != 0
                     && liszt_color_is_colored(LISZT_C_STICKY))
                type = LISZT_C_STICKY;
        } else if (S_ISLNK(mode)) {
            type = LISZT_C_LINK;
        } else if (S_ISFIFO(mode)) {
            type = LISZT_C_FIFO;
        } else if (S_ISSOCK(mode)) {
            type = LISZT_C_SOCK;
        } else if (S_ISBLK(mode)) {
            type = LISZT_C_BLK;
        } else if (S_ISCHR(mode)) {
            type = LISZT_C_CHR;
        } else {
            type = LISZT_C_ORPHAN;
        }
    }
    return type;
}

const struct liszt_binstr *
liszt_color_for(const struct liszt_colorable *c)
{
    enum liszt_cind type = liszt_file_class(c);
    struct color_ext_type *ext = NULL;

    if (type == LISZT_C_FILE) {
        size_t len = strlen(c->name);
        const char *name = c->name + len;
        if (ext_table_ok && len > 0) {
            const struct ext_bucket *b =
                &ext_buckets[fold_byte((unsigned char)name[-1])];
            for (size_t i = 0; i < b->n; i++) {
                struct color_ext_type *e = b->v[i];
                if (e->ext.len <= len) {
                    if (e->exact_match
                        ? memcmp(name - e->ext.len, e->ext.string,
                                 e->ext.len) == 0
                        : liszt_strncasecmp_c(name - e->ext.len,
                                              e->ext.string,
                                              e->ext.len) == 0) {
                        ext = e;
                        break;
                    }
                }
            }
        } else {
            for (ext = color_ext_list; ext != NULL; ext = ext->next) {
                if (ext->ext.len <= len) {
                    if (ext->exact_match) {
                        if (memcmp(name - ext->ext.len, ext->ext.string,
                                   ext->ext.len) == 0)
                            break;
                    } else {
                        if (liszt_strncasecmp_c(name - ext->ext.len,
                                                ext->ext.string,
                                                ext->ext.len) == 0)
                            break;
                    }
                }
            }
        }
    }

    if (type == LISZT_C_LINK && !c->linkok) {
        if (color_symlink_referent
            || liszt_color_is_colored(LISZT_C_ORPHAN))
            type = LISZT_C_ORPHAN;
    }

    const struct liszt_binstr *s =
        ext ? &ext->seq : &color_indicator[type];
    return s->string ? s : NULL;
}

/* --- --color=full filename classes (v0.3) ------------------------------ */

#include "colorclass_tab.h"

/* Class order matches gen-colorclass-tab.sh: Image Video Music
   Lossless Crypto Document Compressed Temp Compiled Build Source. */
static const struct liszt_binstr lcc_styles[11] = {
    { 2, "35" }, { 4, "1;35" }, { 2, "36" }, { 4, "1;36" },
    { 4, "1;32" }, { 2, "32" }, { 2, "31" }, { 1, "2" },
    { 2, "33" }, { 6, "1;4;33" }, { 4, "1;33" },
};

static unsigned char
lcc_fold(unsigned char b)
{
    return (unsigned char)(b >= 'A' && b <= 'Z' ? b + 32 : b);
}

const struct liszt_binstr *
liszt_colorclass_for(const char *name, size_t len)
{
    if (len == 0)
        return NULL;
    /* Case-insensitive readme prefix wins (eza's compatibility rule). */
    if (len >= 6 && liszt_strncasecmp_c(name, "readme", 6) == 0)
        return &lcc_styles[9];
    unsigned char last = lcc_fold((unsigned char)name[len - 1]);
    for (uint16_t i = lcc_name_bucket[last];
         i < lcc_name_bucket[last + 1]; i++) {
        const struct lcc_ent *e = &lcc_name_tab[i];
        if (e->key_len == len
            && memcmp(lcc_name_pool + e->key_off, name, len) == 0)
            return &lcc_styles[e->class];
    }
    const char *dot = memrchr(name, '.', len);
    if (dot != NULL && dot != name && dot[1] != '\0') {
        const char *ext = dot + 1;
        size_t elen = len - (size_t)(ext - name);
        for (uint16_t i = lcc_ext_bucket[last];
             i < lcc_ext_bucket[last + 1]; i++) {
            const struct lcc_ent *e = &lcc_ext_tab[i];
            if (e->key_len == elen
                && liszt_strncasecmp_c(ext, lcc_ext_pool + e->key_off,
                                       elen) == 0)
                return &lcc_styles[e->class];
        }
    }
    if (name[len - 1] == '~'
        || (name[0] == '#' && name[len - 1] == '#' && len > 1))
        return &lcc_styles[7];
    return NULL;
}

/* --- emission ---------------------------------------------------------- */

static off_t escape_bytes;

off_t
liszt_color_bytes(void)
{
    return escape_bytes;
}

void
liszt_color_put(const struct liszt_binstr *s)
{
    if (!used_color) {
        /* GNU put_indicator first-use: mark, then emit the reset
           prologue (signal-handler install is tty-only and deferred -
           the piped goldens cannot observe it). */
        used_color = true;
        liszt_color_prep_non_filename();
    }
    escape_bytes += (off_t)s->len;
    liszt_emit_bytes(s->string, s->len);
}

void
liszt_color_put_ind(enum liszt_cind ind)
{
    liszt_color_put(&color_indicator[ind]);
}

void
liszt_color_restore_default(void)
{
    liszt_color_put_ind(LISZT_C_LEFT);
    liszt_color_put_ind(LISZT_C_RIGHT);
}

void
liszt_color_set_normal(void)
{
    if (liszt_color_is_colored(LISZT_C_NORM)) {
        liszt_color_put_ind(LISZT_C_LEFT);
        liszt_color_put_ind(LISZT_C_NORM);
        liszt_color_put_ind(LISZT_C_RIGHT);
    }
}

void
liszt_color_start(const struct liszt_binstr *seq)
{
    if (liszt_color_is_colored(LISZT_C_NORM))
        liszt_color_restore_default();
    liszt_color_put_ind(LISZT_C_LEFT);
    liszt_color_put(seq);
    liszt_color_put_ind(LISZT_C_RIGHT);
}

void
liszt_color_prep_non_filename(void)
{
    if (color_indicator[LISZT_C_END].string != NULL) {
        liszt_color_put_ind(LISZT_C_END);
    } else {
        liszt_color_put_ind(LISZT_C_LEFT);
        liszt_color_put_ind(LISZT_C_RESET);
        liszt_color_put_ind(LISZT_C_RIGHT);
    }
}

/* Batched styled-char run: N single bytes each under its own (maybe
   NULL) style, emitted as ONE write with one accounting update - the
   --color=full mode string costs 11 regions per line otherwise.
   Callers must fall back to per-token puts when C_NORM is colored
   (start's restore-default dance cannot batch). */
void
liszt_color_put_run(const struct liszt_binstr *const *seqs,
                    const char *chars, size_t n)
{
    char buf[512];
    size_t len = 0;
    off_t esc = 0;

    for (size_t i = 0; i < n; i++) {
        const struct liszt_binstr *s = seqs[i];
        if (s == NULL) {
            buf[len++] = chars[i];
            continue;
        }
        /* First-use prologue stays lazy (GNU put_indicator order):
           flush the plain prefix, then the reset, then the batch. */
        if (!used_color) {
            if (len > 0) {
                liszt_emit_bytes(buf, len);
                len = 0;
            }
            used_color = true;
            liszt_color_prep_non_filename();
        }
        size_t need = color_indicator[LISZT_C_LEFT].len + s->len
            + color_indicator[LISZT_C_RIGHT].len + 1
            + (color_indicator[LISZT_C_END].string != NULL
                   ? color_indicator[LISZT_C_END].len
                   : color_indicator[LISZT_C_LEFT].len
                         + color_indicator[LISZT_C_RESET].len
                         + color_indicator[LISZT_C_RIGHT].len);
        if (len + need >= sizeof buf) {
            liszt_emit_bytes(buf, len);
            len = 0;
        }
        memcpy(buf + len, color_indicator[LISZT_C_LEFT].string,
               color_indicator[LISZT_C_LEFT].len);
        len += color_indicator[LISZT_C_LEFT].len;
        memcpy(buf + len, s->string, s->len);
        len += s->len;
        memcpy(buf + len, color_indicator[LISZT_C_RIGHT].string,
               color_indicator[LISZT_C_RIGHT].len);
        len += color_indicator[LISZT_C_RIGHT].len;
        esc += (off_t)(need - 1);
        buf[len++] = chars[i];
        if (color_indicator[LISZT_C_END].string != NULL) {
            memcpy(buf + len, color_indicator[LISZT_C_END].string,
                   color_indicator[LISZT_C_END].len);
            len += color_indicator[LISZT_C_END].len;
        } else {
            memcpy(buf + len, color_indicator[LISZT_C_LEFT].string,
                   color_indicator[LISZT_C_LEFT].len);
            len += color_indicator[LISZT_C_LEFT].len;
            memcpy(buf + len, color_indicator[LISZT_C_RESET].string,
                   color_indicator[LISZT_C_RESET].len);
            len += color_indicator[LISZT_C_RESET].len;
            memcpy(buf + len, color_indicator[LISZT_C_RIGHT].string,
                   color_indicator[LISZT_C_RIGHT].len);
            len += color_indicator[LISZT_C_RIGHT].len;
        }
    }
    escape_bytes += esc;
    if (len > 0)
        liszt_emit_bytes(buf, len);
}

/* One styled token (LEFT seq RIGHT bytes END) as a single write with
   one accounting update; falls back to the start/prep pair when a
   colored C_NORM forces the restore-default dance. */
void
liszt_color_put_token(const struct liszt_binstr *seq, const char *bytes,
                      size_t blen)
{
    char buf[512];
    size_t len = 0;

    if (liszt_color_is_colored(LISZT_C_NORM)
        || blen + 64 > sizeof buf) {
        liszt_color_start(seq);
        liszt_emit_bytes(bytes, blen);
        liszt_color_prep_non_filename();
        return;
    }
    if (!used_color) {
        used_color = true;
        liszt_color_prep_non_filename();
    }
    memcpy(buf + len, color_indicator[LISZT_C_LEFT].string,
           color_indicator[LISZT_C_LEFT].len);
    len += color_indicator[LISZT_C_LEFT].len;
    memcpy(buf + len, seq->string, seq->len);
    len += seq->len;
    memcpy(buf + len, color_indicator[LISZT_C_RIGHT].string,
           color_indicator[LISZT_C_RIGHT].len);
    len += color_indicator[LISZT_C_RIGHT].len;
    off_t esc = (off_t)len;
    memcpy(buf + len, bytes, blen);
    len += blen;
    if (color_indicator[LISZT_C_END].string != NULL) {
        memcpy(buf + len, color_indicator[LISZT_C_END].string,
               color_indicator[LISZT_C_END].len);
        len += color_indicator[LISZT_C_END].len;
        esc += (off_t)color_indicator[LISZT_C_END].len;
    } else {
        memcpy(buf + len, color_indicator[LISZT_C_LEFT].string,
               color_indicator[LISZT_C_LEFT].len);
        len += color_indicator[LISZT_C_LEFT].len;
        memcpy(buf + len, color_indicator[LISZT_C_RESET].string,
               color_indicator[LISZT_C_RESET].len);
        len += color_indicator[LISZT_C_RESET].len;
        memcpy(buf + len, color_indicator[LISZT_C_RIGHT].string,
               color_indicator[LISZT_C_RIGHT].len);
        len += color_indicator[LISZT_C_RIGHT].len;
        esc += (off_t)(color_indicator[LISZT_C_LEFT].len
                       + color_indicator[LISZT_C_RESET].len
                       + color_indicator[LISZT_C_RIGHT].len);
    }
    escape_bytes += esc;
    liszt_emit_bytes(buf, len);
}

/* Pure assembly of a styled run into OUT (no emission): returns byte
   length, adds SGR overhead into *ESC. The memoized mode-string path
   builds once and replays via put_prebuilt. */
size_t
liszt_color_build_run(const struct liszt_binstr *const *seqs,
                      const char *chars, size_t n, char *out,
                      size_t cap, off_t *esc)
{
    size_t len = 0;
    *esc = 0;
    for (size_t i = 0; i < n; i++) {
        const struct liszt_binstr *s = seqs[i];
        size_t need = 1;
        if (s != NULL)
            need += color_indicator[LISZT_C_LEFT].len + s->len
                + color_indicator[LISZT_C_RIGHT].len
                + (color_indicator[LISZT_C_END].string != NULL
                       ? color_indicator[LISZT_C_END].len
                       : color_indicator[LISZT_C_LEFT].len
                             + color_indicator[LISZT_C_RESET].len
                             + color_indicator[LISZT_C_RIGHT].len);
        if (len + need > cap)
            return 0;
        if (s == NULL) {
            out[len++] = chars[i];
            continue;
        }
        memcpy(out + len, color_indicator[LISZT_C_LEFT].string,
               color_indicator[LISZT_C_LEFT].len);
        len += color_indicator[LISZT_C_LEFT].len;
        memcpy(out + len, s->string, s->len);
        len += s->len;
        memcpy(out + len, color_indicator[LISZT_C_RIGHT].string,
               color_indicator[LISZT_C_RIGHT].len);
        len += color_indicator[LISZT_C_RIGHT].len;
        out[len++] = chars[i];
        if (color_indicator[LISZT_C_END].string != NULL) {
            memcpy(out + len, color_indicator[LISZT_C_END].string,
                   color_indicator[LISZT_C_END].len);
            len += color_indicator[LISZT_C_END].len;
        } else {
            memcpy(out + len, color_indicator[LISZT_C_LEFT].string,
                   color_indicator[LISZT_C_LEFT].len);
            len += color_indicator[LISZT_C_LEFT].len;
            memcpy(out + len, color_indicator[LISZT_C_RESET].string,
                   color_indicator[LISZT_C_RESET].len);
            len += color_indicator[LISZT_C_RESET].len;
            memcpy(out + len, color_indicator[LISZT_C_RIGHT].string,
                   color_indicator[LISZT_C_RIGHT].len);
            len += color_indicator[LISZT_C_RIGHT].len;
        }
        *esc += (off_t)(need - 1);
    }
    return len;
}

void
liszt_color_put_prebuilt(const char *bytes, size_t len, off_t esc)
{
    if (!used_color) {
        used_color = true;
        liszt_color_prep_non_filename();
    }
    escape_bytes += esc;
    liszt_emit_bytes(bytes, len);
}

bool
liszt_color_used(void)
{
    return used_color;
}

/* Whether the main-exit restore is a no-op (lc/rc at their defaults). */
bool
liszt_color_restore_is_noop(void)
{
    return color_indicator[LISZT_C_LEFT].len == 2
        && memcmp(color_indicator[LISZT_C_LEFT].string, "\033[", 2) == 0
        && color_indicator[LISZT_C_RIGHT].len == 1
        && color_indicator[LISZT_C_RIGHT].string[0] == 'm';
}

bool
liszt_funky_decode(char **dest, const char **src, bool equals_end,
                   size_t *output_count)
{
    return get_funky_string(dest, src, equals_end, output_count);
}

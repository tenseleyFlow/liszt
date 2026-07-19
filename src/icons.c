#include "icons.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "options.h"
#include "util.h"

#include "icons_tab.h"

/* --- LS_ICONS overrides ----------------------------------------------

   LS_COLORS-shaped: colon-separated key=value with funky-decoded
   values. Key shapes: "*.suf" suffix (case-insensitive), "name/"
   exact dirname (case-sensitive), two-char LS_COLORS label, plain
   exact filename (case-sensitive). Empty value = blank cell and stops
   the chain. Parse failure diagnoses and SOFT-FAILS to builtins (a
   typo should not strip the feature; deliberate divergence from
   LS_COLORS' kill-color, documented). */

enum ov_kind { OV_NAME, OV_DIR, OV_SUFFIX };

struct icon_ov {
    enum ov_kind kind;
    struct liszt_binstr key;
    struct liszt_binstr val;    /* len 0 = blank-cell kill switch */
    struct icon_ov *next;
};

static struct icon_ov *ov_list;      /* parse order = priority order */
static char *ov_buf;
static struct liszt_binstr label_ov[24];    /* by liszt_cind; len -1 unset */
static bool label_ov_set[24];
static bool observe_stat;

static unsigned spacing = 1;
static bool osc66;

/* Override bucket tables, same folded-final-byte discipline as the
   generated builtin tables. */
struct ov_bucket {
    struct icon_ov **v;
    size_t n, cap;
};
static struct ov_bucket ov_name_b[256], ov_dir_b[256], ov_suf_b[256];

static unsigned char
fold_b(unsigned char b)
{
    return (unsigned char)(b >= 'A' && b <= 'Z' ? b + 32 : b);
}

static void
ov_bucket_add(struct ov_bucket *tab, struct icon_ov *ov)
{
    if (ov->key.len == 0)
        return;
    struct ov_bucket *b =
        &tab[fold_b((unsigned char)ov->key.string[ov->key.len - 1])];
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4;
        b->v = liszt_xrealloc(b->v, b->cap * sizeof *b->v);
    }
    b->v[b->n++] = ov;
}

static const char label_names[][2] = {
    { 'l', 'c' }, { 'r', 'c' }, { 'e', 'c' }, { 'r', 's' }, { 'n', 'o' },
    { 'f', 'i' }, { 'd', 'i' }, { 'l', 'n' }, { 'p', 'i' }, { 's', 'o' },
    { 'b', 'd' }, { 'c', 'd' }, { 'm', 'i' }, { 'o', 'r' }, { 'e', 'x' },
    { 'd', 'o' }, { 's', 'u' }, { 's', 'g' }, { 's', 't' }, { 'o', 'w' },
    { 't', 'w' }, { 'c', 'a' }, { 'm', 'h' }, { 'c', 'l' },
};

static int
label_index(const char *k, size_t len)
{
    if (len != 2)
        return -1;
    for (int i = 0; i < 24; i++)
        if (label_names[i][0] == k[0] && label_names[i][1] == k[1])
            return i;
    return -1;
}

enum { OV_VALUE_CAP = 16 };

static void
parse_ls_icons(void)
{
    const char *env = getenv("LS_ICONS");

    if (env == NULL || *env == '\0')
        return;

    /* Decoded keys and values both land in one buffer, LS_COLORS
       parser shape. */
    ov_buf = liszt_xmalloc(strlen(env) * 2 + 2);
    char *q = ov_buf;
    const char *p = env;
    bool fail = false;

    while (!fail && *p != '\0') {
        while (*p == ':')
            p++;
        if (*p == '\0')
            break;

        char *kstart = q;
        size_t klen = 0;
        bool suffix = false;

        if (*p == '*') {
            suffix = true;
            p++;
        }
        if (!liszt_funky_decode(&q, &p, true, &klen)) {
            fail = true;
            break;
        }
        if (*p != '=') {
            fail = true;
            break;
        }
        p++;
        bool dir = false;
        if (!suffix && klen > 0 && kstart[klen - 1] == '/') {
            dir = true;
            klen--;
        }
        char *vstart = q;
        size_t vlen = 0;
        if (!liszt_funky_decode(&q, &p, false, &vlen)) {
            fail = true;
            break;
        }
        if (vlen > OV_VALUE_CAP) {
            fail = true;
            break;
        }
        if (klen == 0) {
            fail = true;
            break;
        }

        int li;
        if (!suffix && !dir && (li = label_index(kstart, klen)) >= 0) {
            label_ov[li].string = vstart;
            label_ov[li].len = vlen;
            label_ov_set[li] = true;
            if (li == LISZT_C_EXEC || li == LISZT_C_SETUID
                || li == LISZT_C_SETGID || li == LISZT_C_OTHER_WRITABLE
                || li == LISZT_C_STICKY
                || li == LISZT_C_STICKY_OTHER_WRITABLE)
                observe_stat = true;
            continue;
        }

        struct icon_ov *ov = liszt_xmalloc(sizeof *ov);
        ov->kind = suffix ? OV_SUFFIX : dir ? OV_DIR : OV_NAME;
        ov->key.string = kstart;
        ov->key.len = klen;
        ov->val.string = vstart;
        ov->val.len = vlen;
        ov->next = NULL;
        /* Append preserving parse order: first-listed wins. */
        struct icon_ov **tail = &ov_list;
        while (*tail)
            tail = &(*tail)->next;
        *tail = ov;
    }

    if (fail) {
        liszt_error(0,
                    "unparsable value for LS_ICONS environment variable");
        /* Soft-fail: discard overrides, keep builtin icons. */
        for (struct icon_ov *o = ov_list; o != NULL;) {
            struct icon_ov *n = o->next;
            free(o);
            o = n;
        }
        ov_list = NULL;
        memset(label_ov_set, 0, sizeof label_ov_set);
        observe_stat = false;
        free(ov_buf);
        ov_buf = NULL;
        return;
    }

    for (struct icon_ov *o = ov_list; o != NULL; o = o->next)
        ov_bucket_add(o->kind == OV_SUFFIX ? ov_suf_b
                      : o->kind == OV_DIR ? ov_dir_b : ov_name_b, o);
}

void
liszt_icons_init(const struct liszt_options *o)
{
    (void)o;
    const char *sp = getenv("LISZT_ICON_SPACING");
    if (sp == NULL)
        sp = getenv("EZA_ICON_SPACING");
    if (sp != NULL && *sp != '\0') {
        char *end;
        long v = strtol(sp, &end, 10);
        if (end != sp && *end == '\0' && v >= 0 && v <= 8)
            spacing = (unsigned)v;
    }
    const char *oe = getenv("LISZT_ICONS_OSC66");
    osc66 = oe != NULL && strcmp(oe, "1") == 0;
    parse_ls_icons();
}

unsigned
liszt_icon_spacing(void)
{
    return spacing;
}

bool
liszt_icon_osc66(void)
{
    return osc66;
}

bool
liszt_icons_observe_stat(void)
{
    return observe_stat;
}

/* --- lookup ----------------------------------------------------------- */

static const struct icon_ov *
ov_lookup(const struct ov_bucket *tab, const char *name, size_t len,
          bool fold_cmp)
{
    if (len == 0)
        return NULL;
    const struct ov_bucket *b =
        &tab[fold_b((unsigned char)name[len - 1])];
    for (size_t i = 0; i < b->n; i++) {
        const struct icon_ov *ov = b->v[i];
        if (fold_cmp) {
            if (ov->key.len <= len
                && liszt_strncasecmp_c(name + len - ov->key.len,
                                       ov->key.string, ov->key.len) == 0)
                return ov;
        } else {
            if (ov->key.len == len
                && memcmp(name, ov->key.string, len) == 0)
                return ov;
        }
    }
    return NULL;
}

static int
tab_lookup_exact(const struct licon_ent *tab, const uint16_t *bucket,
                 const char *pool, const char *name, size_t len)
{
    if (len == 0)
        return -1;
    unsigned char last = fold_b((unsigned char)name[len - 1]);
    for (uint16_t i = bucket[last]; i < bucket[last + 1]; i++)
        if (tab[i].key_len == len
            && memcmp(pool + tab[i].key_off, name, len) == 0)
            return tab[i].glyph;
    return -1;
}

static int
tab_lookup_suffix(const struct licon_ent *tab, const uint16_t *bucket,
                  const char *pool, const char *name, size_t len)
{
    if (len == 0)
        return -1;
    unsigned char last = fold_b((unsigned char)name[len - 1]);
    for (uint16_t i = bucket[last]; i < bucket[last + 1]; i++)
        if (tab[i].key_len <= len
            && liszt_strncasecmp_c(name + len - tab[i].key_len,
                                   pool + tab[i].key_off,
                                   tab[i].key_len) == 0)
            return tab[i].glyph;
    return -1;
}

static void
out_glyph(struct liszt_icon *out, int g)
{
    out->bytes = licon_glyphs[g].bytes;
    out->len = licon_glyphs[g].len;
}

static bool
out_ov(struct liszt_icon *out, const struct icon_ov *ov)
{
    out->bytes = ov->val.string;
    out->len = (unsigned)ov->val.len;
    return true;
}

void
liszt_icon_for(const struct liszt_colorable *c, struct liszt_icon *out)
{
    enum liszt_cind class = liszt_file_class(c);
    const char *name = c->name;
    size_t len = strlen(name);
    const struct icon_ov *ov;
    int g;

    out->bytes = NULL;
    out->len = 0;

    bool is_dir = class == LISZT_C_DIR || class == LISZT_C_STICKY
        || class == LISZT_C_OTHER_WRITABLE
        || class == LISZT_C_STICKY_OTHER_WRITABLE;

    if (is_dir) {
        if ((ov = ov_lookup(ov_dir_b, name, len, false)) != NULL) {
            out_ov(out, ov);
            return;
        }
        if ((g = tab_lookup_exact(licon_dir_tab, licon_dir_bucket,
                                  licon_dir_pool, name, len)) >= 0) {
            out_glyph(out, g);
            return;
        }
    } else if (class == LISZT_C_FILE || class == LISZT_C_EXEC
               || class == LISZT_C_SETUID || class == LISZT_C_SETGID
               || class == LISZT_C_MULTIHARDLINK
               || class == LISZT_C_CAP) {
        if ((ov = ov_lookup(ov_name_b, name, len, false)) != NULL) {
            out_ov(out, ov);
            return;
        }
        if ((g = tab_lookup_exact(licon_name_tab, licon_name_bucket,
                                  licon_name_pool, name, len)) >= 0) {
            out_glyph(out, g);
            return;
        }
        if ((ov = ov_lookup(ov_suf_b, name, len, true)) != NULL) {
            out_ov(out, ov);
            return;
        }
        if ((g = tab_lookup_suffix(licon_ext_tab, licon_ext_bucket,
                                   licon_ext_pool, name, len)) >= 0) {
            out_glyph(out, g);
            return;
        }
    }

    /* Filetype default: LS_ICONS label override, else builtin table,
       else fi. */
    for (int probe = 0; probe < 2; probe++) {
        enum liszt_cind k = probe == 0 ? class : LISZT_C_FILE;
        if (label_ov_set[k]) {
            out->bytes = label_ov[k].string;
            out->len = (unsigned)label_ov[k].len;
            return;
        }
        if (licon_class_glyph[k] != 65535) {
            out_glyph(out, licon_class_glyph[k]);
            return;
        }
    }
}

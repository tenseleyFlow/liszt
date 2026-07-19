#include "sortkey.h"

#include <errno.h>
#include <locale.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* Ports from rank (adapted, not linked): the collation identity probe
   (locale.c), the MSD byte radix with stable scatter and reversed-bucket
   -r (radix.c), and the strxfrm transform arena (line.c). Simplified
   where ls's domain allows: names cannot contain NUL, so memcoll segment
   handling collapses to plain strcoll; and every comparator chain ends
   in a name comparison over unique-per-directory names, so equal keys
   are byte-identical names (duplicate operands only). */

enum {
    RADIX_BUCKETS = 257,            /* 0 = end-of-key, 1..256 = byte+1 */
    RADIX_INSERTION_THRESHOLD = 24,
    RADIX_DEPTH_LIMIT = 4096
};

static struct {
    enum liszt_sortword word;
    bool reverse;
    bool group_dirs;
    enum liszt_sort_plan engine;
    bool use_strcmp;        /* strcoll failed; GNU falls back wholesale */
    bool in_verify;         /* verify pass: never diagnose, never longjmp */
    bool verify_skipped;
    jmp_buf failed_strcoll;
    /* Transform arena (engine RADIX_TRANSFORMED). */
    unsigned char *xarena;
    size_t xarena_len;
    size_t xarena_cap;
    /* Stats for LISZT_DEBUG_STATS. */
    size_t passes;
    size_t classified;
    size_t insertion_sorts;
    size_t comparator_calls;
} S;

static int identity_cached = -1;

static bool
locale_name_is_identity(const char *name)
{
    return name != NULL
        && (strcmp(name, "C") == 0 || strcmp(name, "POSIX") == 0);
}

static bool
probe_collation_identity(void)
{
    const char *name = setlocale(LC_COLLATE, NULL);

    if (locale_name_is_identity(name))
        return true;
    if (MB_CUR_MAX != 1)
        return false;
    for (unsigned int i = 1; i <= 255U; i++) {
        char a[2] = { (char)i, '\0' };
        for (unsigned int j = 1; j <= 255U; j++) {
            char b[2] = { (char)j, '\0' };
            int coll = strcoll(a, b);
            int byte = i == j ? 0 : (i < j ? -1 : 1);
            if ((coll < 0 && byte >= 0) || (coll > 0 && byte <= 0)
                || (coll == 0 && byte != 0))
                return false;
        }
    }
    return true;
}

bool
liszt_locale_collation_identity(void)
{
    if (identity_cached < 0)
        identity_cached = probe_collation_identity() ? 1 : 0;
    return identity_cached != 0;
}

/* --- filevercmp (rank's counted-byte port of gnulib) ------------------ */

static bool
ver_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

static bool
ver_alpha(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool
ver_alnum(unsigned char c)
{
    return ver_alpha(c) || ver_digit(c);
}

static size_t
version_file_prefix_len(const unsigned char *text, size_t len)
{
    size_t prefix_len = 0;
    size_t i = 0;

    for (;;) {
        if (i == len)
            return prefix_len;
        i++;
        prefix_len = i;
        while (i + 1 < len && text[i] == '.'
               && (ver_alpha(text[i + 1]) || text[i + 1] == '~')) {
            i += 2;
            while (i < len && (ver_alnum(text[i]) || text[i] == '~'))
                i++;
        }
    }
}

static int
version_order(const unsigned char *text, size_t pos, size_t len)
{
    if (pos == len)
        return -1;
    unsigned char byte = text[pos];
    if (ver_digit(byte))
        return 0;
    if (ver_alpha(byte))
        return (int)byte;
    if (byte == '~')
        return -2;
    return (int)byte + 256;
}

static int
version_reverse_compare(const unsigned char *a, size_t a_len,
                        const unsigned char *b, size_t b_len)
{
    size_t ai = 0;
    size_t bi = 0;

    while (ai < a_len || bi < b_len) {
        int first_diff = 0;

        while ((ai < a_len && !ver_digit(a[ai]))
               || (bi < b_len && !ver_digit(b[bi]))) {
            int ao = version_order(a, ai, a_len);
            int bo = version_order(b, bi, b_len);
            if (ao != bo)
                return ao - bo;
            ai++;
            bi++;
        }
        while (ai < a_len && a[ai] == '0')
            ai++;
        while (bi < b_len && b[bi] == '0')
            bi++;
        while (ai < a_len && bi < b_len && ver_digit(a[ai])
               && ver_digit(b[bi])) {
            if (first_diff == 0)
                first_diff = (int)a[ai] - (int)b[bi];
            ai++;
            bi++;
        }
        if (ai < a_len && ver_digit(a[ai]))
            return 1;
        if (bi < b_len && ver_digit(b[bi]))
            return -1;
        if (first_diff != 0)
            return first_diff;
    }
    return 0;
}

static int
filevercmp(const char *sa, const char *sb)
{
    const unsigned char *a = (const unsigned char *)sa;
    const unsigned char *b = (const unsigned char *)sb;
    size_t a_len = strlen(sa);
    size_t b_len = strlen(sb);

    if (a_len == 0)
        return b_len == 0 ? 0 : -1;
    if (b_len == 0)
        return 1;

    /* "" < "." < ".." < other dotfiles < the rest. */
    if (a[0] == '.') {
        if (b[0] != '.')
            return -1;
        bool a_dot = a_len == 1, b_dot = b_len == 1;
        if (a_dot)
            return b_dot ? 0 : -1;
        if (b_dot)
            return 1;
        bool a_dd = a_len == 2 && a[1] == '.';
        bool b_dd = b_len == 2 && b[1] == '.';
        if (a_dd)
            return b_dd ? 0 : -1;
        if (b_dd)
            return 1;
    } else if (b[0] == '.') {
        return 1;
    }

    size_t ap = version_file_prefix_len(a, a_len);
    size_t bp = version_file_prefix_len(b, b_len);
    bool one_pass_only = ap == a_len && bp == b_len;
    int result = version_reverse_compare(a, ap, b, bp);
    if (result != 0 || one_pass_only)
        return result;
    return version_reverse_compare(a, a_len, b, b_len);
}

/* --- scalar comparator chain (the oracle) ----------------------------- */

/* GNU xstrcoll: on strcoll error, diagnose (minor status) and longjmp;
   the whole sort is redone with strcmp. */
static int
name_coll(const char *a, const char *b)
{
    S.comparator_calls++;
    if (S.use_strcmp)
        return strcmp(a, b);
    errno = 0;
    int diff = strcoll(a, b);
    if (errno != 0) {
        /* The verify oracle must not disturb GNU-parity behavior: on
           EILSEQ platforms (BSD libc) it silently stands down instead. */
        if (S.in_verify) {
            S.verify_skipped = true;
            return 0;
        }
        liszt_error(errno, "cannot compare file names %s%s%s and %s%s%s",
                    liszt_qL(), liszt_quote_diag(a), liszt_qR(),
                    liszt_qL(), liszt_quote_diag(b), liszt_qR());
        liszt_set_exit_status(false);
        longjmp(S.failed_strcoll, 1);
    }
    return diff;
}

static int
extension_cmp(const char *a, const char *b)
{
    const char *ea = strrchr(a, '.');
    const char *eb = strrchr(b, '.');
    int diff = name_coll(ea ? ea : "", eb ? eb : "");
    return diff ? diff : name_coll(a, b);
}

static int
timespec_cmp(struct timespec x, struct timespec y)
{
    if (x.tv_sec < y.tv_sec)
        return -1;
    if (x.tv_sec > y.tv_sec)
        return 1;
    if (x.tv_nsec < y.tv_nsec)
        return -1;
    if (x.tv_nsec > y.tv_nsec)
        return 1;
    return 0;
}

/* The full chain over comparator views. The dirs-first prefix is NEVER
   reversed (GNU applies it before the possibly-reversed comparator -
   pinned empirically: -r --group-directories-first keeps dirs first). */
static int
item_cmp(const struct liszt_item *a, const struct liszt_item *b)
{
    if (S.group_dirs) {
        int d = (int)b->group_dir - (int)a->group_dir;
        if (d != 0)
            return d;
    }

    /* GNU's rev_ comparator variants SWAP ARGUMENTS rather than negate:
       observable in the argument order of cannot-compare diagnostics
       (pinned via FreeBSD EILSEQ runs). The dirs-first prefix above uses
       the unswapped pair, exactly like DIRFIRST_CHECK. */
    if (S.reverse) {
        const struct liszt_item *t = a;
        a = b;
        b = t;
    }

    int diff;
    switch (S.word) {
    case LISZT_SORT_EXTENSION:
        diff = extension_cmp(a->name, b->name);
        break;
    case LISZT_SORT_VERSION:
        S.comparator_calls++;
        diff = filevercmp(a->name, b->name);
        if (diff == 0)
            diff = strcmp(a->name, b->name);
        break;
    case LISZT_SORT_SIZE:
        diff = b->size < a->size ? -1 : b->size > a->size ? 1 : 0;
        if (diff == 0)
            diff = name_coll(a->name, b->name);
        break;
    case LISZT_SORT_TIME:
        diff = timespec_cmp(b->mtime, a->mtime);
        if (diff == 0)
            diff = name_coll(a->name, b->name);
        break;
    case LISZT_SORT_NAME:
    default:
        diff = name_coll(a->name, b->name);
        break;
    }
    return diff;
}

/* Entries context for building items: meta is optional (zeros). */
static const struct liszt_entries *cur_es;

static void
make_item(const struct liszt_entry *e, struct liszt_item *out)
{
    const struct liszt_entrymeta *m =
        cur_es->meta ? &cur_es->meta[e->meta_idx] : NULL;
    out->name = liszt_entry_name(cur_es, e);
    out->size = m ? m->st.size : 0;
    out->mtime = m ? m->st.mtime : (struct timespec){ 0, 0 };
    out->group_dir = e->ftype == LISZT_T_DIR
        || (m && (S_ISDIR(m->st.mode) || S_ISDIR(m->linkmode)));
}

static int
entry_cmp(const struct liszt_entry *a, const struct liszt_entry *b)
{
    struct liszt_item ia, ib;
    make_item(a, &ia);
    make_item(b, &ib);
    return item_cmp(&ia, &ib);
}

/* gnulib mpsort, ported verbatim from the pinned tree: GNU's scalar
   sort. The exact comparison sequence matters - on BSD libc, strcoll can
   fail with EILSEQ, and byte parity requires failing on the same first
   pair GNU fails on. */
typedef int (*mp_cmp)(const void *, const void *);

static void mpsort_with_tmp(const void **base, size_t n, const void **tmp,
                            mp_cmp cmp);

static void
mpsort_into_tmp(const void **base, size_t n, const void **tmp, mp_cmp cmp)
{
    size_t n1 = n / 2;
    size_t n2 = n - n1;
    size_t a = 0;
    size_t alim = n1;
    size_t b = n1;
    size_t blim = n;

    mpsort_with_tmp(base + n1, n2, tmp, cmp);
    mpsort_with_tmp(base, n1, tmp, cmp);

    const void *ba = base[a];
    const void *bb = base[b];

    for (;;)
        if (cmp(ba, bb) <= 0) {
            *tmp++ = ba;
            a++;
            if (a == alim) {
                a = b;
                alim = blim;
                break;
            }
            ba = base[a];
        } else {
            *tmp++ = bb;
            b++;
            if (b == blim)
                break;
            bb = base[b];
        }

    memcpy(tmp, base + a, (alim - a) * sizeof *base);
}

static void
mpsort_with_tmp(const void **base, size_t n, const void **tmp, mp_cmp cmp)
{
    if (n <= 2) {
        if (n == 2) {
            const void *p0 = base[0];
            const void *p1 = base[1];
            if (!(cmp(p0, p1) <= 0)) {
                base[0] = p1;
                base[1] = p0;
            }
        }
        return;
    }

    size_t n1 = n / 2;
    size_t t = 0;
    size_t tlim = n1;
    size_t b = n1;
    size_t blim = n;

    mpsort_with_tmp(base + n1, n - n1, tmp, cmp);

    if (n1 < 2)
        tmp[0] = base[0];
    else
        mpsort_into_tmp(base, n1, tmp, cmp);

    const void *tt = tmp[t];
    const void *bb = base[b];

    for (size_t i = 0;;)
        if (cmp(tt, bb) <= 0) {
            base[i++] = tt;
            t++;
            if (t == tlim)
                break;
            tt = tmp[t];
        } else {
            base[i++] = bb;
            b++;
            if (b == blim) {
                memcpy(base + i, tmp + t, (tlim - t) * sizeof *base);
                break;
            }
            bb = base[b];
        }
}

static void
mpsort(const void **base, size_t n, mp_cmp cmp)
{
    mpsort_with_tmp(base, n, base + n, cmp);
}

static int
entry_ptr_cmp(const void *a, const void *b)
{
    return entry_cmp(a, b);
}

/* Scalar engine: mpsort over entry pointers, written back through aux. */
static void
scalar_mpsort_entries(struct liszt_entries *es, struct liszt_entry *aux)
{
    size_t n = es->len;
    const void **ptrs = liszt_xmalloc((n + n / 2) * sizeof *ptrs);
    for (size_t i = 0; i < n; i++)
        ptrs[i] = &es->v[i];
    mpsort(ptrs, n, entry_ptr_cmp);
    for (size_t i = 0; i < n; i++)
        aux[i] = *(const struct liszt_entry *)ptrs[i];
    memcpy(es->v, aux, n * sizeof *aux);
    free(ptrs);
}

/* Hard locales can hold names strcoll may reject (EILSEQ on BSD libc);
   any invalid multibyte name routes the directory to the scalar oracle
   so failure behavior matches GNU byte for byte. */
static bool
any_invalid_multibyte(const struct liszt_entries *es)
{
    for (size_t i = 0; i < es->len; i++) {
        const char *p = liszt_entry_name(es, &es->v[i]);
        size_t left = es->v[i].name_len;
        mbstate_t st;
        memset(&st, 0, sizeof st);
        while (left > 0) {
            size_t r = mbrtowc(NULL, p, left, &st);
            if (r == (size_t)-1 || r == (size_t)-2)
                return true;
            if (r == 0)
                r = 1;
            p += r;
            left -= r;
        }
    }
    return false;
}

/* --- byte radix over name spans (engine RADIX_BYTES) ------------------ */

static size_t
byte_bucket(const struct liszt_entries *es, const struct liszt_entry *e,
            size_t depth)
{
    if (depth >= e->name_len)
        return 0;
    return (size_t)(unsigned char)liszt_entry_name(es, e)[depth] + 1;
}

static int
bytes_cmp_from_depth(const struct liszt_entries *es,
                     const struct liszt_entry *a,
                     const struct liszt_entry *b, size_t depth)
{
    const unsigned char *pa = (const unsigned char *)liszt_entry_name(es, a);
    const unsigned char *pb = (const unsigned char *)liszt_entry_name(es, b);
    size_t la = a->name_len, lb = b->name_len;
    size_t n = (la < lb ? la : lb);
    for (size_t i = depth; i < n; i++) {
        if (pa[i] != pb[i]) {
            int d = pa[i] < pb[i] ? -1 : 1;
            return S.reverse ? -d : d;
        }
    }
    if (la == lb)
        return 0;
    int d = la < lb ? -1 : 1;
    return S.reverse ? -d : d;
}

static void
bytes_insertion(struct liszt_entries *es, struct liszt_entry *v, size_t lo,
                size_t hi, size_t depth)
{
    S.insertion_sorts++;
    for (size_t i = lo + 1; i < hi; i++) {
        struct liszt_entry e = v[i];
        size_t left = lo, right = i;
        while (left < right) {
            size_t mid = left + (right - left) / 2;
            if (bytes_cmp_from_depth(es, &e, &v[mid], depth) < 0)
                right = mid;
            else
                left = mid + 1;
        }
        memmove(v + left + 1, v + left, (i - left) * sizeof *v);
        v[left] = e;
    }
}

static void
bytes_radix_range(struct liszt_entries *es, struct liszt_entry *v,
                  struct liszt_entry *aux, size_t lo, size_t hi,
                  size_t depth)
{
    /* Skip the range's common prefix in one scan (rank's LCP jump). */
    for (;;) {
        size_t b0 = byte_bucket(es, &v[lo], depth);
        if (b0 == 0)
            break;
        bool common = true;
        for (size_t i = lo + 1; i < hi; i++) {
            if (byte_bucket(es, &v[i], depth) != b0) {
                common = false;
                break;
            }
        }
        if (!common)
            break;
        depth++;
        if (depth >= RADIX_DEPTH_LIMIT)
            break;
    }

    if (hi - lo <= RADIX_INSERTION_THRESHOLD || depth >= RADIX_DEPTH_LIMIT) {
        bytes_insertion(es, v, lo, hi, depth);
        return;
    }

    size_t counts[RADIX_BUCKETS] = { 0 };
    size_t starts[RADIX_BUCKETS];
    size_t next[RADIX_BUCKETS];
    size_t sum = lo;

    S.passes++;
    S.classified += hi - lo;
    for (size_t i = lo; i < hi; i++)
        counts[byte_bucket(es, &v[i], depth)]++;
    if (S.reverse) {
        for (size_t b = RADIX_BUCKETS; b > 0; b--) {
            starts[b - 1] = sum;
            next[b - 1] = sum;
            sum += counts[b - 1];
        }
    } else {
        for (size_t b = 0; b < RADIX_BUCKETS; b++) {
            starts[b] = sum;
            next[b] = sum;
            sum += counts[b];
        }
    }
    for (size_t i = lo; i < hi; i++)
        aux[next[byte_bucket(es, &v[i], depth)]++] = v[i];
    memcpy(v + lo, aux + lo, (hi - lo) * sizeof *v);

    for (size_t b = 1; b < RADIX_BUCKETS; b++) {
        size_t start = starts[b];
        size_t end = start + counts[b];
        if (end - start > 1)
            bytes_radix_range(es, v, aux, start, end, depth + 1);
    }
}

/* --- transformed radix (engine RADIX_TRANSFORMED) --------------------- */

/* Sort records pairing the entry with its packed strxfrm span. */
struct xrec {
    struct liszt_entry e;
    uint32_t xoff;
    uint32_t xlen;
};

static const unsigned char *
xrec_key(const struct xrec *r)
{
    return S.xarena + r->xoff;
}

static size_t
x_bucket(const struct xrec *r, size_t depth)
{
    if (depth >= r->xlen)
        return 0;
    return (size_t)xrec_key(r)[depth] + 1;
}

static int
x_cmp_from_depth(const struct xrec *a, const struct xrec *b, size_t depth)
{
    const unsigned char *pa = xrec_key(a);
    const unsigned char *pb = xrec_key(b);
    size_t n = (a->xlen < b->xlen ? a->xlen : b->xlen);
    for (size_t i = depth; i < n; i++) {
        if (pa[i] != pb[i]) {
            int d = pa[i] < pb[i] ? -1 : 1;
            return S.reverse ? -d : d;
        }
    }
    if (a->xlen == b->xlen)
        return 0;
    int d = a->xlen < b->xlen ? -1 : 1;
    return S.reverse ? -d : d;
}

static void
x_insertion(struct xrec *v, size_t lo, size_t hi, size_t depth)
{
    S.insertion_sorts++;
    for (size_t i = lo + 1; i < hi; i++) {
        struct xrec e = v[i];
        size_t left = lo, right = i;
        while (left < right) {
            size_t mid = left + (right - left) / 2;
            if (x_cmp_from_depth(&e, &v[mid], depth) < 0)
                right = mid;
            else
                left = mid + 1;
        }
        memmove(v + left + 1, v + left, (i - left) * sizeof *v);
        v[left] = e;
    }
}

static void
x_radix_range(struct xrec *v, struct xrec *aux, size_t lo, size_t hi,
              size_t depth)
{
    for (;;) {
        size_t b0 = x_bucket(&v[lo], depth);
        if (b0 == 0)
            break;
        bool common = true;
        for (size_t i = lo + 1; i < hi; i++) {
            if (x_bucket(&v[i], depth) != b0) {
                common = false;
                break;
            }
        }
        if (!common)
            break;
        depth++;
        if (depth >= RADIX_DEPTH_LIMIT)
            break;
    }

    if (hi - lo <= RADIX_INSERTION_THRESHOLD || depth >= RADIX_DEPTH_LIMIT) {
        x_insertion(v, lo, hi, depth);
        return;
    }

    size_t counts[RADIX_BUCKETS] = { 0 };
    size_t starts[RADIX_BUCKETS];
    size_t next[RADIX_BUCKETS];
    size_t sum = lo;

    S.passes++;
    S.classified += hi - lo;
    for (size_t i = lo; i < hi; i++)
        counts[x_bucket(&v[i], depth)]++;
    if (S.reverse) {
        for (size_t b = RADIX_BUCKETS; b > 0; b--) {
            starts[b - 1] = sum;
            next[b - 1] = sum;
            sum += counts[b - 1];
        }
    } else {
        for (size_t b = 0; b < RADIX_BUCKETS; b++) {
            starts[b] = sum;
            next[b] = sum;
            sum += counts[b];
        }
    }
    for (size_t i = lo; i < hi; i++)
        aux[next[x_bucket(&v[i], depth)]++] = v[i];
    memcpy(v + lo, aux + lo, (hi - lo) * sizeof *v);

    for (size_t b = 1; b < RADIX_BUCKETS; b++) {
        size_t start = starts[b];
        size_t end = start + counts[b];
        if (end - start > 1)
            x_radix_range(v, aux, start, end, depth + 1);
    }
}

/* Transform every name once into the packed arena. Returns false when a
   transform cannot be represented (paranoia; strxfrm does not report
   errors) - caller falls back to scalar. */
static bool
build_transforms(struct liszt_entries *es, struct xrec *recs)
{
    S.xarena_len = 0;
    for (size_t i = 0; i < es->len; i++) {
        const char *name = liszt_entry_name(es, &es->v[i]);
        size_t need = strxfrm(NULL, name, 0);
        if (need == (size_t)-1)
            return false;
        if (S.xarena_len + need + 1 > S.xarena_cap) {
            size_t cap = S.xarena_cap ? S.xarena_cap : 64 * 1024;
            while (cap < S.xarena_len + need + 1)
                cap += cap / 2;
            S.xarena = liszt_xrealloc(S.xarena, cap);
            S.xarena_cap = cap;
        }
        strxfrm((char *)S.xarena + S.xarena_len, name, need + 1);
        recs[i].e = es->v[i];
        recs[i].xoff = (uint32_t)S.xarena_len;
        recs[i].xlen = (uint32_t)need;
        S.xarena_len += need + 1;
        if (S.xarena_len > UINT32_MAX)
            return false;
    }
    return true;
}

/* --- numeric key radix (engine RADIX_NUMERIC, -S/-t) ------------------ */

/* Descending numeric primary via complemented biased keys: ascending
   radix machinery then yields largest/newest first, S.reverse flips
   buckets exactly like the name engines. Ties fall to the name order
   within equal-key groups. */
struct nrec {
    struct liszt_entry e;
    uint64_t k1;
    uint32_t k2;
};

static unsigned char
nrec_byte(const struct nrec *r, size_t level)
{
    if (level < 8)
        return (unsigned char)(r->k1 >> (56 - 8 * level));
    return (unsigned char)(r->k2 >> (24 - 8 * (level - 8)));
}

static int
nrec_key_cmp(const struct nrec *a, const struct nrec *b)
{
    if (a->k1 != b->k1)
        return a->k1 < b->k1 ? -1 : 1;
    if (a->k2 != b->k2)
        return a->k2 < b->k2 ? -1 : 1;
    return 0;
}

static void
nrec_insertion(struct nrec *v, size_t lo, size_t hi)
{
    S.insertion_sorts++;
    for (size_t i = lo + 1; i < hi; i++) {
        struct nrec e = v[i];
        size_t j = i;
        int rev = S.reverse ? -1 : 1;
        while (j > lo && rev * nrec_key_cmp(&e, &v[j - 1]) < 0) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = e;
    }
}

static void
nrec_radix(struct nrec *v, struct nrec *aux, size_t lo, size_t hi,
           size_t level)
{
    if (level >= 12)
        return;
    if (hi - lo <= RADIX_INSERTION_THRESHOLD) {
        nrec_insertion(v, lo, hi);
        return;
    }

    size_t counts[256] = { 0 };
    size_t starts[256];
    size_t next[256];
    size_t sum = lo;

    S.passes++;
    S.classified += hi - lo;
    for (size_t i = lo; i < hi; i++)
        counts[nrec_byte(&v[i], level)]++;
    if (S.reverse) {
        for (size_t b = 256; b > 0; b--) {
            starts[b - 1] = sum;
            next[b - 1] = sum;
            sum += counts[b - 1];
        }
    } else {
        for (size_t b = 0; b < 256; b++) {
            starts[b] = sum;
            next[b] = sum;
            sum += counts[b];
        }
    }
    for (size_t i = lo; i < hi; i++)
        aux[next[nrec_byte(&v[i], level)]++] = v[i];
    memcpy(v + lo, aux + lo, (hi - lo) * sizeof *v);

    for (size_t b = 0; b < 256; b++) {
        size_t start = starts[b];
        size_t end = start + counts[b];
        if (end - start > 1)
            nrec_radix(v, aux, start, end, level + 1);
    }
}

static uint64_t
bias64(int64_t v)
{
    return (uint64_t)v + 0x8000000000000000ull;
}

/* Sort [lo,hi) of es->v by numeric key desc, name-tie per the active
   locale (bytes radix under identity, scalar merge per group in hard
   locales - a 09 ledger candidate if measured hot). */
static void
numeric_sort_range(struct liszt_entries *es, struct liszt_entry *eaux,
                   size_t lo, size_t hi)
{
    size_t n = hi - lo;
    struct nrec *recs = liszt_xmalloc(n * sizeof *recs);
    struct nrec *aux = liszt_xmalloc(n * sizeof *aux);

    for (size_t i = 0; i < n; i++) {
        const struct liszt_entry *e = &es->v[lo + i];
        const struct liszt_entrymeta *m = &es->meta[e->meta_idx];
        recs[i].e = *e;
        if (S.word == LISZT_SORT_SIZE) {
            recs[i].k1 = ~bias64((int64_t)m->st.size);
            recs[i].k2 = 0;
        } else {
            recs[i].k1 = ~bias64((int64_t)m->st.mtime.tv_sec);
            recs[i].k2 = ~(uint32_t)m->st.mtime.tv_nsec;
        }
    }
    nrec_radix(recs, aux, 0, n, 0);
    for (size_t i = 0; i < n; i++)
        es->v[lo + i] = recs[i].e;

    /* Name-sort equal-key runs. */
    size_t run = 0;
    for (size_t i = 1; i <= n; i++) {
        if (i == n || nrec_key_cmp(&recs[run], &recs[i]) != 0) {
            if (i - run > 1) {
                if (liszt_locale_collation_identity()) {
                    bytes_radix_range(es, es->v, eaux, lo + run, lo + i,
                                      0);
                } else {
                    size_t gn = i - run;
                    const void **ptrs =
                        liszt_xmalloc((gn + gn / 2) * sizeof *ptrs);
                    for (size_t g = 0; g < gn; g++)
                        ptrs[g] = &es->v[lo + run + g];
                    mpsort(ptrs, gn, entry_ptr_cmp);
                    for (size_t g = 0; g < gn; g++)
                        eaux[g] = *(const struct liszt_entry *)ptrs[g];
                    memcpy(es->v + lo + run, eaux, gn * sizeof *eaux);
                    free(ptrs);
                }
            }
            run = i;
        }
    }
    free(aux);
    free(recs);
}

/* --- verification oracle ---------------------------------------------- */

static void
verify_sorted(struct liszt_entries *es)
{
    (void)es;
    S.in_verify = true;
    S.verify_skipped = false;
    for (size_t i = 1; i < cur_es->len; i++) {
        if (S.verify_skipped)
            break;
        if (entry_cmp(&cur_es->v[i - 1], &cur_es->v[i]) > 0) {
            fprintf(stderr,
                    "%s: internal sort verification failed at record %zu\n",
                    liszt_prog, i);
            exit(LISZT_STATUS_SERIOUS);
        }
    }
    S.in_verify = false;
}

/* --- entry points ------------------------------------------------------ */

void
liszt_sort_init(const struct liszt_options *o, const struct liszt_plan *p)
{
    S.word = o->sort;
    S.reverse = o->reverse;
    S.group_dirs = o->group_directories_first
        && o->sort != LISZT_SORT_NONE;
    S.engine = p->sort_engine;
    S.use_strcmp = false;
}

void
liszt_sort_entries(struct liszt_entries *es)
{
    if (es->len < 2 || S.engine == LISZT_PLAN_SORT_NONE)
        return;

    /* GNU's failed-strcoll path re-initializes the ordering vector, then
       redoes the whole sort with strcmp. Snapshot the pre-sort order so
       the fallback starts from the same baseline (stability for
       duplicate operand names). All comparator calls - engines, scalar,
       verify - land in this one setjmp. */
    struct liszt_entry *snapshot = liszt_xmalloc(es->len * sizeof *snapshot);
    memcpy(snapshot, es->v, es->len * sizeof *snapshot);
    struct liszt_entry *aux = liszt_xmalloc(es->len * sizeof *aux);

    cur_es = es;

    /* Grouping is a stable partition (dirs, then non-dirs; never flipped
       by -r), each side sorted independently by the active engine. */
    size_t split = 0;
    if (S.group_dirs) {
        struct liszt_item it;
        size_t k = 0;
        for (size_t i = 0; i < es->len; i++) {
            make_item(&es->v[i], &it);
            if (it.group_dir)
                aux[k++] = es->v[i];
        }
        split = k;
        for (size_t i = 0; i < es->len; i++) {
            make_item(&es->v[i], &it);
            if (!it.group_dir)
                aux[k++] = es->v[i];
        }
        memcpy(es->v, aux, es->len * sizeof *aux);
    }

    /* Per-directory engine downgrade: unrepresentable names fall back
       to the scalar oracle in hard locales. */
    enum liszt_sort_plan engine = S.engine;
    if (engine != LISZT_PLAN_SORT_SCALAR && engine != LISZT_PLAN_SORT_NONE
        && !liszt_locale_collation_identity()
        && any_invalid_multibyte(es))
        engine = LISZT_PLAN_SORT_SCALAR;

    bool engine_ran = false;
    if (setjmp(S.failed_strcoll) != 0) {
        S.use_strcmp = true;
        memcpy(es->v, snapshot, es->len * sizeof *snapshot);
        scalar_mpsort_entries(es, aux);
    } else {
        size_t los[2] = { 0, split };
        size_t his[2] = { split, es->len };
        int nranges = S.group_dirs ? 2 : 1;
        if (!S.group_dirs)
            his[0] = es->len;

        switch (engine) {
        case LISZT_PLAN_SORT_RADIX_BYTES:
            for (int r = 0; r < nranges; r++)
                bytes_radix_range(es, es->v, aux, los[r], his[r], 0);
            engine_ran = true;
            break;
        case LISZT_PLAN_SORT_RADIX_TRANSFORMED: {
            struct xrec *recs = liszt_xmalloc(es->len * sizeof *recs);
            if (!build_transforms(es, recs)) {
                free(recs);
                scalar_mpsort_entries(es, aux);
                break;
            }
            struct xrec *xaux = liszt_xmalloc(es->len * sizeof *xaux);
            for (int r = 0; r < nranges; r++)
                x_radix_range(recs, xaux, los[r], his[r], 0);
            for (size_t i = 0; i < es->len; i++)
                es->v[i] = recs[i].e;
            free(xaux);
            free(recs);
            engine_ran = true;
            break;
        }
        case LISZT_PLAN_SORT_RADIX_NUMERIC:
            for (int r = 0; r < nranges; r++)
                numeric_sort_range(es, aux, los[r], his[r]);
            engine_ran = true;
            break;
        case LISZT_PLAN_SORT_SCALAR:
        default:
            scalar_mpsort_entries(es, aux);
            break;
        }

        if (engine_ran && getenv("LISZT_DEBUG_VERIFY") != NULL)
            verify_sorted(es);
    }

    free(aux);
    free(snapshot);

    if (getenv("LISZT_DEBUG_STATS") != NULL)
        fprintf(stderr,
                "%s: stats passes=%zu classified=%zu insertion_sorts=%zu"
                " comparator_calls=%zu\n",
                liszt_prog, S.passes, S.classified, S.insertion_sorts,
                S.comparator_calls);
}

static void (*op_get_item)(const void *, struct liszt_item *);

static int
op_ptr_cmp(const void *a, const void *b)
{
    struct liszt_item ia, ib;
    op_get_item(a, &ia);
    op_get_item(b, &ib);
    return item_cmp(&ia, &ib);
}

/* Stable, setjmp-protected sort for the small command-line operand
   array; elements are opaque, the comparator view comes from GET_ITEM.
   mpsort keeps GNU's comparison sequence for operands too. */
void
liszt_sort_operands(void *base, size_t n, size_t size,
                    void (*get_item)(const void *, struct liszt_item *))
{
    if (n < 2)
        return;
    char *v = base;
    char *snapshot = liszt_xmalloc(n * size);
    char *tmp = liszt_xmalloc(n * size);
    memcpy(snapshot, v, n * size);

    op_get_item = get_item;
    const void **ptrs = liszt_xmalloc((n + n / 2) * sizeof *ptrs);

    if (setjmp(S.failed_strcoll) != 0) {
        S.use_strcmp = true;
        memcpy(v, snapshot, n * size);
    }
    for (size_t i = 0; i < n; i++)
        ptrs[i] = v + i * size;
    mpsort(ptrs, n, op_ptr_cmp);
    for (size_t i = 0; i < n; i++)
        memcpy(tmp + i * size, ptrs[i], size);
    memcpy(v, tmp, n * size);

    free(ptrs);
    free(tmp);
    free(snapshot);
}

#include "sortkey.h"

#include <errno.h>
#include <locale.h>
#include <setjmp.h>
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
    enum liszt_sort_plan engine;
    bool use_strcmp;        /* strcoll failed; GNU falls back wholesale */
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
        liszt_error(errno, "cannot compare file names %s%s%s and %s%s%s",
                    liszt_qL(), a, liszt_qR(), liszt_qL(), b, liszt_qR());
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
chain_cmp(const char *a, const char *b)
{
    int diff;

    switch (S.word) {
    case LISZT_SORT_EXTENSION:
        diff = extension_cmp(a, b);
        break;
    case LISZT_SORT_VERSION:
        S.comparator_calls++;
        diff = filevercmp(a, b);
        if (diff == 0)
            diff = strcmp(a, b);
        break;
    case LISZT_SORT_NAME:
    default:
        diff = name_coll(a, b);
        break;
    }
    return S.reverse ? -diff : diff;
}

int
liszt_sort_cmp_names(const char *a, const char *b)
{
    return chain_cmp(a, b);
}

/* Stable top-down merge sort - the scalar engine (GNU uses mpsort, also
   a stable merge; ties are byte-identical names, so order matches). */
static void
merge_range(struct liszt_entries *es, struct liszt_entry *v,
            struct liszt_entry *aux, size_t lo, size_t hi)
{
    if (hi - lo < 2)
        return;
    size_t mid = lo + (hi - lo) / 2;
    merge_range(es, v, aux, lo, mid);
    merge_range(es, v, aux, mid, hi);
    memcpy(aux + lo, v + lo, (hi - lo) * sizeof *v);
    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (chain_cmp(liszt_entry_name(es, &aux[j]),
                      liszt_entry_name(es, &aux[i])) < 0)
            v[k++] = aux[j++];
        else
            v[k++] = aux[i++];
    }
    while (i < mid)
        v[k++] = aux[i++];
    while (j < hi)
        v[k++] = aux[j++];
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

/* --- verification oracle ---------------------------------------------- */

static void
verify_sorted(struct liszt_entries *es)
{
    for (size_t i = 1; i < es->len; i++) {
        if (chain_cmp(liszt_entry_name(es, &es->v[i - 1]),
                      liszt_entry_name(es, &es->v[i])) > 0) {
            fprintf(stderr,
                    "%s: internal sort verification failed at record %zu\n",
                    liszt_prog, i);
            exit(LISZT_STATUS_SERIOUS);
        }
    }
}

/* --- entry points ------------------------------------------------------ */

void
liszt_sort_init(const struct liszt_options *o, const struct liszt_plan *p)
{
    S.word = o->sort;
    S.reverse = o->reverse;
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

    bool engine_ran = false;
    if (setjmp(S.failed_strcoll) != 0) {
        S.use_strcmp = true;
        memcpy(es->v, snapshot, es->len * sizeof *snapshot);
        merge_range(es, es->v, aux, 0, es->len);
    } else {
        switch (S.engine) {
        case LISZT_PLAN_SORT_RADIX_BYTES:
            bytes_radix_range(es, es->v, aux, 0, es->len, 0);
            engine_ran = true;
            break;
        case LISZT_PLAN_SORT_RADIX_TRANSFORMED: {
            struct xrec *recs = liszt_xmalloc(es->len * sizeof *recs);
            if (!build_transforms(es, recs)) {
                free(recs);
                merge_range(es, es->v, aux, 0, es->len);
                break;
            }
            struct xrec *xaux = liszt_xmalloc(es->len * sizeof *xaux);
            x_radix_range(recs, xaux, 0, es->len, 0);
            for (size_t i = 0; i < es->len; i++)
                es->v[i] = recs[i].e;
            free(xaux);
            free(recs);
            engine_ran = true;
            break;
        }
        case LISZT_PLAN_SORT_SCALAR:
        default:
            merge_range(es, es->v, aux, 0, es->len);
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

/* Stable, setjmp-protected sort for the small command-line operand
   array; elements are opaque, names come through GET_NAME. */
void
liszt_sort_operands(void *base, size_t n, size_t size,
                    const char *(*get_name)(const void *))
{
    if (n < 2)
        return;
    char *v = base;
    char *snapshot = liszt_xmalloc(n * size);
    char *tmp = liszt_xmalloc(size);
    memcpy(snapshot, v, n * size);

    if (setjmp(S.failed_strcoll) != 0) {
        S.use_strcmp = true;
        memcpy(v, snapshot, n * size);
    }
    for (size_t i = 1; i < n; i++) {
        memcpy(tmp, v + i * size, size);
        size_t j = i;
        while (j > 0
               && chain_cmp(get_name(tmp), get_name(v + (j - 1) * size))
                  < 0) {
            memcpy(v + j * size, v + (j - 1) * size, size);
            j--;
        }
        memcpy(v + j * size, tmp, size);
    }
    free(tmp);
    free(snapshot);
}

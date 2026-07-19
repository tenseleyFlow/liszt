#include "git.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

/* On-disk flag bits (index format). */
#define CE_STAGEMASK 0x3000u
#define CE_EXTENDED  0x4000u
#define CE_VALID     0x8000u    /* assume-unchanged */
#define CE_NAMEMASK  0x0FFFu
#define CE_SKIP_WORKTREE 0x4000u    /* in extended flags */
#define CE_INTENT_TO_ADD 0x2000u

#define S_IFGITLINK 0160000u

static bool debug_git;
static bool debug_git_init;
static _Atomic unsigned long racy_seen;

static bool
dbg(void)
{
    if (!debug_git_init) {
        const char *e = getenv("LISZT_DEBUG_GIT");
        debug_git = e != NULL && strcmp(e, "1") == 0;
        debug_git_init = true;
    }
    return debug_git;
}

static void
degrade(struct liszt_git_ctx *c, const char *why)
{
    c->degraded = true;
    if (dbg())
        fprintf(stderr, "liszt: git: %s: degraded (%s)\n",
                c->root ? c->root : "?", why);
}

static uint32_t
be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
        | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t
be16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* --- config sniff ------------------------------------------------------

   A line scanner, deliberately not a config parser: track the current
   [section] and read the two keys the cheap tier observes. Values are
   taken to the first comment char, trimmed. */

static void
sniff_config(struct liszt_git_ctx *c, const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[512];
    enum { SEC_NONE, SEC_CORE, SEC_EXT } sec = SEC_NONE;

    c->filemode = true;
    if (fp == NULL)
        return;
    while (fgets(line, sizeof line, fp) != NULL) {
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s == '[') {
            if (strncmp(s, "[core]", 6) == 0)
                sec = SEC_CORE;
            else if (strncmp(s, "[extensions]", 12) == 0)
                sec = SEC_EXT;
            else
                sec = SEC_NONE;
            continue;
        }
        char *eq = strchr(s, '=');
        if (eq == NULL || sec == SEC_NONE)
            continue;
        char *key = s;
        size_t klen = (size_t)(eq - s);
        while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t'))
            klen--;
        char *val = eq + 1;
        while (*val == ' ' || *val == '\t')
            val++;
        size_t vlen = strcspn(val, " \t\r\n;#");
        if (sec == SEC_CORE && klen == 8
            && strncasecmp(key, "filemode", 8) == 0)
            c->filemode = !(vlen == 5
                            && strncasecmp(val, "false", 5) == 0);
        else if (sec == SEC_EXT && klen == 12
                 && strncasecmp(key, "objectformat", 12) == 0)
            c->sha256 = vlen == 6 && strncasecmp(val, "sha256", 6) == 0;
    }
    fclose(fp);
}

/* --- index parse ------------------------------------------------------- */

static uint64_t
decode_varint(const unsigned char **bufp, const unsigned char *end)
{
    const unsigned char *buf = *bufp;
    if (buf >= end) {
        *bufp = NULL;
        return 0;
    }
    unsigned char ch = *buf++;
    uint64_t val = ch & 127u;
    while (ch & 128u) {
        val += 1;
        if (buf >= end || val > (UINT64_MAX >> 7)) {
            *bufp = NULL;
            return 0;
        }
        ch = *buf++;
        val = (val << 7) + (ch & 127u);
    }
    *bufp = buf;
    return val;
}

bool
liszt_git_index_parse(struct liszt_git_ctx *c)
{
    const unsigned char *img = c->image;
    size_t len = c->image_len;
    size_t hashlen = c->sha256 ? 32 : 20;
    size_t oidlen = hashlen;

    if (len < 12 + hashlen || memcmp(img, "DIRC", 4) != 0) {
        degrade(c, "not an index");
        return false;
    }
    uint32_t version = be32(img + 4);
    uint32_t n = be32(img + 8);
    if (version < 2 || version > 4) {
        degrade(c, "unsupported version");
        return false;
    }
    size_t limit = len - hashlen;
    /* Entry floor: 40 stat bytes + oid + flags. Guards n against
       overflow before the allocation. */
    if (n > (limit - 12) / (40 + oidlen + 2) + 1) {
        degrade(c, "entry count exceeds file");
        return false;
    }

    c->ents = liszt_xmalloc((n ? n : 1) * sizeof *c->ents);
    size_t arena_cap = 0;
    char *arena = NULL;
    size_t arena_used = 0;
    if (version == 4) {
        arena_cap = limit + (size_t)n;   /* upper bound on expansions */
        arena = liszt_xmalloc(arena_cap ? arena_cap : 1);
    }

    size_t pos = 12;
    const char *prev = "";
    size_t prev_len = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (pos + 40 + oidlen + 2 > limit)
            goto corrupt;
        struct liszt_git_ientry *e = &c->ents[i];
        e->off = (uint32_t)pos;
        e->flags = be16(img + pos + 40 + oidlen);
        e->xflags = 0;
        size_t p = pos + 40 + oidlen + 2;
        if (e->flags & CE_EXTENDED) {
            if (version < 3 || p + 2 > limit)
                goto corrupt;
            e->xflags = be16(img + p);
            p += 2;
        }
        if (version <= 3) {
            size_t namelen = e->flags & CE_NAMEMASK;
            if (namelen == CE_NAMEMASK) {
                const unsigned char *nul =
                    memchr(img + p, 0, limit - p);
                if (nul == NULL)
                    goto corrupt;
                namelen = (size_t)(nul - (img + p));
            } else if (p + namelen >= limit
                       || img[p + namelen] != 0) {
                goto corrupt;
            }
            e->path = (const char *)img + p;
            e->len = (uint32_t)namelen;
            /* Pad to 8 from the entry start, at least one NUL. */
            size_t consumed = (p - pos) + namelen + 1;
            consumed = (consumed + 7) & ~(size_t)7;
            pos += consumed;
            if (pos > limit)
                goto corrupt;
        } else {
            const unsigned char *vp = img + p;
            uint64_t strip = decode_varint(&vp, img + limit);
            if (vp == NULL || strip > prev_len)
                goto corrupt;
            p = (size_t)(vp - img);
            const unsigned char *nul = memchr(img + p, 0, limit - p);
            if (nul == NULL)
                goto corrupt;
            size_t suflen = (size_t)(nul - (img + p));
            size_t keep = prev_len - (size_t)strip;
            size_t namelen = keep + suflen;
            if (arena_used + namelen + 1 > arena_cap)
                goto corrupt;
            char *dst = arena + arena_used;
            memcpy(dst, prev, keep);
            memcpy(dst + keep, img + p, suflen + 1);
            e->path = dst;
            e->len = (uint32_t)namelen;
            arena_used += namelen + 1;
            pos = p + suflen + 1;
        }
        /* The sorted-order guarantee is the binary search's ground;
           verify instead of trusting (stage is the tiebreak). */
        if (i > 0) {
            size_t pl = c->ents[i - 1].len;
            size_t min = pl < e->len ? pl : e->len;
            int cmp = memcmp(c->ents[i - 1].path, e->path, min);
            if (cmp == 0)
                cmp = pl < e->len ? -1 : pl > e->len ? 1 : 0;
            if (cmp > 0)
                goto corrupt;
            if (cmp == 0
                && (c->ents[i - 1].flags & CE_STAGEMASK)
                       > (e->flags & CE_STAGEMASK))
                goto corrupt;
        }
        prev = e->path;
        prev_len = e->len;
    }
    c->n_ents = n;
    c->v4_arena = arena;

    /* Extensions: skip optional ones (uppercase signature); any
       required (lowercase) extension - split index "link", sparse
       "sdir", or unknown - degrades the repo. */
    while (pos + 8 <= limit) {
        const unsigned char *sig = img + pos;
        uint32_t extsize = be32(img + pos + 4);
        if (sig[0] >= 'a' && sig[0] <= 'z') {
            degrade(c, memcmp(sig, "link", 4) == 0 ? "split index"
                    : memcmp(sig, "sdir", 4) == 0 ? "sparse index"
                                                  : "required extension");
            return false;
        }
        if (extsize > limit - pos - 8)
            goto corrupt;
        pos += 8 + extsize;
    }
    if (pos != limit)
        goto corrupt;
    return true;

corrupt:
    free(c->ents);
    c->ents = NULL;
    c->n_ents = 0;
    free(arena);
    c->v4_arena = NULL;
    degrade(c, "malformed index");
    return false;
}

/* --- status compare ---------------------------------------------------- */

char
liszt_git_compare(const struct liszt_git_ctx *c,
                  const struct liszt_git_ientry *e,
                  const struct liszt_git_wtstat *ws)
{
    const unsigned char *ent = c->image + e->off;
    uint32_t imode = be32(ent + 24);
    uint32_t ifmt = imode & 0170000u;

    if (e->flags & CE_STAGEMASK)
        return LISZT_GIT_WT_CONFLICT;
    if (e->xflags & CE_INTENT_TO_ADD)
        return LISZT_GIT_WT_UNTRACKED;
    if ((e->flags & CE_VALID) || (e->xflags & CE_SKIP_WORKTREE))
        return LISZT_GIT_WT_CLEAN;
    if (ifmt == S_IFGITLINK)
        return LISZT_GIT_WT_CLEAN;      /* never recursed */
    if (ws == NULL)
        return LISZT_GIT_WT_CLEAN;

    uint32_t wfmt = ws->mode & S_IFMT;
    if (ifmt == 0120000u) {
        if (!S_ISLNK(ws->mode))
            return LISZT_GIT_WT_TYPECHANGE;
    } else {
        if (!S_ISREG(ws->mode))
            return LISZT_GIT_WT_TYPECHANGE;
        (void)wfmt;
        if (c->filemode
            && ((imode & 0100u) != 0) != ((ws->mode & 0100u) != 0))
            return LISZT_GIT_WT_MODIFIED;
    }
    /* Size: git's own low-32 truncation. */
    if (be32(ent + 36) != (uint32_t)ws->size)
        return LISZT_GIT_WT_MODIFIED;
    /* mtime seconds (32-bit wrap, git-compatible); nanoseconds only
       when both sides have them - a zero-nsec index tolerates any. */
    uint32_t isec = be32(ent + 8);
    uint32_t insec = be32(ent + 12);
    if (isec != (uint32_t)ws->mtime_sec)
        return LISZT_GIT_WT_MODIFIED;
    if (insec != 0 && ws->mtime_nsec != 0
        && insec != (uint32_t)ws->mtime_nsec)
        return LISZT_GIT_WT_MODIFIED;
    /* Stat-equal wins. Racily-clean entries (index not younger than
       the entry's mtime) are counted for the debug channel; git's
       write-side smudge keeps truly-racy ones stat-different. */
    if ((int64_t)isec >= c->index_mtime)
        racy_seen++;
    return LISZT_GIT_WT_CLEAN;
}

/* --- per-directory window ---------------------------------------------- */

/* First index position whose path is >= KEY (prefix-length compare). */
static size_t
lower_bound(const struct liszt_git_ctx *c, const char *key, size_t klen)
{
    size_t lo = 0, hi = c->n_ents;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const struct liszt_git_ientry *e = &c->ents[mid];
        size_t min = e->len < klen ? e->len : klen;
        int cmp = memcmp(e->path, key, min);
        if (cmp == 0)
            cmp = e->len < klen ? -1 : e->len > klen ? 1 : 0;
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static uint32_t
fnv1a(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

/* Window slot values: 0 empty, UINT32_MAX tracked-dir marker base -
   dirs store UINT32_MAX - (first entry idx); files store idx + 1. */
#define WIN_EMPTY 0u

static void
win_insert(struct liszt_git_ctx *c, const char *name, size_t len,
           uint32_t val)
{
    uint32_t mask = (uint32_t)c->win_cap - 1;
    uint32_t h = fnv1a(name, len) & mask;
    for (;;) {
        uint32_t slot = c->win_tab[h];
        if (slot == WIN_EMPTY) {
            c->win_tab[h] = val;
            return;
        }
        /* First insertion wins: stage-0 entries come first in index
           order, and a conflict marker set below sticks. */
        uint32_t idx = slot >= UINT32_MAX / 2
            ? UINT32_MAX - slot : slot - 1;
        const struct liszt_git_ientry *e = &c->ents[idx];
        const char *en = e->path + c->win_plen;
        const char *sl = memchr(en, '/', e->len - c->win_plen);
        size_t elen = sl ? (size_t)(sl - en)
                         : e->len - c->win_plen;
        if (elen == len && memcmp(en, name, len) == 0)
            return;
        h = (h + 1) & mask;
    }
}

static uint32_t
win_find(const struct liszt_git_ctx *c, const char *name, size_t len)
{
    if (c->win_cap == 0)
        return WIN_EMPTY;
    uint32_t mask = (uint32_t)c->win_cap - 1;
    uint32_t h = fnv1a(name, len) & mask;
    for (;;) {
        uint32_t slot = c->win_tab[h];
        if (slot == WIN_EMPTY)
            return WIN_EMPTY;
        uint32_t idx = slot >= UINT32_MAX / 2
            ? UINT32_MAX - slot : slot - 1;
        const struct liszt_git_ientry *e = &c->ents[idx];
        const char *en = e->path + c->win_plen;
        const char *sl = memchr(en, '/', e->len - c->win_plen);
        size_t elen = sl ? (size_t)(sl - en)
                         : e->len - c->win_plen;
        if (elen == len && memcmp(en, name, len) == 0)
            return slot;
        h = (h + 1) & mask;
    }
}

void
liszt_git_window(struct liszt_git_ctx *c, const char *abs)
{
    size_t alen = strlen(abs);

    c->win_lo = c->win_hi = 0;
    c->win_plen = 0;
    if (c->degraded || c->n_ents == 0)
        return;

    /* Repo-relative prefix with trailing slash ("" at the root). */
    char pfx[4096] = "";
    size_t plen = 0;
    if (alen > c->root_len) {
        plen = alen - c->root_len - 1 + 1;      /* skip '/', add '/' */
        if (plen >= sizeof pfx)
            return;
        memcpy(pfx, abs + c->root_len + 1, plen - 1);
        pfx[plen - 1] = '/';
    }
    c->win_plen = plen;

    size_t lo = lower_bound(c, pfx, plen);
    size_t hi;
    if (plen == 0) {
        hi = c->n_ents;
    } else {
        char upper[4096];
        memcpy(upper, pfx, plen);
        upper[plen - 1]++;
        hi = lower_bound(c, upper, plen);
    }
    /* Entries at lo may share only a shorter prefix; the span check
       is the memcmp below during the build. */
    c->win_lo = lo;
    c->win_hi = hi;

    size_t span = hi - lo;
    size_t want = 16;
    while (want < span * 2)
        want *= 2;
    if (want > c->win_cap) {
        free(c->win_tab);
        c->win_tab = liszt_xmalloc(want * sizeof *c->win_tab);
        c->win_cap = want;
    }
    memset(c->win_tab, 0, c->win_cap * sizeof *c->win_tab);

    for (size_t i = lo; i < hi; i++) {
        const struct liszt_git_ientry *e = &c->ents[i];
        if (e->len <= plen || memcmp(e->path, pfx, plen) != 0)
            continue;
        const char *name = e->path + plen;
        size_t nrest = e->len - plen;
        const char *sl = memchr(name, '/', nrest);
        if (sl == NULL)
            win_insert(c, name, nrest, (uint32_t)i + 1);
        else
            win_insert(c, name, (size_t)(sl - name),
                       UINT32_MAX - (uint32_t)i);
    }
}

char
liszt_git_status(const struct liszt_git_ctx *c, const char *name,
                 size_t len, bool is_dir,
                 const struct liszt_git_wtstat *ws)
{
    if (c->degraded)
        return 0;
    uint32_t slot = win_find(c, name, len);
    if (slot == WIN_EMPTY)
        return 0;
    if (slot >= UINT32_MAX / 2) {
        /* Directory with tracked content: one decision, no walk. Any
           conflict below still surfaces per-file when listed. */
        (void)is_dir;
        return LISZT_GIT_WT_CLEAN;
    }
    const struct liszt_git_ientry *e = &c->ents[slot - 1];
    /* A tracked path listed as a directory (file replaced by dir)
       reads as typechange. */
    if (is_dir) {
        const unsigned char *ent = c->image + e->off;
        if ((be32(ent + 24) & 0170000u) == S_IFGITLINK)
            return LISZT_GIT_WT_CLEAN;
        return LISZT_GIT_WT_TYPECHANGE;
    }
    return liszt_git_compare(c, e, ws);
}

/* --- discovery --------------------------------------------------------- */

static struct liszt_git_ctx *repos;
static struct liszt_git_ctx no_repo;    /* sentinel: cached negative */

/* Directory-resolution cache: canonical dir -> ctx (or the negative
   sentinel). Open addressing, FNV over the path. */
static struct {
    char **keys;
    struct liszt_git_ctx **vals;
    size_t cap, used;
} dircache;

static struct liszt_git_ctx **
dircache_slot(const char *dir, size_t len)
{
    if (dircache.cap == 0 || dircache.used * 2 >= dircache.cap) {
        size_t ncap = dircache.cap ? dircache.cap * 2 : 64;
        char **nk = liszt_xmalloc(ncap * sizeof *nk);
        struct liszt_git_ctx **nv = liszt_xmalloc(ncap * sizeof *nv);
        memset(nk, 0, ncap * sizeof *nk);
        for (size_t i = 0; i < dircache.cap; i++) {
            if (dircache.keys[i] == NULL)
                continue;
            uint32_t h = fnv1a(dircache.keys[i],
                               strlen(dircache.keys[i]))
                & (uint32_t)(ncap - 1);
            while (nk[h] != NULL)
                h = (h + 1) & (uint32_t)(ncap - 1);
            nk[h] = dircache.keys[i];
            nv[h] = dircache.vals[i];
        }
        free(dircache.keys);
        free(dircache.vals);
        dircache.keys = nk;
        dircache.vals = nv;
        dircache.cap = ncap;
    }
    uint32_t h = fnv1a(dir, len) & (uint32_t)(dircache.cap - 1);
    while (dircache.keys[h] != NULL
           && (strlen(dircache.keys[h]) != len
               || memcmp(dircache.keys[h], dir, len) != 0))
        h = (h + 1) & (uint32_t)(dircache.cap - 1);
    return &dircache.vals[h];
}

static void
dircache_put(const char *dir, size_t len, struct liszt_git_ctx *ctx)
{
    struct liszt_git_ctx **v = dircache_slot(dir, len);
    size_t idx = (size_t)(v - dircache.vals);
    if (dircache.keys[idx] == NULL) {
        char *k = liszt_xmalloc(len + 1);
        memcpy(k, dir, len);
        k[len] = '\0';
        dircache.keys[idx] = k;
        dircache.used++;
    }
    *v = ctx;
}

/* Read a small whole file; NUL-terminated heap buffer or NULL. */
static char *
slurp(const char *path, size_t *lenp)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size > 1 << 20) {
        close(fd);
        return NULL;
    }
    size_t len = (size_t)st.st_size;
    char *buf = liszt_xmalloc(len + 1);
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, buf + got, len - got);
        if (r <= 0) {
            close(fd);
            free(buf);
            return NULL;
        }
        got += (size_t)r;
    }
    close(fd);
    buf[len] = '\0';
    if (lenp)
        *lenp = len;
    return buf;
}

static char *
join2(const char *a, const char *b)
{
    size_t al = strlen(a), bl = strlen(b);
    char *r = liszt_xmalloc(al + bl + 2);
    memcpy(r, a, al);
    r[al] = '/';
    memcpy(r + al + 1, b, bl + 1);
    return r;
}

/* Resolve a path RELATIVE against BASEDIR unless absolute. */
static char *
resolve_rel(const char *basedir, const char *rel)
{
    if (rel[0] == '/')
        return liszt_xstrdup(rel);
    return join2(basedir, rel);
}

/* First line of BUF after PREFIX, trimmed, or NULL. */
static char *
line_after(char *buf, const char *prefix)
{
    size_t plen = strlen(prefix);
    if (strncmp(buf, prefix, plen) != 0)
        return NULL;
    char *v = buf + plen;
    v[strcspn(v, "\r\n")] = '\0';
    return v;
}

static struct liszt_git_ctx *
open_repo(const char *root, size_t root_len, const char *gitdir_arg)
{
    struct liszt_git_ctx *c = liszt_xmalloc(sizeof *c);

    memset(c, 0, sizeof *c);
    c->root = liszt_xmalloc(root_len + 1);
    memcpy(c->root, root, root_len);
    c->root[root_len] = '\0';
    c->root_len = root_len;
    c->gitdir = liszt_xstrdup(gitdir_arg);
    c->filemode = true;

    /* commondir: linked worktrees keep shared state elsewhere. */
    char *cdpath = join2(c->gitdir, "commondir");
    char *cd = slurp(cdpath, NULL);
    free(cdpath);
    if (cd != NULL) {
        cd[strcspn(cd, "\r\n")] = '\0';
        c->common = resolve_rel(c->gitdir, cd);
        free(cd);
    } else {
        c->common = liszt_xstrdup(c->gitdir);
    }

    char *cfg = join2(c->common, "config");
    sniff_config(c, cfg);
    free(cfg);

    char *ixpath = join2(c->gitdir, "index");
    int fd = open(ixpath, O_RDONLY);
    if (fd < 0) {
        /* Fresh repo: valid, empty. */
        free(ixpath);
        c->next = repos;
        repos = c;
        return c;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        close(fd);
        free(ixpath);
        degrade(c, "index fstat");
        c->next = repos;
        repos = c;
        return c;
    }
    c->index_mtime = (int64_t)st.st_mtime;
    c->image_len = (size_t)st.st_size;
    void *map = c->image_len == 0 ? MAP_FAILED
        : mmap(NULL, c->image_len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map != MAP_FAILED) {
        c->image = map;
        c->mapped = true;
        close(fd);
    } else {
        close(fd);
        c->image = (unsigned char *)slurp(ixpath, &c->image_len);
        if (c->image == NULL) {
            free(ixpath);
            degrade(c, "index read");
            c->next = repos;
            repos = c;
            return c;
        }
    }
    free(ixpath);
    liszt_git_index_parse(c);
    c->next = repos;
    repos = c;
    return c;
}

/* Does DIR contain a repo marker? Fills GITDIR (heap) when yes. */
static bool
probe_dotgit(const char *dir, size_t len, char **gitdir_out)
{
    char *dg = liszt_xmalloc(len + 6);
    memcpy(dg, dir, len);
    memcpy(dg + len, "/.git", 6);

    struct stat st;
    if (lstat(dg, &st) != 0) {
        free(dg);
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        /* Git validates the gitdir; a bare ".git" directory dropped
           by some tool is not a repo. HEAD is the cheap tell. */
        char *head = join2(dg, "HEAD");
        bool ok = lstat(head, &st) == 0;
        free(head);
        if (!ok) {
            free(dg);
            return false;
        }
        *gitdir_out = dg;
        return true;
    }
    if (S_ISREG(st.st_mode)) {
        char *buf = slurp(dg, NULL);
        free(dg);
        if (buf == NULL)
            return false;
        char *v = line_after(buf, "gitdir: ");
        if (v == NULL) {
            free(buf);
            return false;
        }
        char *dirbuf = liszt_xmalloc(len + 1);
        memcpy(dirbuf, dir, len);
        dirbuf[len] = '\0';
        char *gedir = resolve_rel(dirbuf, v);
        free(dirbuf);
        free(buf);
        char *head = join2(gedir, "HEAD");
        bool ok = lstat(head, &st) == 0;
        free(head);
        if (!ok) {
            free(gedir);
            return false;
        }
        *gitdir_out = gedir;
        return true;
    }
    free(dg);
    return false;
}

static struct liszt_git_ctx *
resolve_dir(const char *dir, size_t len, dev_t dev)
{
    struct liszt_git_ctx **cached = dircache_slot(dir, len);
    if (dircache.keys[(size_t)(cached - dircache.vals)] != NULL)
        return *cached;

    struct liszt_git_ctx *ctx = &no_repo;
    char *gitdir = NULL;
    if (probe_dotgit(dir, len, &gitdir)) {
        ctx = open_repo(dir, len, gitdir);
        free(gitdir);
    } else if (len > 1) {
        /* Recurse into the parent; stop at device changes
           (GIT_DISCOVERY_ACROSS_FILESYSTEM default). */
        size_t plen = len - 1;
        while (plen > 1 && dir[plen] != '/')
            plen--;
        if (dir[plen] == '/' || plen == 1) {
            size_t up = plen == 1 ? 1 : plen;
            char *parent = liszt_xmalloc(up + 1);
            memcpy(parent, dir, up);
            parent[up] = '\0';
            struct stat pst;
            if (stat(parent, &pst) == 0 && pst.st_dev == dev)
                ctx = resolve_dir(parent, up, dev);
            free(parent);
        }
    }
    dircache_put(dir, len, ctx);
    return ctx;
}

struct liszt_git_ctx *
liszt_git_ctx_for(const char *abs)
{
    size_t len = strlen(abs);
    struct stat st;

    if (len == 0 || abs[0] != '/')
        return NULL;
    /* Trailing slash off (but keep "/"). */
    while (len > 1 && abs[len - 1] == '/')
        len--;
    if (stat(abs, &st) != 0)
        return NULL;
    struct liszt_git_ctx *c = resolve_dir(abs, len, st.st_dev);
    return c == &no_repo ? NULL : c;
}

void
liszt_git_shutdown(void)
{
    struct liszt_git_ctx *c = repos;
    while (c != NULL) {
        struct liszt_git_ctx *n = c->next;
        if (c->mapped)
            munmap(c->image, c->image_len);
        else
            free(c->image);
        free(c->ents);
        free(c->v4_arena);
        free(c->win_tab);
        free(c->root);
        free(c->gitdir);
        free(c->common);
        free(c);
        c = n;
    }
    repos = NULL;
    for (size_t i = 0; i < dircache.cap; i++)
        free(dircache.keys[i]);
    free(dircache.keys);
    free(dircache.vals);
    memset(&dircache, 0, sizeof dircache);
    if (dbg() && racy_seen > 0)
        fprintf(stderr, "liszt: git: racy-clean entries seen: %lu\n",
                racy_seen);
    racy_seen = 0;
}

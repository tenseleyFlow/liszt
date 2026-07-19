#include "gitignore.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* --- wildmatch --------------------------------------------------------

   Git's matcher (rsync lineage), reimplemented from its semantics.
   Recursion returns a three-way result so '*' backtracking can stop
   early: a component-bounded star that hits a slash aborts to the
   nearest '**' frame; class errors abort the whole match. */

enum {
    WM_MATCH = 0,
    WM_NOMATCH = 1,
    WM_ABORT_TO_STARSTAR = 2,
    WM_ABORT_ALL = 3
};

static bool
is_glob_special(unsigned char c)
{
    return c == '*' || c == '?' || c == '[' || c == '\\';
}

static int
class_test(const char *name, size_t len, unsigned char c)
{
    if (len == 5 && memcmp(name, "alnum", 5) == 0) return isalnum(c);
    if (len == 5 && memcmp(name, "alpha", 5) == 0) return isalpha(c);
    if (len == 5 && memcmp(name, "blank", 5) == 0) return isblank(c);
    if (len == 5 && memcmp(name, "cntrl", 5) == 0) return iscntrl(c);
    if (len == 5 && memcmp(name, "digit", 5) == 0) return isdigit(c);
    if (len == 5 && memcmp(name, "graph", 5) == 0) return isgraph(c);
    if (len == 5 && memcmp(name, "lower", 5) == 0) return islower(c);
    if (len == 5 && memcmp(name, "print", 5) == 0) return isprint(c);
    if (len == 5 && memcmp(name, "punct", 5) == 0) return ispunct(c);
    if (len == 5 && memcmp(name, "space", 5) == 0) return isspace(c);
    if (len == 5 && memcmp(name, "upper", 5) == 0) return isupper(c);
    if (len == 6 && memcmp(name, "xdigit", 6) == 0) return isxdigit(c);
    return -1;
}

static int
dowild(const char *p, const char *text, unsigned flags,
       const char *pat_start)
{
    unsigned char p_ch;

    for (; (p_ch = (unsigned char)*p) != '\0'; text++, p++) {
        unsigned char t_ch = (unsigned char)*text;

        if (t_ch == '\0' && p_ch != '*')
            return WM_ABORT_ALL;

        switch (p_ch) {
        case '\\':
            /* Literal match with the escaped character; a trailing
               backslash never matches (t_ch is nonzero here). */
            p_ch = (unsigned char)*++p;
            /* fallthrough */
        default:
            if (t_ch != p_ch)
                return WM_NOMATCH;
            continue;
        case '?':
            if (t_ch == '/' && (flags & LISZT_WM_PATHNAME))
                return WM_NOMATCH;
            continue;
        case '*': {
            bool match_slash;
            if (*++p == '*') {
                const char *prev_p = p - 2;
                while (*++p == '*')
                    ;
                if (!(flags & LISZT_WM_PATHNAME)) {
                    match_slash = true;
                } else if ((prev_p < pat_start || *prev_p == '/')
                           && (*p == '\0' || *p == '/'
                               || (p[0] == '\\' && p[1] == '/'))) {
                    /* A double-star with a following slash may also
                       match nothing: probe the tail against the
                       unadvanced text. */
                    if (p[0] == '/'
                        && dowild(p + 1, text, flags,
                                  pat_start) == WM_MATCH)
                        return WM_MATCH;
                    match_slash = true;
                } else {
                    /* "a**b" and friends degrade to '*'. */
                    match_slash = false;
                }
            } else {
                match_slash = !(flags & LISZT_WM_PATHNAME);
            }

            if (*p == '\0') {
                /* Trailing "**" takes everything; trailing '*' only
                   a slashless remainder. */
                if (!match_slash && strchr(text, '/') != NULL)
                    return WM_ABORT_TO_STARSTAR;
                return WM_MATCH;
            }
            if (!match_slash && *p == '/') {
                /* "*" then '/': the star finishes this component;
                   jump to the next separator (consumed by the outer
                   loop). */
                const char *slash = strchr(text, '/');
                if (slash == NULL)
                    return WM_ABORT_ALL;
                text = slash;
                break;
            }
            for (;;) {
                if (t_ch == '\0')
                    break;
                /* Star followed by a literal: skip ahead to each
                   occurrence instead of recursing per byte. */
                if (!is_glob_special((unsigned char)*p)) {
                    p_ch = (unsigned char)*p;
                    while ((t_ch = (unsigned char)*text) != '\0'
                           && (match_slash || t_ch != '/')) {
                        if (t_ch == p_ch)
                            break;
                        text++;
                    }
                    if (t_ch != p_ch)
                        return WM_NOMATCH;
                }
                int matched = dowild(p, text, flags, pat_start);
                if (matched != WM_NOMATCH) {
                    if (!match_slash
                        || matched != WM_ABORT_TO_STARSTAR)
                        return matched;
                } else if (!match_slash && t_ch == '/') {
                    return WM_ABORT_TO_STARSTAR;
                }
                t_ch = (unsigned char)*++text;
            }
            return WM_ABORT_ALL;
        }
        case '[': {
            unsigned char prev_ch = 0;
            bool negated = false;
            bool matched = false;

            p_ch = (unsigned char)*++p;
            if (p_ch == '!' || p_ch == '^') {
                negated = true;
                p_ch = (unsigned char)*++p;
            }
            /* ']' first is literal; the loop below runs at least
               once, mirroring that rule. */
            do {
                if (p_ch == '\0')
                    return WM_ABORT_ALL;
                if (p_ch == '\\') {
                    p_ch = (unsigned char)*++p;
                    if (p_ch == '\0')
                        return WM_ABORT_ALL;
                    if (t_ch == p_ch)
                        matched = true;
                } else if (p_ch == '-' && prev_ch != 0
                           && p[1] != '\0' && p[1] != ']') {
                    p_ch = (unsigned char)*++p;
                    if (p_ch == '\\') {
                        p_ch = (unsigned char)*++p;
                        if (p_ch == '\0')
                            return WM_ABORT_ALL;
                    }
                    if (t_ch >= prev_ch && t_ch <= p_ch)
                        matched = true;
                    p_ch = 0;   /* range never seeds another range */
                } else if (p_ch == '[' && p[1] == ':') {
                    const char *s = p + 2;
                    const char *e = s;
                    while (*e != '\0' && *e != ']')
                        e++;
                    if (*e == '\0')
                        return WM_ABORT_ALL;
                    if (e == s || e[-1] != ':') {
                        /* No ":]": not a class after all - fall back
                           to a literal '[' set member, git-style. */
                        p = s - 2;
                        p_ch = '[';
                        if (t_ch == p_ch)
                            matched = true;
                    } else {
                        int r = class_test(s, (size_t)(e - 1 - s),
                                           t_ch);
                        if (r < 0)
                            return WM_ABORT_ALL;
                        if (r)
                            matched = true;
                        p = e;
                        p_ch = 0;
                    }
                } else if (t_ch == p_ch) {
                    matched = true;
                }
                prev_ch = p_ch;
                p_ch = (unsigned char)*++p;
            } while (p_ch != ']');
            if (matched == negated
                || (t_ch == '/' && (flags & LISZT_WM_PATHNAME)))
                return WM_NOMATCH;
            continue;
        }
        }
    }

    return *text != '\0' ? WM_NOMATCH : WM_MATCH;
}

bool
liszt_wildmatch(const char *pattern, const char *text, unsigned flags)
{
    return dowild(pattern, text, flags, pattern) == WM_MATCH;
}

/* --- pattern compiler -------------------------------------------------- */

/* Git's trim_trailing_spaces: unescaped trailing spaces drop, an
   escaped space survives (wildmatch later eats the backslash). Only
   ' ' - tabs are significant. */
static size_t
trim_trailing_spaces(const char *s, size_t len)
{
    size_t last_space = len;    /* len = no trailing run */
    size_t i = 0;

    while (i < len) {
        if (s[i] == ' ') {
            if (last_space == len)
                last_space = i;
            i++;
        } else if (s[i] == '\\') {
            i += 2;
            if (i > len)
                return len;     /* trailing backslash: keep all */
            last_space = len;
        } else {
            last_space = len;
            i++;
        }
    }
    return last_space;
}

void
liszt_gi_file_parse(struct liszt_gi_file *f, const char *buf,
                    size_t buflen, const char *base, size_t base_len)
{
    size_t cap = 8;

    f->pats = liszt_xmalloc(cap * sizeof *f->pats);
    f->n_pats = 0;
    /* Every decoded pattern fits in the source size plus NULs. */
    f->arena = liszt_xmalloc(buflen + 1);
    f->base = liszt_xmalloc(base_len + 1);
    memcpy(f->base, base, base_len);
    f->base[base_len] = '\0';
    f->base_len = base_len;

    char *q = f->arena;
    size_t pos = 0;
    while (pos < buflen) {
        size_t eol = pos;
        while (eol < buflen && buf[eol] != '\n')
            eol++;
        const char *line = buf + pos;
        size_t len = eol - pos;
        pos = eol + 1;

        if (len > 0 && line[len - 1] == '\r')
            len--;
        if (len == 0 || line[0] == '#')
            continue;
        len = trim_trailing_spaces(line, len);
        if (len == 0)
            continue;

        struct liszt_gi_pat pt = { 0 };
        if (line[0] == '!') {
            pt.negated = 1;
            line++;
            len--;
            if (len == 0)
                continue;
        }
        if (line[len - 1] == '/') {
            pt.dir_only = 1;
            len--;
            if (len == 0)
                continue;
        }
        pt.nodir = memchr(line, '/', len) == NULL;
        if (!pt.nodir && line[0] == '/') {
            /* A slash anywhere anchors; the leading one is only
               spelling. */
            line++;
            len--;
            if (len == 0)
                continue;
        }
        bool wild = false;
        for (size_t i = 0; i < len; i++)
            if (is_glob_special((unsigned char)line[i])) {
                wild = true;
                break;
            }
        pt.nowild = !wild;
        memcpy(q, line, len);
        q[len] = '\0';
        pt.pat = q;
        pt.len = (uint32_t)len;
        q += len + 1;

        if (f->n_pats == cap) {
            cap *= 2;
            f->pats = liszt_xrealloc(f->pats, cap * sizeof *f->pats);
        }
        f->pats[f->n_pats++] = pt;
    }
}

void
liszt_gi_file_free(struct liszt_gi_file *f)
{
    free(f->pats);
    free(f->arena);
    free(f->base);
    f->pats = NULL;
    f->arena = NULL;
    f->base = NULL;
    f->n_pats = 0;
}

static bool
pat_matches(const struct liszt_gi_pat *pt, const char *path, size_t len,
            size_t base_off, size_t skip, bool is_dir)
{
    if (pt->dir_only && !is_dir)
        return false;
    if (pt->nodir) {
        const char *bn = path + base_off;
        size_t blen = len - base_off;
        if (pt->nowild)
            return pt->len == blen && memcmp(pt->pat, bn, blen) == 0;
        return liszt_wildmatch(pt->pat, bn, 0);
    }
    /* Slashed pattern: match against the path below this file's
       directory. */
    const char *name = path + skip;
    size_t nlen = len - skip;
    if (pt->nowild)
        return pt->len == nlen && memcmp(pt->pat, name, nlen) == 0;
    return liszt_wildmatch(pt->pat, name, LISZT_WM_PATHNAME);
}

int
liszt_gi_file_match(const struct liszt_gi_file *f, const char *path,
                    size_t len, size_t base_off, bool is_dir)
{
    /* Bytes to drop for base-relative matching: "base/" when the file
       is not at the root. */
    size_t skip = f->base_len == 0 ? 0 : f->base_len + 1;

    for (size_t i = f->n_pats; i > 0; i--) {
        const struct liszt_gi_pat *pt = &f->pats[i - 1];
        if (pat_matches(pt, path, len, base_off, skip, is_dir))
            return pt->negated ? 0 : 1;
    }
    return -1;
}

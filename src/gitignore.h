#ifndef LISZT_GITIGNORE_H
#define LISZT_GITIGNORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Gitignore engine (v0.2 --git/--git-ignore): wildmatch with git's
   semantics plus the pattern compiler and per-file matcher. fnmatch is
   not sufficient - ** path spans, anchoring, dir-only patterns,
   negation, and escaped trailing spaces all differ. */

#define LISZT_WM_PATHNAME 1u    /* '*', '?', classes never match '/' */

/* True when PATTERN matches TEXT whole. Git-compatible: '**' spans
   path components only in pathname mode and only between separators;
   classes support ranges, negation, and POSIX names; backslash
   escapes; a malformed class never matches. */
bool liszt_wildmatch(const char *pattern, const char *text,
                     unsigned flags);

struct liszt_gi_pat {
    const char *pat;        /* NUL-terminated, into the file arena */
    uint32_t len;
    uint8_t negated;        /* leading '!' */
    uint8_t dir_only;       /* trailing '/' */
    uint8_t nodir;          /* no slash: match the basename anywhere */
    uint8_t nowild;         /* no glob specials: memcmp fast path */
};

/* One ignore file's compiled rules. BASE is the repo-relative
   directory the file lives in ("" = root, no trailing slash); slashed
   patterns anchor to it. */
struct liszt_gi_file {
    struct liszt_gi_pat *pats;
    size_t n_pats;
    char *arena;
    char *base;
    size_t base_len;
};

/* Compile BUF (a .gitignore's bytes; not consumed) into F. */
void liszt_gi_file_parse(struct liszt_gi_file *f, const char *buf,
                         size_t buflen, const char *base,
                         size_t base_len);
void liszt_gi_file_free(struct liszt_gi_file *f);

/* Match one repo-relative PATH (no leading slash) whose basename
   starts at BASE_OFF. Scans rules in reverse: the last matching line
   in the file wins, exactly git. Returns 1 exclude, 0 re-include,
   -1 no decision. PATH must lie under F's base (caller guarantees). */
int liszt_gi_file_match(const struct liszt_gi_file *f, const char *path,
                        size_t len, size_t base_off, bool is_dir);

#endif

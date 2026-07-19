#ifndef LISZT_GIT_H
#define LISZT_GIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Dependency-free git status, cheap tier (v0.2): worktree-vs-index
   only. No hashing, no zlib, no HEAD. The index is mmapped and parsed
   into a full-index table; per-listed-directory windows give O(1)
   basename lookups. Anything unexpected degrades the whole repo to a
   blank column - never a crash, never a diagnostic (reasons under
   LISZT_DEBUG_GIT=1). */

/* Worktree-status letters (the second cell char; staged is always
   '-'). 0 = untracked-or-unknown: the caller decides N vs I via the
   gitignore chains. */
#define LISZT_GIT_WT_CLEAN      '-'
#define LISZT_GIT_WT_MODIFIED   'M'
#define LISZT_GIT_WT_TYPECHANGE 'T'
#define LISZT_GIT_WT_UNTRACKED  'N'
#define LISZT_GIT_WT_IGNORED    'I'
#define LISZT_GIT_WT_CONFLICT   'U'

/* One index entry in the full-index table. PATH is repo-relative,
   NUL-terminated (v2/v3 point into the mapped file's padded names;
   v4 into the expansion arena). */
struct liszt_git_ientry {
    const char *path;
    uint32_t len;
    uint32_t off;           /* entry offset in the index image */
    uint16_t flags;         /* on-disk flags word (stage, valid) */
    uint16_t xflags;        /* v3+ extended flags (skip-worktree, ita) */
};

struct liszt_git_ctx {
    char *root;             /* canonical worktree root, no trailing / */
    size_t root_len;
    char *gitdir;           /* .git dir (worktrees: the private one) */
    char *common;           /* commondir (info/exclude, config) */
    unsigned char *image;   /* index bytes */
    size_t image_len;
    bool mapped;            /* munmap vs free */
    char *v4_arena;
    struct liszt_git_ientry *ents;
    size_t n_ents;
    bool sha256;            /* extensions.objectformat = sha256 */
    bool filemode;          /* core.filemode (default true) */
    bool degraded;          /* blank column for the whole repo */
    int64_t index_mtime;    /* racy accounting */
    /* Per-listed-directory window. */
    size_t win_lo, win_hi;  /* entry span under the window prefix */
    size_t win_plen;        /* repo-relative prefix len (with '/') */
    uint32_t *win_tab;      /* basename hash -> entry idx + 1, or DIR */
    size_t win_cap;         /* power of two */
    struct liszt_git_ctx *next;
};

/* Worktree lstat facts the compare needs (decoupled from statinfo
   until the LISZT_WANT_MTIME wiring). */
struct liszt_git_wtstat {
    int64_t mtime_sec;
    int32_t mtime_nsec;
    uint64_t size;
    uint32_t mode;
};

/* Resolve the repo context for canonical absolute directory ABS.
   Cached per directory and per repo root; one .git lstat per new
   subdirectory under a known root. NULL = outside any repo. A
   degraded repo still returns its ctx (degraded flag set). */
struct liszt_git_ctx *liszt_git_ctx_for(const char *abs);

/* Build the lookup window for the listed directory ABS (must be under
   c->root). */
void liszt_git_window(struct liszt_git_ctx *c, const char *abs);

/* Status of one entry NAME (basename) in the windowed directory.
   Returns a LISZT_GIT_WT_* letter, or 0 when the index knows nothing
   (untracked vs ignored is the gitignore layer's call). WS may be
   NULL when no stat is available (returns clean for tracked paths).
   Thread-safe for concurrent calls on one built window. */
char liszt_git_status(const struct liszt_git_ctx *c, const char *name,
                      size_t len, bool is_dir,
                      const struct liszt_git_wtstat *ws);

/* Compare one index entry against worktree facts (unit surface). */
char liszt_git_compare(const struct liszt_git_ctx *c,
                       const struct liszt_git_ientry *e,
                       const struct liszt_git_wtstat *ws);

/* Parse an index image in place (unit surface; ctx_for uses it).
   Returns false on degrade. */
bool liszt_git_index_parse(struct liszt_git_ctx *c);

void liszt_git_shutdown(void);

#endif

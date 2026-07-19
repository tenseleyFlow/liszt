#ifndef LISZT_SYS_XSTAT_H
#define LISZT_SYS_XSTAT_H

#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* Which timestamp -c/-u/--time selected. Values mirror GNU's enum
   time_type (ls.c); the module-level setting below plays the role of
   GNU's global in calc_req_mask and do_statx. */
enum liszt_timetype {
    LISZT_TIME_MTIME = 0,
    LISZT_TIME_CTIME,
    LISZT_TIME_ATIME,
    LISZT_TIME_BTIME
};

/* The single metadata contract: every backend (statx, lstat fallback)
   fills every field the WANTS mask names; renderers read only this
   struct. The mask exists so statx requests only what the plan derived
   (GNU calc_req_mask semantics); the fallback path always fills
   everything. */
struct liszt_statinfo {
    mode_t mode;
    nlink_t nlink;
    uid_t uid;
    gid_t gid;
    off_t size;
    struct timespec time;   /* the SELECTED timestamp (see
                               liszt_xstat_time_type); GNU stores btime
                               in st_mtim the same way. (-1,-1) = birth
                               time requested but unavailable. */
    blkcnt_t blocks;    /* 512-byte units (ST_NBLOCKSIZE) */
    ino_t ino;
    dev_t dev;
    dev_t rdev;
};

/* Set once after options resolution, before any stat call. Every
   backend then fetches and stores that timestamp in .time. */
void liszt_xstat_time_type(enum liszt_timetype t);

enum {
    LISZT_WANT_MODE = 1 << 0,
    LISZT_WANT_NLINK = 1 << 1,
    LISZT_WANT_UID = 1 << 2,
    LISZT_WANT_GID = 1 << 3,
    LISZT_WANT_SIZE = 1 << 4,
    LISZT_WANT_TIME = 1 << 5,   /* the selected timestamp */
    LISZT_WANT_BLOCKS = 1 << 6,
    LISZT_WANT_INO = 1 << 7
};
#define LISZT_WANT_ALL 0xff

/* Stat DIR/NAME (join via an internal reusable buffer) or a whole PATH.
   FOLLOW selects stat vs lstat semantics. Return 0 or -1 with errno. */
int liszt_statx_join(const char *dir, const char *name, unsigned wants,
                     bool follow, struct liszt_statinfo *out);
int liszt_statx_path(const char *path, unsigned wants, bool follow,
                     struct liszt_statinfo *out);

/* Compatibility wrappers (sprint 02 callers): WANT_ALL. */
int liszt_lstat_join(const char *dir, const char *name,
                     struct liszt_statinfo *out);
int liszt_stat_join(const char *dir, const char *name,
                    struct liszt_statinfo *out);
int liszt_stat_path(const char *path, struct liszt_statinfo *out);
int liszt_lstat_path(const char *path, struct liszt_statinfo *out);

/* fstat an open descriptor (loop detection on dirfds: O(1) instead of
   re-walking a deep path). */
int liszt_fstat(int fd, struct liszt_statinfo *out);

/* "DIR/NAME" in a reused internal buffer, valid until the next join or
   stat call through this module. */
const char *liszt_join_path(const char *dir, const char *name);

/* readlink of DIR/NAME: malloc'd raw bytes or NULL (errno set). */
char *liszt_readlink_join(const char *dir, const char *name);

/* llistxattr into BUF (symlinks not followed): the NUL-separated
   attribute-name list, its length returned; 0 = none, -1 = unsupported
   or error. One syscall answers every suffix question, GNU's shape. */
long liszt_xattr_list_join(const char *dir, const char *name, char *buf,
                           size_t size);

/* Whether ATTR appears in DIR/NAME's xattr name list (one syscall). */
/* Read one attribute's value (getfilecon shape): length or -1+errno,
   ENOTSUP where the platform has no xattrs. */
long liszt_xattr_value_join(const char *dir, const char *name,
                            const char *attr, bool follow, char *buf,
                            size_t size);

bool liszt_xattr_list_has(const char *dir, const char *name,
                          const char *attr);

#endif

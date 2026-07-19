#include "sys/xstat.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "util.h"

#if LISZT_HAVE_ST_MTIM
#define ST_MTIMESPEC(st) ((st)->st_mtim)
#elif LISZT_HAVE_ST_MTIMESPEC
#define ST_MTIMESPEC(st) ((st)->st_mtimespec)
#else
#error "no nanosecond mtime member probed"
#endif

#if LISZT_HAVE_GETXATTR || LISZT_HAVE_GETXATTR_DARWIN
#include <sys/xattr.h>
#endif

static char *pathbuf;
static size_t pathbuf_cap;

/* Which timestamp .time carries, set once before any stat (GNU's
   global time_type consulted by calc_req_mask and do_statx). */
static enum liszt_timetype time_type = LISZT_TIME_MTIME;

void
liszt_xstat_time_type(enum liszt_timetype t)
{
    time_type = t;
}

const char *
liszt_join_path(const char *dir, const char *name)
{
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    int need_slash = dlen > 0 && dir[dlen - 1] != '/';
    size_t want = dlen + (size_t)need_slash + nlen + 1;

    if (want > pathbuf_cap) {
        size_t cap = pathbuf_cap ? pathbuf_cap : 256;
        while (cap < want)
            cap += cap / 2;
        pathbuf = liszt_xrealloc(pathbuf, cap);
        pathbuf_cap = cap;
    }
    memcpy(pathbuf, dir, dlen);
    if (need_slash)
        pathbuf[dlen] = '/';
    memcpy(pathbuf + dlen + (size_t)need_slash, name, nlen + 1);
    return pathbuf;
}

#if LISZT_HAVE_STATX

#include <fcntl.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif

static unsigned int
statx_mask(unsigned wants)
{
    unsigned int mask = 0;
    if (wants & LISZT_WANT_MODE)
        mask |= STATX_MODE | STATX_TYPE;
    if (wants & LISZT_WANT_NLINK)
        mask |= STATX_NLINK;
    if (wants & LISZT_WANT_UID)
        mask |= STATX_UID;
    if (wants & LISZT_WANT_GID)
        mask |= STATX_GID;
    if (wants & LISZT_WANT_SIZE)
        mask |= STATX_SIZE;
    if (wants & LISZT_WANT_TIME)
        switch (time_type) {
        case LISZT_TIME_CTIME: mask |= STATX_CTIME; break;
        case LISZT_TIME_ATIME: mask |= STATX_ATIME; break;
        case LISZT_TIME_BTIME: mask |= STATX_BTIME; break;
        case LISZT_TIME_MTIME: default: mask |= STATX_MTIME; break;
        }
    if (wants & LISZT_WANT_BLOCKS)
        mask |= STATX_BLOCKS;
    if (wants & LISZT_WANT_INO)
        mask |= STATX_INO;
    return mask;
}

int
liszt_statx_path(const char *path, unsigned wants, bool follow,
                 struct liszt_statinfo *out)
{
    struct statx stx;
    int flags = AT_NO_AUTOMOUNT | (follow ? 0 : AT_SYMLINK_NOFOLLOW);

    if (statx(AT_FDCWD, path, flags, statx_mask(wants), &stx) < 0)
        return -1;
    out->mode = stx.stx_mode;
    out->nlink = stx.stx_nlink;
    out->uid = stx.stx_uid;
    out->gid = stx.stx_gid;
    out->size = (off_t)stx.stx_size;
    switch (time_type) {
    case LISZT_TIME_CTIME:
        out->time.tv_sec = stx.stx_ctime.tv_sec;
        out->time.tv_nsec = stx.stx_ctime.tv_nsec;
        break;
    case LISZT_TIME_ATIME:
        out->time.tv_sec = stx.stx_atime.tv_sec;
        out->time.tv_nsec = stx.stx_atime.tv_nsec;
        break;
    case LISZT_TIME_BTIME:
        /* The fs may not carry birth times; GNU signals with (-1,-1). */
        if (stx.stx_mask & STATX_BTIME) {
            out->time.tv_sec = stx.stx_btime.tv_sec;
            out->time.tv_nsec = stx.stx_btime.tv_nsec;
        } else {
            out->time.tv_sec = -1;
            out->time.tv_nsec = -1;
        }
        break;
    case LISZT_TIME_MTIME:
    default:
        out->time.tv_sec = stx.stx_mtime.tv_sec;
        out->time.tv_nsec = stx.stx_mtime.tv_nsec;
        break;
    }
    out->blocks = (blkcnt_t)stx.stx_blocks;
    out->ino = stx.stx_ino;
    out->dev = makedev(stx.stx_dev_major, stx.stx_dev_minor);
    out->rdev = makedev(stx.stx_rdev_major, stx.stx_rdev_minor);
    return 0;
}

#else /* stat/lstat fallback */

static void
fill(const struct stat *st, struct liszt_statinfo *out)
{
    out->mode = st->st_mode;
    out->nlink = st->st_nlink;
    out->uid = st->st_uid;
    out->gid = st->st_gid;
    out->size = st->st_size;
    switch (time_type) {
#if LISZT_HAVE_ST_MTIM
    case LISZT_TIME_CTIME: out->time = st->st_ctim; break;
    case LISZT_TIME_ATIME: out->time = st->st_atim; break;
#else
    case LISZT_TIME_CTIME: out->time = st->st_ctimespec; break;
    case LISZT_TIME_ATIME: out->time = st->st_atimespec; break;
#endif
    case LISZT_TIME_BTIME:
#if LISZT_HAVE_ST_BIRTHTIM
        out->time = st->st_birthtim;
#elif LISZT_HAVE_ST_BIRTHTIMESPEC
        out->time = st->st_birthtimespec;
#else
        out->time.tv_sec = -1;      /* GNU's unsupported sentinel */
        out->time.tv_nsec = -1;
#endif
        break;
    case LISZT_TIME_MTIME:
    default:
        out->time = ST_MTIMESPEC(st);
        break;
    }
    out->blocks = st->st_blocks;
    out->ino = st->st_ino;
    out->dev = st->st_dev;
    out->rdev = st->st_rdev;
}

int
liszt_statx_path(const char *path, unsigned wants, bool follow,
                 struct liszt_statinfo *out)
{
    struct stat st;
    (void)wants;
    if ((follow ? stat(path, &st) : lstat(path, &st)) < 0)
        return -1;
    fill(&st, out);
    return 0;
}

#endif

int
liszt_statx_join(const char *dir, const char *name, unsigned wants,
                 bool follow, struct liszt_statinfo *out)
{
    return liszt_statx_path(liszt_join_path(dir, name), wants, follow, out);
}

int
liszt_lstat_join(const char *dir, const char *name,
                 struct liszt_statinfo *out)
{
    return liszt_statx_join(dir, name, LISZT_WANT_ALL, false, out);
}

int
liszt_stat_join(const char *dir, const char *name,
                struct liszt_statinfo *out)
{
    return liszt_statx_join(dir, name, LISZT_WANT_ALL, true, out);
}

int
liszt_stat_path(const char *path, struct liszt_statinfo *out)
{
    return liszt_statx_path(path, LISZT_WANT_ALL, true, out);
}

int
liszt_lstat_path(const char *path, struct liszt_statinfo *out)
{
    return liszt_statx_path(path, LISZT_WANT_ALL, false, out);
}

char *
liszt_readlink_join(const char *dir, const char *name)
{
    const char *path = liszt_join_path(dir, name);
    size_t cap = 128;

    for (;;) {
        char *buf = liszt_xmalloc(cap);
        ssize_t n = readlink(path, buf, cap);
        if (n < 0) {
            int saved = errno;
            free(buf);
            errno = saved;
            return NULL;
        }
        if ((size_t)n < cap) {
            buf[n] = '\0';
            return buf;
        }
        free(buf);
        cap *= 2;
    }
}

long
liszt_xattr_value_join(const char *dir, const char *name, const char *attr,
                       bool follow, char *buf, size_t size)
{
#if LISZT_HAVE_GETXATTR
    const char *path = liszt_join_path(dir, name);
    ssize_t n = follow ? getxattr(path, attr, buf, size)
                       : lgetxattr(path, attr, buf, size);
    return n < 0 ? -1 : (long)n;
#elif LISZT_HAVE_GETXATTR_DARWIN
    ssize_t n = getxattr(liszt_join_path(dir, name), attr, buf, size, 0,
                         follow ? 0 : XATTR_NOFOLLOW);
    return n < 0 ? -1 : (long)n;
#else
    (void)dir; (void)name; (void)attr; (void)follow; (void)buf;
    (void)size;
    errno = ENOTSUP;
    return -1;
#endif
}

long
liszt_xattr_list_join(const char *dir, const char *name, char *buf,
                      size_t size)
{
#if LISZT_HAVE_GETXATTR
    ssize_t n = llistxattr(liszt_join_path(dir, name), buf, size);
    return n < 0 ? -1 : (long)n;
#elif LISZT_HAVE_GETXATTR_DARWIN
    ssize_t n = listxattr(liszt_join_path(dir, name), buf, size,
                          XATTR_NOFOLLOW);
    return n < 0 ? -1 : (long)n;
#else
    (void)dir;
    (void)name;
    (void)buf;
    (void)size;
    return -1;
#endif
}

bool
liszt_xattr_list_has(const char *dir, const char *name, const char *attr)
{
    char buf[4096];
    long n = liszt_xattr_list_join(dir, name, buf, sizeof buf);

    for (long off = 0; off < n; off += (long)strlen(buf + off) + 1)
        if (strcmp(buf + off, attr) == 0)
            return true;
    return false;
}

int
liszt_fstat(int fd, struct liszt_statinfo *out)
{
#if LISZT_HAVE_STATX
    struct statx stx;
    if (statx(fd, "", AT_EMPTY_PATH, STATX_INO, &stx) < 0)
        return -1;
    out->mode = stx.stx_mode;
    out->nlink = stx.stx_nlink;
    out->uid = stx.stx_uid;
    out->gid = stx.stx_gid;
    out->size = (off_t)stx.stx_size;
    out->time.tv_sec = stx.stx_mtime.tv_sec;
    out->time.tv_nsec = stx.stx_mtime.tv_nsec;
    out->blocks = (blkcnt_t)stx.stx_blocks;
    out->ino = stx.stx_ino;
    out->dev = makedev(stx.stx_dev_major, stx.stx_dev_minor);
    out->rdev = makedev(stx.stx_rdev_major, stx.stx_rdev_minor);
    return 0;
#else
    struct stat st;
    if (fstat(fd, &st) < 0)
        return -1;
    fill(&st, out);
    return 0;
#endif
}

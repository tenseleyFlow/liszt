#include "sys/dir.h"

#include <errno.h>
#include <fcntl.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "util.h"

/* Backend selection: raw getdents64 on Linux (no per-entry readdir copy),
   readdir(3) elsewhere. getdirentries stays probed in configure for a
   possible sprint-09 BSD fast path; readdir is correct everywhere. */

#if LISZT_HAVE_GETDENTS64

#include <sys/syscall.h>

#define LISZT_DIRBUF (64u * 1024u)

/* Kernel ABI record; glibc/musl do not expose a public struct for the
   raw syscall. */
struct liszt_kdirent64 {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

struct liszt_dir {
    int fd;
    size_t pos;
    size_t size;
    alignas(max_align_t) char buf[LISZT_DIRBUF];
};

static enum liszt_ftype
type_from_dt(unsigned char dt)
{
    switch (dt) {
    case 1: return LISZT_T_FIFO;    /* DT_FIFO */
    case 2: return LISZT_T_CHR;     /* DT_CHR */
    case 4: return LISZT_T_DIR;     /* DT_DIR */
    case 6: return LISZT_T_BLK;     /* DT_BLK */
    case 8: return LISZT_T_REG;     /* DT_REG */
    case 10: return LISZT_T_LNK;    /* DT_LNK */
    case 12: return LISZT_T_SOCK;   /* DT_SOCK */
    case 14: return LISZT_T_WHT;    /* DT_WHT */
    default: return LISZT_T_UNKNOWN;
    }
}

static int
dir_from_fd(int fd, struct liszt_dir **out)
{
    struct liszt_dir *d = liszt_xmalloc(sizeof *d);
    d->fd = fd;
    d->pos = 0;
    d->size = 0;
    *out = d;
    return 0;
}

int
liszt_diropen(const char *path, struct liszt_dir **out)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    return dir_from_fd(fd, out);
}

int
liszt_diropen_at(int parent_fd, const char *name, struct liszt_dir **out)
{
    int fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    return dir_from_fd(fd, out);
}

int
liszt_dirread(struct liszt_dir *d, struct liszt_dirent *e)
{
    for (;;) {
        if (d->pos >= d->size) {
            long n = syscall(SYS_getdents64, d->fd, d->buf, sizeof d->buf);
            if (n < 0)
                return -1;
            if (n == 0)
                return 0;
            d->pos = 0;
            d->size = (size_t)n;
        }

        struct liszt_kdirent64 *k =
            (struct liszt_kdirent64 *)(d->buf + d->pos);
        /* d_reclen == 0 would spin forever on a corrupt 9p/FUSE/overlay
           directory; treat the buffer as exhausted. */
        if (k->d_reclen == 0 || d->pos + k->d_reclen > d->size) {
            d->pos = d->size;
            continue;
        }
        d->pos += k->d_reclen;

        if (k->d_ino == 0)
            continue;   /* deleted but not yet purged */

        e->name = k->d_name;
        e->namelen = strlen(k->d_name);
        e->type = type_from_dt(k->d_type);
        e->ino = k->d_ino;
        return 1;
    }
}

int
liszt_dirfd(const struct liszt_dir *d)
{
    return d->fd;
}

void
liszt_dirclose(struct liszt_dir *d)
{
    close(d->fd);
    free(d);
}

#else /* readdir backend */

#include <dirent.h>
#include <sys/types.h>

struct liszt_dir {
    DIR *dp;
};

static enum liszt_ftype
type_from_dt(unsigned char dt)
{
    switch (dt) {
#ifdef DT_FIFO
    case DT_FIFO: return LISZT_T_FIFO;
    case DT_CHR: return LISZT_T_CHR;
    case DT_DIR: return LISZT_T_DIR;
    case DT_BLK: return LISZT_T_BLK;
    case DT_REG: return LISZT_T_REG;
    case DT_LNK: return LISZT_T_LNK;
    case DT_SOCK: return LISZT_T_SOCK;
#ifdef DT_WHT
    case DT_WHT: return LISZT_T_WHT;
#endif
#endif
    default: return LISZT_T_UNKNOWN;
    }
}

int
liszt_diropen(const char *path, struct liszt_dir **out)
{
    DIR *dp = opendir(path);
    if (!dp)
        return -1;
    struct liszt_dir *d = liszt_xmalloc(sizeof *d);
    d->dp = dp;
    *out = d;
    return 0;
}

int
liszt_diropen_at(int parent_fd, const char *name, struct liszt_dir **out)
{
    int fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    DIR *dp = fdopendir(fd);
    if (!dp) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    struct liszt_dir *d = liszt_xmalloc(sizeof *d);
    d->dp = dp;
    *out = d;
    return 0;
}

int
liszt_dirread(struct liszt_dir *d, struct liszt_dirent *e)
{
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(d->dp);
        if (!de)
            return errno == 0 ? 0 : -1;
        if (de->d_ino == 0)
            continue;
        e->name = de->d_name;
        e->namelen = strlen(de->d_name);
#if LISZT_HAVE_D_TYPE
        e->type = type_from_dt(de->d_type);
#else
        e->type = LISZT_T_UNKNOWN;
#endif
        e->ino = (unsigned long long)de->d_ino;
        return 1;
    }
}

int
liszt_dirfd(const struct liszt_dir *d)
{
    return dirfd(d->dp);
}

void
liszt_dirclose(struct liszt_dir *d)
{
    closedir(d->dp);
    free(d);
}

#endif

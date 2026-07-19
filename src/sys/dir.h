#ifndef LISZT_SYS_DIR_H
#define LISZT_SYS_DIR_H

#include <stddef.h>

/* File type derived from d_type; UNKNOWN when the filesystem does not
   say (or the platform lacks d_type). Values are liszt's own compact
   enum, not DT_* numbers. */
enum liszt_ftype {
    LISZT_T_UNKNOWN = 0,
    LISZT_T_FIFO,
    LISZT_T_CHR,
    LISZT_T_DIR,
    LISZT_T_BLK,
    LISZT_T_REG,
    LISZT_T_LNK,
    LISZT_T_SOCK,
    LISZT_T_WHT
};

/* Borrowed view into the reader's buffer; valid only until the next
   liszt_dirread on the same handle. Callers copy what they keep. */
struct liszt_dirent {
    const char *name;
    size_t namelen;
    enum liszt_ftype type;
    unsigned long long ino;
};

struct liszt_dir;

/* Unlike the aspen/ferret readers this one DOES return "." and "..":
   ls shows them under -a, and their position in the stream must match
   what GNU ls sees from readdir for -aU byte parity. Filtering is the
   caller's job (dirread ignore modes). */

int liszt_diropen(const char *path, struct liszt_dir **out);
int liszt_diropen_at(int parent_fd, const char *name, struct liszt_dir **out);
/* 1 = entry produced, 0 = end of directory, -1 = error (errno set). */
int liszt_dirread(struct liszt_dir *d, struct liszt_dirent *e);
int liszt_dirfd(const struct liszt_dir *d);
void liszt_dirclose(struct liszt_dir *d);

#endif

#ifndef LISZT_SYS_XSTAT_H
#define LISZT_SYS_XSTAT_H

#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* Minimal metadata contract for sprint 02's naive per-entry stat path
   (sort keys and grouping only). Sprint 03 replaces the acquisition with
   plan-masked statx and widens the struct to the full -l field set; the
   contract rule stands: every backend fills every field, renderers read
   only this struct. */
struct liszt_statinfo {
    off_t size;
    struct timespec mtime;
    mode_t mode;
};

/* lstat/stat of DIR/NAME (join handled internally, reusing one growable
   path buffer). Return 0 or -1 with errno. */
int liszt_lstat_join(const char *dir, const char *name,
                     struct liszt_statinfo *out);
int liszt_stat_join(const char *dir, const char *name,
                    struct liszt_statinfo *out);

/* "DIR/NAME" in a reused internal buffer, valid until the next call to
   any *_join function. */
const char *liszt_join_path(const char *dir, const char *name);

/* Whole-path variants for command-line operands. */
int liszt_stat_path(const char *path, struct liszt_statinfo *out);
int liszt_lstat_path(const char *path, struct liszt_statinfo *out);

#endif

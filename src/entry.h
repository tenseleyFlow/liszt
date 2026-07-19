#ifndef LISZT_ENTRY_H
#define LISZT_ENTRY_H

#include <stddef.h>
#include <stdint.h>

#include <sys/types.h>
#include <time.h>

#include "sys/dir.h"
#include "sys/xstat.h"

/* Compact per-entry record. Names live in the arena, NUL-terminated so a
   span doubles as a C string for syscalls, but name_len is authoritative:
   nothing may assume C-string semantics on name bytes. Decoration slots
   (width, quoted form, color class, stat data) are added by their owning
   sprints. */
struct liszt_entry {
    uint32_t name_off;
    uint32_t name_len;
    uint32_t meta_idx;  /* index into entries->meta, stable across sorts */
    uint8_t ftype;      /* enum liszt_ftype */
};

/* Lazily allocated per-entry metadata (the plan decides whether it is
   fetched at all). */
struct liszt_entrymeta {
    struct liszt_statinfo st;
    mode_t linkmode;        /* symlink target mode (grouping, -l ind.) */
    uint32_t link_off;      /* readlink target in the arena; UINT32_MAX
                               = none/unread */
    unsigned char stat_ok;
    unsigned char acl;      /* 0 none, 1 context-only '.', 2 acl '+' */
};

/* Per-directory entry list: one byte arena plus a record array, both
   grown geometrically, both reusable across directories via _clear. */
struct liszt_entries {
    unsigned char *arena;
    size_t arena_len;
    size_t arena_cap;
    struct liszt_entry *v;
    size_t len;
    size_t cap;
    struct liszt_entrymeta *meta;   /* NULL until ensure_meta */
    size_t meta_cap;
};

void liszt_entries_init(struct liszt_entries *es);
void liszt_entries_clear(struct liszt_entries *es);   /* keep capacity */
void liszt_entries_free(struct liszt_entries *es);
void liszt_entries_add(struct liszt_entries *es, const char *name,
                       size_t len, enum liszt_ftype type);

/* Allocate (zeroed) meta slots for the current entries; link_off slots
   start at UINT32_MAX. */
void liszt_entries_ensure_meta(struct liszt_entries *es);

/* Append LEN bytes plus a NUL to the arena, returning the offset
   (symlink targets ride the same arena as names). */
uint32_t liszt_entries_add_bytes(struct liszt_entries *es, const char *p,
                                 size_t len);

static inline const char *
liszt_entry_name(const struct liszt_entries *es, const struct liszt_entry *e)
{
    return (const char *)es->arena + e->name_off;
}

#endif

#ifndef LISZT_ENTRY_H
#define LISZT_ENTRY_H

#include <stddef.h>
#include <stdint.h>

#include "sys/dir.h"

/* Compact per-entry record. Names live in the arena, NUL-terminated so a
   span doubles as a C string for syscalls, but name_len is authoritative:
   nothing may assume C-string semantics on name bytes. Decoration slots
   (width, quoted form, color class, stat data) are added by their owning
   sprints. */
struct liszt_entry {
    uint32_t name_off;
    uint32_t name_len;
    uint8_t ftype;      /* enum liszt_ftype */
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
};

void liszt_entries_init(struct liszt_entries *es);
void liszt_entries_clear(struct liszt_entries *es);   /* keep capacity */
void liszt_entries_free(struct liszt_entries *es);
void liszt_entries_add(struct liszt_entries *es, const char *name,
                       size_t len, enum liszt_ftype type);

static inline const char *
liszt_entry_name(const struct liszt_entries *es, const struct liszt_entry *e)
{
    return (const char *)es->arena + e->name_off;
}

#endif

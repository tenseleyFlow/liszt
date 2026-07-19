#include "entry.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

void
liszt_entries_init(struct liszt_entries *es)
{
    memset(es, 0, sizeof *es);
}

void
liszt_entries_clear(struct liszt_entries *es)
{
    es->arena_len = 0;
    es->len = 0;
}

void
liszt_entries_free(struct liszt_entries *es)
{
    free(es->arena);
    free(es->v);
    free(es->meta);
    memset(es, 0, sizeof *es);
}

void
liszt_entries_ensure_meta(struct liszt_entries *es)
{
    if (es->meta_cap < es->len) {
        es->meta = liszt_xrealloc(es->meta, es->len * sizeof *es->meta);
        es->meta_cap = es->len;
    }
    memset(es->meta, 0, es->len * sizeof *es->meta);
}

void
liszt_entries_add(struct liszt_entries *es, const char *name, size_t len,
                  enum liszt_ftype type)
{
    /* uint32_t offsets cap a directory's name bytes at 4 GiB; ls never
       gets near this (name limit 255, entry counts in the millions). */
    if (len >= UINT32_MAX || es->arena_len + len + 1 > UINT32_MAX)
        liszt_die(LISZT_STATUS_SERIOUS, 0, "directory too large");

    if (es->arena_len + len + 1 > es->arena_cap) {
        size_t want = es->arena_len + len + 1;
        size_t cap = es->arena_cap ? es->arena_cap : 64 * 1024;
        while (cap < want)
            cap += cap / 2;
        es->arena = liszt_xrealloc(es->arena, cap);
        es->arena_cap = cap;
    }
    if (es->len == es->cap) {
        size_t cap = es->cap ? es->cap + es->cap / 2 : 256;
        es->v = liszt_xrealloc(es->v, cap * sizeof *es->v);
        es->cap = cap;
    }

    struct liszt_entry *e = &es->v[es->len];
    e->name_off = (uint32_t)es->arena_len;
    e->name_len = (uint32_t)len;
    e->meta_idx = (uint32_t)es->len;
    e->ftype = (uint8_t)type;
    es->len++;
    memcpy(es->arena + es->arena_len, name, len);
    es->arena[es->arena_len + len] = '\0';
    es->arena_len += len + 1;
}

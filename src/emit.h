#ifndef LISZT_EMIT_H
#define LISZT_EMIT_H

#include <stddef.h>

/* Buffered stdout emitter: bytes accumulate in a growable buffer and
   drain through write(2) at a threshold, never per-entry. Write errors
   are sticky; the first errno is reported once by liszt_emit_finish.
   (GNU ls reaches the same behavior via stdio + atexit(close_stdout);
   the observable bytes and the single "write error" diagnostic match.)

   Later sprints hang dired byte accounting off this layer, so every
   output byte must flow through it. */

void liszt_emit_bytes(const void *p, size_t n);
void liszt_emit_byte(int c);
void liszt_emit_str(const char *s);
void liszt_emit_flush(void);

/* Final flush. Returns 0, or -1 with the sticky errno in *errnum. */
int liszt_emit_finish(int *errnum);

#endif

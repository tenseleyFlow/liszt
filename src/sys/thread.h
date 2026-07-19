#ifndef LISZT_SYS_THREAD_H
#define LISZT_SYS_THREAD_H

#include <stddef.h>

typedef void (*liszt_task_fn)(void *ctx, size_t task_index);

/* rank's pool, verbatim shape: run task_count invocations of fn across
   up to `threads` workers including the calling thread. Tasks must be
   independent; they are claimed with an atomic counter. Falls back to
   fewer workers, down to fully serial, if thread creation fails. */
void liszt_run_tasks(liszt_task_fn fn, void *ctx, size_t task_count,
                     size_t threads);

#endif

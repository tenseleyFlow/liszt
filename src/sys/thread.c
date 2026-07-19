#include "sys/thread.h"

#include <pthread.h>
#include <stdatomic.h>

enum {
    LISZT_MAX_WORKERS = 64
};

struct task_state {
    liszt_task_fn fn;
    void *ctx;
    size_t count;
    _Atomic size_t next;
};

static void *
task_worker(void *arg)
{
    struct task_state *state = arg;

    for (;;) {
        size_t index = atomic_fetch_add(&state->next, 1);

        if (index >= state->count)
            break;
        state->fn(state->ctx, index);
    }
    return NULL;
}

void
liszt_run_tasks(liszt_task_fn fn, void *ctx, size_t task_count,
                size_t threads)
{
    struct task_state state;
    pthread_t workers[LISZT_MAX_WORKERS];
    size_t spawned = 0;

    if (task_count == 0)
        return;
    state.fn = fn;
    state.ctx = ctx;
    state.count = task_count;
    atomic_init(&state.next, 0);

    if (threads > LISZT_MAX_WORKERS)
        threads = LISZT_MAX_WORKERS;
    size_t want = threads < task_count ? threads : task_count;
    for (size_t i = 1; i < want; i++) {
        if (pthread_create(&workers[spawned], NULL, task_worker,
                           &state) != 0)
            break;
        spawned++;
    }
    (void)task_worker(&state);
    for (size_t i = 0; i < spawned; i++)
        (void)pthread_join(workers[i], NULL);
}

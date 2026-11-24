/*
 * A pool of worker threads for the software backend's parallel
 * rasterisation (soft.c), and the one place that includes a platform
 * thread header (workers.c).
 *
 * A pool belongs to one backend and is driven from one thread at a time.
 * Pools are independent of each other.
 *
 * With JBI_NO_THREADS (JB_THREADS=OFF) the same interface is a pool that
 * is always the calling thread alone: jbi_cpu_count is 1 and
 * jbi_pool_grow never starts a thread (it returns 1).
 */
#ifndef JBI_SOFT_WORKERS_H
#define JBI_SOFT_WORKERS_H

#include "core/alloc.h"

typedef struct jbi_pool jbi_pool;

/* The host's online CPUs. */
int jbi_cpu_count(void);

/* Whether this build can start threads at all (0 with JBI_NO_THREADS). */
int jbi_pool_can_grow(void);

/* A pool of one worker, the calling thread, in memory from a (copied);
   NULL without memory. */
jbi_pool *jbi_pool_create(const jbi_allocator *a);

/* Stops and joins every worker thread, and frees the pool. Call between
   jobs. NULL is ignored. */
void jbi_pool_destroy(jbi_pool *p);

/*
 * Grows the pool to count workers (the caller is one of them; at most
 * JBI_POOL_MAX). It never shrinks; a job gives the extra workers nothing to
 * do. Returns count, or fewer if threads cannot be started. Call between
 * jobs.
 */
enum { JBI_POOL_MAX = 64 };
int jbi_pool_grow(jbi_pool *p, int count);

/* Runs fn(ctx, i) for every worker i of the pool, i = 0 on the calling
   thread, and returns when all have finished. */
void jbi_pool_run(jbi_pool *p, void (*fn)(void *ctx, int index), void *ctx);

#endif

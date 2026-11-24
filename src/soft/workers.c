/*
 * Worker threads for the software backend's parallel rasterisation
 * (workers.h).
 *
 * A job runs fn(ctx, i) on every worker i; the caller is worker 0. Jobs
 * come in bursts (each flush of the primitive queue, a few per frame), so
 * idle workers spin for a short while on the job counter before sleeping
 * on a condition variable, and the caller spins before sleeping while it
 * waits for the others. Destroying the pool raises `stop` and wakes every
 * worker as a new job would; each sees it and returns, and is joined.
 *
 * Built with JBI_NO_THREADS (CMake: JB_THREADS=OFF, forced for the web
 * build), the pool is the caller alone: it never grows past one worker,
 * runs every job on the calling thread, reports one CPU, and nothing here
 * includes a thread header or needs a thread library.
 */
#ifdef JBI_NO_THREADS

#include "soft/workers.h"

struct jbi_pool {
    jbi_allocator alloc; /* the pool's own memory */
};

int jbi_cpu_count(void)
{
    return 1;
}

int jbi_pool_can_grow(void)
{
    return 0;
}

jbi_pool *jbi_pool_create(const jbi_allocator *a)
{
    jbi_pool *p = jbi_calloc(a, 1, sizeof *p);

    if (p != NULL)
        p->alloc = *a;
    return p;
}

void jbi_pool_destroy(jbi_pool *p)
{
    jbi_allocator alloc;

    if (p == NULL)
        return;
    alloc = p->alloc;
    jbi_free(&alloc, p);
}

int jbi_pool_grow(jbi_pool *p, int count)
{
    (void)p;
    (void)count;
    return 1;
}

void jbi_pool_run(jbi_pool *p, void (*fn)(void *ctx, int index), void *ctx)
{
    (void)p;
    fn(ctx, 0);
}

#else

#define _GNU_SOURCE
#include "soft/workers.h"

#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

enum { SPIN = 4000 };

typedef struct {
    struct jbi_pool *pool;
    int index;
    unsigned start_generation; /* the last job before it started */
} Worker;

struct jbi_pool {
    jbi_allocator alloc; /* the pool's own memory */
    int count; /* workers, the caller included */
    pthread_t threads[JBI_POOL_MAX];
    Worker workers[JBI_POOL_MAX];
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_cond_t done;
    atomic_uint generation;
    atomic_int remaining;
    atomic_int stop;
    int sleeping_caller;
    void (*job_fn)(void *ctx, int index);
    void *job_ctx;
};

static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

static void finish_one(jbi_pool *p)
{
    if (atomic_fetch_sub_explicit(&p->remaining, 1, memory_order_acq_rel) == 1) {
        pthread_mutex_lock(&p->lock);
        if (p->sleeping_caller) {
            pthread_cond_signal(&p->done);
        }
        pthread_mutex_unlock(&p->lock);
    }
}

static void *worker_main(void *arg)
{
    Worker *w = arg;
    jbi_pool *p = w->pool;
    unsigned seen = w->start_generation;

    for (;;) {
        unsigned gen;
        int spins = 0;

        while ((gen = atomic_load_explicit(&p->generation, memory_order_acquire)) == seen) {
            if (++spins < SPIN) {
                cpu_relax();
                continue;
            }
            pthread_mutex_lock(&p->lock);
            while (atomic_load_explicit(&p->generation, memory_order_acquire) == seen) {
                pthread_cond_wait(&p->wake, &p->lock);
            }
            pthread_mutex_unlock(&p->lock);
        }
        seen = gen;
        if (atomic_load_explicit(&p->stop, memory_order_acquire))
            break;
        p->job_fn(p->job_ctx, w->index);
        finish_one(p);
    }
    return NULL;
}

int jbi_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : (int)n;
}

int jbi_pool_can_grow(void)
{
    return 1;
}

jbi_pool *jbi_pool_create(const jbi_allocator *a)
{
    jbi_pool *p = jbi_calloc(a, 1, sizeof *p);

    if (p == NULL)
        return NULL;
    p->alloc = *a;
    p->count = 1;
    if (pthread_mutex_init(&p->lock, NULL) != 0) {
        jbi_free(a, p);
        return NULL;
    }
    if (pthread_cond_init(&p->wake, NULL) != 0) {
        pthread_mutex_destroy(&p->lock);
        jbi_free(a, p);
        return NULL;
    }
    if (pthread_cond_init(&p->done, NULL) != 0) {
        pthread_cond_destroy(&p->wake);
        pthread_mutex_destroy(&p->lock);
        jbi_free(a, p);
        return NULL;
    }
    atomic_init(&p->generation, 0);
    atomic_init(&p->remaining, 0);
    atomic_init(&p->stop, 0);
    return p;
}

void jbi_pool_destroy(jbi_pool *p)
{
    jbi_allocator alloc;
    int i;

    if (p == NULL)
        return;
    if (p->count > 1) {
        atomic_store_explicit(&p->stop, 1, memory_order_release);
        pthread_mutex_lock(&p->lock);
        atomic_fetch_add_explicit(&p->generation, 1, memory_order_release);
        pthread_cond_broadcast(&p->wake);
        pthread_mutex_unlock(&p->lock);
        for (i = 1; i < p->count; ++i)
            pthread_join(p->threads[i], NULL);
    }
    pthread_cond_destroy(&p->done);
    pthread_cond_destroy(&p->wake);
    pthread_mutex_destroy(&p->lock);
    alloc = p->alloc;
    jbi_free(&alloc, p);
}

int jbi_pool_grow(jbi_pool *p, int count)
{
    if (count > JBI_POOL_MAX) {
        count = JBI_POOL_MAX;
    }
    if (count <= p->count) {
        return count < 1 ? 1 : count;
    }
    for (int i = p->count; i < count; ++i) {
        Worker *w = &p->workers[i];

        w->pool = p;
        w->index = i;
        w->start_generation = atomic_load_explicit(&p->generation, memory_order_relaxed);
        if (pthread_create(&p->threads[i], NULL, worker_main, w) != 0) {
            count = i;
            break;
        }
        p->count = i + 1;
    }
    return p->count < count ? p->count : count;
}

void jbi_pool_run(jbi_pool *p, void (*fn)(void *ctx, int index), void *ctx)
{
    int spins = 0;

    if (p->count <= 1) {
        fn(ctx, 0);
        return;
    }
    p->job_fn = fn;
    p->job_ctx = ctx;
    atomic_store_explicit(&p->remaining, p->count - 1, memory_order_relaxed);
    pthread_mutex_lock(&p->lock);
    atomic_fetch_add_explicit(&p->generation, 1, memory_order_release);
    pthread_cond_broadcast(&p->wake);
    pthread_mutex_unlock(&p->lock);

    fn(ctx, 0);

    while (atomic_load_explicit(&p->remaining, memory_order_acquire) > 0) {
        if (++spins < SPIN) {
            cpu_relax();
            continue;
        }
        pthread_mutex_lock(&p->lock);
        p->sleeping_caller = 1;
        while (atomic_load_explicit(&p->remaining, memory_order_acquire) > 0) {
            pthread_cond_wait(&p->done, &p->lock);
        }
        p->sleeping_caller = 0;
        pthread_mutex_unlock(&p->lock);
    }
}

#endif /* JBI_NO_THREADS */

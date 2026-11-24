/*
 * Contexts come and go without leaving anything behind: many are created,
 * configured with worker threads and a scaled VRAM, made to draw (so their
 * workers run, and some are left long enough to fall asleep), and
 * destroyed, alone and several alive at once. After every destroy the
 * process has exactly the threads it had before (the seed's workers were
 * never joined); under AddressSanitizer, LeakSanitizer checks at exit that
 * nothing was leaked.
 *
 * Threads are counted in /proc/self/task, so that part needs Linux; without
 * it the test only creates and destroys.
 */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "core/context.h"

static int failures, checks;

static void check(const char *what, long long got, long long want)
{
    ++checks;
    if (got != want) {
        ++failures;
        if (failures <= 20)
            printf("FAIL %s: got %lld, want %lld\n", what, got, want);
    }
}

/* The process's threads, or -1 if they cannot be counted. */
static int thread_count(void)
{
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    int n = 0;

    if (d == NULL)
        return -1;
    while ((e = readdir(d)) != NULL)
        n += e->d_name[0] != '.';
    closedir(d);
    return n;
}

static void nap(long ms)
{
    struct timespec ts = {0, ms * 1000000L};

    nanosleep(&ts, NULL);
}

/*
 * The thread count once it reaches want, or after 2 s. pthread_join returns
 * when the kernel clears the thread's id, which is shortly before the
 * exiting thread leaves /proc/self/task.
 */
static int threads_settled(int want)
{
    int n = thread_count(), k;

    for (k = 0; k < 2000 && n != want; ++k) {
        nap(1);
        n = thread_count();
    }
    return n;
}

static uint32_t xy(int x, int y)
{
    return ((uint32_t)x & 0xffff) | ((uint32_t)y & 0xffff) << 16;
}

/* A clear and two Gouraud triangles over most of the drawing area, so
   every worker has rows to draw. */
static void draw(jb_context *ctx, int k)
{
    uint32_t w[16];
    int n = 0;

    w[n++] = 0xe3000000u;
    w[n++] = 0xe4000000u | 639u | 479u << 10;
    w[n++] = 0x02000000u | (uint32_t)(k * 37 & 0xff);
    w[n++] = xy(0, 0);
    w[n++] = xy(640, 480);
    w[n++] = 0x300000ffu;
    w[n++] = xy(10 + k % 7, 5);
    w[n++] = 0x0000ff00u;
    w[n++] = xy(630, 40);
    w[n++] = 0x00ff0000u;
    w[n++] = xy(100, 470);
    check("words consumed", jbi_gp0(ctx, w, n), n);
    jbi_flush(ctx);
}

int main(void)
{
    enum { ROUNDS = 64, ALIVE = 4 };
    int base, k, i;
    jb_context *alive[ALIVE];
    jbi_config cfg;

    /* The baseline after one context with workers has come and gone, so
       threads a sanitizer's runtime starts with the first thread are in
       it. */
    jbi_config_default(&cfg);
    cfg.threads = 2;
    alive[0] = jbi_context_create(&cfg);
    if (alive[0] == NULL)
        return 2;
    draw(alive[0], 0);
    jbi_context_destroy(alive[0]);
    nap(50);
    base = thread_count();
    for (k = 0; k < ROUNDS; ++k) {
        jb_context *ctx;

        cfg.scale = 1 + k % 3;
        cfg.threads = 1 + k % 5;
        ctx = jbi_context_create(&cfg);
        if (ctx == NULL) {
            printf("FAIL: no context\n");
            return 1;
        }
        check("scale", jbi_scale(ctx), cfg.scale);
        check("threads", jbi_threads(ctx), cfg.threads);
        if (k % 4 != 3)
            draw(ctx, k);
        if (k % 8 == 5) {
            /* More threads later, then fewer: the pool only grows. */
            jbi_configure(ctx, 2, -1, 6);
            draw(ctx, k);
            jbi_configure(ctx, 1, -1, 2);
            draw(ctx, k);
        }
        if (k % 16 == 7)
            nap(20); /* idle workers stop spinning and sleep */
        jbi_context_destroy(ctx);
        if (base > 0)
            check("threads after destroying a context", threads_settled(base), base);
    }

    /* Several at once, destroyed in another order than created. */
    for (k = 0; k < 8; ++k) {
        for (i = 0; i < ALIVE; ++i) {
            cfg.scale = 1 + (i + k) % 2;
            cfg.threads = 2 + i;
            alive[i] = jbi_context_create(&cfg);
            if (alive[i] == NULL) {
                printf("FAIL: no context\n");
                return 1;
            }
            draw(alive[i], i);
        }
        if (base > 0)
            check("threads with contexts alive", threads_settled(base + 1 + 2 + 3 + 4),
                  base + 1 + 2 + 3 + 4);
        for (i = 0; i < ALIVE; ++i)
            jbi_context_destroy(alive[(i * 3 + k) % ALIVE]);
        if (base > 0)
            check("threads after destroying them all", threads_settled(base), base);
    }
    jbi_context_destroy(NULL);

    if (base <= 0)
        printf("context_lifecycle: /proc/self/task not readable, threads not counted\n");
    printf("context_lifecycle: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}

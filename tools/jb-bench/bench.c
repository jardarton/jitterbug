/*
 * jb-bench: how long a frame takes, for fixed workloads (workload.h) at
 * each internal scale and thread count, with the options off (the
 * console's look) and with precise vertices and perspective-correct
 * textures on. C99, the public API only.
 *
 * What is timed is what a program pays for a frame: from its first call
 * (jb_gte_begin_frame) to having the RGBA frame from jb_frame, with the
 * program's own work (GTE plumbing, sorting) included. Between the last
 * drawing call and jb_frame a one-pixel jb_vram_read of a VRAM corner no
 * workload touches waits for the queued primitives to be drawn, so the
 * time jb_frame then takes (scanout and the RGBA conversion) can be shown
 * on its own; jb_frame would do that wait itself, so the total is the
 * same. Frames are drawn in a fixed order (frame k of a run is frame
 * (37 k + 11) mod 360 of the workload's animation), after a warm-up, so
 * every configuration draws the same frames. Timings are observations:
 * nothing here passes or fails on speed.
 *
 *   jb-bench [options]
 *     --workloads LIST  first-light,heavy,fill (all)
 *     --scales LIST     1,2,3,4,6,8 (1,2,3 under emscripten)
 *     --threads LIST    1,2,4,8,0; 0 is jb_config's one per CPU (1 under
 *                       emscripten, which is single-threaded)
 *     --options LIST    off,on: off is the console's look, on is precise
 *                       vertices + perspective-correct textures (off,on)
 *     --frames N        timed frames per configuration (120)
 *     --warmup N        frames drawn first, untimed (10)
 *     --max-time S      stop a configuration after S seconds once it has
 *                       30 frames (10)
 *     --markdown, --csv the results in that form (default: text tables)
 *     --stats           only measure the workloads (primitives, areas,
 *                       overdraw) and print that
 *     --quick           --scales 1,2 --threads 1,0 --frames 20 --warmup 3
 *     --check           draw a few frames of each workload at a few scales,
 *                       thread counts and options and compare their hashes
 *                       with the ones stored here; exit status 1 on any
 *                       difference. Times nothing.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>

#include "workload.h"

#ifndef JB_BENCH_BUILD
#define JB_BENCH_BUILD "unknown"
#endif

enum { MAX_LIST = 16, MAX_RESULTS = 512, SEQ_PERIOD = 360, STAT_FRAMES = 360 };

static const wl_info *const all_workloads[] = {&wl_first_light, &wl_heavy, &wl_fill};
enum { NWORKLOADS = sizeof all_workloads / sizeof all_workloads[0] };

static const char *const option_names[2] = {"off", "on"};

typedef struct {
    int workload, options, scale, threads;
    int frames;
    double median, p10, p90;      /* total ms */
    double frame_median;          /* jb_frame's own ms */
    double fps;
} result;

static double now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static unsigned seq_frame(int k)
{
    return (unsigned)(k * 37 + 11) % SEQ_PERIOD;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* The q-quantile of sorted v (linear between ranks). */
static double quantile(const double *v, int n, double q)
{
    double at = q * (n - 1);
    int i = (int)at;

    if (i >= n - 1)
        return v[n - 1];
    return v[i] + (v[i + 1] - v[i]) * (at - i);
}

static unsigned long long fnv1a(unsigned long long h, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static const unsigned long long fnv_basis = 0xcbf29ce484222325ull;

static int cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : (int)n;
}

/* ------------------------------------------------------------------------ */
/* Running */

static workload *start(int wi, int options, int scale, int threads)
{
    wl_options o;
    jb_result r;
    workload *w;

    o.scale = scale;
    o.threads = threads;
    o.precise = o.persp = options;
    o.log = NULL;
    o.log_user = NULL;
    w = all_workloads[wi]->create(&o, &r);
    if (w == NULL)
        fprintf(stderr, "jb-bench: %s at %dx, %d threads: cannot start: %s\n",
                all_workloads[wi]->name, scale, threads, jb_result_string(r));
    return w;
}

/* One frame; *rgba and its size from jb_frame. 0, or 1 on an error. */
static int draw(workload *w, unsigned frame, wl_stats *st, const uint8_t **rgba, size_t *bytes)
{
    jb_result r = w->draw(w, frame, st);
    int fw, fh;

    if (r != JB_OK) {
        fprintf(stderr, "jb-bench: frame %u: %s (%s)\n", frame, jb_result_string(r),
                jb_error_string(w->jb));
        return 1;
    }
    *rgba = jb_frame(w->jb, &fw, &fh);
    if (*rgba == NULL) {
        fprintf(stderr, "jb-bench: frame %u: no frame (%s)\n", frame, jb_error_string(w->jb));
        return 1;
    }
    *bytes = (size_t)fw * (size_t)fh * 4;
    return 0;
}

/* A count of 1 to max in decimal digits alone, or 0. */
static int parse_count(const char *text, long max)
{
    long v;

    if (text[0] == '\0' || strlen(text) > 9 || strspn(text, "0123456789") != strlen(text))
        return 0;
    v = strtol(text, NULL, 10);
    return v >= 1 && v <= max ? (int)v : 0;
}

static int run(int wi, int options, int scale, int threads, int frames, int warmup,
               double max_time, result *res)
{
    workload *w = start(wi, options, scale, threads);
    double *total, *fr, began;
    int k, n = 0, fail = 0;

    if (w == NULL)
        return 1;
    total = malloc((size_t)frames * sizeof *total);
    fr = malloc((size_t)frames * sizeof *fr);
    if (total == NULL || fr == NULL) {
        free(total);
        free(fr);
        w->destroy(w);
        return 1;
    }
    for (k = 0; k < warmup && !fail; ++k) {
        const uint8_t *px;
        size_t bytes;
        fail = draw(w, seq_frame(k), NULL, &px, &bytes);
    }
    began = now_ms();
    for (k = 0; k < frames && !fail; ++k) {
        double t0, t1, t2;
        const uint8_t *px;
        uint16_t probe;
        jb_result r;
        int fw, fh;

        t0 = now_ms();
        r = w->draw(w, seq_frame(k), NULL);
        /* Waits for the queued primitives (see the top of the file). */
        if (r == JB_OK)
            r = jb_vram_read(w->jb, 1023, 511, 1, 1, &probe);
        t1 = now_ms();
        px = jb_frame(w->jb, &fw, &fh);
        t2 = now_ms();
        if (r != JB_OK || px == NULL) {
            fprintf(stderr, "jb-bench: frame %u failed: %s\n", seq_frame(k), jb_error_string(w->jb));
            fail = 1;
            break;
        }
        total[n] = t2 - t0;
        fr[n] = t2 - t1;
        ++n;
        if (n >= 30 && t2 - began > max_time * 1e3)
            break;
    }
    w->destroy(w);
    if (!fail) {
        qsort(total, (size_t)n, sizeof *total, cmp_double);
        qsort(fr, (size_t)n, sizeof *fr, cmp_double);
        res->workload = wi;
        res->options = options;
        res->scale = scale;
        res->threads = threads;
        res->frames = n;
        res->median = quantile(total, n, 0.5);
        res->p10 = quantile(total, n, 0.1);
        res->p90 = quantile(total, n, 0.9);
        res->frame_median = quantile(fr, n, 0.5);
        res->fps = 1e3 / res->median;
    }
    free(total);
    free(fr);
    return fail;
}

/* ------------------------------------------------------------------------ */
/* Statistics */

static int measure(int wi, wl_stats *st)
{
    workload *w = start(wi, 0, 1, 1);
    int k, fail = 0;

    memset(st, 0, sizeof *st);
    if (w == NULL)
        return 1;
    for (k = 0; k < STAT_FRAMES && !fail; ++k) {
        const uint8_t *px;
        size_t bytes;
        fail = draw(w, (unsigned)k, st, &px, &bytes);
    }
    w->destroy(w);
    return fail;
}

static void print_stats(int wi, const wl_stats *st, int markdown)
{
    double f = (double)st->frames, screen = (double)WL_WIDTH * WL_HEIGHT;
    double px = (double)(st->tri_pixels + st->rect_pixels + st->line_pixels);

    if (markdown)
        printf("| %s | %.0f (%ld-%ld) | %.0f | %.0f | %.0f%% / %.0f%% / %.0f%% | %.0f | %.0f | %.0f | "
               "%.0f | %.1f | %.2f |\n",
               all_workloads[wi]->name, st->prims / f, st->min_prims, st->max_prims,
               st->triangles / f, st->tri_visible / f,
               st->textured ? 100.0 * st->tex4 / st->textured : 0.0,
               st->textured ? 100.0 * st->tex8 / st->textured : 0.0,
               st->textured ? 100.0 * st->tex15 / st->textured : 0.0, st->blended / f,
               st->rects / f, st->lines / f,
               st->tri_visible ? (double)st->tri_pixels / st->tri_visible : 0.0,
               st->rects ? (double)st->rect_pixels / st->rects : 0.0, px / f / screen);
    else
        printf("%-12s %6.0f prims/frame (%ld-%ld): %.0f triangles (%.0f on screen, %.0f px each,\n"
               "             textured %ld%%: 4-bit %.0f%%, 8-bit %.0f%%, 15-bit %.0f%%), %.0f rects "
               "(%.1f px each),\n"
               "             %.0f lines, %.0f semi-transparent; covers %.2f x the 320 x 240 screen\n",
               all_workloads[wi]->name, st->prims / f, st->min_prims, st->max_prims,
               st->triangles / f, st->tri_visible / f,
               st->tri_visible ? (double)st->tri_pixels / st->tri_visible : 0.0,
               st->triangles ? 100 * st->textured / st->triangles : 0L,
               st->textured ? 100.0 * st->tex4 / st->textured : 0.0,
               st->textured ? 100.0 * st->tex8 / st->textured : 0.0,
               st->textured ? 100.0 * st->tex15 / st->textured : 0.0, st->rects / f,
               st->rects ? (double)st->rect_pixels / st->rects : 0.0, st->lines / f,
               st->blended / f, px / f / screen);
}

/* ------------------------------------------------------------------------ */
/* --check */

typedef struct {
    int workload, options, scale, threads, frames;
    unsigned long long hash;
} check_run;

/* first-light's 30-frame 1x run is tests/console_scene/expected.txt's
   "short" (the console-level first-light scene, the example as it was
   first written), so the bench draws that scene as its runner does. Each run at
   more than one thread must hash as the same run at one does. To
   re-record after a deliberate change, run --check and copy the hashes it
   prints. */
static const check_run checks[] = {
    {0, 0, 1, 1, 30, 0x429c447a5d216d7eull},
    {0, 1, 2, 1, 4, 0x02b4d90494dea381ull},
    {0, 1, 2, 3, 4, 0x02b4d90494dea381ull},
    {1, 0, 1, 1, 4, 0x17013c40ff6575acull},
    {1, 0, 1, 3, 4, 0x17013c40ff6575acull},
    {1, 1, 2, 1, 3, 0x475c66cfe1ae3547ull},
    {1, 1, 2, 3, 3, 0x475c66cfe1ae3547ull},
    {2, 0, 1, 1, 3, 0x9afccabf806edc22ull},
    {2, 1, 3, 2, 2, 0x227ed76eb1c7ef4bull},
};

static int check(void)
{
    int i, failures = 0;

    for (i = 0; i < (int)(sizeof checks / sizeof checks[0]); ++i) {
        const check_run *c = &checks[i];
        workload *w = start(c->workload, c->options, c->scale, c->threads);
        unsigned long long h = fnv_basis;
        int k, fail = 0;

        if (w == NULL)
            return 1;
        for (k = 0; k < c->frames && !fail; ++k) {
            const uint8_t *px;
            size_t bytes;
            if ((fail = draw(w, (unsigned)k, NULL, &px, &bytes)) == 0)
                h = fnv1a(h, px, bytes);
        }
        w->destroy(w);
        if (fail)
            return 1;
        printf("%-12s options %-3s %dx %d thread%s %2d frames: %016llx %s\n",
               all_workloads[c->workload]->name, option_names[c->options], c->scale, c->threads,
               c->threads == 1 ? " " : "s", c->frames, h, h == c->hash ? "ok" : "FAIL");
        if (h != c->hash) {
            printf("    want %016llx\n", c->hash);
            ++failures;
        }
    }
    return failures != 0;
}

/* ------------------------------------------------------------------------ */
/* Output */

static void print_machine(int markdown, int csv)
{
    const char *lead = csv ? "# " : markdown ? "- " : "";
    struct utsname u;
    char line[256], cpu[160] = "unknown";
    FILE *fp;

    if ((fp = fopen("/proc/cpuinfo", "r")) != NULL) {
        while (fgets(line, sizeof line, fp) != NULL)
            if (strncmp(line, "model name", 10) == 0) {
                const char *v = strchr(line, ':');
                if (v != NULL) {
                    v += 1 + (v[1] == ' ');
                    strncpy(cpu, v, sizeof cpu - 1);
                    cpu[strcspn(cpu, "\n")] = '\0';
                }
                break;
            }
        fclose(fp);
    }
    printf("%sjitterbug %s, jb-bench built %s\n", lead, jb_version_string(), JB_BENCH_BUILD);
#if defined(__clang__)
    printf("%scompiler: clang %s\n", lead, __clang_version__);
#elif defined(__GNUC__)
    printf("%scompiler: gcc %s\n", lead, __VERSION__);
#endif
    printf("%sCPU: %s, %d logical CPUs online\n", lead, cpu, cpu_count());
    if (uname(&u) == 0)
        printf("%ssystem: %s %s %s\n", lead, u.sysname, u.release, u.machine);
    if ((fp = fopen("/proc/loadavg", "r")) != NULL) {
        if (fgets(line, sizeof line, fp) != NULL) {
            line[strcspn(line, "\n")] = '\0';
            printf("%sload average before: %s\n", lead, line);
        }
        fclose(fp);
    }
}

static void print_load_after(int markdown, int csv)
{
    char line[256];
    FILE *fp = fopen("/proc/loadavg", "r");

    if (fp == NULL)
        return;
    if (fgets(line, sizeof line, fp) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        printf("%sload average after: %s\n", csv ? "# " : markdown ? "- " : "", line);
    }
    fclose(fp);
}

static void thread_label(int t, char *buf, size_t n)
{
    if (t == 0)
        snprintf(buf, n, "auto (%d)", cpu_count() > 16 ? 16 : cpu_count());
    else
        snprintf(buf, n, "%d thread%s", t, t == 1 ? "" : "s");
}

static const result *find(const result *r, int n, int wi, int opt, int scale, int threads)
{
    int i;

    for (i = 0; i < n; ++i)
        if (r[i].workload == wi && r[i].options == opt && r[i].scale == scale &&
            r[i].threads == threads)
            return &r[i];
    return NULL;
}

static void print_tables(const result *r, int nr, const int *wls, int nwl, const int *opts,
                         int nopt, const int *scales, int nscale, const int *threads, int nthr,
                         int markdown)
{
    int a, b, i, j;
    char label[32];

    for (a = 0; a < nwl; ++a)
        for (b = 0; b < nopt; ++b) {
            int wi = wls[a], opt = opts[b];

            if (markdown) {
                printf("\n#### %s, options %s\n\n", all_workloads[wi]->name,
                       opt ? "on (precise vertices + perspective-correct)" : "off (console)");
                printf("Median ms per frame (10th-90th percentile) and fps from the median.\n\n");
                printf("| scale |");
                for (j = 0; j < nthr; ++j) {
                    thread_label(threads[j], label, sizeof label);
                    printf(" %s |", label);
                }
                printf("\n|---|");
                for (j = 0; j < nthr; ++j)
                    printf("---|");
                printf("\n");
            } else {
                printf("\n%s, options %s: median ms (p10-p90) fps\n", all_workloads[wi]->name,
                       option_names[opt]);
                printf("%5s", "scale");
                for (j = 0; j < nthr; ++j) {
                    thread_label(threads[j], label, sizeof label);
                    printf(" %26s", label);
                }
                printf("\n");
            }
            for (i = 0; i < nscale; ++i) {
                printf(markdown ? "| %dx |" : "%4dx", scales[i]);
                for (j = 0; j < nthr; ++j) {
                    const result *x = find(r, nr, wi, opt, scales[i], threads[j]);
                    char cell[64];

                    if (x == NULL)
                        snprintf(cell, sizeof cell, "-");
                    else
                        snprintf(cell, sizeof cell, "%.2f (%.2f-%.2f) %.0f", x->median, x->p10,
                                 x->p90, x->fps);
                    printf(markdown ? " %s |" : " %26s", cell);
                }
                printf("\n");
            }
            /* jb_frame's share: scanout and the RGBA conversion. */
            if (markdown) {
                printf("\njb_frame alone (scanout and RGBA), median ms and share of the frame:\n\n");
                printf("| scale |");
                for (j = 0; j < nthr; ++j) {
                    thread_label(threads[j], label, sizeof label);
                    printf(" %s |", label);
                }
                printf("\n|---|");
                for (j = 0; j < nthr; ++j)
                    printf("---|");
                printf("\n");
            } else {
                printf("  jb_frame alone, median ms (share)\n");
            }
            for (i = 0; i < nscale; ++i) {
                printf(markdown ? "| %dx |" : "%4dx", scales[i]);
                for (j = 0; j < nthr; ++j) {
                    const result *x = find(r, nr, wi, opt, scales[i], threads[j]);
                    char cell[64];

                    if (x == NULL)
                        snprintf(cell, sizeof cell, "-");
                    else
                        snprintf(cell, sizeof cell, "%.2f (%.0f%%)", x->frame_median,
                                 100 * x->frame_median / x->median);
                    printf(markdown ? " %s |" : " %26s", cell);
                }
                printf("\n");
            }
        }
}

/* ------------------------------------------------------------------------ */
/* Options */

static int parse_list(const char *s, int *out, int max)
{
    int n = 0;

    while (*s != '\0' && n < max) {
        char *end;
        long v = strtol(s, &end, 10);
        if (end == s)
            return -1;
        out[n++] = (int)v;
        s = *end == ',' ? end + 1 : end;
        if (*end != ',' && *end != '\0')
            return -1;
    }
    return n;
}

static int parse_names(const char *s, int *out, int max, const char *const *names, int nnames)
{
    int n = 0;

    while (*s != '\0' && n < max) {
        size_t len = strcspn(s, ",");
        int i, found = -1;

        for (i = 0; i < nnames; ++i)
            if (strlen(names[i]) == len && strncmp(s, names[i], len) == 0)
                found = i;
        if (found < 0)
            return -1;
        out[n++] = found;
        s += len + (s[len] == ',');
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *wl_names[NWORKLOADS];
    int wls[MAX_LIST] = {0, 1, 2}, nwl = 3;
    int opts[2] = {0, 1}, nopt = 2;
#ifdef __EMSCRIPTEN__
    int scales[MAX_LIST] = {1, 2, 3}, nscale = 3;
    int threads[MAX_LIST] = {1}, nthr = 1;
#else
    int scales[MAX_LIST] = {1, 2, 3, 4, 6, 8}, nscale = 6;
    int threads[MAX_LIST] = {1, 2, 4, 8, 0}, nthr = 5;
#endif
    int frames = 120, warmup = 10, markdown = 0, csv = 0, stats_only = 0, i, a, b, c, d;
    double max_time = 10;
    result *res;
    int nres = 0;

    for (i = 0; i < NWORKLOADS; ++i)
        wl_names[i] = all_workloads[i]->name;
    for (i = 1; i < argc; ++i) {
        const char *o = argv[i];
        int more = i + 1 < argc, bad = 0;

        if (strcmp(o, "--check") == 0)
            return check();
        else if (strcmp(o, "--markdown") == 0)
            markdown = 1;
        else if (strcmp(o, "--csv") == 0)
            csv = 1;
        else if (strcmp(o, "--stats") == 0)
            stats_only = 1;
        else if (strcmp(o, "--quick") == 0) {
            scales[0] = 1, scales[1] = 2, nscale = 2;
            threads[0] = 1, threads[1] = 0, nthr = 2;
#ifdef __EMSCRIPTEN__
            nthr = 1;
#endif
            frames = 20, warmup = 3;
        } else if (strcmp(o, "--workloads") == 0 && more)
            bad = (nwl = parse_names(argv[++i], wls, MAX_LIST, wl_names, NWORKLOADS)) <= 0;
        else if (strcmp(o, "--options") == 0 && more)
            bad = (nopt = parse_names(argv[++i], opts, 2, option_names, 2)) <= 0;
        else if (strcmp(o, "--scales") == 0 && more)
            bad = (nscale = parse_list(argv[++i], scales, MAX_LIST)) <= 0;
        else if (strcmp(o, "--threads") == 0 && more)
            bad = (nthr = parse_list(argv[++i], threads, MAX_LIST)) <= 0;
        else if (strcmp(o, "--frames") == 0 && more)
            bad = (frames = parse_count(argv[++i], 1000000)) < 1;
        else if (strcmp(o, "--warmup") == 0 && more)
            bad = (warmup = atoi(argv[++i])) < 0;
        else if (strcmp(o, "--max-time") == 0 && more)
            bad = (max_time = atof(argv[++i])) <= 0;
        else
            bad = 1;
        if (bad) {
            fprintf(stderr, "jb-bench: bad or incomplete option %s (see the top of bench.c)\n", o);
            return 2;
        }
    }

    print_machine(markdown, csv);
    printf(csv ? "# " : markdown ? "- " : "");
    printf("frames: %d timed per configuration after %d warm-up (at most %.0f s each once 30 "
           "are done)\n",
           frames, warmup, max_time);

    /* What the workloads draw, measured at 1x over frames 0..359. */
    if (!csv) {
        printf(markdown ? "\n| workload | prims/frame (min-max) | triangles | on screen | textured "
                          "4/8/15-bit | semi-transparent | rects | lines | px/triangle | "
                          "px/rect | overdraw |\n|---|---|---|---|---|---|---|---|---|---|---|\n"
                        : "\n");
        for (a = 0; a < nwl; ++a) {
            wl_stats st;
            if (measure(wls[a], &st) != 0)
                return 1;
            print_stats(wls[a], &st, markdown);
        }
    }
    if (stats_only)
        return 0;

    res = calloc(MAX_RESULTS, sizeof *res);
    if (res == NULL)
        return 1;
    if (csv)
        printf("workload,options,scale,threads,frames,median_ms,p10_ms,p90_ms,fps,"
               "jb_frame_median_ms\n");
    for (a = 0; a < nwl; ++a)
        for (b = 0; b < nopt; ++b)
            for (c = 0; c < nscale; ++c)
                for (d = 0; d < nthr && nres < MAX_RESULTS; ++d) {
                    result *x = &res[nres];

                    if (run(wls[a], opts[b], scales[c], threads[d], frames, warmup, max_time, x) != 0)
                        return 1;
                    ++nres;
                    fprintf(stderr, "%-12s %-3s %dx %2d threads: %8.2f ms (%.2f-%.2f), %d frames\n",
                            all_workloads[x->workload]->name, option_names[x->options], x->scale,
                            x->threads, x->median, x->p10, x->p90, x->frames);
                    if (csv)
                        printf("%s,%s,%d,%d,%d,%.3f,%.3f,%.3f,%.1f,%.3f\n",
                               all_workloads[x->workload]->name, option_names[x->options],
                               x->scale, x->threads, x->frames, x->median, x->p10, x->p90, x->fps,
                               x->frame_median);
                }
    if (!csv)
        print_tables(res, nres, wls, nwl, opts, nopt, scales, nscale, threads, nthr, markdown);
    printf(markdown ? "\n" : "");
    print_load_after(markdown, csv);
    free(res);
    return 0;
}

/*
 * seed_replay: replays the seed's GPU test scenes recorded in tests/seed
 * (format: tests/seed/FORMAT.md) against a jitterbug context
 * (src/core/context.h) with the software backend, and checks every result
 * the recording holds: VRAM hashes at each CAPTURE (1x and scaled), STORE
 * data, PRESENT frames (built by FORMAT.md's algorithm and by jbi_scanout,
 * which must agree), the fake precise table's counters, GP0 and CONFIGURE
 * returns, and at the end of each scene the golden PNGs, pixel for pixel.
 *
 * The traces name the seed's functions. They are replayed as:
 *   GP0, FILL, LOAD, STORE, COPY, FLUSH, RESET_STATE   jbi_gp0, jbi_fill,
 *       jbi_load, jbi_store, jbi_copy, jbi_flush, jbi_reset_state
 *   CONFIGURE, SET_EXACT_1X, SET_TEXTURE_MAPPING   jbi_configure (and
 *       jbi_threads), jbi_set_exact_1x, jbi_set_texture_mapping
 *   PRESENT   jbi_scanout_bytes, jbi_scanout
 *   VRAM_GET, HIRES_REPLICATE, LOWRES_MARK, LOWRES_RESOLVE   the software
 *       backend's jbi_soft_vram, jbi_soft_replicate, jbi_soft_lowres_mark,
 *       jbi_soft_lowres_resolve (src/soft/soft.h); CAPTURE and POKE use its
 *       raw VRAM pointers
 *   PRECISE_*   the fake table below, the context's precise source
 *   SYNC, TRACE_CALL, TRACE_TIME   nothing: the seed's draw-only passes
 *       (which gpu_sync undid) and its timing trace are gone, and these
 *       did nothing in the recording.
 *
 *   seed_replay [--threads N] TRACE.jbt
 *       one scene alone, from its prelude (captures with standalone 0 are
 *       skipped);
 *   seed_replay [--threads N] --sequence TRACE.jbt...
 *       every scene in order in this one process, every capture checked,
 *       and each prelude checked against the state the scenes left.
 *
 * --threads N passes N rasterising threads to every configuration instead
 * of the recorded count (the output must not depend on it).
 *
 * Other scales (FORMAT.md, "Other scales"), in sequence only:
 *
 *   seed_replay [--threads N] --scale S DIR --sequence TRACE.jbt...
 *       every scale above 1x that the traces configure becomes S; the
 *       results that differ from the recording at S are checked against
 *       DIR/Sx.txt (and the end-state images it lists), the rest against
 *       the traces as usual;
 *   seed_replay [--threads N] --record-scale S DIR [--png NN-name]...
 *               --sequence TRACE.jbt...
 *       writes DIR/Sx.txt, and DIR/NN-name.Sx.png for each --png scene that
 *       ends above 1x, from the GPU as built.
 *
 * Re-blessing (docs/testing.md, "Seed traces and re-blessing"), in sequence
 * at the recorded scales only:
 *
 *   seed_replay --bless DIR --sequence TRACE.jbt...
 *       replays as above, but the VRAM hashes of every CAPTURE and of the
 *       preludes, STORE pixels and PRESENT frames are taken from the GPU as
 *       built instead of checked: each scene's trace is written to
 *       DIR/NN-name.jbt with those fields rewritten (nothing else in it
 *       changes), and its end state to DIR/NN-name.1x.raw (and .Nx.raw
 *       above 1x) for vram2png.py. GP0 and CONFIGURE returns, the precise
 *       counters and everything else are still checked;
 *   --dump DIR      also writes the VRAM at every CAPTURE as
 *                   DIR/NN-name.opK.Sx.raw (S 1, and the scale for the
 *                   scaled VRAM);
 *   --compare DIR   compares the VRAM at every CAPTURE with what another
 *                   build wrote there with --dump, and prints, for each
 *                   capture that differs, the pixels that differ, their
 *                   bounding box and how they differ (per channel, in
 *                   5-bit steps; the mask bit). --dump and --compare work
 *                   in every mode, with --scale too. The two must be
 *                   different directories, and --compare's must exist and
 *                   hold every capture the replay reaches: a missing
 *                   reference, or nothing compared at all, exits with 2,
 *                   as a usage or I/O error does (1 is a mismatch).
 *
 * The trace format is read with trace.h, which api_golden.c shares.
 *
 * On a mismatch it prints what differs; for VRAM it writes the actual
 * image (and, against a golden, a diff image) as *.png.out in the current
 * directory, and prints how many pixels differ and where.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core/context.h"
#include "soft/soft.h"
#include "stb_image.h"
#include "stb_image_write.h"
#include "trace.h"

/* The context under test, and its software backend. */
static jb_context *ctx;
static jbi_soft *soft;

/* ------------------------------------------------------------------------ */
/* The fake precise-vertex table (FORMAT.md, "Fake precise-vertex table") */

enum { PRECISE_MAX = 2048 };

static struct {
    int enabled, count;
    unsigned key[PRECISE_MAX];
    jbi_precise_vertex v[PRECISE_MAX];
    unsigned lookups, hits;
} fake;

static void fake_enable(int on)
{
    fake.enabled = on != 0;
}

static void fake_begin_frame(void)
{
    fake.count = 0;
}

static void fake_record(unsigned sxy, const jbi_precise_vertex *v)
{
    int i;

    if (!fake.enabled)
        return;
    for (i = 0; i < fake.count && fake.key[i] != sxy; ++i)
        ;
    if (i < fake.count) {
        fake.v[i] = *v;
    } else if (fake.count < PRECISE_MAX) {
        fake.key[fake.count] = sxy;
        fake.v[fake.count++] = *v;
    }
}

static int fake_lookup(unsigned sxy, jbi_precise_vertex *v)
{
    int i;

    ++fake.lookups;
    for (i = 0; i < fake.count; ++i)
        if (fake.key[i] == sxy) {
            ++fake.hits;
            *v = fake.v[i];
            return 1;
        }
    return 0;
}

static unsigned fake_lookup_poly(const unsigned *sxy, int n, jbi_precise_vertex *v)
{
    unsigned mask = 0;
    int i;

    for (i = 0; i < n; ++i)
        if (fake_lookup(sxy[i], &v[i]))
            mask |= 1u << i;
    return mask;
}

/* The fake as the context's precise source (src/core/precise_source.h). */
static int source_enabled(void *user)
{
    (void)user;
    return fake.enabled;
}

static int source_lookup(void *user, uint32_t sxy, jbi_precise_vertex *v)
{
    (void)user;
    return fake_lookup(sxy, v);
}

static unsigned source_lookup_poly(void *user, const uint32_t *sxy, int n, jbi_precise_vertex *v)
{
    (void)user;
    return fake_lookup_poly(sxy, n, v);
}

static const jbi_precise_source fake_source = {source_enabled, source_lookup,
                                               source_lookup_poly, NULL};

/* A fresh context, as a fresh process held the seed's state. */
static void context_create(void)
{
    jbi_config cfg;

    jbi_config_default(&cfg);
    cfg.precise = &fake_source;
    ctx = jbi_context_create(&cfg);
    if (ctx == NULL) {
        fprintf(stderr, "seed_replay: cannot create a context\n");
        exit(2);
    }
    soft = jbi_soft_from(jbi_context_backend(ctx));
}

/* The software backend's state the traces name. */
static uint16_t *vram_1x(void)
{
    return jbi_soft_vram_raw(soft);
}

static uint16_t *vram_scaled(void)
{
    return jbi_soft_scaled_raw(soft);
}

static int cur_scale(void)
{
    return jbi_scale(ctx);
}

/* ------------------------------------------------------------------------ */
/* Hashes and images */

static uint64_t hash_1x(void)
{
    return trace_fnv_pixels(vram_1x(), (size_t)JBI_VRAM_W * JBI_VRAM_H);
}

static uint64_t hash_scaled(void)
{
    size_t n = (size_t)cur_scale() * cur_scale() * JBI_VRAM_W * JBI_VRAM_H;
    return vram_scaled() != NULL ? trace_fnv_pixels(vram_scaled(), n) : 0;
}

/* A VRAM pixel as the golden images store it (FORMAT.md, "Golden images"). */
static void to_rgba(uint16_t p, unsigned char *o)
{
    o[0] = (unsigned char)((p & 31) << 3);
    o[1] = (unsigned char)(((p >> 5) & 31) << 3);
    o[2] = (unsigned char)(((p >> 10) & 31) << 3);
    o[3] = (p & 0x8000) ? 128 : 255;
}

static unsigned char *vram_rgba(const uint16_t *pix, int w, int h)
{
    unsigned char *rgba = malloc((size_t)w * h * 4);
    size_t i;

    if (rgba != NULL)
        for (i = 0; i < (size_t)w * h; ++i)
            to_rgba(pix[i], rgba + 4 * i);
    return rgba;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s != NULL ? s + 1 : path;
}

static void write_png(const char *name, const unsigned char *rgba, int w, int h)
{
    if (rgba != NULL && stbi_write_png(name, w, h, 4, rgba, w * 4))
        printf("    wrote %s\n", name);
    else
        printf("    could not write %s\n", name);
}

/* ------------------------------------------------------------------------ */
/* Replay */

typedef struct {
    unsigned compared, matched;
} Tally;

static struct {
    Tally capture, store, present, scanout, stats, gp0, configure, prelude, golden;
    unsigned skipped_captures, skipped_goldens, scenes;
} total;

typedef struct {
    trace_reader r;
    char name[96];   /* NN-name */
    int sequence;    /* running every scene in order */
    int threads;     /* > 0: forced thread count */
    unsigned op;     /* index of the current op */
    unsigned code;
    int failures;
    int wrote_capture; /* wrote the images of a mismatching capture */
    int end_valid;     /* the scene-end capture holds in this mode */
    int at_end;        /* the last op was the scene-end capture */
    int end_1x_other;  /* --scale: the scene-end 1x VRAM is not the recorded one */
} Scene;

/* ------------------------------------------------------------------------ */
/* Other scales (FORMAT.md, "Other scales") */

enum { REF_START, REF_CAPTURE, REF_STORE, REF_PRESENT, REF_GOLDEN, REF_KINDS };

static const char *const ref_kinds[REF_KINDS] = {"start", "capture", "store", "present",
                                                 "golden"};

/* One line of a reference file. op is -1 for start and golden. */
typedef struct {
    int kind;
    char scene[96];
    long op;
    int scale, width, height, stride, format;
    int same_1x;       /* start, capture: the 1x hash is the trace's ("=") */
    uint64_t h1, hash; /* start, capture: 1x, scaled; store: data; present: frame */
    int used;
} RefLine;

static struct {
    int n;             /* the scale; 0: no override */
    int record;        /* writing DIR/Nx.txt rather than checking it */
    const char *dir;
    FILE *out;         /* recording */
    RefLine *lines;    /* checking */
    size_t count;
    const char **png;  /* recording: scenes whose end-state images to write */
    int npng;
    unsigned lines_out, checked, pngs;
} sc;

/* A scale the traces configure, under the override. */
static int map_scale(int scale)
{
    return sc.n > 0 && scale > 1 ? sc.n : scale;
}

static void ref_load(void)
{
    char path[4096], line[512], kind[16], h1[24];
    FILE *f;
    size_t cap = 0;

    snprintf(path, sizeof path, "%s/%dx.txt", sc.dir, sc.n);
    f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "seed_replay: cannot read %s\n", path);
        exit(2);
    }
    while (fgets(line, sizeof line, f) != NULL) {
        RefLine l;
        unsigned long long a = 0, b = 0;
        int k, ok;

        if (line[0] == '#' || line[0] == '\n')
            continue;
        memset(&l, 0, sizeof l);
        if (sscanf(line, "%15s %95s %ld", kind, l.scene, &l.op) != 3)
            goto bad;
        for (k = 0; k < REF_KINDS && strcmp(kind, ref_kinds[k]) != 0; ++k)
            ;
        l.kind = k;
        switch (k) {
        case REF_START:
        case REF_CAPTURE:
            ok = sscanf(line, "%*s %*s %*d %d %23s %llx", &l.scale, h1, &b) == 3;
            l.same_1x = strcmp(h1, "=") == 0;
            if (ok && !l.same_1x)
                ok = sscanf(h1, "%llx", &a) == 1;
            break;
        case REF_STORE: ok = sscanf(line, "%*s %*s %*d %llx", &b) == 1; break;
        case REF_PRESENT:
            ok = sscanf(line, "%*s %*s %*d %d %d %d %d %d %llx", &l.scale, &l.width, &l.height,
                        &l.stride, &l.format, &b) == 6;
            break;
        case REF_GOLDEN: ok = sscanf(line, "%*s %*s %*d %d", &l.scale) == 1; break;
        default: ok = 0;
        }
        if (!ok)
            goto bad;
        l.h1 = a;
        l.hash = b;
        if (sc.count == cap) {
            cap = cap ? 2 * cap : 256;
            sc.lines = realloc(sc.lines, cap * sizeof *sc.lines);
            if (sc.lines == NULL) {
                fprintf(stderr, "out of memory\n");
                exit(2);
            }
        }
        sc.lines[sc.count++] = l;
        continue;
    bad:
        fprintf(stderr, "seed_replay: %s: bad line: %s", path, line);
        exit(2);
    }
    fclose(f);
}

/* The reference line of the current op (op -1: the prelude), or NULL. */
static RefLine *ref_find(const Scene *s, int kind, long op)
{
    size_t i;

    for (i = 0; i < sc.count; ++i) {
        RefLine *l = &sc.lines[i];
        if (l->kind == kind && l->op == op && strcmp(l->scene, s->name) == 0) {
            l->used = 1;
            ++sc.checked;
            return l;
        }
    }
    return NULL;
}

static void ref_put(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(sc.out, fmt, ap);
    va_end(ap);
    ++sc.lines_out;
}

static void ref_put_hashes(const Scene *s, int kind, long op, int scale, uint64_t h1,
                           uint64_t want_1x, uint64_t hs)
{
    char one[24];

    if (h1 == want_1x)
        strcpy(one, "=");
    else
        snprintf(one, sizeof one, "%016llx", (unsigned long long)h1);
    ref_put("%s %s %ld %d %s %016llx\n", ref_kinds[kind], s->name, op, scale, one,
            (unsigned long long)hs);
}

/* ------------------------------------------------------------------------ */
/* Re-blessing, dumps and comparisons (the usage above) */

static struct {
    const char *bless, *dump, *compare;
    unsigned patched;   /* fields rewritten in the current scene */
    unsigned patched_total, compared, differed, missing;
} bl;

static void put_u64(unsigned char *p, uint64_t v)
{
    int i;

    for (i = 0; i < 8; ++i)
        p[i] = (unsigned char)(v >> (8 * i));
}

static void put_u32(unsigned char *p, uint32_t v)
{
    int i;

    for (i = 0; i < 4; ++i)
        p[i] = (unsigned char)(v >> (8 * i));
}

static void write_raw(const char *path, const uint16_t *pix, size_t n)
{
    FILE *f = fopen(path, "wb");
    size_t i;

    if (f == NULL) {
        fprintf(stderr, "seed_replay: cannot write %s\n", path);
        exit(2);
    }
    for (i = 0; i < n; ++i) {
        fputc(pix[i] & 0xff, f);
        fputc(pix[i] >> 8, f);
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "seed_replay: cannot write %s\n", path);
        exit(2);
    }
}

/* Whether two paths name the same existing directory. */
static int same_dir(const char *a, const char *b)
{
    struct stat sa, sb;

    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && S_ISDIR(sa.st_mode) &&
           sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/* n pixels from a raw dump, or NULL when there is none of that size. */
static uint16_t *read_raw(const char *path, size_t n)
{
    FILE *f = fopen(path, "rb");
    uint16_t *pix;
    size_t i;

    if (f == NULL)
        return NULL;
    pix = malloc(n * sizeof *pix);
    if (pix == NULL)
        exit(2);
    for (i = 0; i < n; ++i) {
        int lo = fgetc(f), hi = fgetc(f);
        if (lo == EOF || hi == EOF) {
            free(pix);
            fclose(f);
            return NULL;
        }
        pix[i] = (uint16_t)(lo | hi << 8);
    }
    if (fgetc(f) != EOF) {
        free(pix);
        pix = NULL;
    }
    fclose(f);
    return pix;
}

/*
 * One VRAM against another build's: the pixels that differ, their box, and
 * how: the range of each channel's change in 5-bit steps, pixels where a
 * channel moved by more than one step, and mask bit changes.
 */
static void compare_vram(const char *what, const uint16_t *old, const uint16_t *now, int w, int h)
{
    unsigned long count = 0, big = 0, mask = 0;
    int lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0}, x0 = w, y0 = h, x1 = -1, y1 = -1, x, y, c;

    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x) {
            size_t i = (size_t)y * w + x;
            int far = 0;

            if (old[i] == now[i])
                continue;
            ++count;
            x0 = x < x0 ? x : x0;
            y0 = y < y0 ? y : y0;
            x1 = x > x1 ? x : x1;
            y1 = y > y1 ? y : y1;
            for (c = 0; c < 3; ++c) {
                int d = (int)((now[i] >> (5 * c)) & 31) - (int)((old[i] >> (5 * c)) & 31);
                lo[c] = d < lo[c] ? d : lo[c];
                hi[c] = d > hi[c] ? d : hi[c];
                far |= d > 1 || d < -1;
            }
            big += far;
            mask += ((old[i] ^ now[i]) & 0x8000) != 0;
        }
    ++bl.compared;
    if (count == 0)
        return;
    ++bl.differed;
    printf("    DIFF %s: %lu pixels, box (%d, %d)-(%d, %d); steps r %+d..%+d g %+d..%+d "
           "b %+d..%+d; %lu by more than one step; %lu mask bits\n",
           what, count, x0, y0, x1, y1, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], big, mask);
}

/* --dump and --compare at a capture: the 1x VRAM and the scaled one. */
static void dump_compare(const Scene *s)
{
    char path[4096], what[160];
    int k, n = cur_scale();

    for (k = 0; k < 2; ++k) {
        const uint16_t *pix = k == 0 ? vram_1x() : vram_scaled();
        int sn = k == 0 ? 1 : n;
        size_t count = (size_t)JBI_VRAM_W * sn * JBI_VRAM_H * sn;

        if (pix == NULL)
            continue;
        if (bl.dump != NULL) {
            snprintf(path, sizeof path, "%s/%s.op%u.%dx.raw", bl.dump, s->name, s->op, sn);
            write_raw(path, pix, count);
        }
        if (bl.compare != NULL) {
            uint16_t *old;

            snprintf(path, sizeof path, "%s/%s.op%u.%dx.raw", bl.compare, s->name, s->op, sn);
            old = read_raw(path, count);
            snprintf(what, sizeof what, "%s op %u %dx", s->name, s->op, sn);
            if (old == NULL) {
                /* Not a difference: a reference that is not there. */
                printf("MISSING %s: no dump of that size in %s\n", what, bl.compare);
                ++bl.missing;
                continue;
            }
            compare_vram(what, old, pix, JBI_VRAM_W * sn, JBI_VRAM_H * sn);
            free(old);
        }
    }
}

/* What the sequence carried from scene to scene, as the recorder tracks it. */
static struct {
    int scale, dither, threads, exact_1x;
} recorded_cfg = {1, -1, 1, 0};

static void fail(Scene *s, const char *fmt, ...)
{
    va_list ap;

    if (++s->failures > 40)
        return;
    printf("FAIL %s op %u (%s): ", s->name, s->op, trace_op_name(s->code));
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static int tally(Tally *t, int ok)
{
    ++t->compared;
    t->matched += ok != 0;
    return ok;
}

static void check_int(Scene *s, Tally *t, const char *what, long long got, long long want)
{
    if (!tally(t, got == want))
        fail(s, "%s: got %lld, want %lld", what, got, want);
}

static void check_hash(Scene *s, Tally *t, const char *what, uint64_t got, uint64_t want)
{
    if (!tally(t, got == want))
        fail(s, "%s: hash %016llx, want %016llx", what, (unsigned long long)got,
             (unsigned long long)want);
}

/*
 * Compares an image of VRAM with its golden PNG and on a difference writes
 * <golden>.out (the actual image) and <golden minus .png>.diff.png.out.
 */
static void check_golden_at(Scene *s, const char *path, const uint16_t *pix, int w, int h)
{
    char out[4200];
    unsigned char *want, *got, *diff;
    int gw = 0, gh = 0, comp, x, y, x0 = w, y0 = h, x1 = -1, y1 = -1;
    size_t bad = 0;

    ++total.golden.compared;
    want = stbi_load(path, &gw, &gh, &comp, 4);
    if (want == NULL || gw != w || gh != h) {
        fail(s, "golden %s: %s (%d x %d, want %d x %d)", path,
             want == NULL ? stbi_failure_reason() : "wrong size", gw, gh, w, h);
        stbi_image_free(want);
        return;
    }
    got = vram_rgba(pix, w, h);
    diff = malloc((size_t)w * h * 4);
    if (got == NULL || diff == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x) {
            size_t i = ((size_t)y * w + x) * 4;
            int same = memcmp(got + i, want + i, 4) == 0;

            if (!same) {
                ++bad;
                x0 = x < x0 ? x : x0;
                y0 = y < y0 ? y : y0;
                x1 = x > x1 ? x : x1;
                y1 = y > y1 ? y : y1;
            }
            /* The diff: differing pixels magenta, the rest the golden dimmed. */
            diff[i] = same ? want[i] / 4 : 255;
            diff[i + 1] = same ? want[i + 1] / 4 : 0;
            diff[i + 2] = same ? want[i + 2] / 4 : 255;
            diff[i + 3] = 255;
        }
    if (bad == 0) {
        ++total.golden.matched;
    } else {
        fail(s, "golden %s: %zu of %d pixels differ, in x %d..%d, y %d..%d", base_name(path),
             bad, w * h, x0, x1, y0, y1);
        snprintf(out, sizeof out, "%s.out", base_name(path));
        write_png(out, got, w, h);
        snprintf(out, sizeof out, "%.*s.diff.png.out", (int)(strlen(base_name(path)) - 4),
                 base_name(path));
        write_png(out, diff, w, h);
    }
    stbi_image_free(want);
    free(got);
    free(diff);
}

/* The golden image NN-name.<suffix>.png beside the trace. */
static void check_golden(Scene *s, const char *suffix, const uint16_t *pix, int w, int h)
{
    char path[4096];
    size_t stem = strlen(s->r.path) - 4; /* without ".jbt" */

    snprintf(path, sizeof path, "%.*s.%s.png", (int)stem, s->r.path, suffix);
    check_golden_at(s, path, pix, w, h);
}

/* CAPTURE (0x31). */
static void op_capture(Scene *s)
{
    size_t at = s->r.pos; /* the payload, for --bless */
    unsigned kind = trace_u32(&s->r), standalone = trace_u32(&s->r), scale = trace_u32(&s->r);
    unsigned sw = trace_u32(&s->r), sh = trace_u32(&s->r);
    uint64_t h1 = trace_u64(&s->r), hs = trace_u64(&s->r);
    int has = vram_scaled() != NULL, ok;
    uint64_t got_1x, got_s;
    char what[160];

    if (kind == 5) {
        s->end_valid = s->sequence || standalone;
        s->at_end = 1;
    }
    if (!s->sequence && !standalone) {
        ++total.skipped_captures;
        return;
    }
    got_1x = hash_1x();
    got_s = hash_scaled();
    dump_compare(s);
    if (bl.bless != NULL) {
        /* The GPU as built is the reference: its hashes go into the trace. */
        if (got_1x != h1 || got_s != hs) {
            put_u64(s->r.data + at + 20, got_1x);
            put_u64(s->r.data + at + 28, got_s);
            ++bl.patched;
            h1 = got_1x;
            hs = got_s;
        }
    }
    if (sc.n > 0) {
        /* Under --scale the reference file holds the captures that differ
           from the trace at that scale; the others hold as recorded. */
        int same = (unsigned)cur_scale() == scale &&
                   (has ? (unsigned)(JBI_VRAM_W * cur_scale()) : 0) == sw &&
                   (has ? (unsigned)(JBI_VRAM_H * cur_scale()) : 0) == sh && got_1x == h1 &&
                   got_s == hs;
        RefLine *l;

        if (kind == 5)
            s->end_1x_other = got_1x != h1;
        if (sc.record) {
            if (!same)
                ref_put_hashes(s, REF_CAPTURE, (long)s->op, cur_scale(), got_1x, h1, got_s);
            tally(&total.capture, 1);
            return;
        }
        if ((l = ref_find(s, REF_CAPTURE, (long)s->op)) != NULL) {
            scale = (unsigned)l->scale;
            sw = l->scale > 1 ? (unsigned)(JBI_VRAM_W * l->scale) : 0;
            sh = l->scale > 1 ? (unsigned)(JBI_VRAM_H * l->scale) : 0;
            h1 = l->same_1x ? h1 : l->h1;
            hs = l->hash;
        }
    }
    ok = (unsigned)cur_scale() == scale && (has ? (unsigned)(JBI_VRAM_W * cur_scale()) : 0) == sw &&
         (has ? (unsigned)(JBI_VRAM_H * cur_scale()) : 0) == sh && got_1x == h1 && got_s == hs;
    if (tally(&total.capture, ok))
        return;
    fail(s, "capture kind %u: scale %d (want %u), scaled %d x %d (want %u x %u), 1x %016llx "
            "(want %016llx), scaled %016llx (want %016llx)",
         kind, cur_scale(), scale, has ? JBI_VRAM_W * cur_scale() : 0,
         has ? JBI_VRAM_H * cur_scale() : 0, sw, sh, (unsigned long long)got_1x,
         (unsigned long long)h1, (unsigned long long)got_s, (unsigned long long)hs);
    /* No reference image exists mid-scene, only the hash: write the actual
       VRAM of the first mismatching capture for a look. */
    if (!s->wrote_capture && kind != 5) {
        unsigned char *rgba = vram_rgba(vram_1x(), JBI_VRAM_W, JBI_VRAM_H);

        s->wrote_capture = 1;
        snprintf(what, sizeof what, "%s.op%u.1x.png.out", s->name, s->op);
        write_png(what, rgba, JBI_VRAM_W, JBI_VRAM_H);
        free(rgba);
        if (has) {
            rgba = vram_rgba(vram_scaled(), JBI_VRAM_W * cur_scale(), JBI_VRAM_H * cur_scale());
            snprintf(what, sizeof what, "%s.op%u.%dx.png.out", s->name, s->op, cur_scale());
            write_png(what, rgba, JBI_VRAM_W * cur_scale(), JBI_VRAM_H * cur_scale());
            free(rgba);
        }
    }
}

/* STORE (0x04). */
static void op_store(Scene *s)
{
    int x = trace_i32(&s->r), y = trace_i32(&s->r), w = trace_i32(&s->r), h = trace_i32(&s->r);
    uint32_t n = trace_u32(&s->r), i, bad = 0;
    int ww = ((w - 1) & 0x3ff) + 1, hh = ((h - 1) & 0x1ff) + 1;
    int x0 = ww, y0 = hh, x1 = -1, y1 = -1;
    size_t at = s->r.pos; /* the pixels, for --bless */
    uint16_t *want = trace_u16s(&s->r, n), *got;

    if (n != (uint32_t)ww * (uint32_t)hh)
        trace_die(&s->r, "STORE pixel count disagrees with its size");
    got = calloc((size_t)n + 1, 2);
    if (got == NULL)
        trace_die(&s->r, "out of memory");
    jbi_store(ctx, x, y, w, h, got);
    if (bl.bless != NULL && memcmp(got, want, (size_t)n * 2) != 0) {
        for (i = 0; i < n; ++i) {
            s->r.data[at + 2 * i] = (unsigned char)(got[i] & 0xff);
            s->r.data[at + 2 * i + 1] = (unsigned char)(got[i] >> 8);
        }
        memcpy(want, got, (size_t)n * 2);
        ++bl.patched;
    }
    if (sc.n > 0) {
        /* Above 1x a store reads the scaled VRAM back, averaged. */
        uint64_t hash = trace_fnv_pixels(got, n);
        RefLine *l;

        if (sc.record) {
            if (memcmp(got, want, (size_t)n * 2) != 0)
                ref_put("store %s %u %016llx\n", s->name, s->op, (unsigned long long)hash);
            tally(&total.store, 1);
            free(want);
            free(got);
            return;
        }
        if ((l = ref_find(s, REF_STORE, (long)s->op)) != NULL) {
            if (!tally(&total.store, hash == l->hash))
                fail(s, "store (%d, %d) %d x %d: hash %016llx, want %016llx (%s)", x, y, ww, hh,
                     (unsigned long long)hash, (unsigned long long)l->hash, sc.dir);
            free(want);
            free(got);
            return;
        }
    }
    for (i = 0; i < n; ++i)
        if (got[i] != want[i]) {
            int px = (int)(i % (uint32_t)ww), py = (int)(i / (uint32_t)ww);
            ++bad;
            x0 = px < x0 ? px : x0;
            y0 = py < y0 ? py : y0;
            x1 = px > x1 ? px : x1;
            y1 = py > y1 ? py : y1;
        }
    if (!tally(&total.store, bad == 0))
        fail(s, "store (%d, %d) %d x %d: %u of %u pixels differ, in x %d..%d, y %d..%d of the "
                "rectangle",
             x, y, ww, hh, bad, n, x0, x1, y0, y1);
    free(want);
    free(got);
}

/* PRESENT (0x30): the frame by FORMAT.md's algorithm, without the GPU. */
static unsigned char *present_frame(const int *a, int *width, int *height, int *stride,
                                    int *format)
{
    int on = a[0], env = a[1], dx = a[2], dy = a[3], dw = a[4], dh = a[5], n = a[7];
    int w = 512, h = 256, rgb = 0, x, r, i, k;
    unsigned char *f;

    if (env && dw > 0 && dh > 0) {
        w = dw;
        h = dh;
        rgb = a[6] != 0;
    }
    h = h < 512 ? h : 512;
    w = w < (rgb ? 682 : 1024) ? w : (rgb ? 682 : 1024);
    *stride = (rgb ? 3 * w : 2 * w) * n;
    *width = w * n;
    *height = h * n;
    *format = rgb;
    f = calloc((size_t)*stride * h * n + 1, 1);
    if (f == NULL || !on || !env)
        return f;
    x = dx & 1023;
    if (n > 1 && !rgb) {
        for (r = 0; r < h * n; ++r) {
            const uint16_t *src =
                vram_scaled() + (size_t)(((dy + r / n) & 511) * n + r % n) * JBI_VRAM_W * n;
            unsigned char *d = f + (size_t)r * *stride;
            for (i = 0; i < w * n; ++i) {
                uint16_t p = src[(x * n + i) % (JBI_VRAM_W * n)];
                d[2 * i] = (unsigned char)p;
                d[2 * i + 1] = (unsigned char)(p >> 8);
            }
        }
        return f;
    }
    for (r = 0; r < h; ++r) {
        const uint16_t *src = vram_1x() + (size_t)((dy + r) & 511) * JBI_VRAM_W;
        unsigned char *d = f + (size_t)r * n * *stride;
        if (rgb) {
            for (i = 0; i < 3 * w; ++i) {
                int b = (2 * x + i) & 2047;
                unsigned char c = (unsigned char)(src[b >> 1] >> (8 * (b & 1)));
                for (k = 0; k < n; ++k)
                    d[(i / 3 * n + k) * 3 + i % 3] = c;
            }
        } else {
            for (i = 0; i < w; ++i) {
                uint16_t p = src[(x + i) & 1023];
                d[2 * i] = (unsigned char)p;
                d[2 * i + 1] = (unsigned char)(p >> 8);
            }
        }
        for (k = 1; k < n; ++k)
            memcpy(d + (size_t)k * *stride, d, (size_t)*stride);
    }
    return f;
}

static void op_present(Scene *s)
{
    int a[8], want[4], fw, fh, fs, ff, gw, gh, gs, gf, i, ok_alg, ok_scan;
    uint64_t want_hash, h_alg, h_scan;
    unsigned char *alg, *scan;
    size_t bytes, differ = 0;

    size_t at;

    for (i = 0; i < 8; ++i)
        a[i] = trace_i32(&s->r);
    at = s->r.pos; /* the expected frame, for --bless */
    for (i = 0; i < 4; ++i)
        want[i] = trace_i32(&s->r);
    want_hash = trace_u64(&s->r);
    a[7] = map_scale(a[7]);

    /* present_frame sets the dimensions before it allocates: check the
       frame before anything reads it. */
    alg = present_frame(a, &fw, &fh, &fs, &ff);
    scan = malloc(jbi_scanout_bytes(ctx));
    if (alg == NULL || scan == NULL)
        trace_die(&s->r, "out of memory");
    h_alg = trace_fnv_bytes(TRACE_FNV_INIT, alg, (size_t)fs * fh);
    if (bl.bless != NULL &&
        (fw != want[0] || fh != want[1] || fs != want[2] || ff != want[3] || h_alg != want_hash)) {
        put_u32(s->r.data + at, (uint32_t)fw);
        put_u32(s->r.data + at + 4, (uint32_t)fh);
        put_u32(s->r.data + at + 8, (uint32_t)fs);
        put_u32(s->r.data + at + 12, (uint32_t)ff);
        put_u64(s->r.data + at + 16, h_alg);
        want[0] = fw, want[1] = fh, want[2] = fs, want[3] = ff;
        want_hash = h_alg;
        ++bl.patched;
    }
    /* jbi_scanout takes no "display area set" flag: without one the area
       is unset (w, h <= 0) and the display counts as disabled. */
    gf = jbi_scanout(ctx, a[2], a[3], a[1] ? a[4] : 0, a[1] ? a[5] : 0, a[6], a[0] && a[1], scan,
                     &gw, &gh, &gs);
    h_scan = trace_fnv_bytes(TRACE_FNV_INIT, scan, (size_t)gs * gh);

    if (sc.n > 0) {
        /* Under --scale: the frames that differ from the trace's are in the
           reference file. Recording takes the FORMAT.md frame; jbi_scanout
           is still checked against it below. */
        RefLine *l;

        if (sc.record) {
            if (fw != want[0] || fh != want[1] || fs != want[2] || ff != want[3] ||
                h_alg != want_hash) {
                ref_put("present %s %u %d %d %d %d %d %016llx\n", s->name, s->op, a[7], fw, fh,
                        fs, ff, (unsigned long long)h_alg);
                want[0] = fw, want[1] = fh, want[2] = fs, want[3] = ff;
                want_hash = h_alg;
            }
        } else if ((l = ref_find(s, REF_PRESENT, (long)s->op)) != NULL) {
            if (l->scale != a[7])
                fail(s, "PRESENT at scale %d, but %s says %d", a[7], sc.dir, l->scale);
            want[0] = l->width, want[1] = l->height, want[2] = l->stride, want[3] = l->format;
            want_hash = l->hash;
        }
    }
    ok_alg = fw == want[0] && fh == want[1] && fs == want[2] && ff == want[3] &&
             h_alg == want_hash;
    ok_scan = gw == want[0] && gh == want[1] && gs == want[2] && gf == want[3] &&
              h_scan == want_hash;
    tally(&total.present, a[7] == cur_scale() && ok_alg && ok_scan);
    if (a[7] != cur_scale())
        fail(s, "PRESENT scale %d, but the scale is %d", a[7], cur_scale());
    if (!ok_alg)
        fail(s, "frame (FORMAT.md): %d x %d stride %d format %d hash %016llx, want %d x %d "
                "stride %d format %d hash %016llx",
             fw, fh, fs, ff, (unsigned long long)h_alg, want[0], want[1], want[2], want[3],
             (unsigned long long)want_hash);
    if (!ok_scan)
        fail(s, "frame (jbi_scanout): %d x %d stride %d format %d hash %016llx, want %d x %d "
                "stride %d format %d hash %016llx",
             gw, gh, gs, gf, (unsigned long long)h_scan, want[0], want[1], want[2], want[3],
             (unsigned long long)want_hash);
    /* The two ways of building the frame, byte for byte. */
    if (gw == fw && gh == fh && gs == fs && gf == ff) {
        bytes = (size_t)fs * fh;
        for (i = 0; (size_t)i < bytes; ++i)
            differ += alg[i] != scan[i];
    }
    if (!tally(&total.scanout, gw == fw && gh == fh && gs == fs && gf == ff && differ == 0))
        fail(s, "jbi_scanout and FORMAT.md's PRESENT disagree: %d x %d / %d x %d, %zu bytes", gw,
             gh, fw, fh, differ);
    free(alg);
    free(scan);
}

/* POKE (0x32). */
static void op_poke(Scene *s)
{
    uint32_t plane = trace_u32(&s->r), w = trace_u32(&s->r), h = trace_u32(&s->r), runs = trace_u32(&s->r), i, k;
    uint16_t *dst = plane == 0 ? vram_1x() : vram_scaled();
    size_t at = 0, size = (size_t)w * h;

    if (plane > 1 || (plane == 0 && (w != JBI_VRAM_W || h != JBI_VRAM_H)) ||
        (plane == 1 && (vram_scaled() == NULL || w != (uint32_t)(JBI_VRAM_W * cur_scale()) ||
                        h != (uint32_t)(JBI_VRAM_H * cur_scale())))) {
        fail(s, "POKE of plane %u, %u x %u, does not fit the VRAM", plane, w, h);
        return;
    }
    for (i = 0; i < runs; ++i) {
        uint32_t count = trace_u32(&s->r), value = trace_u32(&s->r);
        if (count > size - at)
            trace_die(&s->r, "POKE runs overflow the plane");
        for (k = 0; k < count; ++k)
            dst[at++] = (uint16_t)value;
    }
    if (at != size)
        trace_die(&s->r, "POKE runs do not cover the plane");
}

static int configure(Scene *s, int scale, int dither, int threads)
{
    recorded_cfg.scale = scale;
    recorded_cfg.dither = dither;
    recorded_cfg.threads = threads;
    return jbi_configure(ctx, map_scale(scale), dither, s->threads > 0 ? s->threads : threads);
}

static void op_apply(Scene *s)
{
    trace_reader *r = &s->r;
    int v[6], i;

    switch (s->code) {
    case TRACE_OP_GP0: {
        uint32_t n = trace_u32(r), k;
        int want = trace_i32(r);
        uint32_t *words = malloc((size_t)n * 4 + 4);
        if (words == NULL)
            trace_die(r, "out of memory");
        for (k = 0; k < n; ++k)
            words[k] = trace_u32(r);
        check_int(s, &total.gp0, "jbi_gp0 return", jbi_gp0(ctx, words, (int)n), want);
        free(words);
        break;
    }
    case TRACE_OP_FILL:
        for (i = 0; i < 5; ++i)
            v[i] = trace_i32(r);
        jbi_fill(ctx, v[0], v[1], v[2], v[3], (uint32_t)v[4]);
        break;
    case TRACE_OP_LOAD: {
        uint32_t n;
        uint16_t *pix;
        for (i = 0; i < 4; ++i)
            v[i] = trace_i32(r);
        n = trace_u32(r);
        if (n != (uint32_t)((((v[2] - 1) & 0x3ff) + 1) * (((v[3] - 1) & 0x1ff) + 1)))
            trace_die(r, "LOAD pixel count disagrees with its size");
        pix = trace_u16s(r, n);
        jbi_load(ctx, v[0], v[1], v[2], v[3], pix);
        free(pix);
        break;
    }
    case TRACE_OP_STORE: op_store(s); break;
    case TRACE_OP_COPY:
        for (i = 0; i < 6; ++i)
            v[i] = trace_i32(r);
        jbi_copy(ctx, v[0], v[1], v[2], v[3], v[4], v[5]);
        break;
    case TRACE_OP_FLUSH: jbi_flush(ctx); break;
    case TRACE_OP_SYNC: break; /* gpu_sync undid draw-only passes, which are gone */
    case TRACE_OP_RESET_STATE: jbi_reset_state(ctx); break;
    case TRACE_OP_HIRES_REPLICATE: jbi_soft_replicate(soft); break;
    case TRACE_OP_LOWRES_MARK:
    case TRACE_OP_LOWRES_RESOLVE:
        for (i = 0; i < 4; ++i)
            v[i] = trace_i32(r);
        (s->code == TRACE_OP_LOWRES_MARK ? jbi_soft_lowres_mark : jbi_soft_lowres_resolve)(
            soft, v[0], v[1], v[2], v[3]);
        break;
    case TRACE_OP_TRACE_CALL: break; /* the timing trace is gone */
    case TRACE_OP_TRACE_TIME: (void)trace_i32(r); break;
    case TRACE_OP_CONFIGURE:
        for (i = 0; i < 5; ++i)
            v[i] = trace_i32(r);
        check_int(s, &total.configure, "jbi_configure return", configure(s, v[0], v[1], v[2]),
                  map_scale(v[3]));
        check_int(s, &total.configure, "jbi_threads", jbi_threads(ctx),
                  s->threads > 0 ? s->threads : v[4]);
        break;
    case TRACE_OP_SET_EXACT_1X:
        recorded_cfg.exact_1x = (v[0] = trace_i32(r)) != 0;
        jbi_set_exact_1x(ctx, v[0]);
        break;
    case TRACE_OP_SET_TEXTURE_MAPPING: jbi_set_texture_mapping(ctx, trace_i32(r)); break;
    case TRACE_OP_VRAM_GET: (void)jbi_soft_vram(soft); break;
    case TRACE_OP_PRECISE_ENABLE: fake_enable(trace_i32(r)); break;
    case TRACE_OP_PRECISE_BEGIN_FRAME: fake_begin_frame(); break;
    case TRACE_OP_PRECISE_RECORD: {
        unsigned sxy = trace_u32(r);
        jbi_precise_vertex pv;
        pv.x = trace_i32(r);
        pv.y = trace_i32(r);
        pv.z = trace_i32(r);
        fake_record(sxy, &pv);
        break;
    }
    case TRACE_OP_PRECISE_STATS:
        v[0] = trace_i32(r);
        v[1] = trace_i32(r);
        check_int(s, &total.stats, "precise lookups", fake.lookups, (unsigned)v[0]);
        check_int(s, &total.stats, "precise hits", fake.hits, (unsigned)v[1]);
        fake.lookups = fake.hits = 0;
        break;
    case TRACE_OP_PRESENT: op_present(s); break;
    case TRACE_OP_CAPTURE: op_capture(s); break;
    case TRACE_OP_POKE: op_poke(s); break;
    default: trace_die(r, "unknown op");
    }
    trace_op_done(r);
}

/*
 * The prelude (FORMAT.md, "Prelude"). Alone, it sets the scene's starting
 * state ("Running a scene alone"); in sequence, it checks the state the
 * earlier scenes left.
 */
static void prelude(Scene *s, const trace_prelude *p)
{
    trace_reader *r = &s->r;
    int scale = p->scale, dither = p->dither, threads = p->threads, exact = p->exact_1x;
    int texmap = p->texture_mapping, span = p->persp_span, span_1x = p->persp_span_1x;
    int enabled = p->precise_enabled, got_span, got_span_1x;
    unsigned lookups = p->precise_lookups, hits = p->precise_hits, count = p->precise_count, i;
    unsigned want_scale = p->gpu_scale;
    uint64_t want_1x = p->start_hash_1x, want_scaled = p->start_hash_scaled;

    if (count > PRECISE_MAX)
        trace_die(r, "precise table too large");
    if (!s->sequence) {
        configure(s, scale, dither, threads);
        jbi_reset_state(ctx);
        jbi_set_exact_1x(ctx, exact);
        recorded_cfg.exact_1x = exact != 0;
        jbi_set_texture_mapping(ctx, texmap);
        fake.enabled = 1;
        fake.count = 0;
    }
    for (i = 0; i < count; ++i) {
        uint32_t key;
        jbi_precise_vertex pv;

        trace_precise_entry(r, p, i, &key, &pv.x, &pv.y, &pv.z);
        if (!s->sequence) {
            fake_record(key, &pv);
        } else if (!tally(&total.prelude, (int)i < fake.count && fake.key[i] == key &&
                                              fake.v[i].x == pv.x && fake.v[i].y == pv.y &&
                                              fake.v[i].z == pv.z)) {
            fail(s, "precise table entry %u differs", i);
        }
    }
    if (!s->sequence) {
        fake.enabled = enabled != 0;
        fake.lookups = lookups;
        fake.hits = hits;
    }

    check_int(s, &total.prelude, "configured scale", recorded_cfg.scale, scale);
    check_int(s, &total.prelude, "configured dither", recorded_cfg.dither, dither);
    check_int(s, &total.prelude, "configured threads", recorded_cfg.threads, threads);
    check_int(s, &total.prelude, "exact 1x", recorded_cfg.exact_1x, exact);
    jbi_soft_persp_spans(soft, &got_span, &got_span_1x);
    check_int(s, &total.prelude, "texture perspective", jbi_texture_mapping(ctx), texmap);
    check_int(s, &total.prelude, "persp span", got_span, span);
    check_int(s, &total.prelude, "persp span 1x", got_span_1x, span_1x);
    check_int(s, &total.prelude, "precise enabled", fake.enabled, enabled);
    check_int(s, &total.prelude, "precise lookups", fake.lookups, lookups);
    check_int(s, &total.prelude, "precise hits", fake.hits, hits);
    check_int(s, &total.prelude, "precise entries", fake.count, count);
    check_int(s, &total.prelude, "scale", cur_scale(), map_scale((int)want_scale));
    if (s->sequence && sc.n > 0) {
        /* Under --scale, start hashes that differ are in the reference file. */
        uint64_t got_1x = hash_1x(), got_s = hash_scaled();
        RefLine *l;

        if (sc.record) {
            if (got_1x != want_1x || got_s != want_scaled)
                ref_put_hashes(s, REF_START, -1, cur_scale(), got_1x, want_1x, got_s);
            want_1x = got_1x;
            want_scaled = got_s;
        } else if ((l = ref_find(s, REF_START, -1)) != NULL) {
            want_1x = l->same_1x ? want_1x : l->h1;
            want_scaled = l->hash;
        }
    }
    if (s->sequence && bl.bless != NULL &&
        (hash_1x() != want_1x || hash_scaled() != want_scaled)) {
        size_t header = (size_t)r->data[12] | (size_t)r->data[13] << 8 |
                        (size_t)r->data[14] << 16 | (size_t)r->data[15] << 24;
        want_1x = hash_1x();
        want_scaled = hash_scaled();
        put_u64(r->data + header - 16, want_1x);
        put_u64(r->data + header - 8, want_scaled);
        ++bl.patched;
    }
    if (s->sequence) {
        check_hash(s, &total.prelude, "start 1x VRAM", hash_1x(), want_1x);
        check_hash(s, &total.prelude, "start scaled VRAM", hash_scaled(), want_scaled);
    }
}

static int run_scene(const char *path, int sequence, int threads)
{
    Scene s;
    trace_reader *r = &s.r;
    trace_prelude pre;

    memset(&s, 0, sizeof s);
    s.sequence = sequence;
    s.threads = threads;
    s.code = TRACE_OP_LIMIT;
    trace_open(r, path, s.name, sizeof s.name, &pre);
    prelude(&s, &pre);

    for (s.op = 0;; ++s.op) {
        s.code = trace_next_op(r);
        if (s.code == TRACE_OP_END) {
            if (!s.at_end)
                trace_die(r, "END without the scene-end capture before it");
            break;
        }
        if (s.at_end)
            trace_die(r, "an op after the scene-end capture");
        op_apply(&s);
    }

    /* The end state against the golden images (the scene-end capture's state),
       or under --bless the end state and the trace written out. */
    if (bl.bless != NULL) {
        char out[4096];
        FILE *f;

        snprintf(out, sizeof out, "%s/%s.1x.raw", bl.bless, s.name);
        write_raw(out, vram_1x(), (size_t)JBI_VRAM_W * JBI_VRAM_H);
        if (vram_scaled() != NULL) {
            snprintf(out, sizeof out, "%s/%s.%dx.raw", bl.bless, s.name, cur_scale());
            write_raw(out, vram_scaled(),
                      (size_t)JBI_VRAM_W * cur_scale() * JBI_VRAM_H * cur_scale());
        }
        snprintf(out, sizeof out, "%s/%s.jbt", bl.bless, s.name);
        f = fopen(out, "wb");
        if (f == NULL || fwrite(r->data, 1, r->len, f) != r->len || fclose(f) != 0) {
            fprintf(stderr, "seed_replay: cannot write %s\n", out);
            exit(2);
        }
        printf("    blessed: %u fields rewritten\n", bl.patched);
        bl.patched_total += bl.patched;
        bl.patched = 0;
    } else if (s.end_valid && sc.n > 0) {
        /* Under --scale: the recorded 1x image where the end 1x VRAM is the
           recorded one, and a scaled image where the reference set has one. */
        char img[4096];
        int w = JBI_VRAM_W * cur_scale(), h = JBI_VRAM_H * cur_scale(), k, png = 0;

        if (!s.end_1x_other)
            check_golden(&s, "1x", vram_1x(), JBI_VRAM_W, JBI_VRAM_H);
        else
            ++total.skipped_goldens;
        snprintf(img, sizeof img, "%s/%s.%dx.png", sc.dir, s.name, cur_scale());
        if (vram_scaled() != NULL && sc.record) {
            for (k = 0; k < sc.npng; ++k)
                png |= strcmp(sc.png[k], s.name) == 0;
            if (png) {
                unsigned char *rgba = vram_rgba(vram_scaled(), w, h);
                if (rgba == NULL || !stbi_write_png(img, w, h, 4, rgba, w * 4)) {
                    fprintf(stderr, "seed_replay: cannot write %s\n", img);
                    exit(2);
                }
                free(rgba);
                ref_put("golden %s -1 %d\n", s.name, cur_scale());
                ++sc.pngs;
            }
        } else if (vram_scaled() != NULL && ref_find(&s, REF_GOLDEN, -1) != NULL) {
            check_golden_at(&s, img, vram_scaled(), w, h);
        } else if (vram_scaled() != NULL) {
            ++total.skipped_goldens;
        }
    } else if (s.end_valid) {
        check_golden(&s, "1x", vram_1x(), JBI_VRAM_W, JBI_VRAM_H);
        if (vram_scaled() != NULL) {
            char suffix[8];
            snprintf(suffix, sizeof suffix, "%dx", cur_scale());
            check_golden(&s, suffix, vram_scaled(), JBI_VRAM_W * cur_scale(),
                         JBI_VRAM_H * cur_scale());
        }
    } else {
        ++total.skipped_goldens;
    }
    ++total.scenes;
    printf("%s: %u ops, %s\n", s.name, s.op + 1,
           s.failures ? "FAILED" : "every check matched");
    if (s.failures > 40)
        printf("    (%d failures, the first 40 shown)\n", s.failures);
    trace_close(r);
    return s.failures;
}

static void print_tally(const char *what, const Tally *t)
{
    printf("  %-34s %u compared, %u matched\n", what, t->compared, t->matched);
}

int main(int argc, char **argv)
{
    int i = 1, sequence = 0, threads = 0, failures = 0;

    for (; i < argc && argv[i][0] == '-'; ++i) {
        if (strcmp(argv[i], "--sequence") == 0) {
            sequence = 1;
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            threads = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--scale") == 0 || strcmp(argv[i], "--record-scale") == 0) &&
                   i + 2 < argc) {
            sc.record = argv[i][2] == 'r';
            sc.n = atoi(argv[i + 1]);
            sc.dir = argv[i + 2];
            i += 2;
        } else if (strcmp(argv[i], "--bless") == 0 && i + 1 < argc) {
            bl.bless = argv[++i];
        } else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            bl.dump = argv[++i];
        } else if (strcmp(argv[i], "--compare") == 0 && i + 1 < argc) {
            bl.compare = argv[++i];
        } else if (strcmp(argv[i], "--png") == 0 && i + 1 < argc) {
            const char **p = realloc(sc.png, (size_t)(sc.npng + 1) * sizeof *p);
            if (p == NULL)
                return 2;
            sc.png = p;
            sc.png[sc.npng++] = argv[++i];
        } else {
            break;
        }
    }
    if (i >= argc || (!sequence && argc - i != 1) || threads < 0 || threads > 64 ||
        (sc.dir != NULL && (!sequence || sc.n < 2 || sc.n > JBI_MAX_SCALE)) ||
        (sc.npng > 0 && !sc.record) || (bl.bless != NULL && (!sequence || sc.n > 0))) {
        fprintf(stderr,
                "usage: seed_replay [--threads N] TRACE.jbt\n"
                "       seed_replay [--threads N] --sequence TRACE.jbt...\n"
                "       seed_replay [--threads N] --scale S DIR --sequence TRACE.jbt...\n"
                "       seed_replay [--threads N] --record-scale S DIR [--png NN-name]...\n"
                "                   --sequence TRACE.jbt...\n"
                "       seed_replay --bless DIR --sequence TRACE.jbt...\n"
                "and with any of them --dump DIR, --compare DIR.\n"
                "S is 2 to %d.\n",
                JBI_MAX_SCALE);
        return 2;
    }
    if (bl.compare != NULL) {
        struct stat cs;

        if (stat(bl.compare, &cs) != 0 || !S_ISDIR(cs.st_mode)) {
            fprintf(stderr, "seed_replay: --compare %s: not a directory\n", bl.compare);
            return 2;
        }
        /* The dump is written before the reference is read: the same
           directory for both would compare each image with itself. */
        if (bl.dump != NULL && same_dir(bl.dump, bl.compare)) {
            fprintf(stderr, "seed_replay: --dump and --compare are the same directory\n");
            return 2;
        }
    }
#ifdef JB_SEED_SOURCE_DIR
    /* Re-blessing writes into a scratch copy, which rebless.sh installs
       only after every check passed (docs/testing.md, "Legitimate
       re-blessing"): never into the source tree's tests/seed directly. */
    if ((bl.bless != NULL && (same_dir(bl.bless, JB_SEED_SOURCE_DIR) ||
                              same_dir(bl.bless, JB_SEED_SOURCE_DIR "/scaled"))) ||
        (sc.record && (same_dir(sc.dir, JB_SEED_SOURCE_DIR) ||
                       same_dir(sc.dir, JB_SEED_SOURCE_DIR "/scaled")))) {
        fprintf(stderr, "seed_replay: will not write into %s; use tools/seed-record/rebless.sh\n",
                JB_SEED_SOURCE_DIR);
        return 2;
    }
#endif
    if (sc.record) {
        char path[4096];

        snprintf(path, sizeof path, "%s/%dx.txt", sc.dir, sc.n);
        sc.out = fopen(path, "w");
        if (sc.out == NULL) {
            fprintf(stderr, "seed_replay: cannot write %s\n", path);
            return 2;
        }
        fprintf(sc.out,
                "# The seed scenes at internal scale %d (tests/seed/FORMAT.md, \"Other scales\"):\n"
                "# what differs from the traces when every scale above 1x is %d.\n"
                "# Written by seed_replay --record-scale %d; do not edit.\n",
                sc.n, sc.n, sc.n);
    } else if (sc.n > 0) {
        ref_load();
    }
    /* In sequence the first scene starts fresh: a new context, which is
       configured as jbi_configure(ctx, 1, -1, 1) would. */
    context_create();
    if (sequence && threads > 0)
        jbi_configure(ctx, 1, -1, threads);
    for (; i < argc; ++i)
        failures += run_scene(argv[i], sequence, threads) != 0;
    if (sc.n > 0 && !sc.record) {
        size_t l;
        for (l = 0; l < sc.count; ++l)
            if (!sc.lines[l].used) {
                printf("FAIL %s: %s line for op %ld was never reached\n", sc.lines[l].scene,
                       ref_kinds[sc.lines[l].kind], sc.lines[l].op);
                ++failures;
            }
    }

    printf("%u scene%s%s%s:\n", total.scenes, total.scenes == 1 ? "" : "s",
           sequence ? " in sequence" : " alone", threads > 0 ? ", threads forced" : "");
    if (sc.record) {
        if (fclose(sc.out) != 0) {
            fprintf(stderr, "seed_replay: cannot write %s\n", sc.dir);
            return 2;
        }
        printf("  scale %d: wrote %u reference lines, %u images into %s\n", sc.n, sc.lines_out,
               sc.pngs, sc.dir);
    } else if (sc.n > 0) {
        printf("  scale %d: %zu reference lines from %s, %u checks used them\n", sc.n, sc.count,
               sc.dir, sc.checked);
    }
    if (bl.bless != NULL)
        printf("  blessed into %s: %u fields rewritten\n", bl.bless, bl.patched_total);
    if (bl.compare != NULL)
        printf("  compared with %s: %u VRAM images, %u differ, %u missing\n", bl.compare,
               bl.compared, bl.differed, bl.missing);
    print_tally("captures", &total.capture);
    printf("  %-34s %u\n", "captures skipped (not standalone)", total.skipped_captures);
    print_tally("stores", &total.store);
    print_tally("presents", &total.present);
    print_tally("jbi_scanout vs FORMAT.md frames", &total.scanout);
    print_tally("precise stats", &total.stats);
    print_tally("gp0 returns", &total.gp0);
    print_tally("configure returns and threads", &total.configure);
    print_tally("prelude", &total.prelude);
    print_tally("golden images", &total.golden);
    if (total.skipped_goldens)
        printf("  %-34s %u\n",
               sc.n > 0 ? "goldens skipped (none at this scale)"
                        : "goldens skipped (end not standalone)",
               total.skipped_goldens);
    jbi_context_destroy(ctx);
    if (bl.compare != NULL && (bl.missing > 0 || bl.compared == 0)) {
        fprintf(stderr, "seed_replay: --compare %s: %s\n", bl.compare,
                bl.missing > 0 ? "references missing" : "nothing compared");
        return 2;
    }
    return failures != 0;
}

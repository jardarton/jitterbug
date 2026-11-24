/*
 * Two contexts are independent: two contexts drawing different scenes at
 * once each match their traces, as one alone does. Pairs of the seed's
 * scenes (tests/seed, FORMAT.md) are replayed alone, each on a context of
 * its own with a fake precise table of its own:
 *
 *   (a) on one thread, the two scenes interleaved op by op;
 *   (b) on two threads at once, one scene each.
 *
 * Every result a scene alone must reproduce is compared, as seed_replay
 * does for one scene: the standalone captures (1x and scaled VRAM hashes),
 * stores, presented frames (through jbi_scanout), the fake table's
 * counters, and the GP0 and configuration returns. The pairs include
 * scaled scenes and scenes whose backend rasterises on several threads.
 *
 *   seed_multi TRACE_DIR
 *
 * The trace format is read here independently of seed_replay.c, which
 * keeps its process-wide state; this player keeps all of a scene's state
 * in one struct, so two can run side by side. Built so ThreadSanitizer can
 * run it (no shared mutable state between the players).
 */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/context.h"
#include "soft/soft.h"

/* ------------------------------------------------------------------------ */
/* A fake precise table per player (FORMAT.md, "Fake precise-vertex
   table") */

enum { PRECISE_MAX = 2048 };

typedef struct {
    int enabled, count;
    uint32_t key[PRECISE_MAX];
    jbi_precise_vertex v[PRECISE_MAX];
    unsigned lookups, hits;
} Fake;

static void fake_record(Fake *f, uint32_t sxy, const jbi_precise_vertex *v)
{
    int i;

    if (!f->enabled)
        return;
    for (i = 0; i < f->count && f->key[i] != sxy; ++i)
        ;
    if (i < f->count) {
        f->v[i] = *v;
    } else if (f->count < PRECISE_MAX) {
        f->key[f->count] = sxy;
        f->v[f->count++] = *v;
    }
}

static int fake_enabled(void *user)
{
    return ((Fake *)user)->enabled;
}

static int fake_lookup(void *user, uint32_t sxy, jbi_precise_vertex *v)
{
    Fake *f = user;
    int i;

    ++f->lookups;
    for (i = 0; i < f->count; ++i)
        if (f->key[i] == sxy) {
            ++f->hits;
            *v = f->v[i];
            return 1;
        }
    return 0;
}

static unsigned fake_lookup_poly(void *user, const uint32_t *sxy, int n, jbi_precise_vertex *v)
{
    unsigned mask = 0;
    int i;

    for (i = 0; i < n; ++i)
        if (fake_lookup(user, sxy[i], &v[i]))
            mask |= 1u << i;
    return mask;
}

/* ------------------------------------------------------------------------ */
/* A player: one scene on one context */

enum {
    OP_END = 0x00, OP_GP0, OP_FILL, OP_LOAD, OP_STORE, OP_COPY, OP_FLUSH, OP_SYNC,
    OP_RESET_STATE, OP_HIRES_REPLICATE, OP_LOWRES_MARK, OP_LOWRES_RESOLVE, OP_TRACE_CALL,
    OP_TRACE_TIME,
    OP_CONFIGURE = 0x10, OP_SET_EXACT_1X, OP_SET_TEXTURE_MAPPING, OP_VRAM_GET,
    OP_PRECISE_ENABLE = 0x20, OP_PRECISE_BEGIN_FRAME, OP_PRECISE_RECORD, OP_PRECISE_STATS,
    OP_PRESENT = 0x30, OP_CAPTURE, OP_POKE,
};

typedef struct {
    char name[128];
    unsigned char *data;
    size_t len, pos, end; /* end: of the current op's payload */
    unsigned op;
    int done;
    jb_context *ctx;
    jbi_soft *soft;
    Fake fake;
    jbi_precise_source source;
    unsigned compared, failures, captures;
    uint16_t *buf; /* loads and frames */
    size_t buf_bytes;
} Player;

/* p->buf holds at least bytes. */
static void reserve(Player *p, size_t bytes)
{
    if (bytes <= p->buf_bytes)
        return;
    free(p->buf);
    p->buf = malloc(bytes);
    p->buf_bytes = bytes;
    if (p->buf == NULL) {
        fprintf(stderr, "seed_multi: out of memory\n");
        exit(2);
    }
}

static void fail(Player *p, const char *fmt, long long got, long long want)
{
    if (++p->failures <= 10) {
        printf("FAIL %s op %u: ", p->name, p->op);
        printf(fmt, got, want);
        printf("\n");
    }
}

static void check(Player *p, const char *what, long long got, long long want)
{
    ++p->compared;
    if (got != want)
        fail(p, what, got, want);
}

static uint32_t u32(Player *p)
{
    const unsigned char *b = p->data + p->pos;

    if (p->end - p->pos < 4) {
        fprintf(stderr, "%s: truncated at %zu\n", p->name, p->pos);
        exit(2);
    }
    p->pos += 4;
    return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static int i32(Player *p)
{
    uint32_t v = u32(p);
    return v <= INT32_MAX ? (int)v : -(int)(~v) - 1;
}

static uint64_t u64(Player *p)
{
    uint64_t lo = u32(p);
    return lo | (uint64_t)u32(p) << 32;
}

/* u16[n], padded to 4 bytes, into p->buf. */
static void u16s(Player *p, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; ++i)
        p->buf[i] = (uint16_t)(p->data[p->pos + 2 * i] | p->data[p->pos + 2 * i + 1] << 8);
    p->pos += ((size_t)n * 2 + 3) & ~(size_t)3;
}

static uint64_t fnv(uint64_t h, const unsigned char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; ++i)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

/* A VRAM hash (FORMAT.md, "Hash"): each pixel low byte first. */
static uint64_t hash_pixels(const uint16_t *v, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;

    for (i = 0; i < n; ++i) {
        h = (h ^ (v[i] & 0xffu)) * 0x100000001b3ull;
        h = (h ^ (unsigned)(v[i] >> 8)) * 0x100000001b3ull;
    }
    return h;
}

static void player_open(Player *p, const char *dir, const char *scene)
{
    char path[4096];
    FILE *f;
    long size;
    jbi_config cfg;
    int scale, dither, threads, exact, texmap, enabled;
    unsigned header, count, i;

    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", scene);
    snprintf(path, sizeof path, "%s/%s.jbt", dir, scene);
    f = fopen(path, "rb");
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
        fprintf(stderr, "seed_multi: cannot read %s\n", path);
        exit(2);
    }
    rewind(f);
    p->data = malloc((size_t)size);
    reserve(p, (size_t)JBI_VRAM_W * JBI_VRAM_H * 2);
    if (p->data == NULL || fread(p->data, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "seed_multi: cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    p->len = p->end = (size_t)size;
    if (p->len < 24 || memcmp(p->data, "JBSEEDT\0", 8) != 0) {
        fprintf(stderr, "seed_multi: %s is not a trace\n", path);
        exit(2);
    }

    p->source.enabled = fake_enabled;
    p->source.lookup = fake_lookup;
    p->source.lookup_poly = fake_lookup_poly;
    p->source.user = &p->fake;
    jbi_config_default(&cfg);
    cfg.precise = &p->source;
    p->ctx = jbi_context_create(&cfg);
    if (p->ctx == NULL) {
        fprintf(stderr, "seed_multi: no context\n");
        exit(2);
    }
    p->soft = jbi_soft_from(jbi_context_backend(p->ctx));

    /* The header, then the prelude applied as for a scene alone
       (FORMAT.md, "Running a scene alone"). */
    p->pos = 12;
    header = u32(p);
    p->pos = 20;
    p->pos += (u32(p) + 3) & ~3u;
    scale = i32(p), dither = i32(p), threads = i32(p), exact = i32(p), texmap = i32(p);
    (void)i32(p), (void)i32(p); /* the perspective spans, always the defaults */
    enabled = i32(p);
    p->fake.lookups = u32(p);
    p->fake.hits = u32(p);
    count = u32(p);
    jbi_configure(p->ctx, scale, dither, threads);
    jbi_reset_state(p->ctx);
    jbi_set_exact_1x(p->ctx, exact);
    jbi_set_texture_mapping(p->ctx, texmap);
    p->fake.enabled = 1;
    for (i = 0; i < count; ++i) {
        uint32_t key = u32(p);
        jbi_precise_vertex v;
        v.x = i32(p);
        v.y = i32(p);
        v.z = i32(p);
        fake_record(&p->fake, key, &v);
    }
    p->fake.enabled = enabled != 0;
    p->pos = header; /* skipping the start checks, which hold only in sequence */
}

static void op_capture(Player *p)
{
    unsigned kind = u32(p), standalone = u32(p), scale = u32(p), sw = u32(p), sh = u32(p);
    uint64_t h1 = u64(p), hs = u64(p), got_1x, got_s = 0;
    const uint16_t *scaled = jbi_soft_scaled_raw(p->soft);
    int n = jbi_scale(p->ctx);

    (void)kind;
    if (!standalone)
        return;
    ++p->captures;
    got_1x = hash_pixels(jbi_soft_vram_raw(p->soft), (size_t)JBI_VRAM_W * JBI_VRAM_H);
    if (scaled != NULL)
        got_s = hash_pixels(scaled, (size_t)JBI_VRAM_W * JBI_VRAM_H * n * n);
    check(p, "capture scale: %lld, want %lld", n, scale);
    check(p, "capture scaled width: %lld, want %lld", scaled ? JBI_VRAM_W * n : 0, sw);
    check(p, "capture scaled height: %lld, want %lld", scaled ? JBI_VRAM_H * n : 0, sh);
    check(p, "capture 1x hash: %llx, want %llx", (long long)got_1x, (long long)h1);
    check(p, "capture scaled hash: %llx, want %llx", (long long)got_s, (long long)hs);
}

static void op_present(Player *p)
{
    int a[8], want[4], w, h, stride, format, i;
    uint64_t want_hash;

    for (i = 0; i < 8; ++i)
        a[i] = i32(p);
    for (i = 0; i < 4; ++i)
        want[i] = i32(p);
    want_hash = u64(p);
    check(p, "present scale: %lld, want %lld", jbi_scale(p->ctx), a[7]);
    reserve(p, jbi_scanout_bytes(p->ctx));
    format = jbi_scanout(p->ctx, a[2], a[3], a[1] ? a[4] : 0, a[1] ? a[5] : 0, a[6], a[0] && a[1],
                         p->buf, &w, &h, &stride);
    check(p, "frame width: %lld, want %lld", w, want[0]);
    check(p, "frame height: %lld, want %lld", h, want[1]);
    check(p, "frame stride: %lld, want %lld", stride, want[2]);
    check(p, "frame format: %lld, want %lld", format, want[3]);
    check(p, "frame hash: %llx, want %llx",
          (long long)fnv(0xcbf29ce484222325ull, (const unsigned char *)p->buf, (size_t)stride * h),
          (long long)want_hash);
}

static void op_poke(Player *p)
{
    uint32_t plane = u32(p), w = u32(p), h = u32(p), runs = u32(p), i, k;
    uint16_t *dst = plane == 0 ? jbi_soft_vram_raw(p->soft) : jbi_soft_scaled_raw(p->soft);
    size_t at = 0, size = (size_t)w * h;

    if (dst == NULL || (plane == 0 && (w != JBI_VRAM_W || h != JBI_VRAM_H))) {
        fail(p, "POKE of plane %lld that does not fit (%lld)", plane, 0);
        return;
    }
    for (i = 0; i < runs; ++i) {
        uint32_t c = u32(p), value = u32(p);
        for (k = 0; k < c && at < size; ++k)
            dst[at++] = (uint16_t)value;
    }
}

/* Applies the next op. Returns 0 at the end of the scene. */
static int player_step(Player *p)
{
    int v[6], i;
    uint32_t code, plen;

    if (p->done)
        return 0;
    p->end = p->len;
    code = u32(p);
    plen = u32(p);
    p->end = p->pos + plen;
    switch (code) {
    case OP_END:
        p->done = 1;
        return 0;
    case OP_GP0: {
        uint32_t n = u32(p), k;
        int want = i32(p);
        uint32_t *words = malloc((size_t)n * 4 + 4);
        if (words == NULL)
            exit(2);
        for (k = 0; k < n; ++k)
            words[k] = u32(p);
        check(p, "jbi_gp0 returned %lld, want %lld", jbi_gp0(p->ctx, words, (int)n), want);
        free(words);
        break;
    }
    case OP_FILL:
        for (i = 0; i < 5; ++i)
            v[i] = i32(p);
        jbi_fill(p->ctx, v[0], v[1], v[2], v[3], (uint32_t)v[4]);
        break;
    case OP_LOAD:
        for (i = 0; i < 4; ++i)
            v[i] = i32(p);
        u16s(p, u32(p));
        jbi_load(p->ctx, v[0], v[1], v[2], v[3], p->buf);
        break;
    case OP_STORE: {
        uint32_t n, k, bad = 0;
        uint16_t *got;

        for (i = 0; i < 4; ++i)
            v[i] = i32(p);
        n = u32(p);
        got = malloc((size_t)n * 2 + 2);
        if (got == NULL)
            exit(2);
        jbi_store(p->ctx, v[0], v[1], v[2], v[3], got);
        u16s(p, n);
        for (k = 0; k < n; ++k)
            bad += got[k] != p->buf[k];
        check(p, "store: %lld pixels differ (%lld)", bad, 0);
        free(got);
        break;
    }
    case OP_COPY:
        for (i = 0; i < 6; ++i)
            v[i] = i32(p);
        jbi_copy(p->ctx, v[0], v[1], v[2], v[3], v[4], v[5]);
        break;
    case OP_FLUSH: jbi_flush(p->ctx); break;
    case OP_RESET_STATE: jbi_reset_state(p->ctx); break;
    case OP_HIRES_REPLICATE: jbi_soft_replicate(p->soft); break;
    case OP_LOWRES_MARK:
    case OP_LOWRES_RESOLVE:
        for (i = 0; i < 4; ++i)
            v[i] = i32(p);
        if (code == OP_LOWRES_MARK)
            jbi_soft_lowres_mark(p->soft, v[0], v[1], v[2], v[3]);
        else
            jbi_soft_lowres_resolve(p->soft, v[0], v[1], v[2], v[3]);
        break;
    case OP_SYNC:
    case OP_TRACE_CALL:
    case OP_TRACE_TIME:
        break; /* nothing, as in seed_replay.c */
    case OP_CONFIGURE:
        for (i = 0; i < 5; ++i)
            v[i] = i32(p);
        check(p, "jbi_configure returned %lld, want %lld",
              jbi_configure(p->ctx, v[0], v[1], v[2]), v[3]);
        check(p, "jbi_threads %lld, want %lld", jbi_threads(p->ctx), v[4]);
        break;
    case OP_SET_EXACT_1X: jbi_set_exact_1x(p->ctx, i32(p)); break;
    case OP_SET_TEXTURE_MAPPING: jbi_set_texture_mapping(p->ctx, i32(p)); break;
    case OP_VRAM_GET: (void)jbi_soft_vram(p->soft); break;
    case OP_PRECISE_ENABLE: p->fake.enabled = i32(p) != 0; break;
    case OP_PRECISE_BEGIN_FRAME: p->fake.count = 0; break;
    case OP_PRECISE_RECORD: {
        uint32_t sxy = u32(p);
        jbi_precise_vertex pv;
        pv.x = i32(p);
        pv.y = i32(p);
        pv.z = i32(p);
        fake_record(&p->fake, sxy, &pv);
        break;
    }
    case OP_PRECISE_STATS:
        v[0] = i32(p);
        v[1] = i32(p);
        check(p, "precise lookups %lld, want %lld", p->fake.lookups, (unsigned)v[0]);
        check(p, "precise hits %lld, want %lld", p->fake.hits, (unsigned)v[1]);
        p->fake.lookups = p->fake.hits = 0;
        break;
    case OP_PRESENT: op_present(p); break;
    case OP_CAPTURE: op_capture(p); break;
    case OP_POKE: op_poke(p); break;
    default:
        fprintf(stderr, "%s: unknown op %u\n", p->name, code);
        exit(2);
    }
    p->pos = p->end;
    ++p->op;
    return 1;
}

static void player_close(Player *p)
{
    jbi_context_destroy(p->ctx);
    free(p->data);
    free(p->buf);
}

static void *player_run(void *arg)
{
    Player *p = arg;

    while (player_step(p))
        ;
    return NULL;
}

/* ------------------------------------------------------------------------ */

static const char *const pairs[][2] = {
    {"27-persp_threads", "20-hires_threads"},          /* both on several threads */
    {"18-hires_coverage_3t", "11-hires_skip_1x"},      /* 3 threads at 2x, 1x skipped */
    {"19-hires_render_to_texture_3t", "10-scene"},     /* 2x and 1x */
    {"24-persp_floor_2x", "13-hires_sprites_and_transfers"},
    {"16-hires_precise", "25-persp_fallback_2x"},      /* both use their precise tables */
    {"17-hires_display", "09-display_24bit"},
    {"22-persp_fallback_1x", "05-semi_transparency"},
};

static int report(const char *mode, Player *p)
{
    int ok = p->failures == 0 && p->done && p->captures > 0;

    printf("%s %-32s %4u ops, %3u captures, %5u checks: %s\n", mode, p->name, p->op,
           p->captures, p->compared, ok ? "every check matched" : "FAILED");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    static Player a, b;
    unsigned k;
    int failures = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: seed_multi TRACE_DIR\n");
        return 2;
    }
    for (k = 0; k < sizeof pairs / sizeof pairs[0]; ++k) {
        pthread_t ta, tb;

        /* (a) one thread, interleaved op by op. */
        player_open(&a, argv[1], pairs[k][0]);
        player_open(&b, argv[1], pairs[k][1]);
        while (player_step(&a) | player_step(&b))
            ;
        failures += report("interleaved", &a) + report("interleaved", &b);
        player_close(&a);
        player_close(&b);

        /* (b) two threads at once. */
        player_open(&a, argv[1], pairs[k][0]);
        player_open(&b, argv[1], pairs[k][1]);
        if (pthread_create(&ta, NULL, player_run, &a) != 0 ||
            pthread_create(&tb, NULL, player_run, &b) != 0) {
            fprintf(stderr, "seed_multi: cannot start threads\n");
            return 2;
        }
        pthread_join(ta, NULL);
        pthread_join(tb, NULL);
        failures += report("concurrent ", &a) + report("concurrent ", &b);
        player_close(&a);
        player_close(&b);
    }
    printf("seed_multi: %u pairs, %d scenes failed\n", (unsigned)(sizeof pairs / sizeof pairs[0]),
           failures);
    return failures != 0;
}

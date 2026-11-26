/*
 * clut_cache: the CLUT cache (src/soft/soft.c, include/jitterbug.h at
 * jb_vram_load) where ps1-tests' clut-cache covers it thinly: with the
 * queue and the workers (1, 2, 4 and 8 threads), at 1x, 2x and 3x (the 1x
 * VRAM read back), and its internal switch off.
 *
 * Each case sends GP0 words through jb_gp0 and reads rows back with
 * jb_vram_read; the expected pixels are worked out here from the rules,
 * not from a run. Sprites are raw-textured (GP0(65h)), so a pixel is its
 * palette entry exactly:
 *   self       a 256 x 1 8-bit sprite over its own CLUT keeps the palette
 *              loaded before its first pixel (ps1-tests' row 52)
 *   kept       the CLUT rewritten by a fill between two sprites: the second
 *              draws with the cached palette (row 88)
 *   kept-prim  the same, rewritten by a flat rectangle, a queued primitive
 *   cleared    the same with GP0(01h), with GP1(01h), with GP1(00h) in
 *              between: the second loads the new palette
 *   depth      4-bit then 8-bit with one CLUT loads again; 8-bit then 4-bit
 *              does not (rows 136 and 152)
 *   empty      a sprite above the drawing area still loads the cache
 *              (an assumption: docs/conformance.md, "Assumptions beyond
 *              the evidence")
 *   pool       700 sprites alternating between two CLUTs, more loads than
 *              the palette pool holds between two flushes
 *   off        with the cache off (jbi_set_clut_cache), the self-overwriting
 *              sprite reads the entries it has already written, as the seed
 *              did
 *   rollback   a temporary draw reloads a changed CLUT, then rollback
 *              restores the palette cached before that draw
 * and each case's VRAM must be the same at every thread count and scale.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jitterbug.h>

#include "core/context.h"

static int failures;

static void check_px(const char *what, int x, unsigned got, unsigned want)
{
    if (got == want)
        return;
    if (++failures <= 20)
        printf("FAIL %s: pixel %d is 0x%04x, want 0x%04x\n", what, x, got, want);
}

enum {
    PAGE_X = 64,  /* the texture page, at y 0 */
    CLUT_A = 400, /* CLUT rows, at x 0 */
    CLUT_B = 401,
    ROW = 300     /* where the sprites go */
};

typedef struct {
    jb_context *jb;
    uint32_t w[8192];
    int n;
} Words;

static void put(Words *g, uint32_t v)
{
    if (g->n == (int)(sizeof g->w / sizeof g->w[0])) {
        jb_gp0(g->jb, g->w, (size_t)g->n);
        g->n = 0;
    }
    g->w[g->n++] = v;
}

static void send(Words *g)
{
    jb_gp0(g->jb, g->w, (size_t)g->n);
    g->n = 0;
}

static uint32_t xy(int x, int y)
{
    return (uint32_t)x | (uint32_t)y << 16;
}

/* The draw mode: the page, depth 0 (4-bit) or 1 (8-bit). */
static void mode(Words *g, int depth)
{
    put(g, 0xe1000400u | PAGE_X / 64 | (uint32_t)depth << 7);
}

static void environment(Words *g, int depth)
{
    mode(g, depth);
    put(g, 0xe3000000u);
    put(g, 0xe4000000u | 1023u | 511u << 10);
    put(g, 0xe5000000u);
    put(g, 0xe6000000u);
}

/* CPU to VRAM, one row of n pixels. */
static void load_row(Words *g, int x, int y, int n, const uint16_t *px)
{
    int i;

    put(g, 0xa0000000u);
    put(g, xy(x, y));
    put(g, (uint32_t)n | 1u << 16);
    for (i = 0; i < n; i += 2)
        put(g, (uint32_t)px[i] | (i + 1 < n ? (uint32_t)px[i + 1] << 16 : 0));
}

/* A 256 x 1 raw-textured sprite at (x, y) from texel row v, CLUT at (0, clut_y). */
static void sprite(Words *g, int x, int y, int v, int clut_y)
{
    put(g, 0x65000000u);
    put(g, xy(x, y));
    put(g, (uint32_t)v << 8 | (uint32_t)(clut_y << 6) << 16);
    put(g, 256u | 1u << 16);
}

/* The palette a case starts with: every entry non-zero (0 is transparent). */
static uint16_t entry(int clut, int i)
{
    return (uint16_t)(((i * 37 + 5 + clut * 1000) & 0x7fff) | 1);
}

/* Texel rows: 1 the indices 0..255, 2 reversed, as 8-bit; 3 as 4-bit
   (0..15 repeating). */
static void textures(Words *g)
{
    uint16_t row[128];
    int i;

    for (i = 0; i < 128; ++i)
        row[i] = (uint16_t)(2 * i | (2 * i + 1) << 8);
    load_row(g, PAGE_X, 1, 128, row);
    for (i = 0; i < 128; ++i)
        row[i] = (uint16_t)((255 - 2 * i) | (254 - 2 * i) << 8);
    load_row(g, PAGE_X, 2, 128, row);
    for (i = 0; i < 64; ++i)
        row[i] = (uint16_t)(i % 4 == 0   ? 0x3210
                            : i % 4 == 1 ? 0x7654
                            : i % 4 == 2 ? 0xba98
                                         : 0xfedc);
    load_row(g, PAGE_X, 3, 64, row);
}

static void palette(Words *g, int clut_y, int which)
{
    uint16_t px[256];
    int i;

    for (i = 0; i < 256; ++i)
        px[i] = entry(which, i);
    load_row(g, 0, clut_y, 256, px);
}

/* A fill of the CLUT row with 15-bit colour (r 31): 0x001f. */
static void spoil_fill(Words *g, int clut_y)
{
    put(g, 0x020000ffu);
    put(g, xy(0, clut_y));
    put(g, 256u | 1u << 16);
}

/* The same with a flat rectangle, a primitive the workers draw. */
static void spoil_rect(Words *g, int clut_y)
{
    put(g, 0x6000ff00u); /* green: 0x03e0 */
    put(g, xy(0, clut_y));
    put(g, 256u | 1u << 16);
}

typedef struct {
    const char *name;
    int scale, threads, cache;
} Run;

static jb_context *make(const Run *r)
{
    jb_config cfg;
    jb_context *jb;

    memset(&cfg, 0, sizeof cfg);
    cfg.scale = r->scale;
    cfg.threads = r->threads;
    jb = jb_create(&cfg, NULL);
    if (jb == NULL) {
        printf("FAIL: no context\n");
        exit(1);
    }
    if (!r->cache)
        jbi_set_clut_cache(jb, 0);
    return jb;
}

static void row_back(jb_context *jb, int y, uint16_t *px)
{
    jb_vram_read(jb, 0, y, 256, 1, px);
}

/* Every case, into rows the caller reads back. Returns the VRAM. */
static uint16_t *cases(const Run *r)
{
    static Words g;
    uint16_t got[256];
    char what[160];
    int i, k;
    uint16_t *vram = malloc(1024 * 512 * sizeof *vram);

    g.jb = make(r);
    g.n = 0;
    environment(&g, 1);
    textures(&g);

    /* self: over its own CLUT, reversed indices. */
    palette(&g, CLUT_A, 0);
    put(&g, 0x01000000u);
    sprite(&g, 0, CLUT_A, 2, CLUT_A);
    send(&g);
    row_back(g.jb, CLUT_A, got);
    snprintf(what, sizeof what, "%s: self", r->name);
    for (i = 0; i < 256; ++i) {
        /* Cache on: the palette before the sprite. Off: entry 255 - i,
           already overwritten (by pixel 255 - i, with entry i) when i is
           128 or more. */
        unsigned want = entry(0, 255 - i);
        if (!r->cache && i >= 128)
            want = entry(0, i);
        check_px(what, i, got[i], want);
    }

    /* kept, kept-prim, cleared (three ways). */
    for (k = 0; k < 5; ++k) {
        static const char *const names[5] = {"kept", "kept-prim", "cleared GP0(01h)",
                                             "cleared GP1(01h)", "cleared GP1(00h)"};
        int y = ROW + 4 * k;

        palette(&g, CLUT_B, 1);
        put(&g, 0x01000000u);
        sprite(&g, 0, y, 1, CLUT_B);
        if (k == 1)
            spoil_rect(&g, CLUT_B);
        else
            spoil_fill(&g, CLUT_B);
        send(&g);
        if (k == 2)
            put(&g, 0x01000000u);
        if (k == 3)
            jb_gp1(g.jb, 0x01000000u);
        if (k == 4) {
            jb_gp1(g.jb, 0x00000000u);
            environment(&g, 1);
        }
        sprite(&g, 0, y + 1, 1, CLUT_B);
        send(&g);
        snprintf(what, sizeof what, "%s: %s", r->name, names[k]);
        row_back(g.jb, y, got);
        for (i = 0; i < 256; ++i)
            check_px(what, i, got[i], entry(1, i));
        row_back(g.jb, y + 1, got);
        for (i = 0; i < 256; ++i) {
            unsigned spoilt = k == 1 ? 0x03e0u : 0x001fu;
            check_px(what, i, got[i], k <= 1 && r->cache ? entry(1, i) : spoilt);
        }
    }

    /* depth: 4-bit, spoil, 8-bit (loads again); 8-bit, spoil, 4-bit (kept). */
    for (k = 0; k < 2; ++k) {
        int y = ROW + 40 + 4 * k;

        palette(&g, CLUT_B, 2);
        put(&g, 0x01000000u);
        mode(&g, k == 0 ? 0 : 1);
        sprite(&g, 0, y, k == 0 ? 3 : 1, CLUT_B);
        spoil_fill(&g, CLUT_B);
        mode(&g, k == 0 ? 1 : 0);
        sprite(&g, 0, y + 1, k == 0 ? 1 : 3, CLUT_B);
        mode(&g, 1);
        send(&g);
        snprintf(what, sizeof what, "%s: depth %s", r->name, k == 0 ? "4 then 8" : "8 then 4");
        row_back(g.jb, y + 1, got);
        for (i = 0; i < 256; ++i) {
            unsigned want = k == 0 || !r->cache ? 0x001fu : entry(2, i % 16);
            check_px(what, i, got[i], want);
        }
    }

    /* empty: a sprite the drawing area hides loads the cache. */
    palette(&g, CLUT_B, 3);
    put(&g, 0x01000000u);
    put(&g, 0xe3000000u | 500u << 10); /* the area from row 500 */
    sprite(&g, 0, ROW + 50, 1, CLUT_B);
    put(&g, 0xe3000000u);
    spoil_fill(&g, CLUT_B);
    sprite(&g, 0, ROW + 51, 1, CLUT_B);
    send(&g);
    snprintf(what, sizeof what, "%s: empty", r->name);
    row_back(g.jb, ROW + 51, got);
    for (i = 0; i < 256; ++i)
        check_px(what, i, got[i], r->cache ? entry(3, i) : 0x001fu);
    row_back(g.jb, ROW + 50, got);
    for (i = 0; i < 256; ++i)
        check_px(what, i, got[i], 0);

    /* The cached entries and the backing VRAM hold different pixels. */
    palette(&g, CLUT_A, 7);
    put(&g, 0x01000000u);
    sprite(&g, 0, ROW + 56, 1, CLUT_A);
    spoil_fill(&g, CLUT_A);
    send(&g);
    if (jbi_checkpoint_begin(g.jb) != JBI_CHECKPOINT_OK) {
        printf("FAIL %s: no checkpoint\n", r->name);
        exit(1);
    }
    put(&g, 0x01000000u);
    sprite(&g, 0, ROW + 57, 1, CLUT_A);
    jbi_gp0(g.jb, g.w, g.n);
    g.n = 0;
    jbi_store_1x(g.jb, 0, ROW + 57, 256, 1, got);
    check_px("checkpoint loads the updated palette", 0, got[0], 0x001fu);
    jbi_checkpoint_rollback(g.jb);
    row_back(g.jb, ROW + 57, got);
    check_px("checkpoint restores the drawn row", 0, got[0], 0);
    sprite(&g, 0, ROW + 58, 1, CLUT_A);
    send(&g);
    row_back(g.jb, ROW + 58, got);
    for (i = 0; i < 256; ++i)
        check_px("checkpoint restores the cached palette", i, got[i],
                 r->cache ? entry(7, i) : 0x001fu);

    /* pool: 700 loads of 256 entries between two flushes. */
    palette(&g, CLUT_A, 4);
    palette(&g, CLUT_B, 5);
    put(&g, 0x01000000u);
    for (k = 0; k < 700; ++k)
        sprite(&g, (k % 4) * 256, 100 + k / 4, 1, k & 1 ? CLUT_B : CLUT_A);
    send(&g);
    snprintf(what, sizeof what, "%s: pool", r->name);
    for (k = 0; k < 700; k += 37) {
        jb_vram_read(g.jb, (k % 4) * 256, 100 + k / 4, 256, 1, got);
        for (i = 0; i < 256; ++i)
            check_px(what, i, got[i], entry(k & 1 ? 5 : 4, i));
    }

    jb_vram_read(g.jb, 0, 0, 1024, 512, vram);
    jb_destroy(g.jb);
    return vram;
}

int main(void)
{
    static const Run runs[] = {
        {"1x 1 thread", 1, 1, 1}, {"1x 2 threads", 1, 2, 1}, {"1x 4 threads", 1, 4, 1},
        {"1x 8 threads", 1, 8, 1}, {"2x 1 thread", 2, 1, 1},  {"2x 4 threads", 2, 4, 1},
        {"3x 3 threads", 3, 3, 1}, {"3x 8 threads", 3, 8, 1},
        {"1x 1 thread, cache off", 1, 1, 0}, {"1x 4 threads, cache off", 1, 4, 0},
        {"2x 4 threads, cache off", 2, 4, 0},
    };
    uint16_t *ref = NULL, *ref_off = NULL;
    unsigned k;

    for (k = 0; k < sizeof runs / sizeof runs[0]; ++k) {
        uint16_t *v = cases(&runs[k]);
        uint16_t **first = runs[k].cache ? &ref : &ref_off;

        if (*first == NULL)
            *first = v;
        else {
            if (memcmp(*first, v, 1024 * 512 * sizeof *v) != 0) {
                ++failures;
                printf("FAIL %s: VRAM differs from the first run's\n", runs[k].name);
            }
            free(v);
        }
        printf("%s: done\n", runs[k].name);
    }
    free(ref);
    free(ref_off);
    printf("clut_cache: %d failures\n", failures);
    return failures != 0;
}

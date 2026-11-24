/*
 * Helpers for the public API's tests (tests/api_*.c): checks, GP0 word
 * builders, reading VRAM back through GP0(C0h) and GPUREAD, and a seeded
 * generator of GP0 commands. Everything here uses <jitterbug.h> only.
 * C99.
 */
#ifndef JB_TEST_API_TEST_H
#define JB_TEST_API_TEST_H

#include <jitterbug.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    int checks, failures;
} api_t;

static inline int check(const char *what, long long got, long long want)
{
    ++api_t.checks;
    if (got == want)
        return 1;
    if (++api_t.failures <= 40)
        printf("FAIL %s: got %lld (0x%llx), want %lld (0x%llx)\n", what, got,
               (unsigned long long)got, want, (unsigned long long)want);
    return 0;
}

static inline int check_str(const char *what, const char *got, const char *want)
{
    ++api_t.checks;
    if (got != NULL && strcmp(got, want) == 0)
        return 1;
    if (++api_t.failures <= 40)
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)", want);
    return 0;
}

static inline int finish(const char *name)
{
    printf("%s: %d checks, %d failures\n", name, api_t.checks, api_t.failures);
    return api_t.failures != 0;
}

static inline void *must_alloc(size_t n)
{
    void *p = calloc(n ? n : 1, 1);
    if (p == NULL) {
        printf("out of memory\n");
        exit(2);
    }
    return p;
}

static inline jb_context *must_create(const jb_config *cfg)
{
    jb_result r = JB_ERROR_UNSUPPORTED;
    jb_context *jb = jb_create(cfg, &r);

    if (jb == NULL || r != JB_OK) {
        printf("FAIL jb_create: %s\n", jb_result_string(r));
        exit(2);
    }
    return jb;
}

/* A GP0 position or size word: x in the low half, y in the high. */
static inline uint32_t xy(int x, int y)
{
    return ((uint32_t)x & 0xffffu) | ((uint32_t)y & 0xffffu) << 16;
}

static inline void gp0(jb_context *jb, const uint32_t *w, size_t n)
{
    check("jb_gp0", jb_gp0(jb, w, n), JB_OK);
}

static inline void gp0_1(jb_context *jb, uint32_t w)
{
    gp0(jb, &w, 1);
}

static inline void gp1(jb_context *jb, uint32_t w)
{
    check("jb_gp1", jb_gp1(jb, w), JB_OK);
}

/* The console's transfer sizes: 0 is the maximum. */
static inline int xfer_w(int w)
{
    return ((w - 1) & 0x3ff) + 1;
}

static inline int xfer_h(int h)
{
    return ((h - 1) & 0x1ff) + 1;
}

/* CPU to VRAM: GP0(A0h) and the pixels, two to a word. */
static inline void load_rect(jb_context *jb, int x, int y, int w, int h, const uint16_t *pix)
{
    size_t n = (size_t)xfer_w(w) * (size_t)xfer_h(h), i;
    uint32_t cmd[3];

    cmd[0] = 0xa0000000u;
    cmd[1] = xy(x, y);
    cmd[2] = xy(w, h);
    gp0(jb, cmd, 3);
    for (i = 0; i < n; i += 2) {
        uint32_t v = pix[i];
        if (i + 1 < n)
            v |= (uint32_t)pix[i + 1] << 16;
        gp0_1(jb, v);
    }
}

/* VRAM to CPU: GP0(C0h), then GPUREAD for every word; returns the pixels
   read (the transfer's size). */
static inline size_t read_rect(jb_context *jb, int x, int y, int w, int h, uint16_t *dst)
{
    size_t n = (size_t)xfer_w(w) * (size_t)xfer_h(h), i;
    uint32_t cmd[3];

    cmd[0] = 0xc0000000u;
    cmd[1] = xy(x, y);
    cmd[2] = xy(w, h);
    gp0(jb, cmd, 3);
    for (i = 0; i < n; i += 2) {
        uint32_t v = jb_gpuread(jb);
        dst[i] = (uint16_t)v;
        if (i + 1 < n)
            dst[i + 1] = (uint16_t)(v >> 16);
    }
    return n;
}

enum { VRAM_W = 1024, VRAM_H = 512, VRAM_PIXELS = VRAM_W * VRAM_H };

/* The whole 1x VRAM, row-major (free it). */
static inline uint16_t *read_vram(jb_context *jb)
{
    uint16_t *v = (uint16_t *)must_alloc((size_t)VRAM_PIXELS * 2);
    read_rect(jb, 0, 0, 0, 0, v);
    return v;
}

/* 5 bits to 8, as jb_frame widens them. */
static inline unsigned widen5(unsigned c)
{
    return (c << 3 | c >> 2) & 0xff;
}

/* ------------------------------------------------------------------------ */
/* A seeded generator of GP0 commands */

typedef struct gen {
    uint64_t s;
    int cx, cy; /* where primitives go: the middle of the drawing area */
} gen;

static inline uint32_t rnd(gen *g)
{
    g->s = g->s * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(g->s >> 32);
}

static inline int rnd_below(gen *g, int n)
{
    return (int)(rnd(g) % (uint32_t)n);
}

/* Two numbers below a and b, drawn in that order (the order of a call's
   arguments is unspecified, and the stream must not depend on the
   compiler), as a position or size word. */
static inline uint32_t rnd_xy(gen *g, int a, int b, int dx, int dy)
{
    int x = rnd_below(g, a) + dx;
    int y = rnd_below(g, b) + dy;
    return xy(x, y);
}

/* A vertex position: mostly near the middle of the drawing area, now and
   then anywhere in the 11-bit range, or any 32 bits. */
static inline uint32_t gen_xy(gen *g)
{
    int k = rnd_below(g, 32);

    if (k == 0)
        return rnd(g);
    if (k == 1)
        return rnd_xy(g, 2048, 2048, -1024, -1024);
    return rnd_xy(g, 96, 96, g->cx - 48, g->cy - 48);
}

/* A size word of two gen_size values. */
static inline int gen_size(gen *g, int max);
static inline uint32_t gen_wh(gen *g, int max_w, int max_h)
{
    int w = gen_size(g, max_w);
    int h = gen_size(g, max_h);
    return xy(w, h);
}

/* A command word: op in the high byte, random low bits. */
static inline uint32_t gen_op(gen *g, uint32_t first, int count)
{
    uint32_t op = first + (uint32_t)rnd_below(g, count);
    return op << 24 | (rnd(g) & 0xffffffu);
}

/* A small size, now and then a large one or 0 (the maximum). */
static inline int gen_size(gen *g, int max)
{
    int k = rnd_below(g, 64);

    if (k == 0)
        return 0;
    if (k == 1)
        return rnd_below(g, max + 1);
    return 1 + rnd_below(g, 24);
}

/* A texture word: coordinates, and a CLUT or page in the high half. */
static inline uint32_t gen_uv(gen *g)
{
    return rnd(g);
}

enum { GEN_MAX = 4096 }; /* the most words one gen_command writes */

/*
 * One command, biased towards complete valid ones, into out (GEN_MAX
 * words); returns its word count. With complete_loads, every A0h is
 * followed by all of its pixels; otherwise a large one may be cut short
 * by the commands after it. No GP1 words.
 */
static inline int gen_command(gen *g, uint32_t *out, int complete_loads)
{
    int n = 0, k = rnd_below(g, 100), i;
    uint32_t op, color = rnd(g) & 0xffffffu;

    if (k < 28) { /* polygons, every opcode */
        int verts, gouraud, textured;
        op = 0x20u + (uint32_t)rnd_below(g, 32);
        verts = (op & 0x08) ? 4 : 3;
        gouraud = (op & 0x10) != 0;
        textured = (op & 0x04) != 0;
        out[n++] = op << 24 | color;
        for (i = 0; i < verts; ++i) {
            if (gouraud && i > 0)
                out[n++] = rnd(g);
            out[n++] = gen_xy(g);
            if (textured)
                out[n++] = gen_uv(g);
        }
    } else if (k < 42) { /* rectangles, every opcode */
        op = 0x60u + (uint32_t)rnd_below(g, 32);
        out[n++] = op << 24 | color;
        out[n++] = gen_xy(g);
        if (op & 0x04)
            out[n++] = gen_uv(g);
        if (((op >> 3) & 3) == 0)
            out[n++] = rnd_below(g, 8) == 0 ? rnd(g) : rnd_xy(g, 80, 80, 0, 0);
    } else if (k < 52) { /* single lines */
        op = 0x40u + ((uint32_t)rnd_below(g, 32) & ~0x08u);
        out[n++] = op << 24 | color;
        out[n++] = gen_xy(g);
        if (op & 0x10)
            out[n++] = rnd(g);
        out[n++] = gen_xy(g);
    } else if (k < 57) { /* polylines, terminated */
        int verts = 2 + rnd_below(g, 8);
        op = 0x48u | ((uint32_t)rnd_below(g, 32) & 0x17u);
        out[n++] = op << 24 | color;
        out[n++] = gen_xy(g);
        for (i = 1; i < verts; ++i) {
            if (op & 0x10)
                out[n++] = rnd(g) & 0x0fffffffu; /* never a terminator */
            out[n++] = gen_xy(g) & 0x0fff0fffu;
        }
        out[n++] = rnd_below(g, 2) ? 0x55555555u : (0x50005000u | (rnd(g) & 0x0fff0fffu));
    } else if (k < 61) { /* fill */
        out[n++] = 0x02000000u | color;
        out[n++] = rnd_below(g, 4) ? xy(g->cx - 32, g->cy - 32) : rnd(g);
        out[n++] = gen_wh(g, 1023, 511);
    } else if (k < 65) { /* VRAM to VRAM */
        out[n++] = gen_op(g, 0x80, 32);
        out[n++] = rnd(g);
        out[n++] = rnd(g);
        out[n++] = gen_wh(g, 1024, 512);
    } else if (k < 69) { /* CPU to VRAM */
        int w = gen_size(g, 1024), h = gen_size(g, 512), words;
        if ((size_t)xfer_w(w) * (size_t)xfer_h(h) > 2 * (GEN_MAX - 3))
            h = 1 + rnd_below(g, 4);
        out[n++] = gen_op(g, 0xa0, 32);
        out[n++] = rnd(g);
        out[n++] = xy(w, h);
        words = (xfer_w(w) * xfer_h(h) + 1) / 2;
        if (!complete_loads && rnd_below(g, 8) == 0)
            words = rnd_below(g, words + 1);
        for (i = 0; i < words; ++i)
            out[n++] = rnd(g);
    } else if (k < 71) { /* VRAM to CPU */
        out[n++] = gen_op(g, 0xc0, 32);
        out[n++] = rnd(g);
        out[n++] = gen_wh(g, 1024, 512);
    } else if (k < 76) { /* a drawing area and offset, which moves the middle */
        int x0 = rnd_below(g, 1024), y0 = rnd_below(g, 512);
        int x1 = x0 + 16 + rnd_below(g, 200), y1 = y0 + 16 + rnd_below(g, 120);
        if (rnd_below(g, 16) == 0)
            x0 = 0, y0 = 0, x1 = 1023, y1 = 511;
        x1 = x1 > 1023 ? 1023 : x1;
        y1 = y1 > 511 ? 511 : y1;
        out[n++] = 0xe3000000u | (uint32_t)x0 | (uint32_t)y0 << 10;
        out[n++] = 0xe4000000u | (uint32_t)x1 | (uint32_t)y1 << 10;
        g->cx = (x0 + x1) / 2;
        g->cy = (y0 + y1) / 2;
        if (rnd_below(g, 2)) {
            int ox = rnd_below(g, 64) - 32, oy = rnd_below(g, 64) - 32;
            out[n++] = 0xe5000000u | ((uint32_t)ox & 0x7ff) | ((uint32_t)oy & 0x7ff) << 11;
            g->cx -= ox;
            g->cy -= oy;
        }
    } else if (k < 88) { /* other drawing state, and E0h, E7h-FFh */
        op = 0xe0u + (uint32_t)rnd_below(g, 32);
        if (op >= 0xe3 && op <= 0xe5)
            op = rnd_below(g, 2) ? 0xe1 : 0xe6;
        out[n++] = op << 24;
        out[n - 1] |= rnd(g) & 0xffffffu;
    } else if (k < 95) { /* 00h-1Fh */
        op = (uint32_t)rnd_below(g, 32);
        if (op == 0x02)
            op = 0x1f;
        out[n++] = op << 24;
        out[n - 1] |= rnd(g) & 0xffffffu;
    } else if (complete_loads) {
        /* Any one-word command: whatever follows still starts where the
           generator meant it to, so every load stays complete. */
        uint32_t w = rnd(g) & 0x1fffffffu;
        if (rnd_below(g, 2) || (w >> 24) == 0x02)
            w |= 0xe0000000u;
        out[n++] = w;
    } else {
        /* Any word: it may start a command that takes the words meant to
           follow as its own, or a polyline that runs on. */
        out[n++] = rnd(g);
    }
    return n;
}

#endif

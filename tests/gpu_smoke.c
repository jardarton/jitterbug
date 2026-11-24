/*
 * A sanity check of the GPU (src/core, src/soft), not the golden test: a
 * fill, a flat triangle, a raw textured rectangle after a CPU-to-VRAM load,
 * a VRAM-to-VRAM copy, a dithered Gouraud triangle and a line, all sent as
 * GP0 words, at 1x and 2x with 1 and 4 threads, on one context. It checks
 * a handful of pixels, that 1 and 4 threads give identical VRAM at each
 * scale, and the scanout of the display area.
 *
 * It is built twice: with the precise-vertex table (src/geom/precise.c) as
 * the context's precise source, and with tests/precise_stub.c's
 * (JB_SMOKE_STUB), to show the GPU draws with either.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/context.h"
#include "soft/soft.h"
#ifdef JB_SMOKE_STUB
#include "precise_stub.h"
#else
#include "geom/precise.h"
#endif

static jb_context *ctx;

static int failures, checks;

static void check(const char *what, unsigned got, unsigned want)
{
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got 0x%04x, want 0x%04x\n", what, got, want);
    }
}

static uint32_t xy(int x, int y)
{
    return ((uint32_t)x & 0xffff) | ((uint32_t)y & 0xffff) << 16;
}

static uint16_t rgb15(int r, int g, int b)
{
    return (uint16_t)((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10);
}

enum { TEX_X = 512, TEX_Y = 0, TEX_W = 16, TEX_H = 16 };

static uint16_t texel(int x, int y)
{
    return (uint16_t)(0x8000 | ((x + 1) & 31) | ((y + 1) & 31) << 5 | ((x ^ y) & 31) << 10);
}

/* Words for one scene. */
static int scene(uint32_t *w)
{
    int n = 0, i;

    /* Drawing state: page at (512, 0), 15-bit, no dithering; the whole of
       VRAM as the drawing area; no offset; no mask. */
    w[n++] = 0xe1000000u | (TEX_X / 64) | (2u << 7);
    w[n++] = 0xe3000000u;
    w[n++] = 0xe4000000u | 1023u | 511u << 10;
    w[n++] = 0xe5000000u;
    w[n++] = 0xe6000000u;

    /* Fill (0, 0) 64 x 32 with red. */
    w[n++] = 0x020000ffu;
    w[n++] = xy(0, 0);
    w[n++] = xy(64, 32);

    /* Flat green triangle. */
    w[n++] = 0x2000ff00u;
    w[n++] = xy(100, 100);
    w[n++] = xy(150, 100);
    w[n++] = xy(100, 150);

    /* A 16 x 16 15-bit texture at (512, 0). */
    w[n++] = 0xa0000000u;
    w[n++] = xy(TEX_X, TEX_Y);
    w[n++] = xy(TEX_W, TEX_H);
    for (i = 0; i < TEX_W * TEX_H; i += 2)
        w[n++] = (uint32_t)texel(i % TEX_W, i / TEX_W) |
                 (uint32_t)texel((i + 1) % TEX_W, (i + 1) / TEX_W) << 16;

    /* Raw textured rectangle, variable size, at (200, 200). */
    w[n++] = 0x65808080u;
    w[n++] = xy(200, 200);
    w[n++] = 0; /* CLUT unused at 15 bits, uv (0, 0) */
    w[n++] = xy(TEX_W, TEX_H);

    /* Copy the red fill's corner to (300, 300). */
    w[n++] = 0x80000000u;
    w[n++] = xy(0, 0);
    w[n++] = xy(300, 300);
    w[n++] = xy(16, 16);

    /* Dithered Gouraud triangle, then a Gouraud line. */
    w[n++] = 0xe1000000u | (TEX_X / 64) | (2u << 7) | (1u << 9);
    w[n++] = 0x300000ffu;
    w[n++] = xy(400, 50);
    w[n++] = 0x0000ff00u;
    w[n++] = xy(500, 80);
    w[n++] = 0x00ff0000u;
    w[n++] = xy(420, 170);
    w[n++] = 0x50ffffffu;
    w[n++] = xy(10, 400);
    w[n++] = 0x00102030u;
    w[n++] = xy(300, 460);
    return n;
}

static void clear(void)
{
    jbi_reset_state(ctx);
    jbi_fill(ctx, 0, 0, 1023, 256, 0);
    jbi_fill(ctx, 0, 256, 1023, 256, 0);
}

/* Checks the 1x picture (at 2x, resolved from the scaled VRAM). */
static void check_1x(const char *tag, const uint16_t *v)
{
    char what[96];
    int i, j;

#define AT(x, y) v[(y) * JBI_VRAM_W + (x)]
    snprintf(what, sizeof what, "%s fill", tag);
    check(what, AT(10, 10), rgb15(255, 0, 0));
    snprintf(what, sizeof what, "%s outside the fill", tag);
    check(what, AT(64, 10), 0);
    snprintf(what, sizeof what, "%s triangle inside", tag);
    check(what, AT(110, 110), rgb15(0, 255, 0));
    snprintf(what, sizeof what, "%s triangle's excluded corner", tag);
    check(what, AT(150, 100), 0);
    snprintf(what, sizeof what, "%s texture upload", tag);
    check(what, AT(TEX_X + 3, TEX_Y + 5), texel(3, 5));
    for (j = 0; j < TEX_H; j += 5)
        for (i = 0; i < TEX_W; i += 3) {
            snprintf(what, sizeof what, "%s textured rectangle (%d, %d)", tag, i, j);
            check(what, AT(200 + i, 200 + j), texel(i, j));
        }
    snprintf(what, sizeof what, "%s after the rectangle", tag);
    check(what, AT(200 + TEX_W, 200), 0);
    snprintf(what, sizeof what, "%s copy", tag);
    check(what, AT(305, 305), rgb15(255, 0, 0));
    snprintf(what, sizeof what, "%s after the copy", tag);
    check(what, AT(316, 305), 0);
    snprintf(what, sizeof what, "%s line start", tag);
    check(what, AT(10, 400) != 0, 1);
#undef AT
}

int main(void)
{
    static const int scale[4] = {1, 1, 2, 2}, threads[4] = {1, 4, 1, 4};
    static uint32_t words[4096];
    static uint16_t ref1x[JBI_VRAM_H * JBI_VRAM_W];
    uint16_t *ref2x = NULL;
    int nwords = scene(words), k;
    jbi_soft *soft;
#ifdef JB_SMOKE_STUB
    jbi_precise_source source = precise_stub_source();
#else
    jbi_precise *table = jbi_precise_create();
    jbi_precise_source source;

    if (table == NULL)
        return 2;
    source = jbi_precise_as_source(table);
#endif
    {
        jbi_config cfg;

        jbi_config_default(&cfg);
        cfg.precise = &source;
        ctx = jbi_context_create(&cfg);
    }
    if (ctx == NULL)
        return 2;
    soft = jbi_soft_from(jbi_context_backend(ctx));

    for (k = 0; k < 4; ++k) {
        char tag[32];
        const uint16_t *v;
        int w, h, got, consumed;

        snprintf(tag, sizeof tag, "%dx %d threads", scale[k], threads[k]);
        check("configured scale", (unsigned)jbi_configure(ctx, scale[k], -1, threads[k]),
              (unsigned)scale[k]);
        clear();
        consumed = jbi_gp0(ctx, words, nwords);
        check("words consumed", (unsigned)consumed, (unsigned)nwords);
        jbi_flush(ctx);

        /* Above 1x, the scaled copy: each 1x pixel an N x N block. */
        v = jbi_soft_hires_vram(soft, &w, &h);
        check("scaled width", (unsigned)w, (unsigned)(JBI_VRAM_W * scale[k]));
        if (scale[k] == 2) {
            check("2x triangle", v[(2 * 110 + 1) * w + 2 * 110 + 1], rgb15(0, 255, 0));
            check("2x copy", v[(2 * 305) * w + 2 * 305], rgb15(255, 0, 0));
            check("2x textured rectangle", v[(2 * 205) * w + 2 * 203 + 1], texel(3, 5));
            if (ref2x == NULL) {
                ref2x = malloc((size_t)w * h * sizeof(uint16_t));
                if (ref2x == NULL)
                    return 2;
                memcpy(ref2x, v, (size_t)w * h * sizeof(uint16_t));
            } else {
                check("2x: 4 threads match 1 thread",
                      memcmp(ref2x, v, (size_t)w * h * sizeof(uint16_t)) == 0, 1);
            }
        }

        /* The 1x VRAM. */
        v = jbi_soft_vram(soft);
        check_1x(tag, v);
        if (k == 0)
            memcpy(ref1x, v, sizeof ref1x);
        else if (k == 1)
            check("1x: 4 threads match 1 thread", memcmp(ref1x, v, sizeof ref1x) == 0, 1);

        /* Scanout of a 320 x 240 display area at the origin. */
        {
            uint16_t *frame = malloc(jbi_scanout_bytes(ctx));
            int fw, fh, stride;

            if (frame == NULL)
                return 2;
            got = jbi_scanout(ctx, 0, 0, 320, 240, 0, 1, frame, &fw, &fh, &stride);
            check("scanout format", (unsigned)got, JBI_SCANOUT_BGR555);
            check("scanout width", (unsigned)fw, (unsigned)(320 * scale[k]));
            check("scanout height", (unsigned)fh, (unsigned)(240 * scale[k]));
            check("scanout stride", (unsigned)stride, (unsigned)(320 * 2 * scale[k]));
            check("scanout triangle", frame[(110 * scale[k]) * (stride / 2) + 110 * scale[k]],
                  rgb15(0, 255, 0));
            jbi_scanout(ctx, 0, 0, 320, 240, 0, 0, frame, &fw, &fh, &stride);
            check("scanout disabled is black",
                  frame[(110 * scale[k]) * (stride / 2) + 110 * scale[k]], 0);
            free(frame);
        }
    }
    free(ref2x);
    jbi_context_destroy(ctx);
#ifndef JB_SMOKE_STUB
    jbi_precise_destroy(table);
#endif
    printf("gpu_smoke: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}

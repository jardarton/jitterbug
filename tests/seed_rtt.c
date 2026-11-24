/*
 * Drawing into VRAM and then using what was drawn as a 15-bit texture,
 * above 1x. A drawn texture has detail inside each texel of the scaled
 * VRAM, which an uploaded one never has, so this is the only test that
 * reaches the scaled rasteriser's sub-texel sampling (raster_hires.c,
 * tex_fetch). The recorded scenes in tests/seed do not.
 *
 * The expected hashes were produced by the seed as first committed, before
 * any refactoring, and then moved by the hardware fixes recorded in
 * tests/seed/CHANGES.md (line ties, and the interpolation of colours and
 * texture coordinates). A deliberate fix updates them in the same commit,
 * with its evidence.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/context.h"
#include "precise_stub.h"
#include "soft/soft.h"

static uint32_t xy(int x, int y)
{
    return ((uint32_t)x & 0xffff) | ((uint32_t)y & 0xffff) << 16;
}

static uint32_t uv(int u, int v, unsigned high)
{
    return (uint32_t)u | (uint32_t)v << 8 | (uint32_t)high << 16;
}

/* The page at (0, 0), 15-bit. */
enum { TPAGE = 2 << 7 };

static int scene(uint32_t *w)
{
    int n = 0;

    w[n++] = 0xe1000000u | TPAGE | (1u << 9); /* dithering on */
    w[n++] = 0xe3000000u;
    w[n++] = 0xe4000000u | 1023u | 511u << 10;
    w[n++] = 0xe5000000u;
    w[n++] = 0xe6000000u;

    /* The texture, drawn: a Gouraud triangle, a flat one over part of it,
       and two Gouraud lines, inside (0, 0)-(63, 63). */
    w[n++] = 0x300000ffu;
    w[n++] = xy(2, 3);
    w[n++] = 0x0000ff00u;
    w[n++] = xy(61, 9);
    w[n++] = 0x00ff0000u;
    w[n++] = xy(17, 60);
    w[n++] = 0x2040c0e0u;
    w[n++] = xy(30, 20);
    w[n++] = xy(55, 44);
    w[n++] = xy(28, 50);
    w[n++] = 0x50ffffffu;
    w[n++] = xy(0, 62);
    w[n++] = 0x00204080u;
    w[n++] = xy(63, 1);
    w[n++] = 0x5000ffffu;
    w[n++] = xy(5, 5);
    w[n++] = 0x00ff00ffu;
    w[n++] = xy(40, 58);

    /* Magnified about three times: a modulated textured triangle. */
    w[n++] = 0x24808080u;
    w[n++] = xy(100, 20);
    w[n++] = uv(0, 0, 0);
    w[n++] = xy(290, 35);
    w[n++] = uv(63, 5, TPAGE);
    w[n++] = xy(130, 200);
    w[n++] = uv(10, 63, 0);

    /* A raw textured quad, magnified unevenly and sheared. */
    w[n++] = 0x2d808080u;
    w[n++] = xy(320, 10);
    w[n++] = uv(4, 2, 0);
    w[n++] = xy(470, 30);
    w[n++] = uv(60, 4, TPAGE);
    w[n++] = xy(300, 190);
    w[n++] = uv(2, 61, 0);
    w[n++] = xy(500, 230);
    w[n++] = uv(62, 63, 0);

    /* A Gouraud-modulated, semi-transparent textured quad over the last
       two, slightly minified along x. */
    w[n++] = 0x3eff8040u;
    w[n++] = xy(200, 100);
    w[n++] = uv(0, 0, 0);
    w[n++] = 0x0040ff80u;
    w[n++] = xy(250, 104);
    w[n++] = uv(63, 0, TPAGE);
    w[n++] = 0x008040ffu;
    w[n++] = xy(196, 260);
    w[n++] = uv(0, 63, 0);
    w[n++] = 0x00c0c0c0u;
    w[n++] = xy(256, 250);
    w[n++] = uv(63, 63, 0);

    /* A raw textured rectangle at 1:1, and one with the texture window
       repeating an 8 x 8 corner. */
    w[n++] = 0x65808080u;
    w[n++] = xy(520, 300);
    w[n++] = uv(0, 0, 0);
    w[n++] = xy(64, 64);
    w[n++] = 0xe2000000u | 0x1f | 0x1f << 5 | 2 << 10 | 3 << 15;
    w[n++] = 0x65808080u;
    w[n++] = xy(600, 300);
    w[n++] = uv(0, 0, 0);
    w[n++] = xy(40, 24);
    w[n++] = 0xe2000000u;
    return n;
}

static uint64_t fnv(const void *data, size_t bytes)
{
    const unsigned char *p = data;
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    size_t i;

    for (i = 0; i < bytes; ++i)
        h = (h ^ p[i]) * UINT64_C(0x100000001b3);
    return h;
}

static const struct {
    int scale;
    uint64_t hash_1x, hash_scaled;
} expected[] = {
    {2, UINT64_C(0xc1aa0f9f340341ff), UINT64_C(0x098c7dc4dc1c5311)},
    {3, UINT64_C(0xc1aa0f9f340341ff), UINT64_C(0x5332bb55d10523ea)},
    {4, UINT64_C(0xc1aa0f9f340341ff), UINT64_C(0xab81bf5f721cf104)},
};

int main(int argc, char **argv)
{
    static uint32_t words[256];
    int print = argc > 1 && strcmp(argv[1], "--print") == 0;
    int count = scene(words), failures = 0;
    unsigned k, threads;
    jbi_precise_source source = precise_stub_source();
    jbi_config cfg;
    jb_context *ctx;
    jbi_soft *soft;

    jbi_config_default(&cfg);
    cfg.precise = &source;
    ctx = jbi_context_create(&cfg);
    if (ctx == NULL)
        return 2;
    soft = jbi_soft_from(jbi_context_backend(ctx));

    for (k = 0; k < sizeof expected / sizeof expected[0]; ++k) {
        for (threads = 1; threads <= 4; threads += 3) {
            int scale = expected[k].scale, w, h;
            const uint16_t *scaled;
            uint64_t h1, hs;

            jbi_configure(ctx, scale, -1, (int)threads);
            jbi_set_exact_1x(ctx, 1);
            jbi_reset_state(ctx);
            jbi_fill(ctx, 0, 0, 1023, 256, 0);
            jbi_fill(ctx, 0, 256, 1023, 256, 0);
            if (jbi_gp0(ctx, words, count) != count) {
                printf("FAIL: jbi_gp0 did not take every word\n");
                jbi_context_destroy(ctx);
                return 1;
            }
            scaled = jbi_soft_hires_vram(soft, &w, &h);
            hs = fnv(scaled, (size_t)w * (size_t)h * 2);
            h1 = fnv(jbi_soft_vram(soft), (size_t)JBI_VRAM_W * JBI_VRAM_H * 2);
            if (print) {
                printf("    {%d, UINT64_C(0x%016llx), UINT64_C(0x%016llx)},  /* %u threads */\n",
                       scale, (unsigned long long)h1, (unsigned long long)hs, threads);
            } else if (h1 != expected[k].hash_1x || hs != expected[k].hash_scaled) {
                printf("FAIL %dx, %u threads: 1x %016llx (want %016llx), scaled %016llx "
                       "(want %016llx)\n",
                       scale, threads, (unsigned long long)h1,
                       (unsigned long long)expected[k].hash_1x, (unsigned long long)hs,
                       (unsigned long long)expected[k].hash_scaled);
                failures = 1;
            }
        }
    }
    jbi_context_destroy(ctx);
    if (!print && !failures)
        printf("ok\n");
    return failures;
}

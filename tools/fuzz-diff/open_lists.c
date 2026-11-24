/*
 * The open-question lists (docs/testing.md, "Open-question lists and
 * console probe results"): each a small,
 * hand-built experiment in which one unknown, and only one, can change
 * the result, written by `fuzz-diff open-lists DIR` (main.c adds the
 * prelude every list starts with: GP1(00h), VRAM cleared by four fills).
 *
 * Every list draws with the drawing area the whole of VRAM, the drawing
 * offset 0 unless the experiment is about it, no texture window unless
 * it is needed, the mask settings off and dithering off unless the
 * experiment is about dithering. Each draws a control next to the
 * experiment where one is needed to read the answer.
 */
#include "fd.h"

#include <string.h>

static void put(fd_stream *s, const uint32_t *w, int n)
{
    fd_cmd *c = fd_stream_add(s, w, n);
    c->group = s->n;
}

static void one(fd_stream *s, uint32_t w)
{
    put(s, &w, 1);
}

static uint32_t xy(int x, int y)
{
    return ((uint32_t)x & 0xffffu) | ((uint32_t)y & 0xffffu) << 16;
}

/* The plain drawing state, with E1h = e1. */
static void plain(fd_stream *s, uint32_t e1)
{
    one(s, 0xe1000000u | e1);
    one(s, 0xe2000000u);
    one(s, 0xe3000000u);
    one(s, 0xe4000000u | 1023u | 511u << 10);
    one(s, 0xe5000000u);
    one(s, 0xe6000000u);
}

/* A CPU-to-VRAM load of w x h pixels, all p. */
static void load_all(fd_stream *s, int x, int y, int w, int h, uint16_t p)
{
    uint32_t words[3 + 512];
    int n = 0, i;

    words[n++] = 0xa0000000u;
    words[n++] = xy(x, y);
    words[n++] = xy(w, h);
    for (i = 0; i < (w * h + 1) / 2; ++i)
        words[n++] = (uint32_t)p | (uint32_t)p << 16;
    put(s, words, n);
}

static void fill(fd_stream *s, uint32_t rgb, int x, int y, int w, int h)
{
    uint32_t words[3];

    words[0] = 0x02000000u | rgb;
    words[1] = xy(x, y);
    words[2] = xy(w, h);
    put(s, words, 3);
}

/* offset-wrap: a rectangle at vertex x 40 with the drawing offset x 1000:
   the sum, 1040, is outside -1024..1023. A control rectangle with offset 0
   first. Wrapped to 11 bits (1040 - 2048 = -1008), the 1023-wide
   rectangle covers x -1008..14, so x 0..14 of rows 100..107 are red;
   used as it is, it starts at 1040 and draws nothing. */
static void offset_wrap(fd_stream *s)
{
    uint32_t w[3];

    plain(s, 0);
    w[0] = 0x60f8f8f8u; /* the control: white, 16 x 8 at (8, 8) */
    w[1] = xy(8, 8);
    w[2] = xy(16, 8);
    put(s, w, 3);
    one(s, 0xe5000000u | 1000u);
    w[0] = 0x600000f8u; /* red, 1023 x 8 at vertex (40, 100) */
    w[1] = xy(40, 100);
    w[2] = xy(1023, 8);
    put(s, w, 3);
}

/* quad-order: an opaque Gouraud quad whose triangles overlap (v0 and v3
   both right of the edge v1 v2). Triangle v0 v1 v2 has red but no blue
   (v0 red, v1 and v2 green), triangle v1 v2 v3 blue but no red (v3
   blue): each channel is exactly 0 over the whole of the triangle whose
   vertices have none of it, whatever the interpolation. Where they
   overlap, blue means v1 v2 v3 was drawn last, red v0 v1 v2. Both
   triangles have a doubled area of 4096, so that every colour gradient is
   exact in 12 fractional bits and no interpolation rule can make a
   difference anywhere else. */
static void quad_order(fd_stream *s)
{
    uint32_t w[8];
    int n = 0;

    plain(s, 0);
    w[n++] = 0x380000f8u; /* v0 red */
    w[n++] = xy(164, 112);
    w[n++] = 0x008000u; /* v1 green */
    w[n++] = xy(100, 96);
    w[n++] = 0x008000u; /* v2 green */
    w[n++] = xy(100, 160);
    w[n++] = 0xf80000u; /* v3 blue */
    w[n++] = xy(164, 144);
    put(s, w, n);
}

/* The texture page of these lists: 15-bit, at (512, 256). */
#define PAGE15 (8u | 0x10u | 0x100u)

/* raw-gouraud-dither: a raw-textured Gouraud triangle (GP0(35h)) whose
   every texel is (21, 13, 6), drawn twice: at (20, 20) with dithering
   off, and with the offset (64, 0) (the same dither phase) with it on.
   Raw, both copies are that texel at every pixel; dithered, the second
   is one step lower in each channel where the matrix offset is negative. */
static void raw_gouraud_dither(fd_stream *s)
{
    uint32_t w[12];
    int n = 0, pass;
    const uint16_t texel = 21 | 13 << 5 | 6 << 10;

    plain(s, 0);
    load_all(s, 512, 256, 16, 16, texel);
    w[n++] = 0x350000f8u; /* colours sent, and unused */
    w[n++] = xy(20, 20);
    w[n++] = 2u | 2u << 8;
    w[n++] = 0x00f800u;
    w[n++] = xy(60, 24);
    w[n++] = 13u | 3u << 8 | PAGE15 << 16;
    w[n++] = 0xf80000u;
    w[n++] = xy(30, 56);
    w[n++] = 4u | 13u << 8;
    for (pass = 0; pass < 2; ++pass) {
        if (pass) {
            one(s, 0xe1000200u | PAGE15);
            one(s, 0xe5000000u | 64u);
        }
        put(s, w, n);
    }
}

/* dither-semi: an equal-colour Gouraud triangle (99, 99, 99) dithered,
   opaque at (8, 8) as the control, and semi-transparent (mode 0) at
   (72, 8) (the same dither phase), both over 10 in every channel. The
   opaque one is 11 where the matrix offset is -4 and 12 elsewhere.
   Dithered before the blend: (10 + 11) / 2 = 10 where it is -4, else 11;
   blended in 8 bits first, (80 + 99) / 2 = 89, then dithered: 10 where
   it is -4, -3 or -2, else 11. */
static void dither_semi(fd_stream *s)
{
    uint32_t w[6];
    int k;

    plain(s, 0x200u);
    fill(s, 0x505050u, 0, 0, 160, 80);
    for (k = 0; k < 2; ++k) {
        int x = 8 + 64 * k, n = 0;
        w[n++] = (k ? 0x32000000u : 0x30000000u) | 0x636363u;
        w[n++] = xy(x, 8);
        w[n++] = 0x636363u;
        w[n++] = xy(x + 48, 8);
        w[n++] = 0x636363u;
        w[n++] = xy(x, 56);
        put(s, w, n);
    }
}

/* The texture row of the modulated-semi lists: texel k (k = 1..31 in
   every channel, bit 15 set) at u = k of row 256 of the 15-bit page. */
static void texel_row(fd_stream *s)
{
    uint32_t words[3 + 16];
    int n = 0, k;

    words[n++] = 0xa0000000u;
    words[n++] = xy(512, 256);
    words[n++] = xy(32, 1);
    for (k = 0; k < 32; k += 2) {
        uint32_t a = k ? (uint32_t)(k | k << 5 | k << 10 | 0x8000) : 0;
        uint32_t b = (uint32_t)((k + 1) | (k + 1) << 5 | (k + 1) << 10 | 0x8000);
        words[n++] = a | b << 16;
    }
    put(s, words, n);
}

/* A 31 x 1 semi-transparent modulated sprite of texels 1..31 at (0, y),
   colour rgb, over 8 in every channel. */
static void sprite_row(fd_stream *s, int y, uint32_t rgb)
{
    uint32_t w[4];

    fill(s, 0x404040u, 0, y, 32, 1);
    w[0] = 0x66000000u | rgb;
    w[1] = xy(0, y);
    w[2] = 1u; /* u = 1, v = 0, no CLUT */
    w[3] = xy(31, 1);
    put(s, w, 4);
}

/* modulated-semi-quarter: mode 3 (B + F / 4) on a modulated texel that
   does not pass 31 (colour 127, 100, 64). psx-spx: F is the new pixel, the
   texel as modulation leaves it; so B + ((t c / 128) / 4). psxstack takes
   B + ((t / 4) c / 128). They differ for many t: row 100 shows which. Row
   102, colour 128, is the control, where both give B + t / 4. */
static void modulated_semi_quarter(fd_stream *s)
{
    plain(s, PAGE15 | 3u << 5);
    texel_row(s);
    sprite_row(s, 100, 127u | 100u << 8 | 64u << 16);
    sprite_row(s, 102, 0x808080u);
}

/* modulated-semi-saturate: mode 0 ((B + F) / 2) on modulated texels that
   pass 31 (colour 255: t 255 / 128 is above 31 from t = 17). Saturated
   before the blend, texel 31 gives (8 + 31) / 2 = 19; not, (8 + 61) / 2
   saturated, 31. Row 100. */
static void modulated_semi_saturate(fd_stream *s)
{
    plain(s, PAGE15);
    texel_row(s);
    sprite_row(s, 100, 0xffffffu);
}

/* gouraud-texture-pairs: an opaque Gouraud modulated textured triangle,
   undithered, every texel 31, red stepping by exactly 8 a pixel along
   each row (0 at x 0, 248 at x 31: a gradient exact in any precision).
   Per pixel, pixel x has red 31 * 8x / 128; per pair of pixels (psxstack:
   the colour of the even pixel for the odd one after it, counting from
   the row's first pixel, x = 0), pixel 2k + 1 equals pixel 2k. */
static void gouraud_texture_pairs(fd_stream *s)
{
    uint32_t w[9];
    int n = 0;

    plain(s, 0);
    load_all(s, 512, 256, 2, 1, 0x7fff);
    w[n++] = 0x34000000u; /* v0 red 0 */
    w[n++] = xy(0, 200);
    w[n++] = 0u;
    w[n++] = 248u; /* v1 red 248 */
    w[n++] = xy(31, 200);
    w[n++] = PAGE15 << 16;
    w[n++] = 0u; /* v2 red 0 */
    w[n++] = xy(0, 216);
    w[n++] = 0u;
    put(s, w, n);
}

const fd_open_list fd_open_lists[] = {
    {"offset-wrap", offset_wrap},
    {"quad-order", quad_order},
    {"raw-gouraud-dither", raw_gouraud_dither},
    {"dither-semi", dither_semi},
    {"modulated-semi-quarter", modulated_semi_quarter},
    {"modulated-semi-saturate", modulated_semi_saturate},
    {"gouraud-texture-pairs", gouraud_texture_pairs},
};
const int fd_open_list_count = (int)(sizeof fd_open_lists / sizeof fd_open_lists[0]);

void fd_open_list_build(int i, fd_stream *s)
{
    memset(s, 0, sizeof *s);
    fd_open_lists[i].build(s);
}

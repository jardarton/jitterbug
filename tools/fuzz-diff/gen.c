/*
 * The stream generator (fd.h; the exploratory streams of
 * docs/testing.md, "Three modes").
 *
 * A stream is: the background (32 loads of 128 x 128 pixels that set every
 * pixel of VRAM to seeded data: zeros, mask bits, small values, so that
 * textures and palettes read something), a first drawing state, then
 * commands. Commands are biased toward the interesting: small and
 * degenerate primitives, vertices at and beyond the drawing area and the
 * VRAM edges, the size limits, every blend mode with the mask bits set and
 * tested, texture windows, pages and palettes that wrap, dithering on and
 * off, transfers and copies that overlap and wrap, fills.
 *
 * Every random number is drawn in a statement of its own.
 */
#include "fd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */

uint32_t fd_r32(fd_rng *r)
{
    /* splitmix64 */
    uint64_t z;

    r->s += 0x9e3779b97f4a7c15ull;
    z = r->s;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return (uint32_t)(z >> 32);
}

int fd_below(fd_rng *r, int n)
{
    return n <= 1 ? 0 : (int)(fd_r32(r) % (uint32_t)n);
}

int fd_range(fd_rng *r, int lo, int hi)
{
    return lo + fd_below(r, hi - lo + 1);
}

void fd_rng_stream(fd_rng *r, uint64_t seed, uint64_t index)
{
    r->s = seed * 0xd1342543de82ef95ull + index * 0x2545f4914f6cdd1dull + 1;
    (void)fd_r32(r);
}

/* ------------------------------------------------------------------------ */

void fd_stream_free(fd_stream *s)
{
    int i;

    for (i = 0; i < s->n; ++i)
        free(s->c[i].w);
    free(s->c);
    memset(s, 0, sizeof *s);
}

static void *must(void *p)
{
    if (p == NULL) {
        fprintf(stderr, "fuzz-diff: out of memory\n");
        exit(2);
    }
    return p;
}

fd_cmd *fd_stream_add(fd_stream *s, const uint32_t *w, int n)
{
    fd_cmd *c;

    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->c = must(realloc(s->c, (size_t)s->cap * sizeof *s->c));
    }
    c = &s->c[s->n++];
    memset(c, 0, sizeof *c);
    c->w = must(malloc((size_t)(n ? n : 1) * 4));
    memcpy(c->w, w, (size_t)n * 4);
    c->n = n;
    return c;
}

void fd_stream_copy(fd_stream *dst, const fd_stream *src)
{
    int i;

    memset(dst, 0, sizeof *dst);
    for (i = 0; i < src->n; ++i) {
        fd_cmd *c = fd_stream_add(dst, src->c[i].w, src->c[i].n);
        c->reads = src->c[i].reads;
        c->background = src->c[i].background;
        c->group = src->c[i].group;
        c->bounded = src->c[i].bounded;
    }
}

void fd_stream_remove(fd_stream *s, int at, int count)
{
    int i;

    for (i = at; i < at + count; ++i)
        free(s->c[i].w);
    memmove(&s->c[at], &s->c[at + count], (size_t)(s->n - at - count) * sizeof *s->c);
    s->n -= count;
}

long fd_stream_words(const fd_stream *s)
{
    long n = 0;
    int i;

    for (i = 0; i < s->n; ++i)
        n += s->c[i].n;
    return n;
}

const char *const fd_focus_names[FD_FOCUS_COUNT] = {"all", "xflip", "lineties", "lineg", "quad",
                                                    "v"};

/* ------------------------------------------------------------------------ */
/* The generator's view of the drawing state, to aim primitives */

typedef struct G G;
static void gen_background(G *g);

struct G {
    fd_rng r;
    const fd_gen_options *o;
    fd_stream *s;
    int ax0, ay0, ax1, ay1; /* drawing area */
    int ox, oy;             /* drawing offset */
    uint32_t w[1100];       /* the command being built */
    int n;
};

static int R(G *g, int n)
{
    return fd_below(&g->r, n);
}

static int RR(G *g, int lo, int hi)
{
    return fd_range(&g->r, lo, hi);
}

static uint32_t R32(G *g)
{
    return fd_r32(&g->r);
}

static void put(G *g, uint32_t w)
{
    g->w[g->n++] = w;
}

static fd_cmd *emit(G *g)
{
    fd_cmd *c = fd_stream_add(g->s, g->w, g->n);
    g->n = 0;
    return c;
}

static uint32_t xy16(int x, int y)
{
    return ((uint32_t)x & 0xffffu) | ((uint32_t)y & 0xffffu) << 16;
}

/* A colour: random, an extreme, or the neutral 128 of modulation. */
static uint32_t colour(G *g)
{
    int k = R(g, 8);
    uint32_t c;

    if (k == 0)
        return 0x808080u;
    if (k == 1) {
        c = R(g, 2) ? 0xffffffu : 0;
        return c;
    }
    if (k == 2) {
        int r = R(g, 2) ? 255 : 0;
        int gg = R(g, 2) ? 255 : 0;
        int b = R(g, 2) ? 255 : 0;
        return (uint32_t)r | (uint32_t)gg << 8 | (uint32_t)b << 16;
    }
    c = R32(g) & 0xffffffu;
    return c;
}

/* A screen position for a vertex word: its value before the offset. Upper
   bits beyond the 11 the GPU reads are sometimes set. */
static uint32_t vertex_word(G *g, int x, int y)
{
    uint32_t w = xy16(x - g->ox, y - g->oy) & 0x07ff07ffu;

    if (R(g, 16) == 0) {
        uint32_t junk = R32(g) & 0xf800f800u;
        w |= junk;
    }
    return w;
}

/* A point of interest: the middle of the area, near one of its edges or
   corners, near the VRAM's edges, or anywhere in the 11-bit range. */
static void anchor(G *g, int *x, int *y)
{
    int k = R(g, 16);
    int cx = (g->ax0 + g->ax1) / 2, cy = (g->ay0 + g->ay1) / 2;

    if (k < 7) {
        int dx = RR(g, -40, 40);
        int dy = RR(g, -40, 40);
        *x = cx + dx;
        *y = cy + dy;
    } else if (k < 12) { /* the area's edges */
        int side = R(g, 4);
        int d = RR(g, -3, 3);
        int along_x = RR(g, g->ax0 - 4, g->ax1 + 4);
        int along_y = RR(g, g->ay0 - 4, g->ay1 + 4);
        *x = side == 0 ? g->ax0 + d : side == 1 ? g->ax1 + d : along_x;
        *y = side == 2 ? g->ay0 + d : side == 3 ? g->ay1 + d : along_y;
    } else if (k < 14) { /* VRAM's edges */
        int ex = R(g, 2) ? RR(g, -3, 3) : RR(g, 1020, 1027);
        int ey = R(g, 2) ? RR(g, -3, 3) : RR(g, 508, 515);
        *x = ex;
        *y = ey;
    } else {
        int fx = RR(g, -1024, 1023);
        int fy = RR(g, -1024, 1023);
        *x = fx;
        *y = fy;
    }
}

/* A spread for the other vertices: tiny, small, medium, large, or at the
   size limits. */
static int spread(G *g, int limit)
{
    int k = R(g, 16);

    if (k < 4)
        return RR(g, 0, 3);
    if (k < 9)
        return RR(g, 0, 24);
    if (k < 13)
        return RR(g, 0, 160);
    if (k < 15)
        return RR(g, 0, limit + 2);
    return limit + RR(g, -1, 1);
}

/* A texture coordinate word's u and v: random, or near 0 or 255. */
static int texcoord(G *g)
{
    int k = R(g, 4);

    if (k == 0)
        return RR(g, 0, 7);
    if (k == 1)
        return RR(g, 248, 255);
    return R(g, 256);
}

/* A CLUT field (bits 16-31 of the first texture word). */
static uint32_t clut_field(G *g)
{
    int k = R(g, 4);
    int cx, cy;

    if (k == 0) { /* the right edge: 8-bit palettes there run past 1023 */
        cx = RR(g, 48, 63);
        cy = R(g, 512);
    } else {
        cx = R(g, 64);
        cy = R(g, 512);
    }
    return (uint32_t)cx | (uint32_t)cy << 6;
}

/* A page attribute: page, blend mode, depth (3 now and then), bit 11. */
static uint32_t page_field(G *g)
{
    int k = R(g, 16);
    uint32_t px = (uint32_t)R(g, 16);
    uint32_t py = (uint32_t)R(g, 2);
    uint32_t abr = (uint32_t)R(g, 4);
    uint32_t depth = (uint32_t)(k == 0 ? 3 : R(g, 3));
    uint32_t b11 = (uint32_t)(R(g, 8) == 0);

    return px | py << 4 | abr << 5 | depth << 7 | b11 << 11;
}

/* GP0(01h) before a 4- or 8-bit primitive, when the CLUT cache is to be
   kept out (fd_gen_options.avoid_unmodelled). */
static void cache_guard(G *g, uint32_t page)
{
    if (g->o->avoid_unmodelled && ((page >> 7) & 3) < 2) {
        put(g, 0x01000000u);
        emit(g);
    }
}

/* ------------------------------------------------------------------------ */
/* State */

static void gen_e1(G *g)
{
    uint32_t page = page_field(g) & 0x1ffu;
    uint32_t dither = (uint32_t)R(g, 2);
    uint32_t dfe = (uint32_t)R(g, 2);
    uint32_t b11 = (uint32_t)(R(g, 8) == 0);
    uint32_t fx = 0, fy = 0;

    if (!g->o->avoid_unmodelled || g->o->focus == FD_FOCUS_XFLIP) {
        int k = R(g, g->o->focus == FD_FOCUS_XFLIP ? 2 : 8);
        if (k == 0) {
            fx = (uint32_t)R(g, 2);
            fy = (uint32_t)R(g, 2);
        }
        if (g->o->focus == FD_FOCUS_XFLIP)
            fx = 1;
    }
    put(g, 0xe1000000u | page | dither << 9 | dfe << 10 | b11 << 11 | fx << 12 | fy << 13);
    emit(g);
}

static void gen_e2(G *g)
{
    int k = R(g, 4);
    uint32_t w = 0;

    if (k != 0)
        w = R32(g) & 0xfffffu;
    put(g, 0xe2000000u | w);
    emit(g);
}

static void gen_area(G *g)
{
    int k = R(g, 16);
    int x0, y0, x1, y1;

    if (k == 0) {
        x0 = 0, y0 = 0, x1 = 1023, y1 = 511;
    } else if (k == 1) { /* one pixel, or inverted */
        x0 = R(g, 1024);
        y0 = R(g, 512);
        x1 = x0 + RR(g, -2, 0);
        y1 = y0 + RR(g, -2, 0);
    } else if (k == 2) { /* against the VRAM's far edges */
        x0 = RR(g, 900, 1023);
        y0 = RR(g, 400, 511);
        x1 = 1023;
        y1 = 511;
    } else {
        x0 = R(g, 1000);
        y0 = R(g, 500);
        x1 = x0 + RR(g, 8, 220);
        y1 = y0 + RR(g, 8, 140);
    }
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > 1023)
        x1 = 1023;
    if (y1 > 511)
        y1 = 511;
    if (x1 < 0)
        x1 = 0;
    if (y1 < 0)
        y1 = 0;
    put(g, 0xe3000000u | (uint32_t)x0 | (uint32_t)y0 << 10);
    emit(g);
    put(g, 0xe4000000u | (uint32_t)x1 | (uint32_t)y1 << 10);
    emit(g);
    g->ax0 = x0, g->ay0 = y0, g->ax1 = x1 < x0 ? x0 : x1, g->ay1 = y1 < y0 ? y0 : y1;
}

static void gen_offset(G *g)
{
    int k = R(g, 8);
    int ox, oy;

    if (k == 0) {
        ox = RR(g, -1024, 1023);
        oy = RR(g, -1024, 1023);
    } else if (k < 3) {
        ox = g->ax0;
        oy = g->ay0;
    } else {
        ox = RR(g, -32, 32);
        oy = RR(g, -32, 32);
    }
    put(g, 0xe5000000u | ((uint32_t)ox & 0x7ffu) | ((uint32_t)oy & 0x7ffu) << 11);
    emit(g);
    g->ox = ox, g->oy = oy;
}

static void gen_mask(G *g)
{
    uint32_t m = (uint32_t)R(g, 4);

    put(g, 0xe6000000u | m);
    emit(g);
}

/* ------------------------------------------------------------------------ */
/* Primitives */

static void gen_polygon(G *g, int force_quad, int force_flags, int flags_mask)
{
    uint32_t op = 0x20u | (uint32_t)R(g, 32);
    int verts, gouraud, textured, i, ax, ay, sx, sy, shape;
    int px[4], py[4];
    uint32_t page = page_field(g), clut = clut_field(g), c0 = colour(g);
    int flat = R(g, 4) == 0; /* Gouraud with every vertex the same colour */

    op = (op & ~(uint32_t)flags_mask) | (uint32_t)force_flags;
    if (force_quad)
        op |= 0x08;
    verts = (op & 0x08) ? 4 : 3;
    gouraud = (op & 0x10) != 0;
    textured = (op & 0x04) != 0;
    anchor(g, &ax, &ay);
    sx = spread(g, 1023);
    sy = spread(g, 511);
    shape = R(g, 8);
    for (i = 0; i < verts; ++i) {
        int dx = RR(g, -sx, sx);
        int dy = RR(g, -sy, sy);
        px[i] = ax + dx;
        py[i] = ay + dy;
    }
    if (shape == 0) { /* degenerate: a repeated vertex or a line */
        int k = R(g, 2);
        if (k)
            px[2] = px[1], py[2] = py[1];
        else
            px[2] = 2 * px[1] - px[0], py[2] = 2 * py[1] - py[0];
    } else if (shape == 1) { /* exactly at the limits */
        int wide = R(g, 2);
        int d = RR(g, -1, 1);
        if (wide)
            px[1] = px[0] + 1023 + d;
        else
            py[1] = py[0] + 511 + d;
    } else if (shape == 2 && verts == 4) { /* an axis-aligned quad */
        int w = RR(g, 1, 40);
        int h = RR(g, 1, 40);
        px[1] = px[0] + w, py[1] = py[0];
        px[2] = px[0], py[2] = py[0] + h;
        px[3] = px[0] + w, py[3] = py[0] + h;
    }
    if (textured && g->o->avoid_unmodelled)
        cache_guard(g, page);
    put(g, op << 24 | c0);
    for (i = 0; i < verts; ++i) {
        if (gouraud && i > 0)
            put(g, flat ? c0 : colour(g));
        put(g, vertex_word(g, px[i], py[i]));
        if (textured) {
            int u = texcoord(g);
            int v = texcoord(g);
            uint32_t hi = i == 0 ? clut : i == 1 ? page : (R32(g) & 0xffffu);
            put(g, (uint32_t)u | (uint32_t)v << 8 | hi << 16);
        }
    }
    emit(g);
}

/* A textured triangle whose v varies and u does not, or u and v both. */
static void gen_v_triangle(G *g)
{
    uint32_t gouraud = (uint32_t)R(g, 2);
    uint32_t flags = (uint32_t)R(g, 4);
    uint32_t op = 0x24u | gouraud << 4 | flags;
    int ax, ay, i;
    uint32_t page = page_field(g) & ~0x3u, clut = clut_field(g), c0 = colour(g);
    int u0 = R(g, 256);
    int both = R(g, 2);
    int sx = RR(g, 1, 60);
    int sy = RR(g, 1, 60);
    int px[3], py[3], u[3], v[3];

    anchor(g, &ax, &ay);
    for (i = 0; i < 3; ++i) {
        int dx = RR(g, 0, sx);
        int dy = RR(g, 0, sy);
        int vv = R(g, 256);
        int uu = R(g, 256);
        px[i] = ax + dx;
        py[i] = ay + dy;
        v[i] = vv;
        u[i] = both ? uu : u0;
    }
    if (g->o->avoid_unmodelled)
        cache_guard(g, page);
    put(g, op << 24 | c0);
    for (i = 0; i < 3; ++i) {
        uint32_t hi = i == 0 ? clut : i == 1 ? page : 0;
        if ((op & 0x10) && i > 0)
            put(g, colour(g));
        put(g, vertex_word(g, px[i], py[i]));
        put(g, (uint32_t)u[i] | (uint32_t)v[i] << 8 | hi << 16);
    }
    emit(g);
}

static void gen_rect(G *g)
{
    uint32_t op = 0x60u | (uint32_t)R(g, 32);
    int ax, ay, size = (int)((op >> 3) & 3);
    uint32_t c0 = colour(g);
    int u = texcoord(g);
    int v = texcoord(g);
    uint32_t clut = clut_field(g);

    if (g->o->focus == FD_FOCUS_XFLIP)
        op |= 0x04;
    anchor(g, &ax, &ay);
    if ((op & 0x04) && g->o->avoid_unmodelled)
        put(g, 0x01000000u), emit(g); /* the page's depth is E1h's; guard anyway */
    put(g, op << 24 | c0);
    put(g, vertex_word(g, ax, ay));
    if (op & 0x04)
        put(g, (uint32_t)u | (uint32_t)v << 8 | clut << 16);
    if (size == 0) {
        int w = spread(g, 1023);
        int h = spread(g, 511);
        int k = R(g, 16);
        uint32_t junk = k == 0 ? (R32(g) & 0xfc00fe00u) : 0;
        put(g, (xy16(w, h) & 0x01ff03ffu) | junk);
    }
    emit(g);
}

/* A line's other end: anything, or along one of the eight octants with an
   exact half in its stepping (an even major length and an odd minor one). */
static void line_end(G *g, int ax, int ay, int *bx, int *by, int ties)
{
    if (ties || R(g, 4) == 0) {
        int major = 2 * RR(g, 1, ties ? 40 : 200);
        int minor = 2 * RR(g, 0, major / 2 - 1) + 1;
        int sx = R(g, 2) ? 1 : -1;
        int sy = R(g, 2) ? 1 : -1;
        int ymajor = R(g, 2);
        if (minor > major)
            minor = major - 1;
        *bx = ax + sx * (ymajor ? minor : major);
        *by = ay + sy * (ymajor ? major : minor);
    } else {
        int sx = spread(g, 1023);
        int sy = spread(g, 511);
        int dx = RR(g, -sx, sx);
        int dy = RR(g, -sy, sy);
        *bx = ax + dx;
        *by = ay + dy;
    }
}

static void gen_line(G *g, int poly, int force_gouraud)
{
    uint32_t op = 0x40u | (uint32_t)R(g, 32);
    int ax, ay, bx, by, i, verts;
    int ties = g->o->focus == FD_FOCUS_LINETIES;
    uint32_t c0 = colour(g);

    op = poly ? (op | 0x08) : (op & ~0x08u);
    if (force_gouraud)
        op |= 0x10;
    if (ties)
        op &= ~0x10u;
    anchor(g, &ax, &ay);
    verts = poly ? RR(g, 2, 6) : 2;
    put(g, op << 24 | c0);
    put(g, vertex_word(g, ax, ay) & 0x0fff0fffu);
    for (i = 1; i < verts; ++i) {
        line_end(g, ax, ay, &bx, &by, ties);
        if (op & 0x10) {
            uint32_t c = colour(g);
            if (force_gouraud) { /* steps down as often as up */
                uint32_t prev = g->w[g->n - 2] & 0xffffffu; /* the last colour */
                if (R(g, 2))
                    c = ~prev & 0xffffffu;
            }
            put(g, c & 0x0fffffffu);
        }
        put(g, vertex_word(g, bx, by) & 0x0fff0fffu);
        ax = bx, ay = by;
    }
    if (poly)
        put(g, 0x55555555u);
    emit(g);
}

/* ------------------------------------------------------------------------ */
/* Transfers */

static void gen_fill(G *g)
{
    int k = R(g, 4);
    int x = k == 0 ? R(g, 1024) : RR(g, g->ax0 - 20, g->ax0 + 20) & 1023;
    int y = k == 0 ? R(g, 512) : RR(g, g->ay0 - 20, g->ay0 + 20) & 511;
    int w = spread(g, 1023);
    int h = spread(g, 511);
    uint32_t c = colour(g);

    put(g, 0x02000000u | c);
    put(g, xy16(x, y));
    put(g, xy16(w, h));
    emit(g);
}

static int xfer_size(G *g, int max)
{
    int k = R(g, 16);

    if (k == 0)
        return 0; /* the maximum */
    if (k == 1)
        return RR(g, max - 2, max);
    return RR(g, 1, 40);
}

static void gen_copy(G *g)
{
    int sx = R(g, 1024);
    int sy = R(g, 512);
    int k = R(g, 4);
    int dx = k ? (sx + RR(g, -8, 8)) & 1023 : R(g, 1024);
    int dy = k ? (sy + RR(g, -8, 8)) & 511 : R(g, 512);
    int w = xfer_size(g, 1024);
    int h = xfer_size(g, 512);

    if (w == 0 || h == 0 || w > 200 || h > 200) { /* keep most copies small */
        int big = R(g, 4) == 0;
        if (!big) {
            w = w == 0 || w > 200 ? RR(g, 1, 64) : w;
            h = h == 0 || h > 200 ? RR(g, 1, 64) : h;
        }
    }
    put(g, 0x80000000u | (R32(g) & 0xffffffu));
    put(g, xy16(sx, sy) | (R(g, 8) == 0 ? 0xfc00fe00u : 0));
    put(g, xy16(dx, dy));
    put(g, xy16(w, h));
    emit(g);
}

static uint16_t pixel(G *g, int style)
{
    uint32_t v = R32(g);

    switch (style) {
    case 0:
        return (uint16_t)v;
    case 1: /* many zeros (transparent texels) */
        return (v & 3) == 0 ? 0 : (uint16_t)(v >> 16);
    case 2: /* small indices for 4- and 8-bit textures */
        return (uint16_t)(v & 0x3333u);
    default: /* mask bits set */
        return (uint16_t)(v | 0x8000u);
    }
}

static void gen_load(G *g)
{
    int x = R(g, 1024);
    int y = R(g, 512);
    int k = R(g, 8);
    int w = RR(g, 1, 33);
    int h = RR(g, 1, 17);
    int style = R(g, 4);
    int n, i;

    if (k == 0) { /* across the right or bottom edge */
        x = RR(g, 1000, 1023);
        y = RR(g, 500, 511);
    }
    put(g, 0xa0000000u | (R32(g) & 0xffffffu));
    put(g, xy16(x, y) | (R(g, 8) == 0 ? 0xfc00fe00u : 0));
    put(g, xy16(w, h));
    n = (w * h + 1) / 2;
    for (i = 0; i < n; ++i) {
        uint16_t lo = pixel(g, style);
        uint16_t hi = pixel(g, style);
        put(g, (uint32_t)lo | (uint32_t)hi << 16);
    }
    emit(g);
}

static void gen_read(G *g)
{
    int x = R(g, 1024);
    int y = R(g, 512);
    int w = RR(g, 1, 40);
    int h = RR(g, 1, 12);
    fd_cmd *c;

    if (R(g, 4) == 0) {
        x = RR(g, 1000, 1023);
        y = RR(g, 505, 511);
    }
    put(g, 0xc0000000u | (R32(g) & 0xffffffu));
    put(g, xy16(x, y));
    put(g, xy16(w, h));
    c = emit(g);
    c->reads = (w * h + 1) / 2;
}

/* The background: every pixel of VRAM, in 32 loads of 128 x 128. */
void fd_gen_background(fd_rng *r, fd_stream *s)
{
    G g;

    memset(&g, 0, sizeof g);
    g.r = *r;
    g.s = s;
    gen_background(&g);
    *r = g.r;
}

static void gen_background(G *g)
{
    int bx, by, i;

    for (by = 0; by < 4; ++by) {
        for (bx = 0; bx < 8; ++bx) {
            int style = R(g, 4);
            fd_cmd *c;
            uint32_t *w = must(malloc((3 + 128 * 64) * 4));
            w[0] = 0xa0000000u;
            w[1] = xy16(bx * 128, by * 128);
            w[2] = xy16(128, 128);
            for (i = 0; i < 128 * 64; ++i) {
                uint16_t lo = pixel(g, style);
                uint16_t hi = pixel(g, style);
                w[3 + i] = (uint32_t)lo | (uint32_t)hi << 16;
            }
            c = fd_stream_add(g->s, w, 3 + 128 * 64);
            c->background = 1;
            free(w);
        }
    }
}

/* ------------------------------------------------------------------------ */

static void gen_one(G *g)
{
    int k = R(g, 100);

    switch (g->o->focus) {
    case FD_FOCUS_XFLIP:
        if (k < 60)
            gen_rect(g);
        else if (k < 75)
            gen_e1(g);
        else if (k < 85)
            gen_e2(g);
        else if (k < 92)
            gen_area(g);
        else
            gen_mask(g);
        return;
    case FD_FOCUS_LINETIES:
    case FD_FOCUS_LINEG:
        if (k < 70)
            gen_line(g, k < 20, g->o->focus == FD_FOCUS_LINEG);
        else if (k < 80) {
            /* dithering off: psxstack does not dither lines (divergences.c) */
            uint32_t page = page_field(g) & 0x1ffu;
            put(g, 0xe1000000u | page), emit(g);
        } else if (k < 90)
            gen_area(g);
        else
            gen_mask(g);
        return;
    case FD_FOCUS_QUAD:
        if (k < 70)
            gen_polygon(g, 1, 0x10, 0);
        else if (k < 80)
            gen_e1(g);
        else if (k < 90)
            gen_area(g);
        else
            gen_mask(g);
        return;
    case FD_FOCUS_V:
        if (k < 70)
            gen_v_triangle(g);
        else if (k < 80)
            gen_e1(g);
        else if (k < 88)
            gen_e2(g);
        else if (k < 94)
            gen_area(g);
        else
            gen_mask(g);
        return;
    default:
        break;
    }
    if (k < 30)
        gen_polygon(g, 0, 0, 0);
    else if (k < 45)
        gen_rect(g);
    else if (k < 55)
        gen_line(g, 0, 0);
    else if (k < 60)
        gen_line(g, 1, 0);
    else if (k < 64)
        gen_fill(g);
    else if (k < 68)
        gen_copy(g);
    else if (k < 72)
        gen_load(g);
    else if (k < 75)
        gen_read(g);
    else if (k < 83)
        gen_e1(g);
    else if (k < 86)
        gen_e2(g);
    else if (k < 90)
        gen_area(g);
    else if (k < 92)
        gen_offset(g);
    else if (k < 97)
        gen_mask(g);
    else if (k < 98) {
        put(g, 0x00000000u | (R32(g) & 0xffffffu));
        emit(g);
    } else {
        put(g, 0x01000000u);
        emit(g);
    }
}

void fd_generate(uint64_t seed, uint64_t index, const fd_gen_options *o, fd_stream *s)
{
    G g;
    int i;

    memset(&g, 0, sizeof g);
    memset(s, 0, sizeof *s);
    fd_rng_stream(&g.r, seed, index);
    g.o = o;
    g.s = s;
    gen_background(&g);
    gen_e1(&g);
    gen_e2(&g);
    gen_area(&g);
    if (R(&g, 2))
        gen_offset(&g);
    gen_mask(&g);
    for (i = 0; i < o->commands; ++i)
        gen_one(&g);
}

/*
 * The generators of the checked subsets (docs/testing.md, "Three modes";
 * fd.h).
 *
 * A stream is the background (gen.c), then groups. A group is one
 * primitive or transfer and everything it relies on, sent right before
 * it: the six drawing-state words (E1h with no flip bits), and for the
 * bounded textured primitives the loads that make their texels constant
 * and a GP0(01h). Every group is inside its subset whatever came before
 * it, so that shrinking (main.c) may drop any group but the failing one
 * and stay inside the subset; no word of a group is ever changed.
 *
 * Strict groups are drawn so that no rule on which jitterbug and psxstack
 * are known to differ can change a pixel (docs/testing.md, "Three modes",
 * strict, lists those differences); bounded groups so that only the
 * interpolation rules can,
 * by at most one 5-bit step per channel.
 *
 * Every random number is drawn in a statement of its own.
 */
#include "fd.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    fd_rng r;
    fd_stream *s;
    int group;
    /* The state the group sends (state_emit). */
    uint32_t page;   /* E1h bits 0-8 */
    int dither, dfe, b11;
    uint32_t e2;
    int ax0, ay0, ax1, ay1; /* E3h, E4h as sent (x1 < x0 possible) */
    int ox, oy;
    int mask;        /* E6h bits 0-1 */
    /* The words of the group's primitive, built before anything is sent. */
    uint32_t w[1100];
    int n;
} C;

static int R(C *c, int n)
{
    return fd_below(&c->r, n);
}

static int RR(C *c, int lo, int hi)
{
    return fd_range(&c->r, lo, hi);
}

static uint32_t R32(C *c)
{
    return fd_r32(&c->r);
}

static uint32_t xy16(int x, int y)
{
    return ((uint32_t)x & 0xffffu) | ((uint32_t)y & 0xffffu) << 16;
}

static int imin(int a, int b)
{
    return a < b ? a : b;
}

static int imax(int a, int b)
{
    return a > b ? a : b;
}

static int iabs(int a)
{
    return a < 0 ? -a : a;
}

static void emit_words(C *c, const uint32_t *w, int n, int bounded, int reads)
{
    fd_cmd *cm = fd_stream_add(c->s, w, n);
    cm->group = c->group;
    cm->bounded = bounded;
    cm->reads = reads;
}

static void emit1(C *c, uint32_t w)
{
    emit_words(c, &w, 1, 0, 0);
}

/* ------------------------------------------------------------------------ */
/* The drawing state */

static void state_random(C *c)
{
    int k = R(c, 16);
    int x0, y0, x1, y1;
    int ko = R(c, 8);
    int kw = R(c, 4);

    if (k == 0) {
        x0 = 0, y0 = 0, x1 = 1023, y1 = 511;
    } else if (k == 1) { /* one pixel, or inverted */
        x0 = R(c, 1024);
        y0 = R(c, 512);
        x1 = x0 + RR(c, -2, 0);
        y1 = y0 + RR(c, -2, 0);
    } else if (k == 2) { /* against the far edges */
        x0 = RR(c, 900, 1023);
        y0 = RR(c, 400, 511);
        x1 = 1023;
        y1 = 511;
    } else {
        x0 = R(c, 1000);
        y0 = R(c, 500);
        x1 = x0 + RR(c, 8, 220);
        y1 = y0 + RR(c, 8, 140);
    }
    c->ax0 = imin(imax(x0, 0), 1023);
    c->ay0 = imin(imax(y0, 0), 511);
    c->ax1 = imin(imax(x1, 0), 1023);
    c->ay1 = imin(imax(y1, 0), 511);
    if (ko == 0) {
        c->ox = RR(c, -1024, 1023);
        c->oy = RR(c, -1024, 1023);
    } else if (ko < 3) {
        c->ox = c->ax0;
        c->oy = c->ay0;
    } else if (ko < 5) {
        c->ox = 0;
        c->oy = 0;
    } else {
        c->ox = RR(c, -32, 32);
        c->oy = RR(c, -32, 32);
    }
    c->page = (uint32_t)R(c, 512);
    c->dither = R(c, 2);
    c->dfe = R(c, 2);
    c->b11 = R(c, 8) == 0;
    c->e2 = kw == 0 ? (R32(c) & 0xfffffu) : 0;
    c->mask = R(c, 4);
}

static void state_emit(C *c)
{
    emit1(c, 0xe1000000u | (c->page & 0x1ffu) | (uint32_t)c->dither << 9 |
                 (uint32_t)c->dfe << 10 | (uint32_t)c->b11 << 11);
    emit1(c, 0xe2000000u | c->e2);
    emit1(c, 0xe3000000u | (uint32_t)c->ax0 | (uint32_t)c->ay0 << 10);
    emit1(c, 0xe4000000u | (uint32_t)c->ax1 | (uint32_t)c->ay1 << 10);
    emit1(c, 0xe5000000u | ((uint32_t)c->ox & 0x7ffu) | ((uint32_t)c->oy & 0x7ffu) << 11);
    emit1(c, 0xe6000000u | (uint32_t)c->mask);
}

/* The vertex word of screen position (x, y): both the position and its
   word (the position less the offset) in -1024..1023, so that neither
   side's rule for an out-of-range sum (offset-wrap) can apply. 0: not
   representable. Bits 11-15 of each half sometimes hold junk, which both
   sides ignore; never for lines (a polyline's terminator is recognised by
   bits 12-15). */
static int vertex(C *c, int x, int y, int junk, uint32_t *out)
{
    int wx = x - c->ox, wy = y - c->oy;

    if (x < -1024 || x > 1023 || y < -1024 || y > 1023 || wx < -1024 || wx > 1023 ||
        wy < -1024 || wy > 1023)
        return 0;
    *out = xy16(wx, wy) & 0x07ff07ffu;
    if (junk) {
        int k = R(c, 16);
        if (k == 0) {
            uint32_t j = R32(c) & 0xf800f800u;
            *out |= j;
        }
    }
    return 1;
}

/* A point of interest (gen.c's anchor). */
static void anchor(C *c, int *x, int *y)
{
    int k = R(c, 16);
    int cx = (c->ax0 + c->ax1) / 2, cy = (c->ay0 + c->ay1) / 2;

    if (k < 7) {
        int dx = RR(c, -40, 40);
        int dy = RR(c, -40, 40);
        *x = cx + dx;
        *y = cy + dy;
    } else if (k < 12) {
        int side = R(c, 4);
        int d = RR(c, -3, 3);
        int along_x = RR(c, c->ax0 - 4, c->ax1 + 4);
        int along_y = RR(c, c->ay0 - 4, c->ay1 + 4);
        *x = side == 0 ? c->ax0 + d : side == 1 ? c->ax1 + d : along_x;
        *y = side == 2 ? c->ay0 + d : side == 3 ? c->ay1 + d : along_y;
    } else if (k < 14) {
        int a = R(c, 2);
        int ex = a ? RR(c, -3, 3) : RR(c, 1020, 1027);
        int b = R(c, 2);
        int ey = b ? RR(c, -3, 3) : RR(c, 508, 515);
        *x = ex;
        *y = ey;
    } else {
        int fx = RR(c, -1024, 1023);
        int fy = RR(c, -1024, 1023);
        *x = fx;
        *y = fy;
    }
}

static int spread(C *c, int limit)
{
    int k = R(c, 16);

    if (k < 4)
        return RR(c, 0, 3);
    if (k < 9)
        return RR(c, 0, 24);
    if (k < 13)
        return RR(c, 0, 160);
    if (k < 15)
        return RR(c, 0, limit + 2);
    return limit + RR(c, -1, 1);
}

static uint32_t colour(C *c)
{
    int k = R(c, 8);
    uint32_t v;

    if (k == 0)
        return 0x808080u;
    if (k == 1) {
        v = R(c, 2) ? 0xffffffu : 0;
        return v;
    }
    v = R32(c) & 0xffffffu;
    return v;
}

/* A colour whose channels are at most 128. */
static uint32_t colour128(C *c)
{
    int k = R(c, 4);
    int r, g, b;

    if (k == 0)
        return 0x808080u;
    r = R(c, 129);
    g = R(c, 129);
    b = R(c, 129);
    return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16;
}

/* ------------------------------------------------------------------------ */
/* Textures: what a textured primitive may read, and whether it draws there */

static int depth_of(uint32_t page)
{
    int d = (int)((page >> 7) & 3);
    return d == 0 ? 4 : d == 1 ? 8 : 15;
}

/* Whether [a0, a1] x [b0, b1] (x wrapping at 1024: a1 may be past 1023)
   meets [x0, x1] x [y0, y1] (inside VRAM). */
static int meets_wrapped(int a0, int a1, int b0, int b1, int x0, int x1, int y0, int y1)
{
    if (b1 < y0 || b0 > y1)
        return 0;
    if (a0 > 1023)
        a0 -= 1024, a1 -= 1024;
    if (a0 <= x1 && a1 >= x0)
        return 1;
    return a1 > 1023 && x0 <= a1 - 1024;
}

/* The pixels a primitive with bounds [x0, x1] x [y0, y1] can draw: the
   bounds within the drawing area and VRAM. 0: none. */
static int drawn(const C *c, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = imax(imax(*x0, c->ax0), 0);
    *y0 = imax(imax(*y0, c->ay0), 0);
    *x1 = imin(imin(*x1, c->ax1), 1023);
    *y1 = imin(imin(*y1, c->ay1), 511);
    return *x0 <= *x1 && *y0 <= *y1;
}

/* Whether a primitive drawing in bounds [x0, x1] x [y0, y1] can write a
   pixel its texture page (page) or CLUT (clut, the texture word's bits
   16-31) may read: the whole page, all 256 rows of it, and all the CLUT's
   entries. */
static int over_texture(const C *c, uint32_t page, uint32_t clut, int x0, int y0, int x1, int y1)
{
    int depth = depth_of(page);
    int tpx = (int)(page & 15) * 64, tpy = (int)((page >> 4) & 1) * 256;
    int span = depth == 4 ? 64 : depth == 8 ? 128 : 256;

    if (!drawn(c, &x0, &y0, &x1, &y1))
        return 0;
    if (meets_wrapped(tpx, tpx + span - 1, tpy, tpy + 255, x0, x1, y0, y1))
        return 1;
    if (depth != 15) {
        int cx = (int)(clut & 0x3f) * 16, cy = (int)((clut >> 6) & 0x1ff);
        int n = depth == 4 ? 16 : 256;
        if (meets_wrapped(cx, cx + n - 1, cy, cy, x0, x1, y0, y1))
            return 1;
    }
    return 0;
}

static uint32_t clut_field(C *c)
{
    int k = R(c, 4);
    int cx = k == 0 ? RR(c, 48, 63) : R(c, 64);
    int cy = R(c, 512);

    return (uint32_t)cx | (uint32_t)cy << 6;
}

/* A texture page attribute (bits 0-8, and sometimes 11, which both sides
   store and ignore without GP1(09h)): blend mode abr. */
static uint32_t page_field(C *c, int abr)
{
    uint32_t px = (uint32_t)R(c, 16);
    uint32_t py = (uint32_t)R(c, 2);
    int k = R(c, 16);
    uint32_t depth = (uint32_t)(k == 0 ? 3 : R(c, 3));
    uint32_t b11 = (uint32_t)(R(c, 8) == 0);

    return px | py << 4 | (uint32_t)abr << 5 | depth << 7 | b11 << 11;
}

/* ------------------------------------------------------------------------ */
/* Exact attribute planes */

/*
 * Values of an attribute at nv vertices given as grid offsets (kx, ky)
 * from vertex 0 on a grid of 2^s pixels, on the plane a0 + (px * dx +
 * py * dy) / 2^s: integers in 0..255, and every gradient a multiple of
 * 2^-s, s <= 4, hence exact with 12 fractional bits. one: the gradients
 * forced to (1, 0) or (0, 1) times sign (a 1:1 texture mapping), when they
 * fit.
 */
static void plane_values(C *c, int nv, const int *kx, const int *ky, int s, int one, int axis,
                         int *val)
{
    int lim = 2 << s, tries, i;

    for (tries = 0; tries < 6; ++tries, lim /= 2) {
        int px, py, lo = 1 << 30, hi = -(1 << 30), a0;
        if (one) {
            int sign = R(c, 2) ? 1 : -1;
            px = axis == 0 ? sign * (1 << s) : 0; /* never a shift of a negative value */
            py = axis == 1 ? sign * (1 << s) : 0;
        } else {
            px = RR(c, -lim, lim);
            py = RR(c, -lim, lim);
        }
        for (i = 0; i < nv; ++i) {
            int t = px * kx[i] + py * ky[i]; /* (px dx + py dy) / 2^s */
            lo = imin(lo, t);
            hi = imax(hi, t);
        }
        if (hi - lo > 255) {
            one = 0;
            continue;
        }
        a0 = RR(c, -lo, 255 - hi);
        for (i = 0; i < nv; ++i)
            val[i] = a0 + px * kx[i] + py * ky[i];
        return;
    }
    {
        int a0 = R(c, 256);
        for (i = 0; i < nv; ++i)
            val[i] = a0;
    }
}

static long gcd_ext(long a, long b, long *x, long *y)
{
    long x0 = 1, y0 = 0, x1 = 0, y1 = 1;

    while (b != 0) {
        long q = a / b, t;
        t = a - q * b, a = b, b = t;
        t = x0 - q * x1, x0 = x1, x1 = t;
        t = y0 - q * y1, y0 = y1, y1 = t;
    }
    *x = x0;
    *y = y0;
    return a;
}

/*
 * A triangle v0, v0 + (ex, ey), v0 + (fx, fy) whose doubled signed area
 * ex fy - ey fx is +-2^k, k <= 12: every attribute plane over it has
 * gradients that are multiples of 2^-12, whatever the vertex values.
 * 0: (ex, ey) admits none nearby.
 */
static int pow2_triangle(C *c, int ex, int ey, int *fx, int *fy)
{
    long x, y, g, t, a, b, best = -1;
    int k = R(c, 13);
    int sign = R(c, 2) ? 1 : -1;
    long target = sign * (1L << k);

    if (ex == 0 && ey == 0)
        return 0;
    g = gcd_ext(ex, ey, &x, &y);
    if (g < 0)
        g = -g, x = -x, y = -y;
    if (target % g != 0)
        return 0;
    /* ex * x + ey * y = g: (f) = (-y, x) * target / g + t * (ex, ey) / g */
    a = -y * (target / g);
    b = x * (target / g);
    /* the t that brings f nearest the origin, then a little either way */
    if ((long)ex * ex + (long)ey * ey > 0)
        best = -(a * ex + b * ey) * g / ((long)ex * ex + (long)ey * ey);
    t = best + RR(c, -2, 2);
    *fx = (int)(a + t * ex / g);
    *fy = (int)(b + t * ey / g);
    return iabs(*fx) < 2048 && iabs(*fy) < 2048;
}

/* ------------------------------------------------------------------------ */
/* Polygons */

typedef struct {
    int nv, x[4], y[4];
    int r[4], g[4], b[4], u[4], v[4];
} polygon;

/* Vertex positions: on a grid of 2^s from an anchor (any shape, also
   degenerate or at the size limits), or a power-of-two triangle. */
static void poly_shape(C *c, polygon *p, int nv, int *s, int *kx, int *ky, int *pow2)
{
    int ax, ay, i, gstep, sx, sy;
    int shape = R(c, 8);

    anchor(c, &ax, &ay);
    *s = R(c, 5);
    gstep = 1 << *s;
    sx = spread(c, 1023) / gstep + 1;
    sy = spread(c, 511) / gstep + 1;
    p->nv = nv;
    *pow2 = 0;
    kx[0] = ky[0] = 0;
    for (i = 1; i < nv; ++i) {
        kx[i] = RR(c, -sx, sx);
        ky[i] = RR(c, -sy, sy);
    }
    if (shape == 0 && nv == 3) { /* a repeated vertex or three in a line */
        int k = R(c, 2);
        if (k)
            kx[2] = kx[1], ky[2] = ky[1];
        else
            kx[2] = 2 * kx[1], ky[2] = 2 * ky[1];
    } else if (shape == 1 && nv == 4) { /* axis-aligned */
        int w = RR(c, 1, 64 / gstep + 1);
        int h = RR(c, 1, 64 / gstep + 1);
        kx[1] = w, ky[1] = 0;
        kx[2] = 0, ky[2] = h;
        kx[3] = w, ky[3] = h;
    } else if (shape == 2 && nv == 3) {
        int fx, fy, ex = RR(c, -60, 60), ey = RR(c, -60, 60);
        if (pow2_triangle(c, ex, ey, &fx, &fy)) {
            *s = 0;
            kx[1] = ex, ky[1] = ey, kx[2] = fx, ky[2] = fy;
            *pow2 = 1;
        }
    }
    for (i = 0; i < nv; ++i) {
        p->x[i] = ax + kx[i] * (1 << *s); /* kx may be negative: no shift */
        p->y[i] = ay + ky[i] * (1 << *s);
    }
}

static long cross(const polygon *p, int a, int b, int q)
{
    return (long)(p->x[b] - p->x[a]) * (p->y[q] - p->y[a]) -
           (long)(p->x[q] - p->x[a]) * (p->y[b] - p->y[a]);
}

/* A quad's triangles v0 v1 v2 and v1 v2 v3 overlap: v0 and v3 strictly on
   the same side of v1 v2. */
static int quad_overlaps(const polygon *p)
{
    long c0 = cross(p, 1, 2, 0), c3 = cross(p, 1, 2, 3);
    return (c0 > 0 && c3 > 0) || (c0 < 0 && c3 < 0);
}

static void bounds(const polygon *p, int *x0, int *y0, int *x1, int *y1)
{
    int i;

    *x0 = *x1 = p->x[0];
    *y0 = *y1 = p->y[0];
    for (i = 1; i < p->nv; ++i) {
        *x0 = imin(*x0, p->x[i]);
        *x1 = imax(*x1, p->x[i]);
        *y0 = imin(*y0, p->y[i]);
        *y1 = imax(*y1, p->y[i]);
    }
}

/* The polygon's words into c->w (op with its flags; clut and page in the
   first two texture words). 0: a vertex not representable. */
static int poly_words(C *c, const polygon *p, uint32_t op, uint32_t clut, uint32_t page)
{
    int i, gouraud = (op & 0x10) != 0, textured = (op & 0x04) != 0;

    c->n = 0;
    c->w[c->n++] = op << 24 | (uint32_t)p->r[0] | (uint32_t)p->g[0] << 8 | (uint32_t)p->b[0] << 16;
    for (i = 0; i < p->nv; ++i) {
        uint32_t vw;
        if (gouraud && i > 0)
            c->w[c->n++] = (uint32_t)p->r[i] | (uint32_t)p->g[i] << 8 | (uint32_t)p->b[i] << 16;
        if (!vertex(c, p->x[i], p->y[i], 1, &vw))
            return 0;
        c->w[c->n++] = vw;
        if (textured) {
            uint32_t hi = i == 0 ? clut : i == 1 ? page : (R32(c) & 0xffffu);
            c->w[c->n++] = (uint32_t)p->u[i] | (uint32_t)p->v[i] << 8 | hi << 16;
        }
    }
    return 1;
}

static void set_colour(polygon *p, int i, uint32_t rgb)
{
    p->r[i] = (int)(rgb & 0xff);
    p->g[i] = (int)((rgb >> 8) & 0xff);
    p->b[i] = (int)((rgb >> 16) & 0xff);
}

/* Strict: a flat untextured polygon, any shape (no attribute is
   interpolated; a quad's triangles have the same colour, so their order
   cannot show). Any blend, mask setting, dithering (neither side dithers
   it). */
static int strict_flat_poly(C *c)
{
    polygon p;
    int s, kx[4], ky[4], pow2, nv = R(c, 2) ? 4 : 3, semi = R(c, 2);
    uint32_t op = 0x20u | (nv == 4 ? 0x08u : 0) | (semi ? 0x02u : 0) | (uint32_t)R(c, 2);

    poly_shape(c, &p, nv, &s, kx, ky, &pow2);
    set_colour(&p, 0, colour(c));
    c->page = (c->page & ~0x60u) | (uint32_t)R(c, 4) << 5;
    return poly_words(c, &p, op & ~0x04u, 0, 0);
}

/* Strict: a Gouraud untextured polygon whose colour planes have
   gradients exact in 12 fractional bits (both sides then compute the
   plane exactly and round it the same way). A quad's two triangles lie on
   one plane, so their order cannot show. Dithering only when opaque (the
   order of dithering and blending is open). */
static int strict_gouraud_poly(C *c)
{
    polygon p;
    int s, kx[4], ky[4], pow2, nv = R(c, 3) ? 3 : 4, semi = R(c, 2), i;
    int vr[4], vg[4], vb[4];
    uint32_t op = 0x30u | (nv == 4 ? 0x08u : 0) | (semi ? 0x02u : 0);

    poly_shape(c, &p, nv, &s, kx, ky, &pow2);
    if (pow2) {
        for (i = 0; i < nv; ++i)
            set_colour(&p, i, colour(c));
    } else {
        plane_values(c, nv, kx, ky, s, 0, 0, vr);
        plane_values(c, nv, kx, ky, s, 0, 0, vg);
        plane_values(c, nv, kx, ky, s, 0, 0, vb);
        for (i = 0; i < nv; ++i)
            p.r[i] = vr[i], p.g[i] = vg[i], p.b[i] = vb[i];
    }
    c->page = (c->page & ~0x60u) | (uint32_t)R(c, 4) << 5;
    if (semi)
        c->dither = 0;
    return poly_words(c, &p, op, 0, 0);
}

/*
 * The colour rule for a modulated texel (docs/testing.md, "Three modes", strict):
 * opaque, or blend modes 1 and 2, any colour; mode 0 channels at most 128
 * (the modulated texel cannot pass 31, where the sides saturate at
 * different points); mode 3 channels at most 128 with the shading bit,
 * where psxstack quarters after modulating as jitterbug does, and exactly
 * 128 without it, where psxstack quarters the texel before modulating.
 */
static uint32_t modulated_colour(C *c, int semi, int abr, int shading_bit)
{
    if (!semi || abr == 1 || abr == 2)
        return colour(c);
    if (abr == 0 || shading_bit)
        return colour128(c);
    return 0x808080u;
}

/* Strict: a textured polygon whose texture coordinate planes are exact
   (as the colours above), flat or Gouraud with all vertex colours equal,
   raw or modulated. A quad's triangles lie on one plane of u and of v and
   have one colour, so their order cannot show. Dithering: flat modulated
   ones off (psxstack does not dither them), raw ones with the shading bit
   off (open question), modulated ones with the shading bit only when
   opaque; flat raw ones any (neither side dithers them). Not drawn over
   its texture page or CLUT; GP0(01h) before it (the CLUT cache). */
static int strict_textured_poly(C *c, int *guard)
{
    polygon p;
    int s, kx[4], ky[4], pow2, nv = R(c, 3) ? 3 : 4, i, tries;
    int semi = R(c, 2), raw = R(c, 2), shading = R(c, 3) == 0, abr = R(c, 4);
    int one = R(c, 3) == 0, axis = R(c, 2);
    int vu[4], vv[4], x0, y0, x1, y1;
    uint32_t op = 0x24u | (nv == 4 ? 0x08u : 0) | (shading ? 0x10u : 0) | (semi ? 0x02u : 0) |
                  (raw ? 0x01u : 0);
    uint32_t page = 0, clut = 0, rgb;

    poly_shape(c, &p, nv, &s, kx, ky, &pow2);
    if (pow2) {
        for (i = 0; i < nv; ++i) {
            vu[i] = R(c, 256);
            vv[i] = R(c, 256);
        }
    } else {
        plane_values(c, nv, kx, ky, s, one, axis, vu);
        plane_values(c, nv, kx, ky, s, one, !axis, vv);
    }
    for (i = 0; i < nv; ++i)
        p.u[i] = vu[i], p.v[i] = vv[i];
    bounds(&p, &x0, &y0, &x1, &y1);
    for (tries = 0; tries < 8; ++tries) {
        page = page_field(c, abr);
        clut = clut_field(c);
        if (!over_texture(c, page, clut, x0, y0, x1, y1))
            break;
    }
    if (tries == 8)
        return 0;
    rgb = raw ? (R32(c) & 0xffffffu) : modulated_colour(c, semi, abr, shading);
    for (i = 0; i < nv; ++i)
        set_colour(&p, i, rgb);
    if ((!shading && !raw) || (shading && raw) || (shading && semi))
        c->dither = 0;
    *guard = depth_of(page) != 15;
    return poly_words(c, &p, op, clut, page);
}

/* ------------------------------------------------------------------------ */
/* Rectangles: no interpolation (the texture coordinate steps by one texel
   a pixel on both sides), never dithered on either side. No flips. */

static int strict_rect(C *c, int *guard)
{
    uint32_t op = 0x60u | (uint32_t)R(c, 32);
    int size = (int)((op >> 3) & 3), textured = (op & 0x04) != 0, semi = (op & 0x02) != 0;
    int raw = (op & 0x01) != 0, ax, ay, w, h, tries, u, v;
    int abr = R(c, 4);
    uint32_t vw, clut = 0, rgb;

    anchor(c, &ax, &ay);
    if (size == 0) {
        w = spread(c, 1023) & 0x3ff;
        h = spread(c, 511) & 0x1ff;
    } else {
        w = h = size == 1 ? 1 : size == 2 ? 8 : 16;
    }
    c->page = (c->page & ~0x60u) | (uint32_t)abr << 5;
    if (textured) {
        for (tries = 0; tries < 8; ++tries) {
            uint32_t pg = page_field(c, abr);
            clut = clut_field(c);
            c->page = pg & 0x1ffu;
            if (!over_texture(c, c->page, clut, ax, ay, ax + w - 1, ay + h - 1))
                break;
        }
        if (tries == 8)
            return 0;
        *guard = depth_of(c->page) != 15;
    }
    rgb = textured && !raw ? modulated_colour(c, semi, abr, 0) : colour(c);
    u = R(c, 256);
    v = R(c, 256);
    c->n = 0;
    c->w[c->n++] = op << 24 | rgb;
    if (!vertex(c, ax, ay, 1, &vw))
        return 0;
    c->w[c->n++] = vw;
    if (textured)
        c->w[c->n++] = (uint32_t)u | (uint32_t)v << 8 | clut << 16;
    if (size == 0) {
        /* junk above the width's 10 bits and the height's 9, which both
           sides ignore */
        int k = R(c, 16);
        uint32_t junk = k == 0 ? (R32(c) & 0xfe00fc00u) : 0;
        c->w[c->n++] = xy16(w, h) | junk;
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Lines: flat, or Gouraud with a whole number of colour steps per pixel,
   drawn from the first vertex; horizontal, vertical or at 45 degrees, so
   that no stepping rule can place a pixel differently; never dithered
   (jitterbug dithers every line, psxstack none); a 45-degree segment never
   reaching the drawing area's last column or row, which psxstack leaves
   out of sloped lines. */

/* A segment from (x, y) of length len in direction dir (0-7), and whether
   it is valid: its 45-degree pixels avoid the last column and row. */
static int segment_ok(const C *c, int x, int y, int dx, int dy, int len)
{
    int i;

    if (dx == 0 || dy == 0)
        return 1;
    for (i = 0; i <= len; ++i) {
        int px = x + dx * i, py = y + dy * i;
        if (px >= c->ax0 && px <= c->ax1 && py >= c->ay0 && py <= c->ay1 &&
            (px == c->ax1 || py == c->ay1))
            return 0;
    }
    return 1;
}

static int strict_line(C *c)
{
    static const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1},
                                   {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    int polyline = R(c, 3) == 0, gouraud = R(c, 3) == 0, semi = R(c, 2);
    int verts = polyline ? RR(c, 2, 6) : 2, x, y, i, abr = R(c, 4);
    uint32_t op = 0x40u | (gouraud ? 0x10u : 0) | (polyline ? 0x08u : 0) | (semi ? 0x02u : 0);
    uint32_t rgb = colour(c), vw;
    int cr = (int)(rgb & 0xff), cg = (int)((rgb >> 8) & 0xff), cb = (int)((rgb >> 16) & 0xff);

    c->dither = 0;
    c->page = (c->page & ~0x60u) | (uint32_t)abr << 5;
    anchor(c, &x, &y);
    c->n = 0;
    c->w[c->n++] = op << 24 | rgb;
    if (!vertex(c, x, y, 0, &vw))
        return 0;
    c->w[c->n++] = vw;
    for (i = 1; i < verts; ++i) {
        int tries, d = 0, len = 0, dx = 0, dy = 0;
        for (tries = 0; tries < 16; ++tries) {
            int k = R(c, 8);
            static const int right_or_down[4] = {0, 2, 4, 5};
            int pick = R(c, 4);
            int any = R(c, 8);
            d = gouraud ? right_or_down[pick] : any;
            dx = dirs[d][0];
            dy = dirs[d][1];
            /* Gouraud: drawn from the first vertex on both sides: going right
               (also at 45 degrees), or straight down */
            if (gouraud && !(dx > 0 || (dx == 0 && dy > 0)))
                continue;
            len = k < 3 ? R(c, 4) : k < 6 ? R(c, 64) : k < 7 ? R(c, 300) : (dy ? 511 : 1023) + RR(c, -1, 1);
            if (dx && dy && len > 511)
                len = 511;
            if (segment_ok(c, x, y, dx, dy, len) &&
                vertex(c, x + dx * len, y + dy * len, 0, &vw))
                break;
        }
        if (tries == 16)
            return 0;
        if (gouraud) {
            /* every channel a whole number of steps per pixel */
            int ch[3], k;
            ch[0] = cr, ch[1] = cg, ch[2] = cb;
            for (k = 0; k < 3; ++k) {
                int lo = len ? -ch[k] / len : 0, hi = len ? (255 - ch[k]) / len : 0;
                int step = RR(c, lo, hi);
                ch[k] += step * len;
            }
            cr = ch[0], cg = ch[1], cb = ch[2];
            c->w[c->n++] = (uint32_t)cr | (uint32_t)cg << 8 | (uint32_t)cb << 16;
        }
        x += dx * len;
        y += dy * len;
        c->w[c->n++] = vw;
    }
    if (polyline)
        c->w[c->n++] = 0x55555555u;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Transfers: fills, loads and reads anywhere (wrapping), with the mask
   settings; copies whose source and destination do not overlap, or are the
   same rectangle (each pixel is then read before it is written on either
   side). */

static int xfer_size(C *c, int max)
{
    int k = R(c, 16);

    if (k == 0)
        return 0;
    if (k == 1)
        return RR(c, max - 2, max);
    return RR(c, 1, 40);
}

static int copy_overlaps(int sx, int sy, int dx, int dy, int w, int h)
{
    int offx = ((dx - sx) % 1024 + 1024) % 1024, offy = ((dy - sy) % 512 + 512) % 512;
    return (offx < w || 1024 - offx < w) && (offy < h || 512 - offy < h);
}

static int strict_copy(C *c)
{
    int sx = R(c, 1024);
    int sy = R(c, 512);
    int same = R(c, 8) == 0;
    int w = xfer_size(c, 1024);
    int h = xfer_size(c, 512);
    int tries, dx = sx, dy = sy, ew, eh;
    uint32_t lowjunk = R32(c) & 0xffffffu;
    int jk = R(c, 8);

    if (w == 0 || w > 200)
        w = RR(c, 1, 64);
    if (h == 0 || h > 200)
        h = RR(c, 1, 64);
    ew = ((w - 1) & 0x3ff) + 1;
    eh = ((h - 1) & 0x1ff) + 1;
    if (!same) {
        for (tries = 0; tries < 16; ++tries) {
            int k = R(c, 2);
            dx = k ? (sx + RR(c, -2 * ew, 2 * ew)) & 1023 : R(c, 1024);
            dy = k ? (sy + RR(c, -2 * eh, 2 * eh)) & 511 : R(c, 512);
            if (!copy_overlaps(sx, sy, dx, dy, ew, eh))
                break;
        }
        if (tries == 16)
            dx = sx, dy = sy;
    }
    c->n = 0;
    c->w[c->n++] = 0x80000000u | lowjunk;
    c->w[c->n++] = xy16(sx, sy) | (jk == 0 ? 0xfc00fe00u : 0);
    c->w[c->n++] = xy16(dx, dy);
    c->w[c->n++] = xy16(w, h);
    return 1;
}

static int strict_fill(C *c)
{
    int x = R(c, 1024);
    int y = R(c, 512);
    int w = spread(c, 1023);
    int h = spread(c, 511);
    uint32_t rgb = colour(c);

    c->n = 0;
    c->w[c->n++] = 0x02000000u | rgb;
    c->w[c->n++] = xy16(x, y);
    c->w[c->n++] = xy16(w, h);
    return 1;
}

static int strict_load(C *c)
{
    int k = R(c, 8);
    int x = k == 0 ? RR(c, 1000, 1023) : R(c, 1024);
    int y = k == 0 ? RR(c, 500, 511) : R(c, 512);
    int w = RR(c, 1, 33);
    int h = RR(c, 1, 17);
    int n = (w * h + 1) / 2, i;
    uint32_t lowjunk = R32(c) & 0xffffffu;

    c->n = 0;
    c->w[c->n++] = 0xa0000000u | lowjunk;
    c->w[c->n++] = xy16(x, y);
    c->w[c->n++] = xy16(w, h);
    for (i = 0; i < n; ++i) {
        uint32_t v = R32(c);
        c->w[c->n++] = v;
    }
    return 1;
}

static int strict_read(C *c, int *reads)
{
    int k = R(c, 4);
    int x = k == 0 ? RR(c, 1000, 1023) : R(c, 1024);
    int y = k == 0 ? RR(c, 505, 511) : R(c, 512);
    int w = RR(c, 1, 40);
    int h = RR(c, 1, 12);

    c->n = 0;
    c->w[c->n++] = 0xc0000000u;
    c->w[c->n++] = xy16(x, y);
    c->w[c->n++] = xy16(w, h);
    *reads = (w * h + 1) / 2;
    return 1;
}

/* ------------------------------------------------------------------------ */

void fd_generate_strict(uint64_t seed, uint64_t index, int commands, fd_stream *s)
{
    C c;
    int i;

    memset(&c, 0, sizeof c);
    memset(s, 0, sizeof *s);
    fd_rng_stream(&c.r, seed ^ 0x5157u, index);
    c.s = s;
    fd_gen_background(&c.r, s);
    for (i = 0; i < commands; ++i) {
        int k = R(&c, 100), ok = 0, guard = 0, reads = 0;
        int before = c.s->n;
        c.group = i + 1;
        state_random(&c);
        if (k < 16)
            ok = strict_flat_poly(&c);
        else if (k < 30)
            ok = strict_gouraud_poly(&c);
        else if (k < 48)
            ok = strict_textured_poly(&c, &guard);
        else if (k < 66)
            ok = strict_rect(&c, &guard);
        else if (k < 80)
            ok = strict_line(&c);
        else if (k < 85)
            ok = strict_fill(&c);
        else if (k < 90)
            ok = strict_copy(&c);
        else if (k < 95)
            ok = strict_load(&c);
        else if (k < 98)
            ok = strict_read(&c, &reads);
        else {
            c.n = 0;
            c.w[c.n++] = R(&c, 2) ? 0x01000000u : (R32(&c) & 0xffffffu);
            ok = 1;
        }
        if (!ok)
            continue;
        state_emit(&c);
        if (guard)
            emit1(&c, 0x01000000u);
        emit_words(&c, c.w, c.n, 0, reads);
        (void)before;
    }
}

/* ------------------------------------------------------------------------ */
/* Bounded residual */

/* Bounded: a Gouraud untextured polygon, any shape and colours (a quad's
   triangles not overlapping, where their order would show); opaque with
   dithering on or off, or semi-transparent without. */
static int bounded_gouraud_poly(C *c)
{
    polygon p;
    int s, kx[4], ky[4], pow2, nv = R(c, 3) ? 3 : 4, semi = R(c, 2), i, tries;
    uint32_t op = 0x30u | (nv == 4 ? 0x08u : 0) | (semi ? 0x02u : 0);

    for (tries = 0; tries < 8; ++tries) {
        poly_shape(c, &p, nv, &s, kx, ky, &pow2);
        if (nv == 3 || !quad_overlaps(&p))
            break;
    }
    if (tries == 8)
        return 0;
    for (i = 0; i < nv; ++i)
        set_colour(&p, i, colour(c));
    c->page = (c->page & ~0x60u) | (uint32_t)R(c, 4) << 5;
    if (semi)
        c->dither = 0;
    return poly_words(c, &p, op, 0, 0);
}

/*
 * Bounded: a textured polygon, any shape and texture coordinates, whose
 * every texel is one value T (not 0, bit 15 either way): its CLUT's
 * entries all T (4 and 8 bits, any texture data), or the texture window
 * narrowed to one 8 x 8 block of T (15 bits). Not drawn over its CLUT or
 * that block. Flat, or Gouraud; raw or modulated. A Gouraud modulated one
 * whose colours vary is opaque and dithered, semi-transparent, or mask
 * checked (psxstack takes the colour of such a polygon per pair of pixels
 * otherwise, which no evidence here supports or refutes: docs/conformance.md,
 * "Open questions awaiting hardware"); a quad's triangles then do not
 * overlap. Colours as modulated_colour.
 */
static int bounded_textured_poly(C *c, int *setup_n, uint32_t *setup)
{
    polygon p;
    int s, kx[4], ky[4], pow2, nv = R(c, 3) ? 3 : 4, i, tries;
    int semi = R(c, 2), raw = R(c, 4) == 0, shading = R(c, 2), abr = R(c, 4);
    int vary = shading && !raw && R(c, 4) != 0;
    int x0, y0, x1, y1, depth = 0, n = 0;
    uint32_t op = 0x24u | (nv == 4 ? 0x08u : 0) | (shading ? 0x10u : 0) | (semi ? 0x02u : 0) |
                  (raw ? 0x01u : 0);
    uint32_t page = 0, clut = 0, texel, rgb = 0;
    uint32_t stp = R(c, 2) ? 0x8000u : 0;

    texel = (uint32_t)RR(c, 1, 0x7fff) | stp;
    for (tries = 0; tries < 8; ++tries) {
        poly_shape(c, &p, nv, &s, kx, ky, &pow2);
        if (nv == 3 || !vary || !quad_overlaps(&p))
            break;
    }
    if (tries == 8)
        return 0;
    for (i = 0; i < nv; ++i) {
        p.u[i] = R(c, 256);
        p.v[i] = R(c, 256);
    }
    bounds(&p, &x0, &y0, &x1, &y1);
    for (tries = 0; tries < 8; ++tries) {
        int bx, by;
        page = page_field(c, abr);
        clut = clut_field(c);
        depth = depth_of(page);
        if (depth == 15) {
            /* window mask 31 both ways: u = (u & 7) | offset * 8 */
            int offx = R(c, 32);
            int offy = R(c, 32);
            c->e2 = 31u | 31u << 5 | (uint32_t)offx << 10 | (uint32_t)offy << 15;
            bx = (int)(page & 15) * 64 + offx * 8;
            by = (int)((page >> 4) & 1) * 256 + offy * 8;
            {
                int dx0 = x0, dy0 = y0, dx1 = x1, dy1 = y1;
                if (drawn(c, &dx0, &dy0, &dx1, &dy1) &&
                    meets_wrapped(bx, bx + 7, by, by + 7, dx0, dx1, dy0, dy1))
                    continue;
            }
            n = 0;
            setup[n++] = 0xa0000000u;
            setup[n++] = xy16(bx & 1023, by);
            setup[n++] = xy16(8, 8);
            for (i = 0; i < 32; ++i)
                setup[n++] = texel | texel << 16;
        } else {
            int cx = (int)(clut & 0x3f) * 16, cy = (int)((clut >> 6) & 0x1ff);
            int entries = depth == 4 ? 16 : 256;
            int dx0 = x0, dy0 = y0, dx1 = x1, dy1 = y1;
            if (drawn(c, &dx0, &dy0, &dx1, &dy1) &&
                meets_wrapped(cx, cx + entries - 1, cy, cy, dx0, dx1, dy0, dy1))
                continue;
            n = 0;
            setup[n++] = 0xa0000000u;
            setup[n++] = xy16(cx, cy);
            setup[n++] = xy16(entries, 1);
            for (i = 0; i < entries / 2; ++i)
                setup[n++] = texel | texel << 16;
        }
        break;
    }
    if (tries == 8)
        return 0;
    *setup_n = n;
    if (raw)
        rgb = R32(c) & 0xffffffu;
    else
        rgb = modulated_colour(c, semi, abr, shading);
    for (i = 0; i < nv; ++i) {
        uint32_t ci = rgb;
        if (vary && i > 0)
            ci = semi && abr != 1 && abr != 2 ? colour128(c) : colour(c);
        set_colour(&p, i, ci);
    }
    if (semi || raw || !shading)
        c->dither = 0;
    if (vary && !semi && !c->dither && !(c->mask & 2)) {
        int k = R(c, 2);
        if (k)
            c->dither = 1;
        else
            c->mask |= 2;
    }
    return poly_words(c, &p, op, clut, page);
}

void fd_generate_bounded(uint64_t seed, uint64_t index, int commands, fd_stream *s)
{
    C c;
    int i;
    uint32_t setup[200];

    memset(&c, 0, sizeof c);
    memset(s, 0, sizeof *s);
    fd_rng_stream(&c.r, seed ^ 0xb0b0u, index);
    c.s = s;
    fd_gen_background(&c.r, s);
    for (i = 0; i < commands; ++i) {
        int k = R(&c, 100), ok, setup_n = 0;
        c.group = i + 1;
        state_random(&c);
        if (k < 50)
            ok = bounded_gouraud_poly(&c);
        else
            ok = bounded_textured_poly(&c, &setup_n, setup);
        if (!ok)
            continue;
        if (setup_n) {
            /* the texels, written whatever the mask bits there */
            emit1(&c, 0xe6000000u);
            emit_words(&c, setup, setup_n, 0, 0);
            emit1(&c, 0x01000000u);
        }
        state_emit(&c);
        emit_words(&c, c.w, c.n, 1, 0);
    }
}

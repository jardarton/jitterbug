/*
 * The scaled pass: primitives rasterised into the scaled VRAM
 * (src/core/context.h) at N times the resolution, with the 1x pass's
 * conventions scaled up.
 *
 * - Scaled pixel (X, Y) is sampled at (X / N, Y / N) in 1x coordinates,
 *   just as 1x pixel (x, y) is sampled at (x, y). Triangle vertices are
 *   positioned in 1/256 pixels (the GTE's unrounded projection where the
 *   precise source had one, src/core/precise_source.h), so coverage
 *   follows the exact edges: left and top edges included, right and
 *   bottom excluded. A triangle
 *   with integer vertices covers N x N scaled pixels for every 1x pixel it
 *   covers.
 * - Colours and texture coordinates follow the 1x pass's rule (raster.c,
 *   gradient()): the gradients per 1x pixel have the console's 12
 *   fractional bits, truncated toward zero, and the values start half a
 *   unit above the first vertex's. They are evaluated at the centre of
 *   each scaled pixel relative to its 1x pixel: scaled pixel X takes its
 *   values at X / N + 1/(2N) - 1/2 in 1x coordinates, so the N samples of a
 *   1x pixel lie evenly around the point the 1x pass uses. (At the corner,
 *   X / N, the half unit would shift every texture by half a texel; at the
 *   centre a texture mapped one texel to a pixel gives each 1x pixel's
 *   block its 1x texel, its sub-texels in order, as before.) Values taken
 *   just outside a triangle's edge are held to the vertices' range (or a
 *   texel beside the texture, or a colour beyond 0..255, would show).
 *   15-bit texels come from the scaled VRAM at sub-texel precision, 4- and
 *   8-bit texels from the 1x VRAM, and their colours from the palette the
 *   CLUT cache fixed for the primitive, as at 1x (or with the cache off
 *   from the 1x VRAM).
 * - Rectangles stay on the 1x grid: each texel or pixel becomes an N x N
 *   block (15-bit texels again from the scaled VRAM).
 * - Lines are N scaled pixels thick, so they look as they do at 1x, and
 *   run between the scaled end points, the end pixels included. An exact
 *   half in the minor coordinate rounds as at 1x. Flat lines are dithered
 *   as Gouraud ones are.
 * - Dithering, if enabled, uses the 4x4 matrix over 1x pixels.
 * - Semi-transparency and the mask bit work per scaled pixel.
 * - With perspective-correct texturing, textured triangles whose vertices
 *   have depths (JBI_F_PERSP, src/core/prim.h) are drawn perspective-correct,
 *   at the same (precise) vertex positions: hpersp_span.
 */
#include "soft/raster.h"

#include <stddef.h>

typedef int64_t i64;
/* __int128 is a GNU extension; this only quiets -Wpedantic about it. */
__extension__ typedef __int128 i128;

/* Values in HF fixed point. The gradients' 12 fractional bits (GFRAC) are
   divided by the scale, so HF has many more; the steps per scaled pixel are
   rounded down, which loses under 2^-40 a pixel, so under 2^-26 over any
   distance in VRAM; HBIAS (2^-20) covers that and stays below the
   gradients' unit (2^-12), so a value the 1x pass takes exactly is taken
   the same here. */
#define HF 40
#define HONE ((i64)1 << HF)
#define HHALF (HONE >> 1)
#define GFRAC 12
#define HBIAS ((i64)1 << 20)

/* Right shifts. C99 leaves the right shift of a negative value to the
   implementation, so a value that can be negative is divided instead
   (floor_div64, floor_div128); every value shifted right in this file is
   non-negative: the values a span function steps through are in their
   ranges, 0..255 in HF (hclamp_span; jbi_persp_at clamps), and colours are
   0..255 before >> 3. Conversions to a narrower signed type are of values
   that fit (said where it is not plain). */

static inline int clamp255(int c)
{
    return c < 0 ? 0 : c > 255 ? 255 : c;
}

static inline int min31(int c)
{
    return c > 31 ? 31 : c;
}

static inline i64 floor_div64(i64 n, i64 d) /* d > 0 */
{
    i64 q = n / d;
    return (n % d != 0 && n < 0) ? q - 1 : q;
}

static inline i64 ceil_div64(i64 n, i64 d) /* d > 0 */
{
    return -floor_div64(-n, d);
}

static inline i128 floor_div128(i128 n, i128 d) /* d > 0 */
{
    i128 q = n / d;
    return (n % d != 0 && n < 0) ? q - 1 : q;
}

static inline i128 ceil_div128(i128 n, i128 d) /* d > 0 */
{
    return -floor_div128(-n, d);
}

/* ------------------------------------------------------------------------ */
/* Texels */

/* Row y of the 1x VRAM. */
#define VROW(vram, y) ((vram) + (size_t)(y) * JBI_VRAM_W)

static inline __attribute__((always_inline)) uint32_t
fetch_1x(const jbi_tex_source *t, const uint16_t *vram, uint32_t u, uint32_t v, const int depth)
{
    const uint16_t *row = VROW(vram, (t->tpy + (int)v) & (JBI_VRAM_H - 1));

    if (depth == 4) {
        uint32_t word = row[(t->tpx + (int)(u >> 2)) & (JBI_VRAM_W - 1)];
        uint32_t index = (word >> ((u & 3) << 2)) & 0xf;
        return t->clut != NULL ? t->clut[index]
                               : VROW(vram, t->clut_y)[(t->clut_x + (int)index) & (JBI_VRAM_W - 1)];
    }
    if (depth == 8) {
        uint32_t word = row[(t->tpx + (int)(u >> 1)) & (JBI_VRAM_W - 1)];
        uint32_t index = (word >> ((u & 1) << 3)) & 0xff;
        return t->clut != NULL ? t->clut[index]
                               : VROW(vram, t->clut_y)[(t->clut_x + (int)index) & (JBI_VRAM_W - 1)];
    }
    return row[(t->tpx + (int)u) & (JBI_VRAM_W - 1)];
}

/* A 15-bit texel's sub-texel (su, sv) in the scaled VRAM. u and v are
   already windowed. */
static inline uint32_t fetch_15_hires(const jbi_tex_source *t, const jbi_hires *h, uint32_t u,
                                      uint32_t v, int su, int sv)
{
    int n = h->scale;
    int col = ((t->tpx + (int)u) & (JBI_VRAM_W - 1)) * n + su;
    int row = ((t->tpy + (int)v) & (JBI_VRAM_H - 1)) * n + sv;

    return h->pix[(size_t)row * (size_t)h->width + (size_t)col];
}

/* u and v in HF fixed point; vram is h->vram. */
static inline __attribute__((always_inline)) uint32_t
tex_fetch(const jbi_tex_source *t, const jbi_hires *h, const uint16_t *vram, i64 u, i64 v,
          const int depth)
{
    uint32_t ui = (uint32_t)(u >> HF), vi = (uint32_t)(v >> HF);

    ui = ((ui & 0xff) & t->and_x) | t->or_x;
    vi = ((vi & 0xff) & t->and_y) | t->or_y;
    if (depth != 15)
        return fetch_1x(t, vram, ui, vi, depth);
    return fetch_15_hires(t, h, ui, vi, (int)(((u & (HONE - 1)) * h->scale) >> HF),
                          (int)(((v & (HONE - 1)) * h->scale) >> HF));
}

/* ------------------------------------------------------------------------ */
/* Triangles */

typedef struct {
    i64 r, g, b, u, v;
    i64 drdx, dgdx, dbdx, dudx, dvdx;
    int fr, fg, fb;
    jbi_tex_source tex;
    int semi_mode;
    int check_mask;
    uint32_t set_mask;
    /* The ranges values are held to (r, g, b, u, v). clamp: this span's
       values leave them somewhere and are held to them pixel by pixel
       (only for a narrow triangle, whose values a span steps through fit
       64 bits: hvalue64); otherwise every value the span steps through is
       inside them (hi_rows, hclamp_span). */
    int clamp;
    i64 lo[5], hi[5];
} HSpan;

static inline i64 clamp64(i64 a, i64 lo, i64 hi)
{
    return a < lo ? lo : a > hi ? hi : a;
}

static inline __attribute__((always_inline)) void
hspan_impl(HSpan *s, const jbi_hires *h, int y, int x0, int x1, const int depth,
           const int gouraud, const int raw, const int semi, const int dither, const int clamp)
{
    uint16_t *row = h->pix + (size_t)y * (size_t)h->width;
    const uint16_t *vram = h->vram;
    const signed char *dm = jbi_dither_matrix[(y / h->scale) & 3];
    i64 r = s->r, g = s->g, b = s->b, u = s->u, v = s->v;
    /* 4- and 8-bit texels: neighbouring scaled pixels mostly share one, so
       the last is kept, keyed by its unwindowed coordinates. */
    uint32_t last_key = ~0u, last_texel = 0;
    int x;

    for (x = x0; x < x1; ++x) {
        uint16_t *dst = &row[x];
        uint32_t out, stp = 0;
        int cr, cg, cb;
        i64 er = r, eg = g, eb = b, eu = u, ev = v; /* the values used */

        if (s->check_mask && (*dst & 0x8000))
            goto next;
        if (clamp) {
            er = clamp64(r, s->lo[0], s->hi[0]);
            eg = clamp64(g, s->lo[1], s->hi[1]);
            eb = clamp64(b, s->lo[2], s->hi[2]);
            eu = clamp64(u, s->lo[3], s->hi[3]);
            ev = clamp64(v, s->lo[4], s->hi[4]);
        }
        if (gouraud) {
            cr = (int)(er >> HF);
            cg = (int)(eg >> HF);
            cb = (int)(eb >> HF);
        } else {
            cr = s->fr;
            cg = s->fg;
            cb = s->fb;
        }
        if (depth) {
            uint32_t texel;
            int tr, tg, tb;

            if (depth == 15) {
                texel = tex_fetch(&s->tex, h, vram, eu, ev, depth);
            } else {
                uint32_t key = ((uint32_t)(eu >> HF) & 0xff) | ((uint32_t)(ev >> HF) & 0xff) << 8;
                if (key != last_key) {
                    last_key = key;
                    last_texel = tex_fetch(&s->tex, h, vram, eu, ev, depth);
                }
                texel = last_texel;
            }

            if (texel == 0)
                goto next;
            stp = texel & 0x8000;
            tr = (int)(texel & 31);
            tg = (int)((texel >> 5) & 31);
            tb = (int)((texel >> 10) & 31);
            if (raw) {
                cr = tr;
                cg = tg;
                cb = tb;
            } else if (dither) {
                int d = dm[h->dither_col[x]];
                cr = clamp255(((tr * cr) >> 4) + d) >> 3;
                cg = clamp255(((tg * cg) >> 4) + d) >> 3;
                cb = clamp255(((tb * cb) >> 4) + d) >> 3;
            } else {
                cr = min31((tr * cr) >> 7);
                cg = min31((tg * cg) >> 7);
                cb = min31((tb * cb) >> 7);
            }
        } else if (dither) {
            int d = dm[h->dither_col[x]];
            cr = clamp255(cr + d) >> 3;
            cg = clamp255(cg + d) >> 3;
            cb = clamp255(cb + d) >> 3;
        } else {
            cr >>= 3;
            cg >>= 3;
            cb >>= 3;
        }
        if (semi && (!depth || stp))
            out = jbi_blend(*dst, cr, cg, cb, s->semi_mode);
        else
            out = (uint32_t)cr | (uint32_t)cg << 5 | (uint32_t)cb << 10;
        *dst = (uint16_t)(out | stp | s->set_mask);
    next:
        if (gouraud) {
            r += s->drdx;
            g += s->dgdx;
            b += s->dbdx;
        }
        if (depth) {
            u += s->dudx;
            v += s->dvdx;
        }
    }
}

/* Each variant has a loop that holds values to their ranges and one that
   need not, chosen once a span (HSpan's clamp). */
#define HSPAN_FN(D, G, R, S, T)                                                      \
    static void hspan_##D##_##G##R##S##T(HSpan *s, const jbi_hires *h, int y, int x0, \
                                         int x1)                                      \
    {                                                                                \
        if (s->clamp)                                                                \
            hspan_impl(s, h, y, x0, x1, D, G, R, S, T, 1);                            \
        else                                                                         \
            hspan_impl(s, h, y, x0, x1, D, G, R, S, T, 0);                            \
    }
#define HSPAN_FNS_D(D)                                                        \
    HSPAN_FN(D, 0, 0, 0, 0) HSPAN_FN(D, 0, 0, 0, 1) HSPAN_FN(D, 0, 0, 1, 0)    \
    HSPAN_FN(D, 0, 0, 1, 1) HSPAN_FN(D, 0, 1, 0, 0) HSPAN_FN(D, 0, 1, 0, 1)    \
    HSPAN_FN(D, 0, 1, 1, 0) HSPAN_FN(D, 0, 1, 1, 1) HSPAN_FN(D, 1, 0, 0, 0)    \
    HSPAN_FN(D, 1, 0, 0, 1) HSPAN_FN(D, 1, 0, 1, 0) HSPAN_FN(D, 1, 0, 1, 1)    \
    HSPAN_FN(D, 1, 1, 0, 0) HSPAN_FN(D, 1, 1, 0, 1) HSPAN_FN(D, 1, 1, 1, 0)    \
    HSPAN_FN(D, 1, 1, 1, 1)
HSPAN_FNS_D(0)
HSPAN_FNS_D(4)
HSPAN_FNS_D(8)
HSPAN_FNS_D(15)

typedef void (*HSpanFn)(HSpan *s, const jbi_hires *h, int y, int x0, int x1);

/* A small bias for perspective values, so that values on an integer do
   not truncate below it. */
#define HPBIAS (1.0 / 65536)

/*
 * Scaled pixels [xl, xr) of row y of a perspective-correct triangle: the
 * exact attributes every h->persp_span pixels from xl, and the affine span
 * function fn in between, stepping linearly to the next exact value (to the
 * last pixel's, for the last piece). One copy for every variant (hi_rows
 * passes fn).
 */
static __attribute__((noinline)) void hpersp_span(HSpan *s, HSpanFn fn, const jbi_hires *h,
                                                  const jbi_persp *pp, int y, int xl, int xr)
{
    double row[JBI_PERSP_ATTRS + 1];
    long long a[JBI_PERSP_ATTRS], b[JBI_PERSP_ATTRS];
    int x0, x1, e, k, n = pp->count, len = h->persp_span;

    s->clamp = 0; /* jbi_persp_at clamps */
    jbi_persp_row(pp, y, row);
    jbi_persp_at(pp, row, xl, HF, HPBIAS, a);
    for (x0 = xl; x0 < xr; x0 = x1) {
        i64 step[JBI_PERSP_ATTRS] = {0};

        x1 = xr - x0 > len ? x0 + len : xr;
        e = x1 < xr ? x1 : xr - 1;
        if (e > x0) {
            jbi_persp_at(pp, row, e, HF, HPBIAS, b);
            for (k = 0; k < n; ++k)
                step[k] = (b[k] - a[k]) / (e - x0);
        }
        s->u = a[0];
        s->v = a[1];
        s->dudx = step[0];
        s->dvdx = step[1];
        if (n > 2) {
            s->r = a[2];
            s->g = a[3];
            s->b = a[4];
            s->drdx = step[2];
            s->dgdx = step[3];
            s->dbdx = step[4];
        }
        fn(s, h, y, x0, x1);
        if (x1 < xr)
            for (k = 0; k < n; ++k)
                a[k] = b[k];
    }
}

static int too_far(const jbi_vertex *a, const jbi_vertex *b)
{
    int dx = a->x - b->x, dy = a->y - b->y;
    return dx > 1023 || dx < -1023 || dy > 511 || dy < -511;
}

/*
 * The bounds. A vertex's 1x position is a drawing offset (-1024..1023)
 * plus an 11-bit coordinate (-1024..1023), so -2048..2046, and a precise
 * one lies within 1 1/8 pixels (288/256) of it (gp0.c, PRECISE_SLACK):
 * |qx|, |qy| <= 2048 * 256 + 288 = 2^19 + 288 in 1/256 pixels. Two
 * vertices of a triangle drawn are at most 1023 pixels apart in x and 511
 * in y (too_far), so their qx differ by at most 1023 * 256 + 576 = 262464
 * (just over 2^18) and their qy by at most 511 * 256 + 576 = 131392 (just
 * over 2^17); X and Y (1/256 scaled pixels) are those times the scale
 * n <= 8. The doubled area in 1/256 pixels is a non-zero integer: at
 * least 1 for precise vertices, 65536 for integer ones, and n^2 times that
 * in X, Y.
 *
 * The gradients per 1x pixel of attribute values a0..a2 (0..255) at
 * vertices X, Y, with the console's GFRAC fractional bits truncated toward
 * zero (raster.c, gradient()): the 1x pass's for integer vertices. area is
 * the doubled signed area. nx = a0 (Y1 - Y2) + a1 (Y2 - Y0) + a2 (Y0 - Y1),
 * whose three coefficients sum to zero, so |nx| <= 255 times the largest
 * of them: |nx| <= 255 * 131392 * n and |ny| <= 255 * 262464 * n, both
 * under 2^26 * n <= 2^29. The products are under 2^29 * 2^20 * 2^3 =
 * 2^52, and the quotient is under 2^26 * n * 2^20 * n / n^2 = 2^46 (2^34 a
 * pixel: a sliver from precise vertices, area 1). For integer vertices it
 * is at most 255 * 1023 * 2^12 < 2^30 (the largest coefficient is a
 * difference of two vertices' positions, at most 1023 pixels, and the area
 * at least 65536 n^2). Nothing is capped: the gradient keeps the
 * triangle's direction however steep, and the values are held to the
 * vertices' range only where they are used (hi_rows).
 */
static void hgradient(int a0, int a1, int a2, const i64 *X, const i64 *Y, i64 area, int n,
                      i64 *gx, i64 *gy)
{
    i64 nx = (i64)(a1 - a0) * (Y[2] - Y[0]) - (i64)(a2 - a0) * (Y[1] - Y[0]);
    i64 ny = (i64)(a2 - a0) * (X[1] - X[0]) - (i64)(a1 - a0) * (X[2] - X[0]);

    *gx = nx * ((i64)1 << (GFRAC + 8)) * n / area;
    *gy = ny * ((i64)1 << (GFRAC + 8)) * n / area;
}

/*
 * Two ways to set a triangle's rows up, which give the same values
 * wherever both apply (tests/hires_wide.c compares them pixel by pixel).
 *
 * The narrow one, in 64 bits, for a triangle whose gradients are all
 * under NARROW_G in magnitude (2^18 a 1x pixel): every integer-vertex
 * triangle the GPU draws (the bound above), and every precise one but
 * slivers. The wide one, in 128 bits, for the rest (hwide_row).
 */
#define NARROW_G ((i64)1 << 30)

/* Test hooks (tests/hires_wide.c, which compiles this file again with
   them): the route each triangle takes, and every triangle set up wide. */
#ifndef JBI_HIRES_ROUTED
#define JBI_HIRES_ROUTED(wide) ((void)0)
#endif
#ifndef JBI_HIRES_FORCE_WIDE
#define JBI_HIRES_FORCE_WIDE 0
#endif

/* A gradient per 1x pixel (under 2^46) as the step per scaled pixel, in
   HF: under 2^74 / n, so in 128 bits. */
static inline i128 hstep(i64 g, int n)
{
    return floor_div128((i128)g * ((i128)1 << (HF - GFRAC)), n);
}

/* The same for |g| < NARROW_G: g * 2^28 is under 2^58. */
static inline i64 hstep64(i64 g, int n)
{
    return floor_div64(g * ((i64)1 << (HF - GFRAC)), n);
}

/* The attribute at offset (dxq, dyq) from vertex 0, in 1/256 scaled
   pixels: a0 + 1/2 plus the steps per scaled pixel (hstep) times the
   offset, rounded down, plus HBIAS. The steps are rounded down from the
   gradients, by under 2^-40 a scaled pixel, so over any offset in VRAM
   the value is at most 2^-26 low, which HBIAS covers. |dxq|, |dyq| <
   2^23 (a span starts inside the drawing area, xl * 256 < 2^21; vertex 0
   is at most 8 * (2^19 + 288) < 2^22 + 2^12 from the origin; the centre
   offset is under 2^10), so |d| < 2 * 2^74 * 2^23 = 2^98 and the value is
   under 2^91: 128 bits again. The floor is a division (C99 leaves the
   right shift of a negative value to the implementation). */
static inline i128 hvalue(int a0, i128 sx, i128 sy, i64 dxq, i64 dyq)
{
    i128 d = sx * dxq + sy * dyq;

    return ((i128)a0 << HF) + HHALF + HBIAS + floor_div128(d, 256);
}

/*
 * The 64-bit int whose value is that of u modulo 2^64, for a value known
 * to fit (a conversion of an unsigned value that doesn't fit to a signed
 * type is implementation-defined, so not a cast).
 */
static inline i64 from_u64(uint64_t u)
{
    return u <= (uint64_t)INT64_MAX ? (i64)u : -(i64)(UINT64_MAX - u) - 1;
}

/*
 * hvalue for a narrow triangle, in 64 bits, exactly: the steps split as
 * s = 256 sh + sl (0 <= sl < 256, the split hi_triangle stores), so
 *   floor(d / 256) = (shx dxq + shy dyq) + floor((slx dxq + sly dyq) / 256).
 * The second term is under 2 * 2^8 * 2^23 = 2^32. The first may pass
 * through products of 2^72 (|sh| < 2^50, |dxq| < 2^23) on its way, so it
 * is summed modulo 2^64 (unsigned, which wraps), and that is its value
 * because the value itself fits: every span hi_rows sets up starts and
 * ends at a pixel (xl, y), (xr - 1, y) inside the triangle (on or right of
 * its left edge, left of its right edge, on or below its top and above its
 * bottom), and its values are taken at that pixel's centre relative to its
 * 1x pixel, under half a 1x pixel from it on either axis. Inside the
 * triangle, the plane through the vertices' values is within their range
 * (0..255); the gradients used are truncated from the plane's by under
 * 2^-12 a 1x pixel, over at most 1026 1x pixels across and 514 down from
 * vertex 0 (under 1 in all); and the half pixel to the centre adds under
 * (|gx| + |gy|) / 2 < 2^18. So the value is within 2^18 + 2 of 0..255,
 * under 2^59 in HF, and the first term (the value less a0, the halves and
 * the second term) under 2^60. The value at the span's end, the start plus
 * sx (len - 1), is under 2^59 the same way, so that product fits too.
 */
static inline i64 hvalue64(int a0, i64 shx, i64 slx, i64 shy, i64 sly, i64 dxq, i64 dyq)
{
    uint64_t hi = (uint64_t)shx * (uint64_t)dxq + (uint64_t)shy * (uint64_t)dyq;

    return ((i64)a0 << HF) + HHALF + HBIAS + from_u64(hi) + floor_div64(slx * dxq + sly * dyq, 256);
}

/* First scaled column at or right of edge a-b on the row at yq (1/256). */
static inline int hedge(i64 xa, i64 ya, i64 xb, i64 yb, i64 yq)
{
    i64 dy = yb - ya;
    return (int)ceil_div64(xa * dy + (yq - ya) * (xb - xa), dy * 256);
}

/* What a triangle's rows need once it is set up (hi_triangle). */
typedef struct {
    i64 X[3], Y[3], cross;
    int top, mid, bot, y, y_end, cx0, cx1, persp, n;
    int first, count; /* the attributes interpolated: r, g, b, u, v */
    int wide;         /* set up in 128 bits (NARROW_G) */
    int a0[5];        /* vertex 0's values */
    /* Steps per scaled pixel and per scaled row (hstep), HF bits. Narrow:
       sx64, sy64 and their splits (hvalue64). Wide: sx, sy. */
    i64 sx64[5], shx[5], slx[5], shy[5], sly[5];
    i128 sx[5], sy[5];
    i64 step[5]; /* sx held to the range, for spans in range */
    jbi_persp pp;
} HTri;

/*
 * Where attribute k of a wide triangle's span of len pixels, starting at
 * value a and stepping by st (128 bits), is inside its range [lo, hi]:
 * pixels [in0, in1] (in0 within [0, len], in1 within [-1, len - 1]; past
 * the span it makes no difference), with the value base at in0 and st as
 * the step there (0 if in1 is in0, so it fits: |st| <= hi - lo where two
 * pixels are in range); outside them it is `before` on the left and
 * `after` on the right. The range is at least a unit wide, so past the
 * interval a value is below it on one side and above it on the other.
 */
typedef struct {
    int in0, in1;
    i64 before, after, base, st;
} HRun;

static void hrun_wide(HRun *r, i64 lo, i64 hi, int len, i128 a, i128 st)
{
    i128 e0, e1;

    if (st > 0) {
        e0 = ceil_div128(lo - a, st);
        e1 = floor_div128(hi - a, st);
        r->before = lo;
        r->after = hi;
    } else if (st < 0) {
        e0 = ceil_div128(a - hi, -st);
        e1 = floor_div128(a - lo, -st);
        r->before = hi;
        r->after = lo;
    } else {
        e0 = a >= lo && a <= hi ? 0 : len;
        e1 = len - 1;
        r->before = r->after = a < lo ? lo : hi;
    }
    r->in0 = (int)(e0 < 0 ? 0 : e0 > len ? len : e0);
    r->in1 = (int)(e1 < -1 ? -1 : e1 > len - 1 ? len - 1 : e1);
    r->base = r->in0 <= r->in1 ? (i64)(a + st * r->in0) : 0;
    r->st = r->in1 > r->in0 ? (i64)st : 0;
}

/*
 * Pixels [xl, xl + len) of row y, attributes `first` to first + count - 1
 * as their runs say: each value held to its range, exactly as if every
 * pixel's a + i * st were clamped, but with no value outside the range
 * ever stepped through. The span is cut at every run's ends and each piece
 * drawn by fn with in-range starts and steps.
 */
static void hclamp_span(HSpan *s, HSpanFn fn, const jbi_hires *h, int y, int xl, int len,
                        int first, int count, const HRun *run)
{
    i64 *val[5] = {&s->r, &s->g, &s->b, &s->u, &s->v};
    i64 *step[5] = {&s->drdx, &s->dgdx, &s->dbdx, &s->dudx, &s->dvdx};
    int cut[12], ncut = 0, k, c, i, j;

    s->clamp = 0;
    cut[ncut++] = 0;
    cut[ncut++] = len;
    for (k = first; k < first + count; ++k) {
        cut[ncut++] = run[k].in0;
        cut[ncut++] = run[k].in1 + 1;
    }
    for (i = 1; i < ncut; ++i) /* sort; at most 12 */
        for (j = i; j > 0 && cut[j - 1] > cut[j]; --j) {
            c = cut[j];
            cut[j] = cut[j - 1];
            cut[j - 1] = c;
        }
    for (i = 0; i + 1 < ncut; ++i) {
        int p = cut[i], q = cut[i + 1];

        if (p >= q)
            continue;
        for (k = first; k < first + count; ++k) {
            const HRun *r = &run[k];

            if (q - 1 < r->in0) {
                *val[k] = r->before;
                *step[k] = 0;
            } else if (p > r->in1) {
                *val[k] = r->after;
                *step[k] = 0;
            } else {
                /* [p, q) inside [in0, in1] (the cuts): in-range values */
                *val[k] = r->base + r->st * (p - r->in0);
                *step[k] = q - p > 1 ? r->st : 0;
            }
        }
        fn(s, h, y, xl + p, xl + q);
    }
}

/* Scaled pixels [xl, xr) of row y of a wide triangle, the values taken at
   (dxq, dyq) from vertex 0 (hi_rows). */
static __attribute__((noinline)) void hwide_row(const HTri *t, HSpan *s, HSpanFn fn,
                                                const jbi_hires *h, int y, int xl, int xr,
                                                i64 dxq, i64 dyq)
{
    i128 a[5] = {0, 0, 0, 0, 0};
    HRun run[5];
    int k, clamp = 0;

    for (k = t->first; k < t->first + t->count; ++k) {
        i128 e;

        a[k] = hvalue(t->a0[k], t->sx[k], t->sy[k], dxq, dyq);
        e = a[k] + t->sx[k] * (xr - xl - 1);
        clamp |= a[k] < s->lo[k] || a[k] > s->hi[k] || e < s->lo[k] || e > s->hi[k];
    }
    if (clamp) {
        for (k = t->first; k < t->first + t->count; ++k)
            hrun_wide(&run[k], s->lo[k], s->hi[k], xr - xl, a[k], t->sx[k]);
        hclamp_span(s, fn, h, y, xl, xr - xl, t->first, t->count, run);
        return;
    }
    /* In range at both ends, so at every pixel: 64 bits hold them. */
    s->clamp = 0;
    s->r = (i64)a[0];
    s->g = (i64)a[1];
    s->b = (i64)a[2];
    s->u = (i64)a[3];
    s->v = (i64)a[4];
    s->drdx = t->step[0];
    s->dgdx = t->step[1];
    s->dbdx = t->step[2];
    s->dudx = t->step[3];
    s->dvdx = t->step[4];
    fn(s, h, y, xl, xr);
}

/* Attribute k of a narrow triangle at (dxq, dyq) from vertex 0, and
   whether it or its value `last` pixels on is out of range (clamp). */
static inline __attribute__((always_inline)) i64 hnarrow(const HTri *t, const HSpan *s, int k,
                                                         i64 dxq, i64 dyq, int last, int *clamp)
{
    i64 a = hvalue64(t->a0[k], t->shx[k], t->slx[k], t->shy[k], t->sly[k], dxq, dyq);
    i64 e = a + t->sx64[k] * last;

    *clamp |= a < s->lo[k] || a > s->hi[k] || e < s->lo[k] || e > s->hi[k];
    return a;
}

/*
 * The scaled rows of a triangle the worker owns, each drawn by span
 * function fn. Inlined once per variant (hi_triangle), so fn is a direct
 * call.
 */
static inline __attribute__((always_inline)) void
hi_rows(HTri *t, HSpan *s, const jbi_rows *rows, const jbi_hires *h, HSpanFn fn)
{
    const i64 *X = t->X, *Y = t->Y;
    int top = t->top, mid = t->mid, bot = t->bot, y;

    for (y = t->y; y < t->y_end; ++y) {
        i64 yq = (i64)y * 256, dxq, dyq;
        int xl, xr, clamp;

        if (!jbi_rows_owns(rows, y)) {
            y = jbi_rows_next(rows, y) - 1;
            continue;
        }
        xl = hedge(X[top], Y[top], X[bot], Y[bot], yq);
        xr = yq < Y[mid] ? hedge(X[top], Y[top], X[mid], Y[mid], yq)
                         : hedge(X[mid], Y[mid], X[bot], Y[bot], yq);
        if (t->cross < 0) {
            int tmp = xl;
            xl = xr;
            xr = tmp;
        }
        if (xl < t->cx0)
            xl = t->cx0;
        if (xr > t->cx1)
            xr = t->cx1;
        if (xl >= xr)
            continue;
        if (t->persp) {
            hpersp_span(s, fn, h, &t->pp, y, xl, xr);
            continue;
        }
        /* The values at the centre of scaled pixel (xl, y) relative to its
           1x pixel (the file's top): (xl, y) + 1/2 - N/2 scaled pixels. */
        dxq = (i64)xl * 256 + 128 - 128 * (i64)t->n - X[0];
        dyq = yq + 128 - 128 * (i64)t->n - Y[0];
        if (t->wide) {
            hwide_row(t, s, fn, h, y, xl, xr, dxq, dyq);
            continue;
        }
        /* Out of range somewhere: held to it pixel by pixel, which gives
           what hclamp_span would (the values and the one step past the
           span's end, under 2^60 + 2^57, fit). Else in range at every
           pixel. Attributes not interpolated are not used. */
        clamp = 0;
        if (t->first == 0) {
            s->r = hnarrow(t, s, 0, dxq, dyq, xr - xl - 1, &clamp);
            s->g = hnarrow(t, s, 1, dxq, dyq, xr - xl - 1, &clamp);
            s->b = hnarrow(t, s, 2, dxq, dyq, xr - xl - 1, &clamp);
        }
        if (t->first + t->count == 5) {
            s->u = hnarrow(t, s, 3, dxq, dyq, xr - xl - 1, &clamp);
            s->v = hnarrow(t, s, 4, dxq, dyq, xr - xl - 1, &clamp);
        }
        s->clamp = clamp;
        if (t->first == 0) {
            s->drdx = clamp ? t->sx64[0] : t->step[0];
            s->dgdx = clamp ? t->sx64[1] : t->step[1];
            s->dbdx = clamp ? t->sx64[2] : t->step[2];
        }
        if (t->first + t->count == 5) {
            s->dudx = clamp ? t->sx64[3] : t->step[3];
            s->dvdx = clamp ? t->sx64[4] : t->step[4];
        }
        fn(s, h, y, xl, xr);
    }
}

/* The variant for sel, as tri_variant in raster.c: a switch over hi_rows
   inlined with each span function, rather than a table of function
   pointers (which would be relocated data). */
#define HTRI_CASES(I, D)                                                      \
    case I * 16 + 0: hi_rows(t, s, rows, h, hspan_##D##_0000); return;        \
    case I * 16 + 1: hi_rows(t, s, rows, h, hspan_##D##_0001); return;        \
    case I * 16 + 2: hi_rows(t, s, rows, h, hspan_##D##_0010); return;        \
    case I * 16 + 3: hi_rows(t, s, rows, h, hspan_##D##_0011); return;        \
    case I * 16 + 4: hi_rows(t, s, rows, h, hspan_##D##_0100); return;        \
    case I * 16 + 5: hi_rows(t, s, rows, h, hspan_##D##_0101); return;        \
    case I * 16 + 6: hi_rows(t, s, rows, h, hspan_##D##_0110); return;        \
    case I * 16 + 7: hi_rows(t, s, rows, h, hspan_##D##_0111); return;        \
    case I * 16 + 8: hi_rows(t, s, rows, h, hspan_##D##_1000); return;        \
    case I * 16 + 9: hi_rows(t, s, rows, h, hspan_##D##_1001); return;        \
    case I * 16 + 10: hi_rows(t, s, rows, h, hspan_##D##_1010); return;       \
    case I * 16 + 11: hi_rows(t, s, rows, h, hspan_##D##_1011); return;       \
    case I * 16 + 12: hi_rows(t, s, rows, h, hspan_##D##_1100); return;       \
    case I * 16 + 13: hi_rows(t, s, rows, h, hspan_##D##_1101); return;       \
    case I * 16 + 14: hi_rows(t, s, rows, h, hspan_##D##_1110); return;       \
    case I * 16 + 15: hi_rows(t, s, rows, h, hspan_##D##_1111); return;
static void hi_variant(int sel, HTri *t, HSpan *s, const jbi_rows *rows, const jbi_hires *h)
{
    switch (sel) {
        HTRI_CASES(0, 0)
        HTRI_CASES(1, 4)
        HTRI_CASES(2, 8)
        HTRI_CASES(3, 15)
    default:
        return;
    }
}

static void hi_triangle(const jbi_prim *p, const jbi_rows *rows, const jbi_hires *h)
{
    const jbi_vertex *v = p->u.tri.v;
    int n = h->scale, flags = p->flags;
    int gouraud = (flags & JBI_F_GOURAUD) != 0;
    int textured = (flags & JBI_F_TEXTURED) != 0;
    int raw = textured && (flags & JBI_F_RAW);
    int dither, depth_index, top, mid, bot, i, sel;
    i64 *X, *Y, area, gx[5], gy[5];
    HTri t;
    HSpan s;

    if (flags & JBI_F_NO_SCALED)
        return; /* drawn in the 1x pass only (prim.h) */
    if (too_far(&v[0], &v[1]) || too_far(&v[1], &v[2]) || too_far(&v[2], &v[0]))
        return;
    X = t.X;
    Y = t.Y;
    for (i = 0; i < 3; ++i) {
        X[i] = (i64)p->u.tri.qx[i] * n;
        Y[i] = (i64)p->u.tri.qy[i] * n;
    }
    area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
    if (area == 0)
        return;

    if (raw)
        gouraud = 0;
    dither = h->dither && p->dither && (gouraud || (textured && !raw));

    /* Perspective-correct: flat or not, and the flat colour, from the
       perspective attributes this pass draws with (prim.h), as in
       raster.c; a failed set-up falls back to the vertices'. */
    t.persp = 0;
    s.fr = v[0].r;
    s.fg = v[0].g;
    s.fb = v[0].b;
    if (textured && (flags & JBI_F_PERSP)) {
        double PX[3], PY[3];
        const uint8_t(*pa)[5] = p->u.tri.pattr;
        int pg = gouraud && !(pa[0][2] == pa[1][2] && pa[0][2] == pa[2][2] &&
                              pa[0][3] == pa[1][3] && pa[0][3] == pa[2][3] &&
                              pa[0][4] == pa[1][4] && pa[0][4] == pa[2][4]);

        for (i = 0; i < 3; ++i) {
            PX[i] = (double)X[i] / 256;
            PY[i] = (double)Y[i] / 256;
        }
        t.persp = jbi_persp_setup(&t.pp, p, PX, PY, pg);
        if (t.persp) {
            gouraud = pg;
            s.fr = pa[0][2];
            s.fg = pa[0][3];
            s.fb = pa[0][4];
        }
    }
    if (!t.persp && gouraud && v[0].r == v[1].r && v[0].r == v[2].r && v[0].g == v[1].g &&
        v[0].g == v[2].g && v[0].b == v[1].b && v[0].b == v[2].b)
        gouraud = 0;

    s.drdx = s.dgdx = s.dbdx = s.dudx = s.dvdx = 0;
    s.r = s.g = s.b = s.u = s.v = 0;
    s.clamp = 0;
    t.n = n;
    t.first = gouraud ? 0 : 3;
    t.count = (gouraud ? 3 : 0) + (textured ? 2 : 0);
    t.wide = 0;
    for (i = 0; i < 5; ++i) {
        /* The range each value is clamped to (HSpan): integer parts from
           the vertices' least to their greatest. */
        int a0 = i == 0 ? v[0].r : i == 1 ? v[0].g : i == 2 ? v[0].b : i == 3 ? v[0].u : v[0].v;
        int a1 = i == 0 ? v[1].r : i == 1 ? v[1].g : i == 2 ? v[1].b : i == 3 ? v[1].u : v[1].v;
        int a2 = i == 0 ? v[2].r : i == 1 ? v[2].g : i == 2 ? v[2].b : i == 3 ? v[2].u : v[2].v;
        int lo = a0 < a1 ? a0 : a1, hi = a0 > a1 ? a0 : a1;

        lo = a2 < lo ? a2 : lo;
        hi = a2 > hi ? a2 : hi;
        s.lo[i] = (i64)lo << HF;
        s.hi[i] = ((i64)(hi + 1) << HF) - 1;
        t.a0[i] = a0;
        gx[i] = gy[i] = 0;
        if (i >= t.first && i < t.first + t.count) {
            hgradient(a0, a1, a2, X, Y, area, n, &gx[i], &gy[i]);
            if (gx[i] >= NARROW_G || gx[i] <= -NARROW_G || gy[i] >= NARROW_G ||
                gy[i] <= -NARROW_G)
                t.wide = 1;
        }
    }
    JBI_HIRES_ROUTED(t.wide);
    if (JBI_HIRES_FORCE_WIDE)
        t.wide = 1;
    for (i = 0; i < 5; ++i) {
        i64 most = s.hi[i] - s.lo[i];

        if (i < t.first || i >= t.first + t.count) {
            t.step[i] = 0; /* the rest is not read */
        } else if (t.wide) {
            t.sx[i] = hstep(gx[i], n);
            t.sy[i] = hstep(gy[i], n);
            /* The steps of a span whose values stay in range (hwide_row):
               there |sx| <= hi - lo, unless the span is one pixel and never
               steps. */
            t.step[i] = t.sx[i] > most ? most : t.sx[i] < -most ? -most : (i64)t.sx[i];
        } else {
            i64 sy = hstep64(gy[i], n);

            t.sx64[i] = hstep64(gx[i], n);
            /* the splits hvalue64 takes: s = 256 sh + sl, 0 <= sl < 256 */
            t.shx[i] = floor_div64(t.sx64[i], 256);
            t.slx[i] = t.sx64[i] - 256 * t.shx[i];
            t.shy[i] = floor_div64(sy, 256);
            t.sly[i] = sy - 256 * t.shy[i];
            /* as for a wide triangle */
            t.step[i] = t.sx64[i] > most ? most : t.sx64[i] < -most ? -most : t.sx64[i];
        }
    }
    if (textured)
        s.tex = p->tex;
    s.semi_mode = p->semi_mode;
    s.check_mask = p->check_mask;
    s.set_mask = p->set_mask;

    depth_index = 0;
    if (textured)
        depth_index = p->depth == 4 ? 1 : p->depth == 8 ? 2 : 3;
    sel = depth_index * 16 + ((gouraud << 3) | (raw << 2) | ((flags & JBI_F_SEMI) ? 2 : 0) |
                              dither);

    top = 0;
    mid = 1;
    bot = 2;
    if (Y[mid] < Y[top]) {
        i = top; top = mid; mid = i;
    }
    if (Y[bot] < Y[top]) {
        i = top; top = bot; bot = i;
    }
    if (Y[bot] < Y[mid]) {
        i = mid; mid = bot; bot = i;
    }
    if (Y[top] == Y[bot])
        return;
    t.top = top;
    t.mid = mid;
    t.bot = bot;
    t.cross = (X[mid] - X[top]) * (Y[bot] - Y[top]) - (X[bot] - X[top]) * (Y[mid] - Y[top]);

    t.y = (int)ceil_div64(Y[top], 256);
    t.y_end = (int)ceil_div64(Y[bot], 256);
    if (t.y < p->clip_y0 * n)
        t.y = p->clip_y0 * n;
    if (t.y_end > (p->clip_y1 + 1) * n)
        t.y_end = (p->clip_y1 + 1) * n;
    t.cx0 = p->clip_x0 * n;
    t.cx1 = (p->clip_x1 + 1) * n;
    hi_variant(sel, &t, &s, rows, h);
}

/* ------------------------------------------------------------------------ */
/* Rectangles */

static void hi_rect(const jbi_prim *p, const jbi_rows *rows, const jbi_hires *h)
{
    const uint16_t *vram = h->vram;
    int n = h->scale;
    int x = p->u.rect.x, y = p->u.rect.y, u = p->u.rect.u, v = p->u.rect.v;
    int r = p->u.rect.r, g = p->u.rect.g, b = p->u.rect.b;
    int textured = (p->flags & JBI_F_TEXTURED) != 0;
    int raw = (p->flags & JBI_F_RAW) != 0;
    int semi = (p->flags & JBI_F_SEMI) != 0;
    int du = p->flip_x ? -1 : 1, dv = p->flip_y ? -1 : 1;
    int depth = textured ? p->depth : 0;
    int x0 = x * n, y0 = y * n, x1 = (x + p->u.rect.w) * n, y1 = (y + p->u.rect.h) * n;
    uint32_t flat = (uint32_t)(r >> 3) | (uint32_t)(g >> 3) << 5 | (uint32_t)(b >> 3) << 10;
    const jbi_tex_source *tex = &p->tex;
    int px, py;

    if (x0 < p->clip_x0 * n)
        x0 = p->clip_x0 * n;
    if (y0 < p->clip_y0 * n)
        y0 = p->clip_y0 * n;
    if (x1 > (p->clip_x1 + 1) * n)
        x1 = (p->clip_x1 + 1) * n;
    if (y1 > (p->clip_y1 + 1) * n)
        y1 = (p->clip_y1 + 1) * n;
    if (x0 >= x1 || y0 >= y1)
        return;

    for (py = y0; py < y1; ++py) {
        uint16_t *row;
        int ry, ty, sv, tx, sx, cached = 0x7fffffff;
        uint32_t tv = 0, texel = 0;

        if (!jbi_rows_owns(rows, py)) {
            py = jbi_rows_next(rows, py) - 1;
            continue;
        }
        row = h->pix + (size_t)py * (size_t)h->width;
        ry = py - y * n;
        ty = ry / n;
        sv = ry - ty * n;
        if (dv < 0)
            sv = n - 1 - sv;
        if (textured) {
            tv = (uint32_t)(v + ty * dv);
            tv = ((tv & 0xff) & tex->and_y) | tex->or_y;
        }
        tx = (x0 - x * n) / n;
        sx = (x0 - x * n) - tx * n;
        for (px = x0; px < x1; ++px) {
            uint16_t *dst = &row[px];
            uint32_t stp = 0, out;
            int cr, cg, cb;

            if (p->check_mask && (*dst & 0x8000))
                goto next;
            if (textured) {
                uint32_t tu = (uint32_t)(u + tx * du);

                tu = ((tu & 0xff) & tex->and_x) | tex->or_x;
                if (depth == 15)
                    texel = fetch_15_hires(tex, h, tu, tv, du > 0 ? sx : n - 1 - sx, sv);
                else if (tx != cached)
                    texel = depth == 4 ? fetch_1x(tex, vram, tu, tv, 4)
                                       : fetch_1x(tex, vram, tu, tv, 8);
                cached = tx;
                if (texel == 0)
                    goto next;
                stp = texel & 0x8000;
                cr = (int)(texel & 31);
                cg = (int)((texel >> 5) & 31);
                cb = (int)((texel >> 10) & 31);
                if (!raw) {
                    cr = min31((cr * r) >> 7);
                    cg = min31((cg * g) >> 7);
                    cb = min31((cb * b) >> 7);
                }
            } else {
                cr = (int)(flat & 31);
                cg = (int)((flat >> 5) & 31);
                cb = (int)((flat >> 10) & 31);
            }
            if (semi && (!textured || stp))
                out = jbi_blend(*dst, cr, cg, cb, p->semi_mode);
            else
                out = (uint32_t)cr | (uint32_t)cg << 5 | (uint32_t)cb << 10;
            *dst = (uint16_t)(out | stp | p->set_mask);
        next:
            if (++sx == n) {
                sx = 0;
                ++tx;
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Lines */

static void hi_plot(const jbi_prim *p, const jbi_hires *h, int x, int y, int cr, int cg,
                    int cb, int dither)
{
    uint16_t *dst = &h->pix[(size_t)y * (size_t)h->width + (size_t)x];
    uint32_t out;

    if (p->check_mask && (*dst & 0x8000))
        return;
    if (dither) {
        int d = jbi_dither_matrix[(y / h->scale) & 3][h->dither_col[x]];
        cr = clamp255(cr + d);
        cg = clamp255(cg + d);
        cb = clamp255(cb + d);
    }
    cr >>= 3;
    cg >>= 3;
    cb >>= 3;
    if (p->flags & JBI_F_SEMI)
        out = jbi_blend(*dst, cr, cg, cb, p->semi_mode);
    else
        out = (uint32_t)cr | (uint32_t)cg << 5 | (uint32_t)cb << 10;
    *dst = (uint16_t)(out | p->set_mask);
}

/*
 * A line's colour channel at fraction num / den (clamped to [0, 1]) of its
 * way: as at 1x (raster.c, raster_line), c0 + 1/2 plus g12 (the step per 1x
 * step, 12 fractional bits) times the 1x steps that fraction is, rounded
 * down. At each 1x step's block centre that is the 1x colour.
 */
static inline int line_colour(int c0, i64 g12, int steps, i64 num, i64 den)
{
    i64 v = ((i64)c0 << GFRAC) + ((i64)1 << (GFRAC - 1));

    if (den > 0 && num > 0)
        v += floor_div64(g12 * steps * (num < den ? num : den), den);
    /* v >= 2^(GFRAC - 1): g12 * steps is truncated toward zero from
       (c1 - c0) 2^GFRAC, so v is at least min(c0, c1) 2^GFRAC plus the
       half. */
    return (int)(v >> GFRAC);
}

/*
 * The pixel nearest to c = a + (b - a) * num / den (1/256 scaled pixels,
 * num clamped to [0, den]), from the exact quotient: an exact half goes to
 * the larger pixel if half_up, else the smaller. |a|, |b| < 2^22 and den <
 * 2^15, so the products fit.
 */
static inline int nearest(i64 a, i64 b, i64 num, i64 den, int half_up)
{
    i64 c; /* c * den */

    if (den <= 0 || num <= 0)
        num = 0, den = 1;
    else if (num > den)
        num = den;
    c = a * den + (b - a) * num;
    return half_up ? (int)floor_div64(c + 128 * den, 256 * den)
                   : (int)ceil_div64(c - 128 * den, 256 * den);
}

static void hi_line(const jbi_prim *p, const jbi_rows *rows, const jbi_hires *h)
{
    const jbi_vertex *a = &p->u.line.v[0], *b = &p->u.line.v[1];
    int n = h->scale;
    int adx = a->x > b->x ? a->x - b->x : b->x - a->x;
    int ady = a->y > b->y ? a->y - b->y : b->y - a->y;
    int gouraud = (p->flags & JBI_F_GOURAUD) != 0;
    int dither = h->dither && p->dither; /* flat lines too, as at 1x */
    i64 P0[2], P1[2], dmaj, dmin, g12[3];
    int c0[3], c1[3], major, cx0, cx1, cy0, cy1;
    int ms, me, len, m, steps, k, swapped;

    if (adx > 1023 || ady > 511)
        return;
    P0[0] = (i64)p->u.line.qx[0] * n;
    P0[1] = (i64)p->u.line.qy[0] * n;
    P1[0] = (i64)p->u.line.qx[1] * n;
    P1[1] = (i64)p->u.line.qy[1] * n;
    c0[0] = a->r, c0[1] = a->g, c0[2] = a->b;
    c1[0] = gouraud ? b->r : a->r;
    c1[1] = gouraud ? b->g : a->g;
    c1[2] = gouraud ? b->b : a->b;
    dmaj = P1[0] - P0[0];
    dmin = P1[1] - P0[1];
    major = (dmaj < 0 ? -dmaj : dmaj) > (dmin < 0 ? -dmin : dmin) ? 0 : 1;
    /* The colour steps of the 1x line (raster.c), from its first vertex. */
    steps = adx > ady ? adx : ady;
    for (k = 0; k < 3; ++k)
        g12[k] = steps > 0 ? (i64)(c1[k] - c0[k]) * ((i64)1 << GFRAC) / steps : 0;
    swapped = P1[major] < P0[major];
    if (swapped) {
        for (k = 0; k < 2; ++k) {
            i64 tq = P0[k];
            P0[k] = P1[k];
            P1[k] = tq;
        }
    }
    cx0 = p->clip_x0 * n;
    cx1 = (p->clip_x1 + 1) * n;
    cy0 = p->clip_y0 * n;
    cy1 = (p->clip_y1 + 1) * n;

    /* Major-axis pixels [ms, me]: the end points' N-pixel blocks and those
       between. The minor coordinate and colour run from the first block's
       centre to the last's. */
    ms = (int)floor_div64(P0[major] + 128, 256);
    me = (int)floor_div64(P1[major] + 128, 256) + n - 1;
    len = me - ms - (n - 1);
    for (m = ms; m <= me; ++m) {
        i64 num = 2 * (i64)(m - ms) - (n - 1), den = 2 * (i64)len;
        /* The band's first minor pixel, the nearest to the minor
           coordinate there. An exact half goes as at 1x (raster.c): to the
           larger row along x, the smaller column along y. */
        int lo = nearest(P0[major ^ 1], P1[major ^ 1], num, den, major == 0);
        /* The colour from the line's first vertex, which is the scaled
           line's last when it runs the other way. */
        i64 from = swapped ? den - (num < 0 ? 0 : num > den ? den : num) : num;
        int cr = line_colour(c0[0], g12[0], steps, from, den);
        int cg = line_colour(c0[1], g12[1], steps, from, den);
        int cb = line_colour(c0[2], g12[2], steps, from, den);

        if (major == 0) {
            if (m < cx0 || m >= cx1)
                continue;
            for (k = lo; k < lo + n; ++k) {
                if (k < cy0 || k >= cy1 || !jbi_rows_owns(rows, k))
                    continue;
                hi_plot(p, h, m, k, cr, cg, cb, dither);
            }
        } else {
            if (m < cy0 || m >= cy1 || !jbi_rows_owns(rows, m))
                continue;
            for (k = lo; k < lo + n; ++k) {
                if (k < cx0 || k >= cx1)
                    continue;
                hi_plot(p, h, k, m, cr, cg, cb, dither);
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Fills */

static void hi_fill(const jbi_prim *p, const jbi_rows *rows, const jbi_hires *h)
{
    int n = h->scale;
    int x = p->u.fill.x, y = p->u.fill.y, w = p->u.fill.w, hh = p->u.fill.h;
    int w0 = x + w > JBI_VRAM_W ? JBI_VRAM_W - x : w;
    uint16_t c = p->u.fill.color;
    int j, s, i;

    for (j = 0; j < hh; ++j) {
        int yy = (y + j) & (JBI_VRAM_H - 1);
        for (s = 0; s < n; ++s) {
            int row = yy * n + s;
            uint16_t *d;

            if (!jbi_rows_owns(rows, row))
                continue;
            d = h->pix + (size_t)row * (size_t)h->width;
            for (i = x * n; i < (x + w0) * n; ++i)
                d[i] = c;
            for (i = 0; i < (w - w0) * n; ++i)
                d[i] = c;
        }
    }
}

void jbi_raster_prim_hires(const jbi_prim *p, const jbi_rows *rows, const jbi_hires *h)
{
    switch (p->kind) {
    case JBI_PRIM_TRI:
        hi_triangle(p, rows, h);
        break;
    case JBI_PRIM_RECT:
        hi_rect(p, rows, h);
        break;
    case JBI_PRIM_LINE:
        hi_line(p, rows, h);
        break;
    default:
        hi_fill(p, rows, h);
        break;
    }
}

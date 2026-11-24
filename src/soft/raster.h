/*
 * The software backend's rasterisers (raster.c at 1x, raster_hires.c
 * scaled) and what they share: the rows a worker owns, the targets they
 * draw into, perspective-correct texturing, blending and dithering.
 *
 * Each rasteriser draws only the rows a worker owns (jbi_rows); every
 * row's values are computed from the primitive alone, so the result is the
 * same however the rows are shared out (soft.c).
 */
#ifndef JBI_SOFT_RASTER_H
#define JBI_SOFT_RASTER_H

#include <stdint.h>

#include "core/prim.h"

/*
 * The rows one rasterising worker owns: those whose band (row >> shift)
 * is `index` modulo `count`. count 1 owns every row.
 */
typedef struct jbi_rows {
    int shift, count, index;
} jbi_rows;

static inline int jbi_rows_owns(const jbi_rows *rs, int y)
{
    return rs->count == 1 || ((y >> rs->shift) % rs->count) == rs->index;
}

/* The first owned row at or after y (y >= 0). */
static inline int jbi_rows_next(const jbi_rows *rs, int y)
{
    int band, k;

    if (rs->count == 1)
        return y;
    band = y >> rs->shift;
    k = (rs->index - band % rs->count + rs->count) % rs->count;
    if (k == 0)
        return y;
    return (band + k) << rs->shift;
}

/* The 1x pass's target and options. */
typedef struct jbi_target {
    uint16_t *vram;   /* 1024 x 512, row-major */
    int dither_off;   /* dithering off at 1x (jbi_config.dither 0 at scale 1) */
    int persp;        /* draw JBI_F_PERSP triangles perspective-correct: this
                         pass is the one displayed (no scaled VRAM) */
    int persp_span;   /* pixels between exact perspective values */
} jbi_target;

/* The scaled pass's target and options. */
typedef struct jbi_hires {
    uint16_t *pix;    /* 1024N x 512N */
    int scale;        /* N */
    int width;        /* 1024N */
    int dither;       /* scaled dithering in this pass */
    const uint8_t *dither_col; /* (x / N) & 3 for every scaled column */
    const uint16_t *vram; /* the 1x VRAM: 4- and 8-bit texels, CLUTs */
    int persp_span;   /* pixels between exact perspective values */
} jbi_hires;

/* 1x pass (raster.c): exactly the console's rasterisation. */
void jbi_raster_prim(const jbi_prim *p, const jbi_rows *rows, const jbi_target *t);
/* Scaled pass (raster_hires.c). */
void jbi_raster_prim_hires(const jbi_prim *p, const jbi_rows *rows, const jbi_hires *h);

/* ------------------------------------------------------------------------ */
/*
 * Perspective-correct texturing (jbi_set_texture_mapping,
 * src/core/context.h).
 *
 * The pass that is displayed draws a JBI_F_PERSP triangle (prim.h) with
 * u / z, v / z, 1 / z, and r, g, b over z, interpolated as planes in screen
 * space and divided per pixel, so texture and colour both follow the
 * surface; the 1x pass beside a scaled one stays affine, as the console's.
 * Values are clamped to the vertices' range, the texture coordinates to
 * their bounding box, so rounding cannot sample a neighbouring texture.
 * Coverage, texel fetches, the texture window, CLUTs, modulation, dithering
 * and semi-transparency are those of the affine path.
 *
 * Exact values are computed every persp_span pixels along a row (8 by
 * default in the scaled pass, 4 at 1x; jbi_config), from the first pixel of
 * the span, and interpolated linearly in between (1: every pixel). That
 * changes few pixels against dividing at every pixel, which costs about
 * twice the affine rasterisation. Each row depends only on the primitive,
 * so the result does not depend on the number of threads.
 */

enum { JBI_PERSP_ATTRS = 5 }; /* u, v, r, g, b */

typedef struct jbi_persp {
    /* Planes of w = zmin / z and of each attribute times w, as
       a + dx * x + dy * y in the pass's pixel coordinates. */
    double a[JBI_PERSP_ATTRS + 1], dx[JBI_PERSP_ATTRS + 1], dy[JBI_PERSP_ATTRS + 1];
    double lo[JBI_PERSP_ATTRS], hi[JBI_PERSP_ATTRS];
    int count; /* attributes in use: 2 (u, v) or 5 (with colour) */
} jbi_persp;

/* Vertex i's attribute k of jbi_persp's planes: 0 is 1 (for w), then u,
   v, r, g, b, from the triangle's perspective attributes (prim.h,
   u.tri.pattr). */
static inline int jbi_persp_attr(const jbi_prim *p, int i, int k)
{
    return k == 0 ? 1 : p->u.tri.pattr[i][k - 1];
}

/* Sets up the planes for triangle p with vertex positions X, Y (pixels of
   the pass). Returns 0 if they are degenerate. */
static inline int jbi_persp_setup(jbi_persp *pp, const jbi_prim *p, const double *X,
                                  const double *Y, int gouraud)
{
    const int *z = p->u.tri.z;
    double area, e1x, e1y, e2x, e2y, w[3], val[3];
    int zmin = z[0], i, k;

    e1x = X[1] - X[0];
    e1y = Y[1] - Y[0];
    e2x = X[2] - X[0];
    e2y = Y[2] - Y[0];
    area = e1x * e2y - e2x * e1y;
    if (area == 0)
        return 0;
    if (z[1] < zmin)
        zmin = z[1];
    if (z[2] < zmin)
        zmin = z[2];
    for (i = 0; i < 3; ++i)
        w[i] = (double)zmin / z[i];
    pp->count = gouraud ? 5 : 2;
    for (k = 0; k <= pp->count; ++k) {
        for (i = 0; i < 3; ++i)
            val[i] = jbi_persp_attr(p, i, k) * w[i];
        pp->dx[k] = ((val[1] - val[0]) * e2y - (val[2] - val[0]) * e1y) / area;
        pp->dy[k] = ((val[2] - val[0]) * e1x - (val[1] - val[0]) * e2x) / area;
        pp->a[k] = val[0] - pp->dx[k] * X[0] - pp->dy[k] * Y[0];
        if (k > 0) {
            int lo = jbi_persp_attr(p, 0, k), hi = lo;
            for (i = 1; i < 3; ++i) {
                int a = jbi_persp_attr(p, i, k);
                lo = a < lo ? a : lo;
                hi = a > hi ? a : hi;
            }
            pp->lo[k - 1] = lo;
            pp->hi[k - 1] = hi;
        }
    }
    return 1;
}

/* The planes at the start of row y: row[k] + dx[k] * x is the value at x.
   (w, row[0], is written outside the loop only so that compilers see it is
   always written.) */
static inline void jbi_persp_row(const jbi_persp *pp, double y, double *row)
{
    int k;

    row[0] = pp->a[0] + pp->dy[0] * y;
    for (k = 1; k <= pp->count; ++k)
        row[k] = pp->a[k] + pp->dy[k] * y;
}

/*
 * The attributes at x on a row, in fixed point with `frac` fractional bits,
 * each clamped to its range. bias is added before clamping, as the affine
 * paths add theirs, so that values on an integer do not truncate below it.
 * out[0..1] are u and v, out[2..4] r, g and b when in use.
 */
static inline void jbi_persp_at(const jbi_persp *pp, const double *row, double x, int frac,
                                double bias, long long *out)
{
    double w = row[0] + pp->dx[0] * x, inv, one = (double)(1ll << frac);
    int k;

    /* w > 0 inside the triangle; anything else is rounding at a sliver
       and clamps to the range below. */
    inv = w > 1e-30 ? 1.0 / w : 0.0;
    for (k = 0; k < pp->count; ++k) {
        double a = (row[k + 1] + pp->dx[k + 1] * x) * inv + bias;
        if (!(a >= pp->lo[k])) /* also NaN */
            a = pp->lo[k];
        if (a > pp->hi[k])
            a = pp->hi[k];
        out[k] = (long long)(a * one);
    }
}

/* Shared pixel helpers. */
static inline uint32_t jbi_blend(uint32_t back, int r, int g, int b, int mode)
{
    int br = back & 31, bg = (back >> 5) & 31, bb = (back >> 10) & 31;

    switch (mode) {
    case 0:
        r = (br + r) >> 1;
        g = (bg + g) >> 1;
        b = (bb + b) >> 1;
        break;
    case 1:
        r = br + r > 31 ? 31 : br + r;
        g = bg + g > 31 ? 31 : bg + g;
        b = bb + b > 31 ? 31 : bb + b;
        break;
    case 2:
        r = br - r < 0 ? 0 : br - r;
        g = bg - g < 0 ? 0 : bg - g;
        b = bb - b < 0 ? 0 : bb - b;
        break;
    default:
        r = br + (r >> 2) > 31 ? 31 : br + (r >> 2);
        g = bg + (g >> 2) > 31 ? 31 : bg + (g >> 2);
        b = bb + (b >> 2) > 31 ? 31 : bb + (b >> 2);
        break;
    }
    return (uint32_t)r | (uint32_t)g << 5 | (uint32_t)b << 10;
}

extern const signed char jbi_dither_matrix[4][4];

#endif

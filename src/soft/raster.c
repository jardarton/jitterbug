/*
 * Software rasterizer with PlayStation GPU semantics.
 *
 * - Vertices are integers; there is no sub-pixel precision. A pixel (x, y)
 *   is sampled at its integer coordinate. Triangles include their left and
 *   top edges and exclude the right and bottom ones, so a quad from (0, 0)
 *   to (16, 16) fills 16x16 pixels and shared edges are drawn once.
 * - Colours and texture coordinates are interpolated affinely over each
 *   triangle as the console does (fitted to its dumps; see gradient()):
 *   each attribute's x and y gradients have 12 fractional bits, truncated
 *   toward zero; the value at a pixel is the first vertex's plus half a
 *   unit plus the gradients times the pixel's offset from that vertex, and
 *   its integer part is used.
 * - Textures are 4-bit or 8-bit indices into a CLUT, or direct 15-bit
 *   texels, read from the texture page in VRAM through the texture window.
 *   The CLUT's entries come from the palette the CLUT cache fixed for the
 *   primitive (jbi_tex_source.clut, src/soft/soft.c), or with the cache
 *   off from VRAM.
 *   Texel 0x0000 is transparent. A texel's bit 15 selects semi-transparency
 *   (for semi-transparent primitives) and is copied to the mask bit.
 * - Modulated texels are texel * colour / 128 per component, saturating.
 * - Dithering adds the 4x4 matrix offset to 8-bit colours before they are
 *   reduced to 5 bits; it applies to gouraud-shaded and modulated textured
 *   polygons and to every line, flat or gouraud (psx-spx, Line Commands),
 *   never to rectangles.
 * - Semi-transparency: B/2 + F/2, B + F, B - F, B + F/4, on 5-bit
 *   components with saturation.
 * - Mask: with check_mask, pixels whose bit 15 is set are not overwritten;
 *   set_mask forces bit 15 in every pixel drawn.
 *
 * The polygon span loop is specialised per texture depth and mode bits, so
 * the inner loop has no per-pixel dispatch.
 *
 * With perspective-correct texturing and no scaled pass (jbi_target), textured
 * triangles whose vertices have depths (JBI_F_PERSP, prim.h) are drawn
 * perspective-correct instead (persp_span); everything else stays as above.
 *
 * This is the 1x pass. Primitives carry their drawing state (jbi_prim), and
 * each function draws only the rows a worker owns (jbi_rows); every row's
 * values are computed from the primitive alone, so the result is the same
 * however the rows are shared out. raster_hires.c is the scaled pass.
 */
#include "soft/raster.h"

#include <stddef.h>

typedef int64_t i64;

#define FRAC 32
#define ONE ((i64)1 << FRAC)
/* The console's interpolators: gradients with GFRAC fractional bits, and
   half a unit added to the start value (gradient(), raster_line()). In
   FRAC fixed point every value is then a multiple of 2^(FRAC - GFRAC), so
   stepping is exact. */
#define GFRAC 12
#define GSCALE ((i64)1 << (FRAC - GFRAC))
#define HALF (ONE >> 1)

const signed char jbi_dither_matrix[4][4] = {
    {-4, 0, -3, 1},
    {2, -2, 3, -1},
    {-3, 1, -4, 0},
    {3, -1, 2, -2},
};

static inline int clamp255(int c)
{
    return c < 0 ? 0 : c > 255 ? 255 : c;
}

static inline int min31(int c)
{
    return c > 31 ? 31 : c;
}

#define blend jbi_blend

/* ------------------------------------------------------------------------ */
/* Texture access */

/* Row y of the 1x VRAM. */
#define VROW(vram, y) ((vram) + (size_t)(y) * JBI_VRAM_W)

/* Depth is 4, 8 or 15. u and v are taken modulo 256 before windowing. */
static inline __attribute__((always_inline)) uint32_t
tex_fetch(const jbi_tex_source *t, const uint16_t *vram, uint32_t u, uint32_t v, const int depth)
{
    const uint16_t *row;

    u = ((u & 0xff) & t->and_x) | t->or_x;
    v = ((v & 0xff) & t->and_y) | t->or_y;
    row = VROW(vram, (t->tpy + (int)v) & (JBI_VRAM_H - 1));
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

/* ------------------------------------------------------------------------ */
/* Polygons */

typedef struct {
    /* Attribute values at the span's first pixel, FRAC fixed point. */
    i64 r, g, b, u, v;
    /* Per-pixel steps. */
    i64 drdx, dgdx, dbdx, dudx, dvdx;
    int fr, fg, fb; /* flat colour */
    jbi_tex_source tex;
    int semi_mode;
    int check_mask;
    uint32_t set_mask;
    uint16_t *vram; /* the target's */
} Span;

/*
 * One span of pixels [x0, x1) on row y. The constant arguments select the
 * variant; every call site passes literals, so each instantiation below is
 * a branch-free loop on them.
 */
static inline __attribute__((always_inline)) void
span_impl(Span *s, int y, int x0, int x1, const int depth, const int gouraud,
          const int raw, const int semi, const int dither)
{
    uint16_t *vram = s->vram, *row = VROW(vram, y);
    const signed char *dm = jbi_dither_matrix[y & 3];
    i64 r = s->r, g = s->g, b = s->b, u = s->u, v = s->v;
    int x;

    for (x = x0; x < x1; ++x) {
        uint16_t *dst = &row[x];
        uint32_t out, stp = 0;
        int cr, cg, cb;

        if (s->check_mask && (*dst & 0x8000))
            goto next;
        if (gouraud) {
            cr = (int)(r >> FRAC);
            cg = (int)(g >> FRAC);
            cb = (int)(b >> FRAC);
        } else {
            cr = s->fr;
            cg = s->fg;
            cb = s->fb;
        }
        if (depth) {
            uint32_t texel = tex_fetch(&s->tex, vram, (uint32_t)(u >> FRAC), (uint32_t)(v >> FRAC),
                                       depth);
            int tr, tg, tb;

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
                int d = dm[x & 3];
                cr = clamp255(((tr * cr) >> 4) + d) >> 3;
                cg = clamp255(((tg * cg) >> 4) + d) >> 3;
                cb = clamp255(((tb * cb) >> 4) + d) >> 3;
            } else {
                cr = min31((tr * cr) >> 7);
                cg = min31((tg * cg) >> 7);
                cb = min31((tb * cb) >> 7);
            }
        } else if (dither) {
            int d = dm[x & 3];
            cr = clamp255(cr + d) >> 3;
            cg = clamp255(cg + d) >> 3;
            cb = clamp255(cb + d) >> 3;
        } else {
            cr >>= 3;
            cg >>= 3;
            cb >>= 3;
        }
        if (semi && (!depth || stp))
            out = blend(*dst, cr, cg, cb, s->semi_mode);
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

#define SPAN_FN(D, G, R, S, T)                                                \
    static void span_##D##_##G##R##S##T(Span *s, int y, int x0, int x1)       \
    {                                                                         \
        span_impl(s, y, x0, x1, D, G, R, S, T);                               \
    }
#define SPAN_FNS_D(D)                                                         \
    SPAN_FN(D, 0, 0, 0, 0) SPAN_FN(D, 0, 0, 0, 1) SPAN_FN(D, 0, 0, 1, 0)       \
    SPAN_FN(D, 0, 0, 1, 1) SPAN_FN(D, 0, 1, 0, 0) SPAN_FN(D, 0, 1, 0, 1)       \
    SPAN_FN(D, 0, 1, 1, 0) SPAN_FN(D, 0, 1, 1, 1) SPAN_FN(D, 1, 0, 0, 0)       \
    SPAN_FN(D, 1, 0, 0, 1) SPAN_FN(D, 1, 0, 1, 0) SPAN_FN(D, 1, 0, 1, 1)       \
    SPAN_FN(D, 1, 1, 0, 0) SPAN_FN(D, 1, 1, 0, 1) SPAN_FN(D, 1, 1, 1, 0)       \
    SPAN_FN(D, 1, 1, 1, 1)
SPAN_FNS_D(0)
SPAN_FNS_D(4)
SPAN_FNS_D(8)
SPAN_FNS_D(15)

typedef void (*SpanFn)(Span *s, int y, int x0, int x1);

static inline int floor_div(int n, int d) /* d > 0 */
{
    int q = n / d;
    return (n % d != 0 && n < 0) ? q - 1 : q;
}

static inline int ceil_div(int n, int d) /* d > 0 */
{
    return -floor_div(-n, d);
}

/* First pixel column at or right of the edge a-b at row y (ya <= y <= yb). */
static inline int edge_x(const jbi_vertex *a, const jbi_vertex *b, int y)
{
    int dy = b->y - a->y;
    return ceil_div(a->x * dy + (y - a->y) * (b->x - a->x), dy);
}

/*
 * Gradient of attribute values a0..a2 in FRAC fixed point: the console's,
 * with GFRAC (12) fractional bits, the quotient truncated toward zero (C's
 * division), then scaled to FRAC. Fitted to ps1-tests' dumps, not taken
 * from psx-spx, which does not give the precision: with the start value
 * a0 + 1/2 (raster_triangle), it reproduces every pixel of triangle's three
 * Gouraud triangles (157440 pixels, with and without dithering) and of
 * uv-interpolation's 512 Gouraud quads (65280) and 256 textured quads (u,
 * 32640), and no other choice tried does (11 or 13 bits, rounding or
 * flooring the quotient, other start biases, another reference vertex:
 * docs/conformance.md, "`triangle` and `uv-interpolation`: polygon
 * attribute interpolation"). The numerators can be negative, so they are
 * scaled by multiplying (a left shift of a negative value is undefined);
 * |ax|, |ay| < 2^20, so the products fit.
 */
static inline void gradient(int a0, int a1, int a2, const jbi_vertex *v0,
                            const jbi_vertex *v1, const jbi_vertex *v2, int area,
                            i64 *ddx, i64 *ddy)
{
    i64 ax = (i64)(a1 - a0) * (v2->y - v0->y) - (i64)(a2 - a0) * (v1->y - v0->y);
    i64 ay = (i64)(a2 - a0) * (v1->x - v0->x) - (i64)(a1 - a0) * (v2->x - v0->x);
    *ddx = ax * ((i64)1 << GFRAC) / area * GSCALE;
    *ddy = ay * ((i64)1 << GFRAC) / area * GSCALE;
}

/* A small bias for perspective values, so that values on an integer do
   not truncate below it. */
#define PBIAS (1.0 / 65536)

/*
 * Pixels [xl, xr) of row y of a perspective-correct triangle: the exact
 * attributes every len pixels from xl, and the affine span function fn in
 * between, stepping linearly to the next exact value (to the last pixel's,
 * for the last piece). One copy for every variant (tri_rows passes fn).
 */
static __attribute__((noinline)) void persp_span(Span *s, SpanFn fn, const jbi_persp *pp, int len,
                                                 int y, int xl, int xr)
{
    double row[JBI_PERSP_ATTRS + 1];
    long long a[JBI_PERSP_ATTRS], b[JBI_PERSP_ATTRS];
    int x0, x1, e, k, n = pp->count;

    jbi_persp_row(pp, y, row);
    jbi_persp_at(pp, row, xl, FRAC, PBIAS, a);
    for (x0 = xl; x0 < xr; x0 = x1) {
        i64 step[JBI_PERSP_ATTRS] = {0};

        x1 = xr - x0 > len ? x0 + len : xr;
        e = x1 < xr ? x1 : xr - 1;
        if (e > x0) {
            jbi_persp_at(pp, row, e, FRAC, PBIAS, b);
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
        fn(s, y, x0, x1);
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

/* What a triangle's rows need once it is set up (raster_triangle). */
typedef struct {
    const jbi_prim *p;
    const jbi_vertex *v0, *top, *mid, *bot;
    int cross, y, y_end, gouraud, textured, persp, persp_span;
    i64 r0, g0, b0, u0, v0f;
    i64 drdy, dgdy, dbdy, dudy, dvdy;
    jbi_persp pp;
} Tri;

/*
 * The rows of a triangle the worker owns, each drawn by span function fn.
 * Inlined once per variant (raster_triangle), so fn is a direct call.
 */
static inline __attribute__((always_inline)) void
tri_rows(Tri *t, Span *s, const jbi_rows *rows, SpanFn fn)
{
    const jbi_prim *p = t->p;
    const jbi_vertex *v0 = t->v0, *top = t->top, *mid = t->mid, *bot = t->bot;
    int y;

    for (y = t->y; y < t->y_end; ++y) {
        int xl, xr;
        i64 dx, dy;

        if (!jbi_rows_owns(rows, y)) {
            y = jbi_rows_next(rows, y) - 1;
            continue;
        }
        xl = edge_x(top, bot, y);
        xr = y < mid->y ? edge_x(top, mid, y) : edge_x(mid, bot, y);
        if (t->cross < 0) {
            int tmp = xl;
            xl = xr;
            xr = tmp;
        }
        if (xl < p->clip_x0)
            xl = p->clip_x0;
        if (xr > p->clip_x1 + 1)
            xr = p->clip_x1 + 1;
        if (xl >= xr)
            continue;
        if (t->persp) {
            persp_span(s, fn, &t->pp, t->persp_span, y, xl, xr);
            continue;
        }
        dx = xl - v0->x;
        dy = y - v0->y;
        if (t->gouraud) {
            s->r = t->r0 + s->drdx * dx + t->drdy * dy;
            s->g = t->g0 + s->dgdx * dx + t->dgdy * dy;
            s->b = t->b0 + s->dbdx * dx + t->dbdy * dy;
        }
        if (t->textured) {
            s->u = t->u0 + s->dudx * dx + t->dudy * dy;
            s->v = t->v0f + s->dvdx * dx + t->dvdy * dy;
        }
        fn(s, y, xl, xr);
    }
}

/*
 * The variant for sel = depth index (0 untextured, 1 4-bit, 2 8-bit, 3
 * 15-bit) * 16 + (gouraud raw semi dither): a switch over tri_rows inlined
 * with each span function, rather than a table of function pointers (which
 * would be relocated data), and with the choice made once per triangle.
 */
#define TRI_CASES(I, D)                                                       \
    case I * 16 + 0: tri_rows(t, s, rows, span_##D##_0000); return;           \
    case I * 16 + 1: tri_rows(t, s, rows, span_##D##_0001); return;           \
    case I * 16 + 2: tri_rows(t, s, rows, span_##D##_0010); return;           \
    case I * 16 + 3: tri_rows(t, s, rows, span_##D##_0011); return;           \
    case I * 16 + 4: tri_rows(t, s, rows, span_##D##_0100); return;           \
    case I * 16 + 5: tri_rows(t, s, rows, span_##D##_0101); return;           \
    case I * 16 + 6: tri_rows(t, s, rows, span_##D##_0110); return;           \
    case I * 16 + 7: tri_rows(t, s, rows, span_##D##_0111); return;           \
    case I * 16 + 8: tri_rows(t, s, rows, span_##D##_1000); return;           \
    case I * 16 + 9: tri_rows(t, s, rows, span_##D##_1001); return;           \
    case I * 16 + 10: tri_rows(t, s, rows, span_##D##_1010); return;          \
    case I * 16 + 11: tri_rows(t, s, rows, span_##D##_1011); return;          \
    case I * 16 + 12: tri_rows(t, s, rows, span_##D##_1100); return;          \
    case I * 16 + 13: tri_rows(t, s, rows, span_##D##_1101); return;          \
    case I * 16 + 14: tri_rows(t, s, rows, span_##D##_1110); return;          \
    case I * 16 + 15: tri_rows(t, s, rows, span_##D##_1111); return;
static void tri_variant(int sel, Tri *t, Span *s, const jbi_rows *rows)
{
    switch (sel) {
        TRI_CASES(0, 0)
        TRI_CASES(1, 4)
        TRI_CASES(2, 8)
        TRI_CASES(3, 15)
    default:
        return;
    }
}

static void raster_triangle(const jbi_prim *p, const jbi_rows *rows, const jbi_target *target)
{
    const jbi_vertex *v0 = &p->u.tri.v[0], *v1 = &p->u.tri.v[1], *v2 = &p->u.tri.v[2];
    const jbi_vertex *top, *mid, *bot, *tmp;
    int flags = p->flags;
    int gouraud = (flags & JBI_F_GOURAUD) != 0;
    int textured = (flags & JBI_F_TEXTURED) != 0;
    int raw = textured && (flags & JBI_F_RAW);
    int dither, depth_index, area, sel, flat[3];
    Tri t;
    Span s;

    if (flags & JBI_F_NO_1X)
        return; /* drawn in the scaled pass only (prim.h) */
    if (too_far(v0, v1) || too_far(v1, v2) || too_far(v2, v0))
        return;
    area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    if (area == 0)
        return;

    /* Gouraud with equal vertex colours renders as flat (still dithered);
       raw textures ignore colour. */
    if (raw)
        gouraud = 0;
    dither = p->dither && !target->dither_off && (gouraud || (textured && !raw));

    /* Perspective-correct only where this pass is the one displayed. Its
       colours are the perspective attributes (prim.h), so whether it is
       flat, and its colour if so, come from those; the affine pass, and a
       perspective set-up that fails, use the vertices'. */
    t.persp = 0;
    flat[0] = v0->r;
    flat[1] = v0->g;
    flat[2] = v0->b;
    if (textured && (flags & JBI_F_PERSP) && target->persp) {
        double X[3] = {v0->x, v1->x, v2->x}, Y[3] = {v0->y, v1->y, v2->y};
        const uint8_t(*pa)[5] = p->u.tri.pattr;
        int pg = gouraud && !(pa[0][2] == pa[1][2] && pa[0][2] == pa[2][2] &&
                              pa[0][3] == pa[1][3] && pa[0][3] == pa[2][3] &&
                              pa[0][4] == pa[1][4] && pa[0][4] == pa[2][4]);

        t.persp = jbi_persp_setup(&t.pp, p, X, Y, pg);
        if (t.persp) {
            gouraud = pg;
            flat[0] = pa[0][2];
            flat[1] = pa[0][3];
            flat[2] = pa[0][4];
        }
    }
    if (!t.persp && gouraud && v0->r == v1->r && v0->r == v2->r && v0->g == v1->g &&
        v0->g == v2->g && v0->b == v1->b && v0->b == v2->b)
        gouraud = 0;

    t.drdy = t.dgdy = t.dbdy = t.dudy = t.dvdy = 0;
    s.fr = flat[0];
    s.fg = flat[1];
    s.fb = flat[2];
    s.drdx = s.dgdx = s.dbdx = s.dudx = s.dvdx = 0;
    if (gouraud) {
        gradient(v0->r, v1->r, v2->r, v0, v1, v2, area, &s.drdx, &t.drdy);
        gradient(v0->g, v1->g, v2->g, v0, v1, v2, area, &s.dgdx, &t.dgdy);
        gradient(v0->b, v1->b, v2->b, v0, v1, v2, area, &s.dbdx, &t.dbdy);
    }
    if (textured) {
        gradient(v0->u, v1->u, v2->u, v0, v1, v2, area, &s.dudx, &t.dudy);
        gradient(v0->v, v1->v, v2->v, v0, v1, v2, area, &s.dvdx, &t.dvdy);
        s.tex = p->tex;
    }
    /* The values at vertex 0, plus half a unit (gradient()). */
    t.r0 = ((i64)v0->r << FRAC) + HALF;
    t.g0 = ((i64)v0->g << FRAC) + HALF;
    t.b0 = ((i64)v0->b << FRAC) + HALF;
    t.u0 = ((i64)v0->u << FRAC) + HALF;
    t.v0f = ((i64)v0->v << FRAC) + HALF;
    s.semi_mode = p->semi_mode;
    s.check_mask = p->check_mask;
    s.set_mask = p->set_mask;
    s.vram = target->vram;

    depth_index = 0;
    if (textured)
        depth_index = p->depth == 4 ? 1 : p->depth == 8 ? 2 : 3;
    sel = depth_index * 16 + ((gouraud << 3) | (raw << 2) | ((flags & JBI_F_SEMI) ? 2 : 0) |
                              dither);

    /* Sort by y. */
    top = v0;
    mid = v1;
    bot = v2;
    if (mid->y < top->y) {
        tmp = top; top = mid; mid = tmp;
    }
    if (bot->y < top->y) {
        tmp = top; top = bot; bot = tmp;
    }
    if (bot->y < mid->y) {
        tmp = mid; mid = bot; bot = tmp;
    }
    if (top->y == bot->y)
        return;
    t.p = p;
    t.v0 = v0;
    t.top = top;
    t.mid = mid;
    t.bot = bot;
    t.gouraud = gouraud;
    t.textured = textured;
    t.persp_span = target->persp_span;
    /* < 0: the middle vertex is left of the long edge top-bottom. */
    t.cross = (mid->x - top->x) * (bot->y - top->y) - (bot->x - top->x) * (mid->y - top->y);

    t.y = top->y < p->clip_y0 ? p->clip_y0 : top->y;
    t.y_end = bot->y > p->clip_y1 + 1 ? p->clip_y1 + 1 : bot->y;
    tri_variant(sel, &t, &s, rows);
}

/* ------------------------------------------------------------------------ */
/* Rectangles (TILE, SPRT): no shading, no dithering, texture coordinates
   step by one texel per pixel from the top-left corner. */

static void raster_rect(const jbi_prim *p, const jbi_rows *rows, const jbi_target *target)
{
    uint16_t *vram = target->vram;
    int x = p->u.rect.x, y = p->u.rect.y, u = p->u.rect.u, v = p->u.rect.v;
    int r = p->u.rect.r, g = p->u.rect.g, b = p->u.rect.b;
    int textured = (p->flags & JBI_F_TEXTURED) != 0;
    int raw = (p->flags & JBI_F_RAW) != 0;
    int semi = (p->flags & JBI_F_SEMI) != 0;
    int x0 = x, y0 = y, x1 = x + p->u.rect.w, y1 = y + p->u.rect.h;
    int du = p->flip_x ? -1 : 1, dv = p->flip_y ? -1 : 1;
    int depth = textured ? p->depth : 0;
    uint32_t flat = (uint32_t)(r >> 3) | (uint32_t)(g >> 3) << 5 | (uint32_t)(b >> 3) << 10;
    const jbi_tex_source *tex = &p->tex;
    int px, py;

    if (x0 < p->clip_x0)
        x0 = p->clip_x0;
    if (y0 < p->clip_y0)
        y0 = p->clip_y0;
    if (x1 > p->clip_x1 + 1)
        x1 = p->clip_x1 + 1;
    if (y1 > p->clip_y1 + 1)
        y1 = p->clip_y1 + 1;
    if (x0 >= x1 || y0 >= y1)
        return;

    for (py = y0; py < y1; ++py) {
        uint16_t *row;
        uint32_t tv;

        if (!jbi_rows_owns(rows, py)) {
            py = jbi_rows_next(rows, py) - 1;
            continue;
        }
        row = VROW(vram, py);
        tv = (uint32_t)(v + (py - y) * dv);
        for (px = x0; px < x1; ++px) {
            uint16_t *dst = &row[px];
            uint32_t texel = 0, stp = 0, out;
            int cr, cg, cb;

            if (p->check_mask && (*dst & 0x8000))
                continue;
            if (textured) {
                uint32_t tu = (uint32_t)(u + (px - x) * du);
                texel = depth == 4   ? tex_fetch(tex, vram, tu, tv, 4)
                        : depth == 8 ? tex_fetch(tex, vram, tu, tv, 8)
                                     : tex_fetch(tex, vram, tu, tv, 15);
                if (texel == 0)
                    continue;
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
                out = blend(*dst, cr, cg, cb, p->semi_mode);
            else
                out = (uint32_t)cr | (uint32_t)cg << 5 | (uint32_t)cb << 10;
            *dst = (uint16_t)(out | stp | p->set_mask);
        }
    }
}

/* ------------------------------------------------------------------------ */
/*
 * Lines: both end points drawn, one pixel per step along the major axis
 * (x when |dx| > |dy|, else y). At step i of n the minor coordinate is the
 * start's plus i * d / n rounded to the nearest integer (d the minor
 * difference), computed exactly. At an exact half, a y coordinate (lines
 * along x) rounds to the larger row and an x coordinate (lines along y) to
 * the smaller column: ps1-tests' lines dump has 34 such pixels in lines
 * going right and down along x, which take the larger y, and 25 in lines
 * going down and right along y, which take the smaller x; its 16-segment
 * circle and the other lines have none. That the rule is the same for
 * lines going left or up (so a line draws the same pixels in either
 * direction) is an assumption (docs/conformance.md, "Assumptions beyond the
 * evidence").
 */

/* The minor offset at step i: round(num / n), n > 0, halves up (y) or
   down (x). */
static inline int line_minor(int num, int n, int half_up)
{
    return floor_div(2 * num + n - (half_up ? 0 : 1), 2 * n);
}

static void raster_line(const jbi_prim *p, const jbi_rows *rows, const jbi_target *target)
{
    const jbi_vertex *a = &p->u.line.v[0], *b = &p->u.line.v[1];
    int flags = p->flags;
    int dx = b->x - a->x, dy = b->y - a->y;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    int gouraud = (flags & JBI_F_GOURAUD) != 0;
    int semi = (flags & JBI_F_SEMI) != 0;
    int dither = p->dither && !target->dither_off;
    int x_major = adx > ady, sgn = (x_major ? dx : dy) < 0 ? -1 : 1;
    i64 r, g, bl, sr = 0, sg = 0, sb = 0;
    int i;

    if (adx > 1023 || ady > 511)
        return;
    /* Colours as the console steps them: the first vertex's plus half a
       unit, and per step the difference over the steps with GFRAC
       fractional bits, truncated toward zero, as for polygons
       (gradient()). That reproduces every Gouraud pixel of ps1-tests'
       lines (with and without dithering, the polylines) but the 64 its
       program leaves undefined (tests/conformance/lines.exclude); 12 or
       more fractional bits all do, 11 or fewer do not, and the dump does not
       tell truncating from flooring a negative step. The differences can be
       negative, so they are scaled by multiplying. */
    r = ((i64)a->r << FRAC) + HALF;
    g = ((i64)a->g << FRAC) + HALF;
    bl = ((i64)a->b << FRAC) + HALF;
    if (steps > 0 && gouraud) {
        sr = (i64)(b->r - a->r) * ((i64)1 << GFRAC) / steps * GSCALE;
        sg = (i64)(b->g - a->g) * ((i64)1 << GFRAC) / steps * GSCALE;
        sb = (i64)(b->b - a->b) * ((i64)1 << GFRAC) / steps * GSCALE;
    }
    for (i = 0; i <= steps; ++i, r += sr, g += sg, bl += sb) {
        int px, py;
        int cr = (int)(r >> FRAC), cg = (int)(g >> FRAC), cb = (int)(bl >> FRAC);
        uint16_t *dst;
        uint32_t out;

        if (x_major) {
            px = a->x + sgn * i;
            py = a->y + line_minor(i * dy, steps, 1);
        } else if (steps > 0) {
            py = a->y + sgn * i;
            px = a->x + line_minor(i * dx, steps, 0);
        } else {
            px = a->x;
            py = a->y;
        }
        if (px < p->clip_x0 || px > p->clip_x1 || py < p->clip_y0 || py > p->clip_y1)
            continue;
        if (!jbi_rows_owns(rows, py))
            continue;
        dst = &VROW(target->vram, py)[px];
        if (p->check_mask && (*dst & 0x8000))
            continue;
        if (dither) {
            int d = jbi_dither_matrix[py & 3][px & 3];
            cr = clamp255(cr + d);
            cg = clamp255(cg + d);
            cb = clamp255(cb + d);
        }
        cr >>= 3;
        cg >>= 3;
        cb >>= 3;
        if (semi)
            out = blend(*dst, cr, cg, cb, p->semi_mode);
        else
            out = (uint32_t)cr | (uint32_t)cg << 5 | (uint32_t)cb << 10;
        *dst = (uint16_t)(out | p->set_mask);
    }
}

/* ------------------------------------------------------------------------ */
/* Fill (GP0(02h)): already aligned and wrapped by jbi_fill's rules. */

static void raster_fill(const jbi_prim *p, const jbi_rows *rows, const jbi_target *target)
{
    int x = p->u.fill.x, y = p->u.fill.y, w = p->u.fill.w, h = p->u.fill.h;
    uint16_t c = p->u.fill.color;
    int i, j;

    for (j = 0; j < h; ++j) {
        int yy = (y + j) & (JBI_VRAM_H - 1);
        uint16_t *row;

        if (!jbi_rows_owns(rows, yy))
            continue;
        row = VROW(target->vram, yy);
        for (i = 0; i < w; ++i)
            row[(x + i) & (JBI_VRAM_W - 1)] = c;
    }
}

void jbi_raster_prim(const jbi_prim *p, const jbi_rows *rows, const jbi_target *t)
{
    switch (p->kind) {
    case JBI_PRIM_TRI:
        raster_triangle(p, rows, t);
        break;
    case JBI_PRIM_RECT:
        raster_rect(p, rows, t);
        break;
    case JBI_PRIM_LINE:
        raster_line(p, rows, t);
        break;
    default:
        raster_fill(p, rows, t);
        break;
    }
}

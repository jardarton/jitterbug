/* Screen-space clipping (split.h). */
#include "geom/split.h"

#include <stddef.h>
#include <string.h>

/* __int128 is a GNU extension (src/soft/raster_hires.c uses it too); this
   only quiets -Wpedantic about it. */
__extension__ typedef __int128 i128;
typedef int64_t i64;
typedef jbi_split_vertex sv;

enum { ONE = 256, SLACK = 288, LIMIT_LO = -1024, LIMIT_HI = 1023 };

/* ------------------------------------------------------------------------ */
/* Arithmetic */

static int sign(i128 a)
{
    return a > 0 ? 1 : a < 0 ? -1 : 0;
}

/* floor(n / d), d > 0. */
static i128 floor_div(i128 n, i128 d)
{
    i128 q = n / d;

    return q * d > n ? q - 1 : q;
}

/* round(n / d), d > 0, halves away from zero: the attributes' rule. */
static i64 div_round(i128 n, i128 d)
{
    if (n >= 0)
        return (i64)((2 * n + d) / (2 * d));
    return -(i64)((-2 * n + d) / (2 * d));
}

/* round(n / d), d > 0, halves up: the positions' rule. */
static i128 round_up(i128 n, i128 d)
{
    return floor_div(2 * n + d, 2 * d);
}

/* The nearest pixel to a sub-pixel position, halves up, saturated to an
   int: a far position (a sprite's corner at an int position plus an int
   size) keeps its side of the view, never wrapping to the other. */
static int q_pixel_round(i64 q)
{
    i64 p = (q + ONE / 2) >> 8;

    return p > 0x7fffffff ? 0x7fffffff : p < -0x7fffffff ? -0x7fffffff : (int)p;
}

static int clamp_range(i64 a)
{
    return a < LIMIT_LO ? LIMIT_LO : a > LIMIT_HI ? LIMIT_HI : (int)a;
}

/* ------------------------------------------------------------------------ */
/* The limits */

static int in_range(const sv *a)
{
    return a->x >= -1024 && a->x <= 1023 && a->y >= -1024 && a->y <= 1023;
}

static int near(const sv *a, const sv *b)
{
    i64 dx = (i64)a->x - b->x, dy = (i64)a->y - b->y;

    return dx <= 1023 && dx >= -1023 && dy <= 511 && dy >= -511;
}

int jbi_split_triangle_fits(const sv *a, const sv *b, const sv *c)
{
    return in_range(a) && in_range(b) && in_range(c) && near(a, b) && near(b, c) && near(a, c);
}

/* ------------------------------------------------------------------------ */
/* Positions in a format: 0 the integer one (units of a pixel), 1 the
   sub-pixel one (units of 1/256 pixel). */

typedef struct {
    i64 x, y;
} pt;

static pt pos(const sv *v, int f)
{
    pt p;

    p.x = f ? v->qx : v->x;
    p.y = f ? v->qy : v->y;
    return p;
}

static int orient(pt a, pt b, pt c)
{
    return sign((i128)(b.x - a.x) * (c.y - a.y) - (i128)(c.x - a.x) * (b.y - a.y));
}

static i128 magnitude(i128 a)
{
    return a < 0 ? -a : a;
}

/* Whether an edge line of triangle t (orientation st, not 0) has all of
   the m points o on its outer side or on it. */
static int separated(const pt *t, int st, const pt *o, int m)
{
    int k, j;

    for (k = 0; k < 3; ++k) {
        int out = 1;

        for (j = 0; j < m && out; ++j)
            out = orient(t[k], t[(k + 1) % 3], o[j]) * st <= 0;
        if (out)
            return 1;
    }
    return 0;
}

/* Whether the interiors of triangles a and b overlap: exact, by the
   separating axis test (two triangles' interiors are disjoint exactly when
   an edge line of one has the other on its far side). A triangle with no
   area overlaps nothing. */
static int tri_overlap(const pt *a, const pt *b)
{
    int sa = orient(a[0], a[1], a[2]), sb = orient(b[0], b[1], b[2]);

    if (sa == 0 || sb == 0)
        return 0;
    return !separated(a, sa, b, 3) && !separated(b, sb, a, 3);
}

/* Whether triangle t draws nothing of the rectangle r (x0, x1, y0, y1, in
   the format's units: the view's samples lie in [x0, x1) x [y0, y1)): no
   area, or an edge line (or the rectangle's own) with the whole closed
   rectangle on its outer side. A sample on such a line belongs to the
   triangle only if its edge faces left or up, away from the rectangle,
   which it does not: the fill rule (left and top in, right and bottom
   out). */
static int tri_misses(const pt *t, const i64 *r)
{
    pt c[4];
    int s = orient(t[0], t[1], t[2]), k;
    i64 x0 = t[0].x, x1 = t[0].x, y0 = t[0].y, y1 = t[0].y;

    if (s == 0)
        return 1;
    for (k = 1; k < 3; ++k) {
        x0 = t[k].x < x0 ? t[k].x : x0;
        x1 = t[k].x > x1 ? t[k].x : x1;
        y0 = t[k].y < y0 ? t[k].y : y0;
        y1 = t[k].y > y1 ? t[k].y : y1;
    }
    if (x1 <= r[0] || x0 >= r[1] || y1 <= r[2] || y0 >= r[3])
        return 1;
    c[0].x = r[0], c[0].y = r[2];
    c[1].x = r[1], c[1].y = r[2];
    c[2].x = r[1], c[2].y = r[3];
    c[3].x = r[0], c[3].y = r[3];
    return separated(t, s, c, 4);
}

/* The view's samples in a format: [x0, x1 + 1) x [y0, y1 + 1). */
static void view_rect(const jbi_split_view *view, int f, i64 *r)
{
    i64 one = f ? ONE : 1;

    r[0] = (i64)view->x0 * one;
    r[1] = ((i64)view->x1 + 1) * one;
    r[2] = (i64)view->y0 * one;
    r[3] = ((i64)view->y1 + 1) * one;
}

/* ------------------------------------------------------------------------ */
/* Input vertices */

/* The perspective-correct attributes are the caller's (given) or start as
   the affine ones (split.h). */
static void take(sv *out, const sv *in, int given)
{
    *out = *in;
    if (!given) {
        out->pu = in->u;
        out->pv = in->v;
        out->pr = in->r;
        out->pg = in->g;
        out->pb = in->b;
    }
}

/* Integer positions within the slack of the sub-pixel ones (split.h). */
static void settle(sv *v)
{
    if (v->x * (i64)ONE - v->qx > SLACK || v->qx - v->x * (i64)ONE > SLACK)
        v->x = q_pixel_round(v->qx);
    if (v->y * (i64)ONE - v->qy > SLACK || v->qy - v->y * (i64)ONE > SLACK)
        v->y = q_pixel_round(v->qy);
}

/*
 * Whether a vertex's sub-pixel position is beyond the slack of its integer
 * one, in either coordinate, with the integer one within the GPU's range:
 * a vertex the decoder draws at its integer position in a polygon passed
 * through (an exact 3D face, as console code draws it), its sub-pixel one
 * ignored. A cut polygon's precise pieces must draw it there too
 * (jbi_split_polys, seam). An integer position beyond the range is a
 * stand-in (a far 2D corner saturated): there the sub-pixel one is the
 * position.
 */
static int seam_vertex(const sv *v)
{
    return (v->x * (i64)ONE - v->qx > SLACK || v->qx - v->x * (i64)ONE > SLACK ||
            v->y * (i64)ONE - v->qy > SLACK || v->qy - v->y * (i64)ONE > SLACK) &&
           v->x >= LIMIT_LO && v->x <= LIMIT_HI && v->y >= LIMIT_LO && v->y <= LIMIT_HI;
}

/* The triangles the GPU draws of polygon v (cyclic order, n vertices) when
   it is passed through: itself; a quad's two in Z order (cyclic 0 1 3 and
   1 3 2); else emit_fan's quads and triangle, each quad's two. Returns
   how many, at most 2 (n - 2), as index triples. */
static int gpu_triangles(int n, int (*t)[3])
{
    int i = 1, m = 0;

    if (n == 3) {
        t[0][0] = 0, t[0][1] = 1, t[0][2] = 2;
        return 1;
    }
    if (n == 4) {
        t[0][0] = 0, t[0][1] = 1, t[0][2] = 3;
        t[1][0] = 1, t[1][1] = 3, t[1][2] = 2;
        return 2;
    }
    while (i + 2 < n) { /* the quad p0 pi pi+2 pi+1 in Z order */
        t[m][0] = 0, t[m][1] = i, t[m][2] = i + 2;
        ++m;
        t[m][0] = i, t[m][1] = i + 2, t[m][2] = i + 1;
        ++m;
        i += 2;
    }
    if (i + 1 < n) {
        t[m][0] = 0, t[m][1] = i, t[m][2] = i + 1;
        ++m;
    }
    return m;
}

/* Whether polygon v draws nothing of the view in either format, as the
   GPU would draw it passed through (above). */
static int culled_poly(const jbi_split_view *view, const sv *v, int n)
{
    int t[2 * JBI_SPLIT_MAX_IN][3], m = gpu_triangles(n, t), f, k, j;

    for (f = 0; f < 2; ++f) {
        i64 r[4];

        view_rect(view, f, r);
        for (k = 0; k < m; ++k) {
            pt p[3];

            for (j = 0; j < 3; ++j)
                p[j] = pos(&v[t[k][j]], f);
            if (!tri_misses(p, r))
                return 0;
        }
    }
    return 1;
}

/* Whether polygon v meets the limits as the GPU takes it. */
static int poly_fits(const sv *v, int n)
{
    int i, j;

    if (n == 3)
        return jbi_split_triangle_fits(&v[0], &v[1], &v[2]);
    if (n == 4)
        return jbi_split_triangle_fits(&v[0], &v[1], &v[3]) &&
               jbi_split_triangle_fits(&v[1], &v[3], &v[2]);
    for (i = 0; i < n; ++i)
        for (j = i + 1; j < n; ++j)
            if (!in_range(&v[i]) || !in_range(&v[j]) || !near(&v[i], &v[j]))
                return 0;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Attributes at a new vertex */

/* a (1 - t) + b t for t = num / den, 0 <= num <= den, den > 0, rounded
   once (split.h): in 128 bits, so any ints. */
static int mix(int a, int b, i128 num, i128 den)
{
    return (int)div_round((i128)a * (den - num) + (i128)b * num, den);
}

/* The same with 1/z linear: weights wa and wb (both >= 0, not both 0). */
static int hyper(int a, int b, i128 wa, i128 wb)
{
    return (int)div_round((i128)a * wa + (i128)b * wb, wa + wb);
}

/* The attributes a fraction num / den of the way from a to b (split.h,
   Interpolation): exact rationals rounded once, the same from either end. */
static void interpolate(const sv *a, const sv *b, i128 num, i128 den, sv *o)
{
    if (den <= 0) {
        num = 0;
        den = 1;
    }
    num = num < 0 ? 0 : num > den ? den : num;
    o->r = mix(a->r, b->r, num, den);
    o->g = mix(a->g, b->g, num, den);
    o->b = mix(a->b, b->b, num, den);
    o->u = mix(a->u, b->u, num, den);
    o->v = mix(a->v, b->v, num, den);
    if (a->z > 0 && b->z > 0) {
        i128 wa = (i128)b->z * (den - num), wb = (i128)a->z * num;

        o->z = (int)div_round((i128)a->z * b->z * den, wa + wb);
        o->pu = hyper(a->pu, b->pu, wa, wb);
        o->pv = hyper(a->pv, b->pv, wa, wb);
        o->pr = hyper(a->pr, b->pr, wa, wb);
        o->pg = hyper(a->pg, b->pg, wa, wb);
        o->pb = hyper(a->pb, b->pb, wa, wb);
    } else {
        o->z = 0;
        o->pu = mix(a->pu, b->pu, num, den);
        o->pv = mix(a->pv, b->pv, num, den);
        o->pr = mix(a->pr, b->pr, num, den);
        o->pg = mix(a->pg, b->pg, num, den);
        o->pb = mix(a->pb, b->pb, num, den);
    }
}

/* ------------------------------------------------------------------------ */
/* Clipping a polygon to a tile, exactly */

/* The most vertices a clipped polygon has: each of the four planes adds at
   most one to a convex polygon; twice that, for a polygon that rounding
   has left a little concave. */
enum { MAX_CLIP = 2 * (JBI_SPLIT_MAX_IN + 4) };

enum { V_ORIG, V_CUT, V_CORNER };

/* A vertex of a clipped polygon: its exact position xn / d, yn / d (d >
   0), what it is (an input vertex, the point where input edge e meets
   side, or the corner of side and side2), the attributes there (a's
   positions are not used), and what the boundary does after it: line >= 0
   runs along input edge line, -1 - s along side s. Sides: 0 x >= x0, 1 x
   <= x1, 2 y >= y0, 3 y <= y1. */
typedef struct {
    i128 xn, yn, d;
    sv a;
    signed char kind, side, side2, line;
    unsigned char e;
} cv;

/* The polygon being clipped, in one format. */
typedef struct {
    const sv *v; /* its vertices (settled, perspective attributes taken) */
    int n;
    int f;       /* the format */
    const i64 *rect; /* the tile in the format's units: x0, x1, y0, y1 */
} src;

/* > 0 inside side s, 0 on it, < 0 outside. */
static int side_test(const cv *c, int s, const i64 *rect)
{
    i128 v = (s < 2 ? c->xn : c->yn) - (i128)rect[s] * c->d;

    return s & 1 ? -sign(v) : sign(v);
}

/* The point where input edge e (from vertex e to e + 1) meets side s,
   from the edge's ends alone: the same point for every polygon that has
   the edge, whichever way round. */
static void make_cut(const src *p, int e, int s, cv *o)
{
    const sv *a = &p->v[e], *b = &p->v[(e + 1) % p->n];
    pt pa = pos(a, p->f), pb = pos(b, p->f);
    i128 at = p->rect[s], d, num;

    if (s < 2) {
        d = (i128)pb.x - pa.x;
        num = at - pa.x;
        o->xn = at * d;
        o->yn = (i128)pa.y * d + ((i128)pb.y - pa.y) * num;
    } else {
        d = (i128)pb.y - pa.y;
        num = at - pa.y;
        o->yn = at * d;
        o->xn = (i128)pa.x * d + ((i128)pb.x - pa.x) * num;
    }
    if (d < 0) {
        d = -d;
        num = -num;
        o->xn = -o->xn;
        o->yn = -o->yn;
    }
    o->d = d;
    o->a = *a;
    interpolate(a, b, num, d, &o->a);
    o->kind = V_CUT;
    o->side = (signed char)s;
    o->side2 = -1;
    o->e = (unsigned char)e;
}

/* The nearest whole unit to a rational, halves up, as an int64 (for the
   interpolation of a corner, which is inside the polygon). */
static i64 near_unit(i128 n, i128 d)
{
    return (i64)round_up(n, d);
}

/* a l0 + b l1 + c l2 over det (barycentric weights, det > 0), rounded
   once; the weights may be negative a little outside, so the result is
   kept within the corners' values. */
static int bary(int a, int b, int c, const i128 *l, i128 det)
{
    i64 v = div_round((i128)a * l[0] + (i128)b * l[1] + (i128)c * l[2], det);
    int lo = a < b ? (a < c ? a : c) : (b < c ? b : c), hi = a > b ? (a > c ? a : c) : (b > c ? b : c);

    return v < lo ? lo : v > hi ? hi : (int)v;
}

/* The corner where side t (along which the boundary runs from prev to cur)
   meets side s, its attributes interpolated along the side. */
static void make_corner(const src *p, const cv *prev, const cv *cur, int t, int s, cv *o)
{
    int x_side = t < 2 ? t : s, y_side = t < 2 ? s : t;
    i64 at = p->rect[s], a, b;

    o->xn = p->rect[x_side];
    o->yn = p->rect[y_side];
    o->d = 1;
    o->kind = V_CORNER;
    o->side = (signed char)x_side;
    o->side2 = (signed char)y_side;
    o->e = 0;
    /* Along the side's free axis: y for an x side, x for a y side. */
    if (t < 2) {
        a = near_unit(prev->yn, prev->d);
        b = near_unit(cur->yn, cur->d);
    } else {
        a = near_unit(prev->xn, prev->d);
        b = near_unit(cur->xn, cur->d);
    }
    o->a = prev->a;
    if (b > a)
        interpolate(&prev->a, &cur->a, (i128)at - a, (i128)b - a, &o->a);
    else if (b < a)
        interpolate(&prev->a, &cur->a, (i128)a - at, (i128)a - b, &o->a);
    /* In a triangle the affine attributes are its plane's, exactly: from
       its corners, rounded once. */
    if (p->n == 3) {
        pt k, q[3];
        i128 det, l[3];
        int i;

        k.x = (i64)o->xn;
        k.y = (i64)o->yn;
        for (i = 0; i < 3; ++i)
            q[i] = pos(&p->v[i], p->f);
        det = (i128)(q[1].x - q[0].x) * (q[2].y - q[0].y) -
              (i128)(q[2].x - q[0].x) * (q[1].y - q[0].y);
        if (det != 0) {
            for (i = 0; i < 3; ++i) {
                pt b1 = q[(i + 1) % 3], b2 = q[(i + 2) % 3];

                l[i] = (i128)(b1.x - k.x) * (b2.y - k.y) - (i128)(b2.x - k.x) * (b1.y - k.y);
            }
            if (det < 0) {
                det = -det;
                for (i = 0; i < 3; ++i)
                    l[i] = -l[i];
            }
            o->a.r = bary(p->v[0].r, p->v[1].r, p->v[2].r, l, det);
            o->a.g = bary(p->v[0].g, p->v[1].g, p->v[2].g, l, det);
            o->a.b = bary(p->v[0].b, p->v[1].b, p->v[2].b, l, det);
            o->a.u = bary(p->v[0].u, p->v[1].u, p->v[2].u, l, det);
            o->a.v = bary(p->v[0].v, p->v[1].v, p->v[2].v, l, det);
        }
    }
}

/* The part of in (n vertices) on the kept side of s, into out. */
static int clip_side(const src *p, const cv *in, int n, cv *out, int s)
{
    int i, m = 0;

    for (i = 0; i < n && m + 2 <= MAX_CLIP; ++i) {
        const cv *cur = &in[i], *prev = &in[(i + n - 1) % n];
        int dc = side_test(cur, s, p->rect), dp = side_test(prev, s, p->rect);

        if (dc >= 0) {
            if (dp < 0 && dc > 0) {
                if (prev->line >= 0)
                    make_cut(p, prev->line, s, &out[m]);
                else
                    make_corner(p, prev, cur, -1 - prev->line, s, &out[m]);
                out[m].line = prev->line;
                ++m;
            }
            out[m++] = *cur;
        } else if (dp > 0) {
            if (prev->line >= 0)
                make_cut(p, prev->line, s, &out[m]);
            else
                make_corner(p, prev, cur, -1 - prev->line, s, &out[m]);
            out[m].line = (signed char)(-1 - s);
            ++m;
        } else if (dp == 0 && i > 0) {
            out[m - 1].line = (signed char)(-1 - s); /* prev, on the plane */
        }
    }
    /* The first vertex outside after the last on the plane. */
    if (n > 0 && m > 0 && side_test(&in[0], s, p->rect) < 0 &&
        side_test(&in[n - 1], s, p->rect) == 0)
        out[m - 1].line = (signed char)(-1 - s);
    return m;
}

/* The polygon clipped to the tile, exactly; returns its vertex count. */
static int clip_poly(const src *p, cv *out)
{
    cv a[MAX_CLIP], b[MAX_CLIP];
    int i, m, s;

    for (i = 0; i < p->n; ++i) {
        pt q = pos(&p->v[i], p->f);

        a[i].xn = q.x;
        a[i].yn = q.y;
        a[i].d = 1;
        a[i].a = p->v[i];
        a[i].kind = V_ORIG;
        a[i].side = a[i].side2 = -1;
        a[i].e = (unsigned char)i;
        a[i].line = (signed char)i;
    }
    m = p->n;
    for (s = 0; s < 4 && m > 0; ++s) {
        m = clip_side(p, s & 1 ? b : a, m, s & 1 ? a : b, s);
    }
    for (i = 0; i < m; ++i)
        out[i] = a[i];
    return m;
}

/* ------------------------------------------------------------------------ */
/* Rounding the cuts */

/* A clipped polygon in one format, its cuts rounded. */
typedef struct {
    cv c[MAX_CLIP];
    int m;
    pt at[MAX_CLIP];       /* the positions drawn */
    unsigned char inward[MAX_CLIP]; /* a cut on a kept edge, rounded inward */
    int sigma;             /* the orientation of the input polygon here */
} ring;

/* a / b < c / d, b and d > 0. */
static int less(i128 a, i128 b, i128 c, i128 d)
{
    return a * d < c * b;
}

/* Where c lies along its side: num / den. */
static void along(const cv *c, int s, i128 *num, i128 *den)
{
    *num = s < 2 ? c->yn : c->xn;
    *den = c->d;
}

/* The ring's neighbour of cut i along its side (the other end of the
   boundary that runs along the side from it), or -1. */
static int side_neighbour(const ring *r, int i)
{
    int s = r->c[i].side, next = (i + 1) % r->m, prev = (i + r->m - 1) % r->m;

    if (r->c[i].line == -1 - s)
        return next;
    if (r->c[prev].line == -1 - s)
        return prev;
    return -1;
}

static i64 along_at(pt p, int s)
{
    return s < 2 ? p.y : p.x;
}

static void set_along(pt *p, int s, i64 v)
{
    if (s < 2)
        p->y = v;
    else
        p->x = v;
}

/*
 * The cuts of a ring rounded (split.h, Rounding): a cut on an edge beyond
 * the limits to the nearest unit along its side, halves up, the same for
 * every polygon that has the edge; a cut on an edge kept whole inward,
 * towards its neighbour along the side, unless that would take it past
 * where the neighbour ends up, when it goes the other way and gets no
 * ear. Every other vertex is exact.
 */
static void round_cuts(ring *r, const unsigned char *kept)
{
    int i, pass;

    for (pass = 0; pass < 2; ++pass)
        for (i = 0; i < r->m; ++i) {
            const cv *c = &r->c[i];
            i128 num, den, f, g;
            int s = c->side, nb, dir = 0;

            if (pass == 0) {
                r->inward[i] = 0;
                r->at[i].x = (i64)(c->xn / c->d);
                r->at[i].y = (i64)(c->yn / c->d);
            }
            if (c->kind != V_CUT || (pass == 1) != (kept[c->e] != 0))
                continue;
            along(c, s, &num, &den);
            f = floor_div(num, den);
            g = f * den == num ? f : f + 1;
            if (pass == 0) {
                set_along(&r->at[i], s, (i64)round_up(num, den));
                continue;
            }
            /* Kept: towards the neighbour along the side, if not past it
               (its rounded position, or for a kept neighbour not yet
               rounded, its inward one). */
            nb = side_neighbour(r, i);
            if (nb >= 0) {
                i128 nn, nd;

                along(&r->c[nb], s, &nn, &nd);
                dir = less(num, den, nn, nd) ? 1 : less(nn, nd, num, den) ? -1 : 0;
            }
            if (dir != 0 && f != g) {
                i64 want = (i64)(dir > 0 ? g : f), lim = along_at(r->at[nb], s);
                const cv *n = &r->c[nb];

                if (n->kind == V_CUT && kept[n->e] && nb > i) {
                    i128 nn, nd, nf;

                    along(n, s, &nn, &nd);
                    nf = floor_div(nn, nd);
                    lim = (i64)(dir > 0 ? nf : (nf * nd == nn ? nf : nf + 1));
                }
                if (dir > 0 ? want <= lim : want >= lim) {
                    set_along(&r->at[i], s, want);
                    r->inward[i] = 1;
                    continue;
                }
                set_along(&r->at[i], s, (i64)(dir > 0 ? f : g));
                continue;
            }
            set_along(&r->at[i], s, (i64)round_up(num, den));
            /* An exact cut lies on its edge: an ear needs nothing more. */
            r->inward[i] = f == g;
        }
}

/* ------------------------------------------------------------------------ */
/* Triangulating a clipped polygon */

enum { MAX_TRI = MAX_CLIP };

/*
 * Ear clipping of the ring at (m vertices, orientation sigma), exact:
 * triangles as index triples into tri, returning how many, or -1 when it
 * finds no ear (a ring that is not simple: split.h says when that can
 * be). A vertex with its neighbours on one line goes first, as a triangle
 * of no area (it covers nothing, and keeps the triangulation whole for a
 * second format, where it may have area); then an ear, a vertex turning
 * the ring's way with no other vertex in or on its triangle.
 */
static int earclip(const pt *at, int m, int sigma, int (*tri)[3])
{
    int idx[MAX_CLIP], n = m, t = 0, i, j;

    for (i = 0; i < m; ++i)
        idx[i] = i;
    while (n > 3) {
        int found = -1;

        for (i = 0; i < n && found < 0; ++i)
            if (orient(at[idx[(i + n - 1) % n]], at[idx[i]], at[idx[(i + 1) % n]]) == 0)
                found = i;
        for (i = 0; i < n && found < 0; ++i) {
            pt p = at[idx[(i + n - 1) % n]], v = at[idx[i]], q = at[idx[(i + 1) % n]];
            int ok = orient(p, v, q) * sigma > 0;

            for (j = 0; j < n && ok; ++j) {
                pt w = at[idx[j]];

                if ((w.x == p.x && w.y == p.y) || (w.x == v.x && w.y == v.y) ||
                    (w.x == q.x && w.y == q.y))
                    continue;
                ok = !(orient(p, v, w) * sigma >= 0 && orient(v, q, w) * sigma >= 0 &&
                       orient(q, p, w) * sigma >= 0);
            }
            if (ok)
                found = i;
        }
        if (found < 0)
            return -1;
        tri[t][0] = idx[(found + n - 1) % n];
        tri[t][1] = idx[found];
        tri[t][2] = idx[(found + 1) % n];
        ++t;
        for (j = found; j + 1 < n; ++j)
            idx[j] = idx[j + 1];
        --n;
    }
    if (n == 3) {
        if (orient(at[idx[0]], at[idx[1]], at[idx[2]]) * sigma < 0)
            return -1;
        tri[t][0] = idx[0];
        tri[t][1] = idx[1];
        tri[t][2] = idx[2];
        ++t;
    }
    return t;
}

/* Whether the triangles tri (t of them) over positions at have the
   orientation sigma or none, and pairwise disjoint interiors. */
static int valid(const pt *at, const int (*tri)[3], int t, int sigma)
{
    int i, j, k;

    for (i = 0; i < t; ++i)
        if (orient(at[tri[i][0]], at[tri[i][1]], at[tri[i][2]]) * sigma < 0)
            return 0;
    for (i = 0; i < t; ++i)
        for (j = i + 1; j < t; ++j) {
            pt a[3], b[3];

            for (k = 0; k < 3; ++k) {
                a[k] = at[tri[i][k]];
                b[k] = at[tri[j][k]];
            }
            if (tri_overlap(a, b))
                return 0;
        }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* The pieces */

/* An ear that draws an edge kept whole (split.h): its triangles (where a
   cut at an end is exact, the edge passes through it and the triangle
   there has no area), as the
   edge's ends (0 and 1: input vertices e and e + 1) and the ring's
   vertices (2 + i). */
typedef struct {
    int e;       /* the edge */
    int ring[2]; /* the ring vertices it uses, -1 for none */
    int tri[2][3];
    int ntri;
} ear;

/* One format's result for one polygon in one tile. */
typedef struct {
    ring r;
    int tri[MAX_TRI][3];
    int ntri; /* -1: not drawn in this format */
    ear ears[JBI_SPLIT_MAX_IN];
    int nears;
} shape;

/* The ears of a ring (split.h): for each part of a kept input edge with a
   cut rounded inward at one end or both. */
static void find_ears(shape *s, const sv *v, int n, int f, const unsigned char *kept)
{
    const ring *r = &s->r;
    int i;

    s->nears = 0;
    for (i = 0; i < r->m && s->nears < JBI_SPLIT_MAX_IN; ++i) {
        const cv *c = &r->c[i], *d = &r->c[(i + 1) % r->m];
        int j = (i + 1) % r->m, e = c->line;
        ear *x;
        pt pa, pb;

        if (e < 0 || !kept[e])
            continue;
        if (!(c->kind == V_CUT && c->e == e) && !(d->kind == V_CUT && d->e == e))
            continue;
        if ((c->kind == V_CUT && c->e == e && !r->inward[i]) ||
            (d->kind == V_CUT && d->e == e && !r->inward[j]))
            continue; /* moved outward or not at all: no ear */
        pa = pos(&v[e], f);
        pb = pos(&v[(e + 1) % n], f);
        x = &s->ears[s->nears];
        memset(x, 0, sizeof *x);
        x->e = e;
        x->ring[0] = c->kind == V_CUT ? i : -1;
        x->ring[1] = d->kind == V_CUT ? j : -1;
        if (x->ring[0] >= 0 && x->ring[1] >= 0) {
            /* Both ends outside: the quad a b K1 K0, by the diagonal that
               gives both its triangles the polygon's orientation; or, where
               one cut is exact (on the edge), the triangle from it. */
            pt k0 = r->at[i], k1 = r->at[j];
            int on0 = orient(pa, pb, k0) == 0, on1 = orient(pa, pb, k1) == 0;

            if (on0 && on1)
                continue;
            if (on0 || on1) {
                if (orient(pa, pb, on0 ? k1 : k0) * r->sigma <= 0)
                    continue;
                if (on0) {
                    x->tri[0][0] = 2, x->tri[0][1] = 1, x->tri[0][2] = 3;
                } else {
                    x->tri[0][0] = 0, x->tri[0][1] = 3, x->tri[0][2] = 2;
                }
                x->ntri = 1;
            } else if (orient(pa, pb, k1) * r->sigma > 0 &&
                       orient(pa, k1, k0) * r->sigma >= 0) {
                x->tri[0][0] = 0, x->tri[0][1] = 1, x->tri[0][2] = 3;
                x->tri[1][0] = 0, x->tri[1][1] = 3, x->tri[1][2] = 2;
                x->ntri = 2;
            } else if (orient(pa, pb, k0) * r->sigma > 0 &&
                       orient(pb, k1, k0) * r->sigma >= 0) {
                x->tri[0][0] = 0, x->tri[0][1] = 1, x->tri[0][2] = 2;
                x->tri[1][0] = 1, x->tri[1][1] = 3, x->tri[1][2] = 2;
                x->ntri = 2;
            } else {
                continue;
            }
        } else {
            pt k = x->ring[0] >= 0 ? r->at[i] : r->at[j];

            if (orient(pa, pb, k) * r->sigma <= 0)
                continue;
            x->tri[0][0] = 0, x->tri[0][1] = 1;
            x->tri[0][2] = x->ring[0] >= 0 ? 2 : 3;
            x->ntri = 1;
        }
        ++s->nears;
    }
}

/* The tiles of a view (split.h): at most 2 x 2, each the rectangle of
   pixels it draws (area) and the rectangle its polygons are clipped to,
   the tile widened on each side as far as the GPU's limits allow (at most
   1023 x 511, inside -1024..1023): the guard band, which the drawing area
   clips. */
typedef struct {
    i64 x0, x1, y0, y1; /* the clip rectangle's planes, in pixels */
    jbi_split_area area;
} tile;

/* The band [lo, hi) widened to at most size, evenly, inside the range. */
static void widen(i64 lo, i64 hi, i64 size, i64 *a, i64 *b)
{
    i64 extra = size - (hi - lo);

    *a = lo - extra / 2;
    *b = hi + (extra - extra / 2);
    if (*a < LIMIT_LO) {
        *b += LIMIT_LO - *a;
        *a = LIMIT_LO;
    }
    if (*b > LIMIT_HI) {
        *a -= *b - LIMIT_HI;
        *b = LIMIT_HI;
    }
    *a = *a < LIMIT_LO ? LIMIT_LO : *a;
}

static int make_tiles(const jbi_split_view *view, tile *t)
{
    i64 x0 = view->x0, x1 = (i64)view->x1 + 1, y0 = view->y0, y1 = (i64)view->y1 + 1;
    int nx, ny, i, j, n = 0;

    x1 = x1 > LIMIT_HI ? LIMIT_HI : x1;
    y1 = y1 > LIMIT_HI ? LIMIT_HI : y1;
    nx = x1 - x0 > 1023 ? 2 : 1;
    ny = y1 - y0 > 511 ? 2 : 1;
    for (j = 0; j < ny; ++j)
        for (i = 0; i < nx; ++i) {
            tile *q = &t[n++];
            i64 ax = x0 + (x1 - x0) * i / nx, bx = x0 + (x1 - x0) * (i + 1) / nx;
            i64 ay = y0 + (y1 - y0) * j / ny, by = y0 + (y1 - y0) * (j + 1) / ny;

            widen(ax, bx, 1023, &q->x0, &q->x1);
            widen(ay, by, 511, &q->y0, &q->y1);
            q->area.x0 = (int)ax;
            q->area.y0 = (int)ay;
            q->area.x1 = i + 1 == nx ? view->x1 : (int)bx - 1;
            q->area.y1 = j + 1 == ny ? view->y1 : (int)by - 1;
        }
    return n;
}

/* A clipped polygon's vertex as given to the callback. */
typedef struct {
    const jbi_split_view *view;
    jbi_split_fn fn;
    void *user;
    int pieces;
    unsigned mask; /* the passes a clipped piece may be given for */
} emitter;

/* The vertex for one format: f 0 the integer piece (its sub-pixel position
   that times 256), 1 the sub-pixel piece (its integer position that
   rounded, within the range); f 2 both (integer from ri, sub-pixel and
   attributes from rq). orig: an input vertex, whose integer position is
   kept (an ear's ends, which meet the limits by them). */
static void out_vertex(sv *o, const cv *ci, pt pi, const cv *cq, pt pq, int f, const sv *orig)
{
    *o = f == 0 ? ci->a : cq->a;
    if (f == 0) {
        o->x = (int)pi.x;
        o->y = (int)pi.y;
        o->qx = pi.x * ONE;
        o->qy = pi.y * ONE;
    } else {
        o->qx = pq.x;
        o->qy = pq.y;
        if (f == 2) {
            o->x = (int)pi.x;
            o->y = (int)pi.y;
        } else if (orig != NULL) {
            o->x = orig->x;
            o->y = orig->y;
        } else {
            o->x = clamp_range(q_pixel_round(pq.x));
            o->y = clamp_range(q_pixel_round(pq.y));
        }
    }
}

static unsigned pass_bit(int f)
{
    return f ? JBI_SPLIT_PASS_PRECISE : JBI_SPLIT_PASS_1X;
}

static void give(emitter *em, int poly, const sv *t, unsigned passes, const jbi_split_area *area)
{
    passes &= em->mask;
    if (passes == 0)
        return;
    em->fn(em->user, poly, t, 3, passes, area);
    ++em->pieces;
}

/* The pieces of shape s (format f) of polygon poly. */
static void emit_shape(emitter *em, int poly, const shape *s, int f, const sv *v, int n,
                       const jbi_split_area *area)
{
    int i, k;

    for (i = 0; i < s->ntri; ++i) {
        sv t[3];
        pt p[3];

        for (k = 0; k < 3; ++k) {
            int j = s->tri[i][k];

            p[k] = s->r.at[j];
            out_vertex(&t[k], &s->r.c[j], s->r.at[j], &s->r.c[j], s->r.at[j], f, NULL);
        }
        give(em, poly, t, orient(p[0], p[1], p[2]) != 0 ? pass_bit(f) : 0, area);
    }
    for (i = 0; i < s->nears; ++i) {
        const ear *x = &s->ears[i];

        for (k = 0; k < x->ntri; ++k) {
            sv t[3];
            int j, ok = 1;

            for (j = 0; j < 3; ++j) {
                int c = x->tri[k][j];

                if (c < 2) {
                    const sv *o = &v[(x->e + c) % n];
                    cv tmp;

                    tmp.a = *o;
                    out_vertex(&t[j], &tmp, pos(o, f), &tmp, pos(o, f), f, o);
                } else {
                    int ri = x->ring[c - 2];

                    out_vertex(&t[j], &s->r.c[ri], s->r.at[ri], &s->r.c[ri], s->r.at[ri], f,
                               NULL);
                }
            }
            /* A sub-pixel piece's integer positions must meet the limits
               too (the GPU checks them), which an ear's rounded cut may
               just not. */
            if (f == 1)
                ok = jbi_split_triangle_fits(&t[0], &t[1], &t[2]);
            if (ok)
                give(em, poly, t, pass_bit(f), area);
        }
    }
}

/* Whether the two formats' rings of a polygon are the same vertices (the
   same input vertices, cuts and corners in the same order). */
static int same_ring(const ring *a, const ring *b)
{
    int i;

    if (a->m != b->m)
        return 0;
    for (i = 0; i < a->m; ++i) {
        const cv *p = &a->c[i], *q = &b->c[i];

        if (p->kind != q->kind || p->e != q->e || p->side != q->side || p->side2 != q->side2)
            return 0;
    }
    return 1;
}

/* Both formats' pieces of a polygon at once, where one triangulation
   serves both (split.h): the sub-pixel one, valid by the integer
   positions too, with the positions within the slack of each other. */
static int emit_merged(emitter *em, int poly, const shape *si, const shape *sq, const sv *v,
                       int n, const jbi_split_area *area)
{
    pt ai[MAX_CLIP];
    int i, k, e;

    if (si->ntri < 0 || sq->ntri < 0 || !same_ring(&si->r, &sq->r) || si->nears != sq->nears)
        return 0;
    for (i = 0; i < si->r.m; ++i) {
        i64 dx = sq->r.at[i].x - si->r.at[i].x * ONE, dy = sq->r.at[i].y - si->r.at[i].y * ONE;

        if (dx > SLACK || dx < -SLACK || dy > SLACK || dy < -SLACK)
            return 0;
        ai[i] = si->r.at[i];
    }
    if (!valid(ai, (const int(*)[3])sq->tri, sq->ntri, si->r.sigma))
        return 0;
    for (e = 0; e < si->nears; ++e)
        if (si->ears[e].e != sq->ears[e].e || si->ears[e].ntri != sq->ears[e].ntri ||
            memcmp(si->ears[e].tri, sq->ears[e].tri,
                   (size_t)si->ears[e].ntri * sizeof si->ears[e].tri[0]) != 0 ||
            memcmp(si->ears[e].ring, sq->ears[e].ring, sizeof si->ears[e].ring) != 0)
            return 0;
    for (i = 0; i < sq->ntri; ++i) {
        sv t[3];
        pt p[3], q[3];
        unsigned passes;

        for (k = 0; k < 3; ++k) {
            int j = sq->tri[i][k];

            p[k] = si->r.at[j];
            q[k] = sq->r.at[j];
            out_vertex(&t[k], &si->r.c[j], si->r.at[j], &sq->r.c[j], sq->r.at[j], 2, NULL);
        }
        passes = (orient(p[0], p[1], p[2]) != 0 ? JBI_SPLIT_PASS_1X : 0u) |
                 (orient(q[0], q[1], q[2]) != 0 ? JBI_SPLIT_PASS_PRECISE : 0u);
        give(em, poly, t, passes, area);
    }
    for (e = 0; e < si->nears; ++e) {
        const ear *x = &sq->ears[e];

        for (k = 0; k < x->ntri; ++k) {
            sv t[3];
            int j;

            for (j = 0; j < 3; ++j) {
                int c = x->tri[k][j];

                if (c < 2) {
                    const sv *o = &v[(x->e + c) % n];
                    cv tmp;

                    tmp.a = *o;
                    out_vertex(&t[j], &tmp, pos(o, 0), &tmp, pos(o, 1), 2, NULL);
                } else {
                    int ri = x->ring[c - 2];

                    out_vertex(&t[j], &si->r.c[ri], si->r.at[ri], &sq->r.c[ri], sq->r.at[ri], 2,
                               NULL);
                }
            }
            give(em, poly, t, JBI_SPLIT_PASS_1X | JBI_SPLIT_PASS_PRECISE, area);
        }
    }
    return 1;
}

static int same_vertex(const sv *a, const sv *b)
{
    return a->x == b->x && a->y == b->y && a->qx == b->qx && a->qy == b->qy;
}

/* The input polygons of a primitive, prepared. */
typedef struct {
    sv v[JBI_SPLIT_MAX_PRIM][JBI_SPLIT_MAX_IN];
    int n[JBI_SPLIT_MAX_PRIM];
    unsigned char kept[JBI_SPLIT_MAX_PRIM][JBI_SPLIT_MAX_IN];
    /* The other polygon's edge with the same ends, or -1. */
    signed char shared[JBI_SPLIT_MAX_PRIM][JBI_SPLIT_MAX_IN];
    int sigma[JBI_SPLIT_MAX_PRIM][2]; /* orientation by format */
    int count;
} prim;

static int poly_sign(const sv *v, int n, int f)
{
    i128 s = 0;
    int i;

    for (i = 0; i < n; ++i) {
        pt a = pos(&v[i], f), b = pos(&v[(i + 1) % n], f);

        s += (i128)a.x * b.y - (i128)b.x * a.y;
    }
    return sign(s);
}

static i128 poly_area(const sv *v, int n, int f)
{
    i128 s = 0;
    int i;

    for (i = 0; i < n; ++i) {
        pt a = pos(&v[i], f), b = pos(&v[(i + 1) % n], f);

        s += (i128)a.x * b.y - (i128)b.x * a.y;
    }
    return magnitude(s);
}

/* Whether polygons k and l (convex, each fanned from its first vertex)
   overlap in format f: then they overlap on the console as well (a crossed
   quad's two triangles), and are not held apart. */
static int sources_overlap(const prim *p, int k, int l, int f)
{
    int i, j, c;

    for (i = 1; i + 1 < p->n[k]; ++i)
        for (j = 1; j + 1 < p->n[l]; ++j) {
            pt a[3], b[3];

            for (c = 0; c < 3; ++c) {
                int ia = c == 0 ? 0 : i + c - 1, ib = c == 0 ? 0 : j + c - 1;

                a[c] = pos(&p->v[k][ia], f);
                b[c] = pos(&p->v[l][ib], f);
            }
            if (tri_overlap(a, b))
                return 1;
        }
    return 0;
}

/* Whether the cores of shapes a and b overlap. */
static int shapes_overlap(const shape *a, const shape *b)
{
    int i, j, k;

    for (i = 0; i < a->ntri; ++i)
        for (j = 0; j < b->ntri; ++j) {
            pt x[3], y[3];

            for (k = 0; k < 3; ++k) {
                x[k] = a->r.at[a->tri[i][k]];
                y[k] = b->r.at[b->tri[j][k]];
            }
            if (tri_overlap(x, y))
                return 1;
        }
    return 0;
}

/* One format of a primitive in one tile: each polygon clipped, its cuts
   rounded, triangulated and checked. Returns the polygons left out (for
   not being simple, or for overlapping another: split.h says when these
   can be). */
static int shape_format(const prim *p, int f, const i64 *rect, shape *out)
{
    unsigned char kept[JBI_SPLIT_MAX_PRIM][JBI_SPLIT_MAX_IN];
    int k, i, j, dropped = 0, again = 1;

    memcpy(kept, p->kept, sizeof kept);
    for (k = 0; k < p->count; ++k) {
        shape *s = &out[k];
        src q;

        q.v = p->v[k];
        q.n = p->n[k];
        q.f = f;
        q.rect = rect;
        s->r.sigma = p->sigma[k][f];
        s->r.m = s->r.sigma != 0 ? clip_poly(&q, s->r.c) : 0;
    }
    /* An edge kept whole that both polygons have is drawn whole by both,
       each with its ear, only where every cut of it went inward on both;
       else it is not kept in this tile and format, on either: both cut it
       at the nearest points, as an edge beyond the limits, so that neither
       covers the other's ear and they meet along it. The cuts are rounded
       again then, so that the polygons' other kept cuts are rounded
       against those points. */
    while (again) {
        again = 0;
        for (k = 0; k < p->count; ++k)
            if (out[k].r.m >= 3)
                round_cuts(&out[k].r, kept[k]);
        for (k = 0; k + 1 < p->count; ++k)
            for (i = 0; i < out[k].r.m; ++i) {
                const cv *c = &out[k].r.c[i];
                int e2 = c->kind == V_CUT ? p->shared[k][c->e] : -1;

                if (e2 < 0 || !kept[k][c->e])
                    continue;
                for (j = 0; j < out[k + 1].r.m; ++j) {
                    const cv *d = &out[k + 1].r.c[j];

                    if (d->kind != V_CUT || d->e != e2 || d->side != c->side)
                        continue;
                    if (out[k].r.inward[i] && out[k + 1].r.inward[j])
                        continue;
                    kept[k][c->e] = 0;
                    kept[k + 1][e2] = 0;
                    again = 1;
                }
            }
    }
    for (k = 0; k < p->count; ++k) {
        shape *s = &out[k];

        s->nears = 0;
        s->ntri = -1;
        if (s->r.m < 3)
            continue;
        s->ntri = earclip(s->r.at, s->r.m, s->r.sigma, s->tri);
        if (s->ntri < 0 || !valid(s->r.at, (const int(*)[3])s->tri, s->ntri, s->r.sigma)) {
            s->ntri = -1;
            ++dropped;
            continue;
        }
        find_ears(s, p->v[k], p->n[k], f, kept[k]);
    }
    if (p->count == 2 && out[0].ntri > 0 && out[1].ntri > 0 && !sources_overlap(p, 0, 1, f) &&
        shapes_overlap(&out[0], &out[1])) {
        /* Left out: the smaller. */
        k = poly_area(p->v[0], p->n[0], f) < poly_area(p->v[1], p->n[1], f) ? 0 : 1;
        out[k].ntri = -1;
        out[k].nears = 0;
        ++dropped;
    }
    return dropped;
}

/* ------------------------------------------------------------------------ */
/* Polygons */

/* A passed-through polygon of more than 4 vertices: emit_fan's quads (Z
   order) and last triangle, each drawable in both passes. */
static int emit_fan(emitter *em, int poly, const sv *p, int m)
{
    int i = 1, pieces = 0;

    while (i + 2 < m) {
        sv z[4];

        z[0] = p[0];
        z[1] = p[i];
        z[2] = p[i + 2];
        z[3] = p[i + 1];
        em->fn(em->user, poly, z, 4, JBI_SPLIT_PASS_1X | JBI_SPLIT_PASS_PRECISE, NULL);
        ++pieces;
        i += 2;
    }
    if (i + 1 < m) {
        sv t[3];

        t[0] = p[0];
        t[1] = p[i];
        t[2] = p[i + 1];
        em->fn(em->user, poly, t, 3, JBI_SPLIT_PASS_1X | JBI_SPLIT_PASS_PRECISE, NULL);
        ++pieces;
    }
    return pieces;
}

/* The clipped path of jbi_split_polys for primitive p (its vertices
   settled here), its pieces given to em; returns the polygons left out. */
static int clip_prim(const jbi_split_view *view, prim *p, emitter *em)
{
    tile tiles[4];
    int k, i, f, ntiles, t, left = 0;

    for (k = 0; k < p->count; ++k) {
        for (i = 0; i < p->n[k]; ++i)
            settle(&p->v[k][i]);
        for (i = 0; i < p->n[k]; ++i) {
            const sv *a = &p->v[k][i], *b = &p->v[k][(i + 1) % p->n[k]];

            p->kept[k][i] = (unsigned char)(in_range(a) && in_range(b) && near(a, b));
        }
        for (f = 0; f < 2; ++f)
            p->sigma[k][f] = poly_sign(p->v[k], p->n[k], f);
    }
    for (k = 0; k < p->count; ++k)
        for (i = 0; i < p->n[k]; ++i) {
            const sv *a = &p->v[k][i], *b = &p->v[k][(i + 1) % p->n[k]];
            int l = 1 - k, j;

            p->shared[k][i] = -1;
            for (j = 0; p->count == 2 && j < p->n[l]; ++j) {
                const sv *c = &p->v[l][j], *d = &p->v[l][(j + 1) % p->n[l]];

                if ((same_vertex(a, d) && same_vertex(b, c)) || (same_vertex(a, c) && same_vertex(b, d)))
                    p->shared[k][i] = (signed char)j;
            }
        }
    ntiles = make_tiles(view, tiles);
    for (t = 0; t < ntiles; ++t) {
        shape s[2][JBI_SPLIT_MAX_PRIM];
        const jbi_split_area *area = ntiles > 1 ? &tiles[t].area : NULL;

        for (f = 0; f < 2; ++f) {
            i64 one = f ? ONE : 1, rect[4];

            rect[0] = tiles[t].x0 * one;
            rect[1] = tiles[t].x1 * one;
            rect[2] = tiles[t].y0 * one;
            rect[3] = tiles[t].y1 * one;
            left += shape_format(p, f, rect, s[f]);
        }
        for (k = 0; k < p->count; ++k) {
            /* One set of pieces for both formats where it can serve both;
               a primitive of two polygons always has one for each. */
            if (p->count == 1 && emit_merged(em, k, &s[0][k], &s[1][k], p->v[k], p->n[k], area))
                continue;
            for (f = 0; f < 2; ++f)
                if (s[f][k].ntri >= 0)
                    emit_shape(em, k, &s[f][k], f, p->v[k], p->n[k], area);
        }
    }
    return left;
}

int jbi_split_polys(const jbi_split_view *view, const sv *const *v, const int *n, int count,
                    jbi_split_fn fn, void *user, int *dropped)
{
    prim p;
    emitter em;
    int k, i, f, fits = 1, live = 0, left = 0;

    if (dropped != NULL)
        *dropped = 0;
    if (count < 1 || count > JBI_SPLIT_MAX_PRIM)
        return -1;
    for (k = 0; k < count; ++k)
        if (n[k] < 3 || n[k] > JBI_SPLIT_MAX_IN)
            return -1;
    em.view = view;
    em.fn = fn;
    em.mask = JBI_SPLIT_PASS_1X | JBI_SPLIT_PASS_PRECISE;
    em.user = user;
    em.pieces = 0;
    p.count = count;
    for (k = 0; k < count; ++k) {
        p.n[k] = n[k];
        for (i = 0; i < n[k]; ++i)
            take(&p.v[k][i], &v[k][i], view->perspective_given);
        if (!culled_poly(view, p.v[k], n[k]))
            ++live;
        fits = fits && poly_fits(p.v[k], n[k]);
    }
    if (live == 0)
        return 0;
    if (fits) {
        /* Passed through, as the GP0 words of the same vertices draw. */
        for (k = 0; k < count; ++k) {
            const sv *q = p.v[k];

            if (culled_poly(view, q, n[k]))
                continue;
            if (n[k] == 3) {
                fn(user, k, q, 3, JBI_SPLIT_PASS_1X | JBI_SPLIT_PASS_PRECISE, NULL);
                ++em.pieces;
            } else if (n[k] == 4) {
                sv z[4];

                z[0] = q[0];
                z[1] = q[1];
                z[2] = q[3];
                z[3] = q[2];
                fn(user, k, z, 4, JBI_SPLIT_PASS_1X | JBI_SPLIT_PASS_PRECISE, NULL);
                ++em.pieces;
            } else {
                em.pieces += emit_fan(&em, k, q, n[k]);
            }
        }
        return em.pieces;
    }
    /* Clipped to the tiles. A polygon with a vertex the decoder would
       draw at its integer position passed through (seam_vertex) is
       clipped twice: as ever for the integer format (the 1x pass, and the
       scaled one without precise vertices, which so draw exactly what
       they drew), and with that vertex's sub-pixel position its integer
       one for the precise pass, so that it meets a neighbour passed
       through along their shared edge there (no crack opens). */
    for (k = 0, f = 0; k < count; ++k)
        for (i = 0; i < n[k]; ++i)
            f |= seam_vertex(&p.v[k][i]);
    if (!f) {
        left = clip_prim(view, &p, &em);
    } else {
        prim q = p;
        for (k = 0; k < count; ++k)
            for (i = 0; i < n[k]; ++i)
                if (seam_vertex(&q.v[k][i]))
                    q.v[k][i].qx = q.v[k][i].x * (i64)ONE, q.v[k][i].qy = q.v[k][i].y * (i64)ONE;
        em.mask = JBI_SPLIT_PASS_1X;
        left = clip_prim(view, &p, &em);
        em.mask = JBI_SPLIT_PASS_PRECISE;
        left += clip_prim(view, &q, &em);
    }
    if (dropped != NULL)
        *dropped = left;
    return em.pieces;
}

int jbi_split_poly(const jbi_split_view *view, const sv *v, int n, jbi_split_fn fn, void *user)
{
    return jbi_split_polys(view, &v, &n, 1, fn, user, NULL);
}

/* ------------------------------------------------------------------------ */
/* Lines */

/* Whether a line draws nothing of the view in either pass: by its integer
   positions the 1x line's pixels lie within its ends' bounds; the scaled
   line (N scaled pixels thick, from its ends' blocks, src/soft/raster_
   hires.c) within the 1x pixels from floor(q / 256 - 1/2N) to q / 256 + 1,
   so it misses a column x0 when its greatest sub-pixel x is 256 (x0 - 1)
   or less, at any scale, and likewise on each side. */
static int culled_line(const jbi_split_view *view, const sv *a, const sv *b)
{
    i64 x0 = a->x < b->x ? a->x : b->x, x1 = a->x < b->x ? b->x : a->x;
    i64 y0 = a->y < b->y ? a->y : b->y, y1 = a->y < b->y ? b->y : a->y;
    i64 qx0 = a->qx < b->qx ? a->qx : b->qx, qx1 = a->qx < b->qx ? b->qx : a->qx;
    i64 qy0 = a->qy < b->qy ? a->qy : b->qy, qy1 = a->qy < b->qy ? b->qy : a->qy;
    int miss_i = x1 < view->x0 || x0 > view->x1 || y1 < view->y0 || y0 > view->y1;
    int miss_q = qx1 <= ((i64)view->x0 - 1) * ONE || qx0 >= ((i64)view->x1 + 1) * ONE ||
                 qy1 <= ((i64)view->y0 - 1) * ONE || qy0 >= ((i64)view->y1 + 1) * ONE;

    return miss_i && miss_q;
}

static i64 coord(const sv *a, int axis)
{
    return axis ? a->qy : a->qx;
}

/* The vertex where the segment a b crosses coord(axis) = at, which lies
   between their coordinates: the point on the plane, the other coordinate
   and the attributes the exact values rounded once. */
static void cut(const sv *a, const sv *b, int axis, i64 at, sv *out)
{
    i128 num = (i128)at - coord(a, axis), den = (i128)coord(b, axis) - coord(a, axis);
    i128 other;

    if (den < 0) {
        num = -num;
        den = -den;
    }
    num = num < 0 ? 0 : num > den ? den : num;
    if (den == 0)
        den = 1;
    other = (i128)coord(a, !axis) * (den - num) + (i128)coord(b, !axis) * num;
    *out = *a;
    interpolate(a, b, num, den, out);
    if (axis) {
        out->qx = (i64)round_up(other, den);
        out->qy = at;
    } else {
        out->qx = at;
        out->qy = (i64)round_up(other, den);
    }
    out->x = q_pixel_round(out->qx);
    out->y = q_pixel_round(out->qy);
}

/* The view widened by margin pixels and kept inside the limits, in 1/256
   pixels: g[0] x0, g[1] x1, g[2] y0, g[3] y1. */
static void guard(const jbi_split_view *view, int margin, i64 g[4])
{
    i64 x0 = (i64)view->x0 - margin, x1 = (i64)view->x1 + margin;
    i64 y0 = (i64)view->y0 - margin, y1 = (i64)view->y1 + margin;

    g[0] = (x0 < LIMIT_LO ? LIMIT_LO : x0) * ONE;
    g[1] = (x1 > LIMIT_HI ? LIMIT_HI : x1) * ONE;
    g[2] = (y0 < LIMIT_LO ? LIMIT_LO : y0) * ONE;
    g[3] = (y1 > LIMIT_HI ? LIMIT_HI : y1) * ONE;
}

static void clamp_vertex(sv *v)
{
    v->x = clamp_range(v->x);
    v->y = clamp_range(v->y);
}

/* The segment e[0] e[1] inside the guard g, in place; 0 if none of it is. */
static int clip_segment(sv *e, const i64 g[4])
{
    sv m;
    int k;

    for (k = 0; k < 4; ++k) {
        int ax = k / 2, ge = !(k & 1);
        i64 s0 = ge ? coord(&e[0], ax) - g[k] : g[k] - coord(&e[0], ax);
        i64 s1 = ge ? coord(&e[1], ax) - g[k] : g[k] - coord(&e[1], ax);

        if (s0 < 0 && s1 < 0)
            return 0;
        if (s0 < 0) {
            cut(&e[0], &e[1], ax, g[k], &m);
            e[0] = m;
        } else if (s1 < 0) {
            cut(&e[0], &e[1], ax, g[k], &m);
            e[1] = m;
        }
    }
    clamp_vertex(&e[0]);
    clamp_vertex(&e[1]);
    return 1;
}

int jbi_split_line(const jbi_split_view *view, const sv *a, const sv *b, jbi_split_line_fn fn,
                   void *user)
{
    sv e[2], o[2];
    i64 g[4];

    take(&o[0], a, 0);
    take(&o[1], b, 0);
    if (culled_line(view, &o[0], &o[1]))
        return 0;
    if (in_range(&o[0]) && in_range(&o[1]) && near(&o[0], &o[1])) {
        fn(user, &o[0], &o[1]);
        return 1;
    }
    settle(&o[0]);
    settle(&o[1]);
    /* Inside the view and its margin; if that is still too long for the
       GPU, inside the view alone, which is short enough for a view of at
       most 1024 x 512 (split.h). */
    e[0] = o[0];
    e[1] = o[1];
    guard(view, JBI_SPLIT_MARGIN, g);
    if (!clip_segment(e, g))
        return 0;
    if (!near(&e[0], &e[1])) {
        e[0] = o[0];
        e[1] = o[1];
        guard(view, 0, g);
        if (!clip_segment(e, g))
            return 0;
    }
    if (culled_line(view, &e[0], &e[1]))
        return 0;
    fn(user, &e[0], &e[1]);
    return 1;
}

/*
 * split_fuzz: the splitter's invariants (src/geom/split.h, INVARIANTS),
 * checked on its pieces directly, independently of the rasteriser: each
 * piece is sampled with an exact edge-function test and a fixed tie rule
 * for samples on an edge (any rule that gives a sample on an edge to one
 * of the two sides, as the GPU's left-and-top rule does), in the integer
 * format at every pixel and in the sub-pixel format at 2 x 2 samples a
 * pixel, inside the view and a piece's drawing area.
 *
 *   (N)  no sample of one primitive is covered twice, except where two of
 *        its polygons overlap exactly (a crossed quad);
 *   (C)  every sample more than 1.5 pixels inside it is covered;
 *   (E)  two primitives that share an edge exactly cover no sample twice
 *        between them, unless their polygons overlap;
 *   and every piece meets the GPU's limits, with its positions within the
 *   decoder's slack of each other and a pass to draw in.
 *
 * Seeded runs of the shapes that broke earlier designs: large triangles,
 * tips and needles crossing the view's sides in less than a pixel, thin
 * slivers, sub-pixel positions off the integer ones, quads (convex,
 * crossed, thin along the diagonal) as one primitive, pairs sharing an
 * edge beyond the limits and within them (one of them passed through,
 * some thin along it), convex polygons of up to 10 vertices, and views of
 * one and two tiles. Where it leaves a polygon out of a tile (a sliver
 * that rounding would turn over), (C) shows that nothing more than 1.5
 * pixels inside it was lost. Fixed cases (a crossed quad, a thick
 * triangle), a quad this test found and two convex quads whose kept
 * diagonal crosses the clip rectangle are included, and convex quads
 * like them are generated. And a face passed through whose integer and
 * sub-pixel positions part by more than the slack, beside a neighbour
 * that is cut (passed_through_settled).
 */
#include "api_test.h"

#include <stdlib.h>

#include "geom/split.h"

__extension__ typedef __int128 i128;

typedef struct {
    int64_t x, y;
} P;

enum { MAXP = 2048, VW_MAX = 1024, VH_MAX = 512, SUB = 2 };

typedef struct {
    jbi_split_vertex v[4];
    int n, prim;
    unsigned passes;
    int has_area;
    jbi_split_area area;
} piece;

static piece pieces[MAXP];
static int npieces, cur_prim, bad_piece;

static void collect(void *user, int poly, const jbi_split_vertex *v, int n, unsigned passes,
                    const jbi_split_area *area)
{
    piece *p;
    int i;

    (void)user;
    (void)poly;
    if (npieces >= MAXP) {
        ++bad_piece;
        return;
    }
    p = &pieces[npieces++];
    for (i = 0; i < n; ++i) {
        int64_t dx = v[i].qx - (int64_t)v[i].x * 256, dy = v[i].qy - (int64_t)v[i].y * 256;

        p->v[i] = v[i];
        bad_piece += dx > 288 || dx < -288 || dy > 288 || dy < -288;
    }
    p->n = n;
    p->prim = cur_prim;
    p->passes = passes;
    p->has_area = area != NULL;
    if (area != NULL)
        p->area = *area;
    bad_piece += passes == 0 || passes > 3 || (n != 3 && n != 4);
    if (n == 3)
        bad_piece += !jbi_split_triangle_fits(&v[0], &v[1], &v[2]);
    else if (n == 4)
        bad_piece += !jbi_split_triangle_fits(&v[0], &v[1], &v[2]) ||
                     !jbi_split_triangle_fits(&v[1], &v[2], &v[3]);
}

static P pos(const jbi_split_vertex *v, int f)
{
    P p;

    p.x = f ? v->qx : v->x;
    p.y = f ? v->qy : v->y;
    return p;
}

static i128 cross(P a, P b, P c)
{
    return (i128)(b.x - a.x) * (c.y - a.y) - (i128)(c.x - a.x) * (b.y - a.y);
}

static int edge_in(P a, P b, P s)
{
    i128 e = cross(a, b, s);

    if (e != 0)
        return e > 0;
    /* On it: the left and top edges in (the interior to the right of, or
       below, the edge). */
    return b.y < a.y || (b.y == a.y && b.x > a.x);
}

static int separated(const P *t, const P *o)
{
    i128 st = cross(t[0], t[1], t[2]);
    int k, j;

    for (k = 0; k < 3; ++k) {
        int out = 1;

        for (j = 0; j < 3 && out; ++j) {
            i128 c = cross(t[k], t[(k + 1) % 3], o[j]);

            out = st > 0 ? c <= 0 : c >= 0;
        }
        if (out)
            return 1;
    }
    return 0;
}

/* Whether two triangles' interiors overlap (exact). */
static int tris_overlap(const P *a, const P *b)
{
    if (cross(a[0], a[1], a[2]) == 0 || cross(b[0], b[1], b[2]) == 0)
        return 0;
    return !separated(a, b) && !separated(b, a);
}

static int tri_in(P a, P b, P c, P s)
{
    i128 o = cross(a, b, c);

    if (o == 0)
        return 0;
    if (o < 0) {
        P t = b;

        b = c;
        c = t;
    }
    return edge_in(a, b, s) && edge_in(b, c, s) && edge_in(c, a, s);
}

/* A primitive of the test: up to 2 polygons. */
typedef struct {
    jbi_split_vertex v[2][JBI_SPLIT_MAX_IN];
    int n[2], count;
} prim;

static uint8_t cov[2][VW_MAX * SUB * VH_MAX * SUB]; /* by primitive */

static double dsqrt(double sq)
{
    double t = sq > 1 ? sq : 1;
    int i;

    for (i = 0; i < 60; ++i)
        t = 0.5 * (t + sq / t);
    return sq <= 0 ? 0 : t;
}

static double seg_dist(double px, double py, double ax, double ay, double bx, double by)
{
    double dx = bx - ax, dy = by - ay, len = dx * dx + dy * dy, t, ex, ey;

    t = len > 0 ? ((px - ax) * dx + (py - ay) * dy) / len : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    ex = ax + t * dx - px;
    ey = ay + t * dy - py;
    return dsqrt(ex * ex + ey * ey);
}

/* Strictly inside convex polygon q (n vertices) in format f. */
static int poly_in(const jbi_split_vertex *q, int n, int f, P s)
{
    int i, pos_ = 0, neg = 0;

    for (i = 0; i < n; ++i) {
        i128 c = cross(pos(&q[i], f), pos(&q[(i + 1) % n], f), s);

        pos_ += c > 0;
        neg += c < 0;
    }
    return pos_ == n || neg == n;
}

/* Inside polygon q by the tie rule (its fan's triangles, which tile it):
   how the console would count it. */
static int poly_rule(const jbi_split_vertex *q, int n, int f, P s)
{
    int i;

    for (i = 1; i + 1 < n; ++i)
        if (tri_in(pos(&q[0], f), pos(&q[i], f), pos(&q[i + 1], f), s))
            return 1;
    return 0;
}

/* The distance from (px, py) to polygon q's boundary, in pixels. */
static double poly_dist(const jbi_split_vertex *q, int n, int f, double px, double py)
{
    double one = f ? 256.0 : 1.0, d = 1e18;
    int i;

    for (i = 0; i < n; ++i) {
        P a = pos(&q[i], f), b = pos(&q[(i + 1) % n], f);
        double dd = seg_dist(px, py, a.x / one, a.y / one, b.x / one, b.y / one);

        d = dd < d ? dd : d;
    }
    return d;
}

static int same_point(const jbi_split_vertex *a, const jbi_split_vertex *b)
{
    return a->qx == b->qx && a->qy == b->qy && a->x == b->x && a->y == b->y;
}

/* Whether edge i of polygon k of primitive pr is the other polygon's too. */
static int inner_edge(const prim *pr, int k, int i)
{
    const jbi_split_vertex *p0 = &pr->v[k][i], *p1 = &pr->v[k][(i + 1) % pr->n[k]];
    int l = 1 - k, j;

    if (pr->count < 2)
        return 0;
    for (j = 0; j < pr->n[l]; ++j) {
        const jbi_split_vertex *q0 = &pr->v[l][j], *q1 = &pr->v[l][(j + 1) % pr->n[l]];

        if ((same_point(p0, q0) && same_point(p1, q1)) ||
            (same_point(p0, q1) && same_point(p1, q0)))
            return 1;
    }
    return 0;
}

typedef struct {
    int twice, missing, cross_twice, cases, dropped;
} tally;

static void report(const char *what, const prim *pr, int np, int count)
{
    int a, k, i;

    if (count > 4)
        return;
    printf("  %s:", what);
    for (a = 0; a < np; ++a) {
        printf(" [");
        for (k = 0; k < pr[a].count; ++k) {
            printf(k ? " |" : "");
            for (i = 0; i < pr[a].n[k]; ++i)
                printf(" (%d,%d q%lld,%lld)", pr[a].v[k][i].x, pr[a].v[k][i].y,
                       (long long)pr[a].v[k][i].qx, (long long)pr[a].v[k][i].qy);
        }
        printf(" ]");
    }
    printf("\n");
}

/* The samples looked at: the pixels x0..x0 + w - 1 by y0..y0 + h - 1,
   within the view. */
typedef struct {
    int x0, y0, w, h;
} window;

static P sample(const window *view, int f, int x, int y)
{
    P s;

    if (f) {
        s.x = (int64_t)view->x0 * 256 + (int64_t)x * 256 / SUB + 64;
        s.y = (int64_t)view->y0 * 256 + (int64_t)y * 256 / SUB + 64;
    } else {
        s.x = view->x0 + x;
        s.y = view->y0 + y;
    }
    return s;
}

static int clampi(int64_t a, int lo, int hi)
{
    return a < lo ? lo : a > hi ? hi : (int)a;
}

/* Splits np primitives (2: sharing an edge) in view and checks them. */
static void run(const jbi_split_view *full, const window *win, const prim *pr, int np, tally *t)
{
    window wv, *view = &wv;
    int w, h, a, f, i, k, x, y;
    int twice = 0, missing = 0, cross_twice = 0;

    /* The window within the view. */
    wv.x0 = win->x0 > full->x0 ? win->x0 : full->x0;
    wv.y0 = win->y0 > full->y0 ? win->y0 : full->y0;
    w = (win->x0 + win->w - 1 < full->x1 ? win->x0 + win->w - 1 : full->x1) - wv.x0 + 1;
    h = (win->y0 + win->h - 1 < full->y1 ? win->y0 + win->h - 1 : full->y1) - wv.y0 + 1;
    wv.w = w;
    wv.h = h;
    npieces = 0;
    for (a = 0; a < np; ++a) {
        const jbi_split_vertex *vv[2];
        int dropped = 0;

        cur_prim = a;
        for (k = 0; k < pr[a].count; ++k)
            vv[k] = pr[a].v[k];
        jbi_split_polys(full, vv, pr[a].n, pr[a].count, collect, NULL, &dropped);
        t->dropped += dropped;
        if (dropped && getenv("SPLIT_FUZZ_VERBOSE"))
            report("left out", &pr[a], 1, 1);
    }
    for (f = 0; f < 2 && w > 0 && h > 0; ++f) {
        int sub = f ? SUB : 1, sw = w * sub, sh = h * sub, overlapping[2] = {0, 0};
        int64_t one = f ? 256 : 1;

        for (a = 0; a < np; ++a)
            if (pr[a].count == 2) {
                int i0, i1;

                for (i0 = 1; i0 + 1 < pr[a].n[0]; ++i0)
                    for (i1 = 1; i1 + 1 < pr[a].n[1]; ++i1) {
                        P t0[3], t1[3];

                        t0[0] = pos(&pr[a].v[0][0], f);
                        t0[1] = pos(&pr[a].v[0][i0], f);
                        t0[2] = pos(&pr[a].v[0][i0 + 1], f);
                        t1[0] = pos(&pr[a].v[1][0], f);
                        t1[1] = pos(&pr[a].v[1][i1], f);
                        t1[2] = pos(&pr[a].v[1][i1 + 1], f);
                        overlapping[a] |= tris_overlap(t0, t1);
                    }
            }

        for (a = 0; a < np; ++a)
            memset(cov[a], 0, (size_t)sw * sh);
        for (i = 0; i < npieces; ++i) {
            const piece *p = &pieces[i];
            int tri;

            if (!(p->passes & (f ? JBI_SPLIT_PASS_PRECISE : JBI_SPLIT_PASS_1X)))
                continue;
            for (tri = 0; tri < p->n - 2; ++tri) {
                P c0 = pos(&p->v[tri], f), c1 = pos(&p->v[tri + 1], f), c2 = pos(&p->v[tri + 2], f);
                int64_t bx0 = c0.x, bx1 = c0.x, by0 = c0.y, by1 = c0.y;
                int ax0 = 0, ax1 = sw - 1, ay0 = 0, ay1 = sh - 1, sx0, sx1, sy0, sy1;

                bx0 = c1.x < bx0 ? c1.x : bx0, bx0 = c2.x < bx0 ? c2.x : bx0;
                bx1 = c1.x > bx1 ? c1.x : bx1, bx1 = c2.x > bx1 ? c2.x : bx1;
                by0 = c1.y < by0 ? c1.y : by0, by0 = c2.y < by0 ? c2.y : by0;
                by1 = c1.y > by1 ? c1.y : by1, by1 = c2.y > by1 ? c2.y : by1;
                if (p->has_area) {
                    ax0 = (p->area.x0 - view->x0) * sub;
                    ax1 = (p->area.x1 - view->x0 + 1) * sub - 1;
                    ay0 = (p->area.y0 - view->y0) * sub;
                    ay1 = (p->area.y1 - view->y0 + 1) * sub - 1;
                    ax0 = ax0 < 0 ? 0 : ax0;
                    ay0 = ay0 < 0 ? 0 : ay0;
                    ax1 = ax1 > sw - 1 ? sw - 1 : ax1;
                    ay1 = ay1 > sh - 1 ? sh - 1 : ay1;
                    if (ax0 > ax1 || ay0 > ay1)
                        continue;
                }
                sx0 = clampi((bx0 / one - 1 - view->x0) * sub, ax0, ax1);
                sx1 = clampi((bx1 / one + 2 - view->x0) * sub, ax0, ax1);
                sy0 = clampi((by0 / one - 1 - view->y0) * sub, ay0, ay1);
                sy1 = clampi((by1 / one + 2 - view->y0) * sub, ay0, ay1);
                for (y = sy0; y <= sy1; ++y)
                    for (x = sx0; x <= sx1; ++x)
                        if (tri_in(c0, c1, c2, sample(view, f, x, y))) {
                            uint8_t *c = &cov[p->prim][(size_t)y * sw + x];

                            *c = (uint8_t)(*c < 255 ? *c + 1 : 255);
                        }
            }
        }
        for (y = 0; y < sh; ++y)
            for (x = 0; x < sw; ++x) {
                P s = sample(view, f, x, y);
                int nsrc_all = 0, got_all = 0;
                double px = f ? s.x / 256.0 : (double)s.x, py = f ? s.y / 256.0 : (double)s.y;

                for (a = 0; a < np; ++a) {
                    int nsrc = 0, nrule = 0, nloose = 0, got = cov[a][(size_t)y * sw + x];
                    double d = 1e18;

                    for (k = 0; k < pr[a].count; ++k) {
                        int in = poly_in(pr[a].v[k], pr[a].n[k], f, s);

                        nsrc += in;
                        nrule += poly_rule(pr[a].v[k], pr[a].n[k], f, s);
                        if (overlapping[a]) /* a unit around each, where they overlap */
                            nloose += in || poly_dist(pr[a].v[k], pr[a].n[k], f, px, py) <
                                                (f ? 0.02 : 1.0);
                    }
                    nsrc_all += nrule;
                    got_all += got;
                    if (got > (nloose > 1 ? nloose : 1) && !twice++ && getenv("SPLIT_FUZZ_VERBOSE"))
                        printf("    twice at f%d (%lld, %lld): %d of %d\n", f, (long long)s.x,
                               (long long)s.y, got, nrule);
                    /* A polygon cannot reach column 1023 (jitterbug.h, 2D
                       drawing). */
                    if (nsrc == 0 || got >= 1 || px >= 1023)
                        continue;
                    for (k = 0; k < pr[a].count; ++k)
                        for (i = 0; i < pr[a].n[k]; ++i) {
                            const jbi_split_vertex *e0 = &pr[a].v[k][i],
                                                   *e1 = &pr[a].v[k][(i + 1) % pr[a].n[k]];
                            P q0 = pos(e0, f), q1 = pos(e1, f);
                            double dd;

                            if (!overlapping[a] && inner_edge(&pr[a], k, i))
                                continue;
                            dd = seg_dist(px, py, (double)q0.x / (double)one,
                                          (double)q0.y / (double)one, (double)q1.x / (double)one,
                                          (double)q1.y / (double)one);
                            d = dd < d ? dd : d;
                        }
                    if (d > 1.5 && !missing++ && getenv("SPLIT_FUZZ_VERBOSE"))
                        printf("    missing at f%d (%lld, %lld)\n", f, (long long)s.x,
                               (long long)s.y);
                }
                if (np == 2 && got_all > (nsrc_all > 1 ? nsrc_all : 1))
                    ++cross_twice;
                /* A pair sharing an edge: their union covered well inside,
                   along the shared edge too. */
                if (np == 2 && got_all == 0 && px < 1023 &&
                    (poly_in(pr[0].v[0], 3, f, s) || poly_in(pr[1].v[0], 3, f, s) ||
                     poly_rule(pr[0].v[0], 3, f, s) || poly_rule(pr[1].v[0], 3, f, s))) {
                    double d = 1e18;
                    int m;

                    for (a = 0; a < 2; ++a)
                        for (m = 0; m < 3; ++m) {
                            P q0 = pos(&pr[a].v[0][m], f), q1 = pos(&pr[a].v[0][(m + 1) % 3], f);
                            double dd;

                            if (m == 0) /* a b, the shared edge */
                                continue;
                            dd = seg_dist(px, py, (double)q0.x / (double)one,
                                          (double)q0.y / (double)one, (double)q1.x / (double)one,
                                          (double)q1.y / (double)one);
                            d = dd < d ? dd : d;
                        }
                    if (d > 1.5 && !missing++ && getenv("SPLIT_FUZZ_VERBOSE"))
                        printf("    union missing at f%d (%lld, %lld)\n", f, (long long)s.x,
                               (long long)s.y);
                }
            }
    }
    t->cases++;
    if (w <= 0 || h <= 0)
        return;
    if ((twice || missing || cross_twice) && getenv("SPLIT_FUZZ_VERBOSE"))
        printf("    in a %d x %d view, window (%d, %d) %d x %d\n", full->x1 - full->x0 + 1,
               full->y1 - full->y0 + 1, win->x0, win->y0, win->w, win->h);
    if (twice)
        report("twice", pr, np, ++t->twice);
    if (missing)
        report("missing", pr, np, ++t->missing);
    if (cross_twice && !twice)
        report("shared edge twice", pr, np, ++t->cross_twice);
}

static jbi_split_vertex vx(int64_t x, int64_t y)
{
    jbi_split_vertex v;

    memset(&v, 0, sizeof v);
    v.x = (int)(x < -0x7fffffff ? -0x7fffffff : x > 0x7fffffff ? 0x7fffffff : x);
    v.y = (int)(y < -0x7fffffff ? -0x7fffffff : y > 0x7fffffff ? 0x7fffffff : y);
    v.qx = x * 256;
    v.qy = y * 256;
    v.r = 64;
    return v;
}

static int span(gen *g, int s)
{
    return rnd_below(g, 2 * s + 1) - s;
}

/* A point far from the view, mostly beyond the limits from it. */
static jbi_split_vertex far_point(gen *g, int w, int h)
{
    int s = rnd_below(g, 3) ? 3000 : 60000;

    return vx(w / 2 + span(g, s), h / 2 + span(g, s));
}

/* The band [lo, hi) widened evenly to size inside -1024..1023, as the
   splitter widens a tile into its guard band (split.c, widen). */
static void widen(int lo, int hi, int size, int *a, int *b)
{
    int extra = size - (hi - lo);

    *a = lo - extra / 2;
    *b = hi + (extra - extra / 2);
    if (*a < -1024) {
        *b += -1024 - *a;
        *a = -1024;
    }
    if (*b > 1023) {
        *a -= *b - 1023;
        *b = 1023;
    }
    *a = *a < -1024 ? -1024 : *a;
}

/* The lines a w x h view's polygons can be cut on, or that bound what
   they draw: its sides, its tiles' seams and guard bands (split.h). */
static int lines(int n, int size, int limit, int *out)
{
    int tiles = n > limit ? 2 : 1, t, m = 0;

    out[m++] = 0;
    out[m++] = n;
    for (t = 0; t < tiles; ++t) {
        int a, b;

        widen(n * t / tiles, n * (t + 1) / tiles, size, &a, &b);
        out[m++] = a;
        out[m++] = b;
        if (t > 0)
            out[m++] = n * t / tiles;
    }
    return m;
}

/* A point near one of those lines (within d pixels, either side), inside
   the guard band along it. */
static jbi_split_vertex side_point(gen *g, int w, int h, int d)
{
    int xs[8], ys[8], nx = lines(w, 1023, 1023, xs), ny = lines(h, 511, 511, ys), gx0, gx1, gy0,
        gy1;
    int off = span(g, d);

    widen(0, w, 1023, &gx0, &gx1);
    widen(0, h > 511 ? h / 2 : h, 511, &gy0, &gy1);
    if (rnd_below(g, 2))
        return vx(xs[rnd_below(g, nx)] + off, gy0 + rnd_below(g, gy1 - gy0 + 1));
    return vx(gx0 + rnd_below(g, gx1 - gx0 + 1), ys[rnd_below(g, ny)] + off);
}

static void offset_q(gen *g, jbi_split_vertex *v)
{
    v->qx += span(g, 255);
    v->qy += span(g, 255);
}

/* A random shape of one kind. */
static void shape(gen *g, int kind, int w, int h, prim *pr, int *np)
{
    int k;

    memset(pr, 0, 2 * sizeof *pr);
    pr[0].count = 1;
    pr[0].n[0] = 3;
    *np = 1;
    switch (kind) {
    case 0: /* large */
        for (k = 0; k < 3; ++k)
            pr[0].v[0][k] = far_point(g, w, h);
        break;
    case 1: { /* a tip across a side: two far ends a few pixels apart */
        jbi_split_vertex b = far_point(g, w, h);

        pr[0].v[0][0] = side_point(g, w, h, 3);
        pr[0].v[0][1] = b;
        pr[0].v[0][2] = vx((int64_t)b.x + span(g, 4), (int64_t)b.y + span(g, 4));
        break;
    }
    case 2: { /* a needle from far through the view to far */
        jbi_split_vertex a = far_point(g, w, h);
        int64_t dx = (int64_t)w / 2 - a.x, dy = (int64_t)h / 2 - a.y;

        pr[0].v[0][0] = a;
        pr[0].v[0][1] = vx(a.x + 2 * dx + span(g, 3), a.y + 2 * dy + span(g, 3));
        pr[0].v[0][2] = vx(a.x + 2 * dx + span(g, 3), a.y + 2 * dy + span(g, 3));
        break;
    }
    case 3: { /* nearly collinear, the middle one near a side */
        jbi_split_vertex a = far_point(g, w, h), m = side_point(g, w, h, 2);
        int64_t dx = (int64_t)m.x - a.x, dy = (int64_t)m.y - a.y;

        pr[0].v[0][0] = a;
        pr[0].v[0][1] = vx((int64_t)m.x + span(g, 1), (int64_t)m.y + span(g, 1));
        pr[0].v[0][2] = vx(m.x + dx + span(g, 2), m.y + dy + span(g, 2));
        break;
    }
    case 4: /* sub-pixel positions off the integer ones, any of the above */
        shape(g, rnd_below(g, 4), w, h, pr, np);
        for (k = 0; k < 3; ++k)
            offset_q(g, &pr[0].v[0][k]);
        break;
    case 5: { /* a quad as its GPU triangles, one primitive */
        jbi_split_vertex q[4];
        int kindq = rnd_below(g, 3);

        for (k = 0; k < 4; ++k)
            q[k] = rnd_below(g, 2) ? far_point(g, w, h) : side_point(g, w, h, 40);
        if (kindq == 0) /* convex: a parallelogram */
            q[3] = vx((int64_t)q[1].x + q[2].x - q[0].x, (int64_t)q[1].y + q[2].y - q[0].y);
        else if (kindq == 1) /* thin along the diagonal */
            q[0] = vx(((int64_t)q[1].x + q[2].x) / 2 + span(g, 2),
                      ((int64_t)q[1].y + q[2].y) / 2 + span(g, 2));
        pr[0].count = 2;
        pr[0].n[1] = 3;
        pr[0].v[0][0] = q[0], pr[0].v[0][1] = q[1], pr[0].v[0][2] = q[2];
        pr[0].v[1][0] = q[1], pr[0].v[1][1] = q[2], pr[0].v[1][2] = q[3];
        break;
    }
    case 9: { /* a convex quad whose diagonal meets the limits and crosses
                 the lines its pieces are cut on */
        jbi_split_vertex q[4];
        int tries;

        for (tries = 0; tries < 200; ++tries) {
            int64_t dx, dy;
            P a, b, c, d;
            i128 o1, o2, o3, o4;

            /* One end a pixel or so from a line, so that cuts there lie
               within a unit of each other. */
            q[2] = side_point(g, w, h, 3);
            q[1] = rnd_below(g, 2) ? side_point(g, w, h, 40)
                                   : vx((int64_t)q[2].x + span(g, 1000),
                                        (int64_t)q[2].y + span(g, 500));
            dx = (int64_t)q[2].x - q[1].x;
            dy = (int64_t)q[2].y - q[1].y;
            if (dx > 1023 || dx < -1023 || dy > 511 || dy < -511 || (dx == 0 && dy == 0))
                continue;
            /* The other two anywhere (convex below), near a line or not. */
            q[0] = rnd_below(g, 2) ? side_point(g, w, h, 600) : far_point(g, w, h);
            q[3] = rnd_below(g, 2) ? side_point(g, w, h, 600) : far_point(g, w, h);
            a.x = q[0].x, a.y = q[0].y, b.x = q[1].x, b.y = q[1].y;
            c.x = q[3].x, c.y = q[3].y, d.x = q[2].x, d.y = q[2].y;
            o1 = cross(a, b, c), o2 = cross(b, c, d), o3 = cross(c, d, a), o4 = cross(d, a, b);
            if ((o1 > 0 && o2 > 0 && o3 > 0 && o4 > 0) || (o1 < 0 && o2 < 0 && o3 < 0 && o4 < 0))
                break; /* convex, going 0 1 3 2 */
        }
        if (tries == 200) {
            shape(g, 0, w, h, pr, np);
            return;
        }
        pr[0].count = 2;
        pr[0].n[1] = 3;
        pr[0].v[0][0] = q[0], pr[0].v[0][1] = q[1], pr[0].v[0][2] = q[2];
        pr[0].v[1][0] = q[1], pr[0].v[1][1] = q[2], pr[0].v[1][2] = q[3];
        break;
    }
    case 8: { /* a convex polygon of 4 to 10 vertices, on an ellipse */
        static const int cs[16] = {256, 236, 181, 98, 0, -98, -181, -236,
                                   -256, -236, -181, -98, 0, 98, 181, 236};
        int n = 4 + rnd_below(g, 7), rx = 200 + rnd_below(g, 3000), ry = 200 + rnd_below(g, 3000);
        int cx = w / 2 + span(g, 2000), cy = h / 2 + span(g, 2000), used[16] = {0}, m = 0;

        while (m < n) {
            int a = rnd_below(g, 16);

            if (!used[a])
                used[a] = 1, ++m;
        }
        m = 0;
        for (k = 0; k < 16; ++k)
            if (used[k])
                pr[0].v[0][m++] = vx(cx + (int64_t)rx * cs[k] / 256,
                                     cy + (int64_t)ry * cs[(k + 4) % 16] / 256);
        pr[0].n[0] = n;
        break;
    }
    case 6:   /* two primitives sharing an edge beyond the limits */
    case 7: { /* or within them, maybe one passed through */
        jbi_split_vertex a, b, c, d;
        int64_t ex, ey;

        if (kind == 6) {
            a = far_point(g, w, h);
            b = rnd_below(g, 2) ? far_point(g, w, h) : side_point(g, w, h, 30);
        } else {
            a = side_point(g, w, h, 300);
            b = vx((int64_t)a.x + span(g, 900), (int64_t)a.y + span(g, 450));
        }
        ex = (int64_t)b.x - a.x;
        ey = (int64_t)b.y - a.y;
        c = rnd_below(g, 3) ? far_point(g, w, h) : side_point(g, w, h, 300);
        if (rnd_below(g, 3) == 0) /* thin: near the edge */
            c = vx(a.x + ex * rnd_below(g, 101) / 100 + span(g, 2),
                   a.y + ey * rnd_below(g, 101) / 100 + span(g, 2));
        d = rnd_below(g, 2) ? far_point(g, w, h) : side_point(g, w, h, 300);
        if (rnd_below(g, 3) == 0)
            d = vx(a.x + ex * rnd_below(g, 101) / 100 + span(g, 2),
                   a.y + ey * rnd_below(g, 101) / 100 + span(g, 2));
        {
            P pa = {a.x, a.y}, pb = {b.x, b.y}, pc = {c.x, c.y}, pd = {d.x, d.y};
            i128 sc = cross(pa, pb, pc), sd = cross(pa, pb, pd);

            if (sc == 0 || sd == 0 || (sc > 0) == (sd > 0)) {
                shape(g, 0, w, h, pr, np);
                return;
            }
        }
        if (rnd_below(g, 3) == 0) {
            offset_q(g, &a);
            offset_q(g, &b);
            offset_q(g, &c);
            offset_q(g, &d);
        }
        pr[0].v[0][0] = a, pr[0].v[0][1] = b, pr[0].v[0][2] = c;
        pr[1].count = 1;
        pr[1].n[0] = 3;
        pr[1].v[0][0] = b, pr[1].v[0][1] = a, pr[1].v[0][2] = d;
        *np = 2;
        break;
    }
    }
}

static void fixtures(tally *t)
{
    /* A crossed quad in a 600 x 512 view; its first triangle fits. */
    static const int f5[4][2] = {{100, 300}, {100, 100}, {450, 100}, {500, 1000}};
    /* A thick triangle in 1024 x 512. */
    static const int f6[3][2] = {{1417, -1188}, {1021, -410}, {-813, 1621}};
    jbi_split_view v600 = {0, 0, 599, 511, 0, 0}, v1024 = {0, 0, 1023, 511, 0, 0};
    prim pr[2];
    int k;

    memset(pr, 0, sizeof pr);
    pr[0].count = 2;
    pr[0].n[0] = pr[0].n[1] = 3;
    for (k = 0; k < 3; ++k) {
        pr[0].v[0][k] = vx(f5[k][0], f5[k][1]);
        pr[0].v[1][k] = vx(f5[k + 1][0], f5[k + 1][1]);
    }
    {
        window all = {0, 0, 600, 512};

        run(&v600, &all, pr, 1, t);
    }
    /* A quad whose diagonal meets the limits and crosses a plane where one
       triangle's cut cannot go inward (its neighbour on the side is too
       close): both triangles must then cut it at the same point, with no
       ear, or one's piece covers the other's ear (found by this test). */
    {
        static const int q[4][2] = {{537, -237}, {64, -6}, {-498, 268}, {-971, 499}};
        jbi_split_view v48 = {0, 0, 47, 31, 0, 0};
        window corner = {0, 0, 48, 32};

        memset(pr, 0, sizeof pr);
        pr[0].count = 2;
        pr[0].n[0] = pr[0].n[1] = 3;
        for (k = 0; k < 3; ++k) {
            pr[0].v[0][k] = vx(q[k][0], q[k][1]);
            pr[0].v[1][k] = vx(q[k + 1][0], q[k + 1][1]);
        }
        run(&v48, &corner, pr, 1, t);
    }
    /* A convex quad (GPU order) whose shared kept diagonal, reset
       to its nearest point, passed another kept cut of the upper triangle,
       which was then left out; and one whose diagonal crosses two sides
       and was reset at one only, leaving a crack along it. */
    {
        static const int r6[2][4][2] = {{{-400, 572}, {-340, 470}, {673, 60}, {0, 120}},
                                        {{500, 100}, {-10, 500}, {200, -1}, {-40, 519}}};
        jbi_split_view v320 = {0, 0, 319, 239, 0, 0}, v1023 = {0, 0, 1022, 510, 0, 0};
        window w320 = {0, 0, 320, 240}, w1023 = {0, 0, 1023, 511};
        int r;

        for (r = 0; r < 2; ++r) {
            memset(pr, 0, sizeof pr);
            pr[0].count = 2;
            pr[0].n[0] = pr[0].n[1] = 3;
            for (k = 0; k < 3; ++k) {
                pr[0].v[0][k] = vx(r6[r][k][0], r6[r][k][1]);
                pr[0].v[1][k] = vx(r6[r][k + 1][0], r6[r][k + 1][1]);
            }
            run(r ? &v1023 : &v320, r ? &w1023 : &w320, pr, 1, t);
        }
    }
    memset(pr, 0, sizeof pr);
    pr[0].count = 1;
    pr[0].n[0] = 3;
    for (k = 0; k < 3; ++k)
        pr[0].v[0][k] = vx(f6[k][0], f6[k][1]);
    {
        window all = {0, 0, 1024, 512};

        run(&v1024, &all, pr, 1, t);
    }
}

/*
 * Where the precise pass draws a vertex of a piece, as one number: its
 * sub-pixel position within the decoder's slack of its integer one in both
 * coordinates, else its integer one (the decoder then ignores the
 * sub-pixel position, both coordinates), in 1/256 pixels, x and y packed.
 */
static int64_t drawn_at(const jbi_split_vertex *v)
{
    int64_t dx = v->qx - (int64_t)v->x * 256, dy = v->qy - (int64_t)v->y * 256;
    int far = dx > 288 || dx < -288 || dy > 288 || dy < -288;
    int64_t x = far ? (int64_t)v->x * 256 : v->qx, y = far ? (int64_t)v->y * 256 : v->qy;

    return x * 1000000 + y;
}

/*
 * A 3D face's integer and sub-pixel positions are separate projections,
 * which far off the frame can part by more than the decoder's slack at a
 * vertex: passed through, the precise pass draws that vertex at its
 * integer position (as console code draws it). A neighbour that the
 * splitter cuts settled the same vertex to its sub-pixel position, and a
 * crack opened along their shared edge (the demo's sea, frame 960 at 2x
 * with precise vertices, a pixel on the frame's last row). The fixture is
 * that face, an exact GTE quad passed through, and the cut neighbour
 * sharing its edge: the 1x pass gets the quad as it is, and the precise
 * pass draws the shared vertex at one place in both.
 */
static void passed_through_settled(void)
{
    static const int quad[4][4] = {{30, 117, 7805, 30040},
                                   {-165, 182, -42064, 46633},
                                   {-340, 472, -86739, 121051},
                                   {64, 233, 16618, 59685}};
    static const int tri[3][4] = {{64, 233, 16618, 59685}, {-340, 472, -86739, 121051}, {236, 812, 60697, 207962}};
    jbi_split_view view = {0, 0, 319, 239, 0, 0};
    jbi_split_vertex q[4], t[3];
    const jbi_split_vertex *pv;
    int i, k, n, whole_1x = 0, seen = 0, bad = bad_piece;
    int64_t quad_at = 0, neighbour_at = 1;

    memset(q, 0, sizeof q);
    memset(t, 0, sizeof t);
    for (i = 0; i < 4; ++i)
        q[i].x = quad[i][0], q[i].y = quad[i][1], q[i].qx = quad[i][2], q[i].qy = quad[i][3];
    for (i = 0; i < 3; ++i)
        t[i].x = tri[i][0], t[i].y = tri[i][1], t[i].qx = tri[i][2], t[i].qy = tri[i][3];
    npieces = 0;
    pv = q, n = 4;
    jbi_split_polys(&view, &pv, &n, 1, collect, NULL, NULL);
    for (k = 0; k < npieces; ++k) {
        const piece *p = &pieces[k];
        for (i = 0; i < p->n; ++i) {
            if ((p->passes & JBI_SPLIT_PASS_1X) && p->v[i].qx == -86739)
                whole_1x += p->v[i].x == -340 && p->v[i].y == 472;
            if ((p->passes & JBI_SPLIT_PASS_PRECISE) && p->v[i].x == -340 && p->v[i].y == 472)
                quad_at = drawn_at(&p->v[i]), ++seen;
        }
    }
    npieces = 0;
    pv = t, n = 3;
    jbi_split_polys(&view, &pv, &n, 1, collect, NULL, NULL);
    for (k = 0; k < npieces; ++k)
        for (i = 0; i < pieces[k].n; ++i)
            if ((pieces[k].passes & JBI_SPLIT_PASS_PRECISE) && pieces[k].v[i].y >= 471 &&
                pieces[k].v[i].y <= 473 && pieces[k].v[i].x >= -341 && pieces[k].v[i].x <= -338)
                neighbour_at = drawn_at(&pieces[k].v[i]), ++seen;
    npieces = 0;
    bad_piece = bad; /* the quad passed through is beyond the slack as given */
    check("passed through: the 1x pass gets the quad as it is", whole_1x, 1);
    check("both: the shared vertex in the precise pass's pieces", seen >= 2, 1);
    check("cut beside passed through: the precise pass draws the shared vertex at one place", (long long)neighbour_at,
          (long long)quad_at);
}

int main(void)
{
    /* Views, and how many shapes in each: a small view (its guard band far
       around it), views whose sides are the planes polygons are cut on
       (1023 x 511: no room for a guard band) and views of two tiles (512
       high), sampled in a window around a random place near a plane. */
    static const struct {
        int w, h, runs, win, kind; /* kind -1: any */
    } views[] = {{48, 32, 12000, 0, -1},   {64, 64, 3000, 0, -1},    {1023, 511, 6000, 48, -1},
                 {300, 512, 3000, 48, -1}, {1024, 512, 1500, 48, -1}, {320, 240, 60, 0, -1},
                 /* more convex quads with a kept diagonal across a line */
                 {48, 32, 4000, 0, 9},     {1023, 511, 3000, 48, 9},  {320, 240, 40, 0, 9}};
    gen g = {20261123u, 0, 0};
    tally t;
    size_t vi;
    int i;
    /* SPLIT_FUZZ_KIND n: only shapes of kind n, to look for a case. */
    const char *only = getenv("SPLIT_FUZZ_KIND");
    int kind = only != NULL ? atoi(only) : -1;

    passed_through_settled();
    memset(&t, 0, sizeof t);
    fixtures(&t);
    check("the fixtures: no sample twice", t.twice, 0);
    check("the fixtures: none missing inside", t.missing, 0);
    for (vi = 0; vi < sizeof views / sizeof views[0]; ++vi) {
        jbi_split_view view;
        int w = views[vi].w, h = views[vi].h;

        memset(&view, 0, sizeof view);
        view.x1 = w - 1;
        view.y1 = h - 1;
        for (i = 0; i < views[vi].runs; ++i) {
            prim pr[2];
            window win = {0, 0, w, h};
            int np;

            shape(&g, kind >= 0 ? kind : views[vi].kind >= 0 ? views[vi].kind : rnd_below(&g, 10), w, h, pr, &np);
            if (views[vi].win) {
                /* Around a point near a side or seam of the view. */
                jbi_split_vertex c = side_point(&g, w, h, 8);
                int ww = views[vi].win;

                win.x0 = c.x - ww / 2;
                win.y0 = c.y - ww / 2;
                win.w = win.h = ww;
            }
            run(&view, &win, pr, np, &t);
        }
    }
    printf("split_fuzz: %d cases, %d polygons left out in a tile and format\n", t.cases,
           t.dropped);
    check("pieces within the limits, the slack and a pass", bad_piece, 0);
    check("(N) no sample of a primitive twice", t.twice, 0);
    check("(C) every sample well inside covered", t.missing, 0);
    check("(E) no sample twice across a shared edge", t.cross_twice, 0);
    return finish("split_fuzz");
}

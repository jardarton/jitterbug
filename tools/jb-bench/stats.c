/*
 * jb-bench: counting what a workload sends (workload.h, wl_stats). Only the
 * statistics pass runs this; timed frames do not.
 */
#include "workload.h"

static int iabs(int a)
{
    return a < 0 ? -a : a;
}

/* The console skips a polygon or line with two vertices more than 1023
   pixels apart in x or 511 in y (include/jitterbug.h, Drawing). */
static int too_big(const int *x, const int *y, int n)
{
    int i, j;

    for (i = 0; i < n; ++i)
        for (j = i + 1; j < n; ++j)
            if (iabs(x[i] - x[j]) > 1023 || iabs(y[i] - y[j]) > 511)
                return 1;
    return 0;
}

/* Edge function at a point, in doubled coordinates (pixel centres odd). */
static long long edge(long long ax, long long ay, long long bx, long long by, long long px,
                      long long py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* A point exactly on an edge belongs to one side only, so pixels on an
   edge two triangles share are counted once. */
static int owns(long long e, int dx, int dy)
{
    return e > 0 || (e == 0 && (dy > 0 || (dy == 0 && dx < 0)));
}

/* Pixels of the display whose centres are inside the triangle. */
static long tri_pixels(int x0, int y0, int x1, int y1, int x2, int y2)
{
    int xs[3], ys[3], minx, maxx, miny, maxy, px, py, i;
    long n = 0;

    xs[0] = x0, xs[1] = x1, xs[2] = x2;
    ys[0] = y0, ys[1] = y1, ys[2] = y2;
    if (too_big(xs, ys, 3))
        return 0;
    if (edge(x0, y0, x1, y1, x2, y2) < 0) { /* make it counter-clockwise */
        xs[1] = x2, ys[1] = y2;
        xs[2] = x1, ys[2] = y1;
    }
    minx = maxx = xs[0];
    miny = maxy = ys[0];
    for (i = 1; i < 3; ++i) {
        minx = xs[i] < minx ? xs[i] : minx;
        maxx = xs[i] > maxx ? xs[i] : maxx;
        miny = ys[i] < miny ? ys[i] : miny;
        maxy = ys[i] > maxy ? ys[i] : maxy;
    }
    minx = minx < 0 ? 0 : minx;
    miny = miny < 0 ? 0 : miny;
    maxx = maxx > WL_WIDTH ? WL_WIDTH : maxx;
    maxy = maxy > WL_HEIGHT ? WL_HEIGHT : maxy;
    for (py = miny; py < maxy; ++py)
        for (px = minx; px < maxx; ++px) {
            long long cx = 2 * px + 1, cy = 2 * py + 1;
            int inside = 1;

            for (i = 0; i < 3 && inside; ++i) {
                int j = (i + 1) % 3;
                long long e = edge(2 * xs[i], 2 * ys[i], 2 * xs[j], 2 * ys[j], cx, cy);
                inside = owns(e, xs[j] - xs[i], ys[j] - ys[i]);
            }
            n += inside;
        }
    return n;
}

static void count_tri(wl_stats *st, const jb_poly *p, int a, int b, int c)
{
    long px = tri_pixels(p->v[a].x, p->v[a].y, p->v[b].x, p->v[b].y, p->v[c].x, p->v[c].y);

    ++st->triangles;
    st->tri_pixels += px;
    st->tri_visible += px > 0;
    if (p->texture != NULL) {
        ++st->textured;
        st->tex4 += p->texture->depth == JB_TEX_4BIT;
        st->tex8 += p->texture->depth == JB_TEX_8BIT;
        st->tex15 += p->texture->depth == JB_TEX_15BIT;
    }
}

void wl_count_poly(wl_stats *st, const jb_poly *p)
{
    ++st->prims;
    st->blended += p->blend != JB_BLEND_NONE;
    count_tri(st, p, 0, 1, 2);
    if (p->vertices == 4)
        count_tri(st, p, 1, 2, 3);
}

void wl_count_rect(wl_stats *st, const jb_rect *r)
{
    int x0 = r->x < 0 ? 0 : r->x, y0 = r->y < 0 ? 0 : r->y;
    int x1 = r->x + r->w > WL_WIDTH ? WL_WIDTH : r->x + r->w;
    int y1 = r->y + r->h > WL_HEIGHT ? WL_HEIGHT : r->y + r->h;

    ++st->prims;
    ++st->rects;
    st->blended += r->blend != JB_BLEND_NONE;
    if (x1 > x0 && y1 > y0)
        st->rect_pixels += (long)(x1 - x0) * (y1 - y0);
}

void wl_count_line(wl_stats *st, int x0, int y0, int x1, int y1, int blend)
{
    int xs[2], ys[2], dx = x1 - x0, dy = y1 - y0, n, k;

    ++st->prims;
    ++st->lines;
    st->blended += blend != JB_BLEND_NONE;
    xs[0] = x0, xs[1] = x1, ys[0] = y0, ys[1] = y1;
    if (too_big(xs, ys, 2))
        return;
    n = iabs(dx) > iabs(dy) ? iabs(dx) : iabs(dy);
    for (k = 0; k <= n; ++k) {
        int x = n ? x0 + (dx * k * 2 + (dx < 0 ? -n : n)) / (2 * n) : x0;
        int y = n ? y0 + (dy * k * 2 + (dy < 0 ? -n : n)) / (2 * n) : y0;
        st->line_pixels += x >= 0 && x < WL_WIDTH && y >= 0 && y < WL_HEIGHT;
    }
}

void wl_stats_begin_frame(wl_stats *st)
{
    st->mark = st->prims;
}

void wl_stats_end_frame(wl_stats *st)
{
    long n = st->prims - st->mark;

    if (st->frames == 0 || n < st->min_prims)
        st->min_prims = n;
    if (st->frames == 0 || n > st->max_prims)
        st->max_prims = n;
    ++st->frames;
}

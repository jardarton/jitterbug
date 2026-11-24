/* The VRAM placer (placer.h): a bitmap of taken pixels and a first-fit
   search under the texture page rules. */
#include "core/placer.h"

#include <string.h>

void jbi_placer_init(jbi_placer *p)
{
    memset(p->used, 0, sizeof p->used);
}

/* The bits of [lo, hi) that fall in word k (hi > lo). */
static uint64_t word_mask(int k, int lo, int hi)
{
    int a = lo - k * 64, b = hi - k * 64;
    uint64_t m = ~(uint64_t)0;

    if (a > 0)
        m &= ~(uint64_t)0 << a;
    if (b < 64)
        m &= ((uint64_t)1 << b) - 1;
    return m;
}

/* r clipped to VRAM: 1, or 0 for nothing left. The ends are computed in
   64 bits, so any int position and size will do. */
static int clip(jbi_rect *r)
{
    int64_t x0 = r->x < 0 ? 0 : r->x, y0 = r->y < 0 ? 0 : r->y;
    int64_t x1 = (int64_t)r->x + r->w, y1 = (int64_t)r->y + r->h;

    if (x1 > JBI_PLACER_W)
        x1 = JBI_PLACER_W;
    if (y1 > JBI_PLACER_H)
        y1 = JBI_PLACER_H;
    if (x1 <= x0 || y1 <= y0)
        return 0;
    r->x = (int)x0, r->y = (int)y0, r->w = (int)(x1 - x0), r->h = (int)(y1 - y0);
    return 1;
}

void jbi_placer_mark(jbi_placer *p, jbi_rect r, int taken)
{
    int y, k;

    if (!clip(&r))
        return;
    for (y = r.y; y < r.y + r.h; ++y)
        for (k = r.x / 64; k <= (r.x + r.w - 1) / 64; ++k) {
            uint64_t m = word_mask(k, r.x, r.x + r.w);
            if (taken)
                p->used[y][k] |= m;
            else
                p->used[y][k] &= ~m;
        }
}

int jbi_placer_is_free(const jbi_placer *p, jbi_rect r)
{
    int y, k;

    if (!clip(&r))
        return 1;
    for (y = r.y; y < r.y + r.h; ++y)
        for (k = r.x / 64; k <= (r.x + r.w - 1) / 64; ++k)
            if (p->used[y][k] & word_mask(k, r.x, r.x + r.w))
                return 0;
    return 1;
}

/* The highest set bit of row in [lo, hi), or -1. */
static int last_taken(const uint64_t *row, int lo, int hi)
{
    int k, b;

    for (k = (hi - 1) / 64; k >= lo / 64; --k) {
        uint64_t m = row[k] & word_mask(k, lo, hi);
        if (m == 0)
            continue;
        for (b = 63; !(m >> b & 1); --b)
            ;
        return k * 64 + b;
    }
    return -1;
}

/* The first x from x0 on in one band (the OR of the rows the rectangle
   covers), or -1. */
static int find_x(const uint64_t *band, const jbi_place_req *q, int x0)
{
    int x = (x0 + q->align - 1) & ~(q->align - 1);

    while (x + q->w <= JBI_PLACER_W) {
        int taken;
        if (q->span != 0) {
            int page = x & ~63;
            if (x + q->w > page + q->span) {
                x = page + 64;
                continue;
            }
        }
        taken = last_taken(band, x, x + q->w);
        if (taken < 0)
            return x;
        x = (taken + 1 + q->align - 1) & ~(q->align - 1);
    }
    return -1;
}

static int valid(const jbi_place_req *q)
{
    if (q->w < 1 || q->w > JBI_PLACER_W || q->h < 1 || q->h > 256)
        return 0;
    if (q->align < 1 || q->align > 64 || (q->align & (q->align - 1)) != 0)
        return 0;
    if (q->align_y < 1 || q->align_y > 256 || (q->align_y & (q->align_y - 1)) != 0)
        return 0;
    if (q->span != 0 && q->span != 64 && q->span != 128 && q->span != 256)
        return 0;
    return q->span == 0 || q->w <= q->span;
}

static int try_row(const jbi_placer *p, const jbi_place_req *q, int y, int x0, jbi_rect *out)
{
    uint64_t band[JBI_PLACER_WORDS];
    int k, r, x;

    memcpy(band, p->used[y], sizeof band);
    for (r = 1; r < q->h; ++r)
        for (k = 0; k < JBI_PLACER_WORDS; ++k)
            band[k] |= p->used[y + r][k];
    x = find_x(band, q, x0);
    if (x < 0)
        return 0;
    out->x = x, out->y = y, out->w = q->w, out->h = q->h;
    return 1;
}

/* The rules of a position other than what is taken (placer.h). */
static int fits_rules(const jbi_place_req *q, int x, int y)
{
    if (x + q->w > JBI_PLACER_W || y + q->h > JBI_PLACER_H || y / 256 != (y + q->h - 1) / 256)
        return 0;
    return q->span == 0 || x + q->w <= (x & ~63) + q->span;
}

int jbi_placer_find_next(const jbi_placer *p, const jbi_place_req *req, const jbi_rect *after,
                         jbi_rect *out)
{
    int x, y, top, x0, y0;

    if (!valid(req))
        return 0;
    if (!req->from_bottom) {
        /* Rows ascending, each from x 0; after a position, its row from the
           next x, then the rows below it. */
        for (top = 0; top < JBI_PLACER_H; top += 256)
            for (y = top; y + req->h <= top + 256; y += req->align_y) {
                if (after != NULL && y < after->y)
                    continue;
                x0 = after != NULL && y == after->y ? after->x + 1 : 0;
                if (try_row(p, req, y, x0, out))
                    return 1;
            }
        return 0;
    }
    /* Columns ascending, each from the bottom up; after a position, its
       column above it, then the columns to its right. */
    x0 = 0;
    if (after != NULL)
        x0 = after->x / req->align * req->align;
    for (x = x0; x + req->w <= JBI_PLACER_W; x += req->align) {
        y0 = (JBI_PLACER_H - req->h) / req->align_y * req->align_y;
        if (after != NULL && x == after->x)
            y0 = (after->y - 1) / req->align_y * req->align_y;
        for (y = y0; y >= 0; y -= req->align_y) {
            jbi_rect r;
            if (after != NULL && x == after->x && y >= after->y)
                continue;
            r.x = x, r.y = y, r.w = req->w, r.h = req->h;
            if (fits_rules(req, x, y) && jbi_placer_is_free(p, r)) {
                *out = r;
                return 1;
            }
        }
    }
    return 0;
}

void jbi_placer_bands(const jbi_placer *p, int h, uint64_t (*bands)[JBI_PLACER_WORDS])
{
    int top, y, r, k;

    if (h < 1 || h > 256)
        return;
    for (top = 0; top < JBI_PLACER_H; top += 256)
        for (y = top; y + h <= top + 256; ++y) {
            memcpy(bands[y], p->used[y], sizeof bands[y]);
            for (r = 1; r < h; ++r)
                for (k = 0; k < JBI_PLACER_WORDS; ++k)
                    bands[y][k] |= p->used[y + r][k];
        }
}

int jbi_placer_find_banded(const uint64_t (*bands)[JBI_PLACER_WORDS], const jbi_place_req *req,
                           jbi_rect extra, jbi_rect *out)
{
    uint64_t band[JBI_PLACER_WORDS];
    int top, y, x, k;

    if (!valid(req) || req->from_bottom)
        return 0;
    if (!clip(&extra)) /* as jbi_placer_mark takes it: nothing left (an
                          empty extra included) adds nothing */
        extra.w = 0;
    for (top = 0; top < JBI_PLACER_H; top += 256)
        for (y = top; y + req->h <= top + 256; y += req->align_y) {
            memcpy(band, bands[y], sizeof band);
            if (extra.w > 0 && extra.y < y + req->h && y < extra.y + extra.h)
                for (k = extra.x / 64; k <= (extra.x + extra.w - 1) / 64; ++k)
                    band[k] |= word_mask(k, extra.x, extra.x + extra.w);
            if ((x = find_x(band, req, 0)) >= 0) {
                out->x = x, out->y = y, out->w = req->w, out->h = req->h;
                return 1;
            }
        }
    return 0;
}

int jbi_placer_find(const jbi_placer *p, const jbi_place_req *req, jbi_rect *out)
{
    return jbi_placer_find_next(p, req, NULL, out);
}

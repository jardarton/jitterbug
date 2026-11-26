/*
 * The software backend (soft.h): internal resolution and parallel
 * rasterisation, the scaled VRAM, the primitive queue and the worker bands.
 *
 * Queue. With one thread and no scaled VRAM, submit draws at once.
 * Otherwise primitives collect in a queue until a sync point: an explicit
 * flush, a transfer, a full queue, a primitive that reads texels or CLUT
 * entries that a queued primitive may have written, or one that may write
 * texels or CLUT entries a queued primitive reads (both tracked in
 * 32x32-pixel tiles of the 1x VRAM). A flush runs the workers
 * (jbi_pool_run, workers.h): worker i of K rasterises, for every queued
 * primitive in order, the rows of bands i, i + K, ... in both passes.
 * Bands are 16 rows of the 1x VRAM and 16 rows of the scaled one. Each
 * pixel is written by one worker in list order, so the result does not
 * depend on the number of threads. A primitive that may read texels it
 * writes itself (its texture tiles overlap the tiles it draws into) would
 * see other workers' rows half done, so it is rasterised alone on the
 * calling thread, every row in order, as one thread would draw it.
 *
 * Stale 1x tiles. Above 1x nothing shows the 1x VRAM, so by default
 * (unless exact_1x, jbi_config) primitives other than fills are drawn into
 * the scaled VRAM only, and the 1x tiles they cover are marked stale. The
 * 1x VRAM is read above 1x only for 4- and 8-bit texels and CLUT entries,
 * by VRAM-to-VRAM copies, for 24-bit display, and by tools
 * (jbi_soft_vram). Before such a read, stale tiles are filled from the
 * scaled VRAM (jbi_soft_hires_average), which gives uploaded, filled and
 * copied pixels exactly and drawn ones smoothed, as a store above 1x always
 * has; a copy instead carries the mark to its destination.
 *
 * The CLUT cache. As psx-spx describes it ("GPU Texture Caching"), the GPU
 * keeps the palette it last loaded and loads one from VRAM only when a 4- or
 * 8-bit primitive names a different CLUT, or after GP0(01h); psx-spx's
 * GP1(01h) resets it too, and so GP1(00h). ps1-tests' clut-cache dump
 * shows that an 8-bit primitive after a 4-bit one with the same CLUT loads
 * it again, a 4-bit one after an 8-bit one does not, changing the draw
 * mode with GP0(E1h) changes nothing until something is drawn, and a
 * palette is loaded before the primitive draws its first pixel (a sprite
 * drawn over its own CLUT keeps its colours). How much a load holds is
 * jitterbug's model, not shown: a 4-bit primitive loads the CLUT's 16
 * entries and an 8-bit one its 256, and a primitive that needs more than
 * the last load held loads again (docs/conformance.md, "Assumptions
 * beyond the evidence"). Here every 4- and 8-bit primitive decoded counts,
 * also one that draws nothing; untextured and 15-bit primitives leave the
 * cache as it is (no dump says otherwise).
 *
 * Queued primitives are drawn later, so the palette in force is fixed when
 * one is submitted: on a load, the entries are copied out of the 1x VRAM
 * (after queued primitives that write to them are drawn, and stale 1x tiles
 * filled in) into the backend's palette pool, and every primitive drawn
 * with that palette points at the copy (jbi_tex_source.clut), at any scale
 * and in both passes. The pool is reused after each flush, keeping the
 * cached palette; a load that does not fit flushes first. A primitive with
 * a cached palette reads no CLUT from VRAM, so its CLUT counts in no tile
 * hazard. With the cache off (set_clut_cache) primitives read their CLUTs
 * from VRAM as they draw, as the seed did.
 *
 * Everything here belongs to one backend instance; the worker pool is the
 * instance's own, created when a configuration first asks for more than
 * one thread and joined when the backend is destroyed.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soft/soft_state.h"

enum {
    QUEUE_MAX = 8192,
    BAND_SHIFT = JBI_SOFT_BAND_SHIFT,
    TILE_SHIFT = JBI_SOFT_TILE_SHIFT,
    TILE_COLS = JBI_SOFT_TILE_COLS,
    TILE_ROWS = JBI_SOFT_TILE_ROWS,
    THREADS_MAX = 64,  /* threads a configuration may ask for */
    THREADS_AUTO = 16, /* at most, for threads 0 */
};

/* A message through the configured log callback; nothing without one. */
static void log_msg(const jbi_soft *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void log_msg(const jbi_soft *s, const char *fmt, ...)
{
    char msg[160];
    va_list ap;

    if (s->log == NULL)
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    s->log(s->log_user, msg);
}

/* ------------------------------------------------------------------------ */
/* Tiles */

static uint32_t col_mask(int x0, int x1) /* tile columns x0..x1, inclusive */
{
    uint32_t hi = x1 >= TILE_COLS - 1 ? ~0u : (1u << (x1 + 1)) - 1;
    return hi & ~((1u << x0) - 1);
}

/* Sets the tiles of the 1x rectangle, which may wrap. */
static void mark_rect(uint32_t *tiles, int x, int y, int w, int h)
{
    int x0, x1, y0, y1, t;
    uint32_t m;

    if (w <= 0 || h <= 0)
        return;
    if (w >= JBI_VRAM_W)
        x = 0, w = JBI_VRAM_W;
    if (h >= JBI_VRAM_H)
        y = 0, h = JBI_VRAM_H;
    x &= JBI_VRAM_W - 1;
    y &= JBI_VRAM_H - 1;
    if (x + w > JBI_VRAM_W) {
        mark_rect(tiles, 0, y, x + w - JBI_VRAM_W, h);
        w = JBI_VRAM_W - x;
    }
    if (y + h > JBI_VRAM_H) {
        mark_rect(tiles, x, 0, w, y + h - JBI_VRAM_H);
        h = JBI_VRAM_H - y;
    }
    x0 = x >> TILE_SHIFT;
    x1 = (x + w - 1) >> TILE_SHIFT;
    y0 = y >> TILE_SHIFT;
    y1 = (y + h - 1) >> TILE_SHIFT;
    m = col_mask(x0, x1);
    for (t = y0; t <= y1; ++t)
        tiles[t] |= m;
}

static int any_rect(const uint32_t *tiles, int x, int y, int w, int h)
{
    uint32_t probe[TILE_ROWS] = {0};
    int t;

    mark_rect(probe, x, y, w, h);
    for (t = 0; t < TILE_ROWS; ++t) {
        if (probe[t] & tiles[t])
            return 1;
    }
    return 0;
}

/* Tile buffers share VRAM's layout. Only the marked tiles are copied. */
static void copy_tiles(uint16_t *dst, const uint16_t *src, int scale, int y, uint32_t cols)
{
    int size = (1 << TILE_SHIFT) * scale;
    int stride = JBI_VRAM_W * scale;

    for (int x = 0; cols != 0; ++x, cols >>= 1) {
        size_t start = (size_t)y * size * stride + x * size;

        if (!(cols & 1))
            continue;
        for (int row = 0; row < size; ++row) {
            size_t off = start + (size_t)row * stride;

            memcpy(dst + off, src + off, (size_t)size * sizeof *dst);
        }
    }
}

static void save_tile_row(jbi_saved_vram *saved, const uint16_t *src,
                          int scale, int row, uint32_t cols)
{
    copy_tiles(saved->pixels, src, scale, row, cols & ~saved->tiles[row]);
    saved->tiles[row] |= cols;
}

static int soft_checkpoint_begin(jbi_backend *b)
{
    jbi_soft *s = (jbi_soft *)b;
    jbi_soft_checkpoint *cp = &s->checkpoint;
    size_t pixels = (size_t)JBI_VRAM_W * JBI_VRAM_H;

    assert(!cp->active);
    if (cp->lo.pixels == NULL)
        cp->lo.pixels = jbi_calloc(&s->alloc, pixels, sizeof(uint16_t));
    if (cp->lo.pixels == NULL)
        return 0;
    if (cp->scale != s->scale) {
        jbi_free(&s->alloc, cp->hi.pixels);
        cp->hi.pixels = NULL;
        cp->scale = 0;
        if (s->hvram != NULL) {
            cp->hi.pixels = jbi_calloc(&s->alloc, pixels * s->scale * s->scale,
                                       sizeof(uint16_t));
            if (cp->hi.pixels == NULL)
                return 0;
        }
        cp->scale = s->scale;
    }
    jbi_soft_flush(s);
    memcpy(cp->stale, s->stale, sizeof cp->stale);
    cp->stale_any = s->stale_any;
    cp->clut_valid = s->clut_valid;
    cp->clut_x = s->clut_x;
    cp->clut_y = s->clut_y;
    cp->clut_n = s->clut_n;
    if (s->clut_valid)
        memcpy(cp->palette, s->palettes, (size_t)s->clut_n * sizeof(uint16_t));
    cp->active = 1;
    return 1;
}

static void soft_checkpoint_rollback(jbi_backend *b)
{
    jbi_soft *s = (jbi_soft *)b;
    jbi_soft_checkpoint *cp = &s->checkpoint;

    assert(cp->active && cp->scale == s->scale);
    jbi_soft_flush(s);
    for (int row = 0; row < TILE_ROWS; ++row) {
        copy_tiles(s->vram, cp->lo.pixels, 1, row, cp->lo.tiles[row]);
        if (s->hvram != NULL)
            copy_tiles(s->hvram, cp->hi.pixels, s->scale, row, cp->hi.tiles[row]);
        cp->lo.tiles[row] = cp->hi.tiles[row] = 0;
    }
    memcpy(s->stale, cp->stale, sizeof s->stale);
    s->stale_any = cp->stale_any;
    s->clut_valid = cp->clut_valid;
    s->clut_x = cp->clut_x;
    s->clut_y = cp->clut_y;
    s->clut_n = cp->clut_n;
    s->clut_at = 0;
    s->pool_used = cp->clut_valid ? cp->clut_n : 0;
    if (cp->clut_valid)
        memcpy(s->palettes, cp->palette, (size_t)cp->clut_n * sizeof(uint16_t));
    cp->active = 0;
}

void jbi_soft_checkpoint_touch(jbi_soft *s, int x, int y, int w, int h)
{
    jbi_soft_checkpoint *cp = &s->checkpoint;
    uint32_t tiles[TILE_ROWS] = {0};

    if (!cp->active)
        return;
    mark_rect(tiles, x, y, w, h);
    for (int row = 0; row < TILE_ROWS; ++row) {
        save_tile_row(&cp->lo, s->vram, 1, row, tiles[row]);
        if (s->hvram != NULL)
            save_tile_row(&cp->hi, s->hvram, s->scale, row, tiles[row]);
    }
}

/* The 1x pixels a primitive may write. */
static void prim_bounds(const jbi_prim *p, int *x, int *y, int *w, int *h)
{
    int x0, x1;

    switch (p->kind) {
    case JBI_PRIM_FILL:
        *x = p->u.fill.x;
        *y = p->u.fill.y;
        *w = p->u.fill.w;
        *h = p->u.fill.h;
        return;
    case JBI_PRIM_RECT:
        x0 = p->u.rect.x;
        x1 = p->u.rect.x + p->u.rect.w - 1;
        break;
    case JBI_PRIM_TRI: {
        const jbi_vertex *v = p->u.tri.v;
        x0 = v[0].x < v[1].x ? v[0].x : v[1].x;
        x0 = v[2].x < x0 ? v[2].x : x0;
        x1 = v[0].x > v[1].x ? v[0].x : v[1].x;
        x1 = v[2].x > x1 ? v[2].x : x1;
        --x0;
        ++x1;
        /* The columns of the sub-pixel positions (prim.h), as the decoder
           does for the rows. */
        for (int i = 0; i < 3; ++i) {
            int q = jbi_q_pixel(p->u.tri.qx[i]);
            x0 = q < x0 ? q : x0;
            x1 = q > x1 ? q : x1;
        }
        break;
    }
    default: {
        const jbi_vertex *v = p->u.line.v;
        x0 = (v[0].x < v[1].x ? v[0].x : v[1].x) - 1;
        x1 = (v[0].x > v[1].x ? v[0].x : v[1].x) + 1;
        for (int i = 0; i < 2; ++i) {
            int q = jbi_q_pixel(p->u.line.qx[i]);
            x0 = q < x0 ? q : x0;
            x1 = q + 1 > x1 ? q + 1 : x1;
        }
        break;
    }
    }
    if (x0 < p->clip_x0)
        x0 = p->clip_x0;
    if (x1 > p->clip_x1)
        x1 = p->clip_x1;
    *x = x0;
    *y = p->row0;
    *w = x1 - x0 + 1;
    *h = p->row1 - p->row0 + 1;
}

/*
 * The 1x tiles a primitive reads: its texels (conservatively, the texture
 * coordinates' bounding box, or the whole page with a texture window) and,
 * with clut, its CLUT entries.
 */
static void read_tiles(const jbi_prim *p, uint32_t *out, int clut)
{
    int u0, u1, v0, v1, cols;

    if (!(p->flags & JBI_F_TEXTURED))
        return;
    if (p->kind == JBI_PRIM_TRI) {
        const jbi_vertex *v = p->u.tri.v;
        u0 = v[0].u < v[1].u ? v[0].u : v[1].u;
        u0 = v[2].u < u0 ? v[2].u : u0;
        u1 = v[0].u > v[1].u ? v[0].u : v[1].u;
        u1 = v[2].u > u1 ? v[2].u : u1;
        v0 = v[0].v < v[1].v ? v[0].v : v[1].v;
        v0 = v[2].v < v0 ? v[2].v : v0;
        v1 = v[0].v > v[1].v ? v[0].v : v[1].v;
        v1 = v[2].v > v1 ? v[2].v : v1;
    } else {
        int w = p->u.rect.w, h = p->u.rect.h;
        u0 = p->flip_x ? p->u.rect.u - w + 1 : p->u.rect.u;
        u1 = u0 + w - 1;
        v0 = p->flip_y ? p->u.rect.v - h + 1 : p->u.rect.v;
        v1 = v0 + h - 1;
    }
    if (u0 < 0 || u1 > 255 || p->tex.and_x != 0xff || p->tex.or_x != 0)
        u0 = 0, u1 = 255;
    if (v0 < 0 || v1 > 255 || p->tex.and_y != 0xff || p->tex.or_y != 0)
        v0 = 0, v1 = 255;
    if (p->depth == 4) {
        u0 >>= 2, u1 >>= 2, cols = 16;
    } else if (p->depth == 8) {
        u0 >>= 1, u1 >>= 1, cols = 256;
    } else {
        cols = 0;
    }
    mark_rect(out, p->tex.tpx + u0, p->tex.tpy + v0, u1 - u0 + 1, v1 - v0 + 1);
    if (cols != 0 && clut)
        mark_rect(out, p->tex.clut_x, p->tex.clut_y, cols, 1);
}

static int tiles_meet(const uint32_t *a, const uint32_t *b)
{
    int t;

    for (t = 0; t < TILE_ROWS; ++t) {
        if (a[t] & b[t])
            return 1;
    }
    return 0;
}

static int reads_tiles(const jbi_prim *p, const uint32_t *tiles, int clut)
{
    uint32_t probe[TILE_ROWS] = {0};
    int t;

    read_tiles(p, probe, clut);
    for (t = 0; t < TILE_ROWS; ++t) {
        if (probe[t] & tiles[t])
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Stale 1x tiles */

static int skip_1x(const jbi_soft *s)
{
    return s->hvram != NULL && !s->exact_1x;
}

uint16_t jbi_soft_hires_average(const jbi_soft *s, int x, int y)
{
    int n = s->scale, hw = JBI_VRAM_W * n, nn = n * n, i, j;
    const uint16_t *p = s->hvram + (size_t)y * n * hw + (size_t)x * n;
    int r = 0, g = 0, b = 0;

    for (j = 0; j < n; ++j)
        for (i = 0; i < n; ++i) {
            uint16_t c = p[(size_t)j * hw + i];
            r += c & 31;
            g += (c >> 5) & 31;
            b += (c >> 10) & 31;
        }
    r = (r + nn / 2) / nn;
    g = (g + nn / 2) / nn;
    b = (b + nn / 2) / nn;
    return (uint16_t)(r | g << 5 | b << 10 | (p[0] & 0x8000));
}

/* Fills the stale tiles among `tiles` from the scaled VRAM. */
static void resolve_tiles(jbi_soft *s, const uint32_t *tiles)
{
    int t, c, x, y, any = 0;

    for (t = 0; t < TILE_ROWS; ++t)
        any |= (tiles[t] & s->stale[t]) != 0;
    if (!any)
        return;
    jbi_soft_flush(s);
    for (t = 0; t < TILE_ROWS; ++t) {
        uint32_t todo = tiles[t] & s->stale[t];

        if (s->checkpoint.active)
            save_tile_row(&s->checkpoint.lo, s->vram, 1, t, todo);
        for (c = 0; todo != 0 && c < TILE_COLS; ++c) {
            if (!(todo & (1u << c)))
                continue;
            for (y = t << TILE_SHIFT; y < (t + 1) << TILE_SHIFT; ++y)
                for (x = c << TILE_SHIFT; x < (c + 1) << TILE_SHIFT; ++x)
                    s->vram[(size_t)y * JBI_VRAM_W + x] = jbi_soft_hires_average(s, x, y);
        }
        s->stale[t] &= ~todo;
    }
    s->stale_any = 0;
    for (t = 0; t < TILE_ROWS; ++t)
        s->stale_any |= s->stale[t] != 0;
}

void jbi_soft_lowres_resolve(jbi_soft *s, int x, int y, int w, int h)
{
    uint32_t tiles[TILE_ROWS] = {0};

    if (!s->stale_any || s->hvram == NULL)
        return;
    mark_rect(tiles, x, y, w, h);
    resolve_tiles(s, tiles);
}

static void resolve_all(jbi_soft *s)
{
    if (s->stale_any && s->hvram != NULL)
        jbi_soft_lowres_resolve(s, 0, 0, JBI_VRAM_W, JBI_VRAM_H);
    for (int t = 0; t < TILE_ROWS; ++t)
        s->stale[t] = 0;
    s->stale_any = 0;
}

int jbi_soft_lowres_stale(const jbi_soft *s, int x, int y, int w, int h)
{
    return s->stale_any && any_rect(s->stale, x, y, w, h);
}

void jbi_soft_lowres_mark(jbi_soft *s, int x, int y, int w, int h)
{
    if (!skip_1x(s))
        return;
    mark_rect(s->stale, x, y, w, h);
    s->stale_any = 1;
}

static void soft_set_exact_1x(jbi_backend *b, int exact)
{
    jbi_soft *s = (jbi_soft *)b;

    jbi_soft_flush(s);
    resolve_all(s);
    s->exact_1x = exact != 0;
}

uint16_t *jbi_soft_vram(jbi_soft *s)
{
    jbi_soft_flush(s);
    if (s->stale_any)
        jbi_soft_lowres_resolve(s, 0, 0, JBI_VRAM_W, JBI_VRAM_H);
    return s->vram;
}

/* ------------------------------------------------------------------------ */
/* Scaled VRAM helpers */

void jbi_soft_hires_put(jbi_soft *s, int x, int y, uint16_t pix, int check_mask,
                        uint16_t set_mask)
{
    int n = s->scale, hw = JBI_VRAM_W * n, i, j;
    uint16_t *d = s->hvram + (size_t)y * n * hw + (size_t)x * n;

    for (j = 0; j < n; ++j, d += hw) {
        for (i = 0; i < n; ++i) {
            if (check_mask && (d[i] & 0x8000))
                continue;
            d[i] = (uint16_t)(pix | set_mask);
        }
    }
}

void jbi_soft_replicate(jbi_soft *s)
{
    int x, y;

    assert(!s->checkpoint.active);
    if (s->hvram == NULL)
        return;
    for (y = 0; y < JBI_VRAM_H; ++y)
        for (x = 0; x < JBI_VRAM_W; ++x)
            jbi_soft_hires_put(s, x, y, s->vram[(size_t)y * JBI_VRAM_W + x], 0, 0);
}

/* ------------------------------------------------------------------------ */
/* Running primitives */

typedef struct {
    const jbi_soft *s;
    int count;
    int skip_1x; /* only fills go to the 1x VRAM */
    int bands;   /* workers sharing the rows: s->threads, or 1 */
} FlushJob;

/* Does any of the rows [y0, y1] belong to rs? */
static int rows_touch(const jbi_rows *rs, int y0, int y1)
{
    if (y0 < 0)
        y0 = 0;
    return y0 <= y1 && jbi_rows_next(rs, y0) <= y1;
}

static void run_worker(void *ctx, int index)
{
    const FlushJob *job = ctx;
    const jbi_soft *s = job->s;
    jbi_rows lo = {BAND_SHIFT, job->bands, index};
    jbi_rows hi = {BAND_SHIFT, job->bands, index};
    int n = s->scale, i;

    if (index >= job->bands) /* a bigger pool from an earlier configuration */
        return;
    for (i = 0; i < job->count; ++i) {
        const jbi_prim *p = &s->queue[i];

        if ((!job->skip_1x || p->kind == JBI_PRIM_FILL) && rows_touch(&lo, p->row0, p->row1))
            jbi_raster_prim(p, &lo, &s->target);
        if (s->hvram != NULL &&
            rows_touch(&hi, p->row0 * n, p->row1 * n + n - 1))
            jbi_raster_prim_hires(p, &hi, &s->hires);
    }
}

/* Runs the queue on `bands` workers (s->threads, or 1 for the caller
   alone). */
static void flush_on(jbi_soft *s, int bands)
{
    FlushJob job;
    int t;

    if (s->queued > 0) {
        if (s->checkpoint.active) {
            for (t = 0; t < TILE_ROWS; ++t) {
                save_tile_row(&s->checkpoint.lo, s->vram, 1, t, s->dirty_1x[t]);
                if (s->hvram != NULL)
                    save_tile_row(&s->checkpoint.hi, s->hvram, s->scale, t, s->dirty[t]);
            }
        }
        job.s = s;
        job.count = s->queued;
        job.skip_1x = skip_1x(s);
        job.bands = bands;
        if (bands > 1)
            jbi_pool_run(s->pool, run_worker, &job);
        else
            run_worker(&job, 0);
        s->queued = 0;
        for (t = 0; t < TILE_ROWS; ++t)
            s->dirty[t] = s->dirty_1x[t] = s->reads[t] = 0;
    }
    /* Nothing queued refers to the palette pool now: only the cached
       palette stays, moved to its start. */
    if (s->clut_valid && s->clut_at != 0) {
        memmove(s->palettes, s->palettes + s->clut_at, (size_t)s->clut_n * sizeof *s->palettes);
        s->clut_at = 0;
    }
    s->pool_used = s->clut_valid ? s->clut_n : 0;
}

void jbi_soft_flush(jbi_soft *s)
{
    flush_on(s, s->threads);
}

/* The CLUT cache: whether primitive p (4- or 8-bit) loads its palette. */
static int clut_misses(const jbi_soft *s, const jbi_prim *p)
{
    return !s->clut_valid || p->tex.clut_x != s->clut_x || p->tex.clut_y != s->clut_y ||
           (p->depth == 8 ? 256 : 16) > s->clut_n;
}

/* The palette p draws with, loaded from the 1x VRAM first when load is
   set: VRAM there must be as the commands before p left it. */
static const uint16_t *clut_use(jbi_soft *s, const jbi_prim *p, int load)
{
    if (load) {
        const uint16_t *row = s->vram + (size_t)p->tex.clut_y * JBI_VRAM_W;
        int n = p->depth == 8 ? 256 : 16, i;
        uint16_t *dst;

        if (s->queue == NULL)
            s->pool_used = 0; /* drawn at once: nothing else refers to the pool */
        else if (s->pool_used + n > JBI_SOFT_PALETTE_POOL)
            jbi_soft_flush(s);
        s->clut_valid = 0; /* the flush keeps no palette that is replaced */
        dst = s->palettes + s->pool_used;
        for (i = 0; i < n; ++i)
            dst[i] = row[(p->tex.clut_x + i) & (JBI_VRAM_W - 1)];
        s->clut_at = s->pool_used;
        s->pool_used += n;
        s->clut_valid = 1;
        s->clut_x = p->tex.clut_x;
        s->clut_y = p->tex.clut_y;
        s->clut_n = n;
    }
    return s->palettes + s->clut_at;
}

void jbi_soft_submit(jbi_soft *s, const jbi_prim *p)
{
    uint32_t own[TILE_ROWS] = {0}; /* the tiles p may write */
    int x, y, w, h, alone = 0;
    /* A 4- or 8-bit primitive reads its CLUT from VRAM as it draws (live,
       the cache off), or now (load, a cache miss), or not at all. */
    int paletted = (p->flags & JBI_F_TEXTURED) && (p->depth == 4 || p->depth == 8);
    int live = paletted && !s->clut_cache;
    int load = paletted && s->clut_cache && clut_misses(s, p);
    const uint16_t *palette = NULL;
    jbi_prim *q;

    if (s->queue == NULL) {
        jbi_rows all = {BAND_SHIFT, 1, 0};
        jbi_prim one;

        if (s->checkpoint.active && p->row0 <= p->row1) {
            prim_bounds(p, &x, &y, &w, &h);
            jbi_soft_checkpoint_touch(s, x, y, w, h);
        }
        if (!paletted || live) {
            if (p->row0 <= p->row1)
                jbi_raster_prim(p, &all, &s->target);
            return;
        }
        palette = clut_use(s, p, load);
        if (p->row0 > p->row1)
            return;
        one = *p;
        one.tex.clut = palette;
        jbi_raster_prim(&one, &all, &s->target);
        return;
    }
    if (p->row0 > p->row1) {
        /* Nothing to draw; the cache still sees the primitive. */
        if (load) {
            uint32_t tiles[TILE_ROWS] = {0};

            mark_rect(tiles, p->tex.clut_x, p->tex.clut_y, p->depth == 8 ? 256 : 16, 1);
            if (s->queued > 0 && tiles_meet(tiles, s->dirty))
                jbi_soft_flush(s);
            if (skip_1x(s) && s->stale_any)
                resolve_tiles(s, tiles);
            clut_use(s, p, 1);
        }
        return;
    }
    prim_bounds(p, &x, &y, &w, &h);
    mark_rect(own, x, y, w, h);
    /* Workers draw bands of the queue at once, so a primitive that reads
       what a queued one writes, or writes what a queued one reads, waits
       for the queue to be drawn. A cache load reads the CLUT now. */
    if (s->queued > 0 && (reads_tiles(p, s->dirty, live || load) || tiles_meet(own, s->reads)))
        jbi_soft_flush(s);
    if (s->threads > 1 && (p->flags & JBI_F_TEXTURED) && reads_tiles(p, own, live)) {
        /* Reading its own pixels: drawn alone, in row order (above). */
        alone = 1;
        jbi_soft_flush(s);
    }
    if (skip_1x(s)) {
        /* The scaled pass reads 4- and 8-bit texels and CLUTs at 1x, and a
           cache load reads the CLUT. */
        if (s->stale_any && paletted) {
            uint32_t tiles[TILE_ROWS] = {0};
            read_tiles(p, tiles, live || load);
            resolve_tiles(s, tiles);
        }
    }
    if (paletted && !live)
        palette = clut_use(s, p, load);
    if (skip_1x(s)) {
        if (p->kind != JBI_PRIM_FILL)
            jbi_soft_lowres_mark(s, x, y, w, h);
        else
            mark_rect(s->dirty_1x, x, y, w, h);
    } else {
        mark_rect(s->dirty_1x, x, y, w, h);
    }
    mark_rect(s->dirty, x, y, w, h);
    read_tiles(p, s->reads, live);
    q = &s->queue[s->queued++];
    *q = *p;
    q->tex.clut = palette;
    if (alone)
        flush_on(s, 1);
    else if (s->queued == QUEUE_MAX)
        jbi_soft_flush(s);
}

/* ------------------------------------------------------------------------ */
/* The backend operations */

static void soft_submit(jbi_backend *b, const jbi_prim *p)
{
    jbi_soft_submit((jbi_soft *)b, p);
}

static void soft_flush(jbi_backend *b)
{
    jbi_soft_flush((jbi_soft *)b);
}

static void soft_set_clut_cache(jbi_backend *b, int on)
{
    jbi_soft *s = (jbi_soft *)b;

    jbi_soft_flush(s);
    s->clut_cache = on != 0;
    s->clut_valid = 0;
}

/* In command order: queued primitives keep the palettes they have. */
static void soft_clear_cache(jbi_backend *b)
{
    ((jbi_soft *)b)->clut_valid = 0;
}

/* Drops the scaled VRAM and the queue: drawing at 1x on one thread. */
static void drop_scaled(jbi_soft *s)
{
    jbi_free(&s->alloc, s->hvram);
    jbi_free(&s->alloc, s->dither_col);
    jbi_free(&s->alloc, s->queue);
    s->hvram = NULL;
    s->dither_col = NULL;
    s->queue = NULL;
}

/* The threads a request for `threads` means (jbi_config), at least 1. */
static int wanted_threads(int threads)
{
    if (threads <= 0) {
        /* auto: the CPUs there are, up to 16. Even at 1x, rasterising in
           parallel halves the frame time. */
        threads = jbi_cpu_count();
        if (threads > THREADS_AUTO)
            threads = THREADS_AUTO;
    }
    if (threads > THREADS_MAX)
        threads = THREADS_MAX;
    return threads < 1 ? 1 : threads;
}

/* Those threads started: the number there now are for the request. */
static int start_threads(jbi_soft *s, int threads)
{
    threads = wanted_threads(threads);
    if (threads > 1) {
        int want = threads;

        threads = 1;
        if (jbi_pool_can_grow()) { /* else nothing is allocated for them */
            if (s->pool == NULL)
                s->pool = jbi_pool_create(&s->alloc);
            if (s->pool != NULL)
                threads = jbi_pool_grow(s->pool, want);
        }
        if (threads < want)
            log_msg(s, "jitterbug: started only %d of %d rasterising threads", threads, want);
    }
    return threads < 1 ? 1 : threads;
}

static int soft_set_threads(jbi_backend *b, int threads)
{
    jbi_soft *s = (jbi_soft *)b;

    jbi_soft_flush(s);
    threads = wanted_threads(threads); /* once: "automatic" asks the host */
    if (s->queue == NULL && threads > 1 && jbi_pool_can_grow()) {
        /* 1x on one thread until now, drawing each primitive at once. The
           queue comes before the threads, so that without it none start;
           a build that can start none needs no queue either. */
        s->queue = jbi_calloc(&s->alloc, QUEUE_MAX, sizeof(jbi_prim));
        if (s->queue == NULL) {
            log_msg(s, "jitterbug: no memory for the primitive queue; still drawing on %d "
                       "thread(s)", s->threads);
            return 0;
        }
    }
    threads = start_threads(s, threads);
    if (s->queue != NULL && threads == 1 && s->hvram == NULL) {
        jbi_free(&s->alloc, s->queue);
        s->queue = NULL;
    }
    s->threads = threads;
    return threads;
}

static int soft_configure(jbi_backend *b, int scale, int dither, int threads)
{
    jbi_soft *s = (jbi_soft *)b;
    int i, fresh = 1; /* fresh: the scaled VRAM is a new one, to be filled */

    jbi_soft_flush(s);
    resolve_all(s);
    if (scale < 1)
        scale = 1;
    if (scale > JBI_MAX_SCALE)
        scale = JBI_MAX_SCALE;
    threads = start_threads(s, threads);

    /* The new scaled VRAM before the old one goes, so that without memory
       for it the old picture is still there, untouched. */
    {
        uint16_t *hvram = NULL;
        uint8_t *dither_col = NULL;

        if (scale > 1) {
            hvram = jbi_calloc(&s->alloc, (size_t)JBI_VRAM_W * scale * JBI_VRAM_H * scale,
                               sizeof(uint16_t));
            dither_col = jbi_calloc(&s->alloc, (size_t)JBI_VRAM_W * scale, 1);
        }
        if (scale > 1 && (hvram == NULL || dither_col == NULL)) {
            int old = s->hvram != NULL ? s->scale : 1;

            log_msg(s, "jitterbug: no memory for %dx internal resolution; drawing at %dx", scale,
                    old);
            jbi_free(&s->alloc, hvram);
            jbi_free(&s->alloc, dither_col);
            scale = old;
            fresh = 0;
        } else {
            jbi_free(&s->alloc, s->hvram);
            jbi_free(&s->alloc, s->dither_col);
            s->hvram = hvram;
            s->dither_col = dither_col;
        }
    }
    if (s->queue == NULL && (threads > 1 || s->hvram != NULL)) {
        s->queue = jbi_calloc(&s->alloc, QUEUE_MAX, sizeof(jbi_prim));
        if (s->queue == NULL) {
            /* Without a queue primitives are drawn at once, which is only
               right at 1x on one thread. */
            log_msg(s, "jitterbug: no memory for the primitive queue; drawing at 1x on one "
                       "thread");
            drop_scaled(s);
            scale = 1;
            threads = 1;
        }
    }
    if (s->queue != NULL && threads == 1 && s->hvram == NULL) {
        jbi_free(&s->alloc, s->queue);
        s->queue = NULL;
    }
    s->scale = scale;
    s->threads = threads;
    s->dither = dither;
    if (s->hvram != NULL && fresh) {
        for (i = 0; i < JBI_VRAM_W * scale; ++i)
            s->dither_col[i] = (uint8_t)((i / scale) & 3);
        jbi_soft_replicate(s);
    }
    s->target.vram = s->vram;
    s->target.dither_off = scale == 1 && dither == 0;
    s->target.persp = s->hvram == NULL;
    s->hires.pix = s->hvram;
    s->hires.scale = scale;
    s->hires.width = JBI_VRAM_W * scale;
    s->hires.dither = dither > 0;
    s->hires.dither_col = s->dither_col;
    s->hires.vram = s->vram;
    return scale;
}

static int soft_threads(const jbi_backend *b)
{
    return ((const jbi_soft *)b)->threads;
}

static void soft_destroy(jbi_backend *b)
{
    jbi_soft *s = (jbi_soft *)b;
    jbi_allocator alloc = s->alloc;

    jbi_soft_flush(s);
    jbi_pool_destroy(s->pool);
    drop_scaled(s);
    jbi_free(&alloc, s->checkpoint.lo.pixels);
    jbi_free(&alloc, s->checkpoint.hi.pixels);
    jbi_free(&alloc, s->palettes);
    jbi_free(&alloc, s->vram);
    jbi_free(&alloc, s);
}

static int span_or(int span, int fallback)
{
    return span >= 1 && span <= JBI_PERSP_SPAN_MAX ? span : fallback;
}

jbi_backend *jbi_soft_create(const jbi_config *cfg, int *scale)
{
    jbi_soft *s = jbi_calloc(&cfg->alloc, 1, sizeof *s);

    if (s == NULL)
        return NULL;
    s->alloc = cfg->alloc;
    s->vram = jbi_calloc(&s->alloc, (size_t)JBI_VRAM_W * JBI_VRAM_H, sizeof(uint16_t));
    s->palettes = jbi_calloc(&s->alloc, JBI_SOFT_PALETTE_POOL, sizeof(uint16_t));
    if (s->vram == NULL || s->palettes == NULL) {
        jbi_free(&cfg->alloc, s->vram);
        jbi_free(&cfg->alloc, s->palettes);
        jbi_free(&cfg->alloc, s);
        return NULL;
    }
    s->clut_cache = 1;
    s->base.destroy = soft_destroy;
    s->base.configure = soft_configure;
    s->base.threads = soft_threads;
    s->base.set_threads = soft_set_threads;
    s->base.set_exact_1x = soft_set_exact_1x;
    s->base.set_clut_cache = soft_set_clut_cache;
    s->base.clear_cache = soft_clear_cache;
    s->base.submit = soft_submit;
    s->base.fill = jbi_soft_fill;
    s->base.load = jbi_soft_load;
    s->base.store = jbi_soft_store;
    s->base.store_1x = jbi_soft_store_1x;
    s->base.copy = jbi_soft_copy;
    s->base.flush = soft_flush;
    s->base.checkpoint_begin = soft_checkpoint_begin;
    s->base.checkpoint_rollback = soft_checkpoint_rollback;
    s->base.scanout = jbi_soft_scanout;
    s->log = cfg->log;
    s->log_user = cfg->log_user;
    s->exact_1x = cfg->exact_1x != 0;
    s->target.persp_span = span_or(cfg->persp_span_1x, JBI_PERSP_SPAN_1X);
    s->hires.persp_span = span_or(cfg->persp_span, JBI_PERSP_SPAN);
    s->scale = 1;
    s->threads = 1;
    s->dither = -1;
    *scale = soft_configure(&s->base, cfg->scale, cfg->dither, cfg->threads);
    return &s->base;
}

/* ------------------------------------------------------------------------ */
/* Tests and tools (soft.h) */

jbi_soft *jbi_soft_from(jbi_backend *b)
{
    return b != NULL && b->destroy == soft_destroy ? (jbi_soft *)b : NULL;
}

uint16_t *jbi_soft_vram_raw(jbi_soft *s)
{
    return s->vram;
}

uint16_t *jbi_soft_scaled_raw(jbi_soft *s)
{
    return s->hvram;
}

uint16_t *jbi_soft_hires_vram(jbi_soft *s, int *width, int *height)
{
    jbi_soft_flush(s);
    *width = JBI_VRAM_W * s->scale;
    *height = JBI_VRAM_H * s->scale;
    return s->hvram != NULL ? s->hvram : s->vram;
}

void jbi_soft_vram_changed(jbi_soft *s)
{
    jbi_soft_flush(s);
    jbi_soft_replicate(s);
}

void jbi_soft_persp_spans(const jbi_soft *s, int *span, int *span_1x)
{
    *span = s->hires.persp_span;
    *span_1x = s->target.persp_span;
}

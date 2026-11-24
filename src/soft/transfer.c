/*
 * The software backend's VRAM transfers (backend.h): fill, CPU to VRAM,
 * VRAM to CPU and VRAM to VRAM, with the hardware's wrapping at the VRAM
 * edges. Each updates both VRAMs above 1x.
 */
#include <stddef.h>
#include <string.h>

#include "soft/soft_state.h"

/*
 * The fill's record. Every byte is set, padding included, so a queued fill
 * holds nothing left over from the caller's stack. clip covers the whole
 * VRAM (fills are not clipped) and rows the wrapped rows it touches.
 */
void jbi_soft_fill_record(jbi_prim *p, int x, int y, int w, int h, uint16_t color)
{
    memset(p, 0, sizeof *p);
    p->kind = JBI_PRIM_FILL;
    p->flags = 0;
    p->depth = 0;
    p->u.fill.color = color;
    p->u.fill.x = x;
    p->u.fill.y = y;
    p->u.fill.w = w;
    p->u.fill.h = h;
    p->clip_x0 = 0;
    p->clip_y0 = 0;
    p->clip_x1 = JBI_VRAM_W - 1;
    p->clip_y1 = JBI_VRAM_H - 1;
    if (y + h > JBI_VRAM_H) {
        p->row0 = 0;
        p->row1 = JBI_VRAM_H - 1;
    } else {
        p->row0 = (int16_t)y;
        p->row1 = (int16_t)(y + h - 1);
    }
}

/* Queued like a primitive (an isbg DR_ENV fills inside a primitive list). */
void jbi_soft_fill(jbi_backend *b, int x, int y, int w, int h, uint16_t color)
{
    jbi_prim prim;

    jbi_soft_fill_record(&prim, x, y, w, h, color);
    jbi_soft_submit((jbi_soft *)b, &prim);
}

/* CPU to VRAM writes honour the mask settings. */
void jbi_soft_load(jbi_backend *b, int x, int y, int w, int h, const uint16_t *src,
                   jbi_mask mask)
{
    jbi_soft *s = (jbi_soft *)b;
    int i, j;

    jbi_soft_flush(s);
    for (j = 0; j < h; ++j) {
        int yy = (y + j) & (JBI_VRAM_H - 1);
        uint16_t *row = s->vram + (size_t)yy * JBI_VRAM_W;
        for (i = 0; i < w; ++i) {
            int xx = (x + i) & (JBI_VRAM_W - 1);
            uint16_t *dst = &row[xx];
            uint16_t pix = *src++;
            if (s->hvram != NULL)
                jbi_soft_hires_put(s, xx, yy, pix, mask.check, mask.set);
            if (mask.check && (*dst & 0x8000))
                continue;
            *dst = (uint16_t)(pix | mask.set);
        }
    }
}

/* Above 1x, reads back the scaled VRAM (jbi_soft_hires_average), as
   emulators that render at a higher resolution do: what a program reads
   back of a frame it drew (say, as a backdrop) keeps some of its detail. */
void jbi_soft_store(jbi_backend *b, int x, int y, int w, int h, uint16_t *dst)
{
    jbi_soft *s = (jbi_soft *)b;
    int i, j;

    jbi_soft_flush(s);
    if (s->hvram != NULL) {
        for (j = 0; j < h; ++j)
            for (i = 0; i < w; ++i)
                *dst++ = jbi_soft_hires_average(s, (x + i) & (JBI_VRAM_W - 1),
                                                (y + j) & (JBI_VRAM_H - 1));
        return;
    }
    for (j = 0; j < h; ++j) {
        const uint16_t *row = s->vram + (size_t)((y + j) & (JBI_VRAM_H - 1)) * JBI_VRAM_W;
        for (i = 0; i < w; ++i)
            *dst++ = row[(x + i) & (JBI_VRAM_W - 1)];
    }
}

/* The 1x VRAM as it is kept, after filling in stale tiles (soft.c), which
   only exist with exact_1x off. */
void jbi_soft_store_1x(jbi_backend *b, int x, int y, int w, int h, uint16_t *dst)
{
    jbi_soft *s = (jbi_soft *)b;
    int i, j;

    jbi_soft_flush(s);
    jbi_soft_lowres_resolve(s, x, y, w, h);
    for (j = 0; j < h; ++j) {
        const uint16_t *row = s->vram + (size_t)((y + j) & (JBI_VRAM_H - 1)) * JBI_VRAM_W;
        for (i = 0; i < w; ++i)
            *dst++ = row[(x + i) & (JBI_VRAM_W - 1)];
    }
}

/*
 * Row by row, each row read completely before it is written, which equals
 * the hardware for every non-pathological overlap. The scaled VRAM copies
 * its N rows per 1x row the same way.
 */
void jbi_soft_copy(jbi_backend *b, int sx, int sy, int dx, int dy, int w, int h, jbi_mask mask)
{
    jbi_soft *s = (jbi_soft *)b;
    uint16_t *line = s->line;
    int i, j;

    jbi_soft_flush(s);
    /* Stale 1x source pixels (soft.c): the destination's are stale too. */
    if (jbi_soft_lowres_stale(s, sx, sy, w, h))
        jbi_soft_lowres_mark(s, dx, dy, w, h);
    for (j = 0; j < h; ++j) {
        int syy = (sy + j) & (JBI_VRAM_H - 1), dyy = (dy + j) & (JBI_VRAM_H - 1);
        const uint16_t *src = s->vram + (size_t)syy * JBI_VRAM_W;
        uint16_t *dst = s->vram + (size_t)dyy * JBI_VRAM_W;

        if (s->hvram != NULL) {
            int n = s->scale, hw = JBI_VRAM_W * n, k, r;

            for (r = 0; r < n; ++r) {
                const uint16_t *hsrc = s->hvram + (size_t)(syy * n + r) * hw;
                uint16_t *hdst = s->hvram + (size_t)(dyy * n + r) * hw;

                for (i = 0; i < w; ++i) {
                    const uint16_t *a = hsrc + ((sx + i) & (JBI_VRAM_W - 1)) * n;
                    for (k = 0; k < n; ++k)
                        line[i * n + k] = a[k];
                }
                for (i = 0; i < w; ++i) {
                    uint16_t *c = hdst + ((dx + i) & (JBI_VRAM_W - 1)) * n;
                    for (k = 0; k < n; ++k) {
                        if (mask.check && (c[k] & 0x8000))
                            continue;
                        c[k] = (uint16_t)(line[i * n + k] | mask.set);
                    }
                }
            }
        }
        for (i = 0; i < w; ++i)
            line[i] = src[(sx + i) & (JBI_VRAM_W - 1)];
        for (i = 0; i < w; ++i) {
            uint16_t *d = &dst[(dx + i) & (JBI_VRAM_W - 1)];
            if (mask.check && (*d & 0x8000))
                continue;
            *d = (uint16_t)(line[i] | mask.set);
        }
    }
}

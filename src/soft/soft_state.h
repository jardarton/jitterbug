/*
 * The software backend's state, shared by its files: soft.c (configuration,
 * the queue, tiles), transfer.c (fill, load, store, copy) and scanout.c.
 */
#ifndef JBI_SOFT_SOFT_STATE_H
#define JBI_SOFT_SOFT_STATE_H

#include <stdint.h>

#include "core/backend.h"
#include "core/config.h"
#include "core/prim.h"
#include "soft/raster.h"
#include "soft/soft.h"
#include "soft/workers.h"

enum {
    JBI_SOFT_BAND_SHIFT = 4,   /* 16-row bands */
    JBI_SOFT_TILE_SHIFT = 5,   /* 32x32 tiles for hazards and staleness */
    JBI_SOFT_TILE_COLS = JBI_VRAM_W >> JBI_SOFT_TILE_SHIFT, /* 32: one bit each */
    JBI_SOFT_TILE_ROWS = JBI_VRAM_H >> JBI_SOFT_TILE_SHIFT, /* 16 */
    /* Entries of the palette pool (soft.c, "The CLUT cache"): 256 8-bit
       or 4096 4-bit palettes, 128 KB. */
    JBI_SOFT_PALETTE_POOL = 65536,
};

typedef struct {
    uint16_t *pixels;
    uint32_t tiles[JBI_SOFT_TILE_ROWS];
} jbi_saved_vram;

typedef struct {
    jbi_saved_vram lo, hi;
    int active, scale;
    uint32_t stale[JBI_SOFT_TILE_ROWS];
    int stale_any;
    int clut_valid, clut_x, clut_y, clut_n;
    uint16_t palette[256];
} jbi_soft_checkpoint;

struct jbi_soft {
    jbi_backend base; /* first: the interface */

    /* Configuration. */
    uint16_t *vram;   /* 1x, 1024 x 512 */
    uint16_t *hvram;  /* scaled, 1024N x 512N, or NULL at 1x */
    int scale;        /* N */
    int threads;
    int dither;       /* -1: default for the scale */
    int exact_1x;
    jbi_target target; /* the 1x pass's */
    jbi_hires hires;   /* the scaled pass's */
    uint8_t *dither_col;
    jbi_log_fn log;
    void *log_user;
    jbi_allocator alloc; /* everything below and the backend itself */

    /* The queue, NULL with one thread at 1x. Empty after a flush. */
    jbi_prim *queue;
    int queued;
    uint32_t dirty[JBI_SOFT_TILE_ROWS];    /* tiles written by queued prims */
    uint32_t dirty_1x[JBI_SOFT_TILE_ROWS]; /* ... of those, in the 1x VRAM */
    uint32_t reads[JBI_SOFT_TILE_ROWS];    /* tiles queued prims read */

    /* Stale 1x tiles. */
    uint32_t stale[JBI_SOFT_TILE_ROWS];
    int stale_any;

    /* The CLUT cache (soft.c). */
    int clut_cache;      /* on */
    int clut_valid;      /* holds a palette */
    int clut_x, clut_y;  /* the CLUT it was loaded from */
    int clut_n;          /* entries loaded: 16 or 256 */
    int clut_at;         /* the palette's first entry in the pool */
    int pool_used;       /* pool entries that queued primitives may refer to */
    uint16_t *palettes;  /* the palette pool: JBI_SOFT_PALETTE_POOL entries */

    /* Rasterising workers beyond the caller, created on demand. */
    jbi_pool *pool;

    /* The copy's row buffer (transfer.c). */
    uint16_t line[JBI_VRAM_W * JBI_MAX_SCALE];

    jbi_soft_checkpoint checkpoint;
};

/* soft.c */
void jbi_soft_submit(jbi_soft *s, const jbi_prim *p);
void jbi_soft_flush(jbi_soft *s);
/* Before a transfer writes a rectangle in both VRAMs. */
void jbi_soft_checkpoint_touch(jbi_soft *s, int x, int y, int w, int h);
/* Whether the rectangle has stale 1x tiles. */
int jbi_soft_lowres_stale(const jbi_soft *s, int x, int y, int w, int h);
/* Writes a 1x pixel block of the scaled VRAM, honouring the mask bits. */
void jbi_soft_hires_put(jbi_soft *s, int x, int y, uint16_t pix, int check_mask,
                        uint16_t set_mask);
/*
 * The scaled VRAM's N x N block for a 1x pixel, averaged per component
 * (rounded), with the block's first mask bit. Uploaded pixels are
 * replicated, so this is the 1x pixel exactly; drawn ones come back
 * smoothed from the high-resolution image.
 */
uint16_t jbi_soft_hires_average(const jbi_soft *s, int x, int y);

/* transfer.c: the backend operations. */
void jbi_soft_fill(jbi_backend *b, int x, int y, int w, int h, uint16_t color);
void jbi_soft_load(jbi_backend *b, int x, int y, int w, int h, const uint16_t *src,
                   jbi_mask mask);
void jbi_soft_store(jbi_backend *b, int x, int y, int w, int h, uint16_t *dst);
void jbi_soft_store_1x(jbi_backend *b, int x, int y, int w, int h, uint16_t *dst);
void jbi_soft_copy(jbi_backend *b, int sx, int sy, int dx, int dy, int w, int h, jbi_mask mask);

/* scanout.c */
int jbi_soft_scanout(jbi_backend *b, int x, int y, int w, int h, int rgb24, int enabled,
                     void *pixels, int *width, int *height, int *stride);

#endif

/*
 * The backend interface: what the core (command decode and drawing state,
 * context.h) asks of whatever owns the pixels. The core never touches
 * VRAM; it decodes commands into primitive records (prim.h) and transfer
 * calls, and a backend carries them out. src/soft is the one
 * implementation; a GPU backend, whose VRAM lives on the card, implements
 * the same operations, and pixels come back only through store and
 * scanout.
 *
 * A backend is a struct whose first member is jbi_backend, with the
 * operations filled in when it is created (they live in the instance rather
 * than in a shared table so the library keeps no relocated data). The
 * context that created it owns it and is its only caller, on one thread at
 * a time; a backend may run threads of its own inside an operation.
 *
 * Coordinates and sizes arrive already reduced as the hardware reduces
 * them: transfers are 1..1024 by 1..512 pixels at x < 1024, y < 512 and
 * wrap at the VRAM edges; a fill is aligned to 16 pixels and not empty.
 */
#ifndef JBI_CORE_BACKEND_H
#define JBI_CORE_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "core/prim.h"

/* GP0(E6h) mask bit handling for transfers that honour it. */
typedef struct jbi_mask {
    int check;    /* do not write pixels with the mask bit set */
    uint16_t set; /* 0x8000: set the mask bit in every pixel written */
} jbi_mask;

/* Frame formats of scanout. */
enum { JBI_SCANOUT_BGR555 = 0, JBI_SCANOUT_RGB24 = 1 };

typedef struct jbi_backend jbi_backend;

struct jbi_backend {
    /* Finishes everything, stops the backend's threads and frees it. */
    void (*destroy)(jbi_backend *b);

    /*
     * Configuration, VRAM contents kept. scale 1..JBI_MAX_SCALE, dither
     * and threads as in jbi_config (config.h). Finishes queued work first.
     * Returns the scale in effect (a backend may fall back to 1).
     */
    int (*configure)(jbi_backend *b, int scale, int dither, int threads);
    /* The rasterising threads in effect. */
    int (*threads)(const jbi_backend *b);
    /*
     * Only the rasterising threads, as in jbi_config; no pixel changes.
     * Finishes queued work first. Returns the threads in effect, or 0 if
     * it could not be done, with the old threads kept.
     */
    int (*set_threads)(jbi_backend *b, int threads);
    /* jbi_config.exact_1x, changed at run time. */
    void (*set_exact_1x)(jbi_backend *b, int exact);
    /*
     * The CLUT cache (src/soft/soft.c): on (the console's behaviour, the
     * default), a 4- or 8-bit primitive draws with the palette the cache
     * holds, which is loaded from VRAM only when a primitive names another
     * CLUT or needs more entries than were loaded, or after clear_cache;
     * off, every texel's CLUT entry is read from VRAM as it is drawn.
     * Finishes queued work first.
     */
    void (*set_clut_cache)(jbi_backend *b, int on);
    /* GP0(01h), GP1(01h), GP1(00h): the next 4- or 8-bit primitive loads
       its palette from VRAM. In command order with the primitives. */
    void (*clear_cache)(jbi_backend *b);

    /* Draw or queue a decoded triangle, rectangle or line. The record is
       copied before the call returns. */
    void (*submit)(jbi_backend *b, const jbi_prim *p);
    /* GP0(02h): not clipped, no offset, no mask; queued like a primitive.
       color is a 15-bit pixel. */
    void (*fill)(jbi_backend *b, int x, int y, int w, int h, uint16_t color);
    /* CPU to VRAM: w x h pixels, row-major, honouring the mask settings.
       Finishes queued primitives first. */
    void (*load)(jbi_backend *b, int x, int y, int w, int h, const uint16_t *src,
                 jbi_mask mask);
    /* VRAM to CPU: w x h pixels into dst, row-major. Finishes queued
       primitives first. */
    void (*store)(jbi_backend *b, int x, int y, int w, int h, uint16_t *dst);
    /* The same from the 1x VRAM, which holds the console's pixels when
       exact_1x is on (with it off, stale 1x pixels are filled in from the
       scaled VRAM first). At 1x it is store. */
    void (*store_1x)(jbi_backend *b, int x, int y, int w, int h, uint16_t *dst);
    /* VRAM to VRAM, honouring the mask settings. Finishes queued
       primitives first. */
    void (*copy)(jbi_backend *b, int sx, int sy, int dx, int dy, int w, int h, jbi_mask mask);
    /* Finish every queued primitive. */
    void (*flush)(jbi_backend *b);

    /*
     * The display area as the console's display fetch reads it, at the
     * internal resolution N. The area is w x h pixels at (x, y) in VRAM and
     * wraps at the VRAM edges; w or h <= 0 means 512 x 256 (and 15-bit). h
     * is at most 512, w at most 1024 (682 in 24-bit mode).
     *   - rgb24 0: 15-bit pixels, the VRAM format (bits 0-4 red, 5-9 green,
     *     10-14 blue, bit 15 the mask bit), two bytes each, low first.
     *   - rgb24 1: 3 bytes per pixel, R, G, B, read straight through VRAM
     *     from the byte address of (x, y), as the 24-bit display mode does.
     *   - enabled 0 (display off, or no display area set): a black frame.
     * Writes the frame, *width = w * N by *height = h * N pixels with rows
     * *stride bytes apart, to pixels, which must hold jbi_scanout_bytes
     * (context.h) bytes, or at least the *stride x *height bytes this area
     * gives (the rows are not padded: *stride is *width x 2, or x 3 in
     * 24-bit mode), and be aligned for uint16_t. Returns JBI_SCANOUT_*.
     * Finishes queued primitives first.
     */
    int (*scanout)(jbi_backend *b, int x, int y, int w, int h, int rgb24, int enabled,
                   void *pixels, int *width, int *height, int *stride);
};

#endif

/*
 * The VRAM placer: where jb_texture_create puts texels and palettes
 * (include/jitterbug.h, Textures). Pure: it knows VRAM's shape and the
 * placement rules, and nothing of contexts or pixels.
 *
 * It keeps one bit for each of VRAM's 1024 x 512 pixels: set where the
 * pixel is taken (a placed rectangle, a display buffer, a reserved
 * region). jbi_placer_find looks for a free rectangle under a request's
 * rules and changes nothing; the caller marks what it takes and clears it
 * again when it is freed. Display buffers and reservations are marked and
 * never cleared, so they may overlap each other; placed rectangles never
 * overlap anything, so clearing one frees exactly its pixels.
 *
 * The search, first fit (deterministic: the same requests over the same
 * bits give the same places):
 *   - from the top: y from 0 upwards, and at each y, x from 0 upwards; the
 *     first free position wins. A rectangle never crosses row 256 (both
 *     page rows, 0..255 and 256..511), so y runs 0..256-h and then
 *     256..512-h. For textures.
 *   - from the bottom: column by column, x from 0 upwards, and at each x,
 *     y from 512-h upwards (skipping positions that would cross row 256);
 *     the first free position wins. For palettes, which so fill the
 *     space beside or below the display buffers (at the left) before they
 *     take the bottom of a page column a 256-row texture would need.
 *   - x is a multiple of align (a power of two, 1 to 64), and y of
 *     align_y (1 to 256; rows a texture's v must be a multiple of, the
 *     page rows starting at multiples of 256).
 *   - span, when not 0, is the width of a texture page at the texture's
 *     depth (64, 128 or 256 pixels): the rectangle must lie within
 *     [p, p + span) where p is x rounded down to a multiple of 64, which
 *     is then its page_x. (p is the largest page start at or left of x, so
 *     no other page start can hold the rectangle if p can't.)
 * From the top, at each y the rows y..y+h-1 are OR-ed into one row of 1024
 * bits, and x steps past the rightmost taken pixel that blocks it (rounded
 * up to align), or to the next page column when the span does not fit: at
 * most about (512 - h) x (16 h + 1024) word operations a search. From the
 * bottom, each position is tested in turn: (1024 / align) x 512 tests of
 * h x (w / 64 + 2) words, a few hundred thousand for a palette.
 */
#ifndef JBI_CORE_PLACER_H
#define JBI_CORE_PLACER_H

#include <stdint.h>

#include "core/layout.h"

enum { JBI_PLACER_W = 1024, JBI_PLACER_H = 512, JBI_PLACER_WORDS = JBI_PLACER_W / 64 };

typedef struct jbi_placer {
    uint64_t used[JBI_PLACER_H][JBI_PLACER_WORDS];
} jbi_placer;

typedef struct jbi_place_req {
    int w, h;        /* VRAM pixels: 1..1024 by 1..256 */
    int align;       /* x a multiple of this: a power of two, 1..64 */
    int align_y;     /* y a multiple of this: a power of two, 1..256 */
    int span;        /* 0, or the page width in VRAM pixels: 64, 128 or 256 */
    int from_bottom; /* search from the bottom (palettes) */
} jbi_place_req;

/* Everything free. */
void jbi_placer_init(jbi_placer *p);

/* Marks r taken (taken 1) or free (0); r is clipped to VRAM. */
void jbi_placer_mark(jbi_placer *p, jbi_rect r, int taken);

/* 1 if no pixel of r (clipped to VRAM) is taken. */
int jbi_placer_is_free(const jbi_placer *p, jbi_rect r);

/* A free rectangle meeting req, into *out (1), or 0 for none. A request
   outside the ranges above finds nothing. Changes nothing. */
int jbi_placer_find(const jbi_placer *p, const jbi_place_req *req, jbi_rect *out);

/* The next free rectangle meeting req after the position after (one this
   search order gives for req), in the same order; after NULL is
   jbi_placer_find. For trying the places in turn. */
int jbi_placer_find_next(const jbi_placer *p, const jbi_place_req *req, const jbi_rect *after,
                         jbi_rect *out);

/*
 * For searching one top-down request many times with one more rectangle
 * taken each time (the pair search: texture.c). jbi_placer_bands fills
 * bands[y], for every y a rectangle h rows high may start at (within a
 * page row), with the OR of rows y..y+h-1; jbi_placer_find_banded then
 * finds what jbi_placer_find would for req (top-down, req->h that h) with
 * extra (w 0: none) taken as well, from those bands alone. A search costs
 * one 16-word band and a scan per row, with no row read again.
 */
void jbi_placer_bands(const jbi_placer *p, int h, uint64_t (*bands)[JBI_PLACER_WORDS]);
int jbi_placer_find_banded(const uint64_t (*bands)[JBI_PLACER_WORDS], const jbi_place_req *req,
                           jbi_rect extra, jbi_rect *out);

#endif

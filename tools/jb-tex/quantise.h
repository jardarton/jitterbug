/*
 * jb-tex's conversion of an RGBA image to a texture's texels and palette
 * (docs/tools.md, "How a PNG becomes a texture"). Integer arithmetic only, so
 * the same image gives the same bytes on every platform.
 */
#ifndef JB_TEX_QUANTISE_H
#define JB_TEX_QUANTISE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Converts w x h RGBA pixels (4 bytes each, rows top to bottom) at depth 4,
 * 8 or 15: at 4 and 8 bits fills idx (w x h palette indices) and palette
 * (16 or 256 entries; unused ones 0x0000); at 15 bits fills texels (w x h
 * 15-bit colours). dither not 0 diffuses the error (Floyd-Steinberg).
 * Returns 0, or -1 for no memory.
 */
int jbq_convert(const uint8_t *rgba, int w, int h, int depth, int dither, uint8_t *idx,
                uint16_t *palette, uint16_t *texels);

/*
 * A further palette for the same indices, from another image of the same
 * size (a frame of palette animation): entry i is the mean colour of that
 * image's pixels where idx is i, with entry i of base's semi-transparency
 * bit; an entry no pixel uses, and a transparent one (0x0000 in base),
 * stays as in base. entries 16 or 256.
 */
void jbq_frame_palette(const uint8_t *rgba, int w, int h, const uint8_t *idx, int entries,
                       const uint16_t *base, uint16_t *out);

#endif

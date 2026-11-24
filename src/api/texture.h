/*
 * Textures placed in VRAM for the program (include/jitterbug.h, Textures):
 * texture.c (jb_texture_create and the calls beside it, the context's
 * table of textures over the placer, src/core/placer.h) and font.c (the
 * library's text font, created through the same table).
 *
 * For the friendly draws (sprites, text, meshes): jbi_texture_find says
 * whether a jb_texture is one of the context's live textures,
 * jbi_texture_window gives the GP0(E2h) window that makes a repeat texture
 * wrap, and jbi_font / jbi_font_glyph give the font.
 */
#ifndef JBI_API_TEXTURE_H
#define JBI_API_TEXTURE_H

#include <jitterbug.h>

#include "core/layout.h"
#include "core/placer.h"

/* Textures a context holds at once (the font among them). */
enum { JBI_MAX_TEXTURES = 1024 };

typedef struct jbi_tex_slot {
    int live;
    int internal;    /* the library's own (the font): destroy and the writes refuse it */
    jb_texture tex;  /* as jb_texture_create returned it */
    jbi_rect texels; /* in VRAM pixels */
    jbi_rect clut;   /* the palette's pixels; w 0 for none (15-bit) */
    jbi_rect clut2;  /* the font's blending CLUT when apart from clut; else w 0 */
} jbi_tex_slot;

struct jbi_textures {
    jbi_placer placer;
    jbi_tex_slot slot[JBI_MAX_TEXTURES];
    int font;            /* the font's slot, or -1 until it is first used */
    jbi_rect font_blend; /* the font's blending CLUT (16 entries) */
    /* jb_texture.id: the count of textures the context has made, so each
       has its own (1, 2, ...: the same calls give the same ids); with
       jb_texture.owner, the context's address, it names a texture of one
       live context. serial at its largest refuses further textures. */
    uint32_t serial;
    /* The pair search's row bands of the texels' request (texture.c,
       place; jbi_placer_bands), and how many texel searches and band
       builds the placer has run (a statistic, for tests). */
    uint64_t bands[JBI_PLACER_H][JBI_PLACER_WORDS];
    unsigned long texel_searches;
};

/* The context's table (jb_create; NULL without memory), everything free,
   and its end (jb_destroy). */
struct jbi_textures *jbi_textures_create(jb_context *jb);
void jbi_textures_destroy(jb_context *jb);

/* Marks the display buffers the frame uses (jbi_frame_init's, from the
   reserved size) taken for good. */
void jbi_textures_mark_buffers(jb_context *jb, const jbi_rect *buffers, int n);

/* Turns the mask settings (GP0(E6h)) off for an upload the library makes,
   sending E6h only if one was on; returns the word that puts them back,
   which jbi_mask_restore sends (only if one was on). */
uint32_t jbi_mask_off(jb_context *jb);
void jbi_mask_restore(jb_context *jb, uint32_t saved);

/* The slot of a live texture equal to *t in every field, its id among
   them (as jb_texture_create returned it), or -1. */
int jbi_texture_find(const jb_context *jb, const jb_texture *t);

/*
 * The texture window, GP0(E2h)'s fields in units of 8 texels
 * (jb_set_texture_window), that makes texel coordinates repeat over a
 * repeat texture: with it, page coordinate u becomes t->u + (u mod
 * t->width), and v likewise, so a primitive may give any u, v 0..255 and
 * see the texture tiled from the page's (0, 0). All 0 (no window) for a
 * texture without repeat, or one 256 texels in that direction.
 */
void jbi_texture_window(const jb_texture *t, int *mask_x, int *mask_y, int *offset_x,
                        int *offset_y);

/* Places and uploads a texture, as jb_texture_create; fn names the caller
   in error details; internal marks it the library's (the font: its
   palette is 32 entries, two CLUTs, together when they fit, else apart;
   the second in font_blend). */
jb_result jbi_texture_create(jb_context *jb, const char *fn, const jb_texture_desc *desc,
                             int internal, jb_texture *out);

/* Writes w x h texels at (x, y) of the texture in slot, as jb_texture_write
   does but with no checks and for internal textures too (the font). */
void jbi_texture_put(jb_context *jb, int slot, int x, int y, int w, int h, const void *texels);

/*
 * The font (font.c): an original 8 x 8 bitmap font of ASCII 32..126, a
 * 4-bit texture of 16 x 6 glyphs (128 x 48 texels) with 32 palette
 * entries: its CLUT (clut_x) maps texel 1 to 0x4210, a 15-bit grey of 16
 * (half of full), so a glyph drawn modulated with colour c shows (c >> 3)
 * per channel, the colour a fill of c gives; texel 0 is 0x0000 and not
 * drawn. A second CLUT, the same with bit 15 set (0xC210), is for text that
 * blends: jbi_font_blend gives the font texture with it (at clut_x + 16 when
 * the two fit together, else wherever it found room). jbi_font places the
 * font at first use, internal, and returns it (the opaque CLUT), or NULL
 * with the failure recorded (jbi_api_fail: no room in VRAM,
 * JB_ERROR_OUT_OF_MEMORY, named by fn).
 */
enum {
    JBI_FONT_FIRST = 32,
    JBI_FONT_LAST = 126,
    JBI_FONT_CELL = 8,    /* glyph cells are 8 x 8 texels; text advances 8 */
    JBI_FONT_COLUMNS = 16,
    JBI_FONT_ROWS = 6,
    JBI_FONT_TEXEL = 0x4210 /* texel 1 in the opaque CLUT */
};
const jb_texture *jbi_font(jb_context *jb, const char *fn);

/* The font texture with its blending CLUT (the font must be placed). */
jb_texture jbi_font_blend(const jb_context *jb);

/* The top-left texel of character ch's cell within the font texture (add
   the texture's u, v for page coordinates): 1, or 0 for a character
   outside 32..126, with *u, *v those of '?'. */
int jbi_font_glyph(int ch, int *u, int *v);

/* Row r (0..7, top first) of character ch's glyph (32..126): bit 7 is the
   leftmost texel. 0 outside the range. */
unsigned jbi_font_row(int ch, int r);

/* jb_text_size (draw2d.c) with the most characters or lines a text may
   have as a parameter: jb_text_size is this with INT_MAX / 8, the most
   whose size in pixels fits an int. For testing that limit on short
   texts. */
jb_result jbi_text_size_most(const char *text, size_t most, int *width, int *height);

#endif

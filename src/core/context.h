/*
 * The context: one emulated GPU. It owns the GP0 drawing state (E1h-E6h),
 * the command decoder (gp0.c) and a backend (backend.h; the software one,
 * src/soft), and keeps no state outside itself.
 *
 * Commands arrive as GP0 words (jbi_gp0) or as calls to the transfer
 * functions; primitives become records (prim.h) that the backend may draw
 * later, and every call that reads pixels (store, scanout) finishes them
 * first.
 *
 * Internal resolution (jbi_configure). At scale N > 1 the software backend
 * keeps a second, scaled VRAM of 1024N x 512N pixels beside the 1x one:
 *   - every primitive is rasterised into the scaled VRAM at N times the
 *     resolution, which is what is displayed, and into the 1x VRAM exactly
 *     as the console would (with exact_1x, config.h; otherwise the 1x
 *     pixels are filled from the scaled ones when something reads them),
 *     so stores, CLUTs and 4- and 8-bit textures see what they would on
 *     the console;
 *   - fills, copies and CPU-to-VRAM transfers go to both, a 1x pixel
 *     becoming an N x N block;
 *   - in the scaled pass, 15-bit textures are read from the scaled VRAM at
 *     sub-texel precision. Uploaded texels are replicated N x N there, so
 *     that is the same as reading the 1x VRAM, and where the program samples
 *     what it drew (render to texture) it gets the high-resolution image.
 *     4- and 8-bit textures come from the 1x VRAM, and their palettes from
 *     the CLUT cache (src/soft/soft.c), which loads them from the 1x VRAM.
 *
 * Ownership and threads: jbi_context_create returns a context the caller
 * owns until jbi_context_destroy, which also destroys the backend and joins
 * its worker threads. A context is used from one thread at a time;
 * different contexts are independent and may be used from different
 * threads at once. The precise source and the log callback are called only
 * on the thread calling into the context.
 */
#ifndef JBI_CORE_CONTEXT_H
#define JBI_CORE_CONTEXT_H

#include <stddef.h>
#include <stdint.h>

#include "core/backend.h"
#include "core/config.h"
#include "core/precise_source.h"
#include "core/prim.h"

/* The GP0 drawing state. */
typedef struct jbi_draw_state {
    /* GP0(E1h) draw mode */
    int tpage_x;      /* texture page base, VRAM pixels */
    int tpage_y;
    int semi_mode;    /* 0: B/2+F/2, 1: B+F, 2: B-F, 3: B+F/4 */
    int tex_depth;    /* JBI_TEX_* (3 behaves as 15-bit) */
    int dither;
    int draw_display; /* drawing to the displayed area allowed */
    int rect_flip_x;  /* rectangle texture flip (E1 bits 12, 13) */
    int rect_flip_y;
    /* E1h bit 11 as stored, GPUSTAT bit 15: psx-spx's "Texture page Y Base
       2" (the console's ps1-tests call it texture disable). Written by E1h
       and by a textured polygon's page only while allow_tex_disable is
       set, else written as 0. Stored and reported; it changes no drawing
       (include/jitterbug.h, jb_gp1). */
    int tex_disable;
    /* GP1(09h) bit 0, which allows bit 11 to be written. Not changed by
       GP1(00h) (psx-spx) or by jbi_reset_state. */
    int allow_tex_disable;
    /* GP0(E2h) texture window, in texels */
    uint32_t tw_and_x, tw_or_x;
    uint32_t tw_and_y, tw_or_y;
    /* GP0(E3h), GP0(E4h) drawing area, inclusive */
    int clip_x0, clip_y0, clip_x1, clip_y1;
    /* GP0(E5h) drawing offset */
    int off_x, off_y;
    /* GP0(E6h) mask bit handling */
    uint16_t set_mask; /* 0x8000 when every drawn pixel gets the mask bit */
    int check_mask;    /* do not draw over pixels with the mask bit */
} jbi_draw_state;

typedef struct jb_context jb_context;

/* The public API's state (src/api/api.h). */
struct jbi_api;

struct jb_context {
    jbi_draw_state draw;
    jbi_draw_state checkpoint_draw;
    int checkpoint_active;
    jbi_precise_source precise; /* valid when has_precise */
    int has_precise;
    int texture_perspective;    /* jbi_set_texture_mapping */
    int clut_cache;             /* jbi_set_clut_cache; 1 when created */
    int scale;                  /* the backend's scale in effect */
    jbi_backend *backend;       /* owned */
    jbi_allocator alloc;        /* the configuration's (config.h) */
    /* The public API's state when the context was made by jb_create
       (src/api), which also frees it; NULL otherwise. */
    struct jbi_api *api;
};

/* A context with the software backend, VRAM all zero and the drawing state
   all zero (as a fresh console's, before any reset), or NULL without
   memory. cfg NULL means the defaults (jbi_config_default). */
jb_context *jbi_context_create(const jbi_config *cfg);
void jbi_context_destroy(jb_context *ctx);

/* The backend, for its own accessors (src/soft/soft.h). */
jbi_backend *jbi_context_backend(jb_context *ctx);

/* Reset the drawing state (GP1(01h)-like; VRAM is kept). */
void jbi_reset_state(jb_context *ctx);

/*
 * Execute `count` GP0 words. A packet may hold several commands (DR_ENV,
 * DR_MODE); a truncated trailing command is dropped. Returns the number of
 * words consumed, at most count. Primitives may still be queued on return;
 * jbi_flush finishes them.
 */
int jbi_gp0(jb_context *ctx, const uint32_t *words, int count);

/*
 * Sub-pixel data for one polygon or single line, given with its words
 * rather than looked up in the precise source (the friendly level's
 * records carry it: src/api/record.h).
 *
 *   v[i]       vertex i in the command's order (a quad's Z order): x and y
 *              in 1/JBI_PRECISE_ONE pixels before the drawing offset, z the
 *              view depth (SZ units, the GTE's; any positive scale works,
 *              as only ratios matter), 0 unknown.
 *   known      bit i set: v[i].x and v[i].y are given. A given position
 *              must lie within one pixel and an eighth of the vertex's
 *              integer position in the words, as the precise source's
 *              hits do; one further away is not used (the vertex stays on
 *              the integer grid), which keeps the rasteriser's bounds.
 *   positions  whether the positions are used: the scaled pass (above 1x)
 *              draws known vertices there, as the precise source's
 *              positions with precise vertices on. 0 keeps every vertex on
 *              the integer grid.
 *
 * Depths are used for perspective-correct texturing whenever the context's
 * texture mapping is perspective (jbi_set_texture_mapping): a textured
 * triangle whose three vertices have z > 0 is drawn with them, otherwise
 * affinely.
 *
 *   pattr      with has_pattr set, vertex i's u, v, r, g, b for the
 *              perspective-correct pass in place of the words' (prim.h,
 *              u.tri.pattr): a clipped polygon's new vertices carry their
 *              attributes interpolated both ways, the words the affine ones
 *              (which the 1x pass draws above 1x) and pattr the
 *              perspective-correct ones. Unused without depths.
 *
 * With known 0, positions 0, every z 0 and has_pattr 0 the words draw
 * exactly what jbi_gp0 draws without a precise source.
 */
typedef struct jbi_subpixel {
    jbi_precise_vertex v[4];
    unsigned known;
    int positions;
    uint8_t pattr[4][5];
    int has_pattr;
    /* JBI_F_NO_1X and JBI_F_NO_SCALED (prim.h): the passes the polygon is
       not drawn in; 0 for both. */
    unsigned skip;
} jbi_subpixel;

/*
 * jbi_gp0 with sub-pixel data for the polygon or single line in words
 * (the precise source is not asked). words must be one such command
 * (polygon 20h-3Fh, or line 40h-5Fh without the polyline bit); other
 * commands are carried out as jbi_gp0 carries them, and a polyline's
 * vertices are looked up as usual. sp NULL is jbi_gp0.
 */
int jbi_gp0_subpixel(jb_context *ctx, const uint32_t *words, int count, const jbi_subpixel *sp);

/* VRAM transfers, with the hardware's sizes (0 means the maximum) and
   wrapping at the VRAM edges. A fill is queued like a primitive; the
   others finish queued primitives first. Loads and copies honour the mask
   settings. */
void jbi_fill(jb_context *ctx, int x, int y, int w, int h, uint32_t rgb24);
void jbi_load(jb_context *ctx, int x, int y, int w, int h, const uint16_t *src);
void jbi_store(jb_context *ctx, int x, int y, int w, int h, uint16_t *dst);
void jbi_copy(jb_context *ctx, int sx, int sy, int dx, int dy, int w, int h);
/* A store from the 1x VRAM, with the same sizes and wrapping: the console's
   pixels at any scale when exact_1x is on (backend.h, store_1x). At 1x it
   is jbi_store. */
void jbi_store_1x(jb_context *ctx, int x, int y, int w, int h, uint16_t *dst);

/* Finish every queued primitive. */
void jbi_flush(jb_context *ctx);

typedef enum {
    JBI_CHECKPOINT_OK,
    JBI_CHECKPOINT_ACTIVE,
    JBI_CHECKPOINT_UNSUPPORTED,
    JBI_CHECKPOINT_NO_MEMORY,
} jbi_checkpoint_result;

/*
 * One temporary pass through the console core. Begin finishes outstanding
 * GPU work. Rollback restores the GP0 drawing registers and backend state,
 * including VRAM. Use core drawing, transfers and scanout during the pass.
 * The public API manages its own command stream, display and frame state.
 * Treat raw VRAM pointers as read-only until rollback.
 * Core configuration setters leave settings as they are during a pass.
 * Rollback is harmless when no checkpoint is active.
 */
jbi_checkpoint_result jbi_checkpoint_begin(jb_context *ctx);
void jbi_checkpoint_rollback(jb_context *ctx);

/*
 * Internal resolution and threads, VRAM contents kept: scale, dither and
 * threads as in jbi_config. Returns the scale in effect. jbi_threads: the
 * rasterising threads in effect; jbi_scale: the scale.
 */
int jbi_configure(jb_context *ctx, int scale, int dither, int threads);
int jbi_threads(const jb_context *ctx);
/* Only the threads, with no pixel changed: the threads in effect, or 0 if
   it could not be done (the old ones are kept). */
int jbi_set_threads(jb_context *ctx, int threads);
int jbi_scale(const jb_context *ctx);
/* jbi_config.exact_1x, at run time. */
void jbi_set_exact_1x(jb_context *ctx, int exact);
/* The CLUT cache (backend.h, set_clut_cache), on or off, and whether it
   is on; on when a context is created. The public switch is
   jb_set_clut_cache (docs/design.md, "Runtime options"). */
void jbi_set_clut_cache(jb_context *ctx, int on);
int jbi_clut_cache(const jb_context *ctx);
/* GP0(01h): clears the CLUT cache (backend.h, clear_cache). */
void jbi_clear_cache(jb_context *ctx);
/* jbi_config.texture_perspective, at run time; and its value. */
void jbi_set_texture_mapping(jb_context *ctx, int perspective);
int jbi_texture_mapping(const jb_context *ctx);
/* The precise source (copied), or NULL for none. */
void jbi_set_precise_source(jb_context *ctx, const jbi_precise_source *src);

/* Display output (backend.h, scanout): the bytes a frame may need at the
   current scale, and the frame. */
size_t jbi_scanout_bytes(const jb_context *ctx);
int jbi_scanout(jb_context *ctx, int x, int y, int w, int h, int rgb24, int enabled,
                void *pixels, int *width, int *height, int *stride);

#endif

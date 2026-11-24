/*
 * The public API's state (include/jitterbug.h): what a console's GPU keeps
 * at its ports beside the drawing state the core context holds
 * (src/core/context.h). A context made by jb_create owns one, through
 * jb_context.api.
 *
 *   create.c   jb_create, jb_destroy, results and error strings, the
 *              run-time scale and threads
 *   stream.c   GP0 as a word stream (the command accumulator), GPUREAD
 *   display.c  GP1, GPUSTAT, jb_frame
 *   vram.c     jb_vram_*: VRAM transfers as calls
 *   prim.c     jb_prim_* and the drawing-state calls: GP0 commands built
 *              from structured arguments
 *   gte.c      jb_gte_*: the context's GTE, its precise-vertex table, and
 *              the precise-vertex and perspective-correct options
 *   texture.c  jb_texture_* and the VRAM placer's table (texture.h)
 *   font.c     the library's text font (texture.h)
 *   jbt.c      texture files: jb_texture_parse, jb_texture_load(_mem)
 *   jbm.c      mesh files: jb_mesh_load(_mem), jb_mesh_destroy, and the
 *              context's list of loaded meshes
 *   files.c    what the two share: sections, reading a file (files.h);
 *              the formats' layout is formats.h
 *   gte_helpers.c  the GTE helpers over it (jb_gte_set_matrix ...)
 *   scene.c    the friendly scene's setters (jb_set_camera ...)
 *   record.c   recorded primitives and the ordering table, jb_ot_*
 *              (record.h)
 *   frame.c    the frame: jb_begin_frame, jb_end_frame, layers, the
 *              buffers, widescreen, the options without a home elsewhere,
 *              the sticky error's jb_check, and the 2D draws (frame.h)
 */
#ifndef JBI_API_API_H
#define JBI_API_API_H

#include <stddef.h>
#include <stdint.h>

#include <jitterbug.h>

#include "core/context.h"
#include "core/layout.h"
#include "geom/gte.h"
#include "geom/precise.h"
#include "geom/scene.h"

/* The longest fixed-length GP0 command: a textured Gouraud quad. */
enum { JBI_CMD_MAX = 12 };

/* What the next GP0 word is. */
enum {
    JBI_IN_COMMAND,  /* the first word of a command */
    JBI_IN_FIXED,    /* more of a fixed-length command (cmd, have, need) */
    JBI_IN_POLYLINE, /* a polyline's next word (pl_*) */
    JBI_IN_LOAD      /* CPU-to-VRAM pixel data (ld_*) */
};

struct jbi_api {
    /* GP0 input (stream.c). */
    int state; /* JBI_IN_* */
    uint32_t cmd[JBI_CMD_MAX];
    int have, need;
    /* A polyline: its command word, the last complete vertex, and the
       colour word of a Gouraud vertex whose position is still to come. */
    uint32_t pl_cmd, pl_color, pl_xy, pl_next_color;
    int pl_verts;   /* complete vertices, counted up to 2 */
    int pl_want_xy; /* the next word is a position, never a terminator */
    /* A CPU-to-VRAM transfer: its rectangle (already reduced), the row
       being assembled, and the pixels still to come. */
    int ld_x, ld_y, ld_w, ld_row, ld_col;
    size_t ld_left;
    uint16_t ld_line[JBI_VRAM_W];
    /* A VRAM-to-CPU transfer: the rectangle's pixels, and the next one to
       read; nothing to read when rd_next == rd_count. */
    uint16_t *rd_pix; /* JBI_VRAM_W x JBI_VRAM_H */
    size_t rd_count, rd_next;
    uint32_t latch; /* what GPUREAD returns otherwise */
    /* GP0 state the core does not keep: E2h-E5h as sent (GP1(10h)) and
       the interrupt flag. */
    uint32_t env[4];
    int irq;

    /* Display (display.c). */
    int dma;         /* GP1(04h) */
    int display_off; /* GP1(03h) */
    int disp_x, disp_y;
    uint32_t hrange, vrange; /* GP1(06h), GP1(07h): kept, nothing reads them */
    uint32_t mode;           /* GP1(08h) bits 0-7 */
    int disp_w, disp_h, rgb24; /* what jb_frame shows */
    int mode_explicit;         /* GP1(08h) sent since GP1(00h) */
    int cfg_w, cfg_h;          /* jb_config's display size */
    uint8_t *frame;            /* jb_frame's pixels */
    size_t frame_bytes;

    /* Geometry (gte.c). The GTE records its projections into precise
       while either option is on; the context's precise source is this
       table then, giving positions only with precise_vertices. */
    jbi_gte gte;
    jbi_precise *precise; /* owned, jbi_precise_size() bytes */
    int precise_vertices, perspective_correct;

    /* The friendly scene: camera, lights, fog, the geometry path and its
       own GTE (src/geom/scene.h; set in scene.c, used by the 3D draws). */
    jbi_scene scene;

    int threads; /* jb_config.threads as last set: 0 for one per CPU */

    /* Textures placed in VRAM (texture.c): owned. */
    struct jbi_textures *textures;
    /* Meshes loaded from .jbm files and not yet destroyed (jbm.c): owned,
       a doubly linked list. */
    struct jbi_mesh_block *meshes;
    /* The frame (frame.c). The buffers are fixed when the context is
       made, from the reserved size: res_w is the larger of narrow_w (the
       configured width) and, when widescreen is reserved, wide_w. */
    int narrow_w, wide_w, res_w, res_h;
    int ws_reserved;   /* jb_config.widescreen */
    int widescreen;    /* as last asked for (jb_set_widescreen) */
    int nbuffers;      /* 1 or 2 */
    jbi_rect buffer[2];
    int frame_w;       /* the width frames are drawn at now */
    int shown_w;       /* the frame width the display last took (frame.c) */
    int in_frame, back; /* between jb_begin_frame and jb_end_frame; the
                           buffer being drawn */
    int ended;         /* a frame has ended (the display is enabled after
                          the first) */
    int parity;        /* the back buffer of the next frame, of two */
    int layer;         /* JB_LAYER_* for 2D draws */
    int max_records, ot_size;
    struct jb_ot *frame_ot; /* from the first jb_begin_frame: ot_size + 2
                               buckets (frame.c) */

    jb_log_fn log;
    void *log_user;
    char error[200];
    /* The sticky error: the first failure since it was last taken
       (jb_check, jb_end_frame), JB_OK if none, and its detail. */
    jb_result sticky;
    char sticky_detail[200];
};

/* Records the detail of a failure on jb (jb_error_string), latches it as
   the sticky error if none is latched, and logs it; returns r. */
jb_result jbi_api_fail(jb_context *jb, jb_result r, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* Takes the sticky error (create.c): returns it (JB_OK if none) and clears
   it; *detail (when detail is not NULL) gets its detail, "" for none,
   valid until the next failure. */
jb_result jbi_api_take_error(jb_context *jb, const char **detail);

/* Frees the meshes jb_mesh_load(_mem) made that are still loaded (jbm.c),
   for jb_destroy. */
void jbi_meshes_destroy(jb_context *jb);

/* GP1(01h): drop partial input and unread data (stream.c). */
void jbi_api_reset_input(struct jbi_api *a);
/* GP1(00h) (display.c). */
void jbi_api_reset(jb_context *jb);

/* One complete fixed-length GP0 command of n words, exactly as jb_gp0
   carries it out once its last word has arrived (stream.c), without
   touching jb_gp0's partial input. */
void jbi_api_command(jb_context *jb, const uint32_t *w, int n);

/* Makes the precise table, the context's precise source and its texture
   mapping follow the two options in jb->api (gte.c). */
void jbi_api_apply_options(jb_context *jb);

/*
 * The structured commands (prim.c), as the range rule and the words apart.
 * A check fails through jbi_api_fail with fn's name, as the jb_prim_* call
 * of that kind does; with positions 0 it checks everything but the
 * positions (and a rectangle's size only for being >= 0), for the friendly
 * draws, which clip instead. An emitter sends the words of an argument
 * that passed its check with positions, exactly as the jb_prim_* call
 * does, with sp (NULL for none) as the sub-pixel data of a polygon or line
 * (jbi_gp0_subpixel). jbi_polyline_segment is segment i (from v[i] to
 * v[i + 1]) of a checked polyline as a line: sending each segment so,
 * each with its own blend mode word, draws what jb_prim_polyline draws.
 */
jb_result jbi_check_poly(jb_context *jb, const char *fn, const jb_poly *p, int positions);
void jbi_emit_poly(jb_context *jb, const jb_poly *p, const jbi_subpixel *sp);
jb_result jbi_check_line(jb_context *jb, const char *fn, int gouraud, int blend,
                         const jb_vertex *v, size_t count, int positions);
void jbi_emit_line(jb_context *jb, const jb_line *l, const jbi_subpixel *sp);
jb_result jbi_check_polyline(jb_context *jb, const char *fn, const jb_polyline *l,
                             int positions);
jb_line jbi_polyline_segment(const jb_polyline *l, size_t i);
jb_result jbi_check_rect(jb_context *jb, const char *fn, const jb_rect *r, int positions);
void jbi_emit_rect(jb_context *jb, const jb_rect *r);

/* The frame's part of jb_create (frame.c): the buffers and capacities
   from the configuration, which create.c has checked. */
void jbi_frame_init(jb_context *jb, const jb_config *c);
/* And of jb_destroy. */
void jbi_frame_free(jb_context *jb);

#endif

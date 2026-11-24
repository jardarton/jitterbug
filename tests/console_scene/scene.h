/*
 * The console-level first-light scene: examples/first-light as it was
 * first written, on the console's API alone (jb_vram_load, the GTE by
 * hand, its own ordering table, jb_prim_*, GP1 words), kept as a test
 * because it is the one whole program on that level, with the hashes it
 * was recorded with (expected.txt here). Its runner is main.c; jb-bench
 * (tools/jb-bench/firstlight.c) and determinism_threads use it too.
 *
 * first-light: the scene, without any platform code.
 *
 * A lit, textured cube spins above a checkered floor, a glowing sprite
 * orbits it, and a small panel sits in the corner, all drawn through
 * jitterbug's public API (include/jitterbug.h) the way a console program
 * drew them: textures and palettes uploaded into VRAM, the GTE doing the
 * transforms, projection, lighting, culling and depth, primitives sorted
 * back to front in an ordering table, and two display buffers in VRAM.
 *
 * Everything the scene draws follows from the frame number and the
 * switches, in integer arithmetic only, so every machine draws the same
 * frames. The front-ends (main_headless.c, main_sdl.c, web/web.c) own the
 * frame counter, the window and the clock.
 */
#ifndef FIRST_LIGHT_SCENE_H
#define FIRST_LIGHT_SCENE_H

#include <jitterbug.h>

/* The switches a front-end can flip at run time. */
typedef struct fl_switches {
    int scale;       /* internal resolution, 1 to 8 (the front-ends offer 1-4) */
    int precise;     /* precise vertices: no vertex jitter above 1x */
    int perspective; /* perspective-correct textures: no affine warp */
    int dither;      /* the console's dithering of shaded pixels */
    /* Not a switch: where the context's messages go (jb_config.log,
       log_user), set when it is created; NULL for nowhere. */
    jb_log_fn log;
    void *log_user;
} fl_switches;

/* The switches the console has: 1x, no precise vertices, affine
   textures, dithering on; no log. */
#define FL_SWITCHES_CONSOLE {1, 0, 0, 1, NULL, NULL}

enum {
    FL_WIDTH = 320,   /* the display, in 1x pixels */
    FL_HEIGHT = 240,
    FL_OT_LEN = 256,  /* ordering-table entries */
    FL_PRIMS = 64,    /* primitives a frame can hold */
    FL_LINE_MAX = 6   /* vertices in a polyline */
};

/* One entry of the ordering table's primitive buffer. */
typedef struct fl_prim {
    int next; /* the next primitive in the same entry, or -1 */
    enum { FL_POLY, FL_RECT, FL_LINE, FL_POLYLINE } kind;
    union {
        jb_poly poly;
        jb_rect rect;
        jb_line line;
        struct {
            int gouraud, blend, count;
            jb_vertex v[FL_LINE_MAX];
        } polyline;
    } u;
} fl_prim;

/* All of the scene's state. */
typedef struct fl_scene {
    jb_context *jb;
    fl_switches sw;
    int buffer;       /* the display buffer drawn next: 0 or 1 */
    int shown;        /* the display has been enabled */
    jb_result error;  /* the first error of the last fl_scene_draw */
    jb_texture floor, plasma, emblem, star;
    int ot[FL_OT_LEN]; /* the first primitive of each entry, or -1 */
    fl_prim prims[FL_PRIMS];
    int nprims;
} fl_scene;

/* Makes the context (threads: as jb_config.threads), uploads the textures
   and palettes and applies the switches. JB_OK, or why not; on failure
   there is nothing to free. */
jb_result fl_scene_init(fl_scene *s, const fl_switches *sw, int threads);

/* Applies new switches; they take effect with the next frame drawn. */
jb_result fl_scene_set_switches(fl_scene *s, const fl_switches *sw);

/* Draws frame number `frame` into the back buffer and shows it. Returns the
   first error any call returned (the frame is still drawn as far as it
   could be). */
jb_result fl_scene_draw(fl_scene *s, unsigned frame);

/* The frame on show (jb_frame): RGBA bytes, *w x *h at the current scale. */
const uint8_t *fl_scene_frame(fl_scene *s, int *w, int *h);

void fl_scene_free(fl_scene *s);

#endif

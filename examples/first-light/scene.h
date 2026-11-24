/*
 * first-light: the scene, without any platform code.
 *
 * A lit, textured cube spins above a checkered floor, a glowing star
 * orbits it on a ring of lines, and a small panel sits in the corner, all
 * drawn with jitterbug's friendly level (include/jitterbug.h): textures the
 * library places in VRAM, a camera and lights, meshes, a billboard, 3D
 * lines, 2D draws and text, recorded between jb_begin_frame and
 * jb_end_frame and sorted by the frame's ordering table.
 *
 * Everything the scene draws follows from the frame number and the
 * switches, with the library's own maths (jb_sinf, jb_cosf), which gives
 * the same bits on every machine, so every machine draws the same frames.
 * The front-ends (main_headless.c, main_sdl.c, web/web.c) own the frame
 * counter, the window and the clock.
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
    int widescreen;  /* the 16:9 display, 427 x 240 */
    int geometry;    /* JB_GEOMETRY_GTE (the console's arithmetic) or _FLOAT */
    int clut_cache;  /* the console's CLUT cache (on), or every palette read */
    /* Not a switch: where the context's messages go (jb_config.log,
       log_user), set when it is created; NULL for nowhere. */
    jb_log_fn log;
    void *log_user;
} fl_switches;

/* The switches the console has: 1x, no precise vertices, affine textures,
   dithering on, 4:3, the GTE, the CLUT cache on; no log. */
#define FL_SWITCHES_CONSOLE {1, 0, 0, 1, 0, JB_GEOMETRY_GTE, 1, NULL, NULL}

/* All of the scene's state: the context, the textures it placed, and the
   meshes, which point at them. */
typedef struct fl_scene {
    jb_context *jb;
    fl_switches sw;
    jb_texture floor, plasma, emblem, star;
    jb_gte_vec cube_pos[24], cube_normal[24];
    uint8_t cube_uv[48];
    jb_mesh_face cube_faces[6];
    jb_material cube_mat[2], floor_mat, shadow_mat;
    jb_mesh cube, floor_mesh, shadow;
} fl_scene;

/* Makes the context (threads: as jb_config.threads), places the textures,
   sets the camera and lights and applies the switches. JB_OK, or why not;
   on failure there is nothing to free. */
jb_result fl_scene_init(fl_scene *s, const fl_switches *sw, int threads);

/* Applies new switches; they take effect with the next frame drawn. */
jb_result fl_scene_set_switches(fl_scene *s, const fl_switches *sw);

/* Draws frame number `frame` and shows it. Returns the frame's first error
   (jb_end_frame's), the frame still drawn as far as it could be. */
jb_result fl_scene_draw(fl_scene *s, unsigned frame);

/* The frame on show (jb_frame): RGBA bytes, *w x *h at the current scale. */
const uint8_t *fl_scene_frame(fl_scene *s, int *w, int *h);

void fl_scene_free(fl_scene *s);

#endif

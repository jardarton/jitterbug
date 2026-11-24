/*
 * The demo: the scene, without any platform code.
 *
 * A camera flies over an archipelago at sunset: textured hills, beaches,
 * conifers, a ring of standing stones, a pier of long planks (for the
 * affine wobble), a lighthouse sweeping its beam, the sea moving by
 * palette animation, a sun and fireflies that glow by additive blending,
 * haze by the GTE's depth cue, and clouds in a sky drawn at the console
 * level; twice round the loop the camera slows and turns to look (at the
 * pier, at the stones), where the jitter shows best. Each of the
 * console's artefacts is a switch (dm_switches), flipped by a key
 * (dm_key), and a small panel shows them.
 *
 * Everything the scene draws follows from the frame number and the
 * switches, through integers and the library's own maths (jb_sinf,
 * jb_cosf), which gives the same bits on every machine. The front-ends
 * (main_headless.c, main_sdl.c, web/web.c) own the frame counter, the
 * window and the clock; dm_key is the one keyboard they share.
 */
#ifndef DEMO_SCENE_H
#define DEMO_SCENE_H

#include <jitterbug.h>

#include "assets.h"
#include "terrain.h"

/* The switches. */
typedef struct dm_switches {
    int scale;       /* internal resolution, 1 to 8 (the keys offer 1-4) */
    int precise;     /* precise vertices: no vertex jitter above 1x */
    int perspective; /* perspective-correct textures: no affine wobble */
    int dither;      /* the console's dithering of shaded pixels */
    int widescreen;  /* the 16:9 display, 427 x 240 */
    int geometry;    /* JB_GEOMETRY_GTE (the console's arithmetic) or _FLOAT */
    int clut_cache;  /* the console's CLUT cache (on), or every palette read */
    int fog;         /* the haze (the GTE's depth cue) */
    int panel;       /* the panel of switches and keys */
    /* Not a switch: where the context's messages go (jb_config.log,
       log_user), set when it is created; NULL for nowhere. */
    jb_log_fn log;
    void *log_user;
} dm_switches;

/* The console's switches: 1x, no precise vertices, affine textures,
   dithering on, 4:3, the GTE, the CLUT cache on; fog and the panel on; no
   log. */
#define DM_SWITCHES_CONSOLE {1, 0, 0, 1, 0, JB_GEOMETRY_GTE, 1, 1, 1, NULL, NULL}

/*
 * The flight. The camera's path (scene.c, path) repeats every DM_PATH of
 * its own units; the camera moves one of them a frame but in DM_SLOWS slow
 * passages, each DM_SLOW_W frames long, where it slows to about a sixth of
 * that and turns to look at something (the pier, the standing stones) as
 * it passes, DM_SLOW_A units fewer than frames. So one flight round the
 * loop is DM_LOOP frames, and repeats.
 */
enum {
    DM_PATH = 2048,
    DM_SLOWS = 2,
    DM_SLOW_W = 640,
    DM_SLOW_A = 360,
    DM_LOOP = DM_PATH + DM_SLOWS * DM_SLOW_A /* 2768 */
};

/* A slow passage: the path's unit where the camera passes nearest its
   sight (tau), the frame it starts at (start, in the loop), the sight
   (one copy of it on the map) and how far the camera comes down for it
   at the middle (lower). */
typedef struct dm_slow {
    float tau;
    unsigned start;
    jb_vec3 at;
    float lower;
} dm_slow;

/*
 * The keys, as characters, shared by every front-end (the window and the
 * page map their keys to these; a scripted run gives them by frame):
 *   '1'-'4'  internal scale       'v'  precise vertices
 *   't'      perspective-correct  'd'  dithering
 *   'c'      the CLUT cache       'w'  widescreen
 *   'g'      GTE or float         'f'  fog
 *   'h'      the panel            'p'  pause (Space in the window and page)
 *   'n'      one frame on, while paused (Right too)
 * dm_key applies key to *sw, *paused and *frame and returns 1, or returns
 * 0 with nothing changed for any other key. It only changes the struct:
 * dm_scene_set_switches gives new switches to the scene.
 */
int dm_key(int key, dm_switches *sw, int *paused, unsigned *frame);

/* A fixed camera, for a still of the scene (examples/header-images draws
   the README's banner with one): the eye in the world, its heading in
   turns from +z towards +x, and its pitch in turns below the horizon. */
typedef struct dm_shot {
    jb_vec3 eye;
    float yaw, pitch;
} dm_shot;

/* A tree, standing stone or firefly's place: x, z in the world (one copy
   of the repeating map), y its ground, and a turn and size of its own. */
typedef struct dm_place {
    float x, y, z;
    float turn, size;
} dm_place;

enum { DM_MAX_TREES = 1024, DM_STONES = 7 };

/* A chunk to draw: its place in the world, in chunks (any ints), and its
   centre's distance from the eye, squared. */
typedef struct dm_visible {
    int cx, cz;
    float dist2;
    int fade; /* its textures' fade level, 0..DM_FADES - 1 (scene.c, terrain) */
} dm_visible;

/* The levels the terrain's textures fade through with the haze. */
enum { DM_FADES = 16 };

/* The sea's swell (scene.c, sea_height): DM_WAVES travelling waves, their
   crests at sea level at the most, their troughs DM_SEA_DEPTH below. */
enum { DM_WAVES = 3, DM_SEA_DEPTH = 13 };

enum { DM_MAX_VISIBLE = 512 }; /* chunks drawn in one frame */

/* All of the scene's state. */
typedef struct dm_scene {
    jb_context *jb;
    dm_switches sw;
    dm_assets assets;
    dm_terrain *terrain; /* allocated: about 1.3 MB */
    jb_material land_mat[DM_LAND_MATERIALS], bed_mat, water_mat;
    /* The same at each fade level (scene.c, terrain): level k draws the
       texels through palettes k / (DM_FADES - 1) of the way from their
       own colours to the one the haze takes their texture to (fade_tex:
       the textures with those palettes' CLUTs, fade_clut the 1 x 1
       textures made to hold them; slot 0 sand, 1 grass, 2 rock, 3 water). */
    jb_material land_fade[DM_FADES][DM_LAND_MATERIALS], bed_fade[DM_FADES], water_fade[DM_FADES];
    jb_texture fade_tex[DM_FADES][4], fade_clut[DM_FADES][4];
    uint16_t fade_to[4]; /* each slot's colour at the last level, 15-bit */
    uint32_t mean[DM_TEXTURES];
    /* The fade check's (demo-headless --fade): every chunk this many
       levels farther on (0 to draw the scene as it is). */
    int fade_bias;
    /* The pier check's (demo-headless --pier): parts of the pier left out
       (DM_HIDE_*), 0 to draw it all. */
    int hide;
    /* A fixed camera in place of the flight's (NULL, the default); the
       frame number still moves everything else (waves, beams, clouds). */
    const dm_shot *shot;
    uint8_t land_uv[2 * DM_CV * DM_CV], water_uv[2 * DM_WATER_V * DM_WATER_V];
    dm_place tree[DM_MAX_TREES];
    int trees;
    dm_place stone[DM_STONES];
    dm_place lighthouse;
    /* The pier: where its deck meets the shore (x, y, z), and its heading
       out to sea (turn, in turns from +z towards +x). */
    dm_place pier;
    /* The slow passages, in the order they come round the loop. */
    dm_slow slow[DM_SLOWS];
    /* The fog colour for each texture that brings its mean texel to the
       haze (scene.c, haze_for), for the nearer terrain. */
    uint32_t haze_for[DM_TEXTURES];
    /* The colour each frame is cleared to: the haze (dm_scene_init). The
       gap check (demo-headless --gaps) sets keys instead. */
    uint32_t clear;
    /* The gap check's layers (DM_GAPS_*), 0 to draw the scene as it is:
       DM_GAPS_NO_SKY leaves the sky out, so that the clear colour shows
       where a ridge cracks against it; DM_GAPS_WATER also leaves the
       seabed, the land and everything on it out and draws the sea alone,
       opaque, so that a hole in the sea shows the clear colour
       (dm_scene_covered then asks for the sea). */
    int gaps;
    /* The last frame's view, for dm_scene_covered: the eye, and the
       camera's forward, right and up, unit. */
    jb_vec3 eye, fwd, right, up;
    /* Scratch for each frame: the chunks to draw, and a chunk mesh's
       positions moved to the terrain's origin (scene.c, terrain). */
    dm_visible visible[DM_MAX_VISIBLE];
    jb_gte_vec pos[DM_CV * DM_CV];
    uint32_t sea_color[DM_WATER_V * DM_WATER_V]; /* a sea chunk's vertex colours */
    float sea_phase[DM_WAVES];                   /* each wave's, this frame, in turns */
} dm_scene;

/* Makes the context (threads: as jb_config.threads), places the assets,
   makes the terrain, sets the camera and lights and applies the switches.
   JB_OK, or why not; on failure there is nothing to free. */
jb_result dm_scene_init(dm_scene *s, const dm_switches *sw, int threads);

/* Applies new switches; they take effect with the next frame drawn. On a
   refusal (a scale without memory, say) returns it and keeps the old. */
jb_result dm_scene_set_switches(dm_scene *s, const dm_switches *sw);

/* Draws frame number `frame` and shows it; paused only changes the panel.
   Returns the frame's first error (jb_end_frame's), the frame still drawn
   as far as it could be. */
jb_result dm_scene_draw(dm_scene *s, unsigned frame, int paused);

/* The frame on show (jb_frame): RGBA bytes, *w x *h at the current scale. */
const uint8_t *dm_scene_frame(dm_scene *s, int *w, int *h);

/* Whether pixel (x, y) of the last frame drawn (at its scale) must show
   only the world: the rays through its centre and a 1x pixel to each side
   of it (where a vertex snapped to the 1x grid may move an edge) all meet
   the surface the terrain's meshes draw (dm_surface: land, or the seabed,
   which shows through the sea; with DM_GAPS_WATER in s->gaps the sea's
   plane, in a chunk that has sea) within DM_COVERED world units of the eye,
   along the view and across the ground, well inside what the scene draws
   (its far plane, 4000), found by stepping along each ray DM_COVERED_STEP
   at a time and taken only where the ray is a pixel and a half under it
   (as far as the drawn surface, its corners rounded to pixels, may be
   from the exact one). Below the sky, only the strip towards the horizon beyond
   that, where the world ends, may show the clear colour (or the sea over
   it, where a ray reaches the seabed past the far plane); the clear colour
   showing anywhere here is a gap. */
enum { DM_COVERED = 3600, DM_COVERED_STEP = 8 };
enum { DM_GAPS_NO_SKY = 1, DM_GAPS_WATER = 2 };
enum { DM_HIDE_POSTS = 1, DM_HIDE_DECK = 2 };
int dm_scene_covered(const dm_scene *s, int x, int y);

void dm_scene_free(dm_scene *s);

#endif

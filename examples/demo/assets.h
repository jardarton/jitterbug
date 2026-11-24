/*
 * The demo's assets: every texture and every fixed mesh it draws, loaded
 * here and nowhere else (the terrain, which the program makes from its
 * heightmap, is terrain.c's).
 *
 * Each is a file made by the converters from original sources:
 * assets/make-sources.c draws PNG images and writes OBJ models,
 * tools/jb-tex makes the .jbt textures and tools/jb-mesh the .jbm meshes
 * (assets/conversions.txt), and the files are built into the program
 * (files.h), so the demo, natively and in a browser, reads no file: it
 * loads them with jb_texture_load_mem and jb_mesh_load_mem.
 * examples/demo/README.md, "How the assets are made",
 * says how to make them again.
 */
#ifndef DEMO_ASSETS_H
#define DEMO_ASSETS_H

#include <jitterbug.h>

/* The textures, in the order they are placed in VRAM. */
enum {
    DM_TEX_SAND,  /* 4-bit 64 x 64: beach and seabed */
    DM_TEX_GRASS, /* 4-bit 64 x 64 */
    DM_TEX_ROCK,  /* 4-bit 64 x 64: cliffs, peaks, standing stones */
    DM_TEX_WATER, /* 4-bit 64 x 64, repeating: ripples, with palette frames */
    DM_TEX_CLOUD, /* 4-bit 256 x 64: a band of cloud, entry 0 clear sky */
    DM_TEX_GLOW,  /* 4-bit 32 x 32: a soft round glow, semi-transparent */
    DM_TEX_TOWER, /* 15-bit 32 x 64: the lighthouse's painted bands */
    DM_TEX_PLANKS, /* 4-bit 64 x 256: the pier's deck */
    DM_TEXTURES
};

typedef struct dm_assets {
    jb_texture tex[DM_TEXTURES];
    /* The water's palettes, 16 entries each, one after another: frame k of
       its palette animation is water + 16 k, k below water_frames (the
       first is the palette it is placed with). In the program's memory. */
    const uint16_t *water;
    int water_frames;
    /* The cloud's palette, 16 entries: entry 0 clear sky, the others its
       densities, which the sky tints band by band. */
    const uint16_t *cloud;
    /* A conifer (lit flat, its vertex colours), a standing stone (rock),
       the lighthouse (its tower textured, the lamp room unlit), and its
       beam: two crossed quads, added, bright at the lamp to black at their
       far end, along -z; and the pier (its deck two large quads of planks
       from z = 0 along -z, at y = 0, posts under it). Their memory is the
       context's (jb_mesh_load_mem), freed with it. */
    jb_mesh tree, stone, tower, beam, pier;
} dm_assets;

/* Places every texture and loads every mesh. JB_OK, or the first failure
   (with jb_error_string's detail); what was placed stays until the context
   is destroyed. */
jb_result dm_assets_load(jb_context *jb, dm_assets *a);

#endif

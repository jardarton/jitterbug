/*
 * The demo's terrain: a heightmap the program makes (deterministic integer
 * value noise), cut into chunks of meshes for jb_draw_mesh.
 *
 * The map is DM_MAP x DM_MAP vertices, DM_CELL world units apart, and it
 * repeats in x and z (the noise is periodic), so a camera can fly over it
 * forever: the scene draws each chunk wherever its copy is nearest. Heights
 * are in world units, sea level 0, land above it and the seabed below.
 *
 * Each chunk (DM_CHUNK x DM_CHUNK cells) has three meshes, in chunk-local
 * coordinates (the scene offsets them to a shared origin near the eye,
 * so that a vertex on a seam between chunks is the same input to the GTE
 * in both, and places them all with one model matrix):
 *   - land: every cell with two or more corners above sea level, its
 *     vertices below it raised to it (so a coast is a flat beach meeting
 *     the water), with a material per cell (sand, grass, rock) chosen by
 *     height and slope;
 *   - seabed: every other cell, shaded darker the deeper it is (a corner
 *     above sea level shows as a shallow under the water; a corner it
 *     shares with land is raised to the sea as the land's is, so the two
 *     meet);
 *   - water: a 4 x 4 grid of quads at sea level over the whole chunk, each
 *     as its two GPU triangles (the scene moves their heights with the
 *     swell as it draws them), for a chunk that has any seabed or beach.
 * A land or seabed cell is the two triangles the GPU would cut its quad
 * into, not the quad: a quad is culled by its first triangle alone
 * (jb_mesh_face), which on a ridge, or seen almost edge on, leaves a hole.
 * The meshes point at the materials the caller gives (dm_terrain_build), so
 * the textures can come from anywhere (assets.c).
 */
#ifndef DEMO_TERRAIN_H
#define DEMO_TERRAIN_H

#include <jitterbug.h>

enum {
    DM_MAP = 128,                     /* vertices a side; the map repeats */
    DM_CELL = 64,                     /* world units between vertices */
    DM_CHUNK = 8,                     /* cells a chunk side */
    DM_CHUNKS = DM_MAP / DM_CHUNK,    /* chunks a side */
    DM_WORLD = DM_MAP * DM_CELL,      /* world units a side, the period */
    DM_CHUNK_SIZE = DM_CHUNK * DM_CELL,
    DM_CV = DM_CHUNK + 1,             /* chunk vertices a side */
    DM_WATER_N = 4,                   /* water quads a chunk side */
    DM_WATER_V = DM_WATER_N + 1
};

/* The land materials (a cell's), in the order of dm_terrain_build's. */
enum { DM_SAND = 0, DM_GRASS = 1, DM_ROCK = 2, DM_LAND_MATERIALS = 3 };

typedef struct dm_chunk {
    /* land */
    jb_gte_vec land_pos[DM_CV * DM_CV], land_normal[DM_CV * DM_CV];
    uint32_t land_color[DM_CV * DM_CV];
    jb_mesh_face land_faces[2 * DM_CHUNK * DM_CHUNK]; /* two triangles a cell */
    jb_mesh land;
    /* The land's faces of material m are land_faces[land_first[m]] up to
       land_first[m + 1]: they are in order of material, so that each can
       be drawn with a fog colour of its own. */
    int land_first[DM_LAND_MATERIALS + 1];
    /* seabed */
    jb_gte_vec bed_pos[DM_CV * DM_CV], bed_normal[DM_CV * DM_CV];
    uint32_t bed_color[DM_CV * DM_CV];
    jb_mesh_face bed_faces[2 * DM_CHUNK * DM_CHUNK];
    jb_mesh bed;
    /* water */
    jb_gte_vec water_pos[DM_WATER_V * DM_WATER_V];
    jb_mesh_face water_faces[2 * DM_WATER_N * DM_WATER_N]; /* two triangles a quad */
    jb_mesh water;
    int has_water;
    int lo, hi; /* the lowest and highest height in the chunk */
} dm_chunk;

typedef struct dm_terrain {
    int16_t height[DM_MAP * DM_MAP]; /* vertex (i, j) at x = i * DM_CELL, z = j * DM_CELL */
    uint8_t material[DM_MAP * DM_MAP]; /* cell (i, j), from vertex (i, j) to (i + 1, j + 1) */
    dm_chunk chunk[DM_CHUNKS * DM_CHUNKS];
} dm_terrain;

/* Makes the heightmap (no allocation; t is large, about 1.3 MB). */
void dm_terrain_generate(dm_terrain *t);

/* Builds the chunks' meshes: land with land_materials[DM_SAND..DM_ROCK],
   the seabed with *bed, the water with *water (one texture, repeating:
   its u, v run 0..255 across a chunk). */
void dm_terrain_build(dm_terrain *t, const jb_material *land_materials, const jb_material *bed,
                      const jb_material *water, const uint8_t *land_uv, const uint8_t *water_uv);

/* The texel coordinates the meshes use, for dm_terrain_build: land_uv for
   a chunk's DM_CV x DM_CV vertices (each cell shows the 64 x 64 texture
   once, mirrored from one cell to the next, so shared vertices agree), and
   water_uv for its DM_WATER_V x DM_WATER_V. */
void dm_terrain_uv(uint8_t *land_uv, uint8_t *water_uv);

/* The height of vertex (i, j), any ints (the map repeats). */
int dm_vertex_height(const dm_terrain *t, int i, int j);

/* The height of the ground (or seabed) at world (x, z), any floats: the
   heightmap bilinearly interpolated. */
float dm_ground(const dm_terrain *t, float x, float z);

/* The height of the surface the meshes draw at world (x, z), any floats:
   the land (raised to the sea at a coast) or the seabed, each cell's two
   triangles split as the GPU splits its quad. */
float dm_surface(const dm_terrain *t, float x, float z);

/* floor(x) for floats in int range, without the C library. */
int dm_floor(float x);

/* x modulo m (m > 0), in 0..m-1, for any int. */
int dm_wrap(int x, int m);

#endif

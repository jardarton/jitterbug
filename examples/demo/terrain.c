/*
 * The demo's terrain (terrain.h). Integer value noise for the heights; the
 * normals through the library's float maths (jb_vec3_normalize, the same
 * bits everywhere).
 */
#include "terrain.h"

#include <string.h>

/* An integer hash (a finaliser of the murmur kind). */
static uint32_t mix(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

int dm_wrap(int x, int m)
{
    int r = x % m;

    return r < 0 ? r + m : r;
}

int dm_floor(float x)
{
    int i = (int)x;

    return (float)i > x ? i - 1 : i;
}

/* A lattice value 0..65535 at (i, j) of a lattice with period cells a side. */
static int lattice(int i, int j, int period, uint32_t seed)
{
    i = dm_wrap(i, period), j = dm_wrap(j, period);
    return (int)(mix((uint32_t)i * 0x9e3779b1u ^ mix((uint32_t)j + seed * 0x85ebca6bu)) & 0xffffu);
}

/* Smoothstep in 16.16: 3w^2 - 2w^3 for w 0..65536. */
static int64_t smooth(int64_t w)
{
    return w * w * (3 * 65536 - 2 * w) >> 32;
}

/* Value noise 0..65535 at map vertex (x, y), lattice points cell vertices
   apart, periodic over the map. */
static int value_noise(int x, int y, int cell, uint32_t seed)
{
    int period = DM_MAP / cell, ix = x / cell, iy = y / cell;
    int64_t sx = smooth((int64_t)(x % cell) * 65536 / cell);
    int64_t sy = smooth((int64_t)(y % cell) * 65536 / cell);
    int64_t a = lattice(ix, iy, period, seed), b = lattice(ix + 1, iy, period, seed);
    int64_t c = lattice(ix, iy + 1, period, seed), d = lattice(ix + 1, iy + 1, period, seed);
    int64_t top = a * 65536 + (b - a) * sx, bottom = c * 65536 + (d - c) * sx;

    return (int)((top * 65536 + (bottom - top) * sy) >> 32);
}

int dm_vertex_height(const dm_terrain *t, int i, int j)
{
    return t->height[dm_wrap(j, DM_MAP) * DM_MAP + dm_wrap(i, DM_MAP)];
}

float dm_ground(const dm_terrain *t, float x, float z)
{
    float fx = x / DM_CELL, fz = z / DM_CELL;
    int i = dm_floor(fx), j = dm_floor(fz);
    float u = fx - (float)i, v = fz - (float)j;
    float a = (float)dm_vertex_height(t, i, j), b = (float)dm_vertex_height(t, i + 1, j);
    float c = (float)dm_vertex_height(t, i, j + 1), d = (float)dm_vertex_height(t, i + 1, j + 1);
    float top = a + (b - a) * u, bottom = c + (d - c) * u;

    return top + (bottom - top) * v;
}

void dm_terrain_generate(dm_terrain *t)
{
    static const int cells[5] = {32, 16, 8, 4, 2}, weight[5] = {16, 8, 4, 2, 1};
    int i, j, k;

    memset(t, 0, sizeof *t);
    for (j = 0; j < DM_MAP; ++j)
        for (i = 0; i < DM_MAP; ++i) {
            int64_t n = 0;
            int e, h;
            for (k = 0; k < 5; ++k)
                n += (int64_t)weight[k] * value_noise(i, j, cells[k], 17u + (uint32_t)k);
            /* 0..65535, then sea below 33000: about half the map. */
            e = (int)(n / 31) - 33000;
            if (e > 0) /* land: gentle near the coast, steeper higher up */
                h = 8 + (int)((int64_t)e * 22 / 1000 + (int64_t)e * e / 2400000);
            else /* seabed: shelving off from the coast */
                h = e / 60 - 4;
            t->height[j * DM_MAP + i] = (int16_t)h;
        }
    /* Each cell's material: sand at the coast, rock where steep or high,
       grass between. */
    for (j = 0; j < DM_MAP; ++j)
        for (i = 0; i < DM_MAP; ++i) {
            int a = dm_vertex_height(t, i, j), b = dm_vertex_height(t, i + 1, j);
            int c = dm_vertex_height(t, i, j + 1), d = dm_vertex_height(t, i + 1, j + 1);
            int lo = a, hi = a, m;
            lo = b < lo ? b : lo, lo = c < lo ? c : lo, lo = d < lo ? d : lo;
            hi = b > hi ? b : hi, hi = c > hi ? c : hi, hi = d > hi ? d : hi;
            m = hi - lo > 60 || lo > 520 ? DM_ROCK : lo < 30 ? DM_SAND : DM_GRASS;
            /* A little noise in where grass gives way to rock. */
            if (m == DM_GRASS && (mix((uint32_t)(j * DM_MAP + i) + 99u) & 15u) == 0)
                m = hi - lo > 36 ? DM_ROCK : m;
            t->material[j * DM_MAP + i] = (uint8_t)m;
        }
}

/* The normal at vertex (i, j), 4.12, from the heights around it. */
static jb_gte_vec normal_at(const dm_terrain *t, int i, int j, int flat)
{
    jb_vec3 n;

    if (flat) {
        n.x = 0, n.y = 1, n.z = 0;
    } else {
        n.x = (float)(dm_vertex_height(t, i - 1, j) - dm_vertex_height(t, i + 1, j));
        n.y = 2.0f * DM_CELL;
        n.z = (float)(dm_vertex_height(t, i, j - 1) - dm_vertex_height(t, i, j + 1));
        n = jb_vec3_normalize(n);
    }
    return (jb_gte_vec){(int16_t)(n.x * 4095.0f), (int16_t)(n.y * 4095.0f), (int16_t)(n.z * 4095.0f)};
}

void dm_terrain_uv(uint8_t *land_uv, uint8_t *water_uv)
{
    int i, j;

    for (j = 0; j < DM_CV; ++j)
        for (i = 0; i < DM_CV; ++i) {
            land_uv[2 * (j * DM_CV + i)] = (uint8_t)(i & 1 ? 63 : 0);
            land_uv[2 * (j * DM_CV + i) + 1] = (uint8_t)(j & 1 ? 63 : 0);
        }
    for (j = 0; j < DM_WATER_V; ++j)
        for (i = 0; i < DM_WATER_V; ++i) {
            int u = i * 256 / DM_WATER_N, v = j * 256 / DM_WATER_N;
            water_uv[2 * (j * DM_WATER_V + i)] = (uint8_t)(u > 255 ? 255 : u);
            water_uv[2 * (j * DM_WATER_V + i) + 1] = (uint8_t)(v > 255 ? 255 : v);
        }
}

/* A colour a little lighter or darker than base, by a hash of the vertex:
   the ground's mottling. */
static uint32_t mottle(int i, int j, int base)
{
    int c = base + (int)(mix((uint32_t)(dm_wrap(j, DM_MAP) * DM_MAP + dm_wrap(i, DM_MAP))) % 21u) - 10;

    return JB_RGB(c, c, c);
}

/* Whether cell (i, j) is land: two or more of its corners above the sea. */
static int land_cell(const dm_terrain *t, int i, int j)
{
    return (dm_vertex_height(t, i, j) > 0) + (dm_vertex_height(t, i + 1, j) > 0) +
               (dm_vertex_height(t, i, j + 1) > 0) + (dm_vertex_height(t, i + 1, j + 1) > 0) >=
           2;
}

/* The seabed's height at vertex (i, j): the heightmap's, but raised to the
   sea where the vertex is a corner of a land cell, as the land raises it,
   so that the two meet along a coast with no slit under the beach. */
static int bed_height(const dm_terrain *t, int i, int j)
{
    int h = dm_vertex_height(t, i, j);

    if (h < 0 && (land_cell(t, i - 1, j - 1) || land_cell(t, i, j - 1) || land_cell(t, i - 1, j) ||
                  land_cell(t, i, j)))
        h = 0;
    return h;
}

float dm_surface(const dm_terrain *t, float x, float z)
{
    float fx = x / DM_CELL, fz = z / DM_CELL;
    int i = dm_floor(fx), j = dm_floor(fz), land = land_cell(t, i, j), h[4], k;
    float u = fx - (float)i, v = fz - (float)j;

    /* Corners (i, j), (i + 1, j), (i, j + 1), (i + 1, j + 1), as the cell's
       mesh has them. */
    for (k = 0; k < 4; ++k) {
        int ci = i + (k & 1), cj = j + (k >> 1);
        h[k] = land ? dm_vertex_height(t, ci, cj) : bed_height(t, ci, cj);
        h[k] = land && h[k] < 0 ? 0 : h[k];
    }
    /* Split along (i, j + 1) - (i + 1, j), as the GPU splits the quad. */
    if (u + v <= 1)
        return (float)h[0] + u * (float)(h[1] - h[0]) + v * (float)(h[2] - h[0]);
    return (float)h[3] + (1 - u) * (float)(h[2] - h[3]) + (1 - v) * (float)(h[1] - h[3]);
}

/* Adds cell (i, j) to faces (*n of them so far) as the two triangles the
   GPU would cut its quad into (v[0] v[1] v[3] and v[1] v[2] v[3]), v the
   corners' indices round it anticlockwise from above. Not as the quad: a
   quad is culled by its first triangle alone (jb_mesh_face), so a bent
   one on a ridge, its first triangle turned away and its second towards
   the eye, leaves a hole, and so, by a pixel, does a flat one seen almost
   edge on, its corners rounded to the screen so that its triangles
   disagree. */
static void add_cell(jb_mesh_face *faces, int *n, const uint16_t v[4], int material)
{
    jb_mesh_face f;

    f.material = (uint16_t)material;
    f.vertices = 3, f.v[3] = 0;
    f.v[0] = v[0], f.v[1] = v[1], f.v[2] = v[3];
    faces[(*n)++] = f;
    f.v[0] = v[1], f.v[1] = v[2], f.v[2] = v[3];
    faces[(*n)++] = f;
}

static void build_chunk(dm_terrain *t, int cx, int cz, const jb_material *land_materials,
                        const jb_material *bed, const jb_material *water, const uint8_t *land_uv,
                        const uint8_t *water_uv)
{
    /* Anticlockwise seen from above (+y), as jb_mesh_face wants: (i, j),
       (i, j + 1), (i + 1, j + 1), (i + 1, j). */
    static const int corner[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
    dm_chunk *ch = &t->chunk[cz * DM_CHUNKS + cx];
    int i0 = cx * DM_CHUNK, j0 = cz * DM_CHUNK, i, j, nland = 0, nbed = 0, k;

    ch->lo = 32767, ch->hi = -32768;
    for (j = 0; j < DM_CV; ++j)
        for (i = 0; i < DM_CV; ++i) {
            int h = dm_vertex_height(t, i0 + i, j0 + j), v = j * DM_CV + i;
            int b = bed_height(t, i0 + i, j0 + j), depth = b < 0 ? -b : 0;
            int shade = 118 - depth * 70 / 160;
            ch->lo = h < ch->lo ? h : ch->lo, ch->hi = h > ch->hi ? h : ch->hi;
            ch->land_pos[v] = (jb_gte_vec){(int16_t)(i * DM_CELL), (int16_t)(h < 0 ? 0 : h),
                                           (int16_t)(j * DM_CELL)};
            ch->land_normal[v] = normal_at(t, i0 + i, j0 + j, h <= 0);
            ch->land_color[v] = mottle(i0 + i, j0 + j, 128);
            ch->bed_pos[v] = (jb_gte_vec){(int16_t)(i * DM_CELL), (int16_t)b, (int16_t)(j * DM_CELL)};
            ch->bed_normal[v] = normal_at(t, i0 + i, j0 + j, 0);
            shade = shade < 40 ? 40 : shade;
            ch->bed_color[v] = JB_RGB(shade * 3 / 4, shade * 7 / 8, shade);
        }
    for (j = 0; j < DM_CHUNK; ++j)
        for (i = 0; i < DM_CHUNK; ++i) {
            uint16_t v[4];
            for (k = 0; k < 4; ++k)
                v[k] = (uint16_t)((j + corner[k][1]) * DM_CV + i + corner[k][0]);
            if (land_cell(t, i0 + i, j0 + j))
                add_cell(ch->land_faces, &nland, v, t->material[(j0 + j) * DM_MAP + i0 + i]);
            else
                add_cell(ch->bed_faces, &nbed, v, 0);
        }
    /* The land's faces in order of material, where land_first says. */
    {
        jb_mesh_face sorted[2 * DM_CHUNK * DM_CHUNK];
        int m, n = 0;
        for (m = 0; m < DM_LAND_MATERIALS; ++m) {
            ch->land_first[m] = n;
            for (k = 0; k < nland; ++k)
                if (ch->land_faces[k].material == m)
                    sorted[n++] = ch->land_faces[k];
        }
        ch->land_first[DM_LAND_MATERIALS] = n;
        memcpy(ch->land_faces, sorted, sizeof sorted[0] * (size_t)n);
    }
    ch->land = (jb_mesh){DM_CV * DM_CV, ch->land_pos, ch->land_normal, ch->land_color, land_uv,
                         nland, ch->land_faces, DM_LAND_MATERIALS, land_materials};
    ch->bed = (jb_mesh){DM_CV * DM_CV, ch->bed_pos, ch->bed_normal, ch->bed_color, land_uv, nbed,
                        ch->bed_faces, 1, bed};
    /* The water, wherever the chunk reaches down to the sea. */
    for (j = 0; j < DM_WATER_V; ++j)
        for (i = 0; i < DM_WATER_V; ++i)
            ch->water_pos[j * DM_WATER_V + i] =
                (jb_gte_vec){(int16_t)(i * DM_CHUNK_SIZE / DM_WATER_N), 0,
                             (int16_t)(j * DM_CHUNK_SIZE / DM_WATER_N)};
    /* Each quad as its two GPU triangles: the swell bends them (the scene
       sets the heights as it draws), and a bent quad is culled by its
       first triangle alone. */
    k = 0;
    for (j = 0; j < DM_WATER_N; ++j)
        for (i = 0; i < DM_WATER_N; ++i) {
            uint16_t v[4];
            int c;
            for (c = 0; c < 4; ++c)
                v[c] = (uint16_t)((j + corner[c][1]) * DM_WATER_V + i + corner[c][0]);
            add_cell(ch->water_faces, &k, v, 0);
        }
    ch->has_water = ch->lo <= 0;
    ch->water = (jb_mesh){DM_WATER_V * DM_WATER_V, ch->water_pos, NULL, NULL, water_uv,
                          k, ch->water_faces, 1, water};
}

void dm_terrain_build(dm_terrain *t, const jb_material *land_materials, const jb_material *bed,
                      const jb_material *water, const uint8_t *land_uv, const uint8_t *water_uv)
{
    int cx, cz;

    for (cz = 0; cz < DM_CHUNKS; ++cz)
        for (cx = 0; cx < DM_CHUNKS; ++cx)
            build_chunk(t, cx, cz, land_materials, bed, water, land_uv, water_uv);
}

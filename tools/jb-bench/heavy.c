/*
 * jb-bench's own workloads (workload.h): heavy and fill. Original content,
 * made procedurally here; C99, the public API only, integer arithmetic only.
 *
 * heavy: a flight over hilly terrain, drawn as a console program would:
 * the camera's rotation in the GTE, every 3D point projected with RTPT or
 * RTPS, faces culled with NCLIP and sorted by AVSZ3/AVSZ4 into an ordering
 * table, objects lit with NCCT. Per frame, about:
 *   - terrain: Gouraud-textured triangles, two a cell, 4-bit (grass, sand,
 *     earth, each with its CLUT) or 8-bit (rock), shaded by slope and
 *     darkened with distance;
 *   - water: semi-transparent (B/2 + F/2) 4-bit quads over low ground,
 *     their texture scrolling and their CLUT rewritten each frame;
 *   - blocks standing on the terrain: 8-bit sides and a 15-bit top, lit by
 *     two coloured lights (NCCT/NCCS);
 *   - a sky (a Gouraud quad) and a 15-bit mountain backdrop;
 *   - sprites: glowing motes (additive, 4-bit, 8 x 8 or 16 x 16);
 *   - lines: falling streaks, Gouraud, half of them additive;
 *   - a HUD: two half-blended bars, 8 x 8 glyph sprites, a minimap of
 *     4 x 4 rectangles with a polyline border, a crosshair.
 * The numbers it really draws are measured (jb-bench --stats) and given in
 * docs/performance.md.
 *
 * fill: four quads a frame, each covering the whole display: textured
 * (8-bit, 8-bit, 4-bit, 15-bit), Gouraud-shaded and semi-transparent (B - F,
 * B/2 + F/2, B + F/4, B + F), tilted planes projected by the GTE so the
 * options (precise vertices, perspective-correct) apply to them too.
 *
 * VRAM (1024 x 512), laid out by hand:
 *   (0, 0), (0, 240)  the display buffers, 320 x 240
 *   page (320, 0)     4-bit, 64 x 144 pixels: tiles at u 0 grass, 64 sand,
 *                     128 earth, 192 water (v 0-127, period 64); sprites at
 *                     v 128 (u 0 mote 16 x 16, u 16 mote 8 x 8, u 32 glyphs)
 *   page (384, 0)     8-bit, 64 x 64: u 0 rock, u 64 stone blocks
 *   page (448, 0)     15-bit, 192 x 64: u 0 metal, u 64 mountains (128 x 48)
 *   row 480           4-bit CLUTs at x 0 grass, 16 sand, 32 earth, 48 water,
 *                     64 mote, 80 glyph
 *   rows 481, 482     8-bit CLUTs: rock, stone
 */
#include <stdlib.h>
#include <string.h>

#include "workload.h"

/* ------------------------------------------------------------------------ */
/* Integer maths. 4096 is 1.0; angles are 0..1023 for a whole turn. */

enum { ONE = 4096 };

/* sin(a) * 4096, by Bhaskara I's rational approximation (within 0.2%). */
static int isin(int a)
{
    long long t;
    int h, s;

    a &= 1023;
    h = a & 511;
    t = (long long)h * (512 - h);
    s = (int)(16LL * ONE * t / (5LL * 512 * 512 - 4 * t));
    return a & 512 ? -s : s;
}

static int icos(int a)
{
    return isin(a + 256);
}

static int fmul(int a, int b)
{
    return a * b / ONE;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Floor division and modulo for a positive divisor. */
static int floordiv(int a, int b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

static uint32_t hash3(int x, int y, int z)
{
    uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ (uint32_t)z * 0xcb1ab31fu;

    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return h;
}

/* Smooth value noise over a lattice 2^shift cells apart: 0..1023. */
static int noise(int i, int j, int shift, int seed)
{
    int n = 1 << shift, gi = floordiv(i, n), gj = floordiv(j, n);
    int fi = (i - gi * n) * 256 / n, fj = (j - gj * n) * 256 / n;
    int wi = fi * fi * (768 - 2 * fi) / 65536, wj = fj * fj * (768 - 2 * fj) / 65536;
    int a = (int)(hash3(gi, gj, seed) & 1023), b = (int)(hash3(gi + 1, gj, seed) & 1023);
    int c = (int)(hash3(gi, gj + 1, seed) & 1023), d = (int)(hash3(gi + 1, gj + 1, seed) & 1023);
    int top = a + (b - a) * wi / 256, bottom = c + (d - c) * wi / 256;

    return top + (bottom - top) * wj / 256;
}

/* ------------------------------------------------------------------------ */
/* VRAM layout */

enum {
    PAGE4_X = 320,
    PAGE8_X = 384,
    PAGE15_X = 448,
    CLUT_Y = 480,
    T_GRASS = 0, /* tiles in the 4-bit page, by u */
    T_SAND = 64,
    T_EARTH = 128,
    T_WATER = 192,
    SPRITE_V = 128,
    GLYPH_U = 32,
    ROCK_U = 0, /* in the 8-bit page */
    STONE_U = 64,
    METAL_U = 0, /* in the 15-bit page */
    MOUNT_U = 64,
    MOUNT_W = 128,
    MOUNT_H = 48,
    SEMI = 0x8000
};

static uint16_t rgb15(int r, int g, int b)
{
    return (uint16_t)(clampi(r, 0, 31) | clampi(g, 0, 31) << 5 | clampi(b, 0, 31) << 10);
}

/* The 4-bit page: 256 x 144 texels. */
static int texel4(int u, int v)
{
    uint32_t h = hash3(u, v, 11);

    if (v >= SPRITE_V) {
        int sv = v - SPRITE_V;

        if (u < 24) { /* the motes: 16 x 16 at u 0, 8 x 8 at u 16 */
            int size = u < 16 ? 16 : 8, su = u < 16 ? u : u - 16;
            int dx = 2 * su - (size - 1), dy = 2 * sv - (size - 1);
            int d2 = dx * dx + dy * dy, r2 = size * size;

            return sv < size && d2 < r2 ? 15 - d2 * 14 / r2 : 0;
        }
        if (u >= GLYPH_U && u < GLYPH_U + 16 * 8 && sv < 8) { /* 16 blocky glyphs */
            int g = (u - GLYPH_U) / 8, gx = (u - GLYPH_U) % 8;

            if (gx == 0 || gx == 7 || sv == 0 || sv == 7)
                return 0;
            return (int)(hash3(g, sv, 5) >> (gx * 3) & 1);
        }
        return 0;
    }
    v &= 63;
    if (u < T_SAND) /* grass: speckles and blades */
        return h % 11 == 0 ? 5 : 1 + (int)(h & 3);
    if (u < T_EARTH) /* sand: ripples */
        return 1 + (((u + v / 2) / 6) & 1) * 2 + (int)(h & 1);
    if (u < T_WATER) /* earth with pebbles */
        return h % 17 == 0 ? 6 + (int)(h >> 8 & 1) : 1 + (int)(h & 3);
    /* water: two crossing waves */
    return 1 + (isin(u * 16 + v * 8) + isin(v * 24 - u * 8) + 2 * ONE) * 13 / (4 * ONE);
}

static jb_result make_page4(jb_context *jb)
{
    enum { W = 256, H = SPRITE_V + 16 };
    static uint16_t px[W / 4 * H];
    int u, v, k;

    for (v = 0; v < H; ++v)
        for (u = 0; u < W; u += 4) {
            uint16_t p = 0;

            for (k = 0; k < 4; ++k)
                p |= (uint16_t)(texel4(u + k, v) << (4 * k));
            px[v * (W / 4) + u / 4] = p;
        }
    return jb_vram_load(jb, PAGE4_X, 0, W / 4, H, px);
}

/* The 8-bit page: rock (u 0) and stone blocks (u 64), 64 x 64 each. */
static jb_result make_page8(jb_context *jb)
{
    enum { W = 128, H = 64 };
    static uint16_t px[W / 2 * H];
    int u, v;

    for (v = 0; v < H; ++v)
        for (u = 0; u < W; u += 2) {
            int idx[2], k;

            for (k = 0; k < 2; ++k) {
                int x = u + k;

                if (x < STONE_U) {
                    idx[k] = (int)(noise(x, v, 3, 21) * 160 / 1024 + (hash3(x, v, 22) & 63));
                } else {
                    int row = v / 16, bx = (x - STONE_U + (row & 1) * 16) & 31, by = v & 15;
                    int mortar = bx == 0 || by == 0;
                    idx[k] = mortar ? 8 : 64 + (int)(hash3((x - STONE_U + (row & 1) * 16) / 32, row, 23) & 63) +
                                              (int)(hash3(x, v, 24) & 31);
                }
            }
            px[v * (W / 2) + u / 2] = (uint16_t)(idx[0] | idx[1] << 8);
        }
    return jb_vram_load(jb, PAGE8_X, 0, W / 2, H, px);
}

/* The 15-bit page: metal plates (u 0, 64 x 64) and the mountain backdrop
   (u 64, 128 x 48; transparent, 0x0000, above the ridge). */
static jb_result make_page15(jb_context *jb)
{
    enum { W = 192, H = 64 };
    static uint16_t px[W * H];
    int u, v;

    for (v = 0; v < H; ++v)
        for (u = 0; u < W; ++u) {
            uint16_t p = 0;

            if (u < MOUNT_U) {
                int edge = (u & 31) == 0 || (v & 31) == 0, rivet = (u & 31) == 3 && (v & 31) == 3;
                int c = 14 + (u + v) / 16 - (int)(hash3(u, v, 31) & 1);
                p = edge ? rgb15(6, 7, 8) : rivet ? rgb15(28, 28, 26) : rgb15(c, c + 1, c + 3);
            } else if (v < MOUNT_H) {
                int x = u - MOUNT_U;
                int ridge = 10 + noise(x, 0, 4, 41) * 22 / 1024 + noise(x, 0, 1, 42) * 6 / 1024;
                if (v >= ridge) {
                    int c = 6 + (v - ridge) / 4;
                    p = rgb15(c, c + 2, c + 6);
                }
            }
            px[v * W + u] = p;
        }
    return jb_vram_load(jb, PAGE15_X, 0, W, H, px);
}

/* The water's CLUT at a frame: blue shades turning, all semi-transparent. */
static void water_clut(unsigned frame, uint16_t clut[16])
{
    int i;

    clut[0] = SEMI | rgb15(2, 6, 12);
    for (i = 1; i < 16; ++i) {
        int s = ((i + (int)(frame / 2)) % 15);
        clut[i] = (uint16_t)(SEMI | rgb15(3 + s / 3, 9 + s / 2, 16 + s));
    }
}

static jb_result make_cluts(jb_context *jb)
{
    uint16_t c4[6][16], c8[256];
    jb_result r;
    int i, k;

    for (i = 0; i < 16; ++i) {
        c4[0][i] = rgb15(5 + i % 4, 12 + i % 5 + (i == 5) * 6, 4 + i % 3);  /* grass */
        c4[1][i] = rgb15(22 + i % 3, 20 + i % 3, 13 + i % 2);               /* sand */
        c4[2][i] = rgb15(12 + i % 4 + (i >= 6) * 8, 9 + i % 3 + (i >= 6) * 8, 6 + (i >= 6) * 8); /* earth */
        c4[4][i] = i == 0 ? 0 : (uint16_t)(SEMI | rgb15(31 * i / 15, 26 * i / 15, 10 * i / 15)); /* mote */
        c4[5][i] = i == 0 ? 0 : rgb15(30, 31, 26);                          /* glyph */
    }
    c4[0][0] = c4[1][0] = c4[2][0] = 0x8000; /* opaque black: 0 is transparent */
    water_clut(0, c4[3]);
    for (k = 0; k < 6; ++k)
        if ((r = jb_vram_load(jb, 16 * k, CLUT_Y, 16, 1, c4[k])) != JB_OK)
            return r;
    for (i = 0; i < 256; ++i) /* rock: grey-brown */
        c8[i] = i == 0 ? 0x8000 : rgb15(8 + i / 14, 7 + i / 14, 6 + i / 16);
    if ((r = jb_vram_load(jb, 0, CLUT_Y + 1, 256, 1, c8)) != JB_OK)
        return r;
    for (i = 0; i < 256; ++i) /* stone: mortar dark, blocks warm */
        c8[i] = i < 64 ? rgb15(5, 5, 5) : rgb15(10 + (i - 64) / 8, 9 + (i - 64) / 9, 7 + (i - 64) / 12);
    return jb_vram_load(jb, 0, CLUT_Y + 2, 256, 1, c8);
}

/* ------------------------------------------------------------------------ */
/* The context and the textures, shared by both workloads */

enum { OT_LEN = 1024, PRIMS = 4096, LINE_MAX = 6 };

typedef struct {
    int next;
    enum { P_POLY, P_RECT, P_LINE, P_POLYLINE } kind;
    union {
        jb_poly poly;
        jb_rect rect;
        jb_line line;
        struct {
            int count;
            jb_vertex v[LINE_MAX];
        } pl;
    } u;
} prim;

typedef struct terrain terrain; /* heavy's, below */

typedef struct {
    workload base;
    terrain *terrain; /* heavy only */
    int buffer, shown;
    jb_result error;
    wl_stats *stats;
    jb_texture grass, sand, earth, water, mote, glyph, rock, stone, metal, mount;
    int ot[OT_LEN];
    prim *prims;
    int nprims;
} scene;

static void note(scene *s, jb_result r)
{
    if (r != JB_OK && s->error == JB_OK)
        s->error = r;
}

static void tex(jb_texture *t, int page_x, int depth, int clut_x, int clut_y)
{
    t->page_x = page_x;
    t->page_y = 0;
    t->depth = depth;
    t->clut_x = clut_x;
    t->clut_y = clut_y;
}

static void scene_destroy(workload *w)
{
    scene *s = (scene *)w;

    jb_destroy(s->base.jb);
    free(s->terrain);
    free(s->prims);
    free(s);
}

static scene *scene_create(const wl_options *o, jb_result *result)
{
    scene *s = calloc(1, sizeof *s);
    jb_config c;
    jb_result r;

    if (s == NULL || (s->prims = malloc(PRIMS * sizeof *s->prims)) == NULL) {
        free(s);
        *result = JB_ERROR_OUT_OF_MEMORY;
        return NULL;
    }
    memset(&c, 0, sizeof c);
    c.scale = o->scale;
    c.width = WL_WIDTH;
    c.height = WL_HEIGHT;
    c.threads = o->threads;
    c.precise_vertices = o->precise;
    c.perspective_correct = o->persp;
    c.log = o->log;
    c.log_user = o->log_user;
    if ((s->base.jb = jb_create(&c, &r)) == NULL) {
        free(s->prims);
        free(s);
        *result = r;
        return NULL;
    }
    s->base.destroy = scene_destroy;
    tex(&s->grass, PAGE4_X, JB_TEX_4BIT, 0, CLUT_Y);
    tex(&s->sand, PAGE4_X, JB_TEX_4BIT, 16, CLUT_Y);
    tex(&s->earth, PAGE4_X, JB_TEX_4BIT, 32, CLUT_Y);
    tex(&s->water, PAGE4_X, JB_TEX_4BIT, 48, CLUT_Y);
    tex(&s->mote, PAGE4_X, JB_TEX_4BIT, 64, CLUT_Y);
    tex(&s->glyph, PAGE4_X, JB_TEX_4BIT, 80, CLUT_Y);
    tex(&s->rock, PAGE8_X, JB_TEX_8BIT, 0, CLUT_Y + 1);
    tex(&s->stone, PAGE8_X, JB_TEX_8BIT, 0, CLUT_Y + 2);
    tex(&s->metal, PAGE15_X, JB_TEX_15BIT, 0, 0);
    tex(&s->mount, PAGE15_X, JB_TEX_15BIT, 0, 0);
    if ((r = make_page4(s->base.jb)) != JB_OK || (r = make_page8(s->base.jb)) != JB_OK ||
        (r = make_page15(s->base.jb)) != JB_OK || (r = make_cluts(s->base.jb)) != JB_OK ||
        (r = jb_gp1(s->base.jb, 0x08000001u)) != JB_OK) { /* 320 x 240 */
        scene_destroy(&s->base);
        *result = r;
        return NULL;
    }
    *result = JB_OK;
    return s;
}

/* The frame's start: projections forgotten, the back buffer's area and
   offset, dithering, the texture window, and a clear. */
static int begin_frame(scene *s, int window, uint32_t clear)
{
    int top = s->buffer * WL_HEIGHT;

    s->error = JB_OK;
    note(s, jb_gte_begin_frame(s->base.jb));
    note(s, jb_set_draw_area(s->base.jb, 0, top, WL_WIDTH - 1, top + WL_HEIGHT - 1));
    note(s, jb_set_draw_offset(s->base.jb, 0, top));
    note(s, jb_set_dither(s->base.jb, 1));
    note(s, window ? jb_set_texture_window(s->base.jb, 0xc0 / 8, 0xc0 / 8, 0, 0)
                   : jb_set_texture_window(s->base.jb, 0, 0, 0, 0));
    note(s, jb_vram_fill(s->base.jb, 0, top, WL_WIDTH, WL_HEIGHT, clear));
    return top;
}

/* Shows the buffer just drawn (GP1(05h)), enabling the display at first. */
static void end_frame(scene *s, int top)
{
    note(s, jb_gp1(s->base.jb, 0x05000000u | (uint32_t)top << 10));
    if (!s->shown) {
        note(s, jb_gp1(s->base.jb, 0x03000000u));
        s->shown = 1;
    }
    s->buffer ^= 1;
}

/* Primitives, sent at once (send_*) or through the ordering table. */

static void send_poly(scene *s, const jb_poly *p)
{
    if (s->stats != NULL)
        wl_count_poly(s->stats, p);
    note(s, jb_prim_poly(s->base.jb, p));
}

static void send_rect(scene *s, const jb_rect *r)
{
    if (s->stats != NULL)
        wl_count_rect(s->stats, r);
    note(s, jb_prim_rect(s->base.jb, r));
}

static void ot_clear(scene *s)
{
    int i;

    for (i = 0; i < OT_LEN; ++i)
        s->ot[i] = -1;
    s->nprims = 0;
}

/* A new zeroed primitive at the head of entry z (drawn first of those
   already there, as the console's addPrim), or NULL when full. */
static prim *ot_add(scene *s, int z, int kind)
{
    prim *p;

    if (s->nprims == PRIMS) {
        note(s, JB_ERROR_OUT_OF_MEMORY);
        return NULL;
    }
    p = &s->prims[s->nprims];
    memset(p, 0, sizeof *p);
    p->kind = kind;
    p->next = s->ot[z];
    s->ot[z] = s->nprims++;
    return p;
}

static int ot_entry(int otz)
{
    return clampi(otz >> 2, 1, OT_LEN - 1);
}

static void ot_draw(scene *s)
{
    int z, i, k;

    for (z = OT_LEN - 1; z >= 0; --z)
        for (i = s->ot[z]; i >= 0; i = s->prims[i].next) {
            const prim *p = &s->prims[i];
            jb_polyline pl;

            switch (p->kind) {
            case P_POLY: send_poly(s, &p->u.poly); break;
            case P_RECT: send_rect(s, &p->u.rect); break;
            case P_LINE:
                if (s->stats != NULL)
                    wl_count_line(s->stats, p->u.line.v[0].x, p->u.line.v[0].y, p->u.line.v[1].x,
                                  p->u.line.v[1].y, p->u.line.blend);
                note(s, jb_prim_line(s->base.jb, &p->u.line));
                break;
            case P_POLYLINE:
                if (s->stats != NULL)
                    for (k = 0; k + 1 < p->u.pl.count; ++k)
                        wl_count_line(s->stats, p->u.pl.v[k].x, p->u.pl.v[k].y,
                                      p->u.pl.v[k + 1].x, p->u.pl.v[k + 1].y, JB_BLEND_NONE);
                pl.gouraud = 1;
                pl.blend = JB_BLEND_NONE;
                pl.count = (size_t)p->u.pl.count;
                pl.v = p->u.pl.v;
                note(s, jb_prim_polyline(s->base.jb, &pl));
                break;
            }
        }
}

/* ------------------------------------------------------------------------ */
/* The GTE */

typedef struct {
    int m[3][3];
} matrix;

static void gte_set(scene *s, int reg, int value)
{
    note(s, jb_gte_write(s->base.jb, reg, (uint32_t)value));
}

static int gte_get(scene *s, int reg)
{
    return (int)jb_gte_read(s->base.jb, reg);
}

static void gte_run(scene *s, uint32_t command)
{
    note(s, jb_gte_command(s->base.jb, command));
}

static void gte_matrix(scene *s, int first, const matrix *a)
{
    note(s, jb_gte_write(s->base.jb, first, JB_GTE_PAIR(a->m[0][0], a->m[0][1])));
    note(s, jb_gte_write(s->base.jb, first + 1, JB_GTE_PAIR(a->m[0][2], a->m[1][0])));
    note(s, jb_gte_write(s->base.jb, first + 2, JB_GTE_PAIR(a->m[1][1], a->m[1][2])));
    note(s, jb_gte_write(s->base.jb, first + 3, JB_GTE_PAIR(a->m[2][0], a->m[2][1])));
    gte_set(s, first + 4, a->m[2][2]);
}

static void gte_translation(scene *s, int x, int y, int z)
{
    gte_set(s, JB_GTE_TRX, x);
    gte_set(s, JB_GTE_TRY, y);
    gte_set(s, JB_GTE_TRZ, z);
}

typedef struct {
    int x, y, z;
} vec;

static void gte_vector(scene *s, int k, const vec *v)
{
    note(s, jb_gte_write(s->base.jb, JB_GTE_VXY0 + 2 * k, JB_GTE_PAIR(v->x, v->y)));
    gte_set(s, JB_GTE_VZ0 + 2 * k, v->z);
}

/* Projects n points: RTPT three at a time, RTPS for the rest. */
static void project(scene *s, const vec *v, int n, uint32_t *sxy, int *sz)
{
    int i = 0, k;

    for (; i + 3 <= n; i += 3) {
        for (k = 0; k < 3; ++k)
            gte_vector(s, k, &v[i + k]);
        gte_run(s, JB_GTE_RTPT);
        for (k = 0; k < 3; ++k) {
            sxy[i + k] = (uint32_t)gte_get(s, JB_GTE_SXY0 + k);
            sz[i + k] = gte_get(s, JB_GTE_SZ1 + k);
        }
    }
    for (; i < n; ++i) {
        gte_vector(s, 0, &v[i]);
        gte_run(s, JB_GTE_RTPS);
        sxy[i] = (uint32_t)gte_get(s, JB_GTE_SXY2);
        sz[i] = gte_get(s, JB_GTE_SZ3);
    }
}

/* Lights n normals (NCCT, NCCS) into JB_RGB colours. */
static void light(scene *s, const vec *normal, int n, uint32_t *rgb)
{
    int i = 0, k;

    for (; i + 3 <= n; i += 3) {
        for (k = 0; k < 3; ++k)
            gte_vector(s, k, &normal[i + k]);
        gte_run(s, JB_GTE_NCCT);
        for (k = 0; k < 3; ++k)
            rgb[i + k] = (uint32_t)gte_get(s, JB_GTE_RGB0 + k) & 0xffffffu;
    }
    for (; i < n; ++i) {
        gte_vector(s, 0, &normal[i]);
        gte_run(s, JB_GTE_NCCS);
        rgb[i] = (uint32_t)gte_get(s, JB_GTE_RGB2) & 0xffffffu;
    }
}

static int nclip(scene *s, uint32_t a, uint32_t b, uint32_t c)
{
    gte_set(s, JB_GTE_SXY0, (int)a);
    gte_set(s, JB_GTE_SXY1, (int)b);
    gte_set(s, JB_GTE_SXY2, (int)c);
    gte_run(s, JB_GTE_NCLIP);
    return gte_get(s, JB_GTE_MAC0);
}

static int avsz3(scene *s, int a, int b, int c)
{
    gte_set(s, JB_GTE_SZ1, a);
    gte_set(s, JB_GTE_SZ2, b);
    gte_set(s, JB_GTE_SZ3, c);
    gte_run(s, JB_GTE_AVSZ3);
    return gte_get(s, JB_GTE_OTZ);
}

static int avsz4(scene *s, int a, int b, int c, int d)
{
    gte_set(s, JB_GTE_SZ0, a);
    gte_set(s, JB_GTE_SZ1, b);
    gte_set(s, JB_GTE_SZ2, c);
    gte_set(s, JB_GTE_SZ3, d);
    gte_run(s, JB_GTE_AVSZ4);
    return gte_get(s, JB_GTE_OTZ);
}

static void gte_setup(scene *s)
{
    /* Light directions (world space, y down: from above), a row each, and
       their colours, a column each: a warm sun and a cool sky fill. */
    static const matrix llm = {{{-1600, -3600, 1200}, {2400, -2000, -2600}, {0, 0, 0}}};
    static const matrix lcm = {{{4600, 1200, 0}, {4200, 1800, 0}, {3200, 2800, 0}}};

    gte_set(s, JB_GTE_OFX, WL_WIDTH / 2 * 65536);
    gte_set(s, JB_GTE_OFY, WL_HEIGHT / 2 * 65536);
    gte_set(s, JB_GTE_H, 256);
    gte_set(s, JB_GTE_ZSF3, ONE / 3);
    gte_set(s, JB_GTE_ZSF4, ONE / 4);
    gte_matrix(s, JB_GTE_L11L12, &llm);
    gte_matrix(s, JB_GTE_LR1LR2, &lcm);
    gte_set(s, JB_GTE_RBK, 600);
    gte_set(s, JB_GTE_GBK, 640);
    gte_set(s, JB_GTE_BBK, 760);
    gte_set(s, JB_GTE_RGBC, (int)JB_RGB(128, 128, 128));
}

static jb_vertex at(uint32_t sxy, uint32_t color, int u, int v)
{
    jb_vertex r;

    r.x = JB_GTE_SX(sxy);
    r.y = JB_GTE_SY(sxy);
    r.color = color;
    r.u = u;
    r.v = v;
    return r;
}

/* Whether a screen box misses the display. */
static int off_screen(int x0, int y0, int x1, int y1)
{
    return x1 < 0 || x0 >= WL_WIDTH || y1 < 0 || y0 >= WL_HEIGHT;
}

static int sxy_off_screen(const uint32_t *sxy, int n)
{
    int x0 = 4096, y0 = 4096, x1 = -4096, y1 = -4096, i;

    for (i = 0; i < n; ++i) {
        int x = JB_GTE_SX(sxy[i]), y = JB_GTE_SY(sxy[i]);
        x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1;
        y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
    }
    return off_screen(x0, y0, x1, y1);
}

static uint32_t grey(int c)
{
    c = clampi(c, 0, 255);
    return JB_RGB(c, c, c);
}

/* ------------------------------------------------------------------------ */
/* heavy */

enum {
    CELL = 128,     /* terrain cell, world units */
    GW = 20,        /* cells either side of the camera */
    GD = 32,        /* cells ahead */
    VW = 2 * GW + 1, /* vertices across, and deep */
    VD = GD + 1,
    WATER = 210,    /* water level (heights are 0..~700) */
    CAM_ALT = 900,  /* camera height */
    NEAR = 64,      /* nearest depth drawn */
    FAR = GD * CELL,
    MOTES = 288,
    STREAKS = 144
};

static int height(int i, int j)
{
    return noise(i, j, 3, 1) * 560 / 1024 + noise(i, j, 1, 2) * 140 / 1024;
}

struct terrain {
    int cam_x, cam_z, ci, cj;
    uint32_t sxy[VD][VW];
    int sz[VD][VW];
    uint32_t rgb[VD][VW];
    int h[VD + 2][VW + 2]; /* heights, one beyond each side */
};

/* The camera at a frame: position, and its rotation in the GTE. */
static void camera(scene *s, unsigned frame, terrain *t)
{
    int yaw = 24 * isin((int)frame * 2) / ONE, pitch = 56;
    int cy = icos(yaw), sy = isin(yaw), cp = icos(pitch), sp = isin(pitch);
    matrix r;

    t->cam_z = (int)(frame * 20u);
    t->cam_x = 600 * isin((int)frame * 3) / ONE;
    t->ci = floordiv(t->cam_x, CELL);
    t->cj = floordiv(t->cam_z, CELL);
    r.m[0][0] = cy, r.m[0][1] = 0, r.m[0][2] = -sy;
    r.m[1][0] = -fmul(sp, sy), r.m[1][1] = cp, r.m[1][2] = -fmul(sp, cy);
    r.m[2][0] = fmul(cp, sy), r.m[2][1] = sp, r.m[2][2] = fmul(cp, cy);
    gte_matrix(s, JB_GTE_RT11RT12, &r);
    gte_translation(s, 0, 0, 0);
}

/* A world point relative to the camera (what the GTE takes as V). */
static vec rel(const terrain *t, int x, int y, int z)
{
    vec v;

    v.x = x - t->cam_x;
    v.y = y + CAM_ALT; /* the camera is at world y -CAM_ALT (y is down) */
    v.z = z - t->cam_z;
    return v;
}

static void add_terrain(scene *s, terrain *t)
{
    vec row[VW];
    int a, b, i, j;

    for (b = 0; b < VD + 2; ++b)
        for (a = 0; a < VW + 2; ++a)
            t->h[b][a] = height(t->ci - GW + a - 1, t->cj + b - 1);
    for (b = 0; b < VD; ++b) {
        for (a = 0; a < VW; ++a)
            row[a] = rel(t, (t->ci - GW + a) * CELL, -t->h[b + 1][a + 1], (t->cj + b) * CELL);
        project(s, row, VW, t->sxy[b], t->sz[b]);
        for (a = 0; a < VW; ++a) {
            /* Slope light (from the far left) and distance fog. */
            int dx = t->h[b + 1][a + 2] - t->h[b + 1][a], dz = t->h[b + 2][a + 1] - t->h[b][a + 1];
            int l = clampi(124 + (dz * 2 - dx) * 40 / CELL, 40, 190);
            int fog = clampi(t->sz[b][a] * 112 / FAR, 0, 112);
            t->rgb[b][a] = grey(l * (256 - fog) / 256);
        }
    }
    for (j = 0; j < GD; ++j)
        for (i = 0; i < VW - 1; ++i) {
            /* Corners: a near left, b near right, c far left, d far right. */
            static const int ci4[4] = {0, 1, 0, 1}, cj4[4] = {0, 0, 1, 1};
            int k, tri, avg = 0, low = 1 << 30, u0;
            const jb_texture *tx;
            uint32_t sxy[4];
            int sz[4];

            for (k = 0; k < 4; ++k) {
                int hh = t->h[j + cj4[k] + 1][i + ci4[k] + 1];
                avg += hh;
                low = hh < low ? hh : low;
                sxy[k] = t->sxy[j + cj4[k]][i + ci4[k]];
                sz[k] = t->sz[j + cj4[k]][i + ci4[k]];
            }
            avg /= 4;
            if (avg < WATER + 30)
                tx = &s->sand, u0 = T_SAND;
            else if (avg > 470)
                tx = &s->rock, u0 = ROCK_U;
            else if (hash3(t->ci - GW + i, t->cj + j, 3) % 7 == 0)
                tx = &s->earth, u0 = T_EARTH;
            else
                tx = &s->grass, u0 = T_GRASS;
            for (tri = 0; tri < 2; ++tri) {
                /* a b c, then b d c: both wind the same way. */
                static const int order[2][3] = {{0, 1, 2}, {1, 3, 2}};
                static const int uv[4][2] = {{0, 63}, {63, 63}, {0, 0}, {63, 0}};
                uint32_t q[3];
                int z[3], c[3], skip = 0;
                prim *p;

                for (k = 0; k < 3; ++k) {
                    int corner = order[tri][k];
                    q[k] = sxy[corner];
                    z[k] = sz[corner];
                    c[k] = corner;
                    skip |= z[k] < NEAR;
                }
                if (skip || sxy_off_screen(q, 3) || nclip(s, q[0], q[1], q[2]) >= 0)
                    continue;
                p = ot_add(s, ot_entry(avsz3(s, z[0], z[1], z[2])), P_POLY);
                if (p == NULL)
                    return;
                p->u.poly.vertices = 3;
                p->u.poly.gouraud = 1;
                p->u.poly.texture = tx;
                for (k = 0; k < 3; ++k)
                    p->u.poly.v[k] = at(q[k], t->rgb[j + cj4[c[k]]][i + ci4[c[k]]],
                                        u0 + uv[c[k]][0], uv[c[k]][1]);
            }
            if (low < WATER) {
                /* Water over this cell: a semi-transparent quad at the
                   water level, its texture scrolling. */
                vec w[4];
                uint32_t wsxy[4];
                int wsz[4], v0 = (int)((unsigned)t->cam_z / 8u % 64u), near = 0;
                prim *p;

                for (k = 0; k < 4; ++k)
                    w[k] = rel(t, (t->ci - GW + i + ci4[k]) * CELL, -WATER, (t->cj + j + cj4[k]) * CELL);
                project(s, w, 4, wsxy, wsz);
                for (k = 0; k < 4; ++k)
                    near |= wsz[k] < NEAR;
                if (near || sxy_off_screen(wsxy, 4))
                    continue;
                /* Quad order: far left, far right, near left, near right. */
                p = ot_add(s, ot_entry(avsz4(s, wsz[0], wsz[1], wsz[2], wsz[3]) - 8), P_POLY);
                if (p == NULL)
                    return;
                p->u.poly.vertices = 4;
                p->u.poly.gouraud = 1;
                p->u.poly.blend = JB_BLEND_HALF;
                p->u.poly.texture = &s->water;
                for (k = 0; k < 4; ++k) {
                    static const int qk[4] = {2, 3, 0, 1};
                    int fog = clampi(wsz[qk[k]] * 96 / FAR, 0, 96);
                    p->u.poly.v[k] = at(wsxy[qk[k]], grey(150 - fog), T_WATER + (ci4[qk[k]] ? 63 : 0),
                                        v0 + (cj4[qk[k]] ? 0 : 63));
                }
            }
        }
}

/* Blocks on some cells: 8-bit stone sides, a 15-bit metal top, lit. */
static void add_blocks(scene *s, terrain *t)
{
    /* Faces as a quad takes them (seen from outside: top left, top right,
       bottom left, bottom right). Corner k: x + with bit 0, y at the base
       (down) with bit 1, z + (far) with bit 2. Bottom never shows. */
    static const int face[5][4] = {
        {0, 1, 2, 3}, {5, 4, 7, 6}, {1, 5, 3, 7}, {4, 0, 6, 2}, {4, 5, 0, 1},
    };
    static const int uv[4][2] = {{0, 0}, {63, 0}, {0, 63}, {63, 63}};
    int i, j, k, f;

    for (j = 1; j < GD; ++j)
        for (i = 0; i < VW - 1; ++i) {
            int wi = t->ci - GW + i, wj = t->cj + j;
            uint32_t hsh = hash3(wi, wj, 7), sxy[8], rgb[8];
            int base, size, tall, sz[8], skip = 0;
            vec corner[8], normal[8];

            if (hsh % 11 != 0)
                continue;
            base = t->h[j + 1][i + 1];
            if (base < WATER + 20)
                continue;
            size = 22 + (int)(hsh >> 8 & 15);
            tall = 50 + (int)(hsh >> 12 & 127);
            for (k = 0; k < 8; ++k) {
                corner[k] = rel(t, wi * CELL + CELL / 2 + (k & 1 ? size : -size),
                                k & 2 ? -base + 10 : -base - tall,
                                wj * CELL + CELL / 2 + (k & 4 ? size : -size));
                normal[k].x = k & 1 ? 2365 : -2365;
                normal[k].y = k & 2 ? 2365 : -2365;
                normal[k].z = k & 4 ? 2365 : -2365;
            }
            project(s, corner, 8, sxy, sz);
            for (k = 0; k < 8; ++k)
                skip |= sz[k] < NEAR;
            if (skip || sxy_off_screen(sxy, 8))
                continue;
            light(s, normal, 8, rgb);
            for (f = 0; f < 5; ++f) {
                const int *c = face[f];
                prim *p;

                if (nclip(s, sxy[c[0]], sxy[c[1]], sxy[c[2]]) <= 0)
                    continue;
                p = ot_add(s, ot_entry(avsz4(s, sz[c[0]], sz[c[1]], sz[c[2]], sz[c[3]])), P_POLY);
                if (p == NULL)
                    return;
                p->u.poly.vertices = 4;
                p->u.poly.gouraud = 1;
                p->u.poly.texture = f == 4 ? &s->metal : &s->stone;
                for (k = 0; k < 4; ++k)
                    p->u.poly.v[k] = at(sxy[c[k]], rgb[c[k]], (f == 4 ? METAL_U : STONE_U) + uv[k][0],
                                        uv[k][1]);
            }
        }
}

/* Motes (additive sprites) and falling streaks (lines), drifting towards
   the camera through the space in front of it. */
static void add_motes_and_streaks(scene *s, unsigned frame)
{
    vec v[MOTES];
    uint32_t sxy[MOTES];
    int sz[MOTES], k;

    for (k = 0; k < MOTES; ++k) {
        uint32_t h = hash3(k, 0, 51);
        int z = (int)((uint32_t)(k * 397) + 1000000u - frame * 13u) % (FAR - 400) + 200;

        v[k].x = ((int)(h & 2047) - 1024) * z / 1600;
        v[k].y = CAM_ALT - 280 - (int)(h >> 11 & 511) + 40 * isin((int)frame * 8 + k * 37) / ONE;
        v[k].z = z;
    }
    /* World-axis offsets from the camera, through its rotation. */
    project(s, v, MOTES, sxy, sz);
    for (k = 0; k < MOTES; ++k) {
        int size = sz[k] < 1400 ? 16 : 8, x = JB_GTE_SX(sxy[k]) - size / 2, y = JB_GTE_SY(sxy[k]) - size / 2;
        int c = clampi(128 - sz[k] * 96 / FAR, 32, 128);
        prim *p;

        if (sz[k] < NEAR || off_screen(x, y, x + size - 1, y + size - 1))
            continue;
        p = ot_add(s, ot_entry(sz[k]), P_RECT);
        if (p == NULL)
            return;
        p->u.rect.x = x, p->u.rect.y = y, p->u.rect.w = p->u.rect.h = size;
        p->u.rect.color = grey(c);
        p->u.rect.blend = JB_BLEND_ADD;
        p->u.rect.texture = &s->mote;
        p->u.rect.u = size == 16 ? 0 : 16;
        p->u.rect.v = SPRITE_V;
    }
    for (k = 0; k < STREAKS; ++k) {
        uint32_t h = hash3(k, 1, 52);
        int z = (int)(h >> 8 & 4095) % (FAR - 600) + 300;
        int y = (int)((uint32_t)(k * 211) + frame * 23u) % 900;
        vec e[2];
        uint32_t q[2];
        int qz[2];
        prim *p;

        e[0].x = ((int)(h & 2047) - 1024) * z / 1700;
        e[0].y = y;
        e[0].z = z;
        e[1].x = e[0].x + 12, e[1].y = y + 70, e[1].z = z;
        project(s, e, 2, q, qz);
        if (qz[0] < NEAR || qz[1] < NEAR || sxy_off_screen(q, 2))
            continue;
        p = ot_add(s, ot_entry((qz[0] + qz[1]) / 2), P_LINE);
        if (p == NULL)
            return;
        p->u.line.gouraud = 1;
        p->u.line.blend = k & 1 ? JB_BLEND_NONE : JB_BLEND_ADD;
        p->u.line.v[0] = at(q[0], JB_RGB(40, 50, 70), 0, 0);
        p->u.line.v[1] = at(q[1], JB_RGB(150, 170, 220), 0, 0);
    }
}

/* The sky and the mountains, behind everything: drawn before the table. */
static void draw_backdrop(scene *s, unsigned frame, const terrain *t)
{
    jb_poly p;
    int k, off = (int)((unsigned)(t->cam_x / 8 + 24 * isin((int)frame * 2) / ONE * 4 + 4096) % 64u);

    memset(&p, 0, sizeof p);
    p.vertices = 4;
    p.gouraud = 1;
    p.v[0].x = 0, p.v[0].y = 0, p.v[0].color = JB_RGB(40, 60, 140);
    p.v[1].x = WL_WIDTH, p.v[1].y = 0, p.v[1].color = JB_RGB(56, 64, 150);
    p.v[2].x = 0, p.v[2].y = 96, p.v[2].color = JB_RGB(200, 170, 150);
    p.v[3].x = WL_WIDTH, p.v[3].y = 96, p.v[3].color = JB_RGB(220, 160, 140);
    send_poly(s, &p);
    for (k = 0; k < 6; ++k) {
        int x0 = k * 64 - off, y0 = 34;

        memset(&p, 0, sizeof p);
        p.vertices = 4;
        p.gouraud = 1;
        p.texture = &s->mount;
        p.v[0].x = x0, p.v[0].y = y0, p.v[0].u = MOUNT_U + (k & 1) * 64;
        p.v[1].x = x0 + 64, p.v[1].y = y0, p.v[1].u = MOUNT_U + (k & 1) * 64 + 63;
        p.v[2].x = x0, p.v[2].y = y0 + MOUNT_H, p.v[2].u = p.v[0].u, p.v[2].v = MOUNT_H - 1;
        p.v[3].x = x0 + 64, p.v[3].y = y0 + MOUNT_H, p.v[3].u = p.v[1].u, p.v[3].v = MOUNT_H - 1;
        p.v[0].color = p.v[1].color = JB_RGB(110, 110, 130);
        p.v[2].color = p.v[3].color = JB_RGB(150, 140, 140);
        send_poly(s, &p);
    }
}

/* The HUD, nearest of all (entry 0, added in reverse of drawing order). */
static void add_hud(scene *s, unsigned frame, const terrain *t)
{
    prim *p;
    int k, a, b;

    /* Crosshair. */
    for (k = 0; k < 2; ++k) {
        if ((p = ot_add(s, 0, P_LINE)) == NULL)
            return;
        p->u.line.v[0].x = 160 - 6 * (k == 0), p->u.line.v[0].y = 120 - 6 * (k == 1);
        p->u.line.v[1].x = 160 + 6 * (k == 0), p->u.line.v[1].y = 120 + 6 * (k == 1);
        p->u.line.v[0].color = JB_RGB(255, 255, 255);
    }
    /* Minimap: 8 x 8 cells of 4 x 4 pixels, coloured by height, in a
       Gouraud polyline border. */
    if ((p = ot_add(s, 0, P_POLYLINE)) != NULL) {
        static const int bx[5] = {253, 288, 288, 253, 253}, by[5] = {181, 181, 216, 216, 181};
        p->u.pl.count = 5;
        for (k = 0; k < 5; ++k) {
            p->u.pl.v[k].x = bx[k], p->u.pl.v[k].y = by[k];
            p->u.pl.v[k].color = k & 1 ? JB_RGB(255, 220, 120) : JB_RGB(120, 160, 255);
        }
    }
    for (b = 0; b < 8; ++b)
        for (a = 0; a < 8; ++a) {
            int hh = t->h[1 + b * 4][1 + GW - 16 + a * 4];
            if ((p = ot_add(s, 0, P_RECT)) == NULL)
                return;
            p->u.rect.x = 255 + a * 4, p->u.rect.y = 211 - b * 4, p->u.rect.w = p->u.rect.h = 4;
            p->u.rect.color = hh < WATER ? JB_RGB(40, 80, 200) : grey(hh * 255 / 700);
        }
    /* Glyphs on the top bar: 32 of 8 x 8, raw texels. */
    for (k = 0; k < 32; ++k) {
        if ((p = ot_add(s, 0, P_RECT)) == NULL)
            return;
        p->u.rect.x = 8 + k * 9, p->u.rect.y = 5, p->u.rect.w = p->u.rect.h = 8;
        p->u.rect.color = JB_RGB(128, 128, 128);
        p->u.rect.raw = 1;
        p->u.rect.texture = &s->glyph;
        p->u.rect.u = GLYPH_U + (int)((k * 7u + frame / 8u) % 16u) * 8;
        p->u.rect.v = SPRITE_V;
    }
    /* Two half-blended bars, under the rest (added last). */
    for (k = 0; k < 2; ++k) {
        if ((p = ot_add(s, 0, P_RECT)) == NULL)
            return;
        p->u.rect.x = 0, p->u.rect.y = k ? WL_HEIGHT - 18 : 0;
        p->u.rect.w = WL_WIDTH, p->u.rect.h = 18;
        p->u.rect.color = JB_RGB(0, 0, 40);
        p->u.rect.blend = JB_BLEND_HALF;
    }
}

static jb_result heavy_draw(workload *w, unsigned frame, wl_stats *st)
{
    scene *s = (scene *)w;
    terrain *t = s->terrain;
    uint16_t clut[16];
    int top;

    s->stats = st;
    if (st != NULL)
        wl_stats_begin_frame(st);
    top = begin_frame(s, 0, JB_RGB(40, 60, 140));
    water_clut(frame, clut); /* palette animation */
    note(s, jb_vram_load(s->base.jb, 48, CLUT_Y, 16, 1, clut));
    camera(s, frame, t);
    ot_clear(s);
    add_terrain(s, t);
    add_blocks(s, t);
    add_motes_and_streaks(s, frame);
    add_hud(s, frame, t);
    draw_backdrop(s, frame, t);
    ot_draw(s);
    end_frame(s, top);
    if (st != NULL)
        wl_stats_end_frame(st);
    s->stats = NULL;
    return s->error;
}

static workload *heavy_create(const wl_options *o, jb_result *result)
{
    scene *s = scene_create(o, result);

    if (s == NULL)
        return NULL;
    s->base.draw = heavy_draw;
    if ((s->terrain = malloc(sizeof *s->terrain)) == NULL) {
        scene_destroy(&s->base);
        *result = JB_ERROR_OUT_OF_MEMORY;
        return NULL;
    }
    gte_setup(s);
    if ((*result = s->error) != JB_OK) {
        scene_destroy(&s->base);
        return NULL;
    }
    return &s->base;
}

const wl_info wl_heavy = {
    "heavy", "terrain, water, lit blocks, sprites, lines and a HUD through the GTE", heavy_create};

/* ------------------------------------------------------------------------ */
/* fill */

static jb_result fill_draw(workload *w, unsigned frame, wl_stats *st)
{
    static const int blend[4] = {JB_BLEND_SUB, JB_BLEND_HALF, JB_BLEND_QUARTER, JB_BLEND_ADD};
    scene *s = (scene *)w;
    int top, q, k;

    s->stats = st;
    if (st != NULL)
        wl_stats_begin_frame(st);
    top = begin_frame(s, 1, JB_RGB(60, 50, 70));
    for (q = 0; q < 4; ++q) {
        /* A plane 500 ahead, tilted about x by up to 15 degrees either
           way and turned a little, big enough to cover the display at any
           of those angles. */
        int tilt = 43 * isin((int)frame * 5 + q * 256) / ONE;
        int yaw = 8 * isin((int)frame * 3 + q * 300) / ONE;
        int ct = icos(tilt), st_ = isin(tilt), cy = icos(yaw), sy = isin(yaw);
        int u0 = (int)((frame * 2u + (unsigned)q * 16u) % 64u), v0 = (int)((frame + (unsigned)q * 8u) % 64u);
        static const vec corner[4] = {{-430, -320, 0}, {430, -320, 0}, {-430, 320, 0}, {430, 320, 0}};
        const jb_texture *tx[4];
        uint32_t sxy[4];
        int sz[4];
        matrix r;
        jb_poly p;

        tx[0] = &s->rock, tx[1] = &s->stone, tx[2] = &s->grass, tx[3] = &s->metal;
        r.m[0][0] = cy, r.m[0][1] = 0, r.m[0][2] = sy;
        r.m[1][0] = fmul(st_, sy), r.m[1][1] = ct, r.m[1][2] = -fmul(st_, cy);
        r.m[2][0] = -fmul(ct, sy), r.m[2][1] = st_, r.m[2][2] = fmul(ct, cy);
        gte_matrix(s, JB_GTE_RT11RT12, &r);
        gte_translation(s, 0, 0, 500);
        project(s, corner, 4, sxy, sz);
        memset(&p, 0, sizeof p);
        p.vertices = 4;
        p.gouraud = 1;
        p.blend = blend[q];
        p.texture = tx[q];
        for (k = 0; k < 4; ++k) {
            int c = 96 + 32 * isin((int)frame * 7 + k * 256 + q * 100) / ONE;
            p.v[k] = at(sxy[k], q == 0 ? JB_RGB(c / 3, c / 3, c / 3) : JB_RGB(c, c - 16, c + 16),
                        u0 + (k & 1) * 191, v0 + (k >> 1) * 191);
        }
        send_poly(s, &p);
    }
    end_frame(s, top);
    if (st != NULL)
        wl_stats_end_frame(st);
    s->stats = NULL;
    return s->error;
}

static workload *fill_create(const wl_options *o, jb_result *result)
{
    scene *s = scene_create(o, result);

    if (s == NULL)
        return NULL;
    s->base.draw = fill_draw;
    gte_setup(s);
    if ((*result = s->error) != JB_OK) {
        scene_destroy(&s->base);
        return NULL;
    }
    return &s->base;
}

const wl_info wl_fill = {
    "fill", "four full-screen textured, Gouraud, semi-transparent quads (fill rate)", fill_create};

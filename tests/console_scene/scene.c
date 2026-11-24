/*
 * first-light: the scene (scene.h). C99, the public API only, integer
 * arithmetic only.
 *
 * VRAM, 1024 x 512 16-bit pixels, laid out by hand:
 *
 *   x:  0          320     384     448     512    520
 *   y:0 +-----------+-------+-------+-------+------+
 *       | buffer 0  | floor | plasma| emblem| star |  texture pages at
 *       | 320 x 240 | 4-bit | 8-bit | 15-bit| 4-bit|  x 320, 384, 448, 512
 *   240 +-----------+ 16x64 | 32x64 | 64x64 | 12x32|  (VRAM pixels used)
 *       | buffer 1  +-------+-------+-------+------+
 *       | 320 x 240 |
 *   480 +-----------+  row 480: floor CLUT (x 0-15), star CLUT (x 16-31)
 *   481                row 481: plasma CLUT (x 0-255), rewritten each frame
 *
 * Each frame is drawn into one buffer while the other is on show, then the
 * display is switched to it with GP1(05h).
 */
#include "scene.h"

#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Fixed point: 4096 is 1.0 (the GTE's 1.3.12 format). Angles are 0..1023
   for a whole turn. */

enum { ONE = 4096 };

/* sin of a quarter turn in 64 steps, times 4096 (round(4096 sin(i pi/128))). */
static const short quarter_sine[65] = {
    0,    101,  201,  301,  401,  501,  601,  700,  799,  897,  995,  1092, 1189,
    1285, 1380, 1474, 1567, 1660, 1751, 1842, 1931, 2019, 2106, 2191, 2276, 2359,
    2440, 2520, 2598, 2675, 2751, 2824, 2896, 2967, 3035, 3102, 3166, 3229, 3290,
    3349, 3406, 3461, 3513, 3564, 3612, 3659, 3703, 3745, 3784, 3822, 3857, 3889,
    3920, 3948, 3973, 3996, 4017, 4036, 4052, 4065, 4076, 4085, 4091, 4095, 4096,
};

/* sin(angle) * 4096, angle in 1/1024 turns, interpolated between the
   table's entries (every 4 units). */
static int isin(unsigned angle)
{
    unsigned a = angle & 1023u, r = a & 255u;
    int i, v;

    if (a & 256u) /* the second and fourth quarters run backwards */
        r = 256u - r;
    i = (int)(r >> 2);
    v = quarter_sine[i];
    if (i < 64)
        v += (quarter_sine[i + 1] - quarter_sine[i]) * (int)(r & 3u) / 4;
    return a & 512u ? -v : v;
}

static int icos(unsigned angle)
{
    return isin(angle + 256u);
}

/* a * b in 4.12 (C99 division rounds towards zero on every machine). */
static int fmul(int a, int b)
{
    return a * b / ONE;
}

static int iabs(int a)
{
    return a < 0 ? -a : a;
}

/* ------------------------------------------------------------------------ */
/* VRAM layout (see the top of the file) */

enum {
    FLOOR_PAGE_X = 320,  /* 4-bit: 4 texels a VRAM pixel */
    PLASMA_PAGE_X = 384, /* 8-bit: 2 texels a pixel */
    EMBLEM_PAGE_X = 448, /* 15-bit: 1 texel a pixel */
    STAR_PAGE_X = 512,   /* 4-bit */
    TEX = 64,            /* floor, plasma and emblem are TEX x TEX texels */
    STAR_BIG = 32,       /* the star sprite: 32 x 32 at u 0, 16 x 16 at u 32 */
    STAR_SMALL = 16,
    STAR_ROW = STAR_BIG + STAR_SMALL, /* texels in a row of the star texture */
    CLUT_Y = 480, /* floor and star CLUTs; the plasma's is the row below */
    FLOOR_CLUT_X = 0,
    STAR_CLUT_X = 16
};

/* A 15-bit VRAM colour from 5-bit channels. */
static uint16_t rgb15(int r, int g, int b)
{
    return (uint16_t)(r | g << 5 | b << 10);
}

enum { SEMI = 0x8000 }; /* bit 15: a semi-transparent texel when blending */

/* A small integer hash, for texture speckle. */
static unsigned hash2(unsigned x, unsigned y)
{
    unsigned h = x * 73856093u ^ y * 19349663u;
    return (h ^ h >> 13) * 0x5bd1e995u >> 24;
}

/* Packs n 4-bit texels into VRAM pixels, the first texel of each four in
   the low nibble. */
static void pack_4bit(const uint8_t *texels, int n, uint16_t *px)
{
    int i;

    for (i = 0; i < n / 4; ++i) {
        const uint8_t *t = texels + i * 4;
        px[i] = (uint16_t)(t[0] | t[1] << 4 | t[2] << 8 | t[3] << 12);
    }
}

/* The floor: a 4-bit 2 x 2 checker of speckled stone with dark grout. */
static jb_result make_floor(jb_context *jb)
{
    static const uint8_t base[2] = {1, 8};
    uint8_t tex[TEX * TEX];
    uint16_t px[TEX * TEX / 4], clut[16];
    jb_result r;
    int u, v, i;

    for (v = 0; v < TEX; ++v)
        for (u = 0; u < TEX; ++u) {
            int light = ((u >> 5) ^ (v >> 5)) & 1;
            int grout = (u & 31) == 0 || (v & 31) == 0;
            int speckle = (int)(hash2((unsigned)u, (unsigned)v) & 3);
            tex[v * TEX + u] = (uint8_t)(grout ? 15 : base[light] + speckle);
        }
    for (i = 0; i < 16; ++i)
        clut[i] = 0x8000; /* opaque black: 0x0000 would be transparent */
    clut[1] = rgb15(22, 21, 19), clut[2] = rgb15(23, 22, 20);
    clut[3] = rgb15(21, 20, 18), clut[4] = rgb15(24, 23, 21);
    clut[8] = rgb15(9, 10, 13), clut[9] = rgb15(10, 11, 14);
    clut[10] = rgb15(8, 9, 12), clut[11] = rgb15(11, 12, 15);
    clut[15] = rgb15(4, 4, 5);
    pack_4bit(tex, TEX * TEX, px);
    if ((r = jb_vram_load(jb, FLOOR_PAGE_X, 0, TEX / 4, TEX, px)) != JB_OK)
        return r;
    return jb_vram_load(jb, FLOOR_CLUT_X, CLUT_Y, 16, 1, clut);
}

/* The plasma: 8-bit indices from four sine waves; its 256-colour CLUT
   (plasma_palette) cycles each frame. */
static jb_result make_plasma(jb_context *jb)
{
    uint16_t px[TEX / 2 * TEX];
    int u, v, k;

    for (v = 0; v < TEX; ++v)
        for (u = 0; u < TEX; u += 2) {
            int idx[2];

            for (k = 0; k < 2; ++k) {
                unsigned x = (unsigned)(u + k), y = (unsigned)v;
                int sum = isin(x * 16) + isin(y * 16) + isin((x + y) * 8) + isin((x - y) * 8);
                idx[k] = (sum + 4 * ONE) * 255 / (8 * ONE);
            }
            /* Two texels a pixel, the first in the low byte. */
            px[v * (TEX / 2) + u / 2] = (uint16_t)(idx[0] | idx[1] << 8);
        }
    return jb_vram_load(jb, PLASMA_PAGE_X, 0, TEX / 2, TEX, px);
}

/* The plasma's CLUT for a frame: three phase-shifted sines, turning. At
   most 19 of 31, so the lighting (which may double a texel) shows. */
static void plasma_palette(unsigned frame, uint16_t clut[256])
{
    int i;

    for (i = 0; i < 256; ++i) {
        unsigned a = (unsigned)i * 4 + frame * 8;
        clut[i] = rgb15(10 + 9 * isin(a) / ONE, 10 + 9 * isin(a + 341) / ONE,
                        10 + 9 * isin(a + 683) / ONE);
    }
}

/* The emblem: a 15-bit tile, a gold diamond on a teal gradient with a
   bevelled edge. */
static jb_result make_emblem(jb_context *jb)
{
    uint16_t px[TEX * TEX];
    int u, v;

    for (v = 0; v < TEX; ++v)
        for (u = 0; u < TEX; ++u) {
            int du = iabs(2 * u - 63), dv = iabs(2 * v - 63), c;
            uint16_t p;

            if (du >= 57 || dv >= 57) {
                int lit = du > dv ? u < 32 : v < 32; /* light from the top left */
                p = lit ? rgb15(28, 28, 30) : rgb15(6, 6, 9);
            } else if (du + dv < 44) {
                c = 31 - (du + dv) / 3;
                p = rgb15(c, c * 3 / 4, c / 4 + 1);
            } else {
                p = rgb15(2, 8 + v / 8, 12 + v / 8);
            }
            px[v * TEX + u] = p;
        }
    return jb_vram_load(jb, EMBLEM_PAGE_X, 0, TEX, TEX, px);
}

/* The star: a glowing four-pointed sprite, size x size texels, into tex
   at column u0. Index 0 is transparent, 1-14 a semi-transparent glow, 15
   an opaque core. */
static void draw_star(uint8_t *tex, int u0, int size)
{
    int u, v, r = size - 1, reach = size * size;

    for (v = 0; v < size; ++v)
        for (u = 0; u < size; ++u) {
            int dx = 2 * u - r, dy = 2 * v - r, d2 = dx * dx + dy * dy, idx = 0;
            int arm = iabs(dx) <= 1 || iabs(dy) <= 1;

            if (d2 < reach / 25)
                idx = 15;
            else if (arm && d2 < reach)
                idx = 14 - d2 * 13 / reach;
            if (idx < 15 && d2 < reach / 3 && 14 - d2 * 39 / reach > idx)
                idx = 14 - d2 * 39 / reach;
            tex[v * STAR_ROW + u0 + u] = (uint8_t)idx;
        }
}

static jb_result make_star(jb_context *jb)
{
    uint8_t tex[STAR_ROW * STAR_BIG];
    uint16_t px[STAR_ROW / 4 * STAR_BIG], clut[16];
    jb_result r;
    int i;

    memset(tex, 0, sizeof tex);
    draw_star(tex, 0, STAR_BIG);
    draw_star(tex, STAR_BIG, STAR_SMALL);
    pack_4bit(tex, STAR_ROW * STAR_BIG, px);
    clut[0] = 0x0000; /* transparent */
    for (i = 1; i < 15; ++i)
        clut[i] = (uint16_t)(SEMI | rgb15(31 * i / 14, 26 * i / 14, 12 * i / 14));
    clut[15] = rgb15(31, 31, 28); /* no bit 15: opaque even when blending */
    if ((r = jb_vram_load(jb, STAR_PAGE_X, 0, STAR_ROW / 4, STAR_BIG, px)) != JB_OK)
        return r;
    return jb_vram_load(jb, STAR_CLUT_X, CLUT_Y, 16, 1, clut);
}

/* ------------------------------------------------------------------------ */
/* The GTE */

/* Keeps the first error of a frame. */
static void note(fl_scene *s, jb_result r)
{
    if (r != JB_OK && s->error == JB_OK)
        s->error = r;
}

static void gte_set(fl_scene *s, int reg, int value)
{
    note(s, jb_gte_write(s->jb, reg, (uint32_t)value));
}

static int gte_get(fl_scene *s, int reg)
{
    return (int)jb_gte_read(s->jb, reg);
}

static void gte_run(fl_scene *s, uint32_t command)
{
    note(s, jb_gte_command(s->jb, command));
}

/* A 3 x 3 matrix in 4.12, rows first. (A struct, so that a const one can
   be passed where a changing one can: C99 does not convert int (*)[3] to
   const int (*)[3].) */
typedef struct {
    int m[3][3];
} matrix;

/* A matrix into its five registers (rotation from RT11RT12, light from
   L11L12, light colour from LR1LR2): two 16-bit elements a register, row
   by row, the ninth alone. */
static void gte_matrix(fl_scene *s, int first_reg, const matrix *a)
{
    const int(*m)[3] = a->m;

    note(s, jb_gte_write(s->jb, first_reg + 0, JB_GTE_PAIR(m[0][0], m[0][1])));
    note(s, jb_gte_write(s->jb, first_reg + 1, JB_GTE_PAIR(m[0][2], m[1][0])));
    note(s, jb_gte_write(s->jb, first_reg + 2, JB_GTE_PAIR(m[1][1], m[1][2])));
    note(s, jb_gte_write(s->jb, first_reg + 3, JB_GTE_PAIR(m[2][0], m[2][1])));
    gte_set(s, first_reg + 4, m[2][2]);
}

static const matrix identity = {{{ONE, 0, 0}, {0, ONE, 0}, {0, 0, ONE}}};

static void gte_translation(fl_scene *s, int x, int y, int z)
{
    gte_set(s, JB_GTE_TRX, x);
    gte_set(s, JB_GTE_TRY, y);
    gte_set(s, JB_GTE_TRZ, z);
}

typedef struct {
    short x, y, z;
} vec3;

/* v into the GTE's vector register k (V0, V1 or V2). */
static void gte_vector(fl_scene *s, int k, const vec3 *v)
{
    note(s, jb_gte_write(s->jb, JB_GTE_VXY0 + 2 * k, JB_GTE_PAIR(v->x, v->y)));
    gte_set(s, JB_GTE_VZ0 + 2 * k, v->z);
}

/* Projects n points with the current rotation and translation: RTPT
   three at a time, RTPS for the rest. Screen positions (SXY) into sxy,
   depths (SZ) into sz. */
static void project(fl_scene *s, const vec3 *v, int n, uint32_t *sxy, int *sz)
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

/* Lights n normals with the light matrices, the ambient light and the
   material colour (RGBC): NCCT three at a time, NCCS for the rest.
   Colours into rgb, as JB_RGB values. */
static void light(fl_scene *s, const vec3 *normal, int n, uint32_t *rgb)
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

/* NCLIP: twice the signed area of the screen triangle a, b, c; above 0
   when it turns clockwise on screen (y down), as a face towards us does. */
static int winding(fl_scene *s, uint32_t a, uint32_t b, uint32_t c)
{
    gte_set(s, JB_GTE_SXY0, (int)a);
    gte_set(s, JB_GTE_SXY1, (int)b);
    gte_set(s, JB_GTE_SXY2, (int)c);
    gte_run(s, JB_GTE_NCLIP);
    return gte_get(s, JB_GTE_MAC0);
}

/* AVSZ4: the average of four depths (ZSF4 is a quarter), as OTZ. */
static int average_z4(fl_scene *s, int a, int b, int c, int d)
{
    gte_set(s, JB_GTE_SZ0, a);
    gte_set(s, JB_GTE_SZ1, b);
    gte_set(s, JB_GTE_SZ2, c);
    gte_set(s, JB_GTE_SZ3, d);
    gte_run(s, JB_GTE_AVSZ4);
    return gte_get(s, JB_GTE_OTZ);
}

/* The GTE's settings that never change: the screen centre and distance,
   the depth averages' scales, the lights' colours, the ambient light, and
   the material colour lighting multiplies (128: textures unchanged). */
static void gte_setup(fl_scene *s)
{
    /* Light colours, a column each (4096 is 1.0): a warm key light and a
       blue rim light. */
    static const matrix light_colour = {{
        {6554, 1434, 0}, /* red of lights 1, 2, 3 */
        {5939, 2458, 0}, /* green */
        {5120, 4915, 0}, /* blue */
    }};

    gte_set(s, JB_GTE_OFX, FL_WIDTH / 2 * 65536); /* 16.16 */
    gte_set(s, JB_GTE_OFY, FL_HEIGHT / 2 * 65536);
    gte_set(s, JB_GTE_H, 256); /* the projection plane's distance */
    gte_set(s, JB_GTE_ZSF3, ONE / 3);
    gte_set(s, JB_GTE_ZSF4, ONE / 4);
    gte_matrix(s, JB_GTE_LR1LR2, &light_colour);
    gte_set(s, JB_GTE_RBK, 492); /* ambient, 1.19.12 */
    gte_set(s, JB_GTE_GBK, 492);
    gte_set(s, JB_GTE_BBK, 737);
    gte_set(s, JB_GTE_RGBC, (int)JB_RGB(128, 128, 128));
}

/* ------------------------------------------------------------------------ */
/* The ordering table, the console's way: an array of lists, one per depth
   step, of primitives kept in a buffer. Each primitive goes at the head of
   its entry's list (as the console's addPrim does, so one entry draws its
   primitives in the reverse of the order they were added), and the table
   is drawn from its far end to entry 0. */

static void ot_clear(fl_scene *s)
{
    int i;

    for (i = 0; i < FL_OT_LEN; ++i)
        s->ot[i] = -1;
    s->nprims = 0;
}

/* A new, zeroed primitive of the given kind in entry z, or NULL when the
   buffer is full. */
static fl_prim *ot_add(fl_scene *s, int z, int kind)
{
    fl_prim *p;

    if (s->nprims == FL_PRIMS) {
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

/* The entry for a depth (OTZ or SZ): 8 depth units each, kept clear of
   the entries of the floor (the last), its shadow (the last but one) and
   the panel (0). */
static int ot_entry(int z)
{
    z >>= 3;
    return z < 1 ? 1 : z > FL_OT_LEN - 3 ? FL_OT_LEN - 3 : z;
}

static void ot_draw(fl_scene *s)
{
    int z, i;

    for (z = FL_OT_LEN - 1; z >= 0; --z)
        for (i = s->ot[z]; i >= 0; i = s->prims[i].next) {
            const fl_prim *p = &s->prims[i];
            jb_polyline pl;

            switch (p->kind) {
            case FL_POLY: note(s, jb_prim_poly(s->jb, &p->u.poly)); break;
            case FL_RECT: note(s, jb_prim_rect(s->jb, &p->u.rect)); break;
            case FL_LINE: note(s, jb_prim_line(s->jb, &p->u.line)); break;
            case FL_POLYLINE:
                pl.gouraud = p->u.polyline.gouraud;
                pl.blend = p->u.polyline.blend;
                pl.count = (size_t)p->u.polyline.count;
                pl.v = p->u.polyline.v;
                note(s, jb_prim_polyline(s->jb, &pl));
                break;
            }
        }
}

/* A vertex at a projected screen position. */
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

/* ------------------------------------------------------------------------ */
/* The scene's parts. Space is the camera's: x right, y down, z away from
   the eye, which is at the origin looking along z. */

enum {
    CUBE = 128,    /* half the cube's side */
    CUBE_Z = 700,  /* its centre's distance */
    FLOOR_Y = 200, /* the floor's height (below the eye) */
    ORBIT = 260,   /* the star's orbit radius about the cube */
    RING_POINTS = 16
};

/* The height of the cube's centre at a frame: it bobs. */
static int cube_y(unsigned frame)
{
    return -40 + 20 * isin(frame * 8) / ONE;
}

/* A rotation about y by a, after one about x by b. */
static void rotation(unsigned a, unsigned b, matrix *r)
{
    int ca = icos(a), sa = isin(a), cb = icos(b), sb = isin(b);
    int(*m)[3] = r->m;

    m[0][0] = ca, m[0][1] = fmul(sa, sb), m[0][2] = fmul(sa, cb);
    m[1][0] = 0, m[1][1] = cb, m[1][2] = -sb;
    m[2][0] = -sa, m[2][1] = fmul(ca, sb), m[2][2] = fmul(ca, cb);
}

/* The sky: one Gouraud quad from night blue down to a dusk glow (dithered:
   the bands between its 15-bit steps are broken up), with 1 x 1 stars. */
static void draw_sky(fl_scene *s, unsigned frame)
{
    jb_poly sky;
    int i;

    memset(&sky, 0, sizeof sky);
    sky.vertices = 4;
    sky.gouraud = 1;
    sky.v[0].x = 0, sky.v[0].y = 0, sky.v[0].color = JB_RGB(16, 20, 64);
    sky.v[1].x = FL_WIDTH, sky.v[1].y = 0, sky.v[1].color = JB_RGB(24, 16, 72);
    sky.v[2].x = 0, sky.v[2].y = 150, sky.v[2].color = JB_RGB(208, 112, 88);
    sky.v[3].x = FL_WIDTH, sky.v[3].y = 150, sky.v[3].color = JB_RGB(176, 96, 112);
    note(s, jb_prim_poly(s->jb, &sky));

    for (i = 0; i < 24; ++i) {
        jb_rect dot;
        int b = 128 + 127 * isin(frame * 8 + (unsigned)i * 100) / ONE;

        memset(&dot, 0, sizeof dot);
        dot.x = (i * 97 + 13) % FL_WIDTH;
        dot.y = (i * 53 + 7) % 120;
        dot.w = dot.h = 1;
        dot.color = JB_RGB(b, b, b);
        note(s, jb_prim_rect(s->jb, &dot));
    }
}

/* The floor: one large textured quad, its texture repeated by the texture
   window, darker with distance. Textured affinely, its checks bend along
   the diagonal that splits it into two triangles. */
static void add_floor(fl_scene *s)
{
    static const vec3 corner[4] = {
        {-640, FLOOR_Y, 2200}, {640, FLOOR_Y, 2200}, /* far left, far right */
        {-640, FLOOR_Y, 360},  {640, FLOOR_Y, 360},  /* near left, near right */
    };
    static const int uv[4][2] = {{0, 0}, {255, 0}, {0, 255}, {255, 255}};
    uint32_t sxy[4];
    int sz[4], k;
    fl_prim *p;

    gte_matrix(s, JB_GTE_RT11RT12, &identity);
    gte_translation(s, 0, 0, 0);
    project(s, corner, 4, sxy, sz);
    p = ot_add(s, FL_OT_LEN - 1, FL_POLY);
    if (p == NULL)
        return;
    p->u.poly.vertices = 4;
    p->u.poly.gouraud = 1;
    p->u.poly.texture = &s->floor;
    for (k = 0; k < 4; ++k)
        p->u.poly.v[k] = at(sxy[k], k < 2 ? JB_RGB(40, 40, 48) : JB_RGB(160, 160, 160),
                            uv[k][0], uv[k][1]);
}

/* The cube's shadow: a flat square on the floor, turning with the cube,
   subtracted from what is under it (blend mode B - F). */
static void add_shadow(fl_scene *s, unsigned frame)
{
    static const vec3 corner[4] = {{-150, 0, 150}, {150, 0, 150}, {-150, 0, -150}, {150, 0, -150}};
    uint32_t sxy[4];
    int sz[4], k;
    matrix m;
    fl_prim *p;

    rotation(frame * 4, 0, &m);
    gte_matrix(s, JB_GTE_RT11RT12, &m);
    gte_translation(s, 0, FLOOR_Y, CUBE_Z);
    project(s, corner, 4, sxy, sz);
    p = ot_add(s, FL_OT_LEN - 2, FL_POLY);
    if (p == NULL)
        return;
    p->u.poly.vertices = 4;
    p->u.poly.blend = JB_BLEND_SUB;
    for (k = 0; k < 4; ++k)
        p->u.poly.v[k] = at(sxy[k], JB_RGB(72, 72, 56), 0, 0);
}

/* The cube: eight shared corners projected and lit once, then each face
   that turns towards us sorted by its average depth. Corner i is at
   x = (i & 1 ? + : -) CUBE, y likewise with i & 2, z with i & 4. Its
   normal points out through the corner, so the faces are Gouraud-shaded
   like a smooth solid. */
static void add_cube(fl_scene *s, unsigned frame)
{
    /* Each face's corners in the order a quad takes them (top left, top
       right, bottom left, bottom right, seen from outside), and its
       texture: the 8-bit plasma, or the 15-bit emblem. */
    static const struct {
        int c[4];
        int emblem;
    } face[6] = {
        {{0, 1, 2, 3}, 1}, /* front, z - */
        {{5, 4, 7, 6}, 1}, /* back, z + */
        {{1, 5, 3, 7}, 0}, /* right, x + */
        {{4, 0, 6, 2}, 0}, /* left, x - */
        {{4, 5, 0, 1}, 0}, /* top, y - */
        {{2, 3, 6, 7}, 0}, /* bottom, y + */
    };
    static const int uv[4][2] = {{0, 0}, {TEX - 1, 0}, {0, TEX - 1}, {TEX - 1, TEX - 1}};
    /* Directions towards the two lights, unit length (4096). */
    static const int towards_light[3][3] = {
        {-1843, -2662, -2499}, /* key: above, left, in front */
        {3277, 819, 2294},     /* rim: right, behind */
        {0, 0, 0},
    };
    vec3 corner[8], normal[8];
    uint32_t sxy[8], rgb[8];
    int sz[8], i, j, k;
    matrix rot, llm;

    for (i = 0; i < 8; ++i) {
        corner[i].x = (short)(i & 1 ? CUBE : -CUBE);
        corner[i].y = (short)(i & 2 ? CUBE : -CUBE);
        corner[i].z = (short)(i & 4 ? CUBE : -CUBE);
        normal[i].x = (short)(i & 1 ? 2365 : -2365); /* 4096 / sqrt(3) */
        normal[i].y = (short)(i & 2 ? 2365 : -2365);
        normal[i].z = (short)(i & 4 ? 2365 : -2365);
    }
    rotation(frame * 4, frame * 2 + 96, &rot);
    gte_matrix(s, JB_GTE_RT11RT12, &rot);
    gte_translation(s, 0, cube_y(frame), CUBE_Z);
    project(s, corner, 8, sxy, sz);

    /* The normals are the cube's own, so the lights are turned into the
       cube's space: the light matrix is the light directions times the
       rotation. */
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j)
            llm.m[i][j] = (towards_light[i][0] * rot.m[0][j] + towards_light[i][1] * rot.m[1][j] +
                           towards_light[i][2] * rot.m[2][j]) /
                          ONE;
    gte_matrix(s, JB_GTE_L11L12, &llm);
    light(s, normal, 8, rgb);

    for (i = 0; i < 6; ++i) {
        const int *c = face[i].c;
        fl_prim *p;

        if (winding(s, sxy[c[0]], sxy[c[1]], sxy[c[2]]) <= 0)
            continue; /* turned away */
        p = ot_add(s, ot_entry(average_z4(s, sz[c[0]], sz[c[1]], sz[c[2]], sz[c[3]])), FL_POLY);
        if (p == NULL)
            return;
        p->u.poly.vertices = 4;
        p->u.poly.gouraud = 1;
        p->u.poly.texture = face[i].emblem ? &s->emblem : &s->plasma;
        for (k = 0; k < 4; ++k)
            p->u.poly.v[k] = at(sxy[c[k]], rgb[c[k]], uv[k][0], uv[k][1]);
    }
}

/* The star: a sprite on a ring about the cube, sorted by its depth, so it
   passes behind the cube and in front of it. It blends additively: its
   glow texels have bit 15 set, its core's do not. Near, it is the 32 x 32
   sprite; far, the 16 x 16 one. The ring is lines, each sorted by its own
   depth. */
static void add_star_and_ring(fl_scene *s, unsigned frame)
{
    vec3 ring[RING_POINTS], star;
    uint32_t sxy[RING_POINTS], star_sxy;
    int sz[RING_POINTS], star_sz, i, size, glow;
    unsigned a = frame * 6;
    fl_prim *p;

    gte_matrix(s, JB_GTE_RT11RT12, &identity);
    gte_translation(s, 0, cube_y(frame), CUBE_Z);
    for (i = 0; i < RING_POINTS; ++i) {
        unsigned t = (unsigned)i * (1024 / RING_POINTS);
        ring[i].x = (short)(ORBIT * icos(t) / ONE);
        ring[i].y = 0;
        ring[i].z = (short)(ORBIT * isin(t) / ONE);
    }
    project(s, ring, RING_POINTS, sxy, sz);
    for (i = 0; i < RING_POINTS; ++i) {
        int j = (i + 1) % RING_POINTS;
        /* Brighter nearer: 255 at the front of the ring, 55 at the back. */
        int bi = 255 - (sz[i] - CUBE_Z + ORBIT) * 200 / (2 * ORBIT);
        int bj = 255 - (sz[j] - CUBE_Z + ORBIT) * 200 / (2 * ORBIT);

        p = ot_add(s, ot_entry((sz[i] + sz[j]) / 2), FL_LINE);
        if (p == NULL)
            return;
        p->u.line.gouraud = 1;
        p->u.line.blend = JB_BLEND_ADD;
        p->u.line.v[0] = at(sxy[i], JB_RGB(bi / 4, bi / 3, bi / 2), 0, 0);
        p->u.line.v[1] = at(sxy[j], JB_RGB(bj / 4, bj / 3, bj / 2), 0, 0);
    }

    star.x = (short)(ORBIT * icos(a) / ONE);
    star.y = (short)(30 * isin(a * 2) / ONE);
    star.z = (short)(ORBIT * isin(a) / ONE);
    project(s, &star, 1, &star_sxy, &star_sz);
    size = star_sz < CUBE_Z ? STAR_BIG : STAR_SMALL;
    glow = 104 + 24 * isin(frame * 16) / ONE;
    p = ot_add(s, ot_entry(star_sz), FL_RECT);
    if (p == NULL)
        return;
    p->u.rect.x = JB_GTE_SX(star_sxy) - size / 2;
    p->u.rect.y = JB_GTE_SY(star_sxy) - size / 2;
    p->u.rect.w = p->u.rect.h = size;
    p->u.rect.color = JB_RGB(glow, glow, glow);
    p->u.rect.blend = JB_BLEND_ADD;
    p->u.rect.texture = &s->star;
    p->u.rect.u = size == STAR_BIG ? 0 : STAR_BIG;
}

/* The panel, nearest of all: a darkened box (blend B/2 + F/2), a Gouraud
   border (a polyline), a bar for the cube's turn, and an 8 x 8 light for
   each switch that is on (scale above 1x, precise, perspective, dither). */
static void add_panel(fl_scene *s, unsigned frame)
{
    static const int border[5][2] = {{6, 6}, {105, 6}, {105, 33}, {6, 33}, {6, 6}};
    const int on[4] = {s->sw.scale > 1, s->sw.precise, s->sw.perspective, s->sw.dither};
    fl_prim *p;
    int i;

    p = ot_add(s, 0, FL_POLYLINE);
    if (p != NULL) {
        p->u.polyline.gouraud = 1;
        p->u.polyline.count = 5;
        for (i = 0; i < 5; ++i) {
            p->u.polyline.v[i].x = border[i][0];
            p->u.polyline.v[i].y = border[i][1];
            p->u.polyline.v[i].color = i & 1 ? JB_RGB(255, 200, 80) : JB_RGB(80, 120, 255);
        }
    }
    p = ot_add(s, 0, FL_RECT);
    if (p != NULL) {
        p->u.rect.x = 12, p->u.rect.y = 26, p->u.rect.h = 3;
        p->u.rect.w = (int)((frame * 4) & 1023u) * 88 / 1024;
        p->u.rect.color = JB_RGB(240, 200, 80);
    }
    for (i = 0; i < 4; ++i) {
        p = ot_add(s, 0, FL_RECT);
        if (p == NULL)
            return;
        p->u.rect.x = 12 + i * 12, p->u.rect.y = 12;
        p->u.rect.w = p->u.rect.h = 8;
        p->u.rect.color = on[i] ? JB_RGB(96, 255, 96) : JB_RGB(64, 48, 48);
    }
    /* Added last, so drawn first of its entry: under the rest. */
    p = ot_add(s, 0, FL_RECT);
    if (p != NULL) {
        p->u.rect.x = 6, p->u.rect.y = 6, p->u.rect.w = 100, p->u.rect.h = 28;
        p->u.rect.color = JB_RGB(0, 0, 48);
        p->u.rect.blend = JB_BLEND_HALF;
    }
}

/* ------------------------------------------------------------------------ */
/* The scene */

jb_result fl_scene_init(fl_scene *s, const fl_switches *sw, int threads)
{
    jb_config c;
    jb_result r;

    memset(s, 0, sizeof *s);
    memset(&c, 0, sizeof c);
    c.scale = sw->scale;
    c.threads = threads;
    c.precise_vertices = sw->precise;
    c.perspective_correct = sw->perspective;
    c.log = sw->log;
    c.log_user = sw->log_user;
    s->jb = jb_create(&c, &r);
    if (s->jb == NULL)
        return r;
    s->sw = *sw;

    /* The textures, named as a primitive names them: page, depth, CLUT. */
    s->floor.page_x = FLOOR_PAGE_X, s->floor.depth = JB_TEX_4BIT;
    s->floor.clut_x = FLOOR_CLUT_X, s->floor.clut_y = CLUT_Y;
    s->plasma.page_x = PLASMA_PAGE_X, s->plasma.depth = JB_TEX_8BIT;
    s->plasma.clut_x = 0, s->plasma.clut_y = CLUT_Y + 1;
    s->emblem.page_x = EMBLEM_PAGE_X, s->emblem.depth = JB_TEX_15BIT;
    s->star.page_x = STAR_PAGE_X, s->star.depth = JB_TEX_4BIT;
    s->star.clut_x = STAR_CLUT_X, s->star.clut_y = CLUT_Y;
    if ((r = make_floor(s->jb)) != JB_OK || (r = make_plasma(s->jb)) != JB_OK ||
        (r = make_emblem(s->jb)) != JB_OK || (r = make_star(s->jb)) != JB_OK) {
        fl_scene_free(s);
        return r;
    }

    /* The display: 320 x 240 (GP1(08h), mode 1), enabled once the first
       frame is drawn. */
    note(s, jb_gp1(s->jb, 0x08000001u));
    gte_setup(s);
    if ((r = s->error) != JB_OK)
        fl_scene_free(s);
    return r;
}

jb_result fl_scene_set_switches(fl_scene *s, const fl_switches *sw)
{
    jb_result r;

    if ((r = jb_set_scale(s->jb, sw->scale)) != JB_OK)
        return r;
    jb_set_precise_vertices(s->jb, sw->precise);
    jb_set_perspective_correct(s->jb, sw->perspective);
    s->sw = *sw;
    return JB_OK;
}

jb_result fl_scene_draw(fl_scene *s, unsigned frame)
{
    int top = s->buffer * FL_HEIGHT; /* the back buffer's first row */
    uint16_t clut[256];

    s->error = JB_OK;
    /* This frame's projections are matched with its primitives alone. */
    note(s, jb_gte_begin_frame(s->jb));

    /* Draw into the back buffer: its area and offset, the draw mode's
       dithering, and the texture window repeating every 64 texels (mask
       bits 6 and 7 of u and v, in units of 8 texels: 0xc0 / 8). Every
       texture is 64 texels or smaller at the start of its page, so the
       window changes only the floor's. */
    note(s, jb_set_draw_area(s->jb, 0, top, FL_WIDTH - 1, top + FL_HEIGHT - 1));
    note(s, jb_set_draw_offset(s->jb, 0, top));
    note(s, jb_set_dither(s->jb, s->sw.dither));
    note(s, jb_set_texture_window(s->jb, 0xc0 / 8, 0xc0 / 8, 0, 0));

    /* Clear it (a fill ignores the offset: VRAM coordinates), and cycle the
       plasma's colours by rewriting its CLUT. */
    note(s, jb_vram_fill(s->jb, 0, top, FL_WIDTH, FL_HEIGHT, JB_RGB(12, 10, 20)));
    plasma_palette(frame, clut);
    note(s, jb_vram_load(s->jb, 0, CLUT_Y + 1, 256, 1, clut));

    draw_sky(s, frame);
    ot_clear(s);
    add_floor(s);
    add_shadow(s, frame);
    add_cube(s, frame);
    add_star_and_ring(s, frame);
    add_panel(s, frame);
    ot_draw(s);

    /* Show it: GP1(05h), the display area's top left in VRAM; the first
       time, GP1(03h) too, to enable the display. */
    note(s, jb_gp1(s->jb, 0x05000000u | (uint32_t)top << 10));
    if (!s->shown) {
        note(s, jb_gp1(s->jb, 0x03000000u));
        s->shown = 1;
    }
    s->buffer ^= 1;
    return s->error;
}

const uint8_t *fl_scene_frame(fl_scene *s, int *w, int *h)
{
    return jb_frame(s->jb, w, h);
}

void fl_scene_free(fl_scene *s)
{
    jb_destroy(s->jb);
    s->jb = NULL;
}

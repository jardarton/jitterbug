/*
 * demo-make-sources: the demo's original art, as the files an artist would
 * hand over: PNG images for jb-tex and OBJ models (with their MTL) for
 * jb-mesh. Everything is drawn from its own arithmetic (integer noise and
 * the library's jb_sinf and jb_cosf, the same bits on every machine) and
 * written with integers alone (the OBJ numbers are exact decimals), so the
 * same files come out everywhere, byte for byte; the demo_sources test
 * checks that the files in sources/ are what this writes.
 *
 *   demo-make-sources DIR
 *
 * writes into DIR:
 *   sand.png grass.png rock.png   64 x 64, 16 colours or fewer (4-bit)
 *   water00.png .. water14.png    64 x 64, 15 colours: one ripple pattern,
 *                                 its colours turned one phase on in each
 *                                 image (palette animation frames)
 *   cloud.png                     256 x 64, 15 colours and clear sky
 *   glow.png                      32 x 32, 15 greys and clear
 *   tower.png                     32 x 64, direct colour (15-bit)
 *   planks.png                    64 x 256, 16 colours or fewer (4-bit)
 *   tree.obj stone.obj lighthouse.obj beam.obj pier.obj, and models.mtl
 * Colours are 15-bit ones (each channel's top 5 bits are its value), alpha
 * 255 opaque, 64 semi-transparent (jitterbug's bit 15), 0 clear.
 * examples/demo/README.md, "How the assets are made", says how
 * they become the demo's .jbt and .jbm files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jitterbug.h>

#include "stb_image_write.h"

#define TURN 6.2831853f
#define SN(turns) jb_sinf((turns) * TURN)
#define CS(turns) jb_cosf((turns) * TURN)
#define SEMI 0x8000u

static const char *dir;
static int failed;

static uint32_t mix(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

/* Value noise 0..255 at texel (u, v), lattice points cell texels apart,
   repeating every period texels in u and v (period a multiple of cell). */
static int noise(int u, int v, int cell, int period, uint32_t seed)
{
    int n = period / cell, iu = u / cell, iv = v / cell, fu = u % cell, fv = v % cell;
    int a = (int)(mix((uint32_t)(iu % n) * 7919u + (uint32_t)(iv % n) * 104729u + seed) & 255u);
    int b = (int)(mix((uint32_t)((iu + 1) % n) * 7919u + (uint32_t)(iv % n) * 104729u + seed) & 255u);
    int c = (int)(mix((uint32_t)(iu % n) * 7919u + (uint32_t)((iv + 1) % n) * 104729u + seed) & 255u);
    int d = (int)(mix((uint32_t)((iu + 1) % n) * 7919u + (uint32_t)((iv + 1) % n) * 104729u + seed) &
                  255u);
    int top = a * cell + (b - a) * fu, bottom = c * cell + (d - c) * fu;

    return (top * cell + (bottom - top) * fv) / (cell * cell);
}

/* A 4-bit index from 0..255, clamped. */
static uint8_t idx16(int x)
{
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 15 : x >> 4);
}

/* A palette ramp of n entries from a to b (5-bit channels), with extra bits. */
static void ramp(uint16_t *pal, int n, const int a[3], const int b[3], unsigned bits)
{
    int k;

    for (k = 0; k < n; ++k)
        pal[k] = (uint16_t)(JB_RGB15(a[0] + (b[0] - a[0]) * k / (n - 1),
                                     a[1] + (b[1] - a[1]) * k / (n - 1),
                                     a[2] + (b[2] - a[2]) * k / (n - 1)) |
                            bits);
}

static char *path(const char *name)
{
    static char buf[4096];

    snprintf(buf, sizeof buf, "%s/%s", dir, name);
    return buf;
}

/* Writes w x h 15-bit colours as an RGBA PNG. */
static void png(const char *name, int w, int h, const uint16_t *px)
{
    static uint8_t rgba[256 * 256 * 4];
    int i;

    for (i = 0; i < w * h; ++i) {
        uint16_t c = px[i];
        rgba[4 * i] = (uint8_t)((c & 31u) << 3);
        rgba[4 * i + 1] = (uint8_t)((c >> 5 & 31u) << 3);
        rgba[4 * i + 2] = (uint8_t)((c >> 10 & 31u) << 3);
        rgba[4 * i + 3] = (uint8_t)(c == 0 ? 0 : c & SEMI ? 64 : 255);
    }
    if (!stbi_write_png(path(name), w, h, 4, rgba, w * 4)) {
        fprintf(stderr, "demo-make-sources: cannot write %s\n", path(name));
        failed = 1;
    }
}

/* Indexed texels through a palette, as a PNG. */
static void png_indexed(const char *name, int w, int h, const uint8_t *texels, const uint16_t *pal)
{
    static uint16_t px[256 * 256];
    int i;

    for (i = 0; i < w * h; ++i)
        px[i] = pal[texels[i]];
    png(name, w, h, px);
}

static void textures(void)
{
    static uint8_t t8[256 * 64];
    static uint16_t t15[32 * 64];
    uint16_t pal[16], water[16];
    int u, v, k;

    /* Sand: pale and warm, wind ripples and grains. */
    {
        static const int lo[3] = {17, 14, 8}, hi[3] = {29, 26, 18};
        ramp(pal, 16, lo, hi, 0);
        for (v = 0; v < 64; ++v)
            for (u = 0; u < 64; ++u) {
                float ripple = SN((u + v / 3 + noise(u, v, 16, 64, 3) / 16) / 9.0f);
                int x = 150 + (int)(40 * ripple) + noise(u, v, 4, 64, 4) / 4 - 32 +
                        (int)(mix((uint32_t)(v * 64 + u)) % 33u) - 16;
                t8[v * 64 + u] = idx16(x);
            }
        png_indexed("sand.png", 64, 64, t8, pal);
    }
    /* Grass: clumps of dark and light, blades. */
    {
        static const int lo[3] = {3, 7, 2}, hi[3] = {15, 22, 6};
        ramp(pal, 16, lo, hi, 0);
        for (v = 0; v < 64; ++v)
            for (u = 0; u < 64; ++u) {
                int x = noise(u, v, 16, 64, 5) / 2 + noise(u, v, 4, 64, 6) / 2;
                uint32_t hsh = mix((uint32_t)(v * 64 + u) + 77u);
                if ((hsh & 7u) == 0)
                    x -= 60; /* a shadow between blades */
                else if ((hsh & 15u) == 1)
                    x += 50; /* a lit tip */
                t8[v * 64 + u] = idx16(x);
            }
        png_indexed("grass.png", 64, 64, t8, pal);
    }
    /* Rock: grey-brown strata, cracks. */
    {
        static const int lo[3] = {6, 5, 3}, hi[3] = {23, 21, 16};
        ramp(pal, 16, lo, hi, 0);
        for (v = 0; v < 64; ++v)
            for (u = 0; u < 64; ++u) {
                int n = noise(u, v, 8, 64, 8), x = 70 + noise(u, v, 32, 64, 7) / 2 + n / 3;
                x += (int)(30 * SN((v + n / 20) / 12.0f));
                if (n > 120 && n < 132)
                    x -= 90; /* a crack */
                t8[v * 64 + u] = idx16(x);
            }
        png_indexed("rock.png", 64, 64, t8, pal);
    }
    /* Water: each texel a ripple phase 1..15 from three waves that repeat
       every 64 texels. Phase 1 is the crest, the others a wake fading fast
       behind it, each phase its own colour (blue 29 down to 15), all
       semi-transparent. Image f shows phase k in the colour of phase
       k + f (round 15): the crests move one phase an image. */
    {
        char name[32];
        int f;
        for (v = 0; v < 64; ++v)
            for (u = 0; u < 64; ++u) {
                float p = SN((2 * u + v) / 64.0f) + 0.7f * SN((u - 3 * v) / 64.0f + 0.2f) +
                          0.4f * SN((3 * u + 2 * v) / 64.0f + 0.5f);
                int x = (int)((p + 2.1f) * 15.0f / 4.2f);
                t8[v * 64 + u] = (uint8_t)(1 + (x < 0 ? 0 : x > 14 ? 14 : x));
            }
        water[0] = 0;
        for (k = 1; k <= 15; ++k) {
            int l = 16 - k;
            water[k] = (uint16_t)(JB_RGB15(3 + l * l * l / 211, 9 + l * l * 18 / 225, 14 + l) | SEMI);
        }
        for (f = 0; f < 15; ++f) {
            pal[0] = 0;
            for (k = 1; k <= 15; ++k)
                pal[k] = water[1 + (k - 1 + f) % 15];
            sprintf(name, "water%02d.png", f);
            png_indexed(name, 64, 64, t8, pal);
        }
    }
    /* Cloud: a band 256 texels round (it repeats in u), clear sky or 15
       densities; thin cloud semi-transparent, thick opaque. */
    {
        for (v = 0; v < 64; ++v)
            for (u = 0; u < 256; ++u) {
                int n = noise(u, v, 64, 256, 11) / 2 + noise(u, v, 32, 256, 12) / 3 +
                        noise(u, v, 16, 256, 13) / 6 - 112;
                /* Thinner towards the band's top and bottom rows. */
                int edge = v < 8 ? (8 - v) * 6 : v > 55 ? (v - 55) * 6 : 0;
                n = (n - edge) * 4;
                t8[v * 256 + u] = (uint8_t)(n <= 0 ? 0 : n >= 240 ? 15 : 1 + n / 17);
            }
        pal[0] = 0;
        for (k = 1; k < 16; ++k)
            pal[k] = (uint16_t)(JB_RGB15(14 + k, 14 + k, 15 + k) | (k < 9 ? SEMI : 0u));
        png_indexed("cloud.png", 256, 64, t8, pal);
    }
    /* Glow: round, bright in the middle, fading to nothing; every colour
       semi-transparent, so it adds under JB_BLEND_ADD. */
    {
        for (v = 0; v < 32; ++v)
            for (u = 0; u < 32; ++u) {
                int dx = 2 * u - 31, dy = 2 * v - 31, d2 = dx * dx + dy * dy;
                int f = d2 >= 1024 ? 0 : (1024 - d2) * (1024 - d2) / 4096; /* 0..256 */
                int i = f >= 256 ? 15 : f * 15 / 256 + (f > 0);
                t8[v * 32 + u] = (uint8_t)(i > 15 ? 15 : i);
            }
        pal[0] = 0;
        for (k = 1; k < 16; ++k)
            pal[k] = (uint16_t)(JB_RGB15(31 * k / 15, 31 * k / 15, 31 * k / 15) | SEMI);
        png_indexed("glow.png", 32, 32, t8, pal);
    }
    /* Tower: white and red bands, courses of stone, a window on each white
       band. */
    for (v = 0; v < 64; ++v)
        for (u = 0; u < 32; ++u) {
            int red = (v / 16) & 1, c;
            uint16_t px = red ? JB_RGB15(22, 4, 4) : JB_RGB15(27, 27, 25);
            if ((v & 3) == 3 || ((u + (v / 4 & 1) * 4) & 7) == 0) /* mortar */
                px = red ? JB_RGB15(16, 3, 3) : JB_RGB15(21, 21, 20);
            if (!red && u >= 13 && u <= 18 && (v & 15) >= 5 && (v & 15) <= 11)
                px = JB_RGB15(4, 5, 8); /* a window */
            c = (int)(mix((uint32_t)(v * 32 + u) + 5u) % 3u);
            t15[v * 32 + u] = (uint16_t)(px - (uint16_t)(c * 0x0421u)); /* grain */
        }
    png("tower.png", 32, 64, t15);
    /* Planks: the pier's deck, 64 texels across and 256 along, a plank
       every 16 texels across it, a dark gap between each two, grain along
       the plank and a nail at each end: long straight lines, so that the
       affine texturing of the deck's large quads shows as they bend. */
    {
        static const int lo[3] = {7, 4, 2}, hi[3] = {27, 20, 12};
        ramp(pal, 16, lo, hi, 0);
        for (v = 0; v < 256; ++v)
            for (u = 0; u < 64; ++u) {
                int plank = v / 16, w = v % 16;
                int x = 150 + (int)(mix((uint32_t)plank + 31u) % 41u) - 20 +
                        noise(u, w * 4, 16, 64, 40u + (uint32_t)plank) / 3 - 42 +
                        (int)(mix((uint32_t)(v * 64 + u) + 9u) % 17u) - 8;
                if (w >= 14)
                    x = 8; /* the gap */
                else if (w == 0 || w == 13)
                    x -= 40; /* a plank's edges */
                else if ((u == 4 || u == 59) && (w == 4 || w == 10))
                    x = 30; /* nails */
                t8[v * 64 + u] = idx16(x);
            }
        png_indexed("planks.png", 64, 256, t8, pal);
    }
}

/* --- Models -------------------------------------------------------------- */

/* A model as OBJ will say it: vertices with an optional colour (JB_RGB,
   with_color) and texel coordinates (in the texture of tex_w x tex_h
   texels, for the faces of the material named textured), faces with a
   material's name. */
typedef struct model {
    int n, nf;
    int pos[160][3];
    uint32_t color[160];
    int uv[160][2];
    int face[96][5]; /* vertices, then 3 or 4 indices */
    const char *mat[96];
    int with_color, tex_w, tex_h;
    const char *textured;
} model;

static int vert(model *m, int x, int y, int z, uint32_t color, int u, int v)
{
    int i = m->n++;

    m->pos[i][0] = x, m->pos[i][1] = y, m->pos[i][2] = z;
    m->color[i] = color;
    m->uv[i][0] = u, m->uv[i][1] = v;
    return i;
}

static void face(model *m, const char *mat, int n, int a, int b, int c, int d)
{
    int *f = m->face[m->nf];

    m->mat[m->nf++] = mat;
    f[0] = n, f[1] = a, f[2] = b, f[3] = c, f[4] = d;
}

/* A ring of n vertices at height y, radius r, anticlockwise seen from above
   (+y): x = r sin a, z = r cos a, a growing from start (in turns). */
static int ring(model *m, int n, int r, int y, uint32_t color, float start)
{
    int i, first = m->n;

    for (i = 0; i < n; ++i) {
        float a = start + (float)i / (float)n;
        vert(m, (int)(r * SN(a)), y, (int)(r * CS(a)), color, 0, 0);
    }
    return first;
}

static void cone(model *m, const char *mat, int n, int r, int y0, int y1, uint32_t base,
                 uint32_t tip, float start)
{
    int first = ring(m, n, r, y0, base, start), apex = vert(m, 0, y1, 0, tip, 0, 0), i;

    for (i = 0; i < n; ++i)
        face(m, mat, 3, first + i, first + (i + 1) % n, apex, 0);
}

/* num / den (both positive or num 0) as a decimal, written with integers
   alone: exactly for den a power of two (whose decimals end), else to 9
   places, rounded. */
static void decimal(FILE *fp, long num, long den)
{
    if ((den & (den - 1)) == 0) {
        long rem = num % den;
        fprintf(fp, "%ld", num / den);
        if (rem != 0)
            fputc('.', fp);
        while (rem != 0) {
            rem *= 10;
            fputc('0' + (int)(rem / den), fp);
            rem %= den;
        }
    } else {
        long long scaled = ((long long)num * 1000000000LL + den / 2) / den;
        fprintf(fp, "%lld.%09lld", scaled / 1000000000LL, scaled % 1000000000LL);
    }
}

/* The model as OBJ: positions (with colours), the centres of the texels
   it names (v counted from the bottom, as OBJ does), faces grouped by
   usemtl in their order. */
static void obj(const char *name, const model *m)
{
    FILE *fp = fopen(path(name), "wb");
    const char *cur = NULL;
    int i, k;

    if (fp == NULL) {
        fprintf(stderr, "demo-make-sources: cannot write %s\n", path(name));
        failed = 1;
        return;
    }
    fprintf(fp, "# %s: made by examples/demo/assets/make-sources.c\nmtllib models.mtl\n", name);
    for (i = 0; i < m->n; ++i) {
        fprintf(fp, "v %d %d %d", m->pos[i][0], m->pos[i][1], m->pos[i][2]);
        if (m->with_color)
            for (k = 0; k < 3; ++k)
                fputc(' ', fp), decimal(fp, (long)(m->color[i] >> (8 * k) & 255u), 255);
        fputc('\n', fp);
    }
    if (m->tex_w > 0)
        for (i = 0; i < m->n; ++i) {
            fputs("vt ", fp);
            decimal(fp, 2L * m->uv[i][0] + 1, 2L * m->tex_w);
            fputc(' ', fp);
            decimal(fp, 2L * m->tex_h - (2L * m->uv[i][1] + 1), 2L * m->tex_h);
            fputc('\n', fp);
        }
    for (i = 0; i < m->nf; ++i) {
        const int *f = m->face[i];
        if (cur == NULL || strcmp(cur, m->mat[i]) != 0)
            fprintf(fp, "usemtl %s\n", cur = m->mat[i]);
        fputc('f', fp);
        for (k = 0; k < f[0]; ++k)
            if (m->textured != NULL && strcmp(m->mat[i], m->textured) == 0)
                fprintf(fp, " %d/%d", f[1 + k] + 1, f[1 + k] + 1);
            else
                fprintf(fp, " %d", f[1 + k] + 1);
        fputc('\n', fp);
    }
    if (fclose(fp) != 0) {
        fprintf(stderr, "demo-make-sources: cannot write %s\n", path(name));
        failed = 1;
    }
}

static void models(void)
{
    static model m;

    /* The conifer: a square trunk and two tiers of six-sided cone, flat,
       its colours the vertices'. */
    {
        const uint32_t bark = JB_RGB(96, 64, 44), dark = JB_RGB(44, 92, 40), light = JB_RGB(84, 136, 60);
        int lo, i;
        memset(&m, 0, sizeof m);
        m.with_color = 1;
        lo = ring(&m, 4, 7, 0, bark, 0.125f);
        ring(&m, 4, 7, 40, bark, 0.125f);
        for (i = 0; i < 4; ++i)
            face(&m, "foliage", 4, lo + i, lo + (i + 1) % 4, 4 + lo + (i + 1) % 4, 4 + lo + i);
        cone(&m, "foliage", 6, 46, 28, 124, dark, light, 0.0f);
        cone(&m, "foliage", 6, 33, 84, 172, dark, light, 1.0f / 12.0f);
        obj("tree.obj", &m);
    }
    /* A standing stone: a tapered block, its top slanting (the +x edge 10
       higher), rock on every side. */
    {
        static const int bx = 15, bz = 9, tx = 11, tz = 7, h = 88;
        const int cx[4] = {-1, -1, 1, 1}, cz[4] = {-1, 1, 1, -1}; /* anticlockwise from +y */
        int i, top[4];
        memset(&m, 0, sizeof m);
        m.tex_w = m.tex_h = 64, m.textured = "rock";
        for (i = 0; i < 4; ++i) {
            int j = (i + 1) % 4, w = i & 1 ? 2 * bx : 2 * bz;
            int a = vert(&m, cx[i] * bx, 0, cz[i] * bz, 0, 0, 63);
            int b = vert(&m, cx[j] * bx, 0, cz[j] * bz, 0, w, 63);
            int c = vert(&m, cx[j] * tx, h + (cx[j] > 0) * 10, cz[j] * tz, 0, w - (bx - tx), 0);
            int d = vert(&m, cx[i] * tx, h + (cx[i] > 0) * 10, cz[i] * tz, 0, bx - tx, 0);
            face(&m, "rock", 4, a, b, c, d);
        }
        for (i = 0; i < 4; ++i)
            top[i] = vert(&m, cx[i] * tx, h + (cx[i] > 0) * 10, cz[i] * tz, 0, 8 + (cx[i] > 0) * 22,
                          8 + (cz[i] > 0) * 14);
        face(&m, "rock", 4, top[0], top[1], top[2], top[3]);
        obj("stone.obj", &m);
    }
    /* The lighthouse: a tapering eight-sided tower in painted bands, a
       gallery, a lamp room and a roof. */
    {
        enum { N = 8, R0 = 40, R1 = 27, H = 232, ROOM = 34 };
        int i, centre, room;
        memset(&m, 0, sizeof m);
        m.tex_w = 32, m.tex_h = 64, m.textured = "paint";
        for (i = 0; i < N; ++i) {
            float a0 = (float)i / N, a1 = (float)(i + 1) / N;
            int a = vert(&m, (int)(R0 * SN(a0)), 0, (int)(R0 * CS(a0)), 0, 0, 63);
            int b = vert(&m, (int)(R0 * SN(a1)), 0, (int)(R0 * CS(a1)), 0, 31, 63);
            int c = vert(&m, (int)(R1 * SN(a1)), H, (int)(R1 * CS(a1)), 0, 31, 0);
            int d = vert(&m, (int)(R1 * SN(a0)), H, (int)(R1 * CS(a0)), 0, 0, 0);
            face(&m, "paint", 4, a, b, c, d);
        }
        centre = vert(&m, 0, H, 0, 0, 0, 0);
        ring(&m, N, 38, H, 0, 0.0f);
        for (i = 0; i < N; ++i)
            face(&m, "gallery", 3, centre, centre + 1 + i, centre + 1 + (i + 1) % N, 0);
        room = ring(&m, N, 19, H, 0, 0.0f);
        ring(&m, N, 19, H + ROOM, 0, 0.0f);
        for (i = 0; i < N; ++i)
            face(&m, "lamp", 4, room + i, room + (i + 1) % N, room + N + (i + 1) % N, room + N + i);
        cone(&m, "roof", N, 25, H + ROOM, H + ROOM + 30, 0, 0, 0.0f);
        obj("lighthouse.obj", &m);
    }
    /* The beam: crossed quads from the lamp along -z, bright to black. */
    {
        enum { NEAR = 5, FAR = 64, LEN = 760 };
        const uint32_t lit = JB_RGB(104, 96, 64);
        int a, b, c, d;
        memset(&m, 0, sizeof m);
        m.with_color = 1;
        a = vert(&m, -NEAR, 0, 0, lit, 0, 0), b = vert(&m, NEAR, 0, 0, lit, 0, 0);
        c = vert(&m, FAR, 0, -LEN, 0, 0, 0), d = vert(&m, -FAR, 0, -LEN, 0, 0, 0);
        face(&m, "beam", 4, a, b, c, d);
        a = vert(&m, 0, -NEAR, 0, lit, 0, 0), b = vert(&m, 0, NEAR, 0, lit, 0, 0);
        c = vert(&m, 0, FAR, -LEN, 0, 0, 0), d = vert(&m, 0, -FAR, -LEN, 0, 0, 0);
        face(&m, "beam", 4, a, b, c, d);
        obj("beam.obj", &m);
    }
    /* The pier: a deck 64 wide and 576 long, from the shore at z = 0
       along -z, at y = 0 (the scene sets it on its posts' height), as two
       quads of 288, each the whole planks texture (large on purpose: the
       affine texturing bends their straight planks, and cutting them
       smaller would hide it); and two rows of posts under it, 6 square,
       down 24, the wood untextured. */
    {
        enum { HALF = 32, LEN = 288, POSTS = 7, DOWN = 24 };
        int i, k, side;
        memset(&m, 0, sizeof m);
        m.tex_w = 64, m.tex_h = 256, m.textured = "planks";
        for (k = 0; k < 2; ++k) {
            int z0 = -k * LEN, z1 = -(k + 1) * LEN;
            int a = vert(&m, -HALF, 0, z0, 0, 0, 255), b = vert(&m, HALF, 0, z0, 0, 63, 255);
            int c = vert(&m, HALF, 0, z1, 0, 63, 0), d = vert(&m, -HALF, 0, z1, 0, 0, 0);
            face(&m, "planks", 4, a, b, c, d);
        }
        for (side = -1; side <= 1; side += 2)
            for (i = 0; i < POSTS; ++i) {
                int x = side * (HALF - 3), z = -48 - i * 80, top = m.n;
                /* a ring of 4 at the deck, 4 below, anticlockwise from +y */
                static const int cx[4] = {-3, -3, 3, 3}, cz[4] = {-3, 3, 3, -3};
                for (k = 0; k < 8; ++k)
                    vert(&m, x + cx[k % 4], k < 4 ? 0 : -DOWN, z + cz[k % 4], 0, 0, 0);
                for (k = 0; k < 4; ++k)
                    face(&m, "post", 4, top + 4 + k, top + 4 + (k + 1) % 4, top + (k + 1) % 4, top + k);
            }
        obj("pier.obj", &m);
    }
}

/* The materials' colours: Kd, with 1.0 the texels as they are for a
   textured one (jb-mesh's 128) and full for an untextured one (255). */
static void mtl(void)
{
    static const struct {
        const char *name;
        int r, g, b, den;
    } mats[] = {{"foliage", 255, 255, 255, 255}, {"rock", 176, 170, 160, 128},
                {"paint", 128, 128, 128, 128},   {"gallery", 90, 92, 96, 255},
                {"lamp", 255, 236, 160, 255},    {"roof", 150, 30, 26, 255},
                {"beam", 255, 255, 255, 255},    {"planks", 128, 128, 128, 128},
                {"post", 84, 60, 40, 255}};
    FILE *fp = fopen(path("models.mtl"), "wb");
    size_t i;

    if (fp == NULL) {
        fprintf(stderr, "demo-make-sources: cannot write %s\n", path("models.mtl"));
        failed = 1;
        return;
    }
    fputs("# models.mtl: made by examples/demo/assets/make-sources.c\n", fp);
    for (i = 0; i < sizeof mats / sizeof mats[0]; ++i) {
        fprintf(fp, "newmtl %s\nKd ", mats[i].name);
        decimal(fp, mats[i].r, mats[i].den);
        fputc(' ', fp);
        decimal(fp, mats[i].g, mats[i].den);
        fputc(' ', fp);
        decimal(fp, mats[i].b, mats[i].den);
        fputc('\n', fp);
    }
    if (fclose(fp) != 0)
        failed = 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: demo-make-sources DIR\n");
        return 2;
    }
    dir = argv[1];
    textures();
    models();
    mtl();
    return failed;
}

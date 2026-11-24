/*
 * hires_wide: the scaled pass's two triangle set-ups (src/soft/raster_hires.c,
 * NARROW_G) give the same pixels. The narrow one, in 64 bits, serves every
 * triangle whose gradients are under 2^18 a 1x pixel; the wide one, in
 * 128 bits, the rest, and it is valid for any triangle. This file compiles
 * raster_hires.c a second time with its test hooks: every triangle set up
 * wide (JBI_HIRES_FORCE_WIDE), and the route the library's copy takes
 * counted (JBI_HIRES_ROUTED). Each random triangle is drawn by both copies
 * into two identical scaled VRAMs, and its clipping window compared pixel
 * by pixel; the whole VRAMs are compared after each set.
 *
 * The triangles: small ones about the window, large ones up to the GPU's
 * limits (1023 x 511) crossing it, slivers with integer vertices whose
 * doubled area is a single pixel (the steepest gradients integer vertices
 * can have), and (with precise vertices) slivers a 1/256 pixel thick, with extreme and random colours and texture
 * coordinates; Gouraud, flat, textured at every depth (CLUT cache on and
 * off, texture windows), raw and modulated, semi-transparent, dithered,
 * with the mask bits; at 2x, 3x, 4x and 8x, with integer vertices and with
 * precise ones (up to 1 1/8 pixels off, as gp0.c's PRECISE_SLACK). Every
 * integer-vertex triangle must take the narrow set-up.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long long routed_total, routed_wide;

/* The wide copy. It counts the route the library's copy takes. */
#define JBI_HIRES_FORCE_WIDE 1
#define JBI_HIRES_ROUTED(wide) (++routed_total, routed_wide += (wide))
#define jbi_raster_prim_hires jbi_raster_prim_hires_wide
#include "soft/raster_hires.c"
#undef jbi_raster_prim_hires

/* The library's copy (raster.h's declaration was renamed above). */
void jbi_raster_prim_hires(const jbi_prim *p, const jbi_rows *rows, const jbi_hires *h);

static uint64_t rng = 0x9e3779b97f4a7c15u;

static uint32_t next(void)
{
    uint64_t z = (rng += 0x9e3779b97f4a7c15u);

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebu;
    return (uint32_t)((z ^ (z >> 31)) >> 32);
}

/* lo..hi inclusive */
static int range(int lo, int hi)
{
    return lo + (int)(next() % (uint32_t)(hi - lo + 1));
}

/* An attribute value: often an extreme, else any. */
static int attr(void)
{
    uint32_t k = next() % 4;

    return k == 0 ? 0 : k == 1 ? 255 : range(0, 255);
}

/* x, y with a d - b c = 1 (gcd(a, b) = 1), by Euclid. */
static void bezout(int a, int b, int *x, int *y)
{
    long long r0 = a, r1 = b, s0 = 1, s1 = 0, t0 = 0, t1 = 1;

    while (r1 != 0) {
        long long q = r0 / r1, tmp;

        tmp = r0 - q * r1, r0 = r1, r1 = tmp;
        tmp = s0 - q * s1, s0 = s1, s1 = tmp;
        tmp = t0 - q * t1, t0 = t1, t1 = tmp;
    }
    /* a s0 + b t0 = r0 = +-1 */
    *x = (int)(s0 * r0);
    *y = (int)(t0 * r0);
}

static int too_far_xy(const int *x, const int *y)
{
    int i;

    for (i = 0; i < 3; ++i) {
        int j = (i + 1) % 3, dx = x[i] - x[j], dy = y[i] - y[j];

        if (dx > 1023 || dx < -1023 || dy > 511 || dy < -511)
            return 1;
    }
    return 0;
}

enum { WIN = 48 };

/* A triangle's integer vertices about the window at (wx, wy): kind 0
   small, 1 large, 2 a sliver of doubled area 1. */
static void vertices(int kind, int wx, int wy, int *x, int *y)
{
    int i;

    for (;;) {
        if (kind == 0) {
            for (i = 0; i < 3; ++i) {
                x[i] = wx + range(-40, WIN + 40);
                y[i] = wy + range(-40, WIN + 40);
            }
        } else if (kind == 1) {
            x[0] = wx + range(-1023, WIN + 1023);
            y[0] = wy + range(-511, WIN + 511);
            for (i = 1; i < 3; ++i) {
                x[i] = x[0] + range(-1023, 1023);
                y[i] = y[0] + range(-511, 511);
            }
        } else {
            /* v1 - v0 = (a, b), v2 - v0 = (c, d), a d - b c = 1 */
            int a = range(-1023, 1023), b = range(-511, 511), c, d, k, gx, gy;
            uint32_t lean = next() % 4;

            /* often nearly along an axis, where spans of a few pixels
               step through more than the values' range */
            if (lean == 1)
                b = range(-3, 3);
            else if (lean == 2)
                a = range(-3, 3);

            bezout(a, b, &d, &c); /* a d + b c' = 1, c = -c' */
            c = -c;
            if (a * d - b * c != 1)
                continue;
            /* (c, d) moved along (a, b) to about its middle */
            k = (a != 0 ? (a / 2 - c) / a : (b / 2 - d) / (b != 0 ? b : 1));
            c += k * a;
            d += k * b;
            gx = wx + range(0, WIN);
            gy = wy + range(0, WIN);
            /* through the window */
            x[0] = gx - c;
            y[0] = gy - d;
            x[1] = x[0] + a;
            y[1] = y[0] + b;
            x[2] = gx;
            y[2] = gy;
            if (next() & 1) { /* the other winding */
                int tx = x[1], ty = y[1];
                x[1] = x[2], y[1] = y[2], x[2] = tx, y[2] = ty;
            }
        }
        for (i = 0; i < 3; ++i)
            if (x[i] < -2048 || x[i] > 2046 || y[i] < -2048 || y[i] > 2046)
                break;
        if (i == 3 && !too_far_xy(x, y))
            return;
    }
}

static uint16_t palette[256];

static void random_prim(jbi_prim *p, int precise, int *kind_out)
{
    int x[3], y[3], i, kind = range(0, 5);
    int wx = range(0, JBI_VRAM_W - WIN), wy = range(0, JBI_VRAM_H - WIN);
    uint32_t f = next();

    kind = kind < 2 ? 0 : kind < 4 ? 1 : 2;
    if (precise && kind == 2 && (next() & 1))
        kind = 3;
    *kind_out = kind;
    memset(p, 0, sizeof *p);
    p->kind = JBI_PRIM_TRI;
    p->clip_x0 = (int16_t)wx;
    p->clip_y0 = (int16_t)wy;
    p->clip_x1 = (int16_t)(wx + WIN - 1);
    p->clip_y1 = (int16_t)(wy + WIN - 1);
    p->row0 = 0;
    p->row1 = JBI_VRAM_H - 1;
    vertices(kind == 3 ? 1 : kind, wx, wy, x, y);
    p->flags = (uint8_t)((f & 1 ? JBI_F_GOURAUD : 0) | (f & 2 ? JBI_F_TEXTURED : 0) |
                         ((f & 12) == 12 ? JBI_F_RAW : 0) | (f & 16 ? JBI_F_SEMI : 0));
    p->dither = (uint8_t)((f >> 5) & 1);
    p->semi_mode = (uint8_t)((f >> 6) & 3);
    p->check_mask = (uint8_t)((f >> 8) & 1);
    p->set_mask = (uint16_t)((f >> 9) & 1 ? 0x8000 : 0);
    if (p->flags & JBI_F_TEXTURED) {
        uint32_t m = (f >> 10) & 3;

        p->depth = (uint8_t)(m == 0 ? 4 : m == 1 ? 8 : 15);
        p->tex.tpx = range(0, 15) * 64;
        p->tex.tpy = range(0, 1) * 256;
        p->tex.clut_x = range(0, 63) * 16;
        p->tex.clut_y = range(0, 511);
        p->tex.clut = (f >> 12) & 1 ? palette : NULL;
        if ((f >> 13) & 1) { /* a texture window */
            uint32_t mx = (uint32_t)range(0, 31), my = (uint32_t)range(0, 31);
            uint32_t ox = (uint32_t)range(0, 31), oy = (uint32_t)range(0, 31);

            p->tex.and_x = ~(mx * 8) & 0xff;
            p->tex.and_y = ~(my * 8) & 0xff;
            p->tex.or_x = (ox & mx) * 8;
            p->tex.or_y = (oy & my) * 8;
        } else {
            p->tex.and_x = p->tex.and_y = 0xff;
            p->tex.or_x = p->tex.or_y = 0;
        }
    }
    for (i = 0; i < 3; ++i) {
        jbi_vertex *v = &p->u.tri.v[i];

        v->x = x[i];
        v->y = y[i];
        v->r = attr();
        v->g = attr();
        v->b = attr();
        v->u = attr();
        v->v = attr();
        p->u.tri.qx[i] = x[i] * 256 + (precise ? range(-288, 288) : 0);
        p->u.tri.qy[i] = y[i] * 256 + (precise ? range(-288, 288) : 0);
    }
    if (precise && kind == 3) {
        /* a precise sliver: the third vertex 1/256 pixel off the middle of
           the long edge, which the wide set-up serves */
        p->u.tri.qx[2] = (p->u.tri.qx[0] + p->u.tri.qx[1]) / 2 + 1;
        p->u.tri.qy[2] = (p->u.tri.qy[0] + p->u.tri.qy[1]) / 2;
        p->u.tri.v[2].x = (p->u.tri.qx[2] + 128) / 256;
        p->u.tri.v[2].y = (p->u.tri.qy[2] + 128) / 256;
    }
}

int main(void)
{
    static const int scales[] = {2, 3, 4, 8};
    enum { PER_SET = 3000 };
    uint16_t *vram = malloc((size_t)JBI_VRAM_W * JBI_VRAM_H * 2);
    long long pixels = 0, changed = 0, triangles = 0, failures = 0;
    long long narrow_by_kind[4] = {0, 0, 0, 0}, total_by_kind[4] = {0, 0, 0, 0};
    int si, precise, i;

    if (vram == NULL)
        return 2;
    for (i = 0; i < JBI_VRAM_W * JBI_VRAM_H; ++i)
        vram[i] = (uint16_t)next();
    for (i = 0; i < 256; ++i)
        palette[i] = (uint16_t)next();
    for (si = 0; si < 4; ++si) {
        int n = scales[si], w = JBI_VRAM_W * n, hgt = JBI_VRAM_H * n;
        size_t count = (size_t)w * (size_t)hgt;
        uint16_t *pa = malloc(count * 2), *pb = malloc(count * 2);
        uint16_t *before = malloc((size_t)WIN * n * WIN * n * 2);
        uint8_t *cols = malloc((size_t)w);
        jbi_rows rows = {4, 1, 0};
        jbi_hires ha, hb;

        if (pa == NULL || pb == NULL || before == NULL || cols == NULL)
            return 2;
        for (i = 0; i < w; ++i)
            cols[i] = (uint8_t)((i / n) & 3);
        for (precise = 0; precise < 2; ++precise) {
            long long wide0 = routed_wide, total0 = routed_total;
            int t;

            for (i = 0; i < (int)count; ++i)
                pa[i] = (uint16_t)next();
            memcpy(pb, pa, count * 2);
            ha.pix = pa;
            ha.scale = n;
            ha.width = w;
            ha.dither = 1;
            ha.dither_col = cols;
            ha.vram = vram;
            ha.persp_span = 8;
            hb = ha;
            hb.pix = pb;
            for (t = 0; t < PER_SET; ++t) {
                jbi_prim p;
                int kind, x0, y0, x, y, bad = 0;
                long long w0 = routed_wide, r0 = routed_total;

                random_prim(&p, precise, &kind);
                x0 = p.clip_x0 * n;
                y0 = p.clip_y0 * n;
                for (y = 0; y < WIN * n; ++y)
                    memcpy(&before[(size_t)y * WIN * n], &pa[(size_t)(y0 + y) * w + x0],
                           (size_t)WIN * n * 2);
                jbi_raster_prim_hires(&p, &rows, &ha);
                jbi_raster_prim_hires_wide(&p, &rows, &hb);
                ++triangles;
                if (routed_total != r0) {
                    ++total_by_kind[kind];
                    narrow_by_kind[kind] += routed_wide == w0;
                    if (!precise && routed_wide != w0) {
                        if (++failures <= 20)
                            printf("FAIL %dx: integer-vertex triangle %d set up wide\n", n, t);
                    }
                }
                for (y = 0; y < WIN * n; ++y)
                    for (x = 0; x < WIN * n; ++x) {
                        size_t at = (size_t)(y0 + y) * w + (size_t)(x0 + x);

                        ++pixels;
                        changed += pa[at] != before[(size_t)y * WIN * n + x];
                        if (pa[at] != pb[at] && !bad) {
                            bad = 1;
                            if (++failures <= 20)
                                printf("FAIL %dx precise %d triangle %d (kind %d, flags %d, depth "
                                       "%d): scaled pixel (%d, %d) narrow %04x wide %04x\n",
                                       n, precise, t, kind, p.flags, p.depth, x0 + x, y0 + y,
                                       pa[at], pb[at]);
                        }
                    }
                if (bad) /* go on from equal VRAMs */
                    memcpy(pb, pa, count * 2);
            }
            if (memcmp(pa, pb, count * 2) != 0 && ++failures <= 20)
                printf("FAIL %dx precise %d: the scaled VRAMs differ\n", n, precise);
            printf("%dx precise %d: %d triangles, %lld set up narrow, %lld wide\n", n, precise,
                   PER_SET, (routed_total - total0) - (routed_wide - wide0),
                   routed_wide - wide0);
        }
        free(cols);
        free(before);
        free(pb);
        free(pa);
    }
    printf("set up narrow by kind: small %lld/%lld, large %lld/%lld, integer sliver "
           "%lld/%lld, precise sliver %lld/%lld\n",
           narrow_by_kind[0], total_by_kind[0], narrow_by_kind[1], total_by_kind[1],
           narrow_by_kind[2], total_by_kind[2], narrow_by_kind[3], total_by_kind[3]);
    printf("hires_wide: %lld triangles, %lld pixels compared (%lld drawn), %lld failures\n",
           triangles, pixels, changed, failures);
    free(vram);
    return failures != 0;
}

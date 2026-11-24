/*
 * hires_slivers: thin triangles' colours and texture coordinates in the
 * scaled pass (src/soft/raster_hires.c) against a slow exact evaluation
 * of the rule, pixel by pixel; at 1x the same rule is the 1x pass's.
 *
 * The rule (raster_hires.c, the file's top and hgradient): with vertex
 * positions q in 1/256 1x pixels (whole pixels without precise vertices,
 * and always at 1x), an attribute's gradients per 1x pixel are
 *   gx = trunc(ax * 2^20 / A), gy = trunc(ay * 2^20 / A)
 * (12 fractional bits, truncated toward zero; A the doubled area, ax, ay
 * the attribute's plane normal, all in 1/256 pixels), and scaled pixel
 * (X, Y) at scale S takes
 *   V = a0 + 1/2 + 2^-20 + (gx * dx + gy * dy) / 2^12
 * at dx = (X + 1/2) / S - 1/2 - q0x / 256 (and dy likewise), whose integer
 * part, held to the vertices' least and greatest, is used. At scales that
 * are powers of two the pass computes that exactly; at 3 its steps are
 * rounded down by under 2^-40 a scaled pixel, so it may take the integer
 * part of V less a sliver below 2^-24 instead, which is allowed. Coverage
 * is checked too: a pixel is drawn exactly when its sample point (X / S,
 * Y / S) is inside, or on a left or top edge.
 *
 * The triangles: slivers in every direction (the long edge's slope from
 * flat to steep and back, both ways round), the third vertex a sub-pixel
 * (with precise vertices) or a pixel off the long edge, every vertex
 * order, with Gouraud colours and with raw 15-bit textures whose texels
 * spell u or v (so the 8-bit coordinate is read back whole); slivers
 * 500 and 1000 pixels long leaning every way, where a bent gradient
 * shows; and a few long ones, among them the case below. At 1x, 2x, 3x, 4x
 * and 8x, with precise vertices off and on (the positions come from the
 * GTE: its offset registers place a vertex to 1/256 pixel).
 *
 * The case that failed: (20, 20), (980, 140), (820 + 1/256,
 * 120), black, black and red, at 8x with precise vertices. Gradients held
 * to 2^30 component by component turned (+65280, -522240) per 1x pixel's
 * 2^-12 into (+65280, -262144), so along the covered left edge the red
 * grew past its range and wrapped: red 0 from (276, 52) to (340, 60),
 * where the exact value stays 255.
 *
 * __int128 for the exact evaluation: GCC and Clang (as raster_hires.c).
 */
#include "api_test.h"

__extension__ typedef __int128 i128;
typedef int64_t i64;

static i128 floor_div(i128 n, i128 d) /* d > 0 */
{
    i128 q = n / d;
    return (n % d != 0 && n < 0) ? q - 1 : q;
}

/* ------------------------------------------------------------------------ */
/* Triangles */

enum { PASS_COLOUR, PASS_U, PASS_V, PASSES };

typedef struct {
    int x256[3], y256[3]; /* positions asked of the GTE, 1/256 pixels */
    int r[3], g[3], u[3], v[3];
} tri;

/* The page each texture pass reads, at (x, 256), below everything drawn:
   texel (u, v) spells u (or v) in red (low 5 bits) and green (high 3),
   bit 15 set so 0 is not transparent. */
static const int tex_x[PASSES] = {0, 512, 768};

typedef struct {
    int scale, precise;
    int width, height; /* display, 1x pixels */
    jb_context *jb;
    const uint8_t *frame;
    int fw, fh;
} setup;

/* Fills (0, 0)-(w, h) (one fill covers at most 1023 (rounded up to 1024)
   x 511). */
static void clear(jb_context *jb, int w, int h, uint32_t color)
{
    int y;

    for (y = 0; y < h; y += 256)
        check("jb_vram_fill", jb_vram_fill(jb, 0, y, w > 1023 ? 1023 : w, h - y > 256 ? 256 : h - y,
                                           color),
              JB_OK);
}

static void gte(jb_context *jb, int reg, uint32_t v)
{
    check("jb_gte_write", jb_gte_write(jb, reg, v), JB_OK);
}

/* Vertex at (x256, y256) / 256 through the GTE: an untransformed point at
   depth 256, H 1, so the screen position is the offset (16.16). */
static uint32_t project(jb_context *jb, int x256, int y256)
{
    uint32_t sxy;

    gte(jb, JB_GTE_OFX, (uint32_t)x256 * 256u);
    gte(jb, JB_GTE_OFY, (uint32_t)y256 * 256u);
    check("RTPS", jb_gte_command(jb, JB_GTE_RTPS), JB_OK);
    sxy = jb_gte_read(jb, JB_GTE_SXY2);
    check("SXY x", (int)(sxy & 0xffff), x256 >> 8);
    check("SXY y", (int)(sxy >> 16), y256 >> 8);
    return sxy;
}

static void start(setup *s)
{
    jb_config c;
    static uint16_t page[256 * 256];
    int pass, u, v;

    memset(&c, 0, sizeof c);
    c.scale = s->scale;
    c.width = s->width;
    c.height = s->height;
    c.threads = 2;
    c.precise_vertices = s->precise;
    s->jb = must_create(&c);
    check("GP1(03h)", jb_gp1(s->jb, 0x03000000u), JB_OK);
    check("jb_set_draw_area", jb_set_draw_area(s->jb, 0, 0, 1023, 511), JB_OK);
    gte(s->jb, JB_GTE_TRZ, 256);
    gte(s->jb, JB_GTE_H, 1);
    for (pass = PASS_U; pass <= PASS_V; ++pass) {
        for (v = 0; v < 256; ++v)
            for (u = 0; u < 256; ++u) {
                int t = pass == PASS_U ? u : v;
                page[v * 256 + u] = (uint16_t)(0x8000 | (t & 31) | (t >> 5) << 5);
            }
        check("jb_vram_load", jb_vram_load(s->jb, tex_x[pass], 256, 256, 256, page), JB_OK);
    }
}

static void draw(setup *s, const tri *t, int pass)
{
    uint32_t w[9], p[3];
    int i, n = 0;

    for (i = 0; i < 3; ++i)
        p[i] = project(s->jb, t->x256[i], t->y256[i]);
    if (pass == PASS_COLOUR) {
        for (i = 0; i < 3; ++i) {
            uint32_t c = (uint32_t)t->r[i] | (uint32_t)t->g[i] << 8;
            w[n++] = i == 0 ? 0x30000000u | c : c;
            w[n++] = p[i];
        }
    } else {
        uint32_t page = (uint32_t)tex_x[pass] / 64 | 1u << 4 | 2u << 7;

        w[n++] = 0x25000000u; /* textured, raw */
        for (i = 0; i < 3; ++i) {
            w[n++] = p[i];
            w[n++] = (uint32_t)t->u[i] | (uint32_t)t->v[i] << 8 | (i == 1 ? page << 16 : 0);
        }
    }
    gp0(s->jb, w, (size_t)n);
}

/* The positions the pass uses, 1/256 pixels. */
static void positions(const setup *s, const tri *t, i64 *qx, i64 *qy)
{
    int i;

    for (i = 0; i < 3; ++i) {
        int sub = s->precise && s->scale > 1;
        qx[i] = sub ? t->x256[i] : (t->x256[i] >> 8) * 256;
        qy[i] = sub ? t->y256[i] : (t->y256[i] >> 8) * 256;
    }
}

static int held(i128 v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : (int)v;
}

/* The attribute's value at scaled pixel (X, Y), exactly (the top);
   *alt is the other value allowed at a scale that is not a power of
   two. */
static int value(const setup *s, const i64 *qx, const i64 *qy, const int *a, i64 area, int X,
                 int Y, int *alt)
{
    i64 S = s->scale;
    i64 ax = (i64)(a[1] - a[0]) * (qy[2] - qy[0]) - (i64)(a[2] - a[0]) * (qy[1] - qy[0]);
    i64 ay = (i64)(a[2] - a[0]) * (qx[1] - qx[0]) - (i64)(a[1] - a[0]) * (qx[2] - qx[0]);
    i64 gx = ax * ((i64)1 << 20) / area, gy = ay * ((i64)1 << 20) / area;
    /* dx, dy in 1/(256 S) pixels, V in 1/(2^20 S) */
    i64 dx = (i64)X * 256 + 128 - 128 * S - qx[0] * S;
    i64 dy = (i64)Y * 256 + 128 - 128 * S - qy[0] * S;
    i128 D = (i128)S << 20;
    i128 num = (i128)(2 * a[0] + 1) * ((i128)1 << 19) * S + (i128)gx * dx + (i128)gy * dy;
    int lo = a[0], hi = a[0], k, v;

    for (k = 1; k < 3; ++k) {
        lo = a[k] < lo ? a[k] : lo;
        hi = a[k] > hi ? a[k] : hi;
    }
    num += S; /* 2^-20 */
    v = held(floor_div(num, D), lo, hi);
    *alt = (S & (S - 1)) != 0 ? held(floor_div(num - 1, D), lo, hi) : v;
    return v;
}

/* Whether scaled pixel (X, Y)'s sample point is covered: inside, or on a
   left or top edge (the interior to its right, or below a horizontal
   one). */
static int covered(const setup *s, const i64 *qx, const i64 *qy, i64 area, int X, int Y)
{
    i64 px = (i64)X * 256, py = (i64)Y * 256;
    int i, sign = area > 0 ? 1 : -1;

    for (i = 0; i < 3; ++i) {
        int j = (i + 1) % 3;
        i64 ex = (qx[j] - qx[i]) * s->scale, ey = (qy[j] - qy[i]) * s->scale;
        i64 e = ex * (py - qy[i] * s->scale) - ey * (px - qx[i] * s->scale);

        if (sign * (e > 0 ? 1 : e < 0 ? -1 : 0) > 0)
            continue;
        if (e == 0 && (sign * -ey > 0 || (ey == 0 && sign * ex > 0)))
            continue;
        return 0;
    }
    return 1;
}

static long long pixels_checked, values_checked;

/* Every scaled pixel in (x0, y0)-(x1, y1), 1x pixels inclusive, against
   triangle t drawn in pass `pass` on the background (blue 31). */
static void check_tri(const setup *s, const tri *t, int pass, int x0, int y0, int x1, int y1,
                      const char *name)
{
    i64 qx[3], qy[3], area;
    int X, Y, S = s->scale, bad = 0;
    char what[200];

    positions(s, t, qx, qy);
    area = (qx[1] - qx[0]) * (qy[2] - qy[0]) - (qx[2] - qx[0]) * (qy[1] - qy[0]);
    for (Y = y0 * S; Y < (y1 + 1) * S; ++Y)
        for (X = x0 * S; X < (x1 + 1) * S; ++X) {
            const uint8_t *f = s->frame + ((size_t)Y * (size_t)s->fw + (size_t)X) * 4;
            int r = f[0] >> 3, g = f[1] >> 3, b = f[2] >> 3;
            int in = area != 0 && covered(s, qx, qy, area, X, Y);

            ++pixels_checked;
            if (in != (b != 31)) {
                if (bad++ < 4) {
                    snprintf(what, sizeof what, "%s %dx precise %d pass %d: (%d, %d) drawn",
                             name, S, s->precise, pass, X, Y);
                    check(what, b != 31, in);
                }
                continue;
            }
            if (!in)
                continue;
            ++values_checked;
            if (pass == PASS_COLOUR) {
                int ar, ag, vr = value(s, qx, qy, t->r, area, X, Y, &ar);
                int vg = value(s, qx, qy, t->g, area, X, Y, &ag);

                if (((r != vr >> 3 && r != ar >> 3) || (g != vg >> 3 && g != ag >> 3)) &&
                    bad++ < 4) {
                    snprintf(what, sizeof what, "%s %dx precise %d: (%d, %d) red, green >> 3",
                             name, S, s->precise, X, Y);
                    check(what, r << 8 | g, (vr >> 3) << 8 | vg >> 3);
                }
            } else {
                int alt, got = r | g << 5;
                int want = value(s, qx, qy, pass == PASS_U ? t->u : t->v, area, X, Y, &alt);

                if (got != want && got != alt && bad++ < 4) {
                    snprintf(what, sizeof what, "%s %dx precise %d: (%d, %d) %c", name, S,
                             s->precise, X, Y, pass == PASS_U ? 'u' : 'v');
                    check(what, got, want);
                }
            }
        }
    ++api_t.checks;
    if (bad) {
        ++api_t.failures;
        printf("FAIL %s %dx precise %d pass %d: %d pixels wrong\n", name, S, s->precise, pass,
               bad);
    }
}

static void frame(setup *s)
{
    s->frame = jb_frame(s->jb, &s->fw, &s->fh);
    if (s->frame == NULL || s->fw != s->width * s->scale || s->fh != s->height * s->scale) {
        printf("FAIL jb_frame\n");
        exit(2);
    }
}

/* ------------------------------------------------------------------------ */
/* The sweep */

static uint32_t rng = 12345;

static int pick(int n)
{
    rng = rng * 1103515245u + 12345u;
    return (int)((rng >> 8) % (uint32_t)n);
}

/* Cells of 80 x 60 pixels in a 320 x 240 display: 16 triangles a frame. */
enum { CELL_W = 80, CELL_H = 60, CELLS_X = 4, CELLS_Y = 4, PER_FRAME = CELLS_X * CELLS_Y };

/* Sliver k of the sweep in cell (cx, cy): the long edge from A to B, the
   third vertex at a fraction of the way plus a small offset, in a vertex
   order and with attributes from k. */
static void sliver(int k, int precise, int cx, int cy, tri *t)
{
    /* The long edge's direction: 26 from flat to steep, mirrored 4 ways. */
    static const int dir[][2] = {
        {72, 0}, {72, 1}, {72, 2}, {72, 4}, {72, 7}, {72, 12}, {72, 18}, {72, 26}, {72, 36},
        {72, 47}, {72, 52}, {60, 52}, {48, 52}, {36, 52}, {26, 52}, {18, 52}, {12, 52},
        {8, 52}, {5, 52}, {3, 52}, {2, 52}, {1, 52}, {0, 52}, {1, 51}, {3, 49}, {70, 3},
    };
    /* The third vertex's offset from the long edge, 1/256 pixels. */
    static const int off[][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}, {1, -1}, {-2, 1},
                                 {3, 5},  {64, 0}, {0, -128}, {256, 0}, {0, 256}};
    static const int perm[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1},
                                   {2, 1, 0}};
    int nd = (int)(sizeof dir / sizeof dir[0]), no = (int)(sizeof off / sizeof off[0]);
    int d = k % nd, flip = (k / nd) & 3, o = (k / 2 + k / nd) % no;
    int dx = dir[d][0], dy = dir[d][1];
    int ax = (cx * CELL_W + 4) * 256, ay = (cy * CELL_H + 4) * 256;
    int px[3], py[3], i, order = k % 6, num = 1 + pick(7), ox, oy;

    if (flip & 1) { /* mirrored in x: from the cell's right */
        dx = -dx;
        ax += 72 * 256;
    }
    if (flip & 2) { /* in y: from its bottom */
        dy = -dy;
        ay += 52 * 256;
    }
    if (precise) {
        ax += pick(256);
        ay += pick(256);
        ox = off[o][0];
        oy = off[o][1];
    } else {
        /* a whole pixel off */
        ox = off[o][0] > 0 ? 256 : off[o][0] < 0 ? -256 : 0;
        oy = off[o][1] > 0 ? 256 : off[o][1] < 0 ? -256 : 0;
    }
    px[0] = ax;
    py[0] = ay;
    px[1] = ax + dx * 256;
    py[1] = ay + dy * 256;
    px[2] = ax + dx * 256 * num / 8 + ox;
    py[2] = ay + dy * 256 * num / 8 + oy;
    if (!precise) {
        px[2] = px[2] / 256 * 256;
        py[2] = py[2] / 256 * 256;
    }
    for (i = 0; i < 3; ++i) {
        int j = perm[order][i];
        t->x256[i] = px[j];
        t->y256[i] = py[j];
        /* extremes often, so the range clamps */
        t->r[i] = pick(3) == 0 ? (pick(2) ? 255 : 0) : pick(256);
        t->g[i] = pick(256);
        t->u[i] = pick(3) == 0 ? (pick(2) ? 255 : 0) : pick(256);
        t->v[i] = pick(256);
    }
}

static void sweep(int scale, int precise, int count)
{
    setup s;
    tri t[PER_FRAME];
    int base, pass, i;

    memset(&s, 0, sizeof s);
    s.scale = scale;
    s.precise = precise;
    s.width = CELL_W * CELLS_X;
    s.height = CELL_H * CELLS_Y;
    start(&s);
    for (base = 0; base < count; base += PER_FRAME) {
        for (i = 0; i < PER_FRAME; ++i)
            sliver(base + i, precise, i % CELLS_X, i / CELLS_X, &t[i]);
        for (pass = 0; pass < PASSES; ++pass) {
            clear(s.jb, s.width, s.height, 0xff0000u);
            for (i = 0; i < PER_FRAME && base + i < count; ++i)
                draw(&s, &t[i], pass);
            frame(&s);
            for (i = 0; i < PER_FRAME && base + i < count; ++i) {
                int cx = i % CELLS_X, cy = i / CELLS_X;
                check_tri(&s, &t[i], pass, cx * CELL_W, cy * CELL_H, cx * CELL_W + CELL_W - 1,
                          cy * CELL_H + CELL_H - 1, "sliver");
            }
        }
    }
    jb_destroy(s.jb);
}

/* Long slivers, 8 a frame: along x on a 1024 x 160 display in bands of
   20 rows, or along y on a 160 x 512 one in columns of 20. A gradient
   bent at the long edge shows over its length (as in that case). */
static void long_sweep(int scale, int precise, int along_y)
{
    static const int lean[] = {0, 1, 2, 3, 5, 8, 12, 16};
    static const int off[][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}, {2, -1}, {-3, 2}, {256, 0},
                                 {0, -256}};
    setup s;
    tri t[8];
    int f, pass, i, k;

    memset(&s, 0, sizeof s);
    s.scale = scale;
    s.precise = precise;
    s.width = along_y ? 160 : 1024;
    s.height = along_y ? 512 : 160;
    start(&s);
    for (f = 0; f < 4; ++f) {
        int box[8][4];

        for (i = 0; i < 8; ++i) {
            int j = f * 8 + i, d = lean[i] * ((f & 1) ? -1 : 1), o = (j * 3 + f) % 8, num;
            int L = along_y ? 500 : 1000, a0, a1, b0, b1, ox, oy;

            /* A at the band's start, B along it, leaning by d */
            if (along_y) {
                a0 = (20 * i + 2 + (d < 0 ? 16 : 0)) * 256;
                a1 = 4 * 256;
                b0 = a0 + d * 256;
                b1 = a1 + L * 256;
            } else {
                a0 = 8 * 256;
                a1 = (20 * i + 2 + (d < 0 ? 16 : 0)) * 256;
                b0 = a0 + L * 256;
                b1 = a1 + d * 256;
            }
            if (precise) {
                a0 += pick(256);
                a1 += pick(256);
                b0 += pick(256);
                b1 += pick(256);
            }
            ox = precise ? off[o][0] : off[o][0] > 0 ? 256 : off[o][0] < 0 ? -256 : 0;
            oy = precise ? off[o][1] : off[o][1] > 0 ? 256 : off[o][1] < 0 ? -256 : 0;
            num = 1 + pick(7);
            t[i].x256[0] = a0;
            t[i].y256[0] = a1;
            t[i].x256[1] = b0;
            t[i].y256[1] = b1;
            t[i].x256[2] = a0 + (int)((long long)(b0 - a0) * num / 8) + ox;
            t[i].y256[2] = a1 + (int)((long long)(b1 - a1) * num / 8) + oy;
            if (!precise) {
                t[i].x256[2] = t[i].x256[2] / 256 * 256;
                t[i].y256[2] = t[i].y256[2] / 256 * 256;
            }
            for (k = 0; k < 3; ++k) {
                t[i].r[k] = pick(2) ? 255 : pick(256);
                t[i].g[k] = pick(256);
                t[i].u[k] = pick(2) ? 0 : pick(256);
                t[i].v[k] = pick(256);
            }
            if (along_y) {
                box[i][0] = 20 * i;
                box[i][1] = 0;
                box[i][2] = 20 * i + 19;
                box[i][3] = 511;
            } else {
                box[i][0] = 0;
                box[i][1] = 20 * i;
                box[i][2] = 1023;
                box[i][3] = 20 * i + 19;
            }
        }
        for (pass = 0; pass < PASSES; ++pass) {
            clear(s.jb, s.width, s.height, 0xff0000u);
            for (i = 0; i < 8; ++i)
                draw(&s, &t[i], pass);
            frame(&s);
            for (i = 0; i < 8; ++i)
                check_tri(&s, &t[i], pass, box[i][0], box[i][1], box[i][2], box[i][3],
                          along_y ? "long, along y" : "long, along x");
        }
    }
    jb_destroy(s.jb);
}

/* Long triangles, one a frame, on a 1024 x 160 display. */
static void long_ones(int scale, int precise)
{
    static const int v[][6] = {
        /* the case that failed, in 1/256 pixels */
        {20 * 256, 20 * 256, 980 * 256, 140 * 256, 820 * 256 + 1, 120 * 256},
        /* a whole-pixel sliver of area 1/2 along x: a gradient near 2^30,
           the most a whole-pixel triangle has */
        {10 * 256, 10 * 256, 1011 * 256, 11 * 256, 1010 * 256, 11 * 256},
        /* steep and long, a sub-pixel off */
        {500 * 256, 2 * 256, 530 * 256, 152 * 256, 515 * 256 + 3, 77 * 256},
        /* along x = 2y */
        {100 * 256, 5 * 256, 400 * 256, 155 * 256, 250 * 256 - 2, 80 * 256 + 1},
    };
    setup s;
    int k, pass, i;

    memset(&s, 0, sizeof s);
    s.scale = scale;
    s.precise = precise;
    s.width = 1024;
    s.height = 160;
    start(&s);
    for (k = 0; k < (int)(sizeof v / sizeof v[0]); ++k)
        for (pass = 0; pass < PASSES; ++pass) {
            tri t;

            for (i = 0; i < 3; ++i) {
                t.x256[i] = v[k][2 * i];
                t.y256[i] = v[k][2 * i + 1];
                t.r[i] = i == 2 ? 255 : 0;
                t.g[i] = i == 1 ? 255 : 0;
                t.u[i] = i == 2 ? 255 : 0;
                t.v[i] = i == 1 ? 255 : 3;
            }
            clear(s.jb, 1024, 160, 0xff0000u);
            draw(&s, &t, pass);
            frame(&s);
            {
                /* its bounding box and a pixel round it */
                int x0 = 1023, y0 = 159, x1 = 0, y1 = 0;
                for (i = 0; i < 3; ++i) {
                    int x = t.x256[i] >> 8, y = t.y256[i] >> 8;
                    x0 = x - 2 < x0 ? x - 2 : x0;
                    y0 = y - 2 < y0 ? y - 2 : y0;
                    x1 = x + 2 > x1 ? x + 2 : x1;
                    y1 = y + 2 > y1 ? y + 2 : y1;
                }
                x0 = x0 < 0 ? 0 : x0;
                y0 = y0 < 0 ? 0 : y0;
                x1 = x1 > 1023 ? 1023 : x1;
                y1 = y1 > 159 ? 159 : y1;
                check_tri(&s, &t, pass, x0, y0, x1, y1, k == 0 ? "long ones: bent gradient" : "long");
            }
            if (k == 0 && pass == PASS_COLOUR && precise && scale == 8) {
                /* Its samples: (20 + 8 (y - 20), y), at the
                   corner of their 8 x 8 blocks. */
                int y;
                for (y = 50; y <= 60; ++y) {
                    int x = 20 + 8 * (y - 20);
                    const uint8_t *f =
                        s.frame + ((size_t)y * 8 * (size_t)s.fw + (size_t)x * 8) * 4;
                    check("long ones: bent gradient: red on the left edge", f[0], 255);
                }
            }
        }
    jb_destroy(s.jb);
}

int main(void)
{
    static const int scales[] = {1, 2, 3, 4, 8};
    int si, precise;

    for (si = 0; si < 5; ++si)
        for (precise = 0; precise < 2; ++precise) {
            int scale = scales[si];
            /* 26 directions x 4 mirrorings x 2; half at 8x */
            sweep(scale, precise, scale == 8 ? 112 : 208);
            long_sweep(scale, precise, 0);
            long_sweep(scale, precise, 1);
            if (scale > 1)
                long_ones(scale, precise);
        }
    printf("hires_slivers: %lld pixels, %lld values\n", pixels_checked, values_checked);
    return finish("hires_slivers");
}

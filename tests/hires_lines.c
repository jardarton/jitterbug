/*
 * hires_lines: every pixel of lines in all eight octants against a slow
 * exact evaluation of the line rules, at 1x and in the scaled pass at 2x,
 * 3x, 4x and 8x, each line drawn from either end; and the 1x VRAM of every
 * scaled context against the 1x rule, which the console's dumps pin
 * (src/soft/raster.c, raster_line), unchanged by the scaled pass.
 *
 * The 1x rule: one pixel per step along the major axis (x when |dx| >
 * |dy|), both ends drawn; at step i of n the minor coordinate is the
 * start's plus i * d / n, rounded to the nearest, an exact half to the
 * larger row along x and the smaller column along y.
 *
 * The scaled rule (src/soft/raster_hires.c, the file's top and hi_line):
 * with the end points P0, P1 at S times their positions (1/256 pixels;
 * precise ones with precise vertices), a band of S pixels across the major
 * axis at every major pixel m from the first end's S-pixel block to the
 * last's: from ms = round(P0) to me = round(P1) + S - 1 (ends sorted along
 * the major axis, a half up), the minor coordinate is P0's plus (P1 - P0)
 * times (2 (m - ms) - (S - 1)) / (2 (me - ms - (S - 1))) held to [0, 1],
 * and the band starts at the pixel nearest to it, an exact half as at 1x.
 * Here the nearest is found by comparing exact remainders, not as the pass
 * does it.
 *
 * The case that failed: at 3x the line from (100, 0) to
 * (101, 511) started its bands at scaled x 300 on scaled rows 257 and 258,
 * where the minor coordinate is 300 + 256/511 and 300 + 257/511, so 301:
 * the pass rounded the coordinate down to 1/256 before adding its half,
 * which made all of [1/2, 1/2 + 1/256) a tie.
 *
 * __int128 is not needed; 64 bits hold the products (the pass's bounds).
 */
#include "api_test.h"

typedef int64_t i64;

static i64 floor_div(i64 n, i64 d) /* d > 0 */
{
    i64 q = n / d;
    return (n % d != 0 && n < 0) ? q - 1 : q;
}

typedef struct {
    int x256[2], y256[2]; /* end points, 1/256 pixels */
} line;

typedef struct {
    int scale, precise, width, height;
    jb_context *jb;
    const uint8_t *frame;
    int fw, fh;
} setup;

static long long lines_checked, pixels_checked;

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

static uint32_t project(jb_context *jb, int x256, int y256)
{
    gte(jb, JB_GTE_OFX, (uint32_t)x256 * 256u);
    gte(jb, JB_GTE_OFY, (uint32_t)y256 * 256u);
    check("RTPS", jb_gte_command(jb, JB_GTE_RTPS), JB_OK);
    return jb_gte_read(jb, JB_GTE_SXY2);
}

static void start(setup *s)
{
    jb_config c;

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
}

/* A white flat line from end `from` to the other. */
static void draw(setup *s, const line *l, int from)
{
    uint32_t w[3];
    int a = from, b = 1 - from;

    w[0] = 0x40ffffffu;
    w[1] = project(s->jb, l->x256[a], l->y256[a]);
    w[2] = project(s->jb, l->x256[b], l->y256[b]);
    gp0(s->jb, w, 3);
}

/* ------------------------------------------------------------------------ */
/* The rules: each marks the pixels it draws in map (w x h from (ox, oy),
   pixels of its pass). */

typedef struct {
    int ox, oy, w, h;
    int clip_w, clip_h; /* the drawing area, (0, 0) to all of VRAM */
    unsigned char *map;
} region;

static void mark(region *r, int x, int y)
{
    if (x < 0 || y < 0 || x >= r->clip_w || y >= r->clip_h)
        return; /* outside the drawing area */
    if (x >= r->ox && x < r->ox + r->w && y >= r->oy && y < r->oy + r->h)
        r->map[(size_t)(y - r->oy) * (size_t)r->w + (size_t)(x - r->ox)] = 1;
    else
        r->map[(size_t)r->w * (size_t)r->h] = 1; /* outside: flagged */
}

/* round(num / n), n > 0: an exact half up, or down. */
static int nearest_1x(int num, int n, int half_up)
{
    int k = (int)floor_div(num, n), rem = num - k * n; /* 0 <= rem < n */

    if (2 * rem > n || (2 * rem == n && half_up))
        ++k;
    return k;
}

static void rule_1x(const line *l, region *r)
{
    int x0 = l->x256[0] >> 8, y0 = l->y256[0] >> 8, x1 = l->x256[1] >> 8, y1 = l->y256[1] >> 8;
    int dx = x1 - x0, dy = y1 - y0, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady, i;

    if (steps == 0) {
        mark(r, x0, y0);
        return;
    }
    for (i = 0; i <= steps; ++i) {
        if (adx > ady)
            mark(r, x0 + (dx < 0 ? -i : i), y0 + nearest_1x(i * dy, steps, 1));
        else
            mark(r, x0 + nearest_1x(i * dx, steps, 0), y0 + (dy < 0 ? -i : i));
    }
}

static void rule_scaled(const setup *s, const line *l, region *r)
{
    i64 n = s->scale, p0[2], p1[2], len, m, ms, me;
    int major, k;

    for (k = 0; k < 2; ++k) {
        i64 a = k ? l->y256[0] : l->x256[0], b = k ? l->y256[1] : l->x256[1];

        if (!s->precise) { /* whole pixels */
            a = floor_div(a, 256) * 256;
            b = floor_div(b, 256) * 256;
        }
        p0[k] = a * n;
        p1[k] = b * n;
    }
    {
        i64 ax = p1[0] - p0[0], ay = p1[1] - p0[1];
        major = (ax < 0 ? -ax : ax) > (ay < 0 ? -ay : ay) ? 0 : 1;
    }
    if (p1[major] < p0[major])
        for (k = 0; k < 2; ++k) {
            i64 t = p0[k];
            p0[k] = p1[k];
            p1[k] = t;
        }
    ms = floor_div(p0[major] + 128, 256);
    me = floor_div(p1[major] + 128, 256) + n - 1;
    len = me - ms - (n - 1);
    for (m = ms; m <= me; ++m) {
        i64 num = 2 * (m - ms) - (n - 1), den = 2 * len, c, unit, q, rem;

        /* c / den is the minor coordinate, 1/256 pixels */
        if (den <= 0 || num <= 0) {
            c = p0[major ^ 1];
            den = 1;
        } else if (num >= den) {
            c = p1[major ^ 1];
            den = 1;
        } else {
            c = p0[major ^ 1] * den + (p1[major ^ 1] - p0[major ^ 1]) * num;
        }
        unit = 256 * den;
        q = floor_div(c, unit);
        rem = c - q * unit; /* 0 <= rem < unit */
        if (2 * rem > unit || (2 * rem == unit && major == 0))
            ++q;
        for (k = 0; k < n; ++k) {
            if (major == 0)
                mark(r, (int)m, (int)q + k);
            else
                mark(r, (int)q + k, (int)m);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Checks */

static void compare(const char *name, const setup *s, const line *l, int from, region *want,
                    const uint16_t *got1x, int is_1x_vram)
{
    int x, y, bad = 0, S = is_1x_vram ? 1 : s->scale;
    char what[200];

    if (want->map[(size_t)want->w * (size_t)want->h]) {
        printf("FAIL %s %dx%s precise %d, (%d, %d)/256-(%d, %d)/256: the rule leaves the region "
               "checked\n",
               name, S, is_1x_vram ? " (its 1x VRAM)" : "", s->precise, l->x256[0], l->y256[0],
               l->x256[1], l->y256[1]);
        ++api_t.failures;
        return;
    }
    for (y = 0; y < want->h; ++y)
        for (x = 0; x < want->w; ++x) {
            int gx = want->ox + x, gy = want->oy + y, drawn;

            if (is_1x_vram)
                drawn = got1x[(size_t)y * (size_t)want->w + (size_t)x] != 0;
            else
                drawn = s->frame[((size_t)gy * (size_t)s->fw + (size_t)gx) * 4] != 0;
            ++pixels_checked;
            if (drawn != want->map[(size_t)y * (size_t)want->w + (size_t)x] && bad++ < 4) {
                snprintf(what, sizeof what,
                         "%s %dx%s precise %d, (%d, %d)/256-(%d, %d)/256 from end %d: (%d, %d)",
                         name, S, is_1x_vram ? " (its 1x VRAM)" : "", s->precise, l->x256[0],
                         l->y256[0], l->x256[1], l->y256[1], from, gx, gy);
                check(what, drawn, want->map[(size_t)y * (size_t)want->w + (size_t)x]);
            }
        }
    ++api_t.checks;
    if (bad) {
        ++api_t.failures;
        printf("FAIL %s %dx%s precise %d from end %d: %d pixels wrong\n", name, S,
               is_1x_vram ? " (its 1x VRAM)" : "", s->precise, from, bad);
    }
}

/* Lines drawn on one frame from end `from`, each checked in its box (1x
   pixels, inclusive) in the scaled frame and in the 1x VRAM. */
static void batch(setup *s, const line *l, int (*box)[4], int count, int from,
                  const char *name)
{
    /* on the heap: wasm's initial memory would not hold them */
    enum { MAP = 2048 * 4200 + 1 };
    static unsigned char *map;
    static uint16_t *vram;
    int i, S = s->scale;

    if (map == NULL) {
        map = must_alloc(MAP);
        vram = must_alloc(1024 * 512 * sizeof *vram);
    }

    clear(s->jb, s->width, s->height, 0);
    for (i = 0; i < count; ++i)
        draw(s, &l[i], from);
    s->frame = jb_frame(s->jb, &s->fw, &s->fh);
    if (s->frame == NULL || s->fw != s->width * S || s->fh != s->height * S) {
        printf("FAIL jb_frame\n");
        exit(2);
    }
    for (i = 0; i < count; ++i) {
        region r;

        r.ox = box[i][0] * S;
        r.oy = box[i][1] * S;
        r.w = (box[i][2] - box[i][0] + 1) * S;
        r.h = (box[i][3] - box[i][1] + 1) * S;
        r.map = map;
        r.clip_w = 1024 * S;
        r.clip_h = 512 * S;
        if ((size_t)r.w * (size_t)r.h >= MAP) {
            printf("FAIL %s: box too large\n", name);
            exit(2);
        }
        memset(map, 0, (size_t)r.w * (size_t)r.h + 1);
        if (S == 1)
            rule_1x(&l[i], &r);
        else
            rule_scaled(s, &l[i], &r);
        compare(name, s, &l[i], from, &r, NULL, 0);
        ++lines_checked;
        /* The 1x VRAM, by the 1x rule at every scale. */
        r.ox = box[i][0];
        r.oy = box[i][1];
        r.w = box[i][2] - box[i][0] + 1;
        r.h = box[i][3] - box[i][1] + 1;
        r.clip_w = 1024;
        r.clip_h = 512;
        memset(map, 0, (size_t)r.w * (size_t)r.h + 1);
        rule_1x(&l[i], &r);
        check("jb_vram_read", jb_vram_read(s->jb, r.ox, r.oy, r.w, r.h, vram), JB_OK);
        compare(name, s, &l[i], from, &r, vram, 1);
    }
}

/* ------------------------------------------------------------------------ */
/* The lines */

static uint32_t rng = 777;

static int pick(int n)
{
    rng = rng * 1103515245u + 12345u;
    return (int)((rng >> 8) % (uint32_t)n);
}

/* A line in 1/256 pixels from whole-pixel (x, y) by (dx, dy) pixels, with
   sub-pixel jitter when precise. */
static line make(int x, int y, int dx, int dy, int precise)
{
    line l;

    l.x256[0] = x * 256 + (precise ? pick(256) : 0);
    l.y256[0] = y * 256 + (precise ? pick(256) : 0);
    l.x256[1] = (x + dx) * 256 + (precise ? pick(256) : 0);
    l.y256[1] = (y + dy) * 256 + (precise ? pick(256) : 0);
    return l;
}

/* Steep lines near ties: long and nearly vertical, as that case, on a
   256 x 512 display, 16 a frame in columns 16 pixels apart. */
static void steep(int scale, int precise)
{
    static const int dys[] = {511, 510, 509, 507, 449, 384, 383, 256, 255, 128, 127, 101,
                              77,  33,  17,  9,   5,   3,   2};
    static const int dxs[] = {1, -1, 2, -2, 3, -3, 5, -5};
    setup s;
    line l[16];
    int box[16][4], n = 0, a, b, from;

    memset(&s, 0, sizeof s);
    s.scale = scale;
    s.precise = precise;
    s.width = 256;
    s.height = 512;
    start(&s);
    for (a = 0; a < (int)(sizeof dys / sizeof dys[0]); ++a)
        for (b = 0; b < (int)(sizeof dxs / sizeof dxs[0]); ++b) {
            int dx = dxs[b], dy = dys[a], x = 16 * n + 7, y = 0;

            /* down or up: the end points either way round */
            if ((a + b) & 1) {
                y = dy;
                dy = -dy;
            }
            l[n] = make(x, y, dx, dy, precise);
            box[n][0] = x - 6;
            box[n][1] = 0;
            box[n][2] = x + 7;
            box[n][3] = 511;
            if (++n == 16 || (a == (int)(sizeof dys / sizeof dys[0]) - 1 &&
                              b == (int)(sizeof dxs / sizeof dxs[0]) - 1)) {
                for (from = 0; from < 2; ++from)
                    batch(&s, l, box, n, from, "steep");
                n = 0;
            }
        }
    /* The case that failed: (100, 0) to (101, 511) at 3x, and its mirror. */
    if (scale == 3 && !precise) {
        line r[2];
        int rb[2][4] = {{92, 0, 109, 511}, {148, 0, 165, 511}};
        r[0] = make(100, 0, 1, 511, 0);
        r[1] = make(157, 0, -1, 511, 0);
        for (from = 0; from < 2; ++from) {
            int y;
            batch(&s, r, rb, 2, from, "steep case");
            for (y = 257; y <= 258; ++y)
                check("steep: steep case: band starts at x 301",
                      s.frame[((size_t)y * (size_t)s.fw + 300) * 4] == 0 &&
                          s.frame[((size_t)y * (size_t)s.fw + 301) * 4] != 0,
                      1);
        }
    }
    jb_destroy(s.jb);
}

/* Flat lines near ties: long and nearly horizontal, on a 1024 x 64
   display, 6 a frame in rows 10 pixels apart. */
static void flat(int scale, int precise)
{
    static const int dxs[] = {1023, 1022, 1021, 1019, 999, 768, 767, 512, 511, 300, 257,
                              101,  33,   9,    5,    3};
    static const int dys[] = {1, -1, 2, -2, 3, -3};
    setup s;
    line l[6];
    int box[6][4], n = 0, a, b, from;

    memset(&s, 0, sizeof s);
    s.scale = scale;
    s.precise = precise;
    s.width = 1024;
    s.height = 64;
    start(&s);
    for (a = 0; a < (int)(sizeof dxs / sizeof dxs[0]); ++a)
        for (b = 0; b < (int)(sizeof dys / sizeof dys[0]); ++b) {
            int dx = dxs[a], dy = dys[b], x = 0, y = 10 * n + 4;

            if ((a + b) & 1) {
                x = dx;
                dx = -dx;
            }
            l[n] = make(x, y, dx, dy, precise);
            box[n][0] = 0;
            box[n][1] = y - 4;
            box[n][2] = 1023;
            box[n][3] = y + 5;
            if (++n == 6 || (a == (int)(sizeof dxs / sizeof dxs[0]) - 1 &&
                             b == (int)(sizeof dys / sizeof dys[0]) - 1)) {
                for (from = 0; from < 2; ++from)
                    batch(&s, l, box, n, from, "flat");
                n = 0;
            }
        }
    jb_destroy(s.jb);
}

/* Every octant, many slopes: 80 x 60 cells on a 320 x 240 display. */
static void octants(int scale, int precise)
{
    setup s;
    line l[16];
    int box[16][4], k, from, i;

    memset(&s, 0, sizeof s);
    s.scale = scale;
    s.precise = precise;
    s.width = 320;
    s.height = 240;
    start(&s);
    for (k = 0; k < 8; ++k) {
        for (i = 0; i < 16; ++i) {
            int cx = (i % 4) * 80, cy = (i / 4) * 60;
            int dx = pick(71), dy = pick(51), j = k * 16 + i;

            if (j % 7 == 0)
                dy = dx * 51 / 71; /* near the diagonal */
            if (j % 11 == 0)
                dx = dy; /* on it */
            if (dx == 0 && dy == 0 && precise)
                dx = 1; /* two ends in one pixel share one precise record */
            if (j & 1)
                dx = -dx;
            if (j & 2)
                dy = -dy;
            l[i] = make(cx + 4 + (dx < 0 ? 71 : 0), cy + 4 + (dy < 0 ? 51 : 0), dx, dy, precise);
            box[i][0] = cx;
            box[i][1] = cy;
            box[i][2] = cx + 79;
            box[i][3] = cy + 59;
        }
        for (from = 0; from < 2; ++from)
            batch(&s, l, box, 16, from, "octants");
    }
    jb_destroy(s.jb);
}

int main(void)
{
    static const int scales[] = {1, 2, 3, 4, 8};
    int si, precise;

    for (si = 0; si < 5; ++si)
        for (precise = 0; precise < 2; ++precise) {
            if (scales[si] == 1 && precise)
                continue; /* precise positions are the scaled pass's */
            steep(scales[si], precise);
            flat(scales[si], precise);
            octants(scales[si], precise);
        }
    printf("hires_lines: %lld lines, %lld pixels\n", lines_checked, pixels_checked);
    return finish("hires_lines");
}

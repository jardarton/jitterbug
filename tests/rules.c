/*
 * rules: the drawing rules fitted to ps1-tests' dumps where those dumps
 * cover them thinly (docs/conformance.md, "Assumptions beyond the
 * evidence"), each pixel worked out here from the rule as the library
 * documents it, at 1x and at 2x and 3x (the 1x VRAM read back) on 1 to 4
 * threads:
 *   flip    x-flipped rectangles start one texel right of u0 for every u0
 *           (the dump has u0 = 0 only), also when the drawing area clips
 *           their left part; y-flipped ones start at v0
 *   ties    a line's minor coordinate at an exact half: y rounds to the
 *           larger row in lines along x, x to the smaller column in lines
 *           along y, in all eight directions (the dump has two), so a line
 *           draws the same pixels either way round
 *   colour  a Gouraud line's colour from either end, and a quad's second
 *           triangle, by the 12-bit rule of src/soft/raster.c
 * C99 against <jitterbug.h> alone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jitterbug.h>

static int failures;

static void check(const char *what, long long got, long long want)
{
    if (got == want)
        return;
    if (++failures <= 30)
        printf("FAIL %s: got %lld, want %lld\n", what, got, want);
}

static uint32_t xy(int x, int y)
{
    return ((uint32_t)x & 0xffffu) | ((uint32_t)y & 0xffffu) << 16;
}

static void gp0(jb_context *jb, const uint32_t *w, size_t n)
{
    if (jb_gp0(jb, w, n) != JB_OK)
        check("jb_gp0", 1, 0);
}

static void area(jb_context *jb, int x0, int y0, int x1, int y1)
{
    uint32_t w[4];

    w[0] = 0xe3000000u | (uint32_t)x0 | (uint32_t)y0 << 10;
    w[1] = 0xe4000000u | (uint32_t)x1 | (uint32_t)y1 << 10;
    w[2] = 0xe5000000u;
    w[3] = 0xe6000000u;
    gp0(jb, w, 4);
}

/* Clears rows y..y+h-1 of VRAM's left 512 columns to 0. */
static void clear(jb_context *jb, int y, int h)
{
    uint32_t w[3];

    w[0] = 0x02000000u;
    w[1] = xy(0, y);
    w[2] = 512u | (uint32_t)h << 16;
    gp0(jb, w, 3);
}

/* ------------------------------------------------------------------------ */

enum { PAGE_X = 512 }; /* a 15-bit page at (512, 0): texel u of row v is u + 256 v + 1 */

static void flip_texture(jb_context *jb)
{
    static uint16_t px[256 * 4];
    int i;

    for (i = 0; i < 256 * 4; ++i)
        px[i] = (uint16_t)(i + 1);
    jb_vram_load(jb, PAGE_X, 0, 256, 4, px);
}

static void flips(jb_context *jb, const char *run)
{
    static const int u0s[] = {0, 1, 5, 128, 200, 254, 255};
    uint16_t got[64];
    char what[160];
    unsigned k;
    int clip, x, fy;

    flip_texture(jb);
    for (fy = 0; fy < 2; ++fy)
        for (clip = 0; clip < 2; ++clip)
            for (k = 0; k < sizeof u0s / sizeof u0s[0]; ++k) {
                int u0 = u0s[k], v0 = fy ? 3 : 0, y = 300;
                uint32_t w[5];

                clear(jb, y, 4);
                area(jb, clip ? 10 : 0, 0, 1023, 511);
                /* 15-bit page, x-flip, and the y-flip for fy. */
                w[0] = 0xe1000000u | PAGE_X / 64 | 2u << 7 | 1u << 12 | (uint32_t)fy << 13;
                w[1] = 0x65000000u; /* raw-textured sprite */
                w[2] = xy(0, y);
                w[3] = (uint32_t)u0 | (uint32_t)v0 << 8;
                w[4] = 64u | 2u << 16;
                gp0(jb, w, 1);
                gp0(jb, w + 1, 4);
                for (x = 0; x < 2; ++x) { /* the sprite's two rows */
                    int i;
                    jb_vram_read(jb, 0, y + x, 64, 1, got);
                    for (i = 0; i < 64; ++i) {
                        int u = (u0 + 1 - i) & 255, v = fy ? v0 - x : v0 + x;
                        int want = clip && i < 10 ? 0 : u + 256 * v + 1;
                        snprintf(what, sizeof what, "%s: flip%s u0 %d%s, row %d pixel %d", run,
                                 fy ? " x and y" : " x", u0, clip ? " clipped" : "", x, i);
                        check(what, got[i], want);
                    }
                }
            }
    {
        uint32_t w = 0xe1000000u;
        gp0(jb, &w, 1);
    }
}

/* ------------------------------------------------------------------------ */

static long long floor_div(long long n, long long d)
{
    long long q = n / d;
    return (n % d != 0 && n < 0) ? q - 1 : q;
}

/* The pixel at step i of the line a-b as the library documents it. */
static void line_pixel(int ax, int ay, int bx, int by, int i, int *x, int *y)
{
    int dx = bx - ax, dy = by - ay, adx = abs(dx), ady = abs(dy);
    int n = adx > ady ? adx : ady;

    if (adx > ady) {
        *x = ax + (dx < 0 ? -i : i);
        *y = ay + (int)floor_div(2LL * i * dy + n, 2LL * n); /* halves up */
    } else {
        *y = ay + (dy < 0 ? -i : i);
        *x = ax + (int)floor_div(2LL * i * dx + n - 1, 2LL * n); /* halves down */
    }
}

/* Draws the flat line a-b in white on a cleared 32 x 32 square at (X0,
   Y0), and compares the square with the documented pixels. */
static void one_line(jb_context *jb, const char *run, int ax, int ay, int bx, int by)
{
    enum { S = 32, X0 = 100, Y0 = 200 };
    uint16_t got[S * S];
    unsigned char want[S * S];
    char what[160];
    uint32_t w[3];
    int i, n = abs(bx - ax) > abs(by - ay) ? abs(bx - ax) : abs(by - ay);

    clear(jb, Y0, S);
    area(jb, 0, 0, 1023, 511);
    w[0] = 0x40ffffffu;
    w[1] = xy(X0 + ax, Y0 + ay);
    w[2] = xy(X0 + bx, Y0 + by);
    gp0(jb, w, 3);
    jb_vram_read(jb, X0, Y0, S, S, got);
    memset(want, 0, sizeof want);
    for (i = 0; i <= n; ++i) {
        int x, y;
        line_pixel(ax, ay, bx, by, i, &x, &y);
        want[y * S + x] = 1;
    }
    for (i = 0; i < S * S; ++i) {
        if ((got[i] != 0) != want[i]) {
            snprintf(what, sizeof what, "%s: line (%d, %d)-(%d, %d), pixel (%d, %d)", run, ax, ay,
                     bx, by, i % S, i / S);
            check(what, got[i] != 0, want[i]);
        }
    }
}

static void lines(jb_context *jb, const char *run)
{
    int dmin, s1, s2, major;

    /* Lines of 8 steps from the square's centre (16, 16), the minor
       difference 1..7 (odd ones have a tie at step 4), in each direction
       and back. */
    for (major = 0; major < 2; ++major)
        for (s1 = -1; s1 <= 1; s1 += 2)
            for (s2 = -1; s2 <= 1; s2 += 2)
                for (dmin = 1; dmin < 8; ++dmin) {
                    int dx = major == 0 ? 8 * s1 : dmin * s2, dy = major == 0 ? dmin * s2 : 8 * s1;
                    one_line(jb, run, 16, 16, 16 + dx, 16 + dy);
                    one_line(jb, run, 16 + dx, 16 + dy, 16, 16);
                }
    /* And longer ones with many ties: 2 x 10 steps. */
    one_line(jb, run, 2, 5, 22, 15);
    one_line(jb, run, 22, 15, 2, 5);
    one_line(jb, run, 5, 2, 15, 22);
    one_line(jb, run, 15, 22, 5, 2);
}

/* ------------------------------------------------------------------------ */

/* A channel by the 12-bit rule: c0 + 1/2 + step * i, step = d * 4096 / n
   truncated toward zero. */
static int rule(int c0, int d, int n, int i)
{
    long long step = n > 0 ? (long long)d * 4096 / n : 0;
    return (int)floor_div((long long)c0 * 4096 + 2048 + step * i, 4096);
}

static void colours(jb_context *jb, const char *run)
{
    enum { Y0 = 260 };
    uint16_t got[64];
    char what[160];
    uint32_t w[5];
    int i, dir;

    /* A Gouraud line of 50 steps from red 200 to red 13, drawn both ways:
       each pixel's colour from its own first vertex. */
    for (dir = 0; dir < 2; ++dir) {
        int c0 = dir ? 13 : 200, c1 = dir ? 200 : 13;

        clear(jb, Y0, 2);
        area(jb, 0, 0, 1023, 511);
        w[0] = 0x50000000u | (uint32_t)c0;
        w[1] = xy(dir ? 50 : 0, Y0);
        w[2] = (uint32_t)c1;
        w[3] = xy(dir ? 0 : 50, Y0);
        gp0(jb, w, 4);
        jb_vram_read(jb, 0, Y0, 51, 1, got);
        for (i = 0; i <= 50; ++i) {
            int step = dir ? 50 - i : i;
            snprintf(what, sizeof what, "%s: Gouraud line %s, pixel %d", run,
                     dir ? "right to left" : "left to right", i);
            check(what, got[i] & 31, rule(c0, c1 - c0, 50, step) >> 3);
        }
    }
    /* A Gouraud quad one row high, 37 wide (as uv-interpolation's): its
       triangles are v0 v1 v2 and v1 v2 v3, each from its first vertex;
       the row y is in the first, at x from v0, with the x gradient of
       red (255 over 37 pixels, the y one taking no part). */
    clear(jb, Y0, 2);
    {
        uint32_t q[8];
        q[0] = 0x380000ffu;          /* v0 red 255 */
        q[1] = xy(0, Y0);
        q[2] = 0x00000000u;          /* v1 black */
        q[3] = xy(37, Y0);
        q[4] = 0x000000ffu;          /* v2 red 255 */
        q[5] = xy(0, Y0 + 1);
        q[6] = 0x00000000u;          /* v3 black */
        q[7] = xy(37, Y0 + 1);
        gp0(jb, q, 8);
    }
    jb_vram_read(jb, 0, Y0, 37, 1, got);
    for (i = 0; i < 37; ++i) {
        /* Triangle (0, y) (37, y) (0, y + 1): area 37, x gradient -255/37. */
        snprintf(what, sizeof what, "%s: Gouraud quad, pixel %d", run, i);
        check(what, got[i] & 31, rule(255, -255, 37, i) >> 3);
    }
}

int main(void)
{
    static const struct {
        const char *name;
        int scale, threads;
    } runs[] = {
        {"1x", 1, 1}, {"1x 4 threads", 1, 4}, {"2x 2 threads", 2, 2}, {"3x 3 threads", 3, 3}};
    unsigned r;

    for (r = 0; r < sizeof runs / sizeof runs[0]; ++r) {
        jb_config cfg;
        jb_context *jb;

        memset(&cfg, 0, sizeof cfg);
        cfg.scale = runs[r].scale;
        cfg.threads = runs[r].threads;
        jb = jb_create(&cfg, NULL);
        if (jb == NULL) {
            printf("FAIL: no context\n");
            return 1;
        }
        flips(jb, runs[r].name);
        lines(jb, runs[r].name);
        colours(jb, runs[r].name);
        jb_destroy(jb);
        printf("%s: done\n", runs[r].name);
    }
    printf("rules: %d failures\n", failures);
    return failures != 0;
}

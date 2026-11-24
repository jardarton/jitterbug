/*
 * api_vram: jb_vram_load, jb_vram_read, jb_vram_fill and jb_vram_copy
 * against the GP0 commands they stand for.
 *
 * Two contexts with the same configuration get a seeded run of transfers,
 * A through the calls and B through GP0(A0h) with its pixels, GP0(02h)
 * and GP0(80h): rectangles anywhere, wrapping at the right and bottom
 * edges, sizes of 1, odd ones and the largest (1024 x 512; for fills
 * 1023 x 511, and 0), fills at x not a multiple of 16, overlapping copies,
 * and each with the mask settings of GP0(E6h) changing between them.
 * After every call each jb_vram_read of a random rectangle (wrapping too)
 * equals GP0(C0h) and GPUREAD of the same rectangle on B, and the whole
 * VRAM read back through GPUREAD is compared now and then; jb_frame at the
 * end. At 1x and 2x, with 1 and 4 threads; at 2x, with primitives drawn
 * between the transfers, jb_vram_read gives exactly what a 1x context
 * given the same words reads.
 *
 * Also: the range rule (each argument just out of its range is refused,
 * nothing changes), and jb_vram_read leaves a GP0(C0h) being read through
 * GPUREAD where it was.
 */
#include "api_test.h"

enum { STEPS = 400 };

static uint16_t pix_a[VRAM_PIXELS], pix_b[VRAM_PIXELS], src[VRAM_PIXELS];

/* A position, near an edge now and then so that it wraps. */
static int gen_pos(gen *g, int size)
{
    int k = rnd_below(g, 4);

    if (k == 0)
        return size - 1 - rnd_below(g, 20);
    return rnd_below(g, size);
}

static int gen_len(gen *g, int max)
{
    int k = rnd_below(g, 12);

    if (k == 0)
        return 1;
    if (k == 1)
        return max;
    if (k == 2)
        return 1 + rnd_below(g, max);
    return 1 + 2 * rnd_below(g, 20); /* odd */
}

static void compare_read(gen *g, jb_context *a, jb_context *b, const char *what)
{
    int x = gen_pos(g, VRAM_W), y = gen_pos(g, VRAM_H);
    int w = gen_len(g, VRAM_W), h = gen_len(g, VRAM_H);
    size_t n = (size_t)w * (size_t)h;

    check("jb_vram_read", jb_vram_read(a, x, y, w, h, pix_a), JB_OK);
    read_rect(b, x, y, w, h, pix_b);
    check(what, memcmp(pix_a, pix_b, n * 2) == 0, 1);
}

static void compare_all(jb_context *a, jb_context *b, const char *what)
{
    uint16_t *va = read_vram(a), *vb = read_vram(b);

    check(what, memcmp(va, vb, (size_t)VRAM_PIXELS * 2) == 0, 1);
    free(va);
    free(vb);
}

/* A small Gouraud triangle by GP0 words on both, so that the scaled
   picture differs from the 1x one. */
static void triangle(gen *g, jb_context *c1, jb_context *c2, jb_context *c3)
{
    uint32_t w[6];
    int x = rnd_below(g, 900), y = rnd_below(g, 450);

    w[0] = 0x30000000u | (rnd(g) & 0xffffffu);
    w[1] = xy(x, y);
    w[2] = rnd(g) & 0xffffffu;
    w[3] = xy(x + 37, y + 11);
    w[4] = rnd(g) & 0xffffffu;
    w[5] = xy(x + 5, y + 50);
    gp0(c1, w, 6);
    gp0(c2, w, 6);
    if (c3 != NULL)
        gp0(c3, w, 6);
}

static void run(int scale, int threads, uint64_t seed)
{
    jb_config cfg;
    jb_context *a, *b, *one = NULL;
    gen g = {0, 0, 0};
    char what[96];
    int i, k;
    uint32_t setup[2] = {0xe3000000u, 0xe4000000u | 1023u | 511u << 10};

    memset(&cfg, 0, sizeof cfg);
    cfg.scale = scale;
    cfg.threads = threads;
    a = must_create(&cfg);
    b = must_create(&cfg);
    if (scale > 1)
        one = must_create(NULL); /* the 1x context for the 2x reads */
    gp0(a, setup, 2);
    gp0(b, setup, 2);
    if (one != NULL)
        gp0(one, setup, 2);
    gp1(a, 0x03000000u);
    gp1(b, 0x03000000u);
    g.s = seed;
    for (i = 0; i < STEPS; ++i) {
        k = rnd_below(&g, 100);
        if (k < 35) { /* load */
            int x = gen_pos(&g, VRAM_W), y = gen_pos(&g, VRAM_H);
            int w = gen_len(&g, VRAM_W), h = gen_len(&g, VRAM_H);
            size_t n, j;

            if ((size_t)w * (size_t)h > 200000)
                h = 1 + rnd_below(&g, 8);
            n = (size_t)w * (size_t)h;
            for (j = 0; j < n; ++j)
                src[j] = (uint16_t)rnd(&g);
            check("jb_vram_load", jb_vram_load(a, x, y, w, h, src), JB_OK);
            load_rect(b, x, y, w, h, src);
            if (one != NULL)
                load_rect(one, x, y, w, h, src);
        } else if (k < 55) { /* fill */
            int x = gen_pos(&g, VRAM_W), y = gen_pos(&g, VRAM_H);
            int w = rnd_below(&g, 8) ? rnd_below(&g, 100) : rnd_below(&g, 1024);
            int h = rnd_below(&g, 8) ? rnd_below(&g, 100) : rnd_below(&g, 512);
            uint32_t color = rnd(&g) & 0xffffffu, cmd[3];

            if (rnd_below(&g, 10) == 0)
                w = 1023, h = 511;
            check("jb_vram_fill", jb_vram_fill(a, x, y, w, h, color), JB_OK);
            cmd[0] = 0x02000000u | color;
            cmd[1] = xy(x, y);
            cmd[2] = xy(w, h);
            gp0(b, cmd, 3);
            if (one != NULL)
                gp0(one, cmd, 3);
        } else if (k < 80) { /* copy, overlapping now and then */
            int sx = gen_pos(&g, VRAM_W), sy = gen_pos(&g, VRAM_H);
            int dx = gen_pos(&g, VRAM_W), dy = gen_pos(&g, VRAM_H);
            int w = gen_len(&g, VRAM_W), h = gen_len(&g, VRAM_H);
            uint32_t cmd[4];

            if (rnd_below(&g, 3) == 0)
                dx = (sx + rnd_below(&g, 9) - 4) & 1023, dy = (sy + rnd_below(&g, 9) - 4) & 511;
            check("jb_vram_copy", jb_vram_copy(a, sx, sy, dx, dy, w, h), JB_OK);
            cmd[0] = 0x80000000u;
            cmd[1] = xy(sx, sy);
            cmd[2] = xy(dx, dy);
            cmd[3] = xy(w, h);
            gp0(b, cmd, 4);
            if (one != NULL)
                gp0(one, cmd, 4);
        } else if (k < 90) { /* the mask settings */
            uint32_t e6 = 0xe6000000u | (uint32_t)rnd_below(&g, 4);

            gp0_1(a, e6);
            gp0_1(b, e6);
            if (one != NULL)
                gp0_1(one, e6);
        } else {
            triangle(&g, a, b, one);
        }
        snprintf(what, sizeof what, "%dx %dt step %d: jb_vram_read = GPUREAD", scale, threads, i);
        compare_read(&g, a, b, what);
        if (one != NULL) {
            int x = gen_pos(&g, VRAM_W), y = gen_pos(&g, VRAM_H);
            int w = gen_len(&g, VRAM_W), h = gen_len(&g, VRAM_H);

            jb_vram_read(a, x, y, w, h, pix_a);
            jb_vram_read(one, x, y, w, h, pix_b);
            snprintf(what, sizeof what, "%dx step %d: jb_vram_read = the 1x context's", scale, i);
            check(what, memcmp(pix_a, pix_b, (size_t)w * (size_t)h * 2) == 0, 1);
        }
        if (i % 50 == 49) {
            snprintf(what, sizeof what, "%dx %dt step %d: all of VRAM", scale, threads, i);
            compare_all(a, b, what);
        }
    }
    {
        int wa, ha, wb, hb;
        const uint8_t *fa = jb_frame(a, &wa, &ha), *fb = jb_frame(b, &wb, &hb);

        snprintf(what, sizeof what, "%dx %dt: frames", scale, threads);
        check(what, fa != NULL && fb != NULL && wa == wb && ha == hb &&
                        memcmp(fa, fb, (size_t)wa * (size_t)ha * 4) == 0,
              1);
    }
    jb_destroy(a);
    jb_destroy(b);
    jb_destroy(one);
}

/* Each argument just outside its range: refused, VRAM unchanged. */
static void ranges(void)
{
    jb_context *jb = must_create(NULL);
    uint16_t *before, *after;
    int i;
    static const int bad[][6] = {
        /* x, y, w, h for load/read/copy (copy: source at x, y) */
        {-1, 0, 4, 4}, {1024, 0, 4, 4}, {0, -1, 4, 4}, {0, 512, 4, 4},
        {0, 0, 0, 4},  {0, 0, 1025, 4}, {0, 0, 4, 0},  {0, 0, 4, 513},
    };

    for (i = 0; i < 64; ++i)
        src[i] = (uint16_t)(0x1234 + i);
    check("load at the ends", jb_vram_load(jb, 1023, 511, 1024, 512, src), JB_OK);
    before = read_vram(jb);
    for (i = 0; i < 8; ++i) {
        const int *v = bad[i];
        char what[64];

        snprintf(what, sizeof what, "load %d", i);
        check(what, jb_vram_load(jb, v[0], v[1], v[2], v[3], src), JB_ERROR_INVALID_ARGUMENT);
        snprintf(what, sizeof what, "read %d", i);
        check(what, jb_vram_read(jb, v[0], v[1], v[2], v[3], pix_b), JB_ERROR_INVALID_ARGUMENT);
        snprintf(what, sizeof what, "copy source %d", i);
        check(what, jb_vram_copy(jb, v[0], v[1], 0, 0, v[2], v[3]), JB_ERROR_INVALID_ARGUMENT);
        if (i < 4) {
            snprintf(what, sizeof what, "copy destination %d", i);
            check(what, jb_vram_copy(jb, 0, 0, v[0], v[1], v[2], v[3]),
                  JB_ERROR_INVALID_ARGUMENT);
            snprintf(what, sizeof what, "fill position %d", i);
            check(what, jb_vram_fill(jb, v[0], v[1], 16, 16, 0xff), JB_ERROR_INVALID_ARGUMENT);
        }
    }
    check("fill w 1024", jb_vram_fill(jb, 0, 0, 1024, 1, 0xff), JB_ERROR_INVALID_ARGUMENT);
    check("fill h 512", jb_vram_fill(jb, 0, 0, 1, 512, 0xff), JB_ERROR_INVALID_ARGUMENT);
    check("fill w -1", jb_vram_fill(jb, 0, 0, -1, 1, 0xff), JB_ERROR_INVALID_ARGUMENT);
    check("fill color", jb_vram_fill(jb, 0, 0, 16, 16, 0x1000000u), JB_ERROR_INVALID_ARGUMENT);
    check_str("fill color: error string", jb_error_string(jb),
              "jb_vram_fill: color 0x1000000 is above 0xffffff");
    check("load NULL", jb_vram_load(jb, 0, 0, 1, 1, NULL), JB_ERROR_INVALID_ARGUMENT);
    check_str("load NULL: error string", jb_error_string(jb), "jb_vram_load: pixels is NULL");
    check("read NULL", jb_vram_read(jb, 0, 0, 1, 1, NULL), JB_ERROR_INVALID_ARGUMENT);
    check("copy 1025", jb_vram_copy(jb, 0, 0, 0, 0, 1025, 1), JB_ERROR_INVALID_ARGUMENT);
    check_str("copy size: error string", jb_error_string(jb),
              "jb_vram_copy: size 1025 x 1 is outside 1..1024 x 1..512");
    check("fill 0 x 0 accepted", jb_vram_fill(jb, 1023, 511, 0, 0, 0xffffff), JB_OK);
    after = read_vram(jb);
    check("refused calls change nothing", memcmp(before, after, (size_t)VRAM_PIXELS * 2) == 0,
          1);
    free(before);
    free(after);
    jb_destroy(jb);
}

/* jb_vram_read between the GPUREAD words of a GP0(C0h). */
static void gpuread_untouched(void)
{
    jb_context *a = must_create(NULL), *b = must_create(NULL);
    uint32_t c0[3] = {0xc0000000u, 0, 0};
    int i, n;

    for (i = 0; i < 4096; ++i)
        src[i] = (uint16_t)(i * 2654435761u >> 16);
    jb_vram_load(a, 10, 10, 64, 64, src);
    load_rect(b, 10, 10, 64, 64, src);
    c0[1] = xy(10, 10);
    c0[2] = xy(33, 17);
    gp0(a, c0, 3);
    gp0(b, c0, 3);
    n = (33 * 17 + 1) / 2;
    for (i = 0; i < n; ++i) {
        char what[48];

        if (i == n / 2) {
            check("read in the middle", jb_vram_read(a, 0, 0, 1024, 512, pix_a), JB_OK);
            check("GPUSTAT still has data to read", (jb_gpustat(a) >> 27) & 1, 1);
        }
        snprintf(what, sizeof what, "GPUREAD word %d", i);
        check(what, jb_gpuread(a), jb_gpuread(b));
    }
    check("GPUSTAT the same at the end", jb_gpustat(a), jb_gpustat(b));
    jb_destroy(a);
    jb_destroy(b);
}

int main(void)
{
    run(1, 1, 11);
    run(1, 4, 12);
    run(2, 1, 13);
    run(2, 4, 13);
    ranges();
    gpuread_untouched();
    return finish("api_vram");
}

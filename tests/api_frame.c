/*
 * jb_frame:
 *   - its size is the display size times the scale: jb_config's, then the
 *     console mode GP1(08h) sets, back to jb_config's after GP1(00h);
 *   - at 1x it is the display area of VRAM, read back through GP0(C0h) and
 *     GPUREAD, each 5-bit channel widened as c << 3 | c >> 2, alpha 255,
 *     wrapping at the VRAM edges;
 *   - in 24-bit mode its R, G, B bytes are VRAM's bytes from the area's
 *     start;
 *   - a disabled display gives a black frame of the same size;
 *   - at 2x (and 3x) it equals the internal scanout (jbi_scanout,
 *     src/core/context.h) of a context configured as jb_create configures
 *     one and given the same words, widened the same way: the one place
 *     this test uses the internals;
 *   - the pointer stays the same, and nothing is allocated, while the size
 *     does not grow.
 */
#include "api_test.h"

#include "core/context.h"

static jb_context *with(int scale, int w, int h)
{
    jb_config c;

    memset(&c, 0, sizeof c);
    c.scale = scale;
    c.width = w;
    c.height = h;
    return must_create(&c);
}

/* Some drawing, for the scaled comparisons: a page of texels, a
   Gouraud triangle, a textured quad, lines, a semi-transparent rectangle,
   and a 24-bit-looking run of bytes. */
static int scene(uint32_t *w)
{
    int n = 0, i;

    w[n++] = 0xe1000000u | 8u | 2u << 7 | 1u << 9 | 1u << 10;
    w[n++] = 0xe3000000u;
    w[n++] = 0xe4000000u | 1023u | 511u << 10;
    w[n++] = 0x02102040u;
    w[n++] = xy(0, 0);
    w[n++] = xy(1008, 511);
    w[n++] = 0xa0000000u;
    w[n++] = xy(512, 0);
    w[n++] = xy(16, 16);
    for (i = 0; i < 128; ++i)
        w[n++] = (uint32_t)(i * 0x2493u) | (uint32_t)(0x7fff - i * 0x1111u) << 16;
    w[n++] = 0x30ff0000u;
    w[n++] = xy(10, 10);
    w[n++] = 0x0000ff00u;
    w[n++] = xy(300, 30);
    w[n++] = 0x000000ffu;
    w[n++] = xy(40, 230);
    w[n++] = 0x2c808080u;
    w[n++] = xy(200, 100);
    w[n++] = 0x00000000u;
    w[n++] = xy(310, 110);
    w[n++] = (8u | 2u << 7) << 16 | 0x000fu;
    w[n++] = xy(190, 220);
    w[n++] = 0x0f00u;
    w[n++] = xy(300, 235);
    w[n++] = 0x0f0fu;
    w[n++] = 0x5000ffffu;
    w[n++] = xy(0, 239);
    w[n++] = 0x00ff00ffu;
    w[n++] = xy(319, 0);
    w[n++] = 0x4a40c0f0u;
    w[n++] = xy(5, 200);
    w[n++] = xy(100, 150);
    w[n++] = xy(250, 210);
    w[n++] = 0x55555555u;
    w[n++] = 0x62203040u;
    w[n++] = xy(150, 50);
    w[n++] = xy(120, 90);
    return n;
}

/* The frame at 1x against the VRAM area read back. */
static void check_area(jb_context *jb, const char *what, int x, int y, int w, int h)
{
    uint16_t *area = must_alloc((size_t)w * h * 2 + 2);
    const uint8_t *f;
    int fw, fh, bad = 0, i;
    char msg[128];

    read_rect(jb, x, y, w, h, area);
    f = jb_frame(jb, &fw, &fh);
    snprintf(msg, sizeof msg, "%s: frame size", what);
    if (!check(msg, fw * 10000 + fh, w * 10000 + h) || f == NULL) {
        free(area);
        return;
    }
    for (i = 0; i < w * h; ++i) {
        unsigned p = area[i];
        bad += f[4 * i] != widen5(p & 31) || f[4 * i + 1] != widen5((p >> 5) & 31) ||
               f[4 * i + 2] != widen5((p >> 10) & 31) || f[4 * i + 3] != 255;
    }
    snprintf(msg, sizeof msg, "%s: pixels unlike the VRAM read back", what);
    check(msg, bad, 0);
    free(area);
}

static int all_black(const uint8_t *f, int w, int h)
{
    int i;

    for (i = 0; i < w * h; ++i)
        if (f[4 * i] || f[4 * i + 1] || f[4 * i + 2] || f[4 * i + 3] != 255)
            return 0;
    return 1;
}

static void basics(void)
{
    jb_context *jb = with(0, 0, 0);
    uint16_t *pix = must_alloc(320 * 240 * 2);
    const uint8_t *f;
    int w, h, i;

    f = jb_frame(jb, &w, &h);
    check("default frame size", w * 10000 + h, 320 * 10000 + 240);
    check("a new context's display is disabled: black", f != NULL && all_black(f, w, h), 1);
    /* Every 5-bit value of every channel, and the mask bit, which is not
       shown. */
    for (i = 0; i < 320 * 240; ++i)
        pix[i] = (uint16_t)((i & 31) | ((i >> 5) & 31) << 5 | ((i >> 10) & 31) << 10 |
                            (i & 0x40) << 9);
    load_rect(jb, 100, 50, 320, 240, pix);
    f = jb_frame(jb, &w, &h);
    check("disabled: still black after drawing", f != NULL && all_black(f, w, h), 1);
    gp1(jb, 0x03000000u);
    gp1(jb, 0x05000000u | 100u | 50u << 10);
    check_area(jb, "display at (100, 50)", 100, 50, 320, 240);
    f = jb_frame(jb, &w, &h);
    check("channel 0 widens to 0", f[0], 0);
    check("channel 1 widens to 8", f[4], 8);
    check("channel 16 widens to 132", f[16 * 4], 132);
    check("channel 31 widens to 255", f[31 * 4], 255);
    check("green 31 widens to 255", f[(31 * 32) * 4 + 1], 255);
    check("alpha with the mask bit set", f[0x40 * 4 + 3], 255);
    /* Wrapping at the right and bottom edges. */
    gp1(jb, 0x05000000u | 900u | 400u << 10);
    check_area(jb, "display at (900, 400), wrapping", 900, 400, 320, 240);
    gp1(jb, 0x03000001u);
    f = jb_frame(jb, &w, &h);
    check("disabled again: black, same size",
          f != NULL && w == 320 && h == 240 && all_black(f, w, h), 1);
    free(pix);
    jb_destroy(jb);
}

static void modes(void)
{
    static const int widths[4] = {256, 320, 512, 640};
    jb_context *jb = with(1, 427, 240);
    const uint8_t *f;
    int w, h, mode;

    f = jb_frame(jb, &w, &h);
    check("a 427 x 240 display", w * 10000 + h, 427 * 10000 + 240);
    gp1(jb, 0x03000000u);
    for (mode = 0; mode < 256; ++mode) {
        int want_w = (mode & 0x40) ? 368 : widths[mode & 3];
        int want_h = (mode & 0x04) && (mode & 0x20) ? 480 : 240;
        char what[64];

        if (mode & 0x10)
            continue; /* 24-bit: below */
        gp1(jb, 0x08000000u | (uint32_t)mode);
        snprintf(what, sizeof what, "display mode %02xh", mode);
        if (mode % 37 == 0 || mode == 0x27 || mode == 0x47)
            check_area(jb, what, 0, 0, want_w, want_h);
        f = jb_frame(jb, &w, &h);
        check(what, w * 10000 + h, want_w * 10000 + want_h);
    }
    gp1(jb, 0x00000000u);
    f = jb_frame(jb, &w, &h);
    check("after GP1(00h): the configured size again", w * 10000 + h, 427 * 10000 + 240);
    check("after GP1(00h): display disabled", f != NULL && all_black(f, w, h), 1);
    (void)f;
    jb_destroy(jb);
}

/* 24-bit mode: the area's bytes from (x, y) as R, G, B. */
static void rgb24(void)
{
    jb_context *jb = with(1, 0, 0);
    uint16_t *row = must_alloc(1024 * 2), *pix = must_alloc(1024 * 2);
    const uint8_t *f;
    int w, h, y, i, bad = 0;

    for (y = 0; y < 240; ++y) {
        for (i = 0; i < 1024; ++i)
            pix[i] = (uint16_t)(i * 0x0101 + y * 0x3700 + 0x8001);
        load_rect(jb, 0, 20 + y, 1024, 1, pix);
    }
    gp1(jb, 0x03000000u);
    gp1(jb, 0x05000000u | 1000u | 20u << 10); /* x is a 16-bit pixel address */
    gp1(jb, 0x08000011u);                     /* 320 x 240, 24-bit */
    f = jb_frame(jb, &w, &h);
    check("24-bit frame size", w * 10000 + h, 320 * 10000 + 240);
    for (y = 0; y < 240 && f != NULL; ++y) {
        read_rect(jb, 0, 20 + y, 1024, 1, row);
        for (i = 0; i < 320 * 3; ++i) {
            int at = (2 * 1000 + i) & 2047; /* bytes wrap at the row's end */
            uint8_t want = (uint8_t)(row[at >> 1] >> (8 * (at & 1)));
            bad += f[(size_t)(y * 320 + i / 3) * 4 + i % 3] != want;
        }
        for (i = 0; i < 320; ++i)
            bad += f[(size_t)(y * 320 + i) * 4 + 3] != 255;
    }
    check("24-bit bytes unlike VRAM's", bad, 0);
    gp1(jb, 0x08000037u); /* 640 x 480, 24-bit */
    f = jb_frame(jb, &w, &h);
    check("24-bit 640 x 480 frame size", w * 10000 + h, 640 * 10000 + 480);
    gp1(jb, 0x08000001u);
    check_area(jb, "back to 15-bit", 1000, 20, 320, 240);
    free(row);
    free(pix);
    jb_destroy(jb);
}

/* The internal scanout of the same words, widened. */
static uint8_t *internal_frame(int scale, const uint32_t *words, int n, const uint32_t *gp1s,
                               int ngp1, int *fw, int *fh)
{
    jbi_config cfg;
    jb_context *ctx;
    uint8_t *buf, *rgba;
    int x = 0, y = 0, w = 320, h = 240, rgb = 0, on = 0, stride, fmt, k;
    size_t i;

    /* The display state the GP1 words set (only the ones used here). */
    for (k = 0; k < ngp1; ++k) {
        uint32_t g = gp1s[k];
        switch (g >> 24) {
        case 0x03: on = !(g & 1); break;
        case 0x05: x = (int)(g & 0x3ff), y = (int)((g >> 10) & 0x1ff); break;
        case 0x08:
            w = (g & 0x40) ? 368 : (int[]){256, 320, 512, 640}[g & 3];
            h = (g & 0x04) && (g & 0x20) ? 480 : 240;
            rgb = (g & 0x10) != 0;
            break;
        default: break;
        }
    }
    jbi_config_default(&cfg);
    cfg.scale = scale;
    cfg.exact_1x = 1;
    ctx = jbi_context_create(&cfg);
    jbi_reset_state(ctx);
    jbi_gp0(ctx, words, n);
    buf = must_alloc(jbi_scanout_bytes(ctx));
    fmt = jbi_scanout(ctx, x, y, w, h, rgb, on, buf, fw, fh, &stride);
    rgba = must_alloc((size_t)*fw * *fh * 4);
    for (i = 0; i < (size_t)*fw * *fh; ++i) {
        if (fmt == JBI_SCANOUT_RGB24) {
            rgba[4 * i] = buf[3 * i];
            rgba[4 * i + 1] = buf[3 * i + 1];
            rgba[4 * i + 2] = buf[3 * i + 2];
        } else {
            unsigned p = buf[2 * i] | (unsigned)buf[2 * i + 1] << 8;
            rgba[4 * i] = (uint8_t)widen5(p & 31);
            rgba[4 * i + 1] = (uint8_t)widen5((p >> 5) & 31);
            rgba[4 * i + 2] = (uint8_t)widen5((p >> 10) & 31);
        }
        rgba[4 * i + 3] = 255;
    }
    free(buf);
    jbi_context_destroy(ctx);
    return rgba;
}

static void scaled(int scale)
{
    static const uint32_t displays[][3] = {
        {0x03000000u, 0x05000000u, 0x08000001u},                        /* 320 x 240 */
        {0x03000000u, 0x05000000u | 900u | 400u << 10, 0x08000001u},    /* wrapping */
        {0x03000000u, 0x05000000u | 8u | 4u << 10, 0x08000027u},        /* 640 x 480 */
        {0x03000000u, 0x05000000u | 512u, 0x08000011u},                 /* 24-bit */
        {0x03000001u, 0x05000000u, 0x08000001u},                        /* disabled */
    };
    uint32_t words[1024];
    int n = scene(words), d;
    char what[96];

    for (d = 0; d < (int)(sizeof displays / sizeof displays[0]); ++d) {
        jb_context *jb = with(scale, 0, 0);
        const uint8_t *f;
        uint8_t *want;
        int fw, fh, ww, wh, k;

        gp0(jb, words, (size_t)n);
        for (k = 0; k < 3; ++k)
            gp1(jb, displays[d][k]);
        f = jb_frame(jb, &fw, &fh);
        want = internal_frame(scale, words, n, displays[d], 3, &ww, &wh);
        snprintf(what, sizeof what, "%dx, display %d: frame size", scale, d);
        check(what, fw * 10000 + fh, ww * 10000 + wh);
        snprintf(what, sizeof what, "%dx, display %d: frame size is the display's times %d",
                 scale, d, scale);
        check(what, fw * 10000 + fh, (d == 2 ? 640 : 320) * scale * 10000 +
                                         (d == 2 ? 480 : 240) * scale);
        snprintf(what, sizeof what, "%dx, display %d: unlike the internal scanout", scale, d);
        check(what, f == NULL || fw != ww || fh != wh || memcmp(f, want, (size_t)fw * fh * 4),
              0);
        if (d == 4) {
            snprintf(what, sizeof what, "%dx, disabled: black", scale);
            check(what, f != NULL && all_black(f, fw, fh), 1);
        }
        free(want);
        jb_destroy(jb);
    }
}

/* ------------------------------------------------------------------------ */
/* No allocation per call */

static long allocations;

static void *count_alloc(void *user, size_t size)
{
    (void)user;
    ++allocations;
    return malloc(size);
}

static void count_free(void *user, void *ptr)
{
    (void)user;
    free(ptr);
}

static void no_allocation(void)
{
    jb_config c;
    jb_context *jb;
    const uint8_t *first, *f;
    uint32_t words[1024];
    int n = scene(words), k, w, h;
    long before;

    memset(&c, 0, sizeof c);
    c.scale = 2;
    c.alloc = count_alloc;
    c.free = count_free;
    jb = must_create(&c);
    gp1(jb, 0x03000000u);
    gp1(jb, 0x08000027u); /* the largest frame first: 640 x 480 */
    first = jb_frame(jb, &w, &h);
    before = allocations;
    for (k = 0; k < 30; ++k) {
        gp0(jb, words, (size_t)n);
        gp1(jb, 0x08000000u | (uint32_t)(k % 4));
        f = jb_frame(jb, &w, &h);
        check("the same frame pointer while the size does not grow", f == first, 1);
    }
    check("allocations during frames and drawing", allocations - before, 0);
    jb_destroy(jb);
}

int main(void)
{
    basics();
    modes();
    rgb24();
    scaled(2);
    scaled(3);
    no_allocation();
    return finish("api_frame");
}

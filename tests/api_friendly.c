/*
 * api_friendly: the frame, the ordering table, the sticky error, the new
 * options and the helpers, through <jitterbug.h> alone.
 *
 *   config      the new jb_config fields' ranges
 *   helpers     the colour and word macros against their documented values
 *               and against the jb_set_* calls they stand for
 *   sticky      jb_check and jb_end_frame: the first failure kept, its
 *               detail, taken once; every kind of failing call latches
 *   frame       errors; the display enabled by the first frame; the
 *               buffers in turn (stacked, side by side, one), shown and
 *               cleared; a clear that writes only the buffer's pixels,
 *               through the mask settings, which it restores; widescreen
 *               (the width at once, deferred in a frame, refused without
 *               the reservation); layers; console calls landing under the
 *               friendly draws; the area, offset and texture window around
 *               jb_end_frame; the records' memory failing at the first
 *               jb_begin_frame
 *   2D          jb_draw_* within the console's limits equal to jb_prim_*
 *               (1x to 3x, the float options on, and GTE projections on
 *               the same pixels, which move only jb_prim_*); errors;
 *               anything in range accepted
 *   ot          jb_ot_*: order (highest bucket first, FIFO within one),
 *               clear, every error
 *   clut cache  jb_set_clut_cache on and off against a palette rewritten
 *               in place
 */
#include "api_test.h"

/* ------------------------------------------------------------------------ */

static jb_context *ctx(int w, int h, int scale, int widescreen)
{
    jb_config c;

    memset(&c, 0, sizeof c);
    c.width = w;
    c.height = h;
    c.scale = scale;
    c.widescreen = widescreen;
    return must_create(&c);
}

static uint16_t pixel(jb_context *jb, int x, int y)
{
    uint16_t p = 0;

    jb_vram_read(jb, x, y, 1, 1, &p);
    return p;
}

static void config_fields(void)
{
    jb_config c;
    jb_result r;
    jb_context *jb;

    memset(&c, 0, sizeof c);
    c.max_primitives = -1;
    check("max_primitives -1", jb_create(&c, &r) == NULL && r == JB_ERROR_INVALID_ARGUMENT, 1);
    c.max_primitives = JB_LIMIT_MAX_PRIMITIVES + 1;
    check("max_primitives too many", jb_create(&c, &r) == NULL, 1);
    c.max_primitives = JB_LIMIT_MAX_PRIMITIVES;
    c.ot_size = -1;
    check("ot_size -1", jb_create(&c, &r) == NULL && r == JB_ERROR_INVALID_ARGUMENT, 1);
    c.ot_size = JB_LIMIT_OT_SIZE + 1;
    check("ot_size too many", jb_create(&c, &r) == NULL, 1);
    c.ot_size = JB_LIMIT_OT_SIZE;
    c.geometry = 2;
    check("geometry 2", jb_create(&c, &r) == NULL && r == JB_ERROR_INVALID_ARGUMENT, 1);
    c.geometry = JB_GEOMETRY_FLOAT;
    c.widescreen = 7;
    c.clut_cache_off = 1;
    jb = jb_create(&c, &r);
    check("all set", jb != NULL && r == JB_OK, 1);
    jb_destroy(jb);
}

/* ------------------------------------------------------------------------ */
/* Helpers */

static void helpers(void)
{
    jb_context *a = ctx(0, 0, 1, 0), *b = ctx(0, 0, 1, 0);
    int i;

    check("RGB15", JB_RGB15(31, 0, 0), 0x001f);
    check("RGB15 all", JB_RGB15(1, 2, 3), 1 | 2 << 5 | 3 << 10);
    check("RGB15 masks", JB_RGB15(32, 33, 31), 0 | 1 << 5 | 31 << 10);
    check("RGB_TO_15", JB_RGB_TO_15(JB_RGB(255, 128, 8)), 31 | 16 << 5 | 1 << 10);
    check("RGB_TO_15 low bits", JB_RGB_TO_15(JB_RGB(7, 7, 7)), 0);
    check("NEUTRAL", JB_NEUTRAL, 0x808080);
    check("TEXEL_TRANSPARENT", JB_TEXEL_TRANSPARENT, 0);
    check("TEXEL_BLACK", JB_TEXEL_BLACK, 0x8000);
    check("TEXEL_SEMI", JB_TEXEL_SEMI | JB_RGB15(1, 1, 1), 0x8421);
    check("MASK_BIT", JB_MASK_BIT, 0x8000);
    check("GP1 enable", JB_GP1_DISPLAY_ENABLE(1), 0x03000000);
    check("GP1 enable 5", JB_GP1_DISPLAY_ENABLE(5), 0x03000000);
    check("GP1 disable", JB_GP1_DISPLAY_ENABLE(0), 0x03000001);
    check("GP1 area", JB_GP1_DISPLAY_AREA(320, 240), 0x05000000 | 320 | 240 << 10);
    check("GP1 dma", JB_GP1_DMA(2), 0x04000002);
    check("GP1 mode 256", JB_GP1_DISPLAY_MODE(256, 240), 0x08000000);
    check("GP1 mode 320", JB_GP1_DISPLAY_MODE(320, 240), 0x08000001);
    check("GP1 mode 512", JB_GP1_DISPLAY_MODE(512, 240), 0x08000002);
    check("GP1 mode 640x480", JB_GP1_DISPLAY_MODE(640, 480), 0x08000027);
    check("GP1 mode 368", JB_GP1_DISPLAY_MODE(368, 240), 0x08000040);
    check("GP1 mode 24-bit", JB_GP1_DISPLAY_MODE(320, 240) | JB_GP1_MODE_24BIT, 0x08000011);
    check("GP1 constants", JB_GP1_RESET | JB_GP1_RESET_COMMANDS | JB_GP1_ACK_IRQ, 0x03000000);
    check("GP0 clear cache", JB_GP0_CLEAR_CACHE, 0x01000000);
    check("GP0 irq", JB_GP0_IRQ, 0x1f000000);
    check("GP0 xy", JB_GP0_XY(-1, 2), 0x0002ffff);
    /* The display modes through jb_gp1 give the sizes they name. */
    {
        static const int sizes[5][2] = {{256, 240}, {320, 240}, {368, 240}, {512, 480},
                                        {640, 480}};

        for (i = 0; i < 5; ++i) {
            int w, h;

            gp1(a, JB_GP1_DISPLAY_MODE(sizes[i][0], sizes[i][1]));
            jb_frame(a, &w, &h);
            check("mode size", w * 1000 + h, sizes[i][0] * 1000 + sizes[i][1]);
        }
        gp1(a, JB_GP1_RESET);
    }
    /* The state words against the calls. */
    gp0_1(a, JB_GP0_DRAW_AREA_TOP_LEFT(10, 20));
    gp0_1(a, JB_GP0_DRAW_AREA_BOTTOM_RIGHT(1000, 500));
    gp0_1(a, JB_GP0_DRAW_OFFSET(-1024, 1023));
    gp0_1(a, JB_GP0_TEXTURE_WINDOW(31, 1, 2, 30));
    gp0_1(a, JB_GP0_MASK(1, 1));
    jb_set_draw_area(b, 10, 20, 1000, 500);
    jb_set_draw_offset(b, -1024, 1023);
    jb_set_texture_window(b, 31, 1, 2, 30);
    jb_set_mask(b, 1, 1);
    check("GPUSTAT after the words", jb_gpustat(a), jb_gpustat(b));
    for (i = 2; i <= 5; ++i) {
        gp1(a, 0x10000000u | (uint32_t)i);
        gp1(b, 0x10000000u | (uint32_t)i);
        check("GP1(10h) after the words", jb_gpuread(a), jb_gpuread(b));
    }
    /* The draw mode word against GPUSTAT's layout. */
    gp0_1(a, JB_GP0_DRAW_MODE(448, 256, JB_BLEND_SUB, 8, 1));
    check("DRAW_MODE", jb_gpustat(a) & 0x7ff, 7 | 1 << 4 | 2 << 5 | 1 << 7 | 1 << 9);
    gp0_1(a, JB_GP0_DRAW_MODE(0, 0, JB_BLEND_NONE, 15, 0));
    check("DRAW_MODE 15-bit", jb_gpustat(a) & 0x7ff, 2 << 7);
    gp0_1(a, JB_GP0_DRAW_MODE(64, 0, JB_BLEND_QUARTER, 4, 0));
    check("DRAW_MODE quarter", jb_gpustat(a) & 0x7ff, 1 | 3 << 5);
    jb_destroy(a);
    jb_destroy(b);
}

/* ------------------------------------------------------------------------ */
/* The sticky error */

static void sticky(void)
{
    jb_context *jb = ctx(0, 0, 1, 0);
    const char *d = NULL;
    jb_ot *ot;
    char first[200];

    check("nothing yet", jb_check(jb, &d), JB_OK);
    check_str("nothing's detail", d, "");
    check("no detail pointer", jb_check(jb, NULL), JB_OK);
    check("gp0 fails", jb_gp0(jb, NULL, 3), JB_ERROR_INVALID_ARGUMENT);
    snprintf(first, sizeof first, "%s", jb_error_string(jb));
    check("area fails", jb_set_draw_area(jb, 0, 0, 2000, 0), JB_ERROR_INVALID_ARGUMENT);
    check("scale fails", jb_set_scale(jb, 9), JB_ERROR_UNSUPPORTED);
    check("the first is kept", jb_check(jb, &d), JB_ERROR_INVALID_ARGUMENT);
    check_str("its detail", d, first);
    check("jb_error_string the last", strstr(jb_error_string(jb), "jb_set_scale") != NULL, 1);
    check("taken", jb_check(jb, &d), JB_OK);
    check_str("taken: no detail", d, "");
    /* Each kind latches. */
    check("unsupported first", jb_set_scale(jb, 9), JB_ERROR_UNSUPPORTED);
    check("gte read", jb_gte_read(jb, 64), 0);
    check("unsupported kept", jb_check(jb, &d), JB_ERROR_UNSUPPORTED);
    check("gte read alone", jb_gte_read(jb, -1), 0);
    check("gte read latches", jb_check(jb, NULL), JB_ERROR_INVALID_ARGUMENT);
    ot = jb_ot_create(jb, 1, 1, NULL);
    check("ot add", jb_ot_add_poly(ot, 5, NULL), JB_ERROR_INVALID_ARGUMENT);
    check("ot errors latch", jb_check(jb, &d), JB_ERROR_INVALID_ARGUMENT);
    check("ot detail", strstr(d, "jb_ot_add_poly") != NULL, 1);
    check("widescreen", jb_set_widescreen(jb, 1), JB_ERROR_UNSUPPORTED);
    check("end without begin", jb_end_frame(jb), JB_ERROR_INVALID_ARGUMENT);
    check("both latched, first kept", jb_check(jb, NULL), JB_ERROR_UNSUPPORTED);
    /* jb_end_frame takes it: one before the frame, one in it. */
    jb_vram_fill(jb, 0, 0, 2000, 0, 0);
    check("begin", jb_begin_frame(jb, 0), JB_OK);
    jb_prim_poly(jb, NULL);
    check("end returns the first", jb_end_frame(jb), JB_ERROR_INVALID_ARGUMENT);
    check("and took it", jb_check(jb, NULL), JB_OK);
    check("begin 2", jb_begin_frame(jb, 0), JB_OK);
    check("a clean frame", jb_end_frame(jb), JB_OK);
    check("NULL", jb_check(NULL, &d), JB_ERROR_INVALID_ARGUMENT);
    check_str("NULL's detail", d, "no context");
    jb_ot_destroy(ot);
    jb_destroy(jb);
}

/* ------------------------------------------------------------------------ */
/* The frame */

static void frame_errors(void)
{
    jb_context *jb = ctx(0, 0, 1, 0);
    jb_rect r;
    jb_line l;
    jb_poly p;
    jb_polyline pl;

    memset(&r, 0, sizeof r);
    memset(&l, 0, sizeof l);
    memset(&p, 0, sizeof p);
    memset(&pl, 0, sizeof pl);
    p.vertices = 3;
    check("begin NULL", jb_begin_frame(NULL, 0), JB_ERROR_INVALID_ARGUMENT);
    check("end NULL", jb_end_frame(NULL), JB_ERROR_INVALID_ARGUMENT);
    check("layer NULL", jb_set_layer(NULL, 0), JB_ERROR_INVALID_ARGUMENT);
    check("draw rect NULL", jb_draw_rect(NULL, &r), JB_ERROR_INVALID_ARGUMENT);
    check("end before begin", jb_end_frame(jb), JB_ERROR_INVALID_ARGUMENT);
    check("draw outside a frame", jb_draw_rect(jb, &r), JB_ERROR_INVALID_ARGUMENT);
    check("its detail", strstr(jb_error_string(jb), "jb_begin_frame") != NULL, 1);
    check("poly outside", jb_draw_poly(jb, &p), JB_ERROR_INVALID_ARGUMENT);
    check("line outside", jb_draw_line(jb, &l), JB_ERROR_INVALID_ARGUMENT);
    check("clear colour", jb_begin_frame(jb, 0x1000000u), JB_ERROR_INVALID_ARGUMENT);
    check("begin", jb_begin_frame(jb, 0), JB_OK);
    check("begin again", jb_begin_frame(jb, 0), JB_ERROR_INVALID_ARGUMENT);
    check("layer 2", jb_set_layer(jb, 2), JB_ERROR_INVALID_ARGUMENT);
    check("layer -1", jb_set_layer(jb, -1), JB_ERROR_INVALID_ARGUMENT);
    check("poly NULL", jb_draw_poly(jb, NULL), JB_ERROR_INVALID_ARGUMENT);
    check("line NULL", jb_draw_line(jb, NULL), JB_ERROR_INVALID_ARGUMENT);
    check("polyline NULL", jb_draw_polyline(jb, NULL), JB_ERROR_INVALID_ARGUMENT);
    check("rect NULL", jb_draw_rect(jb, NULL), JB_ERROR_INVALID_ARGUMENT);
    p.vertices = 5;
    check("vertices 5", jb_draw_poly(jb, &p), JB_ERROR_INVALID_ARGUMENT);
    p.vertices = 3;
    p.blend = 9;
    check("blend 9", jb_draw_poly(jb, &p), JB_ERROR_INVALID_ARGUMENT);
    p.blend = 0;
    p.v[0].color = 0x1000000u;
    check("colour", jb_draw_poly(jb, &p), JB_ERROR_INVALID_ARGUMENT);
    p.v[0].color = 0;
    {
        jb_texture t;

        memset(&t, 0, sizeof t);
        t.page_x = 32;
        t.depth = 4;
        p.texture = &t;
        check("texture", jb_draw_poly(jb, &p), JB_ERROR_INVALID_ARGUMENT);
        t.page_x = 64;
        p.v[1].u = 256;
        check("texel", jb_draw_poly(jb, &p), JB_ERROR_INVALID_ARGUMENT);
        p.v[1].u = 0;
        r.texture = &t;
        r.u = -1;
        check("rect texel", jb_draw_rect(jb, &r), JB_ERROR_INVALID_ARGUMENT);
        r.u = 0;
        r.texture = NULL;
        p.texture = NULL;
    }
    l.blend = 5;
    check("line blend", jb_draw_line(jb, &l), JB_ERROR_INVALID_ARGUMENT);
    l.blend = 0;
    pl.count = 1;
    pl.v = l.v;
    check("polyline count 1", jb_draw_polyline(jb, &pl), JB_ERROR_INVALID_ARGUMENT);
    pl.count = 2;
    pl.v = NULL;
    check("polyline v NULL", jb_draw_polyline(jb, &pl), JB_ERROR_INVALID_ARGUMENT);
    r.w = -1;
    check("rect w -1", jb_draw_rect(jb, &r), JB_ERROR_INVALID_ARGUMENT);
    r.w = 10;
    r.h = -3;
    check("rect h -3", jb_draw_rect(jb, &r), JB_ERROR_INVALID_ARGUMENT);
    /* Anything in range for a position is not an error. */
    r.h = 10;
    r.x = -0x7fffffff;
    r.y = 0x7fffffff;
    r.w = 0x7fffffff;
    check("far rect", jb_draw_rect(jb, &r), JB_OK);
    p.v[0].x = 0x7fffffff;
    p.v[1].y = -0x7fffffff;
    p.v[2].x = -0x7fffffff;
    p.vertices = 4;
    p.v[3].y = 0x7fffffff;
    check("far poly", jb_draw_poly(jb, &p), JB_OK);
    l.v[0].x = -0x7fffffff;
    l.v[1].x = 0x7fffffff;
    check("far line", jb_draw_line(jb, &l), JB_OK);
    pl.v = p.v;
    pl.count = 4;
    check("far polyline", jb_draw_polyline(jb, &pl), JB_OK);
    jb_check(jb, NULL);
    check("end", jb_end_frame(jb), JB_OK);
    /* Geometry and the CLUT cache switches. */
    check("geometry 3", jb_set_geometry(jb, 3), JB_ERROR_INVALID_ARGUMENT);
    check("geometry float", jb_set_geometry(jb, JB_GEOMETRY_FLOAT), JB_OK);
    check("geometry gte", jb_set_geometry(jb, JB_GEOMETRY_GTE), JB_OK);
    check("geometry NULL", jb_set_geometry(NULL, 0), JB_ERROR_INVALID_ARGUMENT);
    check("clut cache NULL", jb_set_clut_cache(NULL, 0), JB_ERROR_INVALID_ARGUMENT);
    check("widescreen NULL", jb_set_widescreen(NULL, 0), JB_ERROR_INVALID_ARGUMENT);
    check("widescreen off unreserved", jb_set_widescreen(jb, 0), JB_OK);
    check("widescreen on unreserved", jb_set_widescreen(jb, 1), JB_ERROR_UNSUPPORTED);
    check("its detail", strstr(jb_error_string(jb), "jb_config.widescreen") != NULL, 1);
    jb_destroy(jb);
}

/* Frames in turn: each buffer cleared to its frame's colour and shown. */
static void buffers(int w, int h, int x1, int y1, int two)
{
    jb_context *jb = ctx(w, h, 1, 0);
    int fw, fh, k;
    const uint8_t *f;
    char msg[80];

    check("display disabled at first", (jb_gpustat(jb) >> 23) & 1, 1);
    for (k = 0; k < 4; ++k) {
        uint32_t c = JB_RGB(8 * (k + 1), 16, 200 - 8 * k);
        int bx = (k & 1) && two ? x1 : 0, by = (k & 1) && two ? y1 : 0;

        check("begin", jb_begin_frame(jb, c), JB_OK);
        check("end", jb_end_frame(jb), JB_OK);
        snprintf(msg, sizeof msg, "%dx%d frame %d: its buffer cleared", w, h, k);
        check(msg, pixel(jb, bx + w - 1, by + h - 1), JB_RGB_TO_15(c));
        check(msg, pixel(jb, bx, by), JB_RGB_TO_15(c));
        if (two && k > 0) {
            uint32_t before = JB_RGB(8 * k, 16, 200 - 8 * (k - 1));

            snprintf(msg, sizeof msg, "%dx%d frame %d: the other buffer kept", w, h, k);
            check(msg, pixel(jb, (k & 1) ? 0 : x1, (k & 1) ? 0 : y1), JB_RGB_TO_15(before));
        }
        check("display enabled", (jb_gpustat(jb) >> 23) & 1, 0);
        f = jb_frame(jb, &fw, &fh);
        snprintf(msg, sizeof msg, "%dx%d frame %d: shown", w, h, k);
        check(msg, fw == w && fh == h && f[0] == widen5(JB_RGB_TO_15(c) & 31) &&
                       f[2] == widen5(JB_RGB_TO_15(c) >> 10 & 31), 1);
    }
    /* A display disabled later stays so. */
    gp1(jb, JB_GP1_DISPLAY_ENABLE(0));
    jb_begin_frame(jb, 0);
    jb_end_frame(jb);
    check("disabled stays disabled", (jb_gpustat(jb) >> 23) & 1, 1);
    jb_destroy(jb);
}

/* The clear writes only the buffer, through the mask settings, which it
   keeps. */
static void clear_exact(int w, int h, int widescreen)
{
    jb_context *jb = ctx(w, h, 2, widescreen);
    uint16_t *before = must_alloc((size_t)VRAM_PIXELS * 2), *after = must_alloc((size_t)VRAM_PIXELS * 2);
    uint32_t s = 1;
    int i, k, bad = 0, fw;
    char msg[80];

    for (i = 0; i < VRAM_PIXELS; ++i) {
        s = s * 1103515245u + 12345u;
        before[i] = (uint16_t)(s >> 15);
    }
    jb_vram_load(jb, 0, 0, VRAM_W, VRAM_H, before);
    jb_set_mask(jb, 1, 1);
    for (k = 0; k < 2; ++k) {
        int bx = 0, by = 0, x, y;

        jb_vram_read(jb, 0, 0, VRAM_W, VRAM_H, before);
        check("begin", jb_begin_frame(jb, JB_RGB(200, 100, 50)), JB_OK);
        jb_end_frame(jb);
        jb_frame(jb, &fw, NULL);
        fw /= 2;
        if (k == 1) {
            if (2 * h <= 512)
                by = h;
            else
                bx = widescreen ? (w > 427 ? w : 427) : w;
            if (2 * h > 512 && 2 * bx > 1024)
                bx = 0;
        }
        jb_vram_read(jb, 0, 0, VRAM_W, VRAM_H, after);
        for (y = 0; y < VRAM_H; ++y)
            for (x = 0; x < VRAM_W; ++x) {
                int in = x >= bx && x < bx + fw && y >= by && y < by + h;
                uint16_t want = in ? JB_RGB_TO_15(JB_RGB(200, 100, 50)) : before[y * VRAM_W + x];

                bad += after[y * VRAM_W + x] != want;
            }
        snprintf(msg, sizeof msg, "clear %dx%d%s frame %d: the buffer and only it", w, h,
                 widescreen ? " widescreen" : "", k);
        check(msg, bad, 0);
    }
    check("mask settings kept", (jb_gpustat(jb) >> 11) & 3, 3);
    free(before);
    free(after);
    jb_destroy(jb);
}

/* The displayed frame's rightmost pixel's red byte, and the width. */
static int shown(jb_context *jb, int *w)
{
    int h;
    const uint8_t *f = jb_frame(jb, w, &h);

    return f[(size_t)(*w - 1) * 4];
}

static void widescreen(void)
{
    jb_context *jb = ctx(320, 240, 1, 1);
    int w, h;

    jb_frame(jb, &w, &h);
    check("starts 16:9", w * 1000 + h, 427240);
    check("begin", jb_begin_frame(jb, JB_RGB(255, 255, 255)), JB_OK);
    jb_end_frame(jb);
    check("427 cleared", pixel(jb, 426, 0), 0x7fff);
    jb_begin_frame(jb, JB_RGB(255, 255, 255));
    jb_end_frame(jb);
    check("427 cleared, second buffer", pixel(jb, 426, 240), 0x7fff);
    /* After frames: the change waits for the next frame's flip. */
    check("off", jb_set_widescreen(jb, 0), JB_OK);
    check("the shown frame stays at 427", shown(jb, &w) == 255 && w == 427, 1);
    check("begin 320", jb_begin_frame(jb, JB_RGB(0, 0, 0)), JB_OK);
    check("still 427 while drawing", shown(jb, &w) == 255 && w == 427, 1);
    check("on in a frame", jb_set_widescreen(jb, 1), JB_OK);
    jb_end_frame(jb);
    check("320 from its flip", shown(jb, &w) == 0 && w == 320, 1);
    check("cleared 320 only", pixel(jb, 320, 0) == 0x7fff && pixel(jb, 319, 0) == 0, 1);
    /* The next frame is drawn at 427, but until its flip the
       display shows the 320 frame at 320, not with 107 columns of the
       other buffer beside it. */
    jb_begin_frame(jb, JB_RGB(0, 0, 0));
    check("before the flip: 320, the frame shown", shown(jb, &w) == 0 && w == 320, 1);
    jb_end_frame(jb);
    check("427 from the flip", shown(jb, &w) == 0 && w == 427, 1);
    check("cleared 427", pixel(jb, 426, 240), 0);
    gp1(jb, JB_GP1_RESET);
    jb_frame(jb, &w, &h);
    check("GP1(00h) returns to 16:9", w, 427);
    jb_destroy(jb);
    /* Width above the 16:9 one: both fit; before the first frame the
       display follows at once. */
    jb = ctx(512, 240, 1, 1);
    jb_frame(jb, &w, &h);
    check("16:9 narrower than width", w, 427);
    jb_set_widescreen(jb, 0);
    jb_frame(jb, &w, &h);
    check("back to 512 at once", w, 512);
    jb_destroy(jb);
}

static jb_rect solid(int x, int y, int w, int h, uint32_t c)
{
    jb_rect r;

    memset(&r, 0, sizeof r);
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    r.color = c;
    return r;
}

static void layers_and_state(void)
{
    jb_context *jb = ctx(320, 240, 1, 0);
    jb_rect r;
    uint32_t win;

    jb_set_texture_window(jb, 8, 4, 1, 2);
    gp1(jb, 0x10000002u);
    win = jb_gpuread(jb);
    check("begin", jb_begin_frame(jb, 0), JB_OK);
    r = solid(0, 0, 10, 10, JB_RGB(255, 0, 0));
    check("overlay rect", jb_draw_rect(jb, &r), JB_OK);
    check("to background", jb_set_layer(jb, JB_LAYER_BACKGROUND), JB_OK);
    r = solid(0, 0, 20, 20, JB_RGB(0, 255, 0));
    check("background rect", jb_draw_rect(jb, &r), JB_OK);
    /* A console call lands at once: under the friendly draws. */
    r.color = JB_RGB(255, 255, 255);
    check("prim now", jb_prim_rect(jb, &r), JB_OK);
    check("console pixel at once", pixel(jb, 5, 5), 0x7fff);
    /* The console level moves the offset and area; the frame puts them
       back for its records. */
    jb_set_draw_offset(jb, 500, 300);
    jb_set_draw_area(jb, 600, 300, 700, 400);
    {
        jb_texture t;

        memset(&t, 0, sizeof t);
        t.page_x = 640;
        t.depth = JB_TEX_15BIT;
        r = solid(50, 0, 8, 8, JB_NEUTRAL);
        r.texture = &t;
        check("textured overlay", jb_set_layer(jb, JB_LAYER_OVERLAY) == JB_OK &&
                                      jb_draw_rect(jb, &r) == JB_OK, 1);
    }
    check("end", jb_end_frame(jb), JB_OK);
    check("overlay over background", pixel(jb, 5, 5), JB_RGB_TO_15(JB_RGB(255, 0, 0)));
    check("background over the console draw", pixel(jb, 15, 15), JB_RGB_TO_15(JB_RGB(0, 255, 0)));
    gp1(jb, 0x10000002u);
    check("window put back", jb_gpuread(jb), win);
    gp1(jb, 0x10000005u);
    check("offset the buffer's", jb_gpuread(jb), 0);
    gp1(jb, 0x10000004u);
    check("area the buffer's", jb_gpuread(jb), 319 | 239 << 10);
    /* The second frame (buffer at y 240): begin resets the layer. */
    jb_set_layer(jb, JB_LAYER_BACKGROUND);
    jb_begin_frame(jb, 0);
    r = solid(0, 0, 10, 10, JB_RGB(255, 255, 0));
    jb_draw_rect(jb, &r);
    r = solid(0, 0, 10, 10, JB_RGB(0, 255, 255));
    jb_set_layer(jb, JB_LAYER_BACKGROUND);
    jb_draw_rect(jb, &r);
    jb_end_frame(jb);
    check("layer reset to overlay", pixel(jb, 5, 245), JB_RGB_TO_15(JB_RGB(255, 255, 0)));
    jb_destroy(jb);
}

/* An allocator that fails when told to. */
static int fail_next;

static void *alloc_fn(void *user, size_t size)
{
    (void)user;
    if (fail_next) {
        fail_next = 0;
        return NULL;
    }
    return malloc(size);
}

static void free_fn(void *user, void *p)
{
    (void)user;
    free(p);
}

static void frame_memory(void)
{
    jb_config c;
    jb_context *jb;

    memset(&c, 0, sizeof c);
    c.alloc = alloc_fn;
    c.free = free_fn;
    c.max_primitives = 3;
    jb = must_create(&c);
    fail_next = 1;
    check("records' memory fails", jb_begin_frame(jb, 0), JB_ERROR_OUT_OF_MEMORY);
    check("not in a frame", jb_end_frame(jb), JB_ERROR_INVALID_ARGUMENT);
    check("then it works", jb_begin_frame(jb, 0), JB_OK);
    {
        jb_vertex v[5];
        jb_polyline l;

        memset(v, 0, sizeof v);
        l.gouraud = 0;
        l.blend = 0;
        l.v = v;
        l.count = 5;
        v[1].x = 10;
        v[2].y = 10;
        v[3].x = 20;
        v[4].y = 20;
        check("four segments in three records", jb_draw_polyline(jb, &l),
              JB_ERROR_OUT_OF_MEMORY);
    }
    check("end", jb_end_frame(jb), JB_ERROR_OUT_OF_MEMORY);
    jb_destroy(jb);
}

/* ------------------------------------------------------------------------ */
/* 2D draws within the limits equal jb_prim_* */

static void gen_v(gen *g, jb_vertex *v)
{
    v->x = rnd_below(g, 360) - 20;
    v->y = rnd_below(g, 280) - 20;
    v->color = rnd(g) & 0xffffffu;
    v->u = rnd_below(g, 256);
    v->v = rnd_below(g, 256);
}

/* friendly: jb_draw_*; else jb_prim_* at once, in the frame. */
static void scene2d(jb_context *jb, int friendly, uint64_t seed, int project)
{
    gen g;
    jb_texture tex;
    int i;

    g.s = seed;
    g.cx = g.cy = 0;
    memset(&tex, 0, sizeof tex);
    tex.page_x = 640;
    tex.depth = JB_TEX_8BIT;
    tex.clut_x = 0;
    tex.clut_y = 500;
    check("begin", jb_begin_frame(jb, JB_RGB(10, 20, 30)), JB_OK);
    if (project) {
        /* GTE projections onto the pixels the vertices use. */
        jb_gte_write(jb, JB_GTE_RT11RT12, JB_GTE_PAIR(4096, 0));
        jb_gte_write(jb, JB_GTE_RT22RT23, JB_GTE_PAIR(4096, 0));
        jb_gte_write(jb, JB_GTE_RT33, 4096);
        jb_gte_write(jb, JB_GTE_H, 300);
        jb_gte_write(jb, JB_GTE_OFX, 160u << 16);
        jb_gte_write(jb, JB_GTE_OFY, 120u << 16);
        for (i = 0; i < 4000; ++i) {
            jb_gte_write(jb, JB_GTE_VXY0, JB_GTE_PAIR(rnd_below(&g, 1200) - 600, rnd_below(&g, 900) - 450));
            jb_gte_write(jb, JB_GTE_VZ0, 700 + (uint32_t)rnd_below(&g, 50));
            jb_gte_command(jb, JB_GTE_RTPS);
        }
        g.s = seed;
    }
    for (i = 0; i < 300; ++i) {
        int k = rnd_below(&g, 4), j;
        jb_result r;

        if (k == 0) {
            jb_poly p;

            memset(&p, 0, sizeof p);
            p.vertices = 3 + rnd_below(&g, 2);
            p.gouraud = rnd_below(&g, 2);
            p.blend = rnd_below(&g, 5);
            p.texture = rnd_below(&g, 2) ? &tex : NULL;
            p.raw = rnd_below(&g, 2);
            for (j = 0; j < 4; ++j)
                gen_v(&g, &p.v[j]);
            r = friendly ? jb_draw_poly(jb, &p) : jb_prim_poly(jb, &p);
        } else if (k == 1) {
            jb_line l;

            l.gouraud = rnd_below(&g, 2);
            l.blend = rnd_below(&g, 5);
            gen_v(&g, &l.v[0]);
            gen_v(&g, &l.v[1]);
            r = friendly ? jb_draw_line(jb, &l) : jb_prim_line(jb, &l);
        } else if (k == 2) {
            jb_vertex v[6];
            jb_polyline l;

            l.gouraud = rnd_below(&g, 2);
            l.blend = rnd_below(&g, 5);
            l.count = (size_t)(2 + rnd_below(&g, 5));
            l.v = v;
            for (j = 0; j < (int)l.count; ++j)
                gen_v(&g, &v[j]);
            r = friendly ? jb_draw_polyline(jb, &l) : jb_prim_polyline(jb, &l);
        } else {
            jb_rect q;

            memset(&q, 0, sizeof q);
            q.x = rnd_below(&g, 360) - 20;
            q.y = rnd_below(&g, 280) - 20;
            q.w = rnd_below(&g, 3) ? rnd_below(&g, 40) : 16;
            q.h = rnd_below(&g, 3) ? rnd_below(&g, 40) : 16;
            q.color = rnd(&g) & 0xffffffu;
            q.blend = rnd_below(&g, 5);
            q.texture = rnd_below(&g, 2) ? &tex : NULL;
            q.u = rnd_below(&g, 256);
            q.v = rnd_below(&g, 256);
            q.flip_x = rnd_below(&g, 2);
            q.flip_y = rnd_below(&g, 2);
            r = friendly ? jb_draw_rect(jb, &q) : jb_prim_rect(jb, &q);
        }
        check("2D call", r, JB_OK);
    }
    check("end", jb_end_frame(jb), JB_OK);
}

static void same_vram(const char *what, jb_context *a, jb_context *b)
{
    uint16_t *va = read_vram(a), *vb = read_vram(b);
    int w, h, w2, h2;
    const uint8_t *f;
    uint8_t *copy;
    size_t n;
    char msg[160];

    snprintf(msg, sizeof msg, "%s: VRAM", what);
    check(msg, memcmp(va, vb, (size_t)VRAM_PIXELS * 2) == 0, 1);
    f = jb_frame(a, &w, &h);
    n = (size_t)w * (size_t)h * 4;
    copy = must_alloc(n);
    memcpy(copy, f, n);
    f = jb_frame(b, &w2, &h2);
    snprintf(msg, sizeof msg, "%s: frame", what);
    check(msg, w == w2 && h == h2 && memcmp(copy, f, n) == 0, 1);
    free(copy);
    free(va);
    free(vb);
}

static void draw_equals_prim(int scale, int options, int project, int threads)
{
    jb_config c;
    jb_context *a, *b;
    int k;
    char what[96];

    memset(&c, 0, sizeof c);
    c.scale = scale;
    c.threads = threads;
    c.precise_vertices = c.perspective_correct = options;
    a = must_create(&c);
    b = must_create(&c);
    for (k = 0; k < 2; ++k) {
        uint16_t pal[256];
        int i;

        for (i = 0; i < 256; ++i)
            pal[i] = (uint16_t)(i * 129 + 7);
        jb_vram_load(k ? b : a, 0, 500, 256, 1, pal);
    }
    for (k = 0; k < 3; ++k) {
        scene2d(a, 1, 1000 + (uint64_t)k, project);
        scene2d(b, 0, 1000 + (uint64_t)k, 0);
    }
    snprintf(what, sizeof what, "jb_draw_* = jb_prim_* at %dx, options %d, projections %d, %dt",
             scale, options, project, threads);
    same_vram(what, a, b);
    jb_destroy(a);
    jb_destroy(b);
}

/* ------------------------------------------------------------------------ */
/* The ordering table */

static void ordering_table(void)
{
    jb_context *jb = ctx(320, 240, 1, 0), *other = ctx(320, 240, 1, 0);
    jb_result r = JB_OK;
    jb_ot *ot;
    jb_rect q;
    jb_poly p;
    jb_line l;
    jb_vertex v[3];
    jb_polyline pl;

    jb_set_draw_area(jb, 0, 0, 1023, 511);
    check("create NULL", jb_ot_create(NULL, 4, 4, &r) == NULL && r == JB_ERROR_INVALID_ARGUMENT, 1);
    check("create buckets 0", jb_ot_create(jb, 0, 4, &r) == NULL && r == JB_ERROR_INVALID_ARGUMENT, 1);
    check("create records 0", jb_ot_create(jb, 4, 0, NULL) == NULL, 1);
    check("create buckets too many", jb_ot_create(jb, (1 << 20) + 1, 4, NULL) == NULL, 1);
    check("create records too many", jb_ot_create(jb, 1, (1 << 24) + 1, NULL) == NULL, 1);
    ot = jb_ot_create(jb, 4, 6, &r);
    check("create", ot != NULL && r == JB_OK, 1);
    check("clear NULL", jb_ot_clear(NULL), JB_ERROR_INVALID_ARGUMENT);
    jb_ot_destroy(NULL);
    q = solid(0, 0, 10, 10, JB_RGB(255, 0, 0));
    check("add to bucket 0", jb_ot_add_rect(ot, 0, &q), JB_OK); /* drawn last */
    q = solid(0, 0, 10, 10, JB_RGB(0, 255, 0));
    check("add to bucket 3", jb_ot_add_rect(ot, 3, &q), JB_OK);
    q = solid(20, 0, 10, 10, JB_RGB(0, 0, 255));
    check("bucket 2 first", jb_ot_add_rect(ot, 2, &q), JB_OK);
    q = solid(20, 0, 10, 10, JB_RGB(255, 255, 0));
    check("bucket 2 second", jb_ot_add_rect(ot, 2, &q), JB_OK);
    check("bucket 4", jb_ot_add_rect(ot, 4, &q), JB_ERROR_INVALID_ARGUMENT);
    check("bucket -1", jb_ot_add_rect(ot, -1, &q), JB_ERROR_INVALID_ARGUMENT);
    check("rect NULL", jb_ot_add_rect(ot, 0, NULL), JB_ERROR_INVALID_ARGUMENT);
    check("ot NULL", jb_ot_add_rect(NULL, 0, &q), JB_ERROR_INVALID_ARGUMENT);
    q.x = 1024;
    check("range rule", jb_ot_add_rect(ot, 0, &q), JB_ERROR_INVALID_ARGUMENT);
    check("range detail", strstr(jb_error_string(jb), "jb_ot_add_rect") != NULL, 1);
    memset(&p, 0, sizeof p);
    p.vertices = 3;
    p.v[0].x = -1025;
    check("poly range", jb_ot_add_poly(ot, 0, &p), JB_ERROR_INVALID_ARGUMENT);
    check("poly NULL", jb_ot_add_poly(ot, 0, NULL), JB_ERROR_INVALID_ARGUMENT);
    memset(&l, 0, sizeof l);
    l.blend = 6;
    check("line blend", jb_ot_add_line(ot, 0, &l), JB_ERROR_INVALID_ARGUMENT);
    check("line NULL", jb_ot_add_line(ot, 0, NULL), JB_ERROR_INVALID_ARGUMENT);
    l.blend = 0;
    check("line", jb_ot_add_line(ot, 1, &l), JB_OK);
    memset(v, 0, sizeof v);
    pl.gouraud = 0;
    pl.blend = 0;
    pl.v = v;
    pl.count = 3;
    check("polyline NULL", jb_ot_add_polyline(ot, 0, NULL), JB_ERROR_INVALID_ARGUMENT);
    check("polyline bucket", jb_ot_add_polyline(ot, 9, &pl), JB_ERROR_INVALID_ARGUMENT);
    check("polyline: 2 segments, 1 left", jb_ot_add_polyline(ot, 0, &pl), JB_ERROR_OUT_OF_MEMORY);
    pl.count = 2;
    check("polyline: 1 segment", jb_ot_add_polyline(ot, 0, &pl), JB_OK);
    q.x = 20;
    check("full", jb_ot_add_rect(ot, 0, &q) == JB_ERROR_OUT_OF_MEMORY &&
                      strstr(jb_error_string(jb), "max_records") != NULL, 1);
    check("draw NULL", jb_ot_draw(jb, NULL), JB_ERROR_INVALID_ARGUMENT);
    check("draw NULL context", jb_ot_draw(NULL, ot), JB_ERROR_INVALID_ARGUMENT);
    check("draw on another", jb_ot_draw(other, ot), JB_ERROR_INVALID_ARGUMENT);
    check("draw", jb_ot_draw(jb, ot), JB_OK);
    check("highest bucket first", pixel(jb, 5, 5), JB_RGB_TO_15(JB_RGB(255, 0, 0)));
    check("FIFO within a bucket", pixel(jb, 25, 5), JB_RGB_TO_15(JB_RGB(255, 255, 0)));
    check("clear", jb_ot_clear(ot), JB_OK);
    q = solid(0, 0, 10, 10, JB_RGB(8, 8, 8));
    check("add after clear", jb_ot_add_rect(ot, 3, &q), JB_OK);
    check("draw again", jb_ot_draw(jb, ot), JB_OK);
    check("cleared: one record", pixel(jb, 5, 5) == JB_RGB_TO_15(JB_RGB(8, 8, 8)) &&
                                     pixel(jb, 25, 5) == JB_RGB_TO_15(JB_RGB(255, 255, 0)), 1);
    jb_ot_destroy(ot);
    jb_destroy(jb);
    jb_destroy(other);
}

/* ------------------------------------------------------------------------ */
/* The CLUT cache switch */

static void clut_cache(void)
{
    int on;

    for (on = 0; on < 2; ++on) {
        jb_context *jb = ctx(320, 240, 1, 0);
        jb_texture t;
        jb_rect r;
        uint16_t pal[16], texel = 0x1111; /* four texels of index 1 */
        int i;

        memset(&t, 0, sizeof t);
        t.page_x = 640;
        t.depth = JB_TEX_4BIT;
        t.clut_y = 480;
        jb_set_draw_area(jb, 0, 0, 1023, 511);
        jb_vram_load(jb, 640, 0, 1, 1, &texel);
        for (i = 0; i < 16; ++i)
            pal[i] = JB_RGB15(31, 0, 0);
        jb_vram_load(jb, 0, 480, 16, 1, pal);
        check("switch", jb_set_clut_cache(jb, on), JB_OK);
        r = solid(0, 0, 4, 1, JB_NEUTRAL);
        r.texture = &t;
        r.raw = 1;
        jb_prim_rect(jb, &r);
        for (i = 0; i < 16; ++i)
            pal[i] = JB_RGB15(0, 31, 0);
        jb_vram_load(jb, 0, 480, 16, 1, pal);
        r.y = 1;
        jb_prim_rect(jb, &r);
        check(on ? "cache on: the old palette" : "cache off: the new palette", pixel(jb, 0, 1),
              on ? JB_RGB15(31, 0, 0) : JB_RGB15(0, 31, 0));
        jb_destroy(jb);
    }
    /* Off from the configuration. */
    {
        jb_config c;
        jb_context *jb;
        jb_texture t;
        jb_rect r;
        uint16_t pal = JB_RGB15(31, 0, 0), texel = 0;

        memset(&c, 0, sizeof c);
        c.clut_cache_off = 1;
        jb = must_create(&c);
        memset(&t, 0, sizeof t);
        t.page_x = 640;
        t.depth = JB_TEX_4BIT;
        t.clut_y = 480;
        jb_set_draw_area(jb, 0, 0, 1023, 511);
        jb_vram_load(jb, 640, 0, 1, 1, &texel);
        jb_vram_load(jb, 0, 480, 1, 1, &pal);
        r = solid(0, 0, 1, 1, JB_NEUTRAL);
        r.texture = &t;
        r.raw = 1;
        jb_prim_rect(jb, &r);
        pal = JB_RGB15(0, 0, 31);
        jb_vram_load(jb, 0, 480, 1, 1, &pal);
        r.y = 1;
        jb_prim_rect(jb, &r);
        check("clut_cache_off", pixel(jb, 0, 1), JB_RGB15(0, 0, 31));
        jb_destroy(jb);
    }
}

/* ------------------------------------------------------------------------ */
/* Frame edge cases, each of which failed before its fix */

static int max_red(jb_context *jb, int w, int h)
{
    uint16_t *v = read_vram(jb);
    int x, y, m = 0;

    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x)
            m = (v[y * VRAM_W + x] & 31) > m ? (v[y * VRAM_W + x] & 31) : m;
    free(v);
    return m;
}

/* An untextured primitive's u and v are not read, whatever they are. */
static void unused_uv(void)
{
    jb_context *a = ctx(320, 240, 1, 0), *b = ctx(320, 240, 1, 0);
    jb_line l;
    jb_poly p;
    uint16_t *va, *vb;
    int k;

    for (k = 0; k < 2; ++k) {
        jb_context *jb = k ? b : a;

        memset(&l, 0, sizeof l);
        l.gouraud = 1;
        l.v[0].x = -2000;
        l.v[0].y = 100;
        l.v[0].color = JB_RGB(255, 0, 0);
        l.v[1].x = 2000;
        l.v[1].y = 100;
        l.v[1].color = JB_RGB(0, 0, 255);
        memset(&p, 0, sizeof p);
        p.vertices = 3;
        p.gouraud = 1;
        p.v[0].x = -3000;
        p.v[1].x = 3000;
        p.v[1].y = 50;
        p.v[2].y = 2000;
        p.v[0].color = JB_RGB(0, 255, 0);
        p.v[2].color = JB_RGB(0, 0, 255);
        if (k == 0) {
            l.v[0].u = -0x7fffffff - 1;
            l.v[1].u = 0x7fffffff;
            l.v[0].v = 0x7fffffff;
            p.v[0].u = 0x7fffffff;
            p.v[1].u = -0x7fffffff - 1;
            p.v[2].v = -0x7fffffff - 1;
        }
        jb_begin_frame(jb, 0);
        check("unused uv: line", jb_draw_line(jb, &l), JB_OK);
        check("unused uv: poly", jb_draw_poly(jb, &p), JB_OK);
        check("unused uv: end", jb_end_frame(jb), JB_OK);
    }
    va = read_vram(a);
    vb = read_vram(b);
    check("unused uv: unused u and v change nothing", memcmp(va, vb, (size_t)VRAM_PIXELS * 2), 0);
    free(va);
    free(vb);
    jb_destroy(a);
    jb_destroy(b);
}

/* A 2D textured primitive draws with its texture's window, not the
   console's, which comes back after the frame. */
static void window_word(void)
{
    jb_context *jb = ctx(320, 240, 1, 0);
    jb_texture t;
    jb_rect r;
    uint16_t red = JB_RGB15(31, 0, 0), green = JB_RGB15(0, 31, 0);

    memset(&t, 0, sizeof t);
    t.page_x = 512;
    t.depth = JB_TEX_15BIT;
    jb_vram_load(jb, 512, 0, 1, 1, &red);
    jb_vram_load(jb, 520, 0, 1, 1, &green);
    jb_set_texture_window(jb, 1, 0, 1, 0); /* u 0 reads texel 8 */
    jb_begin_frame(jb, 0);
    r = solid(0, 0, 1, 1, JB_NEUTRAL);
    r.texture = &t;
    r.raw = 1;
    check("window word: prim", jb_prim_rect(jb, &r), JB_OK);
    r.x = 1;
    check("window word: draw", jb_draw_rect(jb, &r), JB_OK);
    check("window word: end", jb_end_frame(jb), JB_OK);
    check("window word: jb_prim_rect through the console's window", pixel(jb, 0, 0), green);
    check("window word: jb_draw_rect with no window", pixel(jb, 1, 0), red);
    gp1(jb, 0x10000002u);
    check("window word: the console's window put back", jb_gpuread(jb), 1u | 1u << 10);
    jb_destroy(jb);
}

/* A quad with one half within the limits and one clipped draws the
   diagonal they share once. */
static void quad_diagonal(void)
{
    jb_context *jb = ctx(320, 240, 1, 0);
    jb_poly p;

    memset(&p, 0, sizeof p);
    p.vertices = 4;
    p.blend = JB_BLEND_ADD;
    p.v[0].x = 20;
    p.v[0].y = 0;
    p.v[1].x = -20;
    p.v[1].y = 80;
    p.v[2].x = 120;
    p.v[2].y = 13;
    p.v[3].x = 900;
    p.v[3].y = 700;
    p.v[0].color = JB_RGB(64, 0, 0);
    jb_begin_frame(jb, 0);
    check("quad diagonal: draw", jb_draw_poly(jb, &p), JB_OK);
    check("quad diagonal: end", jb_end_frame(jb), JB_OK);
    check("quad diagonal: the reported pixel once", pixel(jb, 97, 24) & 31, 8);
    check("quad diagonal: no pixel twice", max_red(jb, 320, 240), 8);
    jb_destroy(jb);
}

/* A line clipped to a 500-high frame is one piece: no pixel twice. */
static void tall_line(void)
{
    jb_context *jb = ctx(320, 500, 2, 0);
    jb_line l;
    uint16_t *v;
    const uint8_t *f;
    int y, x, w, h, wrong = 0, bad = 0;

    memset(&l, 0, sizeof l);
    l.blend = JB_BLEND_ADD;
    l.v[0].x = 50;
    l.v[0].y = -20;
    l.v[1].x = 50;
    l.v[1].y = 520;
    l.v[0].color = JB_RGB(64, 0, 0);
    jb_set_dither(jb, 0);
    jb_begin_frame(jb, 0);
    check("tall line: draw", jb_draw_line(jb, &l), JB_OK);
    check("tall line: end", jb_end_frame(jb), JB_OK);
    v = read_vram(jb);
    for (y = 0; y < 500; ++y)
        wrong += (v[y * VRAM_W + 50] & 31) != 8;
    check("tall line: every row of the line once", wrong, 0);
    free(v);
    /* And the scaled picture: no brighter block where a split was. */
    f = jb_frame(jb, &w, &h);
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x)
            bad += f[((size_t)y * w + x) * 4] > widen5(8);
    check("tall line: scaled picture once", bad, 0);
    jb_destroy(jb);
}

/* Jb_config's earlier fields keep their places. */
static int positional_calls;

static void positional_log(void *user, const char *message)
{
    (void)message;
    ++*(int *)user;
}

/* A program written before the new fields leaves them out: the point of
   the test, so the warning about it is off here. */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
static void positional_config(void)
{
    jb_config c = {1, 300, 200, 1, 0, 0, positional_log, &positional_calls};
    jb_result r;
    jb_context *jb = jb_create(&c, &r);
    int w, h;

    check("positional config: create", jb != NULL && r == JB_OK, 1);
    jb_frame(jb, &w, &h);
    check("positional config: width and height", w * 1000 + h, 300200);
    jb_set_scale(jb, 99);
    check("positional config: the log where it was", positional_calls, 1);
    jb_destroy(jb);
}

/* A polygon with no area draws nothing, clipped or not. */
static void zero_area(void)
{
    jb_context *jb = ctx(320, 240, 1, 0);
    jb_poly p;

    memset(&p, 0, sizeof p);
    p.vertices = 3;
    p.v[0].x = -1999;
    p.v[0].y = -700;
    p.v[1].x = 101;
    p.v[1].y = 0;
    p.v[2].x = 2201;
    p.v[2].y = 700;
    p.v[0].color = JB_RGB(255, 0, 0);
    jb_begin_frame(jb, 0);
    check("zero area: draw", jb_draw_poly(jb, &p), JB_OK);
    p.v[0].x = 11;
    p.v[0].y = 30;
    p.v[1].x = 101;
    p.v[1].y = 60;
    p.v[2].x = 191;
    p.v[2].y = 90;
    check("zero area: draw within the limits", jb_draw_poly(jb, &p), JB_OK);
    check("zero area: end", jb_end_frame(jb), JB_OK);
    check("zero area: nothing drawn", max_red(jb, 320, 240), 0);
    jb_destroy(jb);
}

/* No allocation in warmed frames when widescreen changes. */
static long scanout_allocs;

static void *scanout_alloc(void *user, size_t size)
{
    (void)user;
    ++scanout_allocs;
    return malloc(size);
}

static void scanout_memory(void)
{
    jb_config c;
    jb_context *jb;
    long warm;
    int k;

    memset(&c, 0, sizeof c);
    c.widescreen = 1;
    c.alloc = scanout_alloc;
    c.free = free_fn;
    jb = must_create(&c);
    jb_set_widescreen(jb, 0);
    for (k = 0; k < 2; ++k) {
        jb_begin_frame(jb, 0);
        if (k == 1)
            jb_set_widescreen(jb, 1);
        jb_end_frame(jb);
        jb_frame(jb, NULL, NULL);
    }
    warm = scanout_allocs;
    for (k = 0; k < 3; ++k) {
        jb_begin_frame(jb, 0);
        jb_set_widescreen(jb, k & 1);
        jb_end_frame(jb);
        jb_frame(jb, NULL, NULL);
    }
    check("scanout memory: no allocation after warm-up", scanout_allocs - warm, 0);
    jb_destroy(jb);
}

/* Jb_end_frame's result and jb_error_string's detail match. */
static void end_detail(void)
{
    jb_config c;
    jb_context *jb;
    jb_rect r;
    jb_poly p;

    memset(&c, 0, sizeof c);
    c.max_primitives = 1;
    jb = must_create(&c);
    jb_begin_frame(jb, 0);
    r = solid(0, 0, 4, 4, JB_RGB(255, 255, 255));
    check("end detail: first", jb_draw_rect(jb, &r), JB_OK);
    check("end detail: full", jb_draw_rect(jb, &r), JB_ERROR_OUT_OF_MEMORY);
    memset(&p, 0, sizeof p);
    p.vertices = 3;
    p.blend = 9;
    check("end detail: blend", jb_draw_poly(jb, &p), JB_ERROR_INVALID_ARGUMENT);
    check("end detail: the first failure", jb_end_frame(jb), JB_ERROR_OUT_OF_MEMORY);
    check("end detail: its detail", strstr(jb_error_string(jb), "max_primitives") != NULL, 1);
    check("end detail: taken", jb_check(jb, NULL), JB_OK);
    jb_destroy(jb);
}

/* ------------------------------------------------------------------------ */
/* More frame edge cases, and shared edges across the splitter */

/* A crossed quad within the limits draws what jb_prim_poly draws
   (its halves' areas cancel; it is not degenerate). */
static void crossed_quad(void)
{
    jb_context *a = ctx(320, 240, 1, 0), *b = ctx(320, 240, 1, 0);
    jb_poly p;
    uint16_t *va, *vb;

    memset(&p, 0, sizeof p);
    p.vertices = 4;
    p.blend = JB_BLEND_ADD;
    p.v[0].x = 10, p.v[0].y = 10;
    p.v[1].x = 30, p.v[1].y = 30;
    p.v[2].x = 10, p.v[2].y = 30;
    p.v[3].x = 30, p.v[3].y = 10;
    p.v[0].color = JB_RGB(64, 0, 0);
    jb_begin_frame(a, 0);
    check("crossed quad: draw", jb_draw_poly(a, &p), JB_OK);
    jb_end_frame(a);
    jb_begin_frame(b, 0);
    check("crossed quad: prim", jb_prim_poly(b, &p), JB_OK);
    jb_end_frame(b);
    check("crossed quad: the reported pixel", pixel(a, 13, 24) & 31, 8);
    va = read_vram(a);
    vb = read_vram(b);
    check("crossed quad: crossed quad = jb_prim_poly", memcmp(va, vb, (size_t)VRAM_PIXELS * 2), 0);
    free(va);
    free(vb);
    jb_destroy(a);
    jb_destroy(b);
}

/* A display mode set with GP1(08h) stays through a widescreen
   flip, until GP1(00h), which returns to the frame's width. */
static long counting_allocs;

static void *counting_alloc(void *user, size_t size)
{
    (void)user;
    ++counting_allocs;
    return malloc(size);
}

static void mode_precedence(void)
{
    jb_config c;
    jb_context *jb;
    long before;
    int w, h;

    memset(&c, 0, sizeof c);
    c.widescreen = 1;
    c.alloc = counting_alloc;
    c.free = free_fn;
    jb = must_create(&c);
    jb_set_widescreen(jb, 0);
    jb_begin_frame(jb, 0);
    jb_end_frame(jb);
    gp1(jb, JB_GP1_DISPLAY_MODE(256, 480) | JB_GP1_MODE_24BIT);
    jb_frame(jb, &w, &h);
    check("mode precedence: the mode's size", w * 1000 + h, 256480);
    before = counting_allocs;
    jb_set_widescreen(jb, 1);
    jb_begin_frame(jb, JB_RGB(255, 0, 0));
    jb_end_frame(jb);
    jb_frame(jb, &w, &h);
    check("mode precedence: the mode kept through the flip", w * 1000 + h, 256480);
    check("mode precedence: 24-bit kept", (jb_gpustat(jb) >> 21) & 1, 1);
    check("mode precedence: no allocation", counting_allocs - before, 0);
    gp1(jb, JB_GP1_RESET);
    jb_frame(jb, &w, &h);
    check("mode precedence: GP1(00h) returns to the frame's width", w * 1000 + h, 427240);
    jb_destroy(jb);
}

/* After a change of scale, once jb_frame has run at the new scale,
   a widescreen flip does not allocate. */
static void scale_then_widescreen(void)
{
    jb_config c;
    jb_context *jb;
    long warm;
    int k;

    memset(&c, 0, sizeof c);
    c.width = 32;
    c.height = 36;
    c.scale = 3;
    c.widescreen = 1;
    c.alloc = counting_alloc;
    c.free = free_fn;
    jb = must_create(&c);
    jb_set_widescreen(jb, 0);
    jb_begin_frame(jb, 0);
    jb_end_frame(jb);
    jb_frame(jb, NULL, NULL);
    check("scale then widescreen: scale", jb_set_scale(jb, 4), JB_OK);
    for (k = 0; k < 2; ++k) {
        jb_begin_frame(jb, 0);
        jb_end_frame(jb);
        jb_frame(jb, NULL, NULL);
    }
    warm = counting_allocs;
    jb_set_widescreen(jb, 1);
    for (k = 0; k < 2; ++k) {
        jb_begin_frame(jb, 0);
        jb_end_frame(jb);
        jb_frame(jb, NULL, NULL);
    }
    check("scale then widescreen: no allocation at the widescreen flip", counting_allocs - warm, 0);
    jb_destroy(jb);
}

/* A thin triangle across the coordinate range does not draw any
   pixel twice. */
static void thin_triangle(void)
{
    jb_context *jb = ctx(1024, 512, 1, 0);
    jb_poly p;

    memset(&p, 0, sizeof p);
    p.vertices = 3;
    p.blend = JB_BLEND_ADD;
    p.v[0].x = 1007, p.v[0].y = 378;
    p.v[1].x = -391, p.v[1].y = 1283;
    p.v[2].x = 2407, p.v[2].y = -524;
    p.v[0].color = JB_RGB(64, 0, 0);
    jb_begin_frame(jb, 0);
    check("thin triangle: draw", jb_draw_poly(jb, &p), JB_OK);
    jb_end_frame(jb);
    /* The reported pixel (1005, 379) lies just outside the exact triangle
       (edge A B is at y 379.3 there): it read 16 from two
       overlapping pieces; it is 0 now, as nothing should cover it. */
    check("thin triangle: the reported pixel, outside", pixel(jb, 1005, 379) & 31, 0);
    check("thin triangle: no pixel twice", max_red(jb, 1024, 512), 8);
    {
        /* Covered where the exact triangle is, by more than a pixel; not
           where it is not. */
        static const double t[6] = {1007, 378, -391, 1283, 2407, -524};
        uint16_t *v = read_vram(jb);
        int x, y, k, missing = 0, stray = 0;

        for (y = 0; y < 512; ++y)
            for (x = 0; x < 1023; ++x) {
                double d = 1e9;

                for (k = 0; k < 3; ++k) {
                    double ax = t[2 * k], ay = t[2 * k + 1];
                    double bx = t[(2 * k + 2) % 6], by = t[(2 * k + 3) % 6];
                    double cx = t[(2 * k + 4) % 6], cy = t[(2 * k + 5) % 6];
                    double e = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
                    double side = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
                    double len = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);
                    double dk = (side > 0 ? e : -e);

                    /* signed distance squared, kept signed */
                    dk = dk * (dk < 0 ? -dk : dk) / len;
                    d = dk < d ? dk : d;
                }
                if (d > 1.0)
                    missing += (v[y * VRAM_W + x] & 31) != 8;
                if (d < -1.0)
                    stray += (v[y * VRAM_W + x] & 31) != 0;
            }
        check("thin triangle: inside covered", missing, 0);
        check("thin triangle: outside not", stray, 0);
        free(v);
    }
    jb_destroy(jb);
}

/* Two triangles that share an edge exactly, one split and one not:
   every pixel along the shared edge drawn once (no crack, no overlap). */
static void check_shared_edge(jb_context *jb, int w, int h, int ax, int ay, int bx, int by,
                          const char *what)
{
    uint16_t *v = read_vram(jb);
    int x, y, wrong = 0, twice = 0;
    double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    char msg[96];

    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x) {
            double t = ((x - ax) * dx + (y - ay) * dy) / len2;
            double cr = (x - ax) * dy - (y - ay) * dx; /* distance x length */
            unsigned px = v[y * VRAM_W + x] & 31;

            twice += px > 8;
            if (t > 0.05 && t < 0.95 && cr * cr < len2)
                wrong += px != 8;
        }
    snprintf(msg, sizeof msg, "%s: along the shared edge, every pixel once", what);
    check(msg, wrong, 0);
    snprintf(msg, sizeof msg, "%s: no pixel twice", what);
    check(msg, twice, 0);
    free(v);
}

static void shared_edge_2d(void)
{
    /* A (150, -100), B (160, 200): ABC is far taller than 511, BAD fits. */
    static const int tri[2][6] = {{150, -100, 160, 200, -300, 700},
                                  {160, 200, 150, -100, 300, 50}};
    jb_context *jb = ctx(320, 240, 1, 0);
    jb_poly p;
    int k, i;

    jb_begin_frame(jb, 0);
    for (k = 0; k < 2; ++k) {
        memset(&p, 0, sizeof p);
        p.vertices = 3;
        p.blend = JB_BLEND_ADD;
        for (i = 0; i < 3; ++i) {
            p.v[i].x = tri[k][2 * i];
            p.v[i].y = tri[k][2 * i + 1];
        }
        p.v[0].color = JB_RGB(64, 0, 0);
        check("shared edge 2d: 2D draw", jb_draw_poly(jb, &p), JB_OK);
    }
    jb_end_frame(jb);
    check_shared_edge(jb, 320, 240, 150, -100, 160, 200, "2D");
    jb_destroy(jb);
}

/* The 3D case: default camera, ABC and BAD in a mesh, ABC taller
   than 511 on the screen, BAD within the limits; both geometry paths. */
static void shared_edge_3d(void)
{
    static const jb_gte_vec pos[4] = {
        {-4, 180, -100}, {-1, -45, -100}, {100, -210, -100}, {-100, 0, -100}};
    static const jb_mesh_face faces[2] = {{{0, 1, 2, 0}, 3, 0}, {{1, 0, 3, 0}, 3, 0}};
    jb_material m;
    jb_mesh mesh;
    int g;

    memset(&m, 0, sizeof m);
    m.color = JB_RGB(64, 0, 0);
    m.blend = JB_BLEND_ADD;
    m.unlit = 1;
    m.double_sided = 1;
    memset(&mesh, 0, sizeof mesh);
    mesh.vertex_count = 4;
    mesh.positions = pos;
    mesh.face_count = 2;
    mesh.faces = faces;
    mesh.material_count = 1;
    mesh.materials = &m;
    for (g = 0; g < 2; ++g) {
        jb_context *jb = ctx(320, 240, 1, 0);

        jb_set_geometry(jb, g ? JB_GEOMETRY_FLOAT : JB_GEOMETRY_GTE);
        jb_set_dither(jb, 0);
        jb_begin_frame(jb, 0);
        check("shared edge 3d: 3D draw", jb_draw_mesh(jb, &mesh, NULL), JB_OK);
        check("shared edge 3d: 3D end", jb_end_frame(jb), JB_OK);
        check(g ? "shared edge 3d: 3D float: the reported pixel once" : "shared edge 3d: 3D GTE: the reported pixel once",
              pixel(jb, 155, 10) & 31, 8);
        check(g ? "shared edge 3d: 3D float: no pixel twice" : "shared edge 3d: 3D GTE: no pixel twice",
              max_red(jb, 320, 240), 8);
        jb_destroy(jb);
    }
}

/* ------------------------------------------------------------------------ */
/* jb_frame_size: the size frames are drawn at, in 1x pixels */

static int frame_size(jb_context *jb)
{
    int w = -1, h = -1;

    check("jb_frame_size", jb_frame_size(jb, &w, &h), JB_OK);
    return w * 1000 + h;
}

static void frame_size_calls(void)
{
    jb_context *jb = ctx(0, 0, 2, 0);
    int w = 7, h = 7;

    check("frame size: default", frame_size(jb), 320240);
    check("frame size: NULL context", jb_frame_size(NULL, &w, &h), JB_ERROR_INVALID_ARGUMENT);
    check("frame size: NULL context zeroes", w * 1000 + h, 0);
    check("frame size: NULL pointers", jb_frame_size(jb, NULL, NULL), JB_OK);
    w = 0;
    check("frame size: width alone", jb_frame_size(jb, &w, NULL) == JB_OK && w == 320, 1);
    /* Not jb_frame's size: neither the scale nor a display mode. */
    gp1(jb, JB_GP1_DISPLAY_MODE(640, 480));
    check("frame size: not the display mode", frame_size(jb), 320240);
    jb_frame(jb, &w, &h);
    check("frame size: jb_frame shows the mode at 2x", w * 1000 + h, 1280960);
    jb_destroy(jb);

    jb = ctx(640, 480, 1, 0);
    check("frame size: 640 x 480", frame_size(jb), 640480);
    jb_destroy(jb);

    /* Widescreen: outside a frame the next frame's size at once; in a
       frame its own, the change from the next frame. */
    jb = ctx(320, 240, 1, 1);
    check("frame size: 16:9 reserved and on", frame_size(jb), 427240);
    jb_begin_frame(jb, 0);
    jb_end_frame(jb);
    check("frame size: off", jb_set_widescreen(jb, 0), JB_OK);
    check("frame size: the next frame's at once", frame_size(jb), 320240);
    jb_frame(jb, &w, &h);
    check("frame size: while the display still shows 427", w, 427);
    jb_begin_frame(jb, 0);
    check("frame size: in the frame", frame_size(jb), 320240);
    jb_set_widescreen(jb, 1);
    check("frame size: a change in a frame waits", frame_size(jb), 320240);
    jb_end_frame(jb);
    check("frame size: after the frame, the next", frame_size(jb), 427240);
    jb_begin_frame(jb, 0);
    check("frame size: the next frame", frame_size(jb), 427240);
    jb_end_frame(jb);
    jb_destroy(jb);
}

/* ------------------------------------------------------------------------ */
/* Blending each pixel once, and sprites far past the frame */

/* ------------------------------------------------------------------------ */
/* The splitter's invariant, as a property (src/geom/split.h): one
   primitive, or two that share an edge exactly, drawn additively over
   black, in a frame, at 1x and in the scaled picture with precise
   vertices: no pixel drawn twice, and every pixel well inside drawn. */

/* A shape: the triangles the GPU would draw of it, in pixels of the
   frame. */
typedef struct {
    double x[3], y[3];
} shape_tri;

static double seg_distance(double px, double py, double ax, double ay, double bx, double by)
{
    double dx = bx - ax, dy = by - ay, len = dx * dx + dy * dy, t, ex, ey, r;
    int it;

    t = len > 0 ? ((px - ax) * dx + (py - ay) * dy) / len : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    ex = ax + t * dx - px;
    ey = ay + t * dy - py;
    len = ex * ex + ey * ey;
    r = len > 1 ? len : 1;
    for (it = 0; it < 40; ++it) /* the square root, without libm */
        r = 0.5 * (r + len / r);
    return len == 0 ? 0 : r;
}

static int inside_tri(const shape_tri *t, double px, double py)
{
    double s = 0;
    int k, pos = 0, neg = 0;

    for (k = 0; k < 3; ++k) {
        int j = (k + 1) % 3;

        s = (t->x[j] - t->x[k]) * (py - t->y[k]) - (t->y[j] - t->y[k]) * (px - t->x[k]);
        pos += s > 0;
        neg += s < 0;
    }
    return pos == 0 || neg == 0;
}

/* Whether edge k of triangle i is shared by another triangle of the
   shape (the same two corners): not part of the union's boundary. */
static int shared_edge(const shape_tri *t, int nt, int i, int k)
{
    double ax = t[i].x[k], ay = t[i].y[k], bx = t[i].x[(k + 1) % 3], by = t[i].y[(k + 1) % 3];
    int j, m;

    for (j = 0; j < nt; ++j) {
        if (j == i)
            continue;
        for (m = 0; m < 3; ++m) {
            double cx = t[j].x[m], cy = t[j].y[m], dx = t[j].x[(m + 1) % 3],
                   dy = t[j].y[(m + 1) % 3];

            if ((ax == cx && ay == cy && bx == dx && by == dy) ||
                (ax == dx && ay == dy && bx == cx && by == cy))
                return 1;
        }
    }
    return 0;
}

/* Whether a point lies in the union of the triangles, more than 1.5 pixels
   from its boundary (the triangles' edges that no other shares). */
static int well_inside(const shape_tri *t, int nt, double px, double py)
{
    int i, k, in = 0;

    for (i = 0; i < nt && !in; ++i)
        in = inside_tri(&t[i], px, py);
    if (!in)
        return 0;
    for (i = 0; i < nt; ++i)
        for (k = 0; k < 3; ++k)
            if (!shared_edge(t, nt, i, k) &&
                seg_distance(px, py, t[i].x[k], t[i].y[k], t[i].x[(k + 1) % 3],
                             t[i].y[(k + 1) % 3]) <= 1.5)
                return 0;
    return 1;
}

/* The frame just drawn: the most a pixel was drawn (in 5-bit red: 8 once),
   and how many pixels well inside the shape are not drawn once; in the 1x
   picture (the frame shown at 1x, the 1x VRAM of a single-buffered frame
   above 1x, so the frame must be single-buffered there) and, above 1x,
   the scaled one, judged at pixel centres. */
static void shape_check(jb_context *jb, int w, int h, int scale, const shape_tri *t, int nt,
                        int *twice, int *missing)
{
    uint16_t *v = read_vram(jb);
    double bx0 = 1e9, by0 = 1e9, bx1 = -1e9, by1 = -1e9;
    int x, y, i, k, fw, fh;
    const uint8_t *f;

    for (i = 0; i < nt; ++i)
        for (k = 0; k < 3; ++k) {
            bx0 = t[i].x[k] < bx0 ? t[i].x[k] : bx0;
            by0 = t[i].y[k] < by0 ? t[i].y[k] : by0;
            bx1 = t[i].x[k] > bx1 ? t[i].x[k] : bx1;
            by1 = t[i].y[k] > by1 ? t[i].y[k] : by1;
        }
    *twice = 0;
    *missing = 0;
    f = jb_frame(jb, &fw, &fh);
    if (scale == 1)
        for (y = 0; y < h; ++y)
            for (x = 0; x < w; ++x)
                v[y * VRAM_W + x] = (uint16_t)(f[((size_t)y * fw + x) * 4] >> 3);
    for (y = 0; y < h; ++y)
        for (x = 0; x < w && x < 1023; ++x) {
            unsigned px = v[y * VRAM_W + x] & 31;

            *twice += px > 8;
            if (x >= bx0 && x <= bx1 && y >= by0 && y <= by1 && px != 8 &&
                well_inside(t, nt, x, y))
                ++*missing;
        }
    if (scale > 1) {
        f = jb_frame(jb, &fw, &fh);
        for (y = 0; y < fh; ++y)
            for (x = 0; x < fw && x < 1023 * scale; ++x) {
                unsigned px = f[((size_t)y * fw + x) * 4];
                double cx = (x + 0.5) / scale, cy = (y + 0.5) / scale;

                *twice += px > widen5(8);
                if (cx >= bx0 && cx <= bx1 && cy >= by0 && cy <= by1 && px != widen5(8) &&
                    well_inside(t, nt, cx, cy))
                    ++*missing;
            }
    }
    free(v);
}

/* Draws n polygons (3 or 4 vertices each, GPU order) in one frame,
   additive red 64, and checks the shape their triangles make. */
static void property_case(jb_context *jb, int w, int h, int scale, const int (*poly)[8],
                          const int *verts, int n, const char *what, int *twice, int *cracks)
{
    shape_tri t[4];
    int nt = 0, i, k, d, m;

    jb_begin_frame(jb, 0);
    for (i = 0; i < n; ++i) {
        jb_poly p;

        memset(&p, 0, sizeof p);
        p.vertices = verts[i];
        p.blend = JB_BLEND_ADD;
        for (k = 0; k < verts[i]; ++k) {
            p.v[k].x = poly[i][2 * k];
            p.v[k].y = poly[i][2 * k + 1];
        }
        p.v[0].color = JB_RGB(64, 0, 0);
        check(what, jb_draw_poly(jb, &p), JB_OK);
        for (m = 0; m + 2 < verts[i]; ++m) { /* the GPU's triangles: 0 1 2, 1 2 3 */
            for (k = 0; k < 3; ++k) {
                t[nt].x[k] = poly[i][2 * (m + k)];
                t[nt].y[k] = poly[i][2 * (m + k) + 1];
            }
            ++nt;
        }
    }
    jb_end_frame(jb);
    shape_check(jb, w, h, scale, t, nt, &d, &m);
    if (d && ++*twice <= 3) {
        printf("  %s twice (%d):", what, d);
        for (i = 0; i < n; ++i)
            for (k = 0; k < verts[i]; ++k)
                printf(" (%d, %d)", poly[i][2 * k], poly[i][2 * k + 1]);
        printf("\n");
    }
    if (m && ++*cracks <= 3) {
        printf("  %s cracked (%d):", what, m);
        for (i = 0; i < n; ++i)
            for (k = 0; k < verts[i]; ++k)
                printf(" (%d, %d)", poly[i][2 * k], poly[i][2 * k + 1]);
        printf("\n");
    }
}

/* A random point near the frame (span pixels around its middle). */
static void near_point(gen *g, int w, int h, int span, int *x, int *y)
{
    *x = rnd_below(g, span) - span / 2 + w / 2;
    *y = rnd_below(g, span) - span / 2 + h / 2;
}

/* No pixel blended twice: two fixed cases, then seeded runs
   of large triangles, slivers, nearly collinear triangles, convex quads
   and pairs of triangles sharing an edge (some of them thin along it). */
static void no_double_blend(void)
{
    static const int sizes[3][4] = {{320, 240, 1, 700}, {1024, 512, 1, 250}, {600, 300, 2, 120}};
    static const int fixtures[2][8] = {{175, 898, -376, -713, 371, 1476, 0, 0},
                                       {866, 920, 190, -640, 901, 1000, 864, -128}};
    gen g = {20261122u, 0, 0};
    int s, i, k;

    for (s = 0; s < 3; ++s) {
        int w = sizes[s][0], h = sizes[s][1], scale = sizes[s][2], runs = sizes[s][3];
        jb_context *jb = ctx(w, h, scale, 0);
        int twice = 0, cracks = 0;
        char what[64];

        if (scale > 1)
            jb_set_precise_vertices(jb, 1);
        snprintf(what, sizeof what, "property %dx%d %dx", w, h, scale);
        if (w == 1024) {
            static const int three = 3, four = 4;

            property_case(jb, w, h, scale, &fixtures[0], &three, 1, what, &twice, &cracks);
            property_case(jb, w, h, scale, &fixtures[1], &four, 1, what, &twice, &cracks);
            check("the fixed cases, once", twice, 0);
        }
        for (i = 0; i < runs; ++i) {
            int poly[2][8], verts[2] = {3, 3}, n = 1, kind = rnd_below(&g, 6);

            memset(poly, 0, sizeof poly);
            if (kind == 0) { /* large: anywhere in a wide range */
                int span = rnd_below(&g, 2) ? 6000 : 200000;

                for (k = 0; k < 6; ++k)
                    poly[0][k] = rnd_below(&g, span) - span / 2;
            } else if (kind == 1) { /* a sliver from an apex */
                near_point(&g, w, h, 4000, &poly[0][0], &poly[0][1]);
                near_point(&g, w, h, 8000, &poly[0][2], &poly[0][3]);
                poly[0][4] = poly[0][2] + rnd_below(&g, 13) - 6;
                poly[0][5] = poly[0][3] + rnd_below(&g, 13) - 6;
            } else if (kind == 2) { /* nearly collinear, far apart */
                int t, dx, dy, len, off;

                near_point(&g, w, h, 4000, &poly[0][0], &poly[0][1]);
                near_point(&g, w, h, 4000, &poly[0][2], &poly[0][3]);
                t = rnd_below(&g, 1025);
                dx = poly[0][2] - poly[0][0];
                dy = poly[0][3] - poly[0][1];
                len = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) + 1;
                off = rnd_below(&g, 5) - 2;
                poly[0][4] = poly[0][0] + (int)((long long)dx * t / 1024) -
                             (int)((long long)dy * off * 2 / len);
                poly[0][5] = poly[0][1] + (int)((long long)dy * t / 1024) +
                             (int)((long long)dx * off * 2 / len);
            } else if (kind == 3) {
                /* A convex quad: four corners in order round an ellipse,
                   given in the GPU's order (0 1 3 2 round it). */
                static const int c8[8][2] = {{1000, 0},   {707, 707},  {0, 1000},
                                             {-707, 707}, {-1000, 0},  {-707, -707},
                                             {0, -1000},  {707, -707}};
                int cx, cy, rx = 50 + rnd_below(&g, 3000), ry = 50 + rnd_below(&g, 3000);
                int a = rnd_below(&g, 2), corner[4][2];

                near_point(&g, w, h, 3000, &cx, &cy);
                for (k = 0; k < 4; ++k) {
                    corner[k][0] = cx + (int)((long long)rx * c8[(2 * k + a) % 8][0] / 1000);
                    corner[k][1] = cy + (int)((long long)ry * c8[(2 * k + a) % 8][1] / 1000);
                }
                verts[0] = 4;
                poly[0][0] = corner[0][0], poly[0][1] = corner[0][1];
                poly[0][2] = corner[1][0], poly[0][3] = corner[1][1];
                poly[0][4] = corner[3][0], poly[0][5] = corner[3][1];
                poly[0][6] = corner[2][0], poly[0][7] = corner[2][1];
            } else {
                /* Two triangles sharing an edge A B, C and D on either
                   side of it, now and then a pixel or two from it (kind 5:
                   thin along the edge). */
                int ax, ay, bx, by, dx, dy, len, t, side;

                near_point(&g, w, h, 4000, &ax, &ay);
                near_point(&g, w, h, 4000, &bx, &by);
                dx = bx - ax;
                dy = by - ay;
                len = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) + 1;
                n = 2;
                for (k = 0; k < 2; ++k) {
                    int off = kind == 5 ? 1 + rnd_below(&g, 3) : 20 + rnd_below(&g, 2000);

                    side = k ? -1 : 1;
                    t = rnd_below(&g, 1025);
                    poly[k][0] = k ? bx : ax, poly[k][1] = k ? by : ay;
                    poly[k][2] = k ? ax : bx, poly[k][3] = k ? ay : by;
                    poly[k][4] = ax + (int)((long long)dx * t / 1024) -
                                 side * (int)((long long)dy * off * 2 / len);
                    poly[k][5] = ay + (int)((long long)dy * t / 1024) +
                                 side * (int)((long long)dx * off * 2 / len);
                }
            }
            property_case(jb, w, h, scale, (const int(*)[8])poly, verts, n, what, &twice,
                          &cracks);
        }
        snprintf(what, sizeof what, "property %dx%d %dx: no pixel twice", w, h, scale);
        check(what, twice, 0);
        snprintf(what, sizeof what, "property %dx%d %dx: no crack", w, h, scale);
        check(what, cracks, 0);
        jb_destroy(jb);
    }
}

/* A sprite of INT_MAX width or height from a positive origin draws
   on its own side of the origin, to the frame's edge. */
static void giant_sprite(void)
{
    int axis;

    for (axis = 0; axis < 2; ++axis) {
        jb_context *jb = ctx(32, 32, 1, 0);
        jb_texture tex;
        jb_texture_desc d;
        jb_sprite sp;
        uint16_t red = JB_RGB15(31, 0, 0);

        memset(&d, 0, sizeof d);
        d.width = d.height = 1;
        d.depth = JB_TEX_15BIT;
        d.texels = &red;
        check("giant sprite: texture", jb_texture_create(jb, &d, &tex), JB_OK);
        memset(&sp, 0, sizeof sp);
        sp.texture = &tex;
        sp.x = 1;
        sp.y = 1;
        sp.w = axis ? 16 : 0x7fffffff;
        sp.h = axis ? 0x7fffffff : 16;
        sp.src_w = sp.src_h = 1;
        sp.raw = 1;
        jb_begin_frame(jb, 0);
        check("giant sprite: draw", jb_draw_sprite(jb, &sp), JB_OK);
        check("giant sprite: end", jb_end_frame(jb), JB_OK);
        check(axis ? "giant sprite: tall: inside drawn" : "giant sprite: wide: inside drawn",
              pixel(jb, 10, 10),
              red);
        check(axis ? "giant sprite: tall: far inside drawn" : "giant sprite: wide: far inside drawn",
              pixel(jb, axis ? 10 : 30, axis ? 30 : 10), red);
        check(axis ? "giant sprite: tall: before the origin not" : "giant sprite: wide: before the origin not",
              pixel(jb, axis ? 10 : 0, axis ? 0 : 10), 0);
        jb_destroy(jb);
    }
}

/* ------------------------------------------------------------------------ */
/* Slivers, clipped quads, dropped polygons and 2D affine draws */

/* A piece whose integer triangle the 1x rounding turns over keeps its
   sub-pixel coverage in the scaled picture. The case: projection
   200, 4x, precise vertices, A B C and B A D sharing an edge longer than
   511 pixels on the screen; scanout (328, 800) lies in B A D's right-hand
   sliver. Additive red 64: once is 8 in 5 bits. */
static void precise_sliver(void)
{
    static const jb_gte_vec pos[4] = {
        {-80, 300, -200}, {-77, -280, -200}, {-120, 0, -200}, {-78, 0, -200}};
    static const jb_mesh_face faces[2] = {{{0, 1, 2, 0}, 3, 0}, {{1, 0, 3, 0}, 3, 0}};
    jb_camera cam;
    jb_material m;
    jb_mesh mesh;
    int g;

    memset(&cam, 0, sizeof cam);
    cam.target.z = -1;
    cam.projection = 200;
    memset(&m, 0, sizeof m);
    m.color = JB_RGB(64, 0, 0);
    m.blend = JB_BLEND_ADD;
    m.unlit = 1;
    m.double_sided = 1;
    memset(&mesh, 0, sizeof mesh);
    mesh.vertex_count = 4;
    mesh.positions = pos;
    mesh.face_count = 2;
    mesh.faces = faces;
    mesh.material_count = 1;
    mesh.materials = &m;
    for (g = 0; g < 2; ++g) {
        jb_context *jb = ctx(320, 240, 4, 0);
        const uint8_t *f;
        int w, h, x, y, twice = 0;

        jb_set_precise_vertices(jb, 1);
        jb_set_geometry(jb, g ? JB_GEOMETRY_FLOAT : JB_GEOMETRY_GTE);
        check("precise sliver: camera", jb_set_camera(jb, &cam), JB_OK);
        jb_begin_frame(jb, 0);
        check("precise sliver: draw", jb_draw_mesh(jb, &mesh, NULL), JB_OK);
        check("precise sliver: end", jb_end_frame(jb), JB_OK);
        f = jb_frame(jb, &w, &h);
        check(g ? "precise sliver: float: scanout (328, 800) covered once" : "precise sliver: GTE: scanout (328, 800) covered once",
              f[((size_t)800 * w + 328) * 4], widen5(8));
        for (y = 0; y < h; ++y)
            for (x = 0; x < w; ++x)
                twice += f[((size_t)y * w + x) * 4] > widen5(8);
        check(g ? "precise sliver: float: no pixel twice" : "precise sliver: GTE: no pixel twice",
              twice, 0);
        jb_destroy(jb);
    }
}

/* A clipped convex quad's two triangles draw no pixel twice across
   their diagonal. */
static void clipped_quad_diagonal(void)
{
    jb_context *jb = ctx(1024, 512, 1, 0);
    jb_poly p;

    memset(&p, 0, sizeof p);
    p.vertices = 4;
    p.blend = JB_BLEND_ADD;
    p.v[0].x = 866, p.v[0].y = 920;
    p.v[1].x = 190, p.v[1].y = -640;
    p.v[2].x = 901, p.v[2].y = 1000;
    p.v[3].x = 864, p.v[3].y = -128;
    p.v[0].color = JB_RGB(64, 0, 0);
    jb_begin_frame(jb, 0);
    check("clipped quad diagonal: draw", jb_draw_poly(jb, &p), JB_OK);
    jb_end_frame(jb);
    check("clipped quad diagonal: the reported pixel once", pixel(jb, 565, 224) & 31, 8);
    check("clipped quad diagonal: no pixel twice", max_red(jb, 1024, 512), 8);
    jb_destroy(jb);
}

/* A polygon that can draw no pixel of the frame is dropped with
   JB_OK, and takes no record: here it would not fit. */
static void offscreen_dropped(void)
{
    jb_config c;
    jb_context *jb;
    jb_rect r;
    jb_poly p;

    memset(&c, 0, sizeof c);
    c.width = 32;
    c.height = 32;
    c.max_primitives = 1;
    jb = must_create(&c);
    jb_begin_frame(jb, 0);
    r = solid(0, 0, 1, 1, JB_RGB(255, 255, 255));
    check("offscreen dropped: the one record", jb_draw_rect(jb, &r), JB_OK);
    memset(&p, 0, sizeof p);
    p.vertices = 3;
    p.v[0].x = 32, p.v[0].y = 4; /* columns 32 and on: none of the frame's */
    p.v[1].x = 33, p.v[1].y = 4;
    p.v[2].x = 32, p.v[2].y = 8;
    check("offscreen dropped: right of the frame, dropped", jb_draw_poly(jb, &p), JB_OK);
    p.v[0].x = 4, p.v[0].y = -9; /* rows above 0 */
    p.v[1].x = 20, p.v[1].y = -9;
    p.v[2].x = 4, p.v[2].y = 0;
    check("offscreen dropped: above the frame, dropped", jb_draw_poly(jb, &p), JB_OK);
    check("offscreen dropped: no error", jb_end_frame(jb), JB_OK);
    jb_destroy(jb);
}

/* A 2D draw stays affine where the console-level call finds GTE
   depths for its vertices (jitterbug.h, 2D drawing). The case: 1x,
   perspective-correct on, precise vertices off, three GTE projections at
   depths 256, 512, 256, and a raw 15-bit triangle on their positions with
   u 0, 31, 0 over a texture whose red is its column. */
static void affine_2d(void)
{
    int k;
    uint16_t red_at[2];

    for (k = 0; k < 2; ++k) {
        jb_context *jb = ctx(320, 240, 1, 0);
        jb_texture t;
        jb_poly p;
        uint16_t row[32];
        int i;

        for (i = 0; i < 32; ++i)
            row[i] = JB_RGB15(i, 0, 0);
        jb_vram_load(jb, 512, 0, 32, 1, row);
        jb_set_perspective_correct(jb, 1);
        jb_begin_frame(jb, 0);
        jb_gte_write(jb, JB_GTE_RT11RT12, JB_GTE_PAIR(4096, 0));
        jb_gte_write(jb, JB_GTE_RT22RT23, JB_GTE_PAIR(4096, 0));
        jb_gte_write(jb, JB_GTE_RT33, 4096);
        jb_gte_write(jb, JB_GTE_H, 256);
        {
            static const int16_t vtx[3][3] = {{4, 4, 256}, {56, 8, 512}, {4, 28, 256}};

            for (i = 0; i < 3; ++i) {
                jb_gte_write(jb, JB_GTE_VXY0, JB_GTE_PAIR(vtx[i][0], vtx[i][1]));
                jb_gte_write(jb, JB_GTE_VZ0, (uint32_t)vtx[i][2]);
                jb_gte_command(jb, JB_GTE_RTPS);
            }
        }
        memset(&t, 0, sizeof t);
        t.page_x = 512;
        t.depth = JB_TEX_15BIT;
        memset(&p, 0, sizeof p);
        p.vertices = 3;
        p.texture = &t;
        p.raw = 1;
        p.v[0].x = 4, p.v[0].y = 4;
        p.v[1].x = 28, p.v[1].y = 4, p.v[1].u = 31;
        p.v[2].x = 4, p.v[2].y = 28;
        check("affine 2d: draw", k ? jb_prim_poly(jb, &p) : jb_draw_poly(jb, &p), JB_OK);
        jb_end_frame(jb);
        red_at[k] = (uint16_t)(pixel(jb, 16, 8) & 31);
        jb_destroy(jb);
    }
    check("affine 2d: jb_draw_poly affine", red_at[0], 15);
    check("affine 2d: jb_prim_poly with the GTE's depths", red_at[1], 10);
}

/* ------------------------------------------------------------------------ */
/* Off-screen polygons, coverage and faces cut at the guard band */

/* A 32 x 32 frame holding one record, a 1 x 1 rectangle: what can draw no
   pixel of the frame must return JB_OK and take no record: a
   triangle whose bounds reach the frame but that lies beyond its
   lower-right corner (x + y >= 70 everywhere), and a line on
   row -1, which no scale draws in the frame. */
static void offscreen_exact(void)
{
    jb_config c;
    jb_context *jb;
    jb_rect r;
    jb_poly p;
    jb_line l;

    memset(&c, 0, sizeof c);
    c.width = 32;
    c.height = 32;
    c.max_primitives = 1;
    c.scale = 2;
    jb = must_create(&c);
    jb_set_precise_vertices(jb, 1);
    jb_begin_frame(jb, 0);
    r = solid(0, 0, 1, 1, JB_RGB(255, 255, 255));
    check("offscreen exact: the one record", jb_draw_rect(jb, &r), JB_OK);
    memset(&p, 0, sizeof p);
    p.vertices = 3;
    p.v[0].x = 30, p.v[0].y = 40;
    p.v[1].x = 40, p.v[1].y = 30;
    p.v[2].x = 40, p.v[2].y = 40;
    check("offscreen exact: beyond the corner, dropped", jb_draw_poly(jb, &p), JB_OK);
    memset(&l, 0, sizeof l);
    l.v[0].x = 4, l.v[0].y = -1;
    l.v[1].x = 8, l.v[1].y = -1;
    check("offscreen exact: a line on row -1, dropped", jb_draw_line(jb, &l), JB_OK);
    check("offscreen exact: no error", jb_end_frame(jb), JB_OK);
    jb_destroy(jb);
}

/* A crossed quad whose first triangle fits keeps all of that
   triangle's coverage, and a thick triangle has no hole deep inside.
   Additive red 64 over black (8 in 5 bits once, 16 twice). */
static void crossed_coverage(void)
{
    jb_context *jb = ctx(600, 512, 1, 0);
    jb_poly p;

    memset(&p, 0, sizeof p);
    p.vertices = 4;
    p.blend = JB_BLEND_ADD;
    p.v[0].x = 100, p.v[0].y = 300;
    p.v[1].x = 100, p.v[1].y = 100;
    p.v[2].x = 450, p.v[2].y = 100;
    p.v[3].x = 500, p.v[3].y = 1000;
    p.v[0].color = JB_RGB(64, 0, 0);
    jb_begin_frame(jb, 0);
    check("crossed coverage: draw", jb_draw_poly(jb, &p), JB_OK);
    jb_end_frame(jb);
    check("crossed coverage: the first triangle's pixel drawn", pixel(jb, 110, 200) & 31, 8);
    jb_destroy(jb);
    jb = ctx(1024, 512, 1, 0);
    memset(&p, 0, sizeof p);
    p.vertices = 3;
    p.blend = JB_BLEND_ADD;
    p.v[0].x = 1417, p.v[0].y = -1188;
    p.v[1].x = 1021, p.v[1].y = -410;
    p.v[2].x = -813, p.v[2].y = 1621;
    p.v[0].color = JB_RGB(64, 0, 0);
    jb_begin_frame(jb, 0);
    check("crossed coverage: draw", jb_draw_poly(jb, &p), JB_OK);
    jb_end_frame(jb);
    check("crossed coverage: deep inside, drawn once", pixel(jb, 622, 10) & 31, 8);
    check("crossed coverage: no pixel twice", max_red(jb, 1024, 512), 8);
    /* Its pieces were drawn with the area of each half of the frame
       (512 high: two tiles), and the whole buffer's is back after. */
    gp1(jb, 0x10000003u);
    check("crossed coverage: tiles: the area's top left the buffer's after", jb_gpuread(jb), 0);
    gp1(jb, 0x10000004u);
    check("crossed coverage: tiles: the area's bottom right the buffer's after", jb_gpuread(jb),
          1023u | 511u << 10);
    jb_destroy(jb);
}

/* A one-face mesh, unlit and double-sided, of the given colour and blend,
   drawn with the camera at the origin looking down -z at projection proj. */
static void draw_face(jb_context *jb, const jb_gte_vec *pos, int n, uint32_t color, int blend,
                    float proj)
{
    jb_mesh_face f;
    jb_material mat;
    jb_mesh m;
    jb_camera cam;
    jb_mat4 id = jb_mat4_translate(0, 0, 0);
    int i;

    memset(&cam, 0, sizeof cam);
    cam.target.z = -1.0f;
    cam.up.y = 1.0f;
    cam.projection = proj;
    check("draw face: camera", jb_set_camera(jb, &cam), JB_OK);
    memset(&f, 0, sizeof f);
    for (i = 0; i < n; ++i)
        f.v[i] = (uint16_t)i;
    if (n == 4) { /* positions in the GPU's order: the face goes 0 1 3 2 */
        f.v[2] = 3;
        f.v[3] = 2;
    }
    f.vertices = (uint16_t)n;
    memset(&mat, 0, sizeof mat);
    mat.color = color;
    mat.blend = blend;
    mat.unlit = 1;
    mat.double_sided = 1;
    memset(&m, 0, sizeof m);
    m.vertex_count = n;
    m.positions = pos;
    m.face_count = 1;
    m.faces = &f;
    m.material_count = 1;
    m.materials = &mat;
    check("draw face: mesh", jb_draw_mesh(jb, &m, &id), JB_OK);
}

/* A 3D triangle flat by its integer positions (x 80 at all three)
   but not by its sub-pixel ones (80.75 in the middle), too tall for one
   GPU triangle: the scaled picture with precise vertices still draws it,
   in both geometry paths. */
static void flat_in_one_format(void)
{
    static const jb_gte_vec pos[3] = {{-320, 1120, -400}, {-317, 0, -400}, {-320, -1120, -400}};
    int mode;

    for (mode = 0; mode < 2; ++mode) {
        jb_context *jb = ctx(320, 240, 4, 0);
        const uint8_t *f;
        int w, h;

        jb_set_precise_vertices(jb, 1);
        jb_set_geometry(jb, mode ? JB_GEOMETRY_FLOAT : JB_GEOMETRY_GTE);
        jb_begin_frame(jb, 0);
        draw_face(jb, pos, 3, JB_RGB(255, 0, 0), JB_BLEND_NONE, 100.0f);
        check("flat in one format: end", jb_end_frame(jb), JB_OK);
        f = jb_frame(jb, &w, &h);
        check(mode ? "flat in one format: float: the precise sliver drawn" : "flat in one format: GTE: the precise sliver drawn",
              f[((size_t)480 * w + 321) * 4] > 0, 1);
        jb_destroy(jb);
    }
}

/* A quad face the 3D pipeline cuts at its guard band into its two
   GPU triangles (one corner projects below it), each beyond the GPU's
   limits. In the face order 0 1 2 3 the quad is crossed: the
   console's triangles (p0 p1 p3 and p1 p3 p2) both contain (531, 163)
   exactly, so it blends twice there, as on the console (additive red 64:
   66 once, 132 twice). Going round it convexly (0 1 3 2), its triangles
   are disjoint and the two polygons, clipped together, draw no pixel
   twice. */
static void face_halves(void)
{
    static const jb_gte_vec pos[4] = {
        {351, -685, -200}, {-342, 850, -200}, {431, -828, -200}, {420, 415, -200}};
    int order;

    for (order = 0; order < 2; ++order) {
        jb_context *jb = ctx(1024, 512, 1, 0);
        jb_gte_vec q[4];
        const uint8_t *f;
        int w, h, x, y, most = 0;

        memcpy(q, pos, sizeof q);
        if (order == 0) { /* the face 0 1 2 3: draw_face takes GPU order */
            q[2] = pos[3];
            q[3] = pos[2];
        }
        jb_set_geometry(jb, JB_GEOMETRY_FLOAT);
        jb_begin_frame(jb, 0);
        draw_face(jb, q, 4, JB_RGB(64, 0, 0), JB_BLEND_ADD, 200.0f);
        check("face halves: end", jb_end_frame(jb), JB_OK);
        f = jb_frame(jb, &w, &h);
        for (y = 0; y < h; ++y)
            for (x = 0; x < w; ++x)
                most = f[((size_t)y * w + x) * 4] > most ? f[((size_t)y * w + x) * 4] : most;
        if (order == 0) {
            check("face halves: crossed: inside both triangles, twice as on the console",
                  f[((size_t)163 * w + 531) * 4], widen5(16));
        } else {
            check("face halves: convex: the reported pixel once", f[((size_t)163 * w + 531) * 4],
                  widen5(8));
            check("face halves: convex: no pixel twice", most, widen5(8));
        }
        jb_destroy(jb);
    }
}

/* ------------------------------------------------------------------------ */
/* Diagonals reset at the clip rectangle */

/* A convex quad (GPU order) whose kept diagonal crosses the clip
   rectangle where one triangle's cut cannot go inward: the diagonal is
   then cut at its nearest points by both triangles, and every cut of them
   rounded against that. Before, resetting one cut carried it past
   another of the upper triangle, which was left out ((50, 200) black);
   and the diagonal was reset at one side only, leaving a crack
   along it. Additive red 64 over black, at 1x and above it without
   precise vertices. */
static void diagonal_reset(void)
{
    static const struct {
        int w, h, q[8], at[4][2];
    } cases[2] = {
        {320, 240, {-400, 572, -340, 470, 673, 60, 0, 120}, {{50, 200}, {50, 200}, {50, 200}, {50, 200}}},
        {1023, 511, {500, 100, -10, 500, 200, -1, -40, 519}, {{15, 441}, {14, 443}, {9, 455}, {0, 476}}},
    };
    int c, scale, k;

    for (c = 0; c < 2; ++c)
        for (scale = 1; scale <= 2; ++scale) {
            jb_context *jb = ctx(cases[c].w, cases[c].h, scale, 0);
            const uint8_t *f;
            int w, h, x, y, most = 0;
            jb_poly p;
            char what[96];

            memset(&p, 0, sizeof p);
            p.vertices = 4;
            p.blend = JB_BLEND_ADD;
            for (k = 0; k < 4; ++k) {
                p.v[k].x = cases[c].q[2 * k];
                p.v[k].y = cases[c].q[2 * k + 1];
            }
            p.v[0].color = JB_RGB(64, 0, 0);
            jb_begin_frame(jb, 0);
            check("diagonal reset: draw", jb_draw_poly(jb, &p), JB_OK);
            jb_end_frame(jb);
            f = jb_frame(jb, &w, &h);
            for (k = 0; k < 4; ++k) {
                int px = cases[c].at[k][0], py = cases[c].at[k][1];

                snprintf(what, sizeof what,
                         "diagonal reset: case %d %dx: (%d, %d) inside drawn once", c + 1, scale,
                         px, py);
                if (scale == 1)
                    check(what, pixel(jb, px, py) & 31, 8);
                check(what, f[((size_t)py * scale * w + (size_t)px * scale) * 4], widen5(8));
            }
            for (y = 0; y < h; ++y)
                for (x = 0; x < w; ++x)
                    most = f[((size_t)y * w + x) * 4] > most ? f[((size_t)y * w + x) * 4] : most;
            snprintf(what, sizeof what, "diagonal reset: case %d %dx: no pixel twice", c + 1,
                     scale);
            check(what, most, widen5(8));
            jb_destroy(jb);
        }
}

int main(void)
{
    config_fields();
    helpers();
    sticky();
    frame_errors();
    buffers(320, 240, 0, 240, 1);
    buffers(320, 480, 320, 0, 1);
    buffers(640, 480, 0, 0, 0);
    buffers(1024, 512, 0, 0, 0);
    clear_exact(300, 240, 0);
    clear_exact(320, 240, 1);
    clear_exact(333, 300, 0);
    clear_exact(1024, 512, 0);
    clear_exact(7, 3, 0);
    widescreen();
    layers_and_state();
    frame_memory();
    draw_equals_prim(1, 0, 0, 1);
    draw_equals_prim(2, 0, 0, 4);
    draw_equals_prim(3, 1, 0, 1);
    draw_equals_prim(2, 1, 1, 1);
    ordering_table();
    clut_cache();
    unused_uv();
    window_word();
    quad_diagonal();
    tall_line();
    positional_config();
    zero_area();
    scanout_memory();
    end_detail();
    crossed_quad();
    mode_precedence();
    scale_then_widescreen();
    thin_triangle();
    shared_edge_2d();
    shared_edge_3d();
    frame_size_calls();
    no_double_blend();
    giant_sprite();
    precise_sliver();
    clipped_quad_diagonal();
    offscreen_dropped();
    affine_2d();
    offscreen_exact();
    crossed_coverage();
    flat_in_one_format();
    face_halves();
    diagonal_reset();
    return finish("api_friendly");
}

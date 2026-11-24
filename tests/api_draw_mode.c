/*
 * api_draw_mode: what the drawing-state helpers keep of the draw mode.
 *
 * jb_set_dither changes GP0(E1h)'s bit 9 and nothing else: not the page,
 * blend mode, depth, bit 10 or the flips, and not GPUSTAT bit 15, which
 * GP0(E1h) bit 11 latches while GP1(09h) allows it. A GP0(E1h) cannot keep
 * that latch (with GP1(09h)'s gate shut it stores 0), so the setter sends
 * none. The GP0(E1h) that jb_prim_rect and the blending jb_prim_* calls
 * send is a helper-only state update that keeps the latch ("the draw mode
 * as it is"); a textured jb_prim_poly sends its page with bit 11 clear,
 * which stores 0 as the console's page does (include/jitterbug.h, jb_gp1
 * 09h). The same E1h words sent through jb_gp0 do what the console does
 * with them: bit 15 becomes their bit 11 with the gate set, 0 with it
 * shut (helper_vs_raw).
 *
 * (The cases that failed before: GP1(09h) with 1, GP0(E1h) with bit 11, then
 * jb_set_dither(1) read bit 15 as 0, with the gate set or cleared; and
 * with the latch set and the gate shut, a blended line drawn by
 * jb_prim_line read bit 15 as 1, the same line after a raw GP0(E1h) with
 * bit 11 set as 0.)
 */
#include "api_test.h"

static uint32_t stat_bit(jb_context *jb, int bit)
{
    return (jb_gpustat(jb) >> bit) & 1u;
}

static void word(jb_context *jb, uint32_t w)
{
    gp0(jb, &w, 1);
}

static void gp1w(jb_context *jb, uint32_t w)
{
    check("jb_gp1", jb_gp1(jb, w), JB_OK);
}

/* GPUSTAT's draw-mode bits (0-10) and bit 15. */
static uint32_t mode_bits(jb_context *jb)
{
    return jb_gpustat(jb) & (0x7ffu | 1u << 15);
}

static void dither_keeps(int gate_after)
{
    char what[96];
    jb_context *jb = must_create(NULL);
    /* page 5, page y 1, blend 2, depth 1, bit 10, bit 11, both flips */
    uint32_t e1 = 0xe1000000u | 5u | 1u << 4 | 2u << 5 | 1u << 7 | 1u << 10 | 1u << 11 |
                  1u << 12 | 1u << 13;
    uint32_t before;
    int on;

    gp1w(jb, 0x09000001u);
    word(jb, e1);
    if (!gate_after)
        gp1w(jb, 0x09000000u);
    before = mode_bits(jb);
    snprintf(what, sizeof what, "gate %s: GPUSTAT bit 15 latched", gate_after ? "set" : "shut");
    check(what, stat_bit(jb, 15), 1);
    for (on = 1; on >= 0; --on) {
        check("jb_set_dither", jb_set_dither(jb, on ? 7 : 0), JB_OK);
        snprintf(what, sizeof what, "gate %s: jb_set_dither(%d) sets bit 9",
                 gate_after ? "set" : "shut", on);
        check(what, stat_bit(jb, 9), (uint32_t)on);
        snprintf(what, sizeof what, "gate %s: jb_set_dither(%d) keeps the rest and bit 15",
                 gate_after ? "set" : "shut", on);
        check(what, mode_bits(jb) & ~(1u << 9), before & ~(1u << 9));
    }
    jb_destroy(jb);
}

/* Bit 15 after a helper that sends GP0(E1h), with the latch set and the
   gate set or shut. */
static void helper_keeps(const char *name, int gate_after, int which)
{
    char what[128];
    jb_context *jb = must_create(NULL);
    jb_texture tex = {.page_x = 64, .depth = JB_TEX_15BIT};
    uint32_t before;

    check("jb_set_draw_area", jb_set_draw_area(jb, 0, 0, 255, 255), JB_OK);
    gp1w(jb, 0x09000001u);
    word(jb, 0xe1000000u | 1u << 9 | 1u << 10 | 1u << 11 | 1u << 12);
    if (!gate_after)
        gp1w(jb, 0x09000000u);
    before = mode_bits(jb);
    switch (which) {
    case 0: {
        jb_rect r;
        memset(&r, 0, sizeof r);
        r.w = r.h = 4;
        r.color = 0x808080;
        r.texture = &tex;
        check(name, jb_prim_rect(jb, &r), JB_OK);
        /* The rectangle's page (64: page 1) and depth, its flips clear. */
        before = (before & ~(0xfu | 3u << 7)) | 1u | 2u << 7;
        break;
    }
    case 1: {
        jb_rect r;
        memset(&r, 0, sizeof r);
        r.w = r.h = 4;
        r.blend = JB_BLEND_ADD;
        check(name, jb_prim_rect(jb, &r), JB_OK);
        before = (before & ~(3u << 5)) | 1u << 5;
        break;
    }
    case 2: {
        jb_poly p;
        memset(&p, 0, sizeof p);
        p.vertices = 3;
        p.blend = JB_BLEND_SUB;
        p.v[1].x = 8;
        p.v[2].y = 8;
        check(name, jb_prim_poly(jb, &p), JB_OK);
        before = (before & ~(3u << 5)) | 2u << 5;
        break;
    }
    case 3: {
        jb_line l;
        memset(&l, 0, sizeof l);
        l.blend = JB_BLEND_QUARTER;
        l.v[1].x = 8;
        check(name, jb_prim_line(jb, &l), JB_OK);
        before = (before & ~(3u << 5)) | 3u << 5;
        break;
    }
    default: {
        jb_vertex v[3];
        jb_polyline pl;
        memset(v, 0, sizeof v);
        v[1].x = 8;
        v[2].y = 8;
        pl.gouraud = 0;
        pl.blend = JB_BLEND_HALF;
        pl.count = 3;
        pl.v = v;
        check(name, jb_prim_polyline(jb, &pl), JB_OK);
        before &= ~(3u << 5);
        break;
    }
    }
    snprintf(what, sizeof what, "%s, gate %s: GPUSTAT bits 0-10 and 15", name,
             gate_after ? "set" : "shut");
    check(what, mode_bits(jb), before);
    jb_destroy(jb);
}

/* A textured jb_prim_poly: its page with bit 11 clear stores 0, whether
   the gate is set or shut, as the console's page does; bits 9 and 10
   stay. */
static void poly_page(int gate_after)
{
    char what[96];
    jb_context *jb = must_create(NULL);
    jb_texture tex = {.page_x = 128, .page_y = 256, .depth = JB_TEX_8BIT};
    jb_poly p;

    check("jb_set_draw_area", jb_set_draw_area(jb, 0, 0, 255, 255), JB_OK);
    gp1w(jb, 0x09000001u);
    word(jb, 0xe1000000u | 1u << 9 | 1u << 10 | 1u << 11);
    if (!gate_after)
        gp1w(jb, 0x09000000u);
    memset(&p, 0, sizeof p);
    p.vertices = 3;
    p.texture = &tex;
    p.raw = 1;
    p.v[1].x = 8;
    p.v[2].y = 8;
    check("jb_prim_poly", jb_prim_poly(jb, &p), JB_OK);
    snprintf(what, sizeof what, "textured jb_prim_poly, gate %s: bit 15 written 0",
             gate_after ? "set" : "shut");
    check(what, stat_bit(jb, 15), 0);
    snprintf(what, sizeof what, "textured jb_prim_poly, gate %s: the page, bits 9 and 10",
             gate_after ? "set" : "shut");
    check(what, jb_gpustat(jb) & 0x7ffu, 2u | 1u << 4 | 1u << 7 | 1u << 9 | 1u << 10);
    jb_destroy(jb);
}

/* The flips jb_set_dither keeps, seen in what a textured rectangle draws
   (it takes them from the draw mode): sent through jb_gp0 so the flips
   come from the stored draw mode, not from a helper's own E1h. */
static void dither_keeps_flips(void)
{
    jb_config c;
    jb_context *jb;
    uint16_t tex[4] = {0x8001, 0x8002, 0x8003, 0x8004}, got[4];
    uint32_t sprite[] = {0x65808080u, xy(0, 8), 0, xy(4, 1)};
    int i;

    memset(&c, 0, sizeof c);
    jb = must_create(&c);
    check("jb_set_draw_area", jb_set_draw_area(jb, 0, 0, 255, 255), JB_OK);
    check("jb_vram_load", jb_vram_load(jb, 64, 0, 4, 1, tex), JB_OK);
    /* page 64, 15-bit, x-flipped */
    word(jb, 0xe1000000u | 1u | 2u << 7 | 1u << 12);
    check("jb_set_dither", jb_set_dither(jb, 1), JB_OK);
    gp0(jb, sprite, 4);
    check("jb_vram_read", jb_vram_read(jb, 0, 8, 4, 1, got), JB_OK);
    /* x-flipped from u 0: texels 1, 0, 255, 254 (the header) */
    check("x-flip kept: texel 1", got[0], 0x8002);
    check("x-flip kept: texel 0", got[1], 0x8001);
    for (i = 2; i < 4; ++i)
        check("x-flip kept: texels 255, 254 (zero)", got[i], 0);
    jb_destroy(jb);
}

/* The helper's update against raw GP0(E1h) words: the latch set, the
   gate set or shut, then a blended line by jb_prim_line, or by jb_gp0
   after an E1h with bit 11 set (the case that failed) or clear (the word the
   helper builds). */
static void helper_vs_raw(int gate_after)
{
    static const uint32_t line[] = {0x420000ffu, 0, 8};
    char what[128];
    int k;

    for (k = 0; k < 3; ++k) {
        jb_context *jb = must_create(NULL);
        uint32_t want;

        check("jb_set_draw_area", jb_set_draw_area(jb, 0, 0, 255, 255), JB_OK);
        gp1w(jb, 0x09000001u);
        word(jb, 0xe1000800u);
        if (!gate_after)
            gp1w(jb, 0x09000000u);
        if (k == 0) {
            jb_line l;
            memset(&l, 0, sizeof l);
            l.blend = JB_BLEND_ADD;
            l.v[0].color = 0xff;
            l.v[1].x = 8;
            check("jb_prim_line", jb_prim_line(jb, &l), JB_OK);
            want = 1; /* kept, gate set or shut */
        } else {
            word(jb, k == 1 ? 0xe1000820u : 0xe1000020u);
            gp0(jb, line, 3);
            want = k == 1 ? (uint32_t)gate_after : 0; /* gate && bit 11 */
        }
        snprintf(what, sizeof what, "gate %s, %s: GPUSTAT bit 15", gate_after ? "set" : "shut",
                 k == 0   ? "jb_prim_line blended"
                 : k == 1 ? "raw E1h with bit 11, then the line"
                          : "raw E1h without bit 11, then the line");
        check(what, stat_bit(jb, 15), want);
        jb_destroy(jb);
    }
}

int main(void)
{
    static const char *names[] = {"jb_prim_rect textured", "jb_prim_rect blended",
                                  "jb_prim_poly blended", "jb_prim_line blended",
                                  "jb_prim_polyline blended"};
    int gate, k;

    for (gate = 0; gate < 2; ++gate) {
        dither_keeps(gate);
        for (k = 0; k < 5; ++k)
            helper_keeps(names[k], gate, k);
        poly_page(gate);
        helper_vs_raw(gate);
    }
    dither_keeps_flips();
    return finish("api_draw_mode");
}

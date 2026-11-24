/*
 * api_prim: the structured drawing calls (jb_prim_*, jb_set_draw_area,
 * jb_set_draw_offset, jb_set_texture_window, jb_set_mask, jb_set_dither)
 * against the GP0 words the header says each one sends.
 *
 * Two contexts with the same configuration start from the same VRAM (noise
 * loaded through GP0(A0h), so every texture page, CLUT and mask bit has
 * something in it). A seeded generator then makes a long run of calls:
 * every primitive kind (flat and Gouraud, untextured and textured at 4, 8
 * and 15 bits, triangles and quads, lines, polylines, rectangles of every
 * size including the fixed 1, 8 and 16), every blend mode, raw and
 * modulated, flipped rectangles, the drawing-state calls and raw GP0(E1h)
 * words between them, positions near the middle, at the ends of the
 * -1024..1023 range (clipped, off screen, too large to draw) and
 * degenerate ones (coincident or collinear vertices). Context A gets the
 * calls; context B gets the words, built here from the header's
 * description of them (and psx-spx's layout of the commands), with a
 * polyline as the real polyline command and its terminator, and rectangles
 * of the fixed sizes now as the fixed-size command and now as the
 * variable one with a size word.
 *
 * The range rule: now and then one argument is put out of its range (or
 * a field a command does not use is given a value outside every range,
 * which must be ignored); an out-of-range call must return
 * JB_ERROR_INVALID_ARGUMENT, send nothing, and say why in jb_error_string,
 * and B then gets nothing.
 *
 * Checked: GPUSTAT after every call, the whole 1x VRAM (jb_vram_read)
 * every 40 calls and at the end, jb_frame at the end, and the GP1(10h)
 * latches of the drawing area, offset and texture window. At 1x and 2x,
 * with 1 and 4 threads.
 *
 * Also: the calls are not part of jb_gp0's word stream (a call between the
 * words of a polygon, a polyline and a CPU-to-VRAM transfer that jb_gp0 is
 * receiving), and the ends of every range.
 */
#include "api_test.h"

enum { CALLS = 700, COMPARE_EVERY = 40 };

/* B's draw mode, GP0(E1h) bits 0-13, as the words sent to B leave it. */
static uint32_t mirror_e1;

static void ref_e1(jb_context *b, uint32_t bits)
{
    mirror_e1 = bits & 0x3fffu;
    gp0_1(b, 0xe1000000u | mirror_e1);
}

/* A blending untextured primitive's GP0(E1h). */
static void ref_blend_mode(jb_context *b, int blend)
{
    if (blend != JB_BLEND_NONE)
        ref_e1(b, (mirror_e1 & ~0x60u) | (uint32_t)(blend - 1) << 5);
}

static uint32_t depth_bits(int depth)
{
    return depth == 4 ? 0u : depth == 8 ? 1u : 2u;
}

static uint32_t clut_attr(const jb_texture *t)
{
    return (uint32_t)(t->clut_x / 16) | (uint32_t)t->clut_y << 6;
}

static void ref_poly(jb_context *b, const jb_poly *p)
{
    uint32_t w[12];
    int n = 0, i, tex = p->texture != NULL, g = p->gouraud != 0;
    uint32_t op = 0x20u | (g ? 0x10u : 0) | (p->vertices == 4 ? 0x08u : 0) | (tex ? 0x04u : 0) |
                  (p->blend ? 0x02u : 0) | (tex && p->raw ? 0x01u : 0);

    if (!tex)
        ref_blend_mode(b, p->blend);
    w[n++] = op << 24 | p->v[0].color;
    for (i = 0; i < p->vertices; ++i) {
        uint32_t attr = 0;

        if (g && i > 0)
            w[n++] = p->v[i].color;
        w[n++] = xy(p->v[i].x, p->v[i].y);
        if (!tex)
            continue;
        if (i == 0) {
            attr = clut_attr(p->texture);
        } else if (i == 1) {
            uint32_t mode = p->blend ? (uint32_t)(p->blend - 1) : (mirror_e1 >> 5) & 3;

            attr = (uint32_t)(p->texture->page_x / 64) |
                   (uint32_t)(p->texture->page_y / 256) << 4 | mode << 5 |
                   depth_bits(p->texture->depth) << 7;
            mirror_e1 = (mirror_e1 & ~0x1ffu) | attr;
        }
        w[n++] = (uint32_t)p->v[i].u | (uint32_t)p->v[i].v << 8 | attr << 16;
    }
    gp0(b, w, (size_t)n);
}

static void ref_polyline(jb_context *b, int gouraud, int blend, const jb_vertex *v, size_t count)
{
    uint32_t w[64];
    size_t n = 0, i;
    uint32_t op = (count == 2 ? 0x40u : 0x48u) | (gouraud ? 0x10u : 0) | (blend ? 0x02u : 0);

    ref_blend_mode(b, blend);
    w[n++] = op << 24 | v[0].color;
    w[n++] = xy(v[0].x, v[0].y);
    for (i = 1; i < count; ++i) {
        if (gouraud)
            w[n++] = v[i].color;
        w[n++] = xy(v[i].x, v[i].y);
    }
    if (count > 2)
        w[n++] = 0x55555555u;
    gp0(b, w, n);
}

static void ref_rect(jb_context *b, const jb_rect *r, int use_fixed)
{
    uint32_t w[4], size = 0;
    int n = 0, tex = r->texture != NULL;

    if (tex) {
        uint32_t mode = r->blend ? (uint32_t)(r->blend - 1) : (mirror_e1 >> 5) & 3;

        ref_e1(b, (mirror_e1 & ~(0x1ffu | 0x3000u)) | (uint32_t)(r->texture->page_x / 64) |
                      (uint32_t)(r->texture->page_y / 256) << 4 | mode << 5 |
                      depth_bits(r->texture->depth) << 7 | (r->flip_x ? 1u << 12 : 0) |
                      (r->flip_y ? 1u << 13 : 0));
    } else {
        ref_blend_mode(b, r->blend);
    }
    if (use_fixed && r->w == r->h)
        size = r->w == 1 ? 1u : r->w == 8 ? 2u : r->w == 16 ? 3u : 0u;
    w[n++] = (0x60u | size << 3 | (tex ? 0x04u : 0) | (r->blend ? 0x02u : 0) |
              (tex && r->raw ? 0x01u : 0))
                 << 24 |
             r->color;
    w[n++] = xy(r->x, r->y);
    if (tex)
        w[n++] = (uint32_t)r->u | (uint32_t)r->v << 8 | clut_attr(r->texture) << 16;
    if (size == 0)
        w[n++] = xy(r->w, r->h);
    gp0(b, w, (size_t)n);
}

/* ------------------------------------------------------------------------ */
/* The generator */

static int gen_coord(gen *g, int centre)
{
    int k = rnd_below(g, 40);

    if (k == 0)
        return rnd_below(g, 2) ? -1024 : 1023;
    if (k == 1)
        return rnd_below(g, 2048) - 1024;
    return centre - 60 + rnd_below(g, 120);
}

static void gen_vertex(gen *g, jb_vertex *v)
{
    v->x = gen_coord(g, 160);
    v->y = gen_coord(g, 120);
    v->color = rnd(g) & 0xffffffu;
    if (rnd_below(g, 4) == 0)
        v->color = 0x808080u; /* textures as they are */
    v->u = rnd_below(g, 256);
    v->v = rnd_below(g, 256);
}

static void gen_texture(gen *g, jb_texture *t)
{
    static const int depths[3] = {4, 8, 15};

    t->page_x = 64 * rnd_below(g, 16);
    t->page_y = 256 * rnd_below(g, 2);
    t->depth = depths[rnd_below(g, 3)];
    t->clut_x = 16 * rnd_below(g, 64);
    t->clut_y = rnd_below(g, 512);
}

static int gen_blend(gen *g)
{
    return rnd_below(g, 2) ? JB_BLEND_NONE : 1 + rnd_below(g, 4);
}

/* Fields a command does not carry: values outside every range, which
   must be ignored. */
static void spoil_unused(gen *g, jb_vertex *v, int n, int gouraud, int textured)
{
    int i;

    if (rnd_below(g, 4) != 0)
        return;
    for (i = 0; i < n; ++i) {
        if (!gouraud && i > 0)
            v[i].color = 0xdeadbeefu;
        if (!textured) {
            v[i].u = -7777;
            v[i].v = 9999;
        }
    }
}

static struct {
    int calls, refused, compared;
} tally;

/* An API call that must have been refused. */
static void expect_refused(jb_context *a, jb_result r, const char *what)
{
    char msg[96];

    snprintf(msg, sizeof msg, "%s out of range: refused", what);
    check(msg, r, JB_ERROR_INVALID_ARGUMENT);
    snprintf(msg, sizeof msg, "%s out of range: an error string", what);
    check(msg, strlen(jb_error_string(a)) > 10, 1);
    ++tally.refused;
}

static void step_poly(gen *g, jb_context *a, jb_context *b)
{
    jb_texture tex;
    jb_poly p;
    int i, bad = rnd_below(g, 12) == 0 ? 1 + rnd_below(g, 9) : 0;
    jb_result r;

    memset(&p, 0, sizeof p);
    p.vertices = rnd_below(g, 2) ? 4 : 3;
    p.gouraud = rnd_below(g, 2) ? 1 + rnd_below(g, 5) : 0; /* any value but 0 is Gouraud */
    p.blend = gen_blend(g);
    p.raw = rnd_below(g, 3) == 0;
    gen_texture(g, &tex);
    p.texture = rnd_below(g, 3) ? &tex : NULL;
    for (i = 0; i < 4; ++i)
        gen_vertex(g, &p.v[i]);
    switch (rnd_below(g, 10)) {
    case 0: /* every vertex the same */
        for (i = 1; i < 4; ++i)
            p.v[i].x = p.v[0].x, p.v[i].y = p.v[0].y;
        break;
    case 1: /* collinear */
        for (i = 1; i < 4; ++i)
            p.v[i].x = p.v[0].x + 7 * i, p.v[i].y = p.v[0].y + 3 * i;
        break;
    default:
        break;
    }
    for (i = 0; i < 4; ++i) {
        if (p.v[i].x > 1023)
            p.v[i].x = 1023;
        if (p.v[i].y > 1023)
            p.v[i].y = 1023;
    }
    spoil_unused(g, p.v, p.vertices, p.gouraud, p.texture != NULL);
    switch (bad) {
    case 0: break;
    case 1: p.v[rnd_below(g, p.vertices)].x = rnd_below(g, 2) ? 1024 : -1025; break;
    case 2: p.v[rnd_below(g, p.vertices)].y = rnd_below(g, 2) ? 1024 : -1025; break;
    case 3: p.raw = 0, p.v[0].color = 0x01000000u; break; /* a colour that is used */
    case 4: p.blend = rnd_below(g, 2) ? 5 : -1; break;
    case 5: p.vertices = rnd_below(g, 2) ? 2 : 5; break;
    case 6:
        if (p.texture == NULL)
            bad = 0;
        else
            p.v[rnd_below(g, p.vertices)].u = 256;
        break;
    case 7:
        if (p.texture == NULL)
            bad = 0;
        else
            tex.page_x = 32;
        break;
    case 8:
        if (p.texture == NULL)
            bad = 0;
        else
            tex.depth = 16;
        break;
    default:
        if (p.texture == NULL)
            bad = 0;
        else {
            if (tex.depth == JB_TEX_15BIT) /* a CLUT position that is used */
                tex.depth = JB_TEX_8BIT;
            tex.clut_y = 512;
        }
        break;
    }
    r = jb_prim_poly(a, &p);
    if (bad) {
        expect_refused(a, r, "jb_prim_poly");
        return;
    }
    check("jb_prim_poly", r, JB_OK);
    ref_poly(b, &p);
}

static void step_line(gen *g, jb_context *a, jb_context *b)
{
    jb_vertex v[12];
    size_t count = rnd_below(g, 3) ? 2 : (size_t)(2 + rnd_below(g, 10)), i;
    int gouraud = rnd_below(g, 2), blend = gen_blend(g), bad = rnd_below(g, 12) == 0;
    jb_result r;

    for (i = 0; i < count; ++i)
        gen_vertex(g, &v[i]);
    spoil_unused(g, v, (int)count, gouraud, 0);
    if (bad) {
        switch (rnd_below(g, 3)) {
        case 0: v[count - 1].x = 1024; break;
        case 1: v[0].color = 0xff000000u; break;
        default: blend = 7; break;
        }
    }
    if (count == 2 && rnd_below(g, 2)) {
        jb_line l;

        l.gouraud = gouraud;
        l.blend = blend;
        l.v[0] = v[0];
        l.v[1] = v[1];
        r = jb_prim_line(a, &l);
    } else {
        jb_polyline l;

        l.gouraud = gouraud;
        l.blend = blend;
        l.count = count;
        l.v = v;
        r = jb_prim_polyline(a, &l);
    }
    if (bad) {
        expect_refused(a, r, "jb_prim_line/polyline");
        return;
    }
    check("jb_prim_line/polyline", r, JB_OK);
    ref_polyline(b, gouraud, blend, v, count);
}

static void step_rect(gen *g, jb_context *a, jb_context *b)
{
    static const int fixed[4] = {1, 8, 16, 0};
    jb_texture tex;
    jb_rect r;
    int k = rnd_below(g, 10), bad = rnd_below(g, 12) == 0 ? 1 + rnd_below(g, 5) : 0;
    jb_result res;

    memset(&r, 0, sizeof r);
    r.x = gen_coord(g, 140);
    r.y = gen_coord(g, 100);
    if (k < 4) {
        r.w = r.h = fixed[k];
    } else if (k == 4) {
        r.w = rnd_below(g, 1024);
        r.h = rnd_below(g, 512);
    } else {
        r.w = rnd_below(g, 70);
        r.h = rnd_below(g, 70);
    }
    r.color = rnd(g) & 0xffffffu;
    r.blend = gen_blend(g);
    r.raw = rnd_below(g, 3) == 0;
    gen_texture(g, &tex);
    r.texture = rnd_below(g, 3) ? &tex : NULL;
    r.u = rnd_below(g, 256);
    r.v = rnd_below(g, 256);
    r.flip_x = rnd_below(g, 3) == 0;
    r.flip_y = rnd_below(g, 3) == 0 ? 2 : 0;
    if (r.texture == NULL && rnd_below(g, 4) == 0)
        r.u = 1000, r.v = -1000; /* not used, so not checked */
    switch (bad) {
    case 1: r.w = 1024; break;
    case 2: r.h = rnd_below(g, 2) ? 512 : -1; break;
    case 3: r.x = -1025; break;
    case 4: r.raw = 0, r.color = 0x1000000u; break;
    case 5:
        if (r.texture == NULL)
            bad = 0;
        else
            tex.page_y = 128;
        break;
    default: break;
    }
    res = jb_prim_rect(a, &r);
    if (bad) {
        expect_refused(a, res, "jb_prim_rect");
        return;
    }
    check("jb_prim_rect", res, JB_OK);
    ref_rect(b, &r, rnd_below(g, 2));
}

static void step_state(gen *g, jb_context *a, jb_context *b)
{
    int k = rnd_below(g, 6), bad = rnd_below(g, 10) == 0;
    jb_result r = JB_OK;

    switch (k) {
    case 0: { /* the area: mostly around the display, now and then anything */
        int l = rnd_below(g, 60), t = rnd_below(g, 60);
        int rt = 200 + rnd_below(g, 120), bt = 150 + rnd_below(g, 90);

        if (rnd_below(g, 6) == 0) {
            l = rnd_below(g, 1024), t = rnd_below(g, 512);
            rt = rnd_below(g, 1024), bt = rnd_below(g, 512);
        }
        if (bad) {
            r = jb_set_draw_area(a, l, t, rnd_below(g, 2) ? 1024 : -1, bt);
            break;
        }
        r = jb_set_draw_area(a, l, t, rt, bt);
        gp0_1(b, 0xe3000000u | (uint32_t)l | (uint32_t)t << 10);
        gp0_1(b, 0xe4000000u | (uint32_t)rt | (uint32_t)bt << 10);
        break;
    }
    case 1: {
        int x = rnd_below(g, 80) - 40, y = rnd_below(g, 80) - 40;

        if (rnd_below(g, 10) == 0)
            x = rnd_below(g, 2) ? -1024 : 1023;
        if (bad) {
            r = jb_set_draw_offset(a, x, 1024);
            break;
        }
        r = jb_set_draw_offset(a, x, y);
        gp0_1(b, 0xe5000000u | ((uint32_t)x & 0x7ffu) | ((uint32_t)y & 0x7ffu) << 11);
        break;
    }
    case 2: {
        int mx = rnd_below(g, 32), my = rnd_below(g, 32), ox = rnd_below(g, 32),
            oy = rnd_below(g, 32);

        if (rnd_below(g, 2))
            mx = my = 0;
        if (bad) {
            r = jb_set_texture_window(a, mx, my, 32, oy);
            break;
        }
        r = jb_set_texture_window(a, mx, my, ox, oy);
        gp0_1(b, 0xe2000000u | (uint32_t)mx | (uint32_t)my << 5 | (uint32_t)ox << 10 |
                     (uint32_t)oy << 15);
        break;
    }
    case 3: {
        int set = rnd_below(g, 4) == 0 ? 3 : 0, chk = rnd_below(g, 4) == 0 ? -1 : 0;

        bad = 0;
        r = jb_set_mask(a, set, chk);
        gp0_1(b, 0xe6000000u | (set ? 1u : 0) | (chk ? 2u : 0));
        break;
    }
    case 4: {
        int on = rnd_below(g, 2);

        bad = 0;
        r = jb_set_dither(a, on ? 5 : 0);
        ref_e1(b, (mirror_e1 & ~0x200u) | (on ? 0x200u : 0));
        break;
    }
    default: { /* a raw draw mode on both, which the calls must keep */
        uint32_t w = 0xe1000000u | (rnd(g) & 0x3fffu);

        bad = 0;
        gp0_1(a, w);
        ref_e1(b, w);
        break;
    }
    }
    if (bad) {
        expect_refused(a, r, "drawing state");
        return;
    }
    check("drawing state call", r, JB_OK);
}

static void compare_vram(jb_context *a, jb_context *b, const char *what)
{
    static uint16_t va[VRAM_PIXELS], vb[VRAM_PIXELS];
    size_t i, diff = 0, first = 0;

    check("jb_vram_read A", jb_vram_read(a, 0, 0, VRAM_W, VRAM_H, va), JB_OK);
    check("jb_vram_read B", jb_vram_read(b, 0, 0, VRAM_W, VRAM_H, vb), JB_OK);
    for (i = 0; i < (size_t)VRAM_PIXELS; ++i) {
        if (va[i] != vb[i] && diff++ == 0)
            first = i;
    }
    if (!check(what, (long long)diff, 0))
        printf("  first difference at (%d, %d): %04x against %04x\n", (int)(first % VRAM_W),
               (int)(first / VRAM_W), va[first], vb[first]);
    ++tally.compared;
}

static void compare_frames(jb_context *a, jb_context *b, const char *what)
{
    int wa, ha, wb, hb;
    const uint8_t *fa = jb_frame(a, &wa, &ha), *fb = jb_frame(b, &wb, &hb);

    check("frame A", fa != NULL, 1);
    check("frame B", fb != NULL, 1);
    if (fa == NULL || fb == NULL)
        return;
    check("frame sizes", wa * 10000 + ha, wb * 10000 + hb);
    check(what, memcmp(fa, fb, (size_t)wa * (size_t)ha * 4) == 0, 1);
}

/* Both contexts as a run starts: noise in all of VRAM, the drawing area
   on the display, the display on. */
static void start(jb_context *c, gen *noise)
{
    static uint32_t w[3 + VRAM_PIXELS / 2];
    size_t i;

    w[0] = 0xa0000000u;
    w[1] = 0;
    w[2] = xy(0, 0);
    for (i = 0; i < (size_t)VRAM_PIXELS / 2; ++i)
        w[3 + i] = rnd(noise);
    gp0(c, w, 3 + (size_t)VRAM_PIXELS / 2);
    gp0_1(c, 0xe3000000u);
    gp0_1(c, 0xe4000000u | 319u | 239u << 10);
    gp1(c, 0x03000000u);
}

static void run(int scale, int threads, uint64_t seed)
{
    jb_config cfg;
    jb_context *a, *b;
    gen g = {0, 0, 0}, noise = {0, 0, 0};
    char what[96];
    int i, k;

    memset(&cfg, 0, sizeof cfg);
    cfg.scale = scale;
    cfg.threads = threads;
    a = must_create(&cfg);
    b = must_create(&cfg);
    noise.s = seed * 31 + 7;
    start(a, &noise);
    noise.s = seed * 31 + 7;
    start(b, &noise);
    mirror_e1 = 0;
    g.s = seed;
    for (i = 0; i < CALLS; ++i) {
        k = rnd_below(&g, 100);
        if (k < 40)
            step_poly(&g, a, b);
        else if (k < 58)
            step_line(&g, a, b);
        else if (k < 80)
            step_rect(&g, a, b);
        else
            step_state(&g, a, b);
        ++tally.calls;
        snprintf(what, sizeof what, "%dx %dt call %d: GPUSTAT", scale, threads, i);
        check(what, jb_gpustat(a), jb_gpustat(b));
        if ((i + 1) % COMPARE_EVERY == 0) {
            snprintf(what, sizeof what, "%dx %dt after call %d: VRAM", scale, threads, i);
            compare_vram(a, b, what);
        }
    }
    snprintf(what, sizeof what, "%dx %dt: VRAM at the end", scale, threads);
    compare_vram(a, b, what);
    snprintf(what, sizeof what, "%dx %dt: frame at the end", scale, threads);
    compare_frames(a, b, what);
    for (k = 2; k <= 5; ++k) {
        gp1(a, 0x10000000u | (uint32_t)k);
        gp1(b, 0x10000000u | (uint32_t)k);
        snprintf(what, sizeof what, "%dx %dt: GP1(10h) latch %d", scale, threads, k);
        check(what, jb_gpuread(a), jb_gpuread(b));
    }
    jb_destroy(a);
    jb_destroy(b);
}

/* ------------------------------------------------------------------------ */

/* Calls made while jb_gp0 holds part of a command: they are drawn at once,
   and the command completes with the next words. */
static void not_in_the_stream(int scale)
{
    jb_config cfg;
    jb_context *a, *b;
    gen noise = {99, 0, 0};
    jb_rect r;
    jb_vertex pl[3];
    jb_polyline line;
    uint32_t poly[4], pline[5], load[3 + 64];
    uint16_t pix[128];
    int i;

    memset(&cfg, 0, sizeof cfg);
    cfg.scale = scale;
    a = must_create(&cfg);
    b = must_create(&cfg);
    start(a, &noise);
    noise.s = 99;
    start(b, &noise);
    mirror_e1 = 0;
    memset(&r, 0, sizeof r);
    r.x = 20, r.y = 20, r.w = 40, r.h = 30, r.color = JB_RGB(200, 40, 40);
    memset(pl, 0, sizeof pl);
    pl[0].x = 5, pl[0].y = 100, pl[0].color = JB_RGB(255, 255, 0);
    pl[1].x = 150, pl[1].y = 130;
    pl[2].x = 60, pl[2].y = 200;
    line.gouraud = 0, line.blend = JB_BLEND_NONE, line.count = 3, line.v = pl;

    /* A flat triangle cut after two words, a rectangle, the rest. */
    poly[0] = 0x2000ff00u, poly[1] = xy(30, 10), poly[2] = xy(90, 25), poly[3] = xy(50, 70);
    gp0(a, poly, 2);
    check("rect inside a polygon's words", jb_prim_rect(a, &r), JB_OK);
    gp0(a, poly + 2, 2);
    ref_rect(b, &r, 0);
    gp0(b, poly, 4);

    /* A polyline cut after two vertices, a polyline call, the rest. */
    pline[0] = 0x480000ffu, pline[1] = xy(10, 10), pline[2] = xy(200, 50);
    pline[3] = xy(100, 150), pline[4] = 0x55555555u;
    gp0(a, pline, 3);
    check("polyline inside a polyline's words", jb_prim_polyline(a, &line), JB_OK);
    gp0(a, pline + 3, 2);
    ref_polyline(b, 0, JB_BLEND_NONE, pl, 3);
    gp0(b, pline, 5);

    /* A 16 x 8 transfer cut after its fourth row, a rectangle, the rest:
       for B, the two halves as transfers of their own (the stream writes
       each row as it completes). */
    for (i = 0; i < 128; ++i)
        pix[i] = (uint16_t)(i * 517u);
    load[0] = 0xa0000000u, load[1] = xy(100, 60), load[2] = xy(16, 8);
    for (i = 0; i < 64; ++i)
        load[3 + i] = (uint32_t)pix[2 * i] | (uint32_t)pix[2 * i + 1] << 16;
    gp0(a, load, 3 + 32);
    r.x = 90, r.y = 50, r.w = 30, r.h = 30, r.color = JB_RGB(10, 220, 90);
    check("rect inside a transfer's words", jb_prim_rect(a, &r), JB_OK);
    gp0(a, load + 3 + 32, 32);
    load_rect(b, 100, 60, 16, 4, pix);
    ref_rect(b, &r, 0);
    load_rect(b, 100, 64, 16, 4, pix + 64);

    compare_vram(a, b, "calls inside jb_gp0's commands: VRAM");
    compare_frames(a, b, "calls inside jb_gp0's commands: frame");
    check("calls inside jb_gp0's commands: GPUSTAT", jb_gpustat(a), jb_gpustat(b));
    jb_destroy(a);
    jb_destroy(b);
}

/* The ends of each range are accepted and drawn as their words. */
static void range_ends(void)
{
    jb_context *a = must_create(NULL), *b = must_create(NULL);
    gen noise = {5, 0, 0};
    jb_texture t = {.page_x = 960, .page_y = 256, .depth = JB_TEX_15BIT, .clut_x = 1008,
                    .clut_y = 511};
    jb_poly p;
    jb_rect r;
    int i;

    start(a, &noise);
    noise.s = 5;
    start(b, &noise);
    mirror_e1 = 0;
    check("area 0..1023 x 0..511", jb_set_draw_area(a, 0, 0, 1023, 511), JB_OK);
    gp0_1(b, 0xe3000000u);
    gp0_1(b, 0xe4000000u | 1023u | 511u << 10);
    check("offset -1024, 1023", jb_set_draw_offset(a, -1024, 1023), JB_OK);
    gp0_1(b, 0xe5000000u | 0x400u | 0x3ffu << 11);
    check("GPUREAD offset -1024, 1023", (gp1(a, 0x10000005u), jb_gpuread(a)),
          0x400u | 0x3ffu << 11);
    check("offset 1023, -1024", jb_set_draw_offset(a, 1023, -1024), JB_OK);
    gp0_1(b, 0xe5000000u | 0x3ffu | 0x400u << 11);
    check("offset 0, 0", jb_set_draw_offset(a, 0, 0), JB_OK);
    gp0_1(b, 0xe5000000u);
    check("window 31s", jb_set_texture_window(a, 31, 31, 31, 31), JB_OK);
    gp0_1(b, 0xe2000000u | 0xfffffu);
    check("GPUREAD window 31s", (gp1(a, 0x10000002u), jb_gpuread(a)), 0xfffffu);
    check("window off", jb_set_texture_window(a, 0, 0, 0, 0), JB_OK);
    gp0_1(b, 0xe2000000u);

    memset(&p, 0, sizeof p);
    p.vertices = 4;
    p.gouraud = 1;
    p.texture = &t;
    p.blend = JB_BLEND_QUARTER;
    for (i = 0; i < 4; ++i) {
        p.v[i].x = (i & 1) ? 1023 : 1000;
        p.v[i].y = (i & 2) ? 511 : 480;
        p.v[i].color = 0xffffffu;
        p.v[i].u = (i & 1) ? 255 : 0;
        p.v[i].v = (i & 2) ? 255 : 0;
    }
    check("poly at the ends of the ranges", jb_prim_poly(a, &p), JB_OK);
    ref_poly(b, &p);
    for (i = 0; i < 4; ++i)
        p.v[i].x = -1024 + (i & 1) * 40;
    check("poly at x -1024", jb_prim_poly(a, &p), JB_OK);
    ref_poly(b, &p);

    memset(&r, 0, sizeof r);
    r.x = -1024, r.y = -1024, r.w = 1023, r.h = 511, r.color = 0xffffffu;
    check("largest rect", jb_prim_rect(a, &r), JB_OK);
    ref_rect(b, &r, 0);
    r.x = 1023, r.y = 1023;
    check("rect at 1023", jb_prim_rect(a, &r), JB_OK);
    ref_rect(b, &r, 0);
    r.x = 600, r.y = 300, r.w = 0, r.h = 0;
    check("empty rect", jb_prim_rect(a, &r), JB_OK);
    ref_rect(b, &r, 0);
    /* Fields a command does not use are not checked and change nothing: a
       15-bit texture's CLUT position, a raw primitive's colours. */
    {
        jb_texture odd = {.page_x = 448, .depth = JB_TEX_15BIT, .clut_x = 12345, .clut_y = -7};
        jb_texture plain = {.page_x = 448, .depth = JB_TEX_15BIT};
        jb_poly q;
        jb_rect s;

        memset(&q, 0, sizeof q);
        q.vertices = 3, q.gouraud = 1, q.raw = 1;
        q.v[0].x = 100, q.v[0].y = 100;
        q.v[1].x = 220, q.v[1].y = 110, q.v[1].u = 200;
        q.v[2].x = 130, q.v[2].y = 230, q.v[2].v = 200;
        q.texture = &plain;
        check("raw 15-bit poly", jb_prim_poly(b, &q), JB_OK);
        q.texture = &odd;
        q.v[0].color = 0xdeadbeefu, q.v[1].color = 0xff000000u, q.v[2].color = 0x01234567u;
        check("raw 15-bit poly, unused fields out of range", jb_prim_poly(a, &q), JB_OK);
        memset(&s, 0, sizeof s);
        s.x = 300, s.y = 100, s.w = 40, s.h = 30, s.raw = 1;
        s.texture = &plain;
        check("raw 15-bit rect", jb_prim_rect(b, &s), JB_OK);
        s.texture = &odd, s.color = 0xff000000u;
        check("raw 15-bit rect, unused fields out of range", jb_prim_rect(a, &s), JB_OK);
        /* Used, the same values are refused. */
        q.raw = 0;
        check("modulated poly with those colours", jb_prim_poly(a, &q),
              JB_ERROR_INVALID_ARGUMENT);
        odd.depth = JB_TEX_8BIT, q.raw = 1;
        check("8-bit poly with that CLUT position", jb_prim_poly(a, &q),
              JB_ERROR_INVALID_ARGUMENT);
    }
    compare_vram(a, b, "range ends: VRAM");
    check("range ends: GPUSTAT", jb_gpustat(a), jb_gpustat(b));
    jb_destroy(a);
    jb_destroy(b);
}

int main(void)
{
    run(1, 1, 0x1234);
    run(1, 4, 0x1234);
    run(2, 1, 0xbeef);
    run(2, 4, 0xbeef);
    run(1, 1, 0x777);
    not_in_the_stream(1);
    not_in_the_stream(2);
    range_ends();
    printf("api_prim: %d calls, %d refused, %d VRAM comparisons\n", tally.calls, tally.refused,
           tally.compared);
    check("enough refused calls", tally.refused > 100, 1);
    return finish("api_prim");
}

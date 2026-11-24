/*
 * api_draw2d: sprites and text (include/jitterbug.h, Sprites and text),
 * and repeat textures through the 2D draws.
 *
 *   sprite = rect     1:1 sprites (every depth, repeat, sub-rectangles,
 *                     tints, raw, blends, both flips) equal the jb_prim_rect
 *                     the header gives for them, sent at once in a frame, at
 *                     1x, 2x and 3x and on 4 threads: VRAM and jb_frame
 *   scaled = quad     scaled sprites equal the jb_prim_poly quad the header
 *                     describes, within the console's limits
 *   scaled texels     a scaled raw sprite fills exactly its rectangle, with
 *                     texels of its source only (none from past its edge),
 *                     the first and last texels at its corners, flipped too
 *   flips, clipping   1:1 sprites against their texels by hand: mirrored,
 *                     cut at every edge of the frame; scaled ones thousands
 *                     of pixels across, which are split; none writes outside
 *                     the back buffer; positions near the int limits
 *   repeat            a repeat texture tiles through jb_draw_rect,
 *                     jb_draw_poly and jb_draw_sprite (a source longer than
 *                     the texture), the window put back after the frame
 *   text              pixels against the font's bitmap: the colour's top 5
 *                     bits, '\n', spaces, '?' for other bytes, cut at the
 *                     frame's edges, the same 1x VRAM at 3x; blended text
 *                     adds and carries bit 15; jb_text_size; layers
 *   errors            every documented error, its detail, nothing recorded,
 *                     each latched for jb_end_frame; a full frame drops
 *                     characters and names max_primitives; no room for the
 *                     font
 *
 * It reads the font's glyphs from src/api/font.c (jbi_font_row), so it is
 * built from the library's sources (tests/CMakeLists.txt).
 */
#include "api_test.h"

#include "api/texture.h"

enum { FW = 320, FH = 240 };

static jb_context *ctx(int scale, int threads, int w, int h, int max_prims)
{
    jb_config c;

    memset(&c, 0, sizeof c);
    c.scale = scale;
    c.threads = threads;
    c.width = w;
    c.height = h;
    c.max_primitives = max_prims;
    return must_create(&c);
}

static uint16_t vram_a[VRAM_PIXELS], vram_b[VRAM_PIXELS];

static void read_all(jb_context *jb, uint16_t *dst)
{
    jb_vram_read(jb, 0, 0, VRAM_W, VRAM_H, dst);
}

/* The back buffer of the frame just ended: the first frame draws into
   the first buffer, (0, 0), the second into (0, 240). */
static uint16_t at(const uint16_t *v, int x, int y)
{
    return v[y * VRAM_W + x];
}

static void same(const char *what, jb_context *a, jb_context *b)
{
    const uint8_t *f;
    uint8_t *copy;
    int w, h, w2, h2;
    size_t n;
    char m[160];

    read_all(a, vram_a);
    read_all(b, vram_b);
    snprintf(m, sizeof m, "%s: VRAM", what);
    check(m, memcmp(vram_a, vram_b, sizeof vram_a) == 0, 1);
    f = jb_frame(a, &w, &h);
    n = (size_t)w * (size_t)h * 4;
    copy = must_alloc(n);
    memcpy(copy, f, n);
    f = jb_frame(b, &w2, &h2);
    snprintf(m, sizeof m, "%s: frame", what);
    check(m, w == w2 && h == h2 && memcmp(copy, f, n) == 0, 1);
    free(copy);
}

/* ------------------------------------------------------------------------ */
/* Textures, made the same way in each context (placement is
   deterministic, so they get the same jb_texture). */

enum { NTEX = 5 };

static jb_texture texs[NTEX];
static uint8_t idx[256 * 256];
static uint16_t pix[256 * 256];

static uint32_t hash(uint32_t a, uint32_t b)
{
    uint32_t h = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u) * 0x85ebca6bu;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return h;
}

static jb_texture make(jb_context *jb, int w, int h, int depth, int repeat, uint32_t seed)
{
    jb_texture_desc d;
    jb_texture t;
    uint16_t pal[256];
    int i;

    for (i = 0; i < w * h; ++i) {
        uint32_t v = hash(seed, (uint32_t)i);
        idx[i] = (uint8_t)(depth == 4 ? v & 15 : v & 255);
        /* 15-bit: never 0x0000, a few semi-transparent */
        pix[i] = (uint16_t)((v & 0x7fff) | 1 | ((v >> 20) % 5 == 0 ? 0x8000 : 0));
    }
    for (i = 0; i < 256; ++i) {
        uint32_t v = hash(seed ^ 0x55u, (uint32_t)i);
        pal[i] = (uint16_t)((v & 0x7fff) | ((v >> 20) % 4 == 0 ? 0x8000 : 0));
    }
    pal[0] = 0; /* index 0 transparent, as art has it */
    memset(&d, 0, sizeof d);
    d.width = w;
    d.height = h;
    d.depth = depth;
    d.texels = depth == 15 ? (const void *)pix : (const void *)idx;
    d.palette = depth == 15 ? NULL : pal;
    d.repeat = repeat;
    memset(&t, 0, sizeof t);
    check("create texture", jb_texture_create(jb, &d, &t), JB_OK);
    return t;
}

static void make_all(jb_context *jb)
{
    texs[0] = make(jb, 30, 20, 4, 0, 1);
    texs[1] = make(jb, 64, 32, 8, 1, 2);
    texs[2] = make(jb, 17, 9, 15, 0, 3);
    texs[3] = make(jb, 16, 16, 4, 1, 4);
    texs[4] = make(jb, 100, 70, 8, 0, 5);
}

/* The window the header gives for a texture: tiling a repeat one. */
static void window_for(jb_context *jb, const jb_texture *t)
{
    if (t->repeat)
        jb_set_texture_window(jb, t->width < 256 ? (256 - t->width) / 8 : 0,
                              t->height < 256 ? (256 - t->height) / 8 : 0,
                              t->width < 256 ? t->u / 8 : 0, t->height < 256 ? t->v / 8 : 0);
    else
        jb_set_texture_window(jb, 0, 0, 0, 0);
}

/* A random sprite's source, within the limits. */
static void gen_source(gen *g, const jb_texture *t, jb_sprite *s)
{
    int lw, lh;

    s->texture = t;
    s->u = rnd_below(g, t->width);
    s->v = rnd_below(g, t->height);
    lw = t->repeat ? 256 - s->u : t->width - s->u;
    lh = t->repeat ? 256 - s->v : t->height - s->v;
    if (lw > 90)
        lw = 90;
    if (lh > 90)
        lh = 90;
    s->src_w = 1 + rnd_below(g, lw);
    s->src_h = 1 + rnd_below(g, lh);
    s->tint = rnd_below(g, 3) ? rnd(g) & 0xffffffu : 0;
    s->blend = rnd_below(g, 5);
    s->raw = rnd_below(g, 4) == 0;
    s->flip_x = rnd_below(g, 2);
    s->flip_y = rnd_below(g, 2);
}

/* ------------------------------------------------------------------------ */

static void sprite_equals_rect(int scale, int threads)
{
    jb_context *a = ctx(scale, threads, 0, 0, 0), *b = ctx(scale, threads, 0, 0, 0);
    gen g;
    int frame, i;
    char what[96];

    make_all(a);
    make_all(b);
    g.s = 77;
    g.cx = g.cy = 0;
    for (frame = 0; frame < 2; ++frame) {
        check("begin", jb_begin_frame(a, JB_RGB(20, 40, 60)), JB_OK);
        check("begin", jb_begin_frame(b, JB_RGB(20, 40, 60)), JB_OK);
        for (i = 0; i < 300; ++i) {
            jb_sprite s;
            jb_rect r;
            const jb_texture *t = &texs[rnd_below(&g, NTEX)];
            int k = rnd_below(&g, 3);

            memset(&s, 0, sizeof s);
            gen_source(&g, t, &s);
            s.x = rnd_below(&g, FW + 80) - 40;
            s.y = rnd_below(&g, FH + 80) - 40;
            /* 1:1 three ways: the size given as both w, h and src_w, src_h;
               as src_w, src_h alone; and all 0 (the rest of the texture). */
            if (k == 0) {
                s.w = s.src_w, s.h = s.src_h;
            } else if (k == 1) {
                s.w = s.h = 0;
            } else {
                s.w = s.h = s.src_w = s.src_h = 0;
            }
            check("jb_draw_sprite", jb_draw_sprite(a, &s), JB_OK);
            /* What the header says it equals. */
            memset(&r, 0, sizeof r);
            r.texture = t;
            r.x = s.x;
            r.y = s.y;
            r.w = k == 0 ? s.w : k == 1 ? s.src_w : t->width - s.u;
            r.h = k == 0 ? s.h : k == 1 ? s.src_h : t->height - s.v;
            r.color = s.tint ? s.tint : JB_NEUTRAL;
            r.blend = s.blend;
            r.raw = s.raw;
            r.flip_x = s.flip_x;
            r.flip_y = s.flip_y;
            r.u = (t->u + s.u + (s.flip_x ? r.w - 2 : 0)) & 255;
            r.v = (t->v + s.v + (s.flip_y ? r.h - 1 : 0)) & 255;
            window_for(b, t);
            check("jb_prim_rect", jb_prim_rect(b, &r), JB_OK);
        }
        jb_set_texture_window(b, 0, 0, 0, 0);
        check("end", jb_end_frame(a), JB_OK);
        check("end", jb_end_frame(b), JB_OK);
    }
    snprintf(what, sizeof what, "sprite = jb_prim_rect at %dx, %d threads", scale, threads);
    same(what, a, b);
    check("window put back", jb_gpustat(a) == jb_gpustat(b), 1);
    jb_destroy(a);
    jb_destroy(b);
}

static void scaled_equals_quad(int scale)
{
    jb_context *a = ctx(scale, 1, 0, 0, 0), *b = ctx(scale, 1, 0, 0, 0);
    gen g;
    int i;
    char what[96];

    make_all(a);
    make_all(b);
    g.s = 99;
    g.cx = g.cy = 0;
    check("begin", jb_begin_frame(a, JB_RGB(5, 5, 5)), JB_OK);
    check("begin", jb_begin_frame(b, JB_RGB(5, 5, 5)), JB_OK);
    for (i = 0; i < 200; ++i) {
        jb_sprite s;
        jb_poly p;
        const jb_texture *t = &texs[rnd_below(&g, NTEX)];
        int u0, v0, u1, v1, x1, y1, k;

        memset(&s, 0, sizeof s);
        gen_source(&g, t, &s);
        s.x = rnd_below(&g, FW + 80) - 40;
        s.y = rnd_below(&g, FH + 80) - 40;
        s.w = 1 + rnd_below(&g, 300);
        s.h = 1 + rnd_below(&g, 200);
        if (s.w == s.src_w && s.h == s.src_h)
            ++s.w;
        check("jb_draw_sprite scaled", jb_draw_sprite(a, &s), JB_OK);
        u0 = t->u + s.u, v0 = t->v + s.v;
        if (t->repeat)
            u0 = s.u % t->width, v0 = s.v % t->height;
        u1 = u0 + s.src_w - 1, v1 = v0 + s.src_h - 1;
        if (s.flip_x)
            k = u0, u0 = u1, u1 = k;
        if (s.flip_y)
            k = v0, v0 = v1, v1 = k;
        x1 = s.x + s.w, y1 = s.y + s.h;
        memset(&p, 0, sizeof p);
        p.vertices = 4;
        p.texture = t;
        p.blend = s.blend;
        p.raw = s.raw;
        for (k = 0; k < 4; ++k) {
            p.v[k].x = k & 1 ? x1 : s.x;
            p.v[k].y = k & 2 ? y1 : s.y;
            p.v[k].u = k & 1 ? u1 : u0;
            p.v[k].v = k & 2 ? v1 : v0;
            p.v[k].color = s.tint ? s.tint : JB_NEUTRAL;
        }
        window_for(b, t);
        check("jb_prim_poly", jb_prim_poly(b, &p), JB_OK);
    }
    jb_set_texture_window(b, 0, 0, 0, 0);
    check("end", jb_end_frame(a), JB_OK);
    check("end", jb_end_frame(b), JB_OK);
    snprintf(what, sizeof what, "scaled sprite = jb_prim_poly at %dx", scale);
    same(what, a, b);
    jb_destroy(a);
    jb_destroy(b);
}

/* The texel (x, y) of a 15-bit texture made by make(), from VRAM. */
static uint16_t texel15(const uint16_t *v, const jb_texture *t, int x, int y)
{
    return at(v, t->page_x + t->u + x, t->page_y + t->v + y);
}

static void scaled_texels(void)
{
    static const int sizes[][4] = {{17, 9, 40, 30}, {17, 9, 5, 4},  {6, 5, 7, 300},
                                   {17, 9, 900, 20}, {3, 2, 1000, 600}, {1, 1, 20, 20}};
    jb_context *jb = ctx(1, 1, 0, 0, 0);
    jb_texture t = make(jb, 17, 9, 15, 0, 3);
    int i, f;

    for (i = 0; i < 6; ++i)
        for (f = 0; f < 4; ++f) {
            jb_sprite s;
            uint16_t clear;
            int x, y, bad_area = 0, bad_texel = 0, X0 = 30, Y0 = 20, x1, y1;
            char m[96];

            memset(&s, 0, sizeof s);
            s.texture = &t;
            s.u = 17 - sizes[i][0];
            s.v = 9 - sizes[i][1];
            s.src_w = sizes[i][0];
            s.src_h = sizes[i][1];
            s.w = sizes[i][2];
            s.h = sizes[i][3];
            s.x = X0;
            s.y = Y0;
            s.raw = 1;
            s.flip_x = f & 1;
            s.flip_y = f >> 1;
            /* Two frames: the second draws into the buffer at (0, 240). */
            jb_begin_frame(jb, JB_RGB(0, 0, 0));
            jb_end_frame(jb);
            jb_begin_frame(jb, JB_RGB(0, 0, 0));
            check("scaled", jb_draw_sprite(jb, &s), JB_OK);
            jb_end_frame(jb);
            read_all(jb, vram_a);
            clear = 0;
            x1 = X0 + s.w < FW ? X0 + s.w : FW;
            y1 = Y0 + s.h < FH ? Y0 + s.h : FH;
            for (y = 0; y < FH; ++y)
                for (x = 0; x < FW; ++x) {
                    uint16_t p = at(vram_a, x, FH + y);
                    int inside = x >= X0 && x < x1 && y >= Y0 && y < y1, k, j, found = 0;
                    if (inside != (p != clear))
                        ++bad_area;
                    if (!inside)
                        continue;
                    for (j = 0; j < s.src_h && !found; ++j)
                        for (k = 0; k < s.src_w && !found; ++k)
                            if (texel15(vram_a, &t, s.u + k, s.v + j) == p)
                                found = 1;
                    if (!found)
                        ++bad_texel;
                }
            snprintf(m, sizeof m, "scaled %dx%d to %dx%d flip %d: fills its rectangle", s.src_w,
                     s.src_h, s.w, s.h, f);
            check(m, bad_area, 0);
            snprintf(m, sizeof m, "scaled %dx%d to %dx%d flip %d: source texels only", s.src_w,
                     s.src_h, s.w, s.h, f);
            check(m, bad_texel, 0);
            snprintf(m, sizeof m, "scaled flip %d: first texel at the top-left", f);
            check(m, at(vram_a, X0, FH + Y0),
                  texel15(vram_a, &t, s.u + (s.flip_x ? s.src_w - 1 : 0),
                          s.v + (s.flip_y ? s.src_h - 1 : 0)));
        }
    jb_destroy(jb);
}

/* 1:1, against the texels by hand: flips, and the frame's edges. */
static void flips_and_clipping(void)
{
    static const int places[][2] = {{40, 30}, {-5, -3}, {FW - 7, FH - 4}, {-16, 100},
                                    {200, -8}, {FW - 1, 50}, {-1000, -1000}, {FW, FH}};
    jb_context *jb = ctx(1, 1, 0, 0, 0);
    jb_texture t = make(jb, 17, 9, 15, 0, 3);
    int i, f;

    for (i = 0; i < 8; ++i)
        for (f = 0; f < 4; ++f) {
            jb_sprite s;
            int x, y, bad = 0;
            char m[96];

            memset(&s, 0, sizeof s);
            s.texture = &t;
            s.x = places[i][0];
            s.y = places[i][1];
            s.u = 2;
            s.v = 1;
            s.raw = 1;
            s.flip_x = f & 1;
            s.flip_y = f >> 1;
            jb_begin_frame(jb, JB_RGB(0, 0, 0));
            jb_end_frame(jb);
            jb_begin_frame(jb, JB_RGB(0, 0, 0));
            check("sprite", jb_draw_sprite(jb, &s), JB_OK);
            jb_end_frame(jb);
            read_all(jb, vram_a);
            /* 15 x 8 texels from (2, 1). */
            for (y = 0; y < FH; ++y)
                for (x = 0; x < FW; ++x) {
                    int sx = x - s.x, sy = y - s.y;
                    uint16_t want = 0;
                    if (sx >= 0 && sx < 15 && sy >= 0 && sy < 8)
                        want = texel15(vram_a, &t, 2 + (s.flip_x ? 14 - sx : sx),
                                       1 + (s.flip_y ? 7 - sy : sy));
                    if (at(vram_a, x, FH + y) != want)
                        ++bad;
                }
            snprintf(m, sizeof m, "1:1 at (%d, %d) flip %d", s.x, s.y, f);
            check(m, bad, 0);
        }
    jb_destroy(jb);
}

/* Huge scaled sprites are split, and nothing lands outside the back
   buffer; positions near the int limits are fine. */
static void huge(void)
{
    jb_context *jb = ctx(1, 1, 0, 0, 0);
    jb_texture t = make(jb, 17, 9, 15, 0, 3);
    static const int cases[][4] = {{-2000, -1500, 2400, 1900}, {-5000, 10, 6000, 30},
                                   {100, -3000, 40, 4000},     {-1024, -512, 2048, 1024}};
    int i, x, y;

    for (i = 0; i < 4; ++i) {
        jb_sprite s;
        int bad_out = 0, bad_in = 0, x1, y1, x0, y0;
        char m[96];

        jb_begin_frame(jb, JB_RGB(0, 0, 0));
        jb_end_frame(jb); /* the next frame draws into (0, 240) */
        read_all(jb, vram_b);
        memset(&s, 0, sizeof s);
        s.texture = &t;
        s.x = cases[i][0];
        s.y = cases[i][1];
        s.w = cases[i][2];
        s.h = cases[i][3];
        s.raw = 1;
        jb_begin_frame(jb, JB_RGB(0, 0, 0));
        check("huge", jb_draw_sprite(jb, &s), JB_OK);
        check("huge end", jb_end_frame(jb), JB_OK);
        read_all(jb, vram_a);
        x0 = s.x > 0 ? s.x : 0, y0 = s.y > 0 ? s.y : 0;
        x1 = s.x + s.w < FW ? s.x + s.w : FW, y1 = s.y + s.h < FH ? s.y + s.h : FH;
        for (y = 0; y < VRAM_H; ++y)
            for (x = 0; x < VRAM_W; ++x) {
                int in_buffer = x < FW && y >= FH && y < 2 * FH;
                if (!in_buffer) {
                    if (at(vram_a, x, y) != at(vram_b, x, y))
                        ++bad_out;
                } else {
                    int inside = x >= x0 && x < x1 && y - FH >= y0 && y - FH < y1;
                    if (inside != (at(vram_a, x, y) != 0))
                        ++bad_in;
                }
            }
        snprintf(m, sizeof m, "huge %d: nothing outside the back buffer", i);
        check(m, bad_out, 0);
        snprintf(m, sizeof m, "huge %d: covers its part of the frame", i);
        check(m, bad_in, 0);
    }
    /* Near the int limits: each case alone in a frame, and the
       pixels checked. Off the frame, nothing; from (10, 10) with sizes up
       to INT_MAX, the sprite covers (20, 20) with its first texel (the
       scale is so large that the first texel spans the frame). */
    {
        static const struct {
            int x, y, w, h, drawn;
        } extreme[] = {{0x7fffff00, 0, 0, 0, 0},
                     {-0x7fffffff, 0, 0x7fffffff, 0, 0},
                     {10, 10, 0x7fffffff, 0x7fffffff, 1},
                     {10, 10, 0x7fffffff, 0x7fffffff - 20, 1},
                     {10, 10, 0x7fffffff - 20, 0x7fffffff, 1},
                     {10, 10, 0x7fffffff - 20, 0x7fffffff - 20, 1},
                     {-0x7fffffff, -0x7fffffff, 0x7fffffff, 0x7fffffff, 0},
                     {-0x7ffffff0, -0x7ffffff0, 0x7fffffff, 0x7fffffff, 1}};
        for (i = 0; i < (int)(sizeof extreme / sizeof extreme[0]); ++i) {
            jb_sprite s;
            int lit = 0;
            char m[96];
            memset(&s, 0, sizeof s);
            s.texture = &t;
            s.raw = 1;
            s.x = extreme[i].x, s.y = extreme[i].y, s.w = extreme[i].w, s.h = extreme[i].h;
            jb_begin_frame(jb, 0);
            jb_end_frame(jb); /* the next frame draws into (0, 240) */
            jb_begin_frame(jb, 0);
            snprintf(m, sizeof m, "extreme %d: draw", i);
            check(m, jb_draw_sprite(jb, &s), JB_OK);
            snprintf(m, sizeof m, "extreme %d: end", i);
            check(m, jb_end_frame(jb), JB_OK);
            read_all(jb, vram_a);
            for (y = 0; y < FH; ++y)
                for (x = 0; x < FW; ++x)
                    lit += at(vram_a, x, FH + y) != 0;
            snprintf(m, sizeof m, "extreme %d (%d, %d) %d x %d: drawn or not", i, s.x, s.y, s.w,
                     s.h);
            check(m, lit > 0, extreme[i].drawn);
            if (extreme[i].drawn && extreme[i].x == 10) {
                snprintf(m, sizeof m, "extreme %d: (20, 20) shows the first texel", i);
                check(m, at(vram_a, 20, FH + 20), texel15(vram_a, &t, 0, 0));
                snprintf(m, sizeof m, "extreme %d: (9, 9) not drawn", i);
                check(m, at(vram_a, 9, FH + 9), 0);
            }
        }
    }
    jb_destroy(jb);
}

/* ------------------------------------------------------------------------ */
/* Repeat textures through the 2D draws */

static void repeat(void)
{
    jb_context *jb = ctx(1, 1, 0, 0, 0), *b = ctx(1, 1, 0, 0, 0);
    jb_texture t = make(jb, 16, 8, 15, 1, 9), tb = make(b, 16, 8, 15, 1, 9);
    jb_rect r;
    jb_poly p;
    jb_sprite s;
    int x, y, bad, k;

    check("other context: the same id, another owner", t.id == tb.id && t.owner != tb.owner,
          1);
    tb.owner = t.owner;
    check("same texture", memcmp(&t, &tb, sizeof t) == 0, 1);
    tb = t; /* drawn in b by its fields, as a texture made by hand can be */
    jb_texture plain = make(jb, 17, 9, 15, 0, 3);

    jb_set_texture_window(jb, 3, 3, 3, 3); /* the program's window, kept */
    jb_begin_frame(jb, 0);
    jb_end_frame(jb);
    jb_begin_frame(jb, 0);
    /* Repeat and plain records alternate under the program's window: each
       repeat one tiles, each plain one has no window. */
    /* A rectangle: u, v anywhere in the page tile the texture. */
    memset(&r, 0, sizeof r);
    r.texture = &t;
    r.raw = 1;
    r.x = 10, r.y = 10, r.w = 70, r.h = 30, r.u = 5, r.v = 3;
    check("rect", jb_draw_rect(jb, &r), JB_OK);
    r.texture = &plain;
    r.x = 10, r.y = 60, r.w = 17, r.h = 9, r.u = plain.u, r.v = plain.v;
    check("plain rect", jb_draw_rect(jb, &r), JB_OK);
    /* A sprite whose source runs past the texture. */
    memset(&s, 0, sizeof s);
    s.texture = &t;
    s.raw = 1;
    s.x = 100, s.y = 10, s.u = 3, s.v = 2, s.src_w = 60, s.src_h = 25;
    check("sprite past the edge", jb_draw_sprite(jb, &s), JB_OK);
    s.texture = &plain, s.u = s.v = 0, s.src_w = s.src_h = 0, s.x = 100, s.y = 60;
    check("plain sprite", jb_draw_sprite(jb, &s), JB_OK);
    check("end", jb_end_frame(jb), JB_OK);
    read_all(jb, vram_a);
    for (y = 0, bad = 0; y < 30; ++y)
        for (x = 0; x < 70; ++x)
            if (at(vram_a, 10 + x, FH + 10 + y) != texel15(vram_a, &t, (5 + x) % 16, (3 + y) % 8))
                ++bad;
    check("repeat: jb_draw_rect tiles", bad, 0);
    for (y = 0, bad = 0; y < 25; ++y)
        for (x = 0; x < 60; ++x)
            if (at(vram_a, 100 + x, FH + 10 + y) != texel15(vram_a, &t, (3 + x) % 16, (2 + y) % 8))
                ++bad;
    check("repeat: jb_draw_sprite tiles", bad, 0);
    for (y = 0, bad = 0; y < 9; ++y)
        for (x = 0; x < 17; ++x)
            bad += (at(vram_a, 10 + x, FH + 60 + y) != texel15(vram_a, &plain, x, y)) +
                   (at(vram_a, 100 + x, FH + 60 + y) != texel15(vram_a, &plain, x, y));
    check("repeat: plain records between have no window", bad, 0);
    {
        /* The program's window is as it was. */
        uint32_t w;
        gp1(jb, 0x10000002u);
        w = jb_gpuread(jb);
        check("repeat: window put back", w, 3 | 3 << 5 | 3 << 10 | 3 << 15);
    }
    /* A polygon, against jb_prim_poly with the window by hand. */
    jb_set_texture_window(jb, 0, 0, 0, 0);
    jb_begin_frame(jb, 0);
    check("begin b", jb_begin_frame(b, 0), JB_OK);
    memset(&p, 0, sizeof p);
    p.vertices = 4;
    p.texture = &t;
    p.raw = 1;
    for (k = 0; k < 4; ++k) {
        p.v[k].x = 20 + (k & 1) * 90;
        p.v[k].y = 40 + (k >> 1) * 50 + (k & 1) * 7;
        p.v[k].u = (k & 1) * 90;
        p.v[k].v = (k >> 1) * 50;
    }
    check("poly", jb_draw_poly(jb, &p), JB_OK);
    p.texture = &tb;
    window_for(b, &tb);
    check("prim poly", jb_prim_poly(b, &p), JB_OK);
    jb_set_texture_window(b, 0, 0, 0, 0);
    jb_end_frame(jb);
    jb_end_frame(b);
    read_all(jb, vram_a);
    read_all(b, vram_b);
    for (y = 0, bad = 0; y < FH; ++y)
        for (x = 0; x < FW; ++x)
            if (at(vram_a, x, y) != at(vram_b, x, y))
                ++bad;
    check("repeat: jb_draw_poly = jb_prim_poly with the window", bad, 0);
    for (y = 42, bad = 0; y < 88; ++y) /* inside the sloping top edge */
        if (at(vram_a, 30, y) == 0)
            ++bad;
    check("repeat: the polygon is drawn (u past 16 is not transparent)", bad, 0);
    jb_destroy(jb);
    jb_destroy(b);
}

/* ------------------------------------------------------------------------ */
/* Text */

static int glyph_bit(int ch, int x, int y)
{
    if (ch < 32 || ch > 126)
        ch = '?';
    return (int)(jbi_font_row(ch, y) >> (7 - x) & 1);
}

/* Draws text in the second frame of a fresh context and checks every
   pixel of the frame against the font: text pixels are want, others the
   clear colour. */
static void text_case(const char *what, int scale, int x0, int y0, uint32_t color, int blend,
                      const char *text)
{
    jb_context *jb = ctx(scale, 1, 0, 0, 0);
    const uint32_t clear = JB_RGB(80, 40, 160);
    uint16_t bg = JB_RGB_TO_15(clear), fg = JB_RGB_TO_15(color);
    int x, y, bad = 0;
    char m[128];

    if (blend == JB_BLEND_ADD) {
        int k;
        fg = 0x8000;
        for (k = 0; k < 15; k += 5) {
            int c = ((bg >> k) & 31) + ((JB_RGB_TO_15(color) >> k) & 31);
            fg = (uint16_t)(fg | (c > 31 ? 31 : c) << k);
        }
    }
    jb_begin_frame(jb, clear);
    jb_end_frame(jb);
    jb_begin_frame(jb, clear);
    if (blend < 0)
        check(what, jb_draw_text(jb, x0, y0, color, text), JB_OK);
    else
        check(what, jb_draw_text_blended(jb, x0, y0, color, blend, text), JB_OK);
    check(what, jb_end_frame(jb), JB_OK);
    read_all(jb, vram_a);
    for (y = 0; y < FH; ++y)
        for (x = 0; x < FW; ++x) {
            /* Which character's cell (x, y) is in, by walking the text. */
            int64_t px = x0, py = y0;
            int on = 0;
            const unsigned char *p;
            for (p = (const unsigned char *)text; *p; ++p) {
                if (*p == '\n') {
                    px = x0;
                    py += 8;
                    continue;
                }
                if (x >= px && x < px + 8 && y >= py && y < py + 8 && *p != ' ')
                    on = glyph_bit(*p, (int)(x - px), (int)(y - py));
                px += 8;
            }
            if (at(vram_a, x, FH + y) != (on ? fg : bg))
                ++bad;
        }
    snprintf(m, sizeof m, "text %s at %dx: pixels", what, scale);
    check(m, bad, 0);
    jb_destroy(jb);
}

static void text(void)
{
    int w, h;

    text_case("plain", 1, 10, 20, JB_RGB(200, 100, 40), -1, "Hi there!\nline 2 ~{}");
    text_case("plain", 3, 10, 20, JB_RGB(200, 100, 40), -1, "Hi there!\nline 2 ~{}");
    text_case("every glyph", 1, 0, 0, JB_RGB(255, 255, 255), -1,
              " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`\n"
              "abcdefghijklmnopqrstuvwxyz{|}~");
    text_case("other bytes as ?", 1, 30, 30, JB_RGB(10, 250, 90), -1,
              "a\x01" "b\t\x7f\xc3\xa9z\r");
    text_case("cut at the edges", 1, -3, -5, JB_RGB(255, 0, 0), -1,
              "edge\n\n\n                                       XYZW");
    text_case("past the bottom right", 1, FW - 12, FH - 3, JB_RGB(0, 255, 0), -1, "MW\nMW");
    text_case("black", 1, 50, 50, JB_RGB(0, 0, 0), -1, "dark");
    text_case("dim", 1, 50, 50, JB_RGB(7, 7, 7), -1, "five bits");
    text_case("blended", 1, 12, 40, JB_RGB(100, 100, 100), JB_BLEND_ADD, "glow\nadd");
    text_case("blend none", 1, 12, 40, JB_RGB(100, 100, 100), JB_BLEND_NONE, "opaque");
    text_case("far away", 1, -100000, 2000000000, JB_RGB(1, 2, 3), -1, "nothing here");

    check("size", jb_text_size("Hello", &w, &h), JB_OK);
    check("size w", w, 40);
    check("size h", h, 8);
    jb_text_size("ab\nlonger line\n", &w, &h);
    check("size lines w", w, 88);
    check("size lines h", h, 24);
    jb_text_size("", &w, &h);
    check("size empty", w * 1000 + h, 0);
    jb_text_size("\n", &w, &h);
    check("size newline", w * 1000 + h, 16);
    check("size NULL sizes", jb_text_size("x", NULL, NULL), JB_OK);
    w = h = 5;
    check("size text NULL", jb_text_size(NULL, &w, &h), JB_ERROR_INVALID_ARGUMENT);
    check("size text NULL gives 0", w * 1000 + h, 0);
    jb_text_size("a\xc3\xa9", &w, &h);
    check("size counts bytes", w, 24);

    /* Layers, in two regions with the calls in different orders:
       on the left, overlay text before a background rectangle (the text
       shows over it); on the right, an overlay rectangle before background
       text (the rectangle hides it). */
    {
        jb_context *jb = ctx(1, 1, 0, 0, 0);
        const uint16_t white = JB_RGB_TO_15(JB_RGB(255, 255, 255));
        const uint16_t red = JB_RGB_TO_15(JB_RGB(255, 0, 0));
        const uint16_t blue = JB_RGB_TO_15(JB_RGB(0, 0, 255));
        jb_rect r;
        int x, y, bad_left = 0, bad_right = 0, glyph_pixels = 0;
        jb_begin_frame(jb, 0);
        jb_end_frame(jb);
        jb_begin_frame(jb, 0);
        memset(&r, 0, sizeof r);
        r.x = 0, r.y = 0, r.w = 40, r.h = 8, r.color = JB_RGB(255, 0, 0);
        jb_set_layer(jb, JB_LAYER_OVERLAY);
        check("layer: overlay text", jb_draw_text(jb, 0, 0, JB_RGB(255, 255, 255), "A"), JB_OK);
        jb_set_layer(jb, JB_LAYER_BACKGROUND);
        check("layer: background rect", jb_draw_rect(jb, &r), JB_OK);
        r.x = 100, r.color = JB_RGB(0, 0, 255);
        jb_set_layer(jb, JB_LAYER_OVERLAY);
        check("layer: overlay rect", jb_draw_rect(jb, &r), JB_OK);
        jb_set_layer(jb, JB_LAYER_BACKGROUND);
        check("layer: background text", jb_draw_text(jb, 100, 0, JB_RGB(0, 255, 0), "A"), JB_OK);
        jb_end_frame(jb);
        read_all(jb, vram_a);
        for (y = 0; y < 8; ++y)
            for (x = 0; x < 8; ++x) {
                int on = glyph_bit('A', x, y);
                glyph_pixels += on;
                bad_left += at(vram_a, x, FH + y) != (on ? white : red);
                bad_right += at(vram_a, 100 + x, FH + y) != blue;
            }
        check("layer: overlay text shows over the background", bad_left, 0);
        check("layer: the overlay hides background text", bad_right, 0);
        check("layer: the glyph has pixels", glyph_pixels > 0, 1);
        jb_destroy(jb);
    }
}

/* ------------------------------------------------------------------------ */
/* Errors */

static void errors(void)
{
    jb_context *jb = ctx(1, 1, 0, 0, 0);
    jb_texture t = make(jb, 17, 9, 15, 0, 3), r16 = make(jb, 16, 16, 4, 1, 4), bad;
    jb_sprite s, ok;
    const char *detail;

    memset(&ok, 0, sizeof ok);
    ok.texture = &t;

    /* Outside a frame, each latched. */
    check("sprite outside a frame", jb_draw_sprite(jb, &ok), JB_ERROR_INVALID_ARGUMENT);
    check_str("sprite outside detail", jb_error_string(jb),
              "jb_draw_sprite: no frame begun (call jb_begin_frame first)");
    check("latched", jb_check(jb, &detail), JB_ERROR_INVALID_ARGUMENT);
    check_str("latched detail", detail, "jb_draw_sprite: no frame begun (call jb_begin_frame first)");
    check("text outside a frame", jb_draw_text(jb, 0, 0, 0, "x"), JB_ERROR_INVALID_ARGUMENT);
    check("text latched", jb_check(jb, NULL), JB_ERROR_INVALID_ARGUMENT);
    check("blended outside a frame", jb_draw_text_blended(jb, 0, 0, 0, 1, "x"),
          JB_ERROR_INVALID_ARGUMENT);
    jb_check(jb, NULL);

    check("jb NULL", jb_draw_sprite(NULL, &ok), JB_ERROR_INVALID_ARGUMENT);
    check("text jb NULL", jb_draw_text(NULL, 0, 0, 0, "x"), JB_ERROR_INVALID_ARGUMENT);
    check("blended jb NULL", jb_draw_text_blended(NULL, 0, 0, 0, 0, "x"),
          JB_ERROR_INVALID_ARGUMENT);

    jb_begin_frame(jb, 0);
    check("sprite NULL", jb_draw_sprite(jb, NULL), JB_ERROR_INVALID_ARGUMENT);
    check_str("sprite NULL detail", jb_error_string(jb), "jb_draw_sprite: sprite is NULL");
    s = ok, s.texture = NULL;
    check("texture NULL", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    check_str("texture NULL detail", jb_error_string(jb), "jb_draw_sprite: sprite->texture is NULL");
    bad = t, bad.page_x = 70;
    s = ok, s.texture = &bad;
    check("texture field", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    bad = t, bad.u = 250, bad.width = 17;
    check("texture leaves its page", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    check("texture leaves its page detail",
          strstr(jb_error_string(jb), "leave its page") != NULL, 1);
    s = ok, s.u = 17;
    check("u 17", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    check_str("u detail", jb_error_string(jb),
              "jb_draw_sprite: u, v (17, 0) is outside the texture's 0..16 x 0..8");
    s = ok, s.v = -1;
    check("v -1", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    s = ok, s.u = 5, s.src_w = 13;
    check("src_w past the texture", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    check_str("src_w detail", jb_error_string(jb),
              "jb_draw_sprite: source 13 x 0 is outside 0..12 x 0..9 from (5, 0)");
    s = ok, s.src_h = -1;
    check("src_h -1", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    s = ok, s.texture = &r16, s.u = 5, s.src_w = 251;
    check("repeat: src_w 251 from 5 is fine", jb_draw_sprite(jb, &s), JB_OK);
    s.src_w = 252;
    check("repeat: src_w 252 from 5", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    s = ok, s.w = -1;
    check("w -1", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    check_str("w detail", jb_error_string(jb), "jb_draw_sprite: size -1 x 0 is negative");
    s = ok, s.h = -2;
    check("h -2", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    s = ok, s.tint = 0x1000000;
    check("tint", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    s.raw = 1;
    check("tint ignored when raw", jb_draw_sprite(jb, &s), JB_OK);
    s = ok, s.blend = 5;
    check("blend 5", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    s = ok, s.blend = -1;
    check("blend -1", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    check("text NULL", jb_draw_text(jb, 0, 0, 0, NULL), JB_ERROR_INVALID_ARGUMENT);
    check_str("text NULL detail", jb_error_string(jb), "jb_draw_text: text is NULL");
    check("text colour", jb_draw_text(jb, 0, 0, 0x1000000, "x"), JB_ERROR_INVALID_ARGUMENT);
    check("text blend", jb_draw_text_blended(jb, 0, 0, 0, 9, "x"), JB_ERROR_INVALID_ARGUMENT);
    check("text blended NULL", jb_draw_text_blended(jb, 0, 0, 0, 1, NULL),
          JB_ERROR_INVALID_ARGUMENT);
    {
        /* A hand-made texture: the page from its u, v. */
        jb_texture hand;
        memset(&hand, 0, sizeof hand);
        hand.page_x = 640;
        hand.depth = 15;
        s = ok, s.texture = &hand, s.u = 255, s.src_w = 1;
        check("hand-made texture", jb_draw_sprite(jb, &s), JB_OK);
        s.src_w = 2;
        check("hand-made texture: past the page", jb_draw_sprite(jb, &s),
              JB_ERROR_INVALID_ARGUMENT);
    }
    /* The first failure in the frame is what jb_end_frame returns. */
    check("end returns the first", jb_end_frame(jb), JB_ERROR_INVALID_ARGUMENT);
    jb_destroy(jb);

    /* Nothing recorded by a failing call: a frame with only failures is
       the clear alone. */
    {
        jb_context *a = ctx(1, 1, 0, 0, 0), *b = ctx(1, 1, 0, 0, 0);
        jb_texture ta = make(a, 17, 9, 15, 0, 3);
        make(b, 17, 9, 15, 0, 3);
        jb_begin_frame(a, JB_RGB(1, 2, 3));
        jb_begin_frame(b, JB_RGB(1, 2, 3));
        s = ok, s.texture = &ta, s.u = 3, s.src_w = 20;
        jb_draw_sprite(a, &s);
        jb_draw_text(a, 0, 0, 0x1000000, "xxx");
        jb_end_frame(a);
        jb_end_frame(b);
        same("failing calls record nothing", a, b);
        jb_destroy(a);
        jb_destroy(b);
    }

    /* A full frame: the characters that fit, then JB_ERROR_OUT_OF_MEMORY
       naming max_primitives, latched for jb_end_frame. */
    {
        jb_context *small = ctx(1, 1, 0, 0, 10);
        jb_begin_frame(small, 0);
        jb_end_frame(small);
        jb_begin_frame(small, 0);
        check("full", jb_draw_text(small, 0, 0, JB_RGB(255, 255, 255), "ABCDEFGHIJKLMNOP"),
              JB_ERROR_OUT_OF_MEMORY);
        check("full detail", strstr(jb_error_string(small), "max_primitives") != NULL, 1);
        check("full latched", jb_end_frame(small), JB_ERROR_OUT_OF_MEMORY);
        read_all(small, vram_a);
        {
            int x, y, any = 0;
            for (y = 0; y < 8; ++y)
                for (x = 72; x < 80; ++x)
                    any |= at(vram_a, x, FH + y) != 0;
            check("full: the tenth character drawn", any, 1);
            any = 0;
            for (y = 0; y < 8; ++y)
                for (x = 80; x < 128; ++x)
                    any |= at(vram_a, x, FH + y) != 0;
            check("full: the eleventh not", any, 0);
        }
        jb_destroy(small);
    }

    /* No room for the font: one buffer covering VRAM. */
    {
        jb_context *full = ctx(1, 1, 1024, 512, 0);
        jb_begin_frame(full, 0);
        check("no room for the font", jb_draw_text(full, 0, 0, 0, "x"), JB_ERROR_OUT_OF_MEMORY);
        check_str("no room detail", jb_error_string(full),
                  "jb_draw_text: no room in VRAM for a 128 x 48 4-bit texture");
        check("no room latched", jb_end_frame(full), JB_ERROR_OUT_OF_MEMORY);
        jb_destroy(full);
    }
}

/* ------------------------------------------------------------------------ */
/* Sprite and text edge cases, each of which failed before its fix */

/* The texture's own fields checked before any sum; a repeat
   texture made by hand must be one the window can wrap, and a valid one
   samples nothing outside its source (a sentinel beside it). */
static void sprite_texture_fields(void)
{
    jb_context *jb = ctx(1, 1, 0, 0, 0);
    uint16_t src[16], sentinel = 0x7c1f;
    jb_texture t;
    jb_sprite s;
    int i, x, bad = 0;

    jb_begin_frame(jb, 0);
    memset(&t, 0, sizeof t);
    t.page_x = 640, t.depth = JB_TEX_15BIT;
    memset(&s, 0, sizeof s);
    s.texture = &t;
    s.raw = 1;
    t.u = 0x7fffffff, t.width = 8;
    check("sprite texture fields: u INT_MAX", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    t.u = (int)(-0x7fffffff - 1), t.width = 0;
    check("sprite texture fields: u INT_MIN", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    t.u = 0, t.v = 0x7fffffff, t.height = 8;
    check("sprite texture fields: v INT_MAX", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    t.v = (int)(-0x7fffffff - 1), t.height = 0;
    check("sprite texture fields: v INT_MIN", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    t.v = 0, t.width = -1;
    check("sprite texture fields: width -1", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    t.width = 0, t.height = -1;
    check("sprite texture fields: height -1", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    t.height = 0, t.width = 257;
    check("sprite texture fields: width 257", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    t.width = 0, t.u = 256;
    check("sprite texture fields: u 256", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    t.u = 250, t.width = 8;
    check("sprite texture fields: u + width past the page", jb_draw_sprite(jb, &s),
          JB_ERROR_INVALID_ARGUMENT);
    t.u = 248;
    check("sprite texture fields: u + width the page's end", jb_draw_sprite(jb, &s), JB_OK);
    /* Repeat, made by hand. */
    t.u = 1, t.width = t.height = 8, t.repeat = 1;
    check("sprite texture fields: repeat at u 1", jb_draw_sprite(jb, &s),
          JB_ERROR_INVALID_ARGUMENT);
    t.u = 0, t.width = 12;
    check("sprite texture fields: repeat 12 wide", jb_draw_sprite(jb, &s),
          JB_ERROR_INVALID_ARGUMENT);
    t.width = 0;
    check("sprite texture fields: repeat with width 0", jb_draw_sprite(jb, &s),
          JB_ERROR_INVALID_ARGUMENT);
    jb_end_frame(jb);
    jb_check(jb, NULL);
    /* A valid one: 8 x 1 texels at VRAM x 648..655 (u 8 of page 640), the
       sentinel at 647 and 656; drawn tiled 1:1 and scaled. */
    for (i = 0; i < 8; ++i)
        src[i] = (uint16_t)(0x0421 * (i + 1));
    jb_vram_load(jb, 648, 0, 8, 1, src);
    jb_vram_load(jb, 647, 0, 1, 1, &sentinel);
    jb_vram_load(jb, 656, 0, 1, 1, &sentinel);
    memset(&t, 0, sizeof t);
    t.page_x = 640, t.depth = JB_TEX_15BIT, t.u = 8, t.width = 8, t.height = 8, t.repeat = 1;
    jb_begin_frame(jb, 0);
    s.texture = &t, s.u = 3, s.v = 0, s.src_w = 40, s.src_h = 1, s.x = 0, s.y = 0;
    check("sprite texture fields: a valid repeat texture, 1:1", jb_draw_sprite(jb, &s), JB_OK);
    s.y = 2, s.w = 100, s.h = 1;
    check("sprite texture fields: and scaled", jb_draw_sprite(jb, &s), JB_OK);
    check("sprite texture fields: end", jb_end_frame(jb), JB_OK);
    read_all(jb, vram_a);
    for (x = 0; x < 100; ++x) {
        bad += at(vram_a, x, FH + 2) == sentinel;
        if (x < 40)
            bad += at(vram_a, x, FH) != src[(3 + x) % 8];
    }
    check("sprite texture fields: the source tiled, the sentinel never", bad, 0);
    jb_destroy(jb);
}

/* A repeat sprite's source runs to the page's last texel: src_w up
   to 256 - u. */
static void repeat_source_limit(void)
{
    jb_context *jb = ctx(1, 1, 0, 0, 0);
    jb_texture t = make(jb, 8, 8, 15, 1, 7);
    jb_sprite s;

    jb_begin_frame(jb, 0);
    memset(&s, 0, sizeof s);
    s.texture = &t, s.raw = 1, s.u = 1, s.src_w = 255, s.src_h = 1;
    check("repeat source limit: src_w 255 from 1", jb_draw_sprite(jb, &s), JB_OK);
    s.w = 300, s.h = 2;
    check("repeat source limit: and scaled", jb_draw_sprite(jb, &s), JB_OK);
    s.src_w = 256;
    check("repeat source limit: 256 from 1", jb_draw_sprite(jb, &s), JB_ERROR_INVALID_ARGUMENT);
    s.u = 0;
    check("repeat source limit: 256 from 0", jb_draw_sprite(jb, &s), JB_OK);
    jb_end_frame(jb);
    jb_destroy(jb);
}

/* A sprite far larger than the frame keeps its scale: a 256 x 1
   source over x from -800000000, w INT_MAX, samples at screen x the texel
   (x + 800000000) x 255 / INT_MAX (the quad's corners carry texels 0 and
   255), give or take one where a cut rounds. */
static void huge_scale_kept(void)
{
    jb_context *jb = ctx(1, 1, 0, 0, 0);
    uint16_t px[256];
    jb_texture t;
    jb_sprite s;
    int i, x, bad = 0;

    for (i = 0; i < 256; ++i)
        px[i] = (uint16_t)(i + 1);
    {
        jb_texture_desc d;
        memset(&d, 0, sizeof d);
        d.width = 256, d.height = 1, d.depth = JB_TEX_15BIT, d.texels = px;
        check("huge scale kept: texture", jb_texture_create(jb, &d, &t), JB_OK);
    }
    jb_begin_frame(jb, 0);
    jb_end_frame(jb);
    jb_begin_frame(jb, 0);
    memset(&s, 0, sizeof s);
    s.texture = &t, s.raw = 1, s.x = -800000000, s.y = 10, s.w = 0x7fffffff, s.h = 1;
    check("huge scale kept: draw", jb_draw_sprite(jb, &s), JB_OK);
    check("huge scale kept: end", jb_end_frame(jb), JB_OK);
    read_all(jb, vram_a);
    for (x = 0; x < FW; x += 7) {
        long want = (long)(((long long)x + 800000000) * 255 / 0x7fffffff) + 1;
        long got = at(vram_a, x, FH + 10);
        if (got < want - 1 || got > want + 1)
            ++bad;
    }
    check("huge scale kept: the texel at screen x 0", at(vram_a, 0, FH + 10) >= 95 &&
                                               at(vram_a, 0, FH + 10) <= 97,
          1);
    check("huge scale kept: the scale kept across the frame", bad, 0);
    jb_destroy(jb);
}

/* A width or height past the largest int is refused, the sizes 0,
   whichever is asked for. */
static void text_size_overflow(void)
{
    /* The limit tested on short texts through jbi_text_size_most
       (jb_text_size is it with INT_MAX / 8), so no large buffer is needed
       and every check always runs. With a limit of 5: 5 characters or 5
       lines fit, 6 do not; refused, both sizes are 0, whichever is asked
       for. */
    int w = 7, h = 7;

    check("text size overflow: 5 wide", jbi_text_size_most("aaaaa", 5, &w, &h), JB_OK);
    check("text size overflow: 5 wide: 40 x 8", w * 1000 + h, 40008);
    w = h = 7;
    check("text size overflow: 6 wide", jbi_text_size_most("aaaaaa", 5, &w, &h),
          JB_ERROR_INVALID_ARGUMENT);
    check("text size overflow: 6 wide gives 0", w == 0 && h == 0, 1);
    h = 7;
    check("text size overflow: 6 wide, height alone", jbi_text_size_most("aaaaaa\n", 5, NULL, &h),
          JB_ERROR_INVALID_ARGUMENT);
    check("text size overflow: height 0", h, 0);
    check("text size overflow: 5 lines", jbi_text_size_most("a\na\na\na\na", 5, &w, &h), JB_OK);
    check("text size overflow: 5 lines: 8 x 40", w * 1000 + h, 8040);
    w = 7;
    check("text size overflow: 6 lines", jbi_text_size_most("a\na\na\na\na\n", 5, &w, NULL),
          JB_ERROR_INVALID_ARGUMENT);
    check("text size overflow: width 0", w, 0);
    check("text size overflow: 5 newlines, nothing else",
          jbi_text_size_most("\n\n\n\n\n", 5, NULL, NULL),
          JB_ERROR_INVALID_ARGUMENT);
    check("text size overflow: 4 newlines", jbi_text_size_most("\n\n\n\n", 5, &w, &h), JB_OK);
    check("text size overflow: 4 newlines: 0 x 40", w * 1000 + h, 40);
    /* jb_text_size's own limit: INT_MAX / 8 lines of 8 pixels fit an int. */
    check("text size overflow: jb_text_size's limit fits an int",
          (long)(0x7fffffff / 8) * 8 <= 0x7fffffff, 1);
    check("text size overflow: jb_text_size", jb_text_size("abc\nde", &w, &h), JB_OK);
    check("text size overflow: jb_text_size: 24 x 16", w * 1000 + h, 24016);
}

/* Text that shows nothing places no font. In a context with no
   room left, "", spaces and newlines, and text wholly off the frame
   succeed; in one with room, they leave it to the next texture. */
static void text_without_glyphs(void)
{
    jb_context *jb = ctx(1, 1, 0, 0, 0);
    jb_texture t;
    jb_texture_desc d;

    jb_vram_reserve(jb, 320, 0, 704, 512);
    jb_vram_reserve(jb, 0, 480, 320, 32);
    jb_begin_frame(jb, 0);
    check("text without glyphs: empty", jb_draw_text(jb, 0, 0, 0, ""), JB_OK);
    check("text without glyphs: spaces and newlines", jb_draw_text(jb, 0, 0, 0, " \n  \n"), JB_OK);
    check("text without glyphs: off the frame", jb_draw_text(jb, -100, 50, 0, "A"), JB_OK);
    check("text without glyphs: below it", jb_draw_text(jb, 10, 240, 0, "ABC"), JB_OK);
    /* In the frame, but only blank texels of the cell: column 7 (x
       -7), columns 6-7 (x -6), rows 6-7 of a capital (y -6), the last row
       of '_' (whose ink is row 6, at y -7), the right column of a cell at
       the right edge (x 320 - 1 shows column 0, blank for 'A'). */
    check("text without glyphs: only column 7", jb_draw_text(jb, -7, 0, 0xffffff, "A"), JB_OK);
    check("text without glyphs: only columns 6-7", jb_draw_text(jb, -6, 0, 0xffffff, "A"), JB_OK);
    check("text without glyphs: only rows 6-7", jb_draw_text(jb, 0, -6, 0xffffff, "AH"), JB_OK);
    check("text without glyphs: '_' clipped above its ink", jb_draw_text(jb, 50, -7, 0xffffff, "_"),
          JB_OK);
    check("text without glyphs: column 0 at the right edge",
          jb_draw_text(jb, FW - 1, 0, 0xffffff, "A"),
          JB_OK);
    check("text without glyphs: '?' for a byte, clipped blank",
          jb_draw_text(jb, -7, 20, 0xffffff, "\x01"),
          JB_OK);
    /* And with one column of ink in the frame, the font is needed. */
    check("text without glyphs: column 5 shows", jb_draw_text(jb, -5, 0, 0xffffff, "A"),
          JB_ERROR_OUT_OF_MEMORY);
    check("text without glyphs: blended, off it", jb_draw_text_blended(jb, 400, 0, 0, 1, "x"),
          JB_OK);
    check("text without glyphs: a glyph that shows needs the font", jb_draw_text(jb, 0, 0, 0, "A"),
          JB_ERROR_OUT_OF_MEMORY);
    jb_end_frame(jb);
    jb_destroy(jb);
    jb = ctx(1, 1, 0, 0, 0);
    jb_begin_frame(jb, 0);
    jb_draw_text(jb, 0, 0, 0, " ");
    jb_draw_text(jb, -50, 0, 0, "Q");
    jb_end_frame(jb);
    memset(&d, 0, sizeof d);
    d.width = 4, d.height = 4, d.depth = JB_TEX_15BIT;
    check("text without glyphs: a texture", jb_texture_create(jb, &d, &t), JB_OK);
    check("text without glyphs: in the first place, not after a font",
          t.page_x + t.u == 320 && t.v == 0, 1);
    jb_destroy(jb);
}

/* The font's two CLUTs need not be side by side: with room for
   its texels and two 16-entry palettes apart (x 0 and 32 of row 511), text
   draws, opaque and blended, each with its own palette. */
static void font_palettes_apart(void)
{
    jb_context *jb = ctx(1, 1, 0, 0, 0);
    const uint16_t bg = JB_RGB_TO_15(JB_RGB(80, 40, 160));
    int x, y, bad = 0, k;

    check("font palettes apart: reserve", jb_vram_reserve(jb, 0, 0, 640, 48), JB_OK);
    check("font palettes apart: reserve", jb_vram_reserve(jb, 672, 0, 352, 48), JB_OK);
    check("font palettes apart: reserve", jb_vram_reserve(jb, 0, 48, 1024, 463), JB_OK);
    check("font palettes apart: reserve", jb_vram_reserve(jb, 16, 511, 16, 1), JB_OK);
    check("font palettes apart: reserve", jb_vram_reserve(jb, 48, 511, 976, 1), JB_OK);
    jb_begin_frame(jb, JB_RGB(80, 40, 160));
    jb_end_frame(jb);
    jb_begin_frame(jb, JB_RGB(80, 40, 160));
    check("font palettes apart: opaque", jb_draw_text(jb, 0, 0, JB_RGB(200, 100, 40), "A"), JB_OK);
    check("font palettes apart: blended", jb_draw_text_blended(jb, 16, 0, JB_RGB(100, 100, 100),
                                                  JB_BLEND_ADD, "A"),
          JB_OK);
    check("font palettes apart: end", jb_end_frame(jb), JB_OK);
    read_all(jb, vram_a);
    for (y = 0; y < 8; ++y)
        for (x = 0; x < 8; ++x) {
            uint16_t add = 0x8000;
            for (k = 0; k < 15; k += 5) {
                int c = ((bg >> k) & 31) + ((JB_RGB_TO_15(JB_RGB(100, 100, 100)) >> k) & 31);
                add = (uint16_t)(add | (c > 31 ? 31 : c) << k);
            }
            bad += at(vram_a, x, FH + y) !=
                   (glyph_bit('A', x, y) ? JB_RGB_TO_15(JB_RGB(200, 100, 40)) : bg);
            bad += at(vram_a, 16 + x, FH + y) != (glyph_bit('A', x, y) ? add : bg);
        }
    check("font palettes apart: both drawn with their palettes", bad, 0);
    check("font palettes apart: the palettes apart", at(vram_a, 1, 511) == 0x4210 &&
                                             at(vram_a, 33, 511) == (0x4210 | 0x8000),
          1);
    jb_destroy(jb);
}

/* When the font's first atlas place leaves room for only one of
   its palettes, another atlas place is tried. Free (inclusive): x 641..672
   at y 0, 640..688 at y 1, 641..688 at y 2..47, 657..688 at y 48; all else
   reserved. The atlas at (641, 0) leaves one palette place, (672, 48); the
   one at (657, 1) leaves (640, 1) and (656, 0), and text draws. */
static void font_atlas_search(void)
{
    static const int free_rows[][3] = {{0, 641, 672}, {1, 640, 688}, {48, 657, 688}};
    jb_context *jb = ctx(1, 1, 0, 0, 0);
    const uint16_t bg = JB_RGB_TO_15(JB_RGB(80, 40, 160));
    int y, x, k, bad = 0;

    for (y = 0; y < 512; ++y) {
        int x0 = -1, x1 = -1;
        for (k = 0; k < 3; ++k)
            if (free_rows[k][0] == y)
                x0 = free_rows[k][1], x1 = free_rows[k][2];
        if (y >= 2 && y <= 47)
            x0 = 641, x1 = 688;
        if (x0 < 0) {
            jb_vram_reserve(jb, 0, y, 1024, 1);
            continue;
        }
        jb_vram_reserve(jb, 0, y, x0, 1);
        jb_vram_reserve(jb, x1 + 1, y, 1023 - x1, 1);
    }
    jb_begin_frame(jb, JB_RGB(80, 40, 160));
    jb_end_frame(jb);
    jb_begin_frame(jb, JB_RGB(80, 40, 160));
    check("font atlas search: opaque text", jb_draw_text(jb, 0, 0, JB_RGB(200, 100, 40), "A"),
          JB_OK);
    check("font atlas search: blended text", jb_draw_text_blended(jb, 16, 0, JB_RGB(100, 100, 100),
                                                      JB_BLEND_ADD, "A"),
          JB_OK);
    check("font atlas search: end", jb_end_frame(jb), JB_OK);
    read_all(jb, vram_a);
    check("font atlas search: the palettes",
          at(vram_a, 641, 1) == 0x4210 && at(vram_a, 657, 0) == 0xc210, 1);
    for (y = 0; y < 8; ++y)
        for (x = 0; x < 8; ++x) {
            uint16_t add = 0x8000;
            for (k = 0; k < 15; k += 5) {
                int c = ((bg >> k) & 31) + ((JB_RGB_TO_15(JB_RGB(100, 100, 100)) >> k) & 31);
                add = (uint16_t)(add | (c > 31 ? 31 : c) << k);
            }
            bad += at(vram_a, x, FH + y) !=
                   (glyph_bit('A', x, y) ? JB_RGB_TO_15(JB_RGB(200, 100, 40)) : bg);
            bad += at(vram_a, 16 + x, FH + y) != (glyph_bit('A', x, y) ? add : bg);
        }
    check("font atlas search: both drawn with their palettes", bad, 0);
    jb_destroy(jb);
}

/* Text that a full frame cannot record places no font: with one
   record a frame, a rectangle takes it, and "A" then fails with nothing
   written to VRAM (no font palette at the bottom left). Then, in a frame
   with room, the font is placed and the text drawn. The first text
   also needs a slot in the table of textures: with 1024 textures it fails,
   and with one freed it draws. */
static void text_and_full_tables(void)
{
    jb_config c;
    jb_context *jb;
    jb_rect r;
    uint16_t px = 0;
    static jb_texture many[1024];
    jb_texture_desc d;
    int i, ok = 1;

    memset(&c, 0, sizeof c);
    c.width = 32, c.height = 32, c.max_primitives = 1;
    jb = must_create(&c);
    memset(&r, 0, sizeof r);
    r.w = r.h = 1, r.color = JB_RGB(255, 255, 255);
    jb_begin_frame(jb, 0);
    check("text and full tables: the one record", jb_draw_rect(jb, &r), JB_OK);
    check("text and full tables: text, the frame full", jb_draw_text(jb, 0, 0, JB_NEUTRAL, "A"),
          JB_ERROR_OUT_OF_MEMORY);
    jb_vram_read(jb, 1, 511, 1, 1, &px);
    check("text and full tables: no font palette written", px, 0);
    jb_end_frame(jb);
    jb_begin_frame(jb, 0);
    check("text and full tables: text, the frame empty", jb_draw_text(jb, 0, 0, JB_NEUTRAL, "A"),
          JB_OK);
    check("text and full tables: end", jb_end_frame(jb), JB_OK);
    jb_vram_read(jb, 1, 511, 1, 1, &px);
    check("text and full tables: now the font palette", px, 0x4210);
    jb_destroy(jb);

    jb = ctx(1, 1, 0, 0, 0);
    memset(&d, 0, sizeof d);
    d.width = d.height = 1, d.depth = JB_TEX_15BIT;
    for (i = 0; i < 1024; ++i)
        ok &= jb_texture_create(jb, &d, &many[i]) == JB_OK;
    check("text and full tables: 1024 textures", ok, 1);
    jb_begin_frame(jb, 0);
    check("text and full tables: text, the table full", jb_draw_text(jb, 0, 0, JB_NEUTRAL, "A"),
          JB_ERROR_OUT_OF_MEMORY);
    check("text and full tables: detail", strstr(jb_error_string(jb), "1024 textures") != NULL, 1);
    jb_end_frame(jb);
    check("text and full tables: free one", jb_texture_destroy(jb, &many[0]), JB_OK);
    jb_begin_frame(jb, 0);
    check("text and full tables: text, a slot free", jb_draw_text(jb, 0, 0, JB_NEUTRAL, "A"),
          JB_OK);
    check("text and full tables: end", jb_end_frame(jb), JB_OK);
    jb_destroy(jb);

    /* Text whose font cannot be placed takes no record: with one
       record a frame and the table full, the text fails and a fallback
       rectangle in the same frame still draws. */
    jb = must_create(&c);
    ok = 1;
    for (i = 0; i < 1024; ++i)
        ok &= jb_texture_create(jb, &d, &many[i]) == JB_OK;
    check("text and full tables: 1024 textures", ok, 1);
    jb_begin_frame(jb, 0);
    check("text and full tables: text, the table full", jb_draw_text(jb, 0, 0, JB_NEUTRAL, "A"),
          JB_ERROR_OUT_OF_MEMORY);
    jb_check(jb, NULL);
    check("text and full tables: the fallback rect", jb_draw_rect(jb, &r), JB_OK);
    check("text and full tables: end", jb_end_frame(jb), JB_OK);
    {
        int fw, fh;
        const uint8_t *f = jb_frame(jb, &fw, &fh);
        check("text and full tables: the rect drawn",
              f != NULL && f[0] == 255 && f[1] == 255 && f[2] == 255,
              1);
    }
    jb_destroy(jb);
}

/* JB_TINT_BLACK draws a sprite's texels black, where a tint of 0 is
   JB_NEUTRAL (the texels as they are). */
static void sprite_tint_black(void)
{
    jb_context *jb = ctx(1, 1, 0, 0, 0);
    jb_texture t = make(jb, 16, 16, 15, 0, 7);
    jb_sprite s;
    uint16_t px[16 * 16];
    int i, black = 1, kept = 0;

    memset(&s, 0, sizeof s);
    s.texture = &t;
    s.x = s.y = 10;
    s.tint = JB_TINT_BLACK;
    jb_begin_frame(jb, JB_RGB(200, 200, 200));
    check("tint black: draw", jb_draw_sprite(jb, &s), JB_OK);
    jb_end_frame(jb);
    jb_vram_read(jb, 10, 10, 16, 16, px);
    for (i = 0; i < 256; ++i)
        black &= (px[i] & 0x7fff) == 0;
    check("tint black: every texel black", black, 1);
    jb_vram_read(jb, 9, 9, 1, 1, px);
    check("tint black: the clear around it", px[0], JB_RGB_TO_15(JB_RGB(200, 200, 200)));
    s.tint = 0;
    jb_begin_frame(jb, JB_RGB(200, 200, 200));
    jb_draw_sprite(jb, &s);
    jb_end_frame(jb);
    jb_vram_read(jb, 10, 10 + 240, 16, 16, px); /* the second buffer */
    for (i = 0; i < 256; ++i)
        kept += (px[i] & 0x7fff) == (pix[i] & 0x7fff);
    check("tint 0: the texels as they are", kept, 256);
    check("tint black: no error", jb_check(jb, NULL), JB_OK);
    jb_destroy(jb);
}

/* w, h alone scale the rest of the texture (src_w, src_h 0), up or down:
   the same pixels as with the source given. */
static void sprite_size_alone(void)
{
    static const int sizes[3][2] = {{40, 30}, {8, 6}, {17, 17}};
    int i;

    for (i = 0; i < 3; ++i) {
        jb_context *a = ctx(1, 1, 0, 0, 0), *b = ctx(1, 1, 0, 0, 0);
        jb_texture ta = make(a, 17, 9, 15, 0, 3), tb = make(b, 17, 9, 15, 0, 3);
        jb_sprite s;
        char what[64];

        memset(&s, 0, sizeof s);
        s.x = s.y = 5, s.u = 2, s.v = 1;
        s.w = sizes[i][0], s.h = sizes[i][1];
        s.texture = &ta;
        jb_begin_frame(a, 0);
        check("size alone", jb_draw_sprite(a, &s), JB_OK);
        jb_end_frame(a);
        s.texture = &tb, s.src_w = 15, s.src_h = 8; /* the rest from (2, 1) */
        jb_begin_frame(b, 0);
        check("size with its source", jb_draw_sprite(b, &s), JB_OK);
        jb_end_frame(b);
        snprintf(what, sizeof what, "w, h %d x %d alone: the rest of the texture scaled",
                 sizes[i][0], sizes[i][1]);
        same(what, a, b);
        jb_destroy(a);
        jb_destroy(b);
    }
}

int main(void)
{
    sprite_tint_black();
    sprite_size_alone();
    text_and_full_tables();
    font_atlas_search();
    sprite_texture_fields();
    repeat_source_limit();
    huge_scale_kept();
    text_size_overflow();
    text_without_glyphs();
    font_palettes_apart();
    sprite_equals_rect(1, 1);
    sprite_equals_rect(2, 1);
    sprite_equals_rect(3, 1);
    sprite_equals_rect(2, 4);
    scaled_equals_quad(1);
    scaled_equals_quad(2);
    scaled_texels();
    flips_and_clipping();
    huge();
    repeat();
    text();
    errors();
    return finish("api_draw2d");
}

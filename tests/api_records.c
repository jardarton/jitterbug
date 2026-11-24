/*
 * api_records: the friendly level's machinery from the inside
 * (src/api/record.h, src/api/frame.h, src/geom/split.h).
 *
 *   equivalence  a seeded run of primitives of every kind (within the
 *                range rule) and state words between them, drawn three
 *                ways on three contexts: A by the jb_prim_* calls, B added
 *                to a jb_ot and drawn by jb_ot_draw, C as internal records
 *                with integer sub-pixel data (jbi_record_draw). The whole
 *                1x VRAM and jb_frame must be the same for all three, at
 *                1x, 2x and 3x, with 1 and 4 threads, with the float
 *                options off and on (on, with no GTE projections, so C,
 *                whose vertices are its own, must equal the table path).
 *   table        with GTE projections in the precise table, A and B (the
 *                ordering table, drawn later) still agree, at 1x and 2x.
 *   sub-pixel    at 2x with precise vertices and perspective-correct
 *                textures, textured polygons from GTE projections drawn by
 *                jb_prim_poly (the precise table) and as records whose
 *                sub-pixel data is what the table holds for their
 *                vertices: the same scaled picture, and not the picture
 *                the options off give.
 *   splitter     pass-through unchanged (a quad back in Z order); culling;
 *                every piece within the GPU's limits for random huge
 *                polygons and the extreme ints; the interpolation at a
 *                cut, linear and perspective-correct.
 *   frame        huge polygons drawn additively through jb_draw_poly in a
 *                1024 x 512 and a 320 x 240 frame: no pixel twice, none
 *                missing inside, none outside; a Gouraud one's colours
 *                near the exact interpolation; long lines clipped onto
 *                their exact pixels; the rectangle clip texel-exact
 *                (flipped and not) against jb_prim_rect; the scene buckets
 *                far to near, FIFO within a bucket, between the background
 *                and overlay layers; where out of range; the frame full.
 *
 * It compiles the public API's sources itself (tests/CMakeLists.txt), as
 * their internal functions are not exported from a shared library.
 */
#include "api_test.h"

#include "api/api.h"
#include "api/frame.h"
#include "api/record.h"
#include "geom/split.h"

enum { STEPS = 500 };

static void noise(jb_context *jb, uint32_t seed)
{
    uint16_t *v = must_alloc((size_t)VRAM_PIXELS * 2);
    uint32_t s = seed;
    int i;

    for (i = 0; i < VRAM_PIXELS; ++i) {
        s = s * 1664525u + 1013904223u;
        v[i] = (uint16_t)(s >> 16);
    }
    check("noise", jb_vram_load(jb, 0, 0, VRAM_W, VRAM_H, v), JB_OK);
    free(v);
}

static jb_context *make(int scale, int threads, int options)
{
    jb_config c;
    jb_context *jb;

    memset(&c, 0, sizeof c);
    c.scale = scale;
    c.threads = threads;
    c.precise_vertices = options;
    c.perspective_correct = options;
    jb = must_create(&c);
    noise(jb, 77);
    check("area", jb_set_draw_area(jb, 0, 0, 1023, 511), JB_OK);
    check("offset", jb_set_draw_offset(jb, 40, 30), JB_OK);
    gp1(jb, JB_GP1_DISPLAY_ENABLE(1));
    return jb;
}

/* Two contexts' 1x VRAM and frames the same. */
static void same(const char *what, jb_context *a, jb_context *b)
{
    uint16_t *va = must_alloc((size_t)VRAM_PIXELS * 2), *vb = must_alloc((size_t)VRAM_PIXELS * 2);
    int wa, ha, wb, hb;
    const uint8_t *fa, *fb;
    uint8_t *copy;
    size_t n;
    char msg[192];

    jb_vram_read(a, 0, 0, VRAM_W, VRAM_H, va);
    jb_vram_read(b, 0, 0, VRAM_W, VRAM_H, vb);
    snprintf(msg, sizeof msg, "%s: VRAM", what);
    check(msg, memcmp(va, vb, (size_t)VRAM_PIXELS * 2) == 0, 1);
    fa = jb_frame(a, &wa, &ha);
    n = (size_t)wa * (size_t)ha * 4;
    copy = must_alloc(n);
    memcpy(copy, fa, n);
    fb = jb_frame(b, &wb, &hb);
    snprintf(msg, sizeof msg, "%s: frame", what);
    check(msg, wa == wb && ha == hb && memcmp(copy, fb, n) == 0, 1);
    free(copy);
    free(va);
    free(vb);
}

/* ------------------------------------------------------------------------ */
/* Equivalence */

static void gen_vertex(gen *g, jb_vertex *v)
{
    v->x = rnd_below(g, 30) == 0 ? rnd_below(g, 2048) - 1024 : 100 + rnd_below(g, 200);
    v->y = rnd_below(g, 30) == 0 ? rnd_below(g, 2048) - 1024 : 60 + rnd_below(g, 140);
    v->color = rnd(g) & 0xffffffu;
    if (rnd_below(g, 4) == 0)
        v->color = JB_NEUTRAL;
    v->u = rnd_below(g, 256);
    v->v = rnd_below(g, 256);
}

static void gen_texture(gen *g, jb_texture *t)
{
    static const int depths[3] = {4, 8, 15};

    memset(t, 0, sizeof *t);
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

/* A record of C's: integer sub-pixel data for n vertices. */
static void integer_sub(jbi_record *r, const jb_vertex *v, int n)
{
    int i;

    r->sub = 1;
    r->sp.known = (1u << n) - 1;
    for (i = 0; i < n; ++i) {
        r->sp.v[i].x = v[i].x * JBI_PRECISE_ONE;
        r->sp.v[i].y = v[i].y * JBI_PRECISE_ONE;
        r->sp.v[i].z = 0;
    }
}

static void step(gen *g, jb_context *a, jb_ot *ot, jb_context *c)
{
    jb_texture tex;
    jbi_record r;
    int k = rnd_below(g, 10);

    memset(&r, 0, sizeof r);
    gen_texture(g, &tex);
    if (k < 4) {
        jb_poly p;
        int i;

        memset(&p, 0, sizeof p);
        p.vertices = rnd_below(g, 2) ? 4 : 3;
        p.gouraud = rnd_below(g, 2);
        p.blend = gen_blend(g);
        p.raw = rnd_below(g, 3) == 0;
        p.texture = rnd_below(g, 3) ? &tex : NULL;
        for (i = 0; i < 4; ++i)
            gen_vertex(g, &p.v[i]);
        check("prim_poly", jb_prim_poly(a, &p), JB_OK);
        check("ot_add_poly", jb_ot_add_poly(ot, 0, &p), JB_OK);
        r.kind = JBI_REC_POLY;
        r.u.poly = p;
        r.u.poly.texture = NULL;
        if (p.texture) {
            r.textured = 1;
            r.tex = tex;
        }
        integer_sub(&r, p.v, p.vertices);
        jbi_record_draw(c, &r);
    } else if (k < 6) {
        jb_line l;

        l.gouraud = rnd_below(g, 2);
        l.blend = gen_blend(g);
        gen_vertex(g, &l.v[0]);
        gen_vertex(g, &l.v[1]);
        check("prim_line", jb_prim_line(a, &l), JB_OK);
        check("ot_add_line", jb_ot_add_line(ot, 0, &l), JB_OK);
        r.kind = JBI_REC_LINE;
        r.u.line = l;
        integer_sub(&r, l.v, 2);
        jbi_record_draw(c, &r);
    } else if (k < 7) {
        jb_vertex v[8];
        jb_polyline l;
        size_t i;

        l.count = (size_t)(2 + rnd_below(g, 6));
        l.gouraud = rnd_below(g, 2);
        l.blend = gen_blend(g);
        l.v = v;
        for (i = 0; i < l.count; ++i)
            gen_vertex(g, &v[i]);
        check("prim_polyline", jb_prim_polyline(a, &l), JB_OK);
        check("ot_add_polyline", jb_ot_add_polyline(ot, 0, &l), JB_OK);
        for (i = 0; i + 1 < l.count; ++i) {
            memset(&r, 0, sizeof r);
            r.kind = JBI_REC_LINE;
            r.u.line = jbi_polyline_segment(&l, i);
            integer_sub(&r, r.u.line.v, 2);
            jbi_record_draw(c, &r);
        }
    } else if (k < 9) {
        static const int sizes[5] = {1, 8, 16, 37, 0};
        jb_rect q;

        memset(&q, 0, sizeof q);
        q.x = 100 + rnd_below(g, 200);
        q.y = 60 + rnd_below(g, 140);
        q.w = sizes[rnd_below(g, 5)];
        q.h = rnd_below(g, 2) ? q.w : rnd_below(g, 60);
        q.color = rnd(g) & 0xffffffu;
        q.blend = gen_blend(g);
        q.texture = rnd_below(g, 2) ? &tex : NULL;
        q.raw = rnd_below(g, 2);
        q.u = rnd_below(g, 256);
        q.v = rnd_below(g, 256);
        q.flip_x = rnd_below(g, 2);
        q.flip_y = rnd_below(g, 2);
        check("prim_rect", jb_prim_rect(a, &q), JB_OK);
        check("ot_add_rect", jb_ot_add_rect(ot, 0, &q), JB_OK);
        r.kind = JBI_REC_RECT;
        r.u.rect = q;
        r.u.rect.texture = NULL;
        if (q.texture) {
            r.textured = 1;
            r.tex = tex;
        }
        jbi_record_draw(c, &r);
    } else {
        /* State between them: a window, the mask, the draw mode. */
        uint32_t w;
        int m = rnd_below(g, 4);

        if (m == 0)
            w = JB_GP0_TEXTURE_WINDOW(rnd_below(g, 32), rnd_below(g, 32), rnd_below(g, 32),
                                      rnd_below(g, 32));
        else if (m == 1)
            w = JB_GP0_MASK(rnd_below(g, 4) == 0, rnd_below(g, 4) == 0);
        else if (m == 2)
            w = JB_GP0_DRAW_MODE(64 * rnd_below(g, 16), 0, rnd_below(g, 5), 8, rnd_below(g, 2));
        else
            w = JB_GP0_CLEAR_CACHE;
        gp0_1(a, w);
        /* B gets state at once (the console level), so its table is drawn
           first. */
        check("ot_draw", jb_ot_draw(ot->jb, ot), JB_OK);
        check("ot_clear", jb_ot_clear(ot), JB_OK);
        gp0_1(ot->jb, w);
        r.kind = JBI_REC_STATE;
        r.u.word = w;
        jbi_record_draw(c, &r);
    }
}

static void equivalence(int scale, int threads, int options)
{
    jb_context *a = make(scale, threads, options), *b = make(scale, threads, options);
    jb_context *c = make(scale, threads, options);
    jb_ot *ot = jb_ot_create(b, 4, 64, NULL);
    gen g = {12345u + (uint64_t)scale * 7 + (uint64_t)threads, 0, 0};
    char what[96];
    int i;

    for (i = 0; i < STEPS; ++i) {
        step(&g, a, ot, c);
        if (ot->count > 40) {
            jb_ot_draw(b, ot);
            jb_ot_clear(ot);
        }
    }
    jb_ot_draw(b, ot);
    snprintf(what, sizeof what, "equivalence %dx %dt options %d: ot", scale, threads, options);
    same(what, a, b);
    snprintf(what, sizeof what, "equivalence %dx %dt options %d: records", scale, threads,
             options);
    same(what, a, c);
    jb_ot_destroy(ot);
    jb_destroy(a);
    jb_destroy(b);
    jb_destroy(c);
}

/* ------------------------------------------------------------------------ */
/* GTE projections */

static void gte_setup(jb_context *jb)
{
    jb_gte_write(jb, JB_GTE_RT11RT12, JB_GTE_PAIR(4096, 0));
    jb_gte_write(jb, JB_GTE_RT13RT21, 0);
    jb_gte_write(jb, JB_GTE_RT22RT23, JB_GTE_PAIR(4096, 0));
    jb_gte_write(jb, JB_GTE_RT31RT32, 0);
    jb_gte_write(jb, JB_GTE_RT33, 4096);
    jb_gte_write(jb, JB_GTE_TRX, 0);
    jb_gte_write(jb, JB_GTE_TRY, 0);
    jb_gte_write(jb, JB_GTE_TRZ, 0);
    jb_gte_write(jb, JB_GTE_H, 260);
    jb_gte_write(jb, JB_GTE_OFX, 160u << 16);
    jb_gte_write(jb, JB_GTE_OFY, 120u << 16);
}

static uint32_t project(jb_context *jb, int x, int y, int z)
{
    jb_gte_write(jb, JB_GTE_VXY0, JB_GTE_PAIR(x, y));
    jb_gte_write(jb, JB_GTE_VZ0, (uint32_t)z);
    jb_gte_command(jb, JB_GTE_RTPS);
    return jb_gte_read(jb, JB_GTE_SXY2);
}

enum { TABLE_POLYS = 150 };

/* With projections in the table: A (jb_prim_*) and B (jb_ot, drawn after
   all of them) agree. Both project everything first, so both look up the
   same table. */
static void with_table(int scale)
{
    jb_context *a = make(scale, 1, 1), *b = make(scale, 1, 1);
    jb_ot *ot = jb_ot_create(b, 3, TABLE_POLYS, NULL);
    static jb_poly polys[TABLE_POLYS];
    static jb_texture texs[TABLE_POLYS];
    gen g = {999, 0, 0};
    int i, k;
    char what[48];

    gte_setup(a);
    gte_setup(b);
    for (i = 0; i < TABLE_POLYS; ++i) {
        jb_poly *p = &polys[i];

        memset(p, 0, sizeof *p);
        gen_texture(&g, &texs[i]);
        p->vertices = 3 + rnd_below(&g, 2);
        p->texture = &texs[i];
        p->gouraud = 1;
        for (k = 0; k < p->vertices; ++k) {
            int x = rnd_below(&g, 600) - 300, y = rnd_below(&g, 400) - 200;
            int z = 300 + rnd_below(&g, 1200);
            uint32_t sa = project(a, x, y, z);

            check("same projection", project(b, x, y, z), sa);
            p->v[k].x = JB_GTE_SX(sa);
            p->v[k].y = JB_GTE_SY(sa);
            p->v[k].color = JB_NEUTRAL;
            p->v[k].u = rnd_below(&g, 256);
            p->v[k].v = rnd_below(&g, 256);
        }
    }
    for (i = 0; i < TABLE_POLYS; ++i) {
        check("table prim", jb_prim_poly(a, &polys[i]), JB_OK);
        check("table ot", jb_ot_add_poly(ot, 1, &polys[i]), JB_OK);
    }
    check("ot full", jb_ot_add_poly(ot, 1, &polys[0]), JB_ERROR_OUT_OF_MEMORY);
    check("ot_draw", jb_ot_draw(b, ot), JB_OK);
    snprintf(what, sizeof what, "table %dx", scale);
    same(what, a, b);
    jb_ot_destroy(ot);
    jb_destroy(a);
    jb_destroy(b);
}

/* Records with the table's own sub-pixel data draw what the table path
   draws. */
static void subpixel(void)
{
    jb_context *p = make(2, 1, 1), *q = make(2, 1, 1), *plain = make(2, 1, 0);
    gen g = {4242, 0, 0};
    int i, k, w, h;
    const uint8_t *f;
    uint8_t *copy;

    gte_setup(p);
    for (i = 0; i < 200; ++i) {
        jb_poly poly;
        jb_texture tex;
        jbi_record r;
        uint32_t keys[4];
        jbi_precise_vertex pv[4];
        unsigned hits;

        memset(&poly, 0, sizeof poly);
        memset(&r, 0, sizeof r);
        gen_texture(&g, &tex);
        poly.vertices = 3 + rnd_below(&g, 2);
        poly.texture = &tex;
        poly.gouraud = rnd_below(&g, 2);
        for (k = 0; k < poly.vertices; ++k) {
            int x = rnd_below(&g, 400) - 200, y = rnd_below(&g, 300) - 150;
            uint32_t s = project(p, x, y, 400 + rnd_below(&g, 1500));

            /* The words carry SXY as it is; the decoder adds the offset. */
            poly.v[k].x = JB_GTE_SX(s);
            poly.v[k].y = JB_GTE_SY(s);
            poly.v[k].color = rnd(&g) & 0xffffffu;
            poly.v[k].u = rnd_below(&g, 256);
            poly.v[k].v = rnd_below(&g, 256);
            keys[k] = JB_GP0_XY(poly.v[k].x, poly.v[k].y);
        }
        hits = jbi_precise_lookup_poly(p->api->precise, keys, poly.vertices, pv);
        check("the table knows the vertices", hits, (1u << poly.vertices) - 1);
        check("subpixel prim", jb_prim_poly(p, &poly), JB_OK);
        check("plain prim", jb_prim_poly(plain, &poly), JB_OK);
        r.kind = JBI_REC_POLY;
        r.u.poly = poly;
        r.u.poly.texture = NULL;
        r.textured = 1;
        r.tex = tex;
        r.sub = 1;
        r.sp.known = hits;
        for (k = 0; k < poly.vertices; ++k)
            r.sp.v[k] = pv[k];
        jbi_record_draw(q, &r);
    }
    same("sub-pixel data = the precise table at 2x", p, q);
    f = jb_frame(q, &w, &h);
    copy = must_alloc((size_t)w * h * 4);
    memcpy(copy, f, (size_t)w * h * 4);
    check("the options change the picture",
          memcmp(copy, jb_frame(plain, NULL, NULL), (size_t)w * h * 4) != 0, 1);
    free(copy);
    jb_destroy(p);
    jb_destroy(q);
    jb_destroy(plain);
}

/* ------------------------------------------------------------------------ */
/* The splitter */

typedef struct {
    int pieces, bad;
    jbi_split_vertex last[4];
    int last_n;
    /* A vertex looked for (find), and what it had. */
    int64_t fx, fy;
    int found, fz, fu, fpu, fr;
} collected;

static void collect(void *user, int poly, const jbi_split_vertex *v, int n, unsigned passes,
                    const jbi_split_area *area)
{
    collected *c = user;
    int i;

    (void)poly;
    (void)area;
    c->bad += passes == 0 || passes > 3;

    ++c->pieces;
    c->last_n = n;
    for (i = 0; i < n; ++i)
        c->last[i] = v[i];
    if (n == 3) {
        c->bad += !jbi_split_triangle_fits(&v[0], &v[1], &v[2]);
    } else if (n == 4) {
        c->bad += !jbi_split_triangle_fits(&v[0], &v[1], &v[2]);
        c->bad += !jbi_split_triangle_fits(&v[1], &v[2], &v[3]);
    } else {
        ++c->bad;
    }
    for (i = 0; i < n; ++i) {
        int64_t dx = v[i].qx - (int64_t)v[i].x * 256, dy = v[i].qy - (int64_t)v[i].y * 256;

        c->bad += dx > 288 || dx < -288 || dy > 288 || dy < -288;
        c->bad += v[i].r < 0 || v[i].r > 255 || v[i].u < 0 || v[i].u > 255;
        c->bad += v[i].v < 0 || v[i].v > 255 || v[i].z < 0;
        if (v[i].qx == c->fx && v[i].qy == c->fy) {
            c->found = 1;
            c->fz = v[i].z;
            c->fu = v[i].u;
            c->fpu = v[i].pu;
            c->fr = v[i].r;
        }
    }
}

static jbi_split_vertex sv(int x, int y)
{
    jbi_split_vertex v;

    memset(&v, 0, sizeof v);
    v.x = x;
    v.y = y;
    v.qx = (int64_t)x * 256;
    v.qy = (int64_t)y * 256;
    return v;
}

static void splitter_units(void)
{
    jbi_split_view view = {0, 0, 319, 239, 0, 0};
    jbi_split_vertex v[4];
    collected c;
    gen g = {31337, 0, 0};
    int i, k, persp;

    /* Pass-through: a quad given cyclically comes back in Z order. */
    v[0] = sv(10, 10);
    v[1] = sv(100, 12);
    v[2] = sv(110, 90);
    v[3] = sv(5, 80);
    memset(&c, 0, sizeof c);
    check("pass-through quad", jbi_split_poly(&view, v, 4, collect, &c), 1);
    check("quad Z order", c.last_n == 4 && c.last[0].x == 10 && c.last[1].x == 100 &&
                              c.last[2].x == 5 && c.last[3].x == 110, 1);
    check("n 2", jbi_split_poly(&view, v, 2, collect, &c), -1);
    check("n 11", jbi_split_poly(&view, v, 11, collect, &c), -1);
    /* Culled: all beyond the left edge. */
    v[0] = sv(-500, 10);
    v[1] = sv(-400, 12);
    v[2] = sv(-450, 900);
    check("culled", jbi_split_poly(&view, v, 3, collect, &c), 0);
    check("culled line", jbi_split_line(&view, &v[0], &v[1], NULL, NULL), 0);
    /* Random huge polygons: every piece within the limits. */
    for (i = 0; i < 3000; ++i) {
        jbi_split_view big;
        jbi_split_vertex p[5];
        int n = 3 + rnd_below(&g, 2);

        big.x0 = 0;
        big.y0 = 0;
        big.x1 = rnd_below(&g, 1024);
        big.y1 = rnd_below(&g, 512);
        big.perspective = rnd_below(&g, 2);
        big.perspective_given = 0;
        for (k = 0; k < 3; ++k) {
            int span = rnd_below(&g, 4) == 0 ? 200000 : 5000;

            p[k] = sv(rnd_below(&g, span) - span / 2, rnd_below(&g, span) - span / 2);
            /* Now and then on the range's edge, and a sub-pixel position
               anywhere in the slack around the integer one (split.h). */
            if (rnd_below(&g, 4) == 0)
                p[k] = sv(rnd_below(&g, 2) ? 1023 : -1024, p[k].y);
            if (rnd_below(&g, 4) == 0)
                p[k] = sv(p[k].x, rnd_below(&g, 2) ? 1023 : -1024);
            if (rnd_below(&g, 2)) {
                p[k].qx += rnd_below(&g, 511) - 255;
                p[k].qy += rnd_below(&g, 511) - 255;
            }
            p[k].z = rnd_below(&g, 3) ? 1 + rnd_below(&g, 60000) : 0;
            p[k].r = rnd_below(&g, 256);
            p[k].u = rnd_below(&g, 256);
            p[k].v = rnd_below(&g, 256);
        }
        if (n == 4) { /* a parallelogram: convex */
            p[3] = sv(p[0].x + p[2].x - p[1].x, p[0].y + p[2].y - p[1].y);
            p[3].z = p[1].z;
        }
        memset(&c, 0, sizeof c);
        jbi_split_poly(&big, p, n, collect, &c);
        check("pieces within the limits", c.bad, 0);
    }
    /* The extreme ints. */
    v[0] = sv(0x7fffffff, -0x7fffffff);
    v[1] = sv(-0x7fffffff, -0x7fffffff);
    v[2] = sv(0, 0x7fffffff);
    memset(&c, 0, sizeof c);
    check("extreme pieces", jbi_split_poly(&view, v, 3, collect, &c) > 0, 1);
    check("extreme within limits", c.bad, 0);
    /* A cut at x = -1024 (the GPU's range) on the edge from (-2000, 100),
       z 100, u 0, to (2000, 100), z 300, u 200: t = 976 / 4000 there.
       Linear: u = 48.8, so 49. With 1/z linear: z = 120000000 / 1004800 =
       119.4, so 119, and u = 200 * 97600 / 1004800 = 19.4, so 19
       perspective-correct. Both are kept whatever view.perspective says
       (it is not read): the choice is made when the piece is drawn. The
       view takes in the cut, so that its piece is not culled. */
    for (persp = 0; persp < 2; ++persp) {
        jbi_split_view pv = {-1024, 0, -1, 511, 0, 0};
        jbi_split_vertex t[3];

        pv.perspective = persp;
        t[0] = sv(-2000, 100);
        t[0].z = 100;
        t[1] = sv(2000, 100);
        t[1].z = 300;
        t[1].u = 200;
        t[2] = sv(0, 400);
        t[2].z = 100;
        memset(&c, 0, sizeof c);
        c.fx = -1024 * 256;
        c.fy = 100 * 256;
        jbi_split_poly(&pv, t, 3, collect, &c);
        check("cut vertex found", c.found, 1);
        check("cut depth", c.fz, 119);
        check("cut texel, linear", c.fu, 49);
        check("cut texel, perspective-correct", c.fpu, 19);
    }
}

/* ------------------------------------------------------------------------ */
/* Drawing through the frame */

static jb_context *frame_context(int w, int h, int scale)
{
    jb_config c;

    memset(&c, 0, sizeof c);
    c.width = w;
    c.height = h;
    c.scale = scale;
    return must_create(&c);
}

static double root(double sq)
{
    double t = sq > 1 ? sq : 1;
    int i;

    for (i = 0; i < 80; ++i)
        t = 0.5 * (t + sq / t);
    return t;
}

/* Additive coverage of a huge flat triangle in a w x h frame. */
static void coverage(int w, int h, const int *xy, int scale)
{
    jb_context *jb = frame_context(w, h, scale);
    uint16_t *v = must_alloc((size_t)VRAM_PIXELS * 2);
    jb_poly p;
    int x, y, twice = 0, missing = 0, stray = 0, k;
    double e[3][3];
    char msg[96];

    memset(&p, 0, sizeof p);
    p.vertices = 3;
    p.blend = JB_BLEND_ADD;
    for (k = 0; k < 3; ++k) {
        p.v[k].x = xy[2 * k];
        p.v[k].y = xy[2 * k + 1];
        p.v[k].color = JB_RGB(64, 64, 64);
    }
    check("begin", jb_begin_frame(jb, 0), JB_OK);
    check("draw huge", jb_draw_poly(jb, &p), JB_OK);
    check("end", jb_end_frame(jb), JB_OK);
    jb_vram_read(jb, 0, 0, VRAM_W, VRAM_H, v);
    /* Edge functions as distances in pixels, positive inside. */
    for (k = 0; k < 3; ++k) {
        double ax = xy[2 * k], ay = xy[2 * k + 1];
        double bx = xy[2 * ((k + 1) % 3)], by = xy[2 * ((k + 1) % 3) + 1];
        double cx = xy[2 * ((k + 2) % 3)], cy = xy[2 * ((k + 2) % 3) + 1];
        double nx = ay - by, ny = bx - ax, len = root(nx * nx + ny * ny);

        e[k][0] = nx / len;
        e[k][1] = ny / len;
        e[k][2] = -(e[k][0] * ax + e[k][1] * ay);
        if (e[k][0] * cx + e[k][1] * cy + e[k][2] < 0) {
            e[k][0] = -e[k][0];
            e[k][1] = -e[k][1];
            e[k][2] = -e[k][2];
        }
    }
    for (y = 0; y < h; ++y) {
        for (x = 0; x < w; ++x) {
            unsigned px = v[y * VRAM_W + x] & 0x1f;
            double d = 1e9;

            for (k = 0; k < 3; ++k) {
                double dk = e[k][0] * x + e[k][1] * y + e[k][2];

                d = dk < d ? dk : d;
            }
            twice += px > 8;
            if (d > 1.5)
                missing += px != 8 && x < 1023; /* a polygon cannot reach x 1023 (jitterbug.h, 2D drawing) */
            if (d < -1.5)
                stray += px != 0;
        }
    }
    snprintf(msg, sizeof msg, "huge triangle in %dx%d at %dx: drawn twice", w, h, scale);
    check(msg, twice, 0);
    snprintf(msg, sizeof msg, "huge triangle in %dx%d at %dx: missing inside", w, h, scale);
    check(msg, missing, 0);
    snprintf(msg, sizeof msg, "huge triangle in %dx%d at %dx: outside", w, h, scale);
    check(msg, stray, 0);
    free(v);
    jb_destroy(jb);
}

static void huge_polygons(void)
{
    static const int tris[][6] = {
        {-3000, -100, 4000, 200, 300, 3000},
        {-50000, 250, 60000, 240, 500, -70000},
        {10, 10, 1010, 20, 500, 505}, /* fits: passed through */
        {-5, -5, 1030, 0, 0, 520},    /* just over: cut */
        {1023, 511, -900, 500, 512, -400},
    };
    size_t i;

    for (i = 0; i < sizeof tris / sizeof tris[0]; ++i) {
        coverage(1024, 512, tris[i], 1);
        coverage(320, 240, tris[i], 1);
    }
    coverage(1024, 512, tris[0], 2);
    coverage(1024, 512, tris[3], 3);
}

/* A Gouraud triangle far beyond the frame: its colours where the exact
   interpolation has them, give or take rounding and the GPU's 5 bits. */
static void gouraud_clip(void)
{
    jb_context *jb = frame_context(320, 240, 1);
    uint16_t *v = must_alloc((size_t)VRAM_PIXELS * 2);
    static const int xy[6] = {-2000, -1500, 2400, 0, 0, 2600};
    static const int col[3][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
    jb_poly p;
    int x, y, k, worst = 0;
    double det;

    memset(&p, 0, sizeof p);
    p.vertices = 3;
    p.gouraud = 1;
    for (k = 0; k < 3; ++k) {
        p.v[k].x = xy[2 * k];
        p.v[k].y = xy[2 * k + 1];
        p.v[k].color = JB_RGB(col[k][0], col[k][1], col[k][2]);
    }
    jb_set_dither(jb, 0);
    check("begin gouraud", jb_begin_frame(jb, 0), JB_OK);
    check("draw gouraud", jb_draw_poly(jb, &p), JB_OK);
    check("end gouraud", jb_end_frame(jb), JB_OK);
    jb_vram_read(jb, 0, 0, VRAM_W, VRAM_H, v);
    det = (double)(xy[2] - xy[0]) * (xy[5] - xy[1]) - (double)(xy[4] - xy[0]) * (xy[3] - xy[1]);
    for (y = 0; y < 240; y += 3) {
        for (x = 0; x < 320; x += 3) {
            double l1 = ((double)(x - xy[0]) * (xy[5] - xy[1]) -
                         (double)(xy[4] - xy[0]) * (y - xy[1])) / det;
            double l2 = ((double)(xy[2] - xy[0]) * (y - xy[1]) -
                         (double)(x - xy[0]) * (xy[3] - xy[1])) / det;
            double l0 = 1 - l1 - l2;
            unsigned px = v[y * VRAM_W + x];
            int c;

            for (c = 0; c < 3; ++c) {
                double want = (l0 * col[0][c] + l1 * col[1][c] + l2 * col[2][c]) / 8;
                double d = (double)((px >> (5 * c)) & 31) - want;

                d = d < 0 ? -d : d;
                worst = d > worst ? (int)(d + 0.999) : worst;
            }
        }
    }
    check("gouraud colours within 2 of 31", worst <= 2, 1);
    free(v);
    jb_destroy(jb);
}

static void lines_clip(void)
{
    jb_context *jb = frame_context(320, 240, 1);
    uint16_t *v = must_alloc((size_t)VRAM_PIXELS * 2);
    jb_line l;
    int x, y, wrong = 0;

    memset(&l, 0, sizeof l);
    jb_set_dither(jb, 0);
    check("begin lines", jb_begin_frame(jb, 0), JB_OK);
    l.v[0].color = JB_RGB(255, 255, 255);
    l.v[0].x = -5000;
    l.v[0].y = 100;
    l.v[1].x = 5000;
    l.v[1].y = 100;
    check("long line", jb_draw_line(jb, &l), JB_OK);
    l.v[0].x = -2000;
    l.v[0].y = -2000;
    l.v[1].x = 3000;
    l.v[1].y = 3000;
    check("diagonal line", jb_draw_line(jb, &l), JB_OK);
    l.v[0].x = 50;
    l.v[0].y = -90000;
    l.v[1].x = 50;
    l.v[1].y = 90000;
    check("vertical line", jb_draw_line(jb, &l), JB_OK);
    l.v[0].x = -90000;
    l.v[0].y = -5;
    l.v[1].x = 90000;
    l.v[1].y = -5;
    check("line above the frame", jb_draw_line(jb, &l), JB_OK);
    check("end lines", jb_end_frame(jb), JB_OK);
    jb_vram_read(jb, 0, 0, VRAM_W, VRAM_H, v);
    for (y = 0; y < 240; ++y)
        for (x = 0; x < 320; ++x) {
            int want = y == 100 || x == y || x == 50;

            wrong += ((v[y * VRAM_W + x] & 0x7fff) != 0) != want;
        }
    check("clipped lines on their pixels", wrong, 0);
    free(v);
    jb_destroy(jb);
}

/* A rectangle clipped by the frame: texel for texel what the part in the
   frame draws through jb_prim_rect. */
static void rect_clip(void)
{
    static const char *names[4] = {"rect clip", "rect clip x-flipped", "rect clip y-flipped",
                                   "rect clip both flipped"};
    int flip;

    for (flip = 0; flip < 4; ++flip) {
        jb_context *a = frame_context(320, 240, 1), *b = frame_context(320, 240, 1);
        jb_texture tex;
        jb_rect r, q;

        memset(&tex, 0, sizeof tex);
        tex.page_x = 512;
        tex.depth = JB_TEX_15BIT;
        noise(a, 5);
        noise(b, 5);
        memset(&r, 0, sizeof r);
        r.x = -1100;
        r.y = -700;
        r.w = 1300;
        r.h = 800;
        r.texture = &tex;
        r.color = JB_NEUTRAL;
        r.u = 17;
        r.v = 201;
        r.flip_x = flip & 1;
        r.flip_y = flip >> 1;
        check("begin a", jb_begin_frame(a, 0), JB_OK);
        check("draw clipped rect", jb_draw_rect(a, &r), JB_OK);
        check("end a", jb_end_frame(a), JB_OK);
        check("begin b", jb_begin_frame(b, 0), JB_OK);
        q = r;
        q.x = 0;
        q.y = 0;
        q.w = 200;
        q.h = 100;
        q.u = (r.flip_x ? r.u - 1100 : r.u + 1100) & 255;
        q.v = (r.flip_y ? r.v - 700 : r.v + 700) & 255;
        check("prim rect", jb_prim_rect(b, &q), JB_OK);
        check("end b", jb_end_frame(b), JB_OK);
        same(names[flip], a, b);
        jb_destroy(a);
        jb_destroy(b);
    }
}

/* ------------------------------------------------------------------------ */
/* Perspective at the cuts: a clipped textured triangle
   with depths, its texels 1 + u, drawn with the option as set at
   jb_end_frame (changed after recording or not), and the 1x VRAM above 1x
   the same with the option on as off. */

typedef struct {
    uint16_t *vram;
    uint8_t *frame;
    size_t bytes;
} picture;

static void picture_free(picture *p)
{
    free(p->vram);
    free(p->frame);
}

/* The triangle recorded with the option at record_opt, drawn with it at
   draw_opt, at scale. */
static picture persp_frame(int scale, int record_opt, int draw_opt)
{
    jb_config c;
    jb_context *jb;
    jbi_record t;
    jbi_split_vertex v[3];
    uint16_t row[256];
    picture pic;
    const uint8_t *f;
    int i, w, h;

    memset(&c, 0, sizeof c);
    c.scale = scale;
    c.perspective_correct = record_opt;
    jb = must_create(&c);
    for (i = 0; i < 256; ++i)
        row[i] = (uint16_t)(1 + i);
    for (i = 0; i < 4; ++i)
        jb_vram_load(jb, 512, i, 256, 1, row);
    memset(&t, 0, sizeof t);
    t.kind = JBI_REC_POLY;
    t.u.poly.raw = 1;
    t.textured = 1;
    t.tex.page_x = 512;
    t.tex.depth = JB_TEX_15BIT;
    v[0] = sv(-2000, 100);
    v[0].z = 100;
    v[1] = sv(2000, 100);
    v[1].z = 300;
    v[1].u = 200;
    v[2] = sv(0, 400);
    v[2].z = 100;
    check("persp begin", jb_begin_frame(jb, 0), JB_OK);
    check("persp record", jbi_frame_add_poly(jb, "t", JBI_FRAME_LAYER, &t, v, 3), JB_OK);
    jb_set_perspective_correct(jb, draw_opt);
    check("persp end", jb_end_frame(jb), JB_OK);
    pic.vram = read_vram(jb);
    f = jb_frame(jb, &w, &h);
    pic.bytes = (size_t)w * (size_t)h * 4;
    pic.frame = must_alloc(pic.bytes);
    memcpy(pic.frame, f, pic.bytes);
    jb_destroy(jb);
    return pic;
}

static int same_picture(const picture *a, const picture *b)
{
    return a->bytes == b->bytes && memcmp(a->frame, b->frame, a->bytes) == 0 &&
           memcmp(a->vram, b->vram, (size_t)VRAM_PIXELS * 2) == 0;
}

static void persp_cuts(void)
{
    int scale;

    for (scale = 1; scale <= 2; ++scale) {
        picture off = persp_frame(scale, 0, 0), on = persp_frame(scale, 1, 1);
        picture off_on = persp_frame(scale, 0, 1), on_off = persp_frame(scale, 1, 0);
        char msg[96];

        snprintf(msg, sizeof msg, "%dx: turned on after recording = on throughout", scale);
        check(msg, same_picture(&off_on, &on), 1);
        snprintf(msg, sizeof msg, "%dx: turned off after recording = off throughout", scale);
        check(msg, same_picture(&on_off, &off), 1);
        snprintf(msg, sizeof msg, "%dx: the option changes the picture", scale);
        check(msg, memcmp(on.frame, off.frame, on.bytes) != 0, 1);
        if (scale > 1) {
            check("2x: the 1x VRAM the same with the option on as off",
                  memcmp(on.vram, off.vram, (size_t)VRAM_PIXELS * 2), 0);
            /* The affine value (104 = 1 + u 103). */
            check("2x: the 1x pixel affine", on.vram[110 * VRAM_W + 120], off.vram[110 * VRAM_W + 120]);
        }
        picture_free(&off);
        picture_free(&on);
        picture_free(&off_on);
        picture_free(&on_off);
    }
}

/* A polygon with no area gives nothing, passed through or clipped
  . */
static void degenerate(void)
{
    jbi_split_view view = {0, 0, 319, 239, 0, 0};
    jbi_split_vertex t[3];
    collected c;

    t[0] = sv(10, 10);
    t[1] = sv(50, 30);
    t[2] = sv(90, 50);
    memset(&c, 0, sizeof c);
    check("collinear, within the limits", jbi_split_poly(&view, t, 3, collect, &c), 0);
    t[0] = sv(-1999, -700);
    t[1] = sv(101, 0);
    t[2] = sv(2201, 700);
    check("collinear, clipped", jbi_split_poly(&view, t, 3, collect, &c), 0);
    check("no pieces", c.pieces, 0);
    /* Area only by the sub-pixel positions: passed through (the precise
       pass may draw it), not clipped. */
    t[0] = sv(10, 10);
    t[1] = sv(50, 30);
    t[2] = sv(90, 50);
    t[1].qy += 100;
    check("precise area, within the limits", jbi_split_poly(&view, t, 3, collect, &c), 1);
}

/* Interpolation of any ints without overflow: the cut at x -8
   of u from INT_MIN at x -2000 to INT_MAX at x 2000 is INT_MIN +
   round(4294967295 * 1992 / 4000), INT_MIN + 2138893713 = -8589935. */
static int overflow_u;

static void overflow_segment(void *user, const jbi_split_vertex *a, const jbi_split_vertex *b)
{
    (void)user;
    (void)b;
    overflow_u = a->u;
}

static void wide_ints(void)
{
    jbi_split_view view = {0, 0, 319, 239, 0, 0};
    jbi_split_vertex a = sv(-2000, 100), b = sv(2000, 100);

    a.u = -0x7fffffff - 1;
    b.u = 0x7fffffff;
    check("wide ints: one segment", jbi_split_line(&view, &a, &b, overflow_segment, NULL), 1);
    check("wide ints: the cut's u, without overflow", overflow_u, -8589935);
}

/* ------------------------------------------------------------------------ */
/* Records and cuts: edge cases, each of which failed before its fix */

/* A perspective-correct triangle is flat or not, and of which
   colour, by its perspective attributes. Affine colours all 127, the
   perspective ones all 128 (or a gradient): a texel of red 12 modulated
   draws 12 by the perspective pass (12 * 128 / 128) and 11 by the affine
   one (12 * 127 / 128). */
static jb_context *persp_context(int scale)
{
    jb_config c;
    jb_context *jb;
    uint16_t row[256];
    int i;

    memset(&c, 0, sizeof c);
    c.scale = scale;
    c.perspective_correct = 1;
    jb = must_create(&c);
    for (i = 0; i < 256; ++i)
        row[i] = JB_RGB15(12, 12, 12);
    for (i = 0; i < 4; ++i)
        jb_vram_load(jb, 512, i, 256, 1, row);
    jb_set_dither(jb, 0);
    jb_set_draw_area(jb, 0, 0, 1023, 511);
    gp1(jb, JB_GP1_DISPLAY_ENABLE(1));
    return jb;
}

static void persp_record(jbi_record *r, const int *pred)
{
    static const int xy[3][2] = {{10, 10}, {200, 20}, {30, 200}};
    static const int z[3] = {100, 150, 200};
    int i;

    memset(r, 0, sizeof *r);
    r->kind = JBI_REC_POLY;
    r->u.poly.vertices = 3;
    r->u.poly.gouraud = 1;
    r->textured = 1;
    r->tex.page_x = 512;
    r->tex.depth = JB_TEX_15BIT;
    r->sub = 1;
    r->sp.known = 7;
    r->sp.has_pattr = 1;
    for (i = 0; i < 3; ++i) {
        r->u.poly.v[i].x = xy[i][0];
        r->u.poly.v[i].y = xy[i][1];
        r->u.poly.v[i].color = JB_RGB(127, 127, 127);
        r->sp.v[i].x = xy[i][0] * 256;
        r->sp.v[i].y = xy[i][1] * 256;
        r->sp.v[i].z = z[i];
        r->sp.pattr[i][2] = (uint8_t)pred[i];
        r->sp.pattr[i][3] = 128;
        r->sp.pattr[i][4] = 128;
    }
}

static void persp_flatness(void)
{
    static const int flat[3] = {128, 128, 128}, ramp[3] = {64, 255, 128};
    jbi_record r;
    jb_context *jb;
    uint16_t px, px2;
    int w, h;
    const uint8_t *f;

    /* 1x: the perspective pass is the 1x one. */
    jb = persp_context(1);
    persp_record(&r, flat);
    jbi_record_draw(jb, &r);
    jb_vram_read(jb, 50, 50, 1, 1, &px);
    check("persp flatness: 1x: flat by the perspective colours", px & 31, 12);
    jb_destroy(jb);
    /* 2x: the scaled picture by the perspective pass, the 1x VRAM by the
       affine one. */
    jb = persp_context(2);
    persp_record(&r, flat);
    jbi_record_draw(jb, &r);
    jb_vram_read(jb, 50, 50, 1, 1, &px);
    check("persp flatness: 2x: the 1x VRAM affine", px & 31, 11);
    f = jb_frame(jb, &w, &h);
    check("persp flatness: 2x: the scaled picture perspective", f[((size_t)100 * w + 100) * 4],
          widen5(12));
    jb_destroy(jb);
    /* A gradient in the perspective colours only: shaded at 1x. */
    jb = persp_context(1);
    persp_record(&r, ramp);
    jbi_record_draw(jb, &r);
    jb_vram_read(jb, 20, 20, 1, 1, &px);
    jb_vram_read(jb, 150, 25, 1, 1, &px2);
    check("persp flatness: 1x: the perspective gradient drawn", (px & 31) != (px2 & 31), 1);
    jb_destroy(jb);
}

/* Perspective-correct attributes given with the
   vertices are kept (perspective_given), and carried into the records
   (jbi_frame_add_poly_persp). */
static void given_attributes(void)
{
    jbi_split_view view = {0, 0, 319, 239, 0, 0};
    jbi_split_vertex t[3];
    collected c;
    jb_context *jb;
    jbi_record tmpl;
    int i;

    t[0] = sv(10, 10);
    t[1] = sv(60, 10);
    t[2] = sv(10, 60);
    for (i = 0; i < 3; ++i) {
        t[i].z = 100;
        t[i].u = 10;
        t[i].pu = 77;
    }
    memset(&c, 0, sizeof c);
    c.fx = 10 * 256;
    c.fy = 10 * 256;
    jbi_split_poly(&view, t, 3, collect, &c);
    check("given: not read without the flag", c.fpu, 10);
    view.perspective_given = 1;
    memset(&c, 0, sizeof c);
    c.fx = 10 * 256;
    c.fy = 10 * 256;
    jbi_split_poly(&view, t, 3, collect, &c);
    check("given: kept with the flag", c.fpu, 77);
    /* Through a cut: the edge from (10, -400), given 60, to (20, 400),
       given 100, at equal depths, meets the guard band's top (y -135 for
       a 320 x 240 view: 511 rows centred on it, split.h) at x 13.3125,
       265/800 of the way: given 73.25, so 73, not the affine 10. */
    t[0] = sv(10, -400);
    t[1] = sv(20, 400);
    t[2] = sv(200, 0);
    t[0].pu = 60;
    t[1].pu = 100;
    t[2].pu = 80;
    for (i = 0; i < 3; ++i) {
        t[i].z = 100;
        t[i].u = 10;
    }
    memset(&c, 0, sizeof c);
    c.fx = 3408; /* 13.3125 x 256 */
    c.fy = -135 * 256;
    jbi_split_poly(&view, t, 3, collect, &c);
    check("given: the cut found", c.found, 1);
    check("given: carried through a cut", c.fpu, 73);
    /* Into the records. */
    jb = must_create(NULL);
    memset(&tmpl, 0, sizeof tmpl);
    tmpl.kind = JBI_REC_POLY;
    tmpl.textured = 1;
    tmpl.tex.page_x = 512;
    tmpl.tex.depth = JB_TEX_15BIT;
    t[0] = sv(10, 10);
    t[1] = sv(60, 10);
    t[2] = sv(10, 60);
    for (i = 0; i < 3; ++i) {
        t[i].z = 100;
        t[i].u = 10;
        t[i].pu = 77;
    }
    jb_begin_frame(jb, 0);
    check("given: recorded", jbi_frame_add_poly_persp(jb, "t", JBI_FRAME_LAYER, &tmpl, t, 3),
          JB_OK);
    {
        const jbi_record *rec = &jb->api->frame_ot->rec[0];

        check("given: the record's perspective u", rec->sp.pattr[0][0], 77);
        check("given: the record's affine u", rec->u.poly.v[0].u, 10);
    }
    jb_end_frame(jb);
    jb_destroy(jb);
}

/* Fields a primitive does not use are not used when it is clipped:
   an untextured rectangle's texel coordinates are not stepped, a raw
   polygon's colours not interpolated. */
static void ignored_fields(void)
{
    jb_context *jb = must_create(NULL);
    jb_rect q;
    jb_poly p;
    jb_texture tex;
    const jb_ot *ot;
    int k, i, n, bad = 0;

    memset(&q, 0, sizeof q);
    q.x = -1200;
    q.w = 1300;
    q.h = 5;
    q.u = 123;
    q.v = 45;
    memset(&tex, 0, sizeof tex);
    tex.page_x = 512;
    tex.depth = JB_TEX_15BIT;
    memset(&p, 0, sizeof p);
    p.vertices = 3;
    p.texture = &tex;
    p.raw = 1;
    p.gouraud = 1;
    p.v[0].x = -2000, p.v[0].y = 10;
    p.v[1].x = 2000, p.v[1].y = 10;
    p.v[2].x = 10, p.v[2].y = 200;
    for (i = 0; i < 3; ++i)
        p.v[i].color = 0xabcdefu - (uint32_t)i * 0x111111u;
    jb_begin_frame(jb, 0);
    check("ignored: rect", jb_draw_rect(jb, &q), JB_OK);
    check("ignored: raw poly", jb_draw_poly(jb, &p), JB_OK);
    ot = jb->api->frame_ot;
    n = ot->count;
    check("ignored: records made", n >= 2, 1);
    for (k = 0; k < n; ++k) {
        const jbi_record *r = &ot->rec[k];

        if (r->kind == JBI_REC_RECT)
            bad += r->u.rect.u != 123 || r->u.rect.v != 45;
        if (r->kind == JBI_REC_POLY)
            for (i = 0; i < r->u.poly.vertices; ++i)
                bad += r->u.poly.v[i].color != 0 || r->sp.pattr[i][2] != 0;
    }
    check("ignored: unused fields not used", bad, 0);
    jb_end_frame(jb);
    jb_destroy(jb);
}

/* The splitter's edge rule: an edge within the limits is never cut, whether
   the triangle is split or not; an edge beyond them is cut alike from
   either side. */
typedef struct {
    int on_edge;      /* vertices strictly inside the edge a b */
    int64_t ax, ay, bx, by;
    int64_t seen[64][2];
    int nseen;
} edge_probe;

static void edge_collect(void *user, int poly, const jbi_split_vertex *v, int n,
                         unsigned passes, const jbi_split_area *area)
{
    edge_probe *e = user;
    int i;

    (void)poly;
    (void)passes;
    (void)area;

    for (i = 0; i < n; ++i) {
        int64_t cr = (v[i].qx - e->ax) * (e->by - e->ay) -
                     (v[i].qy - e->ay) * (e->bx - e->ax);
        int inside = cr == 0 && !(v[i].qx == e->ax && v[i].qy == e->ay) &&
                     !(v[i].qx == e->bx && v[i].qy == e->by);
        int64_t lo = e->ax < e->bx ? e->ax : e->bx, hi = e->ax < e->bx ? e->bx : e->ax;

        if (inside && v[i].qx >= lo && v[i].qx <= hi) {
            int k, dup = 0;

            ++e->on_edge;
            for (k = 0; k < e->nseen; ++k)
                dup |= e->seen[k][0] == v[i].qx && e->seen[k][1] == v[i].qy;
            if (!dup && e->nseen < 64) {
                e->seen[e->nseen][0] = v[i].qx;
                e->seen[e->nseen][1] = v[i].qy;
                ++e->nseen;
            }
        }
    }
}

static void edge_rule(void)
{
    jbi_split_view view = {0, 0, 319, 239, 0, 0};
    jbi_split_vertex t[3];
    edge_probe e1, e2;

    /* A (150, -100), B (160, 200) fits; C makes A B C far too tall. */
    t[0] = sv(150, -100);
    t[1] = sv(160, 200);
    t[2] = sv(-300, 700);
    memset(&e1, 0, sizeof e1);
    e1.ax = 150 * 256, e1.ay = -100 * 256, e1.bx = 160 * 256, e1.by = 200 * 256;
    check("edge rule: split", jbi_split_poly(&view, t, 3, edge_collect, &e1) > 1, 1);
    check("edge rule: an edge within the limits is not cut", e1.on_edge, 0);
    /* A (0, -600), B (10, 600) is too tall itself: both sides cut it at
       the same vertices. */
    memset(&e1, 0, sizeof e1);
    memset(&e2, 0, sizeof e2);
    e1.ax = e2.ax = 0, e1.ay = e2.ay = -600 * 256;
    e1.bx = e2.bx = 10 * 256, e1.by = e2.by = 600 * 256;
    t[0] = sv(0, -600);
    t[1] = sv(10, 600);
    t[2] = sv(-200, 0);
    jbi_split_poly(&view, t, 3, edge_collect, &e1);
    t[0] = sv(10, 600);
    t[1] = sv(0, -600);
    t[2] = sv(250, 50);
    jbi_split_poly(&view, t, 3, edge_collect, &e2);
    check("edge rule: the long edge is cut", e1.nseen > 0, 1);
    check("edge rule: cut at as many points from both sides", e1.nseen, e2.nseen);
    {
        int i, k, same = 0;

        for (i = 0; i < e1.nseen; ++i)
            for (k = 0; k < e2.nseen; ++k)
                same += e1.seen[i][0] == e2.seen[k][0] && e1.seen[i][1] == e2.seen[k][1];
        check("edge rule: at the same points", same, e1.nseen);
    }
}

/* ------------------------------------------------------------------------ */
/* Vertices at the range's edges with sub-pixel positions past them, as
   split.h's slack allows (a whole-pixel 1023 whose sub-pixel position is
   up to 255/256 pixel further): a polygon over the frame still covers all
   of it, once. */

static jbi_split_vertex slack_vertex(int x, int y, int dqx, int dqy)
{
    jbi_split_vertex v = sv(x, y);

    v.qx += dqx;
    v.qy += dqy;
    v.r = 64; /* additive red 64: 8 once, 16 twice (255 saturates) */
    return v;
}

/* The polygon v (n corners) additive red over a black w x h frame: how
   many pixels are not exactly once. */
static int slack_frame(const jbi_split_vertex *v, int n, int w, int h, int scale, int *pieces)
{
    jb_config c;
    jb_context *jb;
    jbi_record t;
    uint16_t *vram;
    const uint8_t *f;
    int x, y, bad = 0, fw, fh;

    memset(&c, 0, sizeof c);
    c.width = w;
    c.height = h;
    c.scale = scale;
    jb = must_create(&c);
    jb_set_precise_vertices(jb, 1);
    memset(&t, 0, sizeof t);
    t.kind = JBI_REC_POLY;
    t.u.poly.blend = JB_BLEND_ADD;
    jb_begin_frame(jb, 0);
    check("slack record", jbi_frame_add_poly(jb, "t", JBI_FRAME_LAYER, &t, v, n), JB_OK);
    *pieces = jb->api->frame_ot->count;
    jb_end_frame(jb);
    vram = read_vram(jb);
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x)
            bad += (vram[y * VRAM_W + x] & 31) != 8; /* additive 64: 8 once */
    free(vram);
    if (scale > 1) {
        f = jb_frame(jb, &fw, &fh);
        for (y = 0; y < fh; ++y)
            for (x = 0; x < fw; ++x)
                bad += f[((size_t)y * fw + x) * 4] != widen5(8);
    }
    jb_destroy(jb);
    return bad;
}

static void slack_edges(void)
{
    /* Each corner's sub-pixel offset: past the range's edge where the
       corner is on it (1023 + 255/256, or -1024 - 255/256 is outside the
       slack's meaning only by sign: -1024 with -255). */
    static const int d[4] = {255, -255, 128, -128};
    int k, scale, most = 0;
    char msg[96];

    for (scale = 1; scale <= 2; ++scale) {
        for (k = 0; k < 16; ++k) {
            jbi_split_vertex q[4], t[3];
            int dx = d[k & 3], dy = d[k >> 2], pieces, bad;

            /* A quad over the frame from corners on the range's edges. */
            q[0] = slack_vertex(-1024, -1024, -dx, -dy);
            q[1] = slack_vertex(1023, -1024, dx, -dy);
            q[2] = slack_vertex(1023, 1023, dx, dy);
            q[3] = slack_vertex(-1024, 1023, -dx, dy);
            bad = slack_frame(q, 4, 320, 240, scale, &pieces);
            snprintf(msg, sizeof msg, "slack quad %d at %dx: the frame once", k, scale);
            check(msg, bad, 0);
            most = pieces > most ? pieces : most;
            /* A triangle that needs clipping, one corner at x 1023 past it. */
            t[0] = slack_vertex(1023, -10, dx, 0);
            t[1] = slack_vertex(-3000, -3000, 0, 0);
            t[2] = slack_vertex(-3000, 3000, 0, 0);
            bad = slack_frame(t, 3, 320, 240, scale, &pieces);
            snprintf(msg, sizeof msg, "slack triangle %d at %dx: the frame once", k, scale);
            check(msg, bad, 0);
        }
    }
    (void)most;
}

/* A cut where the ends straddle the plane by their integer
   positions while their sub-pixel ones lie on it (within the slack): the
   integer edge (1022, 0) to (1024, 60) meets x 1023 (the right plane: no
   column of a 1024-wide view is past it) at (1023, 30), halfway, where
   red 10 and 20 give 15; no division by zero, and the sub-pixel triangle,
   inside the plane, is not cut at all. */
static void parallel_cut(void)
{
    jbi_split_view view = {0, 0, 1023, 511, 0, 0};
    jbi_split_vertex t[3];
    collected c;
    int pieces;

    t[0] = sv(1022, 0);
    t[1] = sv(1024, 60);
    t[2] = sv(1000, 100);
    t[0].qx = 1023 * 256;
    t[1].qx = 1023 * 256;
    t[0].r = 10;
    t[1].r = 20;
    t[2].r = 30;
    memset(&c, 0, sizeof c);
    c.fx = 1023 * 256;
    c.fy = 30 * 256;
    pieces = jbi_split_poly(&view, t, 3, collect, &c);
    check("parallel cut: pieces given", pieces > 0 && pieces == c.pieces, 1);
    check("parallel cut: pieces within the limits", c.bad, 0);
    check("parallel cut: the vertex on the plane", c.found, 1);
    check("parallel cut: its attributes, halfway", c.fr, 15);
}

/* A new vertex's attributes are the exact value rounded once,
   halves away from zero. The edge (-1025, 0), red 8, to (-1023, 20), red
   7, meets the range's plane x -1024 at (-1024, 10): red 7.5, so 8 (not 7,
   as rounding the difference, -0.5, would give). */
static void cut_rounding(void)
{
    jbi_split_view view = {-1024, 0, -700, 300, 0, 0};
    jbi_split_vertex t[3];
    collected c;

    t[0] = sv(-1025, 0);
    t[1] = sv(-1023, 20);
    t[2] = sv(-900, 100);
    t[0].r = 8;
    t[1].r = 7;
    t[2].r = 7;
    memset(&c, 0, sizeof c);
    c.fx = -1024 * 256;
    c.fy = 10 * 256;
    jbi_split_poly(&view, t, 3, collect, &c);
    check("cut rounding: the cut found", c.found, 1);
    check("cut rounding: 7.5 rounds to 8", c.fr, 8);
}

/* Scene buckets, layers and capacity. */
static void scene_order(void)
{
    static const struct {
        int where;
        uint32_t color;
        int x;
    } recs[] = {
        {JBI_FRAME_OVERLAY, JB_RGB(255, 0, 0), 0}, /* overlay: drawn last */
        {3, JB_RGB(0, 255, 0), 0},                 /* far */
        {0, JB_RGB(0, 0, 255), 0},                 /* near: over far */
        {JBI_FRAME_BACKGROUND, JB_RGB(255, 255, 0), 0},
        {2, JB_RGB(0, 255, 255), 20}, /* bucket 2: first in */
        {2, JB_RGB(255, 0, 255), 20}, /* then this over it */
        {JBI_FRAME_BACKGROUND, JB_RGB(8, 8, 8), 40},
        {3, JB_RGB(16, 16, 16), 40},
    };
    jb_config c;
    jb_context *jb;
    jbi_record *r;
    uint16_t px;
    int i, fw, fh;
    jb_rect q;

    memset(&c, 0, sizeof c);
    c.ot_size = 4;
    c.max_primitives = 9;
    c.geometry = JB_GEOMETRY_FLOAT;
    jb = must_create(&c);
    check("geometry from the config", jbi_api_geometry(jb), JB_GEOMETRY_FLOAT);
    check("a record outside a frame", jbi_frame_record(jb, "t", 0) == NULL, 1);
    check("not active", jbi_frame_active(jb), 0);
    check("begin", jb_begin_frame(jb, 0), JB_OK);
    check("active", jbi_frame_active(jb), 1);
    jbi_frame_size(jb, &fw, &fh);
    check("frame size", fw * 1000 + fh, 320240);
    check("ot size", jbi_frame_ot_size(jb), 4);
    for (i = 0; i < (int)(sizeof recs / sizeof recs[0]); ++i) {
        r = jbi_frame_record(jb, "t", recs[i].where);
        r->kind = JBI_REC_RECT;
        r->u.rect.x = recs[i].x;
        r->u.rect.w = 10;
        r->u.rect.h = 10;
        r->u.rect.color = recs[i].color;
    }
    check("where 4 out of range", jbi_frame_record(jb, "t", 4) == NULL, 1);
    check("where -4 out of range", jbi_frame_record(jb, "t", -4) == NULL, 1);
    check("ninth record", jbi_frame_record(jb, "t", 1) != NULL, 1);
    check("tenth: full", jbi_frame_record(jb, "t", 1) == NULL, 1);
    /* The first failure was the record outside a frame. */
    check("end: the first error", jb_end_frame(jb), JB_ERROR_INVALID_ARGUMENT);
    jb_vram_read(jb, 5, 5, 1, 1, &px);
    check("overlay on top", px, JB_RGB_TO_15(JB_RGB(255, 0, 0)));
    jb_vram_read(jb, 25, 5, 1, 1, &px);
    check("FIFO in a bucket", px, JB_RGB_TO_15(JB_RGB(255, 0, 255)));
    jb_vram_read(jb, 45, 5, 1, 1, &px);
    check("scene over background", px, JB_RGB_TO_15(JB_RGB(16, 16, 16)));
    /* Now only the frame full. */
    check("begin 2", jb_begin_frame(jb, 0), JB_OK);
    for (i = 0; i < 9; ++i)
        check("fill the frame", jbi_frame_record(jb, "t", JBI_FRAME_LAYER) != NULL, 1);
    memset(&q, 0, sizeof q);
    q.w = q.h = 4;
    check("draw into a full frame", jb_draw_rect(jb, &q), JB_ERROR_OUT_OF_MEMORY);
    check("the detail names the field",
          strstr(jb_error_string(jb), "jb_config.max_primitives") != NULL, 1);
    check("end 2: out of memory", jb_end_frame(jb), JB_ERROR_OUT_OF_MEMORY);
    check("taken", jb_check(jb, NULL), JB_OK);
    jb_destroy(jb);
}

/* Whether a polygon's pieces serve both formats (merged) depended
   on stack bytes an ear left unset. This triangle in a 320 x 240
   view gives 5 pieces, merged, whatever the stack held before. */
static void poison_stack(unsigned char fill)
{
    volatile unsigned char junk[1 << 16];
    size_t i;

    for (i = 0; i < sizeof junk; ++i)
        junk[i] = (unsigned char)(fill + i * 37);
}

static void pieces_whatever_the_stack(void)
{
    jbi_split_view view = {0, 0, 319, 239, 0, 0};
    jbi_split_vertex t[3];
    collected c;
    int k;

    t[0] = sv(0, 10);
    t[1] = sv(674, 211);
    t[2] = sv(-500, 100);
    for (k = 0; k < 3; ++k) {
        poison_stack((unsigned char)(k * 85));
        memset(&c, 0, sizeof c);
        check("pieces whatever the stack: the same pieces whatever the stack held",
              jbi_split_poly(&view, t, 3, collect, &c), 5);
    }
}

int main(void)
{
    static const int scales[3] = {1, 2, 3};
    int s, t, o;

    for (s = 0; s < 3; ++s)
        for (t = 1; t <= 4; t += 3)
            for (o = 0; o < 2; ++o)
                equivalence(scales[s], t, o);
    with_table(1);
    with_table(2);
    subpixel();
    splitter_units();
    huge_polygons();
    gouraud_clip();
    lines_clip();
    rect_clip();
    scene_order();
    persp_cuts();
    degenerate();
    wide_ints();
    persp_flatness();
    given_attributes();
    ignored_fields();
    edge_rule();
    slack_edges();
    parallel_cut();
    cut_rounding();
    pieces_whatever_the_stack();
    return finish("api_records");
}

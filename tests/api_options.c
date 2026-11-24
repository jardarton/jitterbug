/*
 * api_options: precise vertices and perspective-correct textures
 * (jb_config, jb_set_precise_vertices, jb_set_perspective_correct,
 * jb_gte_begin_frame), and the run-time scale and threads (jb_set_scale,
 * jb_set_threads).
 *
 * One small scene, drawn the way a program would: a camera set in the
 * context's GTE, a few triangles and a quad projected with RTPT and RTPS
 * at depths from about 800 to 3000, a screen offset with a fraction, and
 * each drawn with jb_prim_poly as a textured Gouraud polygon (a line too),
 * at its SX, SY. Frame after frame the camera moves by a fraction of a
 * pixel. The same scene is drawn through the internals (the one place
 * this test uses them): a jbi_gte recording into its own jbi_precise
 * table, that table as the context's precise source (positions only with
 * precise vertices; for perspective alone a source that gives depths but
 * no positions), the texture mapping set to perspective for
 * perspective-correct, and the same GP0 words given to jbi_gp0.
 *
 * Checked, at 2x and 4x (and 1x where it matters), with 1 and 4 threads:
 *   - each option, and both, give the frame the internal path gives;
 *   - precise vertices change the frame above 1x (the vertices are at
 *     sub-pixel positions) and nothing at 1x; perspective changes it at
 *     every scale; above 1x neither changes the 1x VRAM;
 *   - with both off the frame is the one a context without them draws, and
 *     the internal path's without a precise source; turned on and off
 *     again, nothing remains of them;
 *   - switched at run time between frames and in the middle of one, the
 *     frames are the internal path's switched at the same points;
 *   - the thread count changes none of it;
 *   - jb_set_scale and jb_set_threads keep the 1x VRAM, and what is drawn
 *     after them is what a context made at that scale, given that VRAM,
 *     draws; their errors.
 */
#include "api_test.h"

#include "core/context.h"
#include "geom/gte.h"
#include "geom/precise.h"

/* Where the scene draws: the public API, or the internals. */
typedef struct target {
    jb_context *pub; /* a jb_create context, or NULL */
    jb_context *ctx; /* else an internal one, with its GTE and table */
    jbi_gte gte;
    jbi_precise *table;
    jbi_precise_source source;
} target;

static int no_positions(void *user)
{
    (void)user;
    return 0;
}

static int depth_lookup(void *user, uint32_t sxy, jbi_precise_vertex *v)
{
    return jbi_precise_lookup(user, sxy, v);
}

static unsigned depth_lookup_poly(void *user, const uint32_t *sxy, int n, jbi_precise_vertex *v)
{
    return jbi_precise_lookup_poly(user, sxy, n, v);
}

/* The options, set as a program sets them, or as the internals need. */
static void options(target *t, int precise, int persp)
{
    if (t->pub != NULL) {
        check("jb_set_precise_vertices", jb_set_precise_vertices(t->pub, precise), JB_OK);
        check("jb_set_perspective_correct", jb_set_perspective_correct(t->pub, persp), JB_OK);
        return;
    }
    jbi_precise_enable(t->table, precise || persp);
    if (precise) {
        t->source = jbi_precise_as_source(t->table);
        jbi_set_precise_source(t->ctx, &t->source);
    } else if (persp) {
        t->source.enabled = no_positions;
        t->source.lookup = depth_lookup;
        t->source.lookup_poly = depth_lookup_poly;
        t->source.user = t->table;
        jbi_set_precise_source(t->ctx, &t->source);
    } else {
        jbi_set_precise_source(t->ctx, NULL);
    }
    jbi_set_texture_mapping(t->ctx, persp);
}

static void gte_w(target *t, int reg, uint32_t v)
{
    if (t->pub != NULL)
        check("jb_gte_write", jb_gte_write(t->pub, reg, v), JB_OK);
    else if (reg < 32)
        jbi_gte_write_data(&t->gte, reg, v);
    else
        jbi_gte_write_ctrl(&t->gte, reg - 32, v);
}

static uint32_t gte_r(target *t, int reg)
{
    if (t->pub != NULL)
        return jb_gte_read(t->pub, reg);
    return reg < 32 ? jbi_gte_read_data(&t->gte, reg) : jbi_gte_read_ctrl(&t->gte, reg - 32);
}

static void gte_cmd(target *t, uint32_t cmd)
{
    if (t->pub != NULL)
        check("jb_gte_command", jb_gte_command(t->pub, cmd), JB_OK);
    else
        jbi_gte_command(&t->gte, cmd);
}

static void words(target *t, const uint32_t *w, int n)
{
    if (t->pub != NULL)
        gp0(t->pub, w, (size_t)n);
    else
        jbi_gp0(t->ctx, w, n);
}

static void begin_frame(target *t)
{
    if (t->pub != NULL)
        check("jb_gte_begin_frame", jb_gte_begin_frame(t->pub), JB_OK);
    else
        jbi_precise_begin_frame(t->table);
}

/* The texture: a 64 x 64 15-bit pattern at (512, 0), and the area. */
static void setup(target *t)
{
    static uint16_t tex[64 * 64];
    uint32_t area[2] = {0xe3000000u, 0xe4000000u | 319u | 239u << 10};
    int i;

    for (i = 0; i < 64 * 64; ++i) {
        int x = i % 64, y = i / 64;
        tex[i] = (uint16_t)(((x / 8 + y / 8) & 1) ? 0x7fff : (x >> 1) | (y >> 1) << 5 | 0x2000);
    }
    words(t, area, 2);
    if (t->pub != NULL) {
        check("jb_vram_load", jb_vram_load(t->pub, 512, 0, 64, 64, tex), JB_OK);
        gp1(t->pub, 0x03000000u);
    } else {
        jbi_load(t->ctx, 512, 0, 64, 64, tex);
    }
    /* The camera: identity rotation, H 400, the screen centre with a
       fraction. */
    gte_w(t, JB_GTE_RT11RT12, JB_GTE_PAIR(4096, 0));
    gte_w(t, JB_GTE_RT13RT21, 0);
    gte_w(t, JB_GTE_RT22RT23, JB_GTE_PAIR(4096, 0));
    gte_w(t, JB_GTE_RT31RT32, 0);
    gte_w(t, JB_GTE_RT33, 4096);
    gte_w(t, JB_GTE_H, 400);
    gte_w(t, JB_GTE_OFX, (160u << 16) | 0x4000u);
    gte_w(t, JB_GTE_OFY, (120u << 16) | 0xc000u);
    gte_w(t, JB_GTE_TRY, 0);
    gte_w(t, JB_GTE_TRZ, 1500);
}

static const int model[4][4][3] = {
    {{-300, -200, -500}, {300, -250, 800}, {-100, 250, 0}, {0, 0, 0}},
    {{100, 100, -600}, {500, 0, 1200}, {350, 300, 200}, {0, 0, 0}},
    {{-500, 100, 300}, {-200, -50, -400}, {-450, 350, -200}, {0, 0, 0}},
    {{-200, -400, 1000}, {200, -400, 1000}, {-200, -150, -300}, {200, -150, -300}},
};

static void draw_poly(target *t, const uint32_t *sxy, int n)
{
    static const int uv[4][2] = {{0, 0}, {63, 0}, {0, 63}, {63, 63}};
    static const uint32_t color[4] = {0x808080u, 0x60a0ffu, 0xff8060u, 0x80ff80u};
    int i;

    if (t->pub != NULL) {
        jb_texture tex = {.page_x = 512, .depth = JB_TEX_15BIT};
        jb_poly p;

        memset(&p, 0, sizeof p);
        p.vertices = n;
        p.gouraud = 1;
        p.texture = &tex;
        for (i = 0; i < n; ++i) {
            p.v[i].x = JB_GTE_SX(sxy[i]);
            p.v[i].y = JB_GTE_SY(sxy[i]);
            p.v[i].color = color[i];
            p.v[i].u = uv[i][0];
            p.v[i].v = uv[i][1];
        }
        check("jb_prim_poly", jb_prim_poly(t->pub, &p), JB_OK);
    } else {
        uint32_t w[12];
        int k = 0;

        for (i = 0; i < n; ++i) {
            uint32_t attr = i == 1 ? 8u | 2u << 7 : 0; /* page (512, 0), 15-bit */

            w[k++] = i == 0 ? (n == 4 ? 0x3cu : 0x34u) << 24 | color[0] : color[i];
            w[k++] = sxy[i];
            w[k++] = (uint32_t)uv[i][0] | (uint32_t)uv[i][1] << 8 | attr << 16;
        }
        words(t, w, k);
    }
}

static void draw_line(target *t, uint32_t a, uint32_t b)
{
    if (t->pub != NULL) {
        jb_line l;

        memset(&l, 0, sizeof l);
        l.v[0].x = JB_GTE_SX(a), l.v[0].y = JB_GTE_SY(a), l.v[0].color = 0xffff00u;
        l.v[1].x = JB_GTE_SX(b), l.v[1].y = JB_GTE_SY(b);
        check("jb_prim_line", jb_prim_line(t->pub, &l), JB_OK);
    } else {
        uint32_t w[3] = {0x40ffff00u, a, b};
        words(t, w, 3);
    }
}

/* One frame of the scene, the camera moved by f units (a fraction of a
   pixel at these depths). switch_at: before that polygon the options
   become (precise2, persp2); -1 never. */
static void frame(target *t, int f, int switch_at, int precise2, int persp2)
{
    int k, i;

    begin_frame(t);
    if (t->pub != NULL)
        check("jb_vram_fill", jb_vram_fill(t->pub, 0, 0, 320, 240, 0x302010u), JB_OK);
    else
        jbi_fill(t->ctx, 0, 0, 320, 240, 0x302010u);
    gte_w(t, JB_GTE_TRX, (uint32_t)(f * 3));
    for (k = 0; k < 4; ++k) {
        uint32_t sxy[4];

        if (k == switch_at)
            options(t, precise2, persp2);
        for (i = 0; i < 3; ++i) {
            gte_w(t, JB_GTE_VXY0 + 2 * i, JB_GTE_PAIR(model[k][i][0], model[k][i][1]));
            gte_w(t, JB_GTE_VZ0 + 2 * i, (uint32_t)model[k][i][2]);
        }
        gte_cmd(t, JB_GTE_RTPT);
        for (i = 0; i < 3; ++i)
            sxy[i] = gte_r(t, JB_GTE_SXY0 + i);
        if (k == 3) {
            gte_w(t, JB_GTE_VXY0, JB_GTE_PAIR(model[k][3][0], model[k][3][1]));
            gte_w(t, JB_GTE_VZ0, (uint32_t)model[k][3][2]);
            gte_cmd(t, JB_GTE_RTPS);
            sxy[3] = gte_r(t, JB_GTE_SXY2);
        }
        draw_poly(t, sxy, k == 3 ? 4 : 3);
        if (k == 0)
            draw_line(t, sxy[0], sxy[1]);
    }
}

static void open_public(target *t, int scale, int threads, int precise, int persp)
{
    jb_config c;

    memset(t, 0, sizeof *t);
    memset(&c, 0, sizeof c);
    c.scale = scale;
    c.threads = threads;
    c.precise_vertices = precise;
    c.perspective_correct = persp;
    t->pub = must_create(&c);
    setup(t);
}

static void open_internal(target *t, int scale, int threads, int precise, int persp)
{
    jbi_config c;

    memset(t, 0, sizeof *t);
    jbi_config_default(&c);
    c.scale = scale;
    c.threads = threads;
    c.exact_1x = 1; /* as jb_create */
    t->ctx = jbi_context_create(&c);
    check("internal context", t->ctx != NULL, 1);
    jbi_reset_state(t->ctx);
    t->table = jbi_precise_create();
    jbi_gte_init(&t->gte);
    jbi_gte_set_precise(&t->gte, t->table);
    options(t, precise, persp);
    setup(t);
}

static void close_target(target *t)
{
    if (t->pub != NULL) {
        jb_destroy(t->pub);
    } else {
        jbi_context_destroy(t->ctx);
        jbi_precise_destroy(t->table);
    }
}

/* The frame (display area 320 x 240 at (0, 0)) as RGBA; free it. */
static uint8_t *grab(target *t, int *w, int *h)
{
    uint8_t *rgba;

    if (t->pub != NULL) {
        const uint8_t *f = jb_frame(t->pub, w, h);

        check("jb_frame", f != NULL, 1);
        rgba = must_alloc((size_t)*w * *h * 4);
        memcpy(rgba, f, (size_t)*w * *h * 4);
    } else {
        uint8_t *buf = must_alloc(jbi_scanout_bytes(t->ctx));
        int stride;
        size_t i;

        jbi_scanout(t->ctx, 0, 0, 320, 240, 0, 1, buf, w, h, &stride);
        rgba = must_alloc((size_t)*w * *h * 4);
        for (i = 0; i < (size_t)*w * *h; ++i) {
            unsigned p = buf[2 * i] | (unsigned)buf[2 * i + 1] << 8;
            rgba[4 * i] = (uint8_t)widen5(p & 31);
            rgba[4 * i + 1] = (uint8_t)widen5((p >> 5) & 31);
            rgba[4 * i + 2] = (uint8_t)widen5((p >> 10) & 31);
            rgba[4 * i + 3] = 255;
        }
        free(buf);
    }
    return rgba;
}

static uint16_t *grab_1x(target *t)
{
    uint16_t *v = must_alloc((size_t)VRAM_PIXELS * 2);

    if (t->pub != NULL)
        check("jb_vram_read", jb_vram_read(t->pub, 0, 0, VRAM_W, VRAM_H, v), JB_OK);
    else
        jbi_store_1x(t->ctx, 0, 0, 0, 0, v);
    return v;
}

static int same_frames(const uint8_t *a, const uint8_t *b, int scale)
{
    return memcmp(a, b, (size_t)320 * scale * 240 * scale * 4) == 0;
}

enum { FRAMES = 3 };

/* The scene's last frame, and its 1x VRAM, with the options from the
   start; public or internal. */
static uint8_t *scene(int pub, int scale, int threads, int precise, int persp, uint16_t **vram)
{
    target t;
    uint8_t *img;
    int w, h, f;

    if (pub)
        open_public(&t, scale, threads, precise, persp);
    else
        open_internal(&t, scale, threads, precise, persp);
    for (f = 0; f < FRAMES; ++f)
        frame(&t, f, -1, 0, 0);
    img = grab(&t, &w, &h);
    check("frame size", w * 10000 + h, 320 * scale * 10000 + 240 * scale);
    if (vram != NULL)
        *vram = grab_1x(&t);
    close_target(&t);
    return img;
}

static void options_at(int scale, int threads)
{
    uint16_t *v_off, *v_on;
    uint8_t *off = scene(1, scale, threads, 0, 0, &v_off);
    uint8_t *off_i = scene(0, scale, threads, 0, 0, NULL);
    char what[96];
    int p;

    snprintf(what, sizeof what, "%dx %dt: both off = internal, no precise source", scale, threads);
    check(what, same_frames(off, off_i, scale), 1);
    for (p = 1; p < 4; ++p) {
        int precise = p & 1, persp = (p >> 1) & 1;
        uint8_t *on = scene(1, scale, threads, precise, persp, &v_on);
        uint8_t *on_i = scene(0, scale, threads, precise, persp, NULL);
        int changes = scale > 1 || persp;

        snprintf(what, sizeof what, "%dx %dt precise %d persp %d: = internal", scale, threads,
                 precise, persp);
        check(what, same_frames(on, on_i, scale), 1);
        snprintf(what, sizeof what, "%dx %dt precise %d persp %d: %s the frame", scale, threads,
                 precise, persp, changes ? "changes" : "does not change");
        check(what, !same_frames(on, off, scale), changes);
        if (scale > 1) {
            snprintf(what, sizeof what, "%dx %dt precise %d persp %d: 1x VRAM unchanged", scale,
                     threads, precise, persp);
            check(what, memcmp(v_on, v_off, (size_t)VRAM_PIXELS * 2) == 0, 1);
        }
        free(on);
        free(on_i);
        free(v_on);
    }
    free(off);
    free(off_i);
    free(v_off);
}

/* Precise vertices alone, compared across thread counts. */
static void threads_agree(int scale)
{
    int p;

    for (p = 0; p < 4; ++p) {
        uint8_t *one = scene(1, scale, 1, p & 1, p >> 1, NULL);
        uint8_t *four = scene(1, scale, 4, p & 1, p >> 1, NULL);
        char what[64];

        snprintf(what, sizeof what, "%dx options %d: 1 thread = 4 threads", scale, p);
        check(what, same_frames(one, four, scale), 1);
        free(one);
        free(four);
    }
}

/* Options switched between frames and inside one, public against
   internal, frame by frame; then everything off again. */
static void switching(int scale, int threads)
{
    static const int plan[][4] = {
        /* switch before polygon, precise, persp; then from the next frame */
        {-1, 0, 0, 0}, {0, 1, 0, 0}, {2, 0, 1, 0}, {1, 1, 1, 0}, {3, 0, 0, 0}, {-1, 0, 0, 0},
    };
    target a, b;
    int f;

    open_public(&a, scale, threads, 0, 0);
    open_internal(&b, scale, threads, 0, 0);
    for (f = 0; f < (int)(sizeof plan / sizeof plan[0]); ++f) {
        uint8_t *fa, *fb;
        int w, h;
        char what[64];

        frame(&a, f, plan[f][0], plan[f][1], plan[f][2]);
        frame(&b, f, plan[f][0], plan[f][1], plan[f][2]);
        fa = grab(&a, &w, &h);
        fb = grab(&b, &w, &h);
        snprintf(what, sizeof what, "%dx %dt switching, frame %d", scale, threads, f);
        check(what, same_frames(fa, fb, scale), 1);
        free(fa);
        free(fb);
    }
    close_target(&a);
    close_target(&b);

    /* On and off again: nothing of them remains. */
    {
        uint8_t *never = scene(1, scale, threads, 0, 0, NULL), *img;
        int w, h;

        open_public(&a, scale, threads, 1, 1);
        frame(&a, 0, -1, 0, 0);
        options(&a, 0, 0);
        for (f = 0; f < FRAMES; ++f)
            frame(&a, f, -1, 0, 0);
        img = grab(&a, &w, &h);
        check("on, then off: as never on", same_frames(img, never, scale), 1);
        free(img);
        free(never);
        close_target(&a);
    }
}

/* ------------------------------------------------------------------------ */
/* jb_set_scale and jb_set_threads */

static void reconfigure(void)
{
    static const int scales[] = {2, 4, 1, 3, 3, 0, 2};
    jb_context *jb = must_create(NULL);
    uint16_t *before, *after;
    gen noise = {4242, 0, 0};
    target t;
    size_t i;
    int k, w, h;

    memset(&t, 0, sizeof t);
    t.pub = jb;
    {
        uint16_t *pix = must_alloc((size_t)VRAM_PIXELS * 2);

        for (i = 0; i < (size_t)VRAM_PIXELS; ++i)
            pix[i] = (uint16_t)rnd(&noise);
        jb_vram_load(jb, 0, 0, VRAM_W, VRAM_H, pix);
        free(pix);
    }
    setup(&t);
    frame(&t, 0, -1, 0, 0);
    for (k = 0; k < (int)(sizeof scales / sizeof scales[0]); ++k) {
        int s = scales[k] > 1 ? scales[k] : 1;
        char what[64];
        jb_config c;
        target fresh;
        uint8_t *fa, *fb;

        before = grab_1x(&t);
        snprintf(what, sizeof what, "jb_set_scale %d", scales[k]);
        check(what, jb_set_scale(jb, scales[k]), JB_OK);
        if (k == 4)
            check("jb_set_threads 4", jb_set_threads(jb, 4), JB_OK);
        if (k == 6)
            check("jb_set_threads 0", jb_set_threads(jb, 0), JB_OK);
        after = grab_1x(&t);
        snprintf(what, sizeof what, "scale %d: 1x VRAM kept", scales[k]);
        check(what, memcmp(before, after, (size_t)VRAM_PIXELS * 2) == 0, 1);

        /* A fresh context at that scale, given this VRAM. */
        memset(&c, 0, sizeof c);
        c.scale = s;
        memset(&fresh, 0, sizeof fresh);
        fresh.pub = must_create(&c);
        jb_vram_load(fresh.pub, 0, 0, VRAM_W, VRAM_H, after);
        setup(&fresh); /* the camera; frame sets every other input */
        frame(&t, k + 1, -1, 0, 0);
        frame(&fresh, k + 1, -1, 0, 0);
        fa = grab(&t, &w, &h);
        snprintf(what, sizeof what, "scale %d: frame size", scales[k]);
        check(what, w * 10000 + h, 320 * s * 10000 + 240 * s);
        fb = grab(&fresh, &w, &h);
        snprintf(what, sizeof what, "scale %d: drawn as a fresh context draws", scales[k]);
        check(what, same_frames(fa, fb, s), 1);
        free(fa);
        free(fb);
        free(before);
        free(after);
        jb_destroy(fresh.pub);
    }
    check("jb_set_scale -1", jb_set_scale(jb, -1), JB_ERROR_INVALID_ARGUMENT);
    check_str("jb_set_scale -1: error string", jb_error_string(jb),
              "jb_set_scale: scale -1 is negative");
    check("jb_set_scale 9", jb_set_scale(jb, 9), JB_ERROR_UNSUPPORTED);
    check_str("jb_set_scale 9: error string", jb_error_string(jb),
              "jb_set_scale: scale 9: the software backend draws at 1x to 8x");
    check("jb_set_threads -1", jb_set_threads(jb, -1), JB_ERROR_INVALID_ARGUMENT);
    check("jb_set_threads 65", jb_set_threads(jb, 65), JB_ERROR_INVALID_ARGUMENT);
    check_str("jb_set_threads 65: error string", jb_error_string(jb),
              "jb_set_threads: threads 65 is outside 0..64");
    check("jb_set_scale NULL", jb_set_scale(NULL, 2), JB_ERROR_INVALID_ARGUMENT);
    check("jb_set_threads NULL", jb_set_threads(NULL, 2), JB_ERROR_INVALID_ARGUMENT);
    check("jb_set_precise_vertices NULL", jb_set_precise_vertices(NULL, 1),
          JB_ERROR_INVALID_ARGUMENT);
    check("jb_set_perspective_correct NULL", jb_set_perspective_correct(NULL, 1),
          JB_ERROR_INVALID_ARGUMENT);
    jb_destroy(jb);
}

/* An allocator whose n-th allocation from now fails (0: none). */
typedef struct {
    long count, fail_at;
} Failing;

static void *failing_alloc(void *user, size_t size)
{
    Failing *f = user;

    if (f->fail_at > 0 && ++f->count == f->fail_at)
        return NULL;
    return malloc(size);
}

static void failing_free(void *user, void *ptr)
{
    (void)user;
    free(ptr);
}

/* A change of scale starts no threads: after a request for two that got
   one (no memory for the pool), it goes on with one, whether the new scale
   can be had or not. In a single-threaded build one is all there ever is. */
static void scale_starts_no_threads(void)
{
    Failing f = {0, 0};
    jb_config cfg;
    jb_context *jb;

    memset(&cfg, 0, sizeof cfg);
    cfg.scale = 2;
    cfg.threads = 1;
    cfg.alloc = failing_alloc;
    cfg.free = failing_free;
    cfg.alloc_user = &f;
    jb = must_create(&cfg);
    f.count = 0, f.fail_at = 1; /* the pool */
    check("jb_set_threads 2 without memory for the pool", jb_set_threads(jb, 2), JB_OK);
    f.fail_at = 0;
    check("one thread in effect", jbi_threads(jb), 1);
    f.count = 0, f.fail_at = 1; /* the 3x scaled VRAM */
    check("jb_set_scale 3 without memory", jb_set_scale(jb, 3), JB_ERROR_OUT_OF_MEMORY);
    f.fail_at = 0;
    check("still one thread after the failed scale", jbi_threads(jb), 1);
    check("still 2x", jbi_scale(jb), 2);
    check("jb_set_scale 3", jb_set_scale(jb, 3), JB_OK);
    check("still one thread after the scale", jbi_threads(jb), 1);
    jb_destroy(jb);
}

int main(void)
{
    options_at(1, 1);
    options_at(2, 1);
    options_at(2, 4);
    options_at(4, 1);
    options_at(4, 4);
    threads_agree(2);
    threads_agree(4);
    switching(2, 1);
    switching(4, 4);
    switching(1, 1);
    reconfigure();
    scale_starts_no_threads();
    return finish("api_options");
}

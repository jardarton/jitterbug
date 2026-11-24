/*
 * Errors and lifecycle of the public API: every configuration field out of
 * range, NULL arguments to every function, the result and error strings,
 * the log callback, the allocator callbacks (every allocation counted and
 * freed; each allocation failing in turn during jb_create and jb_frame),
 * and many contexts created, used and destroyed (LeakSanitizer checks at
 * exit, in the sanitizer build, that nothing leaked).
 */
#include "api_test.h"

/* ------------------------------------------------------------------------ */
/* The log callback */

static struct {
    int calls;
    char last[256];
} logged;

static void log_fn(void *user, const char *message)
{
    ++logged.calls;
    check("log user pointer", user == &logged, 1);
    snprintf(logged.last, sizeof logged.last, "%s", message);
}

/* ------------------------------------------------------------------------ */
/* A counting allocator that can fail */

typedef struct {
    long live, total, fail_at; /* fail_at: the n-th allocation fails (1-based), 0 none */
    size_t live_bytes;
} Counter;

typedef struct {
    size_t size;
    double align; /* keeps what follows aligned for any object */
} Header;

static void *count_alloc(void *user, size_t size)
{
    Counter *c = user;
    Header *h;

    check("alloc of 0 bytes", size == 0, 0);
    if (c->fail_at > 0 && c->total + 1 == c->fail_at) {
        ++c->total;
        return NULL;
    }
    h = malloc(sizeof(Header) + size);
    if (h == NULL)
        return NULL;
    ++c->total;
    ++c->live;
    c->live_bytes += size;
    h->size = size;
    return h + 1;
}

static void count_free(void *user, void *ptr)
{
    Counter *c = user;
    Header *h = (Header *)ptr - 1;

    check("free of NULL", ptr == NULL, 0);
    --c->live;
    c->live_bytes -= h->size;
    free(h);
}

static jb_config counted(Counter *c, int scale, int threads)
{
    jb_config cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.scale = scale;
    cfg.threads = threads;
    cfg.alloc = count_alloc;
    cfg.free = count_free;
    cfg.alloc_user = c;
    return cfg;
}

/* ------------------------------------------------------------------------ */

static void bad_config(const char *what, jb_config cfg, jb_result want, const char *in_message)
{
    jb_result r = JB_OK;
    jb_context *jb;
    char msg[160];

    cfg.log = log_fn;
    cfg.log_user = &logged;
    logged.calls = 0;
    logged.last[0] = 0;
    jb = jb_create(&cfg, &r);
    snprintf(msg, sizeof msg, "%s: context", what);
    check(msg, jb != NULL, 0);
    snprintf(msg, sizeof msg, "%s: result", what);
    check(msg, r, want);
    snprintf(msg, sizeof msg, "%s: logged once", what);
    check(msg, logged.calls, 1);
    snprintf(msg, sizeof msg, "%s: log says \"%s\" (got \"%s\")", what, in_message, logged.last);
    check(msg, strstr(logged.last, in_message) != NULL, 1);
    /* Without a result pointer or a log it still fails quietly. */
    cfg.log = NULL;
    snprintf(msg, sizeof msg, "%s: context without result pointer", what);
    check(msg, jb_create(&cfg, NULL) != NULL, 0);
    jb_destroy(jb);
}

static jb_config config0(void)
{
    jb_config c;
    memset(&c, 0, sizeof c);
    return c;
}

static void configs(void)
{
    jb_config c;

    c = config0(), c.scale = -1;
    bad_config("scale -1", c, JB_ERROR_INVALID_ARGUMENT, "scale -1");
    c = config0(), c.scale = 9;
    bad_config("scale 9", c, JB_ERROR_UNSUPPORTED, "scale 9");
    c = config0(), c.scale = 1000;
    bad_config("scale 1000", c, JB_ERROR_UNSUPPORTED, "scale 1000");
    c = config0(), c.width = -1;
    bad_config("width -1", c, JB_ERROR_INVALID_ARGUMENT, "display size");
    c = config0(), c.width = 1025;
    bad_config("width 1025", c, JB_ERROR_INVALID_ARGUMENT, "display size");
    c = config0(), c.height = -5;
    bad_config("height -5", c, JB_ERROR_INVALID_ARGUMENT, "display size");
    c = config0(), c.height = 513;
    bad_config("height 513", c, JB_ERROR_INVALID_ARGUMENT, "display size");
    c = config0(), c.threads = -1;
    bad_config("threads -1", c, JB_ERROR_INVALID_ARGUMENT, "threads");
    c = config0(), c.threads = 65;
    bad_config("threads 65", c, JB_ERROR_INVALID_ARGUMENT, "threads");
    c = config0(), c.alloc = count_alloc;
    bad_config("alloc without free", c, JB_ERROR_INVALID_ARGUMENT, "alloc and free");
    c = config0(), c.free = count_free;
    bad_config("free without alloc", c, JB_ERROR_INVALID_ARGUMENT, "alloc and free");
}

/* The edges of every range work, and 0 is the default. */
static void good_configs(void)
{
    static const struct {
        int scale, width, height, threads, want_w, want_h;
    } ok[] = {
        {0, 0, 0, 0, 320, 240},     {1, 0, 0, 1, 320, 240},  {2, 1, 1, 2, 2, 2},
        {8, 16, 8, 3, 128, 64},     {1, 1024, 512, 4, 1024, 512}, {3, 427, 240, 0, 1281, 720},
        {1, 0, 100, 64, 320, 100},  {1, 200, 0, 1, 200, 240},
    };
    unsigned k;

    for (k = 0; k < sizeof ok / sizeof ok[0]; ++k) {
        jb_config c = config0();
        jb_result r = JB_ERROR_UNSUPPORTED;
        jb_context *jb;
        int w = -1, h = -1;
        char msg[96];

        c.scale = ok[k].scale;
        c.width = ok[k].width;
        c.height = ok[k].height;
        c.threads = ok[k].threads;
        jb = jb_create(&c, &r);
        snprintf(msg, sizeof msg, "good config %u: result", k);
        check(msg, r, JB_OK);
        if (jb == NULL)
            continue;
        snprintf(msg, sizeof msg, "good config %u: no error yet", k);
        check_str(msg, jb_error_string(jb), "");
        snprintf(msg, sizeof msg, "good config %u: frame", k);
        check(msg, jb_frame(jb, &w, &h) != NULL, 1);
        snprintf(msg, sizeof msg, "good config %u: frame size", k);
        check(msg, w * 10000 + h, ok[k].want_w * 10000 + ok[k].want_h);
        jb_destroy(jb);
    }
    /* No config at all. */
    {
        jb_context *jb = jb_create(NULL, NULL);
        int w, h;
        check("jb_create(NULL, NULL)", jb != NULL, 1);
        check("default frame", jb_frame(jb, &w, &h) != NULL && w == 320 && h == 240, 1);
        jb_destroy(jb);
    }
}

static void strings(void)
{
    check_str("JB_OK", jb_result_string(JB_OK), "ok");
    check_str("JB_ERROR_INVALID_ARGUMENT", jb_result_string(JB_ERROR_INVALID_ARGUMENT),
              "invalid argument");
    check_str("JB_ERROR_OUT_OF_MEMORY", jb_result_string(JB_ERROR_OUT_OF_MEMORY),
              "out of memory");
    check_str("JB_ERROR_UNSUPPORTED", jb_result_string(JB_ERROR_UNSUPPORTED), "unsupported");
    check_str("an unknown result", jb_result_string((jb_result)99), "unknown result");
    check_str("an unknown negative result", jb_result_string((jb_result)-1), "unknown result");
    check_str("jb_error_string(NULL)", jb_error_string(NULL), "no context");
}

static void null_arguments(void)
{
    jb_config c = config0();
    jb_context *jb;
    const uint32_t nop = 0;
    int w = 7, h = 7;

    check("jb_gp0(NULL)", jb_gp0(NULL, &nop, 1), JB_ERROR_INVALID_ARGUMENT);
    check("jb_gp0(NULL, NULL, 0)", jb_gp0(NULL, NULL, 0), JB_ERROR_INVALID_ARGUMENT);
    check("jb_gp1(NULL)", jb_gp1(NULL, 0), JB_ERROR_INVALID_ARGUMENT);
    check("jb_gpuread(NULL)", (long long)jb_gpuread(NULL), 0);
    check("jb_gpustat(NULL)", (long long)jb_gpustat(NULL), 0);
    check("jb_frame(NULL)", jb_frame(NULL, &w, &h) != NULL, 0);
    check("jb_frame(NULL) size", w * 100 + h, 0);
    check("jb_frame(NULL, NULL, NULL)", jb_frame(NULL, NULL, NULL) != NULL, 0);
    jb_destroy(NULL);

    c.log = log_fn;
    c.log_user = &logged;
    jb = must_create(&c);
    logged.calls = 0;
    check("jb_gp0 words NULL, count 3", jb_gp0(jb, NULL, 3), JB_ERROR_INVALID_ARGUMENT);
    check_str("its error string", jb_error_string(jb), "jb_gp0: words is NULL but count is 3");
    check("logged", logged.calls, 1);
    check_str("the log message", logged.last, "jb_gp0: words is NULL but count is 3");
    check("jb_gp0 words NULL, count 0", jb_gp0(jb, NULL, 0), JB_OK);
    check("jb_gp0 one word", jb_gp0(jb, &nop, 1), JB_OK);
    check_str("a success leaves the error string", jb_error_string(jb),
              "jb_gp0: words is NULL but count is 3");
    check("jb_frame without size pointers", jb_frame(jb, NULL, NULL) != NULL, 1);
    check("logged nothing more", logged.calls, 1);
    jb_destroy(jb);
}

/* The calls over the console's commands: jb NULL, NULL pointers, and the
   text of a few range errors (each call's ranges are tested with it, in
   api_prim, api_vram, api_gte and api_options). */
static void null_arguments_calls(void)
{
    jb_context *jb = must_create(NULL);
    uint16_t px[4] = {0, 0, 0, 0};
    jb_poly poly;
    jb_line line;
    jb_polyline pl;
    jb_rect rect;
    jb_vertex v[2];

    memset(&poly, 0, sizeof poly);
    memset(&line, 0, sizeof line);
    memset(&rect, 0, sizeof rect);
    memset(v, 0, sizeof v);
    poly.vertices = 3;
    pl.gouraud = 0, pl.blend = 0, pl.count = 2, pl.v = v;

    check("jb_vram_load(NULL)", jb_vram_load(NULL, 0, 0, 1, 1, px), JB_ERROR_INVALID_ARGUMENT);
    check("jb_vram_read(NULL)", jb_vram_read(NULL, 0, 0, 1, 1, px), JB_ERROR_INVALID_ARGUMENT);
    check("jb_vram_fill(NULL)", jb_vram_fill(NULL, 0, 0, 1, 1, 0), JB_ERROR_INVALID_ARGUMENT);
    check("jb_vram_copy(NULL)", jb_vram_copy(NULL, 0, 0, 0, 0, 1, 1), JB_ERROR_INVALID_ARGUMENT);
    check("jb_prim_poly(NULL)", jb_prim_poly(NULL, &poly), JB_ERROR_INVALID_ARGUMENT);
    check("jb_prim_line(NULL)", jb_prim_line(NULL, &line), JB_ERROR_INVALID_ARGUMENT);
    check("jb_prim_polyline(NULL)", jb_prim_polyline(NULL, &pl), JB_ERROR_INVALID_ARGUMENT);
    check("jb_prim_rect(NULL)", jb_prim_rect(NULL, &rect), JB_ERROR_INVALID_ARGUMENT);
    check("jb_set_draw_area(NULL)", jb_set_draw_area(NULL, 0, 0, 1, 1), JB_ERROR_INVALID_ARGUMENT);
    check("jb_set_draw_offset(NULL)", jb_set_draw_offset(NULL, 0, 0), JB_ERROR_INVALID_ARGUMENT);
    check("jb_set_texture_window(NULL)", jb_set_texture_window(NULL, 0, 0, 0, 0),
          JB_ERROR_INVALID_ARGUMENT);
    check("jb_set_mask(NULL)", jb_set_mask(NULL, 0, 0), JB_ERROR_INVALID_ARGUMENT);
    check("jb_set_dither(NULL)", jb_set_dither(NULL, 0), JB_ERROR_INVALID_ARGUMENT);
    check("jb_gte_write(NULL)", jb_gte_write(NULL, 0, 0), JB_ERROR_INVALID_ARGUMENT);
    check("jb_gte_read(NULL)", (long long)jb_gte_read(NULL, 0), 0);
    check("jb_gte_command(NULL)", jb_gte_command(NULL, JB_GTE_RTPS), JB_ERROR_INVALID_ARGUMENT);
    check("jb_gte_begin_frame(NULL)", jb_gte_begin_frame(NULL), JB_ERROR_INVALID_ARGUMENT);
    check("jb_set_scale(NULL)", jb_set_scale(NULL, 1), JB_ERROR_INVALID_ARGUMENT);
    check("jb_set_threads(NULL)", jb_set_threads(NULL, 1), JB_ERROR_INVALID_ARGUMENT);
    check("jb_set_precise_vertices(NULL)", jb_set_precise_vertices(NULL, 1),
          JB_ERROR_INVALID_ARGUMENT);
    check("jb_set_perspective_correct(NULL)", jb_set_perspective_correct(NULL, 1),
          JB_ERROR_INVALID_ARGUMENT);

    check("jb_prim_poly(jb, NULL)", jb_prim_poly(jb, NULL), JB_ERROR_INVALID_ARGUMENT);
    check_str("its error string", jb_error_string(jb), "jb_prim_poly: poly is NULL");
    check("jb_prim_line(jb, NULL)", jb_prim_line(jb, NULL), JB_ERROR_INVALID_ARGUMENT);
    check_str("its error string", jb_error_string(jb), "jb_prim_line: line is NULL");
    check("jb_prim_polyline(jb, NULL)", jb_prim_polyline(jb, NULL), JB_ERROR_INVALID_ARGUMENT);
    check_str("its error string", jb_error_string(jb), "jb_prim_polyline: line is NULL");
    pl.v = NULL;
    check("polyline v NULL", jb_prim_polyline(jb, &pl), JB_ERROR_INVALID_ARGUMENT);
    check_str("its error string", jb_error_string(jb), "jb_prim_polyline: line->v is NULL");
    pl.v = v;
    pl.count = 1;
    check("polyline of 1", jb_prim_polyline(jb, &pl), JB_ERROR_INVALID_ARGUMENT);
    check_str("its error string", jb_error_string(jb), "jb_prim_polyline: count 1 is below 2");
    check("jb_prim_rect(jb, NULL)", jb_prim_rect(jb, NULL), JB_ERROR_INVALID_ARGUMENT);
    check_str("its error string", jb_error_string(jb), "jb_prim_rect: rect is NULL");
    check("jb_vram_read pixels NULL", jb_vram_read(jb, 0, 0, 1, 1, NULL),
          JB_ERROR_INVALID_ARGUMENT);
    check_str("its error string", jb_error_string(jb), "jb_vram_read: pixels is NULL");

    poly.vertices = 5;
    jb_prim_poly(jb, &poly);
    check_str("vertices 5", jb_error_string(jb), "jb_prim_poly: vertices 5 is neither 3 nor 4");
    poly.vertices = 3;
    poly.v[2].x = 1024;
    jb_prim_poly(jb, &poly);
    check_str("x 1024", jb_error_string(jb), "jb_prim_poly: v[2].x 1024 is outside -1024..1023");
    poly.v[2].x = 0;
    poly.v[0].color = 0x1000000u;
    jb_prim_poly(jb, &poly);
    check_str("color", jb_error_string(jb),
              "jb_prim_poly: v[0].color 0x1000000 is above 0xffffff");
    poly.v[0].color = 0;
    {
        jb_texture t = {.page_x = 32, .depth = JB_TEX_4BIT};

        poly.texture = &t;
        jb_prim_poly(jb, &poly);
        check_str("page_x 32", jb_error_string(jb),
                  "jb_prim_poly: texture->page_x 32 is not a multiple of 64");
        t.page_x = 0;
        t.page_y = 100;
        jb_prim_poly(jb, &poly);
        check_str("page_y 100", jb_error_string(jb),
                  "jb_prim_poly: texture->page_y 100 is neither 0 nor 256");
        t.page_y = 0;
        t.depth = 16;
        jb_prim_poly(jb, &poly);
        check_str("depth 16", jb_error_string(jb),
                  "jb_prim_poly: texture->depth 16 is not 4, 8 or 15");
        t.depth = 4;
        poly.v[1].u = 256;
        jb_prim_poly(jb, &poly);
        check_str("u 256", jb_error_string(jb), "jb_prim_poly: v[1].u 256 is outside 0..255");
    }
    rect.w = 1024;
    jb_prim_rect(jb, &rect);
    check_str("rect w", jb_error_string(jb), "jb_prim_rect: w 1024 is outside 0..1023");
    rect.w = 0;
    rect.blend = 5;
    jb_prim_rect(jb, &rect);
    check_str("blend 5", jb_error_string(jb), "jb_prim_rect: blend 5 is outside 0..4");
    jb_set_draw_area(jb, 0, 512, 10, 10);
    check_str("area top", jb_error_string(jb), "jb_set_draw_area: top 512 is outside 0..511");
    jb_set_draw_offset(jb, -1025, 0);
    check_str("offset", jb_error_string(jb), "jb_set_draw_offset: x -1025 is outside -1024..1023");
    jb_set_texture_window(jb, 0, 0, 0, 32);
    check_str("window", jb_error_string(jb),
              "jb_set_texture_window: offset_y 32 is outside 0..31");
    jb_vram_load(jb, 0, 512, 1, 1, px);
    check_str("load position", jb_error_string(jb),
              "jb_vram_load: position (0, 512) is outside 0..1023 x 0..511");
    jb_vram_fill(jb, 0, 0, 1024, 1, 0);
    check_str("fill size", jb_error_string(jb),
              "jb_vram_fill: size 1024 x 1 is outside 0..1023 x 0..511");
    jb_vram_copy(jb, 0, 0, 1024, 0, 1, 1);
    check_str("copy destination", jb_error_string(jb),
              "jb_vram_copy: destination (1024, 0) is outside 0..1023 x 0..511");
    jb_destroy(jb);
}

/* FNV-1a 64 of the frame, and its size. */
static uint64_t frame_hash(jb_context *jb, int *w, int *h)
{
    const uint8_t *f = jb_frame(jb, w, h);
    uint64_t hash = 0xcbf29ce484222325ull;
    size_t i, n;

    if (f == NULL)
        return 0;
    n = (size_t)*w * (size_t)*h * 4;
    for (i = 0; i < n; ++i)
        hash = (hash ^ f[i]) * 0x100000001b3ull;
    return hash;
}

/* Every allocation goes through the callbacks and is freed. */
static void allocator(void)
{
    Counter c;
    jb_config cfg;
    jb_context *jb;
    long after_frame;
    int k, w, h;

    memset(&c, 0, sizeof c);
    cfg = counted(&c, 2, 3);
    jb = must_create(&cfg);
    /* The context, the backend, both VRAMs, the dither table, the queue,
       the worker pool, the API's state and its read buffer. */
    check("allocations at create (at least 9)", c.total >= 9, 1);
    check("the scaled VRAM came from the allocator", c.live_bytes >= 1024u * 512 * 2 * 4, 1);
    gp1(jb, 0x03000000u);
    {
        uint32_t tri[] = {0x30ff0000u, xy(0, 0), 0x0000ff00u, xy(300, 10), 0x000000ffu, xy(20, 200)};
        gp0(jb, tri, 6);
    }
    check("frame", jb_frame(jb, &w, &h) != NULL, 1);
    after_frame = c.total;
    for (k = 0; k < 20; ++k) {
        uint16_t px[4];
        uint32_t fill[] = {0x02102030u, xy(0, 0), xy(64, 64)};
        gp0(jb, fill, 3);
        read_rect(jb, 0, 0, 4, 1, px);
        jb_frame(jb, &w, &h);
        jb_gpustat(jb);
        gp1(jb, 0x10000007u);
        jb_gpuread(jb);
        /* The calls over the commands, the GTE and the two options. */
        {
            jb_texture t = {.page_x = 512, .depth = JB_TEX_8BIT, .clut_x = 0, .clut_y = 480};
            jb_poly p;
            jb_rect r;
            jb_vertex lv[3];
            jb_polyline pl;

            memset(&p, 0, sizeof p);
            memset(&r, 0, sizeof r);
            memset(lv, 0, sizeof lv);
            p.vertices = 4, p.gouraud = 1, p.texture = &t, p.blend = JB_BLEND_ADD;
            p.v[1].x = 50, p.v[2].y = 50, p.v[3].x = 50, p.v[3].y = 50;
            r.x = 10, r.y = 10, r.w = 16, r.h = 16, r.texture = &t;
            lv[1].x = 100, lv[2].y = 100;
            pl.gouraud = 0, pl.blend = JB_BLEND_SUB, pl.count = 3, pl.v = lv;
            check("alloc test: area", jb_set_draw_area(jb, 0, 0, 319, 239), JB_OK);
            check("alloc test: offset", jb_set_draw_offset(jb, 1, 2), JB_OK);
            check("alloc test: window", jb_set_texture_window(jb, 1, 2, 3, 4), JB_OK);
            check("alloc test: mask", jb_set_mask(jb, 0, 0), JB_OK);
            check("alloc test: dither", jb_set_dither(jb, 1), JB_OK);
            check("alloc test: poly", jb_prim_poly(jb, &p), JB_OK);
            check("alloc test: rect", jb_prim_rect(jb, &r), JB_OK);
            check("alloc test: polyline", jb_prim_polyline(jb, &pl), JB_OK);
            check("alloc test: load", jb_vram_load(jb, 0, 300, 4, 1, px), JB_OK);
            check("alloc test: read", jb_vram_read(jb, 0, 300, 4, 1, px), JB_OK);
            check("alloc test: fill", jb_vram_fill(jb, 0, 300, 16, 16, 0x123456u), JB_OK);
            check("alloc test: copy", jb_vram_copy(jb, 0, 300, 100, 300, 16, 16), JB_OK);
            check("alloc test: options", jb_set_precise_vertices(jb, k & 1), JB_OK);
            check("alloc test: options", jb_set_perspective_correct(jb, k & 2), JB_OK);
            check("alloc test: begin frame", jb_gte_begin_frame(jb), JB_OK);
            check("alloc test: GTE", jb_gte_write(jb, JB_GTE_TRZ, 500), JB_OK);
            check("alloc test: GTE", jb_gte_command(jb, JB_GTE_RTPT), JB_OK);
            jb_gte_read(jb, JB_GTE_SXY2);
            check("alloc test: same scale", jb_set_scale(jb, 2), JB_OK);
            check("alloc test: same threads", jb_set_threads(jb, 3), JB_OK);
        }
    }
    check("no allocation per call after the first frame", c.total, after_frame);
    /* A scale with no memory for it: the context stays at 2x, and its
       picture, which has detail the 1x VRAM does not, is untouched. */
    {
        uint32_t tri[] = {0x30ff0000u, xy(3, 5), 0x0000ff00u, xy(290, 31), 0x000000ffu, xy(41, 215)};
        uint64_t before, changed;
        int t;

        gp0(jb, tri, 6);
        before = frame_hash(jb, &w, &h);
        c.fail_at = c.total + 1;
        check("jb_set_scale without memory", jb_set_scale(jb, 4), JB_ERROR_OUT_OF_MEMORY);
        check("its error string", strstr(jb_error_string(jb), "no memory for the 4x") != NULL, 1);
        c.fail_at = 0;
        check("still 2x, the same frame", frame_hash(jb, &w, &h) == before && w == 640, 1);
        /* The threads change with no pixel changed, with memory or without
           (the queue is there at 2x; threads that can't start are logged). */
        for (t = 1; t <= 4; ++t) {
            c.fail_at = (t & 1) ? c.total + 1 : 0;
            check("jb_set_threads at 2x", jb_set_threads(jb, t), JB_OK);
            c.fail_at = 0;
            check("the same frame after jb_set_threads", frame_hash(jb, &w, &h) == before, 1);
            check("still 2x after jb_set_threads", w, 640);
        }
        /* A real change of scale does remake the picture from the 1x VRAM. */
        check("jb_set_scale(3)", jb_set_scale(jb, 3), JB_OK);
        check("jb_set_scale(2)", jb_set_scale(jb, 2), JB_OK);
        changed = frame_hash(jb, &w, &h);
        check("the picture remade from 1x differs", changed != before, 1);
        check("jb_set_threads back", jb_set_threads(jb, 3), JB_OK);
    }
    /* A bigger frame that can't be had leaves the smaller one usable, with
       no new allocation. */
    c.fail_at = c.total + 1;
    gp1(jb, 0x08000027u); /* 640 x 480 */
    check("a frame that can't grow", jb_frame(jb, &w, &h) == NULL, 1);
    c.fail_at = c.total + 1;
    gp1(jb, 0x08000001u); /* 320 x 240 again */
    check("the smaller frame after a failed growth", jb_frame(jb, &w, &h) != NULL, 1);
    check("its width", w, 320 * 2);
    c.fail_at = 0;
    jb_destroy(jb);
    check("allocations left after destroy", c.live, 0);
    check("bytes left after destroy", (long long)c.live_bytes, 0);

    /* Each allocation of jb_create and the first jb_frame failing in
       turn, at 1x and 2x, with threads: the result says so, nothing
       leaks, and a context that is made works. */
    for (k = 1;; ++k) {
        jb_result r = JB_OK;
        int scale;
        long total_ok = 0;

        for (scale = 1; scale <= 2; ++scale) {
            char msg[96];

            memset(&c, 0, sizeof c);
            c.fail_at = k;
            cfg = counted(&c, scale, 2);
            jb = jb_create(&cfg, &r);
            if (jb == NULL) {
                snprintf(msg, sizeof msg, "allocation %d of %dx fails: result", k, scale);
                check(msg, r, JB_ERROR_OUT_OF_MEMORY);
            } else {
                const uint8_t *f = jb_frame(jb, &w, &h);
                if (f == NULL) {
                    snprintf(msg, sizeof msg, "allocation %d of %dx fails in jb_frame: error", k,
                             scale);
                    check(msg, strstr(jb_error_string(jb), "no memory") != NULL, 1);
                    snprintf(msg, sizeof msg, "allocation %d of %dx fails in jb_frame: size", k,
                             scale);
                    check(msg, w * 100 + h, 0);
                    f = jb_frame(jb, &w, &h); /* now it can */
                }
                snprintf(msg, sizeof msg, "allocation %d of %dx: frame after a failure", k, scale);
                check(msg, f != NULL && w == 320 * scale && h == 240 * scale, 1);
                jb_destroy(jb);
            }
            snprintf(msg, sizeof msg, "allocation %d of %dx fails: allocations left", k, scale);
            check(msg, c.live, 0);
            if (c.total < k)
                total_ok += 1;
        }
        if (total_ok == 2)
            break; /* past the last allocation at both scales */
        if (k > 64) {
            check("the failing-allocation loop ends", k, 0);
            break;
        }
    }
}

/* Many contexts, created and destroyed, some alive together. */
static void many(void)
{
    jb_context *alive[4] = {NULL, NULL, NULL, NULL};
    int k;

    for (k = 0; k < 120; ++k) {
        jb_config c = config0();
        jb_context *jb;
        uint32_t tri[] = {0x20ff00ffu, xy(5, 5), xy(200, 30), xy(40, 180)};
        int w, h;

        c.scale = 1 + k % 3;
        c.threads = 1 + k % 4;
        jb = must_create(&c);
        gp1(jb, 0x03000000u);
        gp0(jb, tri, 4);
        if (k % 3 == 0)
            check("frame of a short-lived context", jb_frame(jb, &w, &h) != NULL, 1);
        if (alive[k % 4] != NULL)
            jb_destroy(alive[k % 4]);
        alive[k % 4] = jb;
    }
    for (k = 0; k < 4; ++k)
        jb_destroy(alive[k]);
}

int main(void)
{
    strings();
    configs();
    good_configs();
    null_arguments();
    null_arguments_calls();
    allocator();
    many();
    return finish("api_errors");
}

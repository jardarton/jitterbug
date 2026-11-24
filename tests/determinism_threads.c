/*
 * determinism_threads: the same frames at every thread count, checked
 * against hashes stored once for every build (docs/determinism.md).
 *
 * Each case is a source of drawing at a scale (1x, 2x, 3x) with the two
 * float options (precise vertices and perspective-correct textures) off or
 * on. It is drawn on a fresh context at 1, 2, 3, 4, 8 and 16 threads, and
 * once more with the thread count changed by jb_set_threads while it
 * draws (1, 16, 2, 8, 3, 4, round again). Every run must give the hash
 * that determinism_threads.txt stores for the case. A single-threaded
 * build (JB_THREADS off, emscripten) clamps every request to one thread,
 * so there it checks the same hashes with one thread only, and says so.
 *
 * Every context it makes (its own and the workloads') logs to it. With
 * threads, a request that the library did not meet ("started only N of M
 * rasterising threads": threads that would not start, or no memory for
 * them) fails the case, since a run that fell back to one thread checks
 * nothing about threads. Single-threaded (JB_TEST_SINGLE_THREADED), the
 * clamping is what is expected.
 *
 * The sources, all through the public API:
 *   random0-3  seeded GP0 streams made to need the queue's ordering:
 *              textured primitives that overlap and blend over texture
 *              pages and palettes other primitives draw into, sprites
 *              that sample their own destination, polygons from the GTE
 *              (so the options apply), mask settings, texture windows,
 *              and fills, copies, uploads and reads (GP0(C0h) through
 *              jb_gpuread) between them; random3 is mostly large
 *              primitives
 *   flood      more small blended primitives than the queue holds (8192),
 *              with nothing between them that flushes it
 *   seed       the 28 recorded seed scenes (tests/seed) in order on one
 *              context, as api_golden.c expresses their ops; the options
 *              do nothing there (no GTE), so only off
 *   first-light, heavy, fill
 *              jb-bench's workloads (tools/jb-bench/workload.h): the
 *              example's scene and two busy frames, a few frames each
 *
 * The GP0 words of random, flood and seed are cut into jb_gp0 calls three
 * ways: as generated (whole commands or packets), one word a call, and in
 * random lengths, which cut commands and transfers anywhere. The runs of a
 * case use the three in turn, starting at a different one for each case,
 * so every thread count meets every cut over the cases.
 *
 * The hash (FNV-1a 64 over 64-bit little-endian lanes, below) covers, in
 * order: every word read back through jb_gpuread, the 1x VRAM at a few
 * points (jb_vram_read) and at the end, and the frame (jb_frame) at the
 * end, for seed at the end of every scene too; for random, flood and seed
 * the display is all of VRAM (1024 x 512 at the context's scale), so the
 * frame is the whole scaled picture; for the workloads it is every frame
 * they show.
 *
 *   determinism_threads [--record] EXPECTED SEED_TRACE...
 *
 * --record prints the hashes in EXPECTED's format, from the runs at one
 * thread, for a deliberate change (still failing if the thread counts
 * disagree). The cases and their order are fixed here; EXPECTED must hold
 * every one of them and nothing else.
 */
#include "api_test.h"
#include "trace.h"
#include "workload.h"

/* ------------------------------------------------------------------------ */
/* The hash */

#define HASH_INIT 0xcbf29ce484222325ull
#define HASH_PRIME 0x100000001b3ull

static uint64_t hash_lane(uint64_t h, uint64_t lane)
{
    return (h ^ lane) * HASH_PRIME;
}

/* Bytes as little-endian 64-bit lanes, the last one zero-padded, then the
   length. */
static uint64_t hash_bytes(uint64_t h, const uint8_t *p, size_t n)
{
    size_t i = 0;

    for (; i + 8 <= n; i += 8)
        h = hash_lane(h, (uint64_t)p[i] | (uint64_t)p[i + 1] << 8 | (uint64_t)p[i + 2] << 16 |
                             (uint64_t)p[i + 3] << 24 | (uint64_t)p[i + 4] << 32 |
                             (uint64_t)p[i + 5] << 40 | (uint64_t)p[i + 6] << 48 |
                             (uint64_t)p[i + 7] << 56);
    if (i < n) {
        uint64_t lane = 0;
        int k;

        for (k = 0; i < n; ++i, ++k)
            lane |= (uint64_t)p[i] << (8 * k);
        h = hash_lane(h, lane);
    }
    return hash_lane(h, (uint64_t)n);
}

static uint64_t hash_pixels(uint64_t h, const uint16_t *p, size_t n)
{
    size_t i = 0;

    for (; i + 4 <= n; i += 4)
        h = hash_lane(h, (uint64_t)p[i] | (uint64_t)p[i + 1] << 16 | (uint64_t)p[i + 2] << 32 |
                             (uint64_t)p[i + 3] << 48);
    for (; i < n; ++i)
        h = hash_lane(h, p[i]);
    return hash_lane(h, (uint64_t)n);
}

/* ------------------------------------------------------------------------ */
/* A run: one context, its hash, and how its GP0 words are cut */

enum { CUT_WHOLE, CUT_WORDS, CUT_RANDOM, CUTS };
static const char *const cut_names[CUTS] = {"whole", "words", "random"};

/* The thread counts every case runs at, and the cycle jb_set_threads goes
   round in the last run. */
static const int thread_counts[] = {1, 2, 3, 4, 8, 16};
enum { COUNTS = sizeof thread_counts / sizeof thread_counts[0] };
static const int cycle[] = {1, 16, 2, 8, 3, 4};
enum { CYCLE = sizeof cycle / sizeof cycle[0] };

typedef struct {
    jb_context *jb;
    uint64_t h;
    int cut;
    uint64_t cut_rng; /* the random cut's lengths, apart from the stream's */
    int cycling, cycle_at;
    uint32_t *pending; /* GP0 words not yet sent */
    size_t npending, cap;
    int failed;
} run;

static uint16_t vram_copy[VRAM_PIXELS];

/* The library's messages about rasterising threads it did not start, for
   every context made here: in this case, and in all. */
static struct {
    unsigned in_case, total;
    char last[256];
} fewer;

static void log_fn(void *user, const char *message)
{
    (void)user;
    if (strstr(message, "rasterising threads") != NULL) {
        ++fewer.in_case;
        ++fewer.total;
        snprintf(fewer.last, sizeof fewer.last, "%s", message);
    }
}

static void run_check(run *r, const char *what, jb_result got)
{
    if (got != JB_OK && !r->failed) {
        printf("FAIL %s: %s (%s)\n", what, jb_result_string(got), jb_error_string(r->jb));
        r->failed = 1;
    }
    check(what, got, JB_OK);
}

/* Sends the pending words, cut as the run says. */
static void send(run *r)
{
    size_t at = 0;

    while (at < r->npending) {
        size_t n = r->npending - at;

        if (r->cut == CUT_WORDS)
            n = 1;
        else if (r->cut == CUT_RANDOM) {
            size_t k;
            r->cut_rng = r->cut_rng * 6364136223846793005ull + 1442695040888963407ull;
            k = 1 + (size_t)((r->cut_rng >> 33) % 37u);
            n = k < n ? k : n;
        }
        run_check(r, "jb_gp0", jb_gp0(r->jb, r->pending + at, n));
        at += n;
    }
    r->npending = 0;
}

static void emit(run *r, uint32_t w)
{
    if (r->npending == r->cap) {
        uint32_t *p;
        r->cap = r->cap ? r->cap * 2 : 4096;
        p = (uint32_t *)realloc(r->pending, r->cap * sizeof *p);
        if (p == NULL) {
            printf("out of memory\n");
            exit(2);
        }
        r->pending = p;
    }
    r->pending[r->npending++] = w;
}

static void emit_all(run *r, const uint32_t *w, size_t n)
{
    size_t i;

    for (i = 0; i < n; ++i)
        emit(r, w[i]);
}

/* The next thread count of the cycle, in the run that changes it. */
static void maybe_cycle(run *r)
{
    if (!r->cycling)
        return;
    send(r);
    r->cycle_at = (r->cycle_at + 1) % CYCLE;
    run_check(r, "jb_set_threads", jb_set_threads(r->jb, cycle[r->cycle_at]));
}

/* The whole 1x VRAM into the hash. */
static void hash_vram(run *r)
{
    send(r);
    run_check(r, "jb_vram_read", jb_vram_read(r->jb, 0, 0, VRAM_W, VRAM_H, vram_copy));
    r->h = hash_pixels(r->h, vram_copy, VRAM_PIXELS);
}

/* The frame into the hash. */
static void hash_frame(run *r)
{
    const uint8_t *f;
    int w = 0, h = 0;

    send(r);
    f = jb_frame(r->jb, &w, &h);
    if (f == NULL) {
        run_check(r, "jb_frame", JB_ERROR_OUT_OF_MEMORY);
        return;
    }
    r->h = hash_lane(r->h, (uint64_t)w << 32 | (uint32_t)h);
    r->h = hash_bytes(r->h, f, (size_t)w * (size_t)h * 4);
}

/* A context that shows all of VRAM. */
static void run_start(run *r, int scale, int options, int threads, int cut, uint64_t cut_seed)
{
    jb_config c;
    jb_result res = JB_ERROR_UNSUPPORTED;

    memset(&c, 0, sizeof c);
    c.scale = scale;
    c.width = VRAM_W;
    c.height = VRAM_H;
    c.threads = threads;
    c.precise_vertices = options;
    c.perspective_correct = options;
    c.log = log_fn;
    r->jb = jb_create(&c, &res);
    if (r->jb == NULL) {
        printf("FAIL jb_create at %dx, %d threads: %s\n", scale, threads, jb_result_string(res));
        exit(2);
    }
    r->h = HASH_INIT;
    r->cut = cut;
    r->cut_rng = cut_seed;
    r->npending = 0;
    r->failed = 0;
    run_check(r, "GP1(03h)", jb_gp1(r->jb, 0x03000000u));
}

static uint64_t run_end(run *r)
{
    hash_vram(r);
    hash_frame(r);
    r->h = hash_lane(r->h, jb_gpustat(r->jb));
    jb_destroy(r->jb);
    r->jb = NULL;
    free(r->pending);
    r->pending = NULL;
    return r->h;
}

/* ------------------------------------------------------------------------ */
/* random: seeded streams that depend on the queue's order */

/*
 * The generator draws one number a statement: C leaves the order in which
 * the operands of an expression or the arguments of a call are evaluated
 * to the compiler, so two draws in one expression would make the stream,
 * and the hashes, depend on it.
 */
typedef struct {
    uint64_t s;
    int big;  /* mostly large primitives */
    int calm; /* a stretch with nothing that flushes the queue (below) */
} rng;

static uint32_t next(rng *g)
{
    g->s = g->s * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(g->s >> 32);
}

static int below(rng *g, int n)
{
    return (int)(next(g) % (uint32_t)n);
}

static int between(rng *g, int lo, int hi)
{
    return lo + below(g, hi - lo + 1);
}

/* A GP0 position or size word from two draws in [x0, x1] and [y0, y1], x
   first. */
static uint32_t draw_xy(rng *g, int x0, int x1, int y0, int y1)
{
    int x = between(g, x0, x1);
    int y = between(g, y0, y1);

    return xy(x, y);
}

/*
 * Primitives draw in the left 640 x 512 of VRAM. Most of the time their
 * texture pages and palettes are there too, so they sample what others
 * drew, and transfers come between them: the queue is flushed every few
 * primitives, for the orders the workers must keep. A calm stretch (half
 * of them, 100 commands each) draws only inside that area, takes every
 * texel and palette from the right 384 x 512 that nothing draws into, and
 * makes no transfers, so the queue grows long and the workers share many
 * primitives at once.
 */
enum { AREA_W = 640, AREA_H = 512, STRETCH = 100 };

/* A vertex within reach of (cx, cy) (the drawing offset is kept small). */
static uint32_t vertex_near(rng *g, int cx, int cy, int reach)
{
    return draw_xy(g, cx - reach, cx + reach, cy - reach, cy + reach);
}

static int reach(rng *g)
{
    int k = below(g, 16);

    if (g->big)
        return k < 4 ? 40 : 200;
    return k == 0 ? 220 : k < 4 ? 90 : 36;
}

/* A texture page word's high half (as in a polygon's second texture word,
   and GP0(E1h)'s bits 0-8): page x in 64s, page y 0 or 256, blend mode,
   depth (0 4-bit, 1 8-bit, 2 15-bit). Inside the area, or in a calm
   stretch wholly to its right (a page is 64, 128 or 256 pixels wide). */
static uint32_t page_bits(rng *g)
{
    uint32_t depth = (uint32_t)below(g, 3);
    uint32_t py = (uint32_t)below(g, 2);
    uint32_t mode = (uint32_t)below(g, 4);
    uint32_t px;

    if (g->calm)
        px = (uint32_t)between(g, AREA_W / 64, 16 - (1 << depth));
    else
        px = (uint32_t)below(g, AREA_W / 64);
    return px | py << 4 | mode << 5 | depth << 7;
}

/* A palette word's high half: x in 16s, y; a 256-entry palette from the
   calm range ends inside VRAM. */
static uint32_t clut_bits(rng *g)
{
    int x, y;

    if (g->calm)
        x = between(g, AREA_W / 16, (1024 - 256) / 16);
    else
        x = below(g, AREA_W / 16);
    y = below(g, AREA_H);
    return (uint32_t)x | (uint32_t)y << 6;
}

/* GP0(E1h): a page, dithering, the rectangle flips. */
static uint32_t draw_mode(rng *g)
{
    uint32_t page = page_bits(g);
    uint32_t dither = (uint32_t)below(g, 2);
    uint32_t flips = (uint32_t)below(g, 4);

    return 0xe1000000u | page | dither << 9 | flips << 12;
}

/* A texture word: u and v in [u0, u0 + span] and [v0, v0 + span] (wrapped
   to a byte), and the high half given. */
static uint32_t uv(rng *g, int u0, int v0, int span, uint32_t high)
{
    int u = u0 + below(g, span + 1);
    int v = v0 + below(g, span + 1);

    return (uint32_t)(u & 0xff) | (uint32_t)(v & 0xff) << 8 | high << 16;
}

static uint32_t colour(rng *g)
{
    return next(g) & 0xffffffu;
}

/* A polygon of any kind, its vertices around (cx, cy). */
static void emit_poly(run *r, rng *g, int cx, int cy)
{
    uint32_t op = 0x20u + (uint32_t)below(g, 32);
    int verts = op & 0x08 ? 4 : 3, gouraud = (op & 0x10) != 0, textured = (op & 0x04) != 0;
    int rr = reach(g);
    int u0 = below(g, 256);
    int v0 = below(g, 256);
    int span = below(g, 2) ? 255 : 64;
    int i;

    emit(r, op << 24 | colour(g));
    for (i = 0; i < verts; ++i) {
        if (gouraud && i > 0)
            emit(r, colour(g));
        emit(r, vertex_near(g, cx, cy, rr));
        if (textured) {
            uint32_t high = i == 0 ? clut_bits(g) : i == 1 ? page_bits(g) : 0u;
            emit(r, uv(g, u0, v0, span, high));
        }
    }
}

/* A rectangle of any kind; sometimes textured from the page it draws on,
   so it samples its own destination. */
static void emit_rect(run *r, rng *g, int cx, int cy)
{
    uint32_t op = 0x60u + (uint32_t)below(g, 32);
    int size = (int)(op >> 3) & 3;
    int x = cx + between(g, -60, 60);
    int y = cy + between(g, -60, 60);

    if (op & 0x04) {
        /* The draw mode gives a rectangle its page and flips. */
        uint32_t e1 = draw_mode(g);
        if (!g->calm && below(g, 3) == 0) /* its own page: at (x & ~63, y & 256) */
            e1 = (e1 & ~0x1fu) | (uint32_t)(((x & 0x3ff) >> 6) & 15) |
                 (uint32_t)((y >> 8) & 1) << 4;
        emit(r, e1);
    }
    emit(r, op << 24 | colour(g));
    emit(r, xy(x, y));
    if (op & 0x04) {
        uint32_t clut = clut_bits(g);
        emit(r, uv(g, 0, 0, 255, clut));
    }
    if (size == 0) {
        int big = below(g, 8) == 0 || g->big;
        emit(r, draw_xy(g, 1, big ? 300 : 48, 1, big ? 200 : 48));
    }
}

static void emit_line(run *r, rng *g, int cx, int cy)
{
    uint32_t op = 0x40u + ((uint32_t)below(g, 32) & ~0x08u);
    int rr = reach(g) * 2;

    if (below(g, 3) == 0) { /* a polyline */
        int n = between(g, 2, 7), i;

        op |= 0x08;
        emit(r, op << 24 | colour(g));
        emit(r, vertex_near(g, cx, cy, rr));
        for (i = 1; i < n; ++i) {
            if (op & 0x10)
                emit(r, colour(g));
            emit(r, vertex_near(g, cx, cy, rr) & 0x0fff0fffu); /* never the end mark */
        }
        emit(r, 0x55555555u);
        return;
    }
    emit(r, op << 24 | colour(g));
    emit(r, vertex_near(g, cx, cy, rr));
    if (op & 0x10)
        emit(r, colour(g));
    emit(r, vertex_near(g, cx, cy, rr));
}

static void gte_write(run *r, int reg, uint32_t value)
{
    run_check(r, "jb_gte_write", jb_gte_write(r->jb, reg, value));
}

/* The GTE's camera: a slight rotation, the centre of the area, depth 1000. */
static void gte_setup(run *r, rng *g)
{
    int c = 4096 - below(g, 200);
    int s = between(g, -900, 900);
    int h = between(g, 250, 700);

    gte_write(r, JB_GTE_RT11RT12, JB_GTE_PAIR(c, -s));
    gte_write(r, JB_GTE_RT13RT21, JB_GTE_PAIR(0, s));
    gte_write(r, JB_GTE_RT22RT23, JB_GTE_PAIR(c, s / 3));
    gte_write(r, JB_GTE_RT31RT32, JB_GTE_PAIR(0, -s / 3));
    gte_write(r, JB_GTE_RT33, 4096);
    gte_write(r, JB_GTE_TRX, 0);
    gte_write(r, JB_GTE_TRY, 0);
    gte_write(r, JB_GTE_TRZ, 1000);
    gte_write(r, JB_GTE_OFX, (uint32_t)(AREA_W / 2) << 16);
    gte_write(r, JB_GTE_OFY, (uint32_t)(AREA_H / 2) << 16);
    gte_write(r, JB_GTE_H, (uint32_t)h);
}

/* A textured triangle or quad from vertices the GTE projects, so precise
   vertices and perspective-correct textures apply to it. */
static void emit_gte_poly(run *r, rng *g)
{
    static const int corner[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    uint32_t sxy[4], op, low = (uint32_t)below(g, 4); /* raw, blended */
    uint32_t quad = (uint32_t)below(g, 2), gouraud = (uint32_t)below(g, 2);
    int rr = g->big ? 500 : 160, span, u0, v0, bx, by, bz, k;

    op = 0x24u | low | quad << 3 | gouraud << 4;
    span = below(g, 2) ? 255 : 100;
    u0 = below(g, 256 - span);
    v0 = below(g, 256 - span);
    bx = between(g, -300, 300);
    by = between(g, -300, 300);
    bz = between(g, -300, 300);
    send(r); /* the projections, then the primitives drawn from them */
    for (k = 0; k < 3; ++k) {
        int x = bx + between(g, -rr, rr);
        int y = by + between(g, -rr, rr);
        int z = bz + between(g, -rr / 2, rr / 2);
        gte_write(r, 2 * k, JB_GTE_PAIR(x, y));
        gte_write(r, 2 * k + 1, (uint32_t)z);
    }
    run_check(r, "jb_gte_command RTPT", jb_gte_command(r->jb, JB_GTE_RTPT));
    for (k = 0; k < 3; ++k)
        sxy[k] = jb_gte_read(r->jb, JB_GTE_SXY0 + k);
    if (quad) {
        int x = bx + between(g, -rr, rr);
        int y = by + between(g, -rr, rr);
        gte_write(r, JB_GTE_VXY0, JB_GTE_PAIR(x, y));
        gte_write(r, JB_GTE_VZ0, (uint32_t)bz);
        run_check(r, "jb_gte_command RTPS", jb_gte_command(r->jb, JB_GTE_RTPS));
        sxy[3] = jb_gte_read(r->jb, JB_GTE_SXY2);
    }
    emit(r, op << 24 | colour(g));
    for (k = 0; k < (quad ? 4 : 3); ++k) {
        uint32_t high;
        if (gouraud && k > 0)
            emit(r, colour(g));
        emit(r, sxy[k]);
        high = k == 0 ? clut_bits(g) : k == 1 ? page_bits(g) : 0u;
        emit(r, (uint32_t)(u0 + corner[k][0] * span) | (uint32_t)(v0 + corner[k][1] * span) << 8 |
                    high << 16);
    }
}

/* A position in the area. */
static uint32_t in_area(rng *g)
{
    return draw_xy(g, 0, AREA_W - 1, 0, AREA_H - 1);
}

/* Fills, copies, uploads and reads inside the area. */
static void emit_transfer(run *r, rng *g)
{
    int k = below(g, 8), i;

    if (k < 3) { /* fill */
        emit(r, 0x02000000u | colour(g));
        emit(r, in_area(g));
        emit(r, draw_xy(g, 1, g->big ? 400 : 128, 1, g->big ? 300 : 64));
    } else if (k < 5) { /* VRAM to VRAM, often overlapping */
        int x = below(g, AREA_W);
        int y = below(g, AREA_H);
        emit(r, 0x80000000u);
        emit(r, xy(x, y));
        emit(r, draw_xy(g, x - 20, x + 20, y - 20, y + 20));
        emit(r, draw_xy(g, 1, 96, 1, 96));
    } else if (k < 7) { /* CPU to VRAM: texels or a palette */
        int w = between(g, 1, 40);
        int h = between(g, 1, 24);
        if (below(g, 3) == 0) {
            w = 16 * between(g, 1, 16);
            h = 1;
        }
        emit(r, 0xa0000000u);
        emit(r, in_area(g));
        emit(r, xy(w, h));
        for (i = 0; i < (w * h + 1) / 2; ++i) {
            uint32_t w2 = next(g);
            emit(r, w2 & (below(g, 4) ? 0x7fff7fffu : 0xffffffffu));
        }
    } else { /* VRAM to CPU, read back now */
        int w = between(g, 1, 24);
        int h = between(g, 1, 12);
        emit(r, 0xc0000000u);
        emit(r, in_area(g));
        emit(r, xy(w, h));
        send(r);
        for (i = 0; i < (w * h + 1) / 2; ++i)
            r->h = hash_lane(r->h, jb_gpuread(r->jb));
    }
}

/* The drawing area: all of VRAM, or in a calm stretch the area alone. */
static void emit_area(run *r, const rng *g)
{
    emit(r, 0xe3000000u);
    emit(r, 0xe4000000u | (g->calm ? AREA_W - 1u : 1023u) | 511u << 10);
}

/* Drawing state: the draw mode (dither, blend mode, page), the texture
   window, the mask settings, the offset, now and then a smaller area. */
static void emit_state(run *r, rng *g)
{
    int k = below(g, 10);

    if (k < 4) {
        emit(r, draw_mode(g));
    } else if (k < 6) {
        uint32_t window = next(g) & 0xfffffu;
        emit(r, 0xe2000000u | (below(g, 2) ? 0u : window));
    } else if (k < 8) {
        uint32_t mask = (uint32_t)below(g, 4);
        emit(r, 0xe6000000u | (below(g, 3) == 0 ? mask : 0u));
    } else if (k < 9) {
        int dx = between(g, -8, 8);
        int dy = between(g, -8, 8);
        emit(r, 0xe5000000u | ((uint32_t)dx & 0x7ff) | ((uint32_t)dy & 0x7ff) << 11);
    } else if (below(g, 2)) {
        int x0 = below(g, AREA_W - 64);
        int y0 = below(g, AREA_H - 64);
        int x1 = x0 + between(g, 32, 300);
        int y1 = y0 + between(g, 32, 200);
        int right = g->calm ? AREA_W - 1 : 1023;
        emit(r, 0xe3000000u | (uint32_t)x0 | (uint32_t)y0 << 10);
        emit(r, 0xe4000000u | (uint32_t)(x1 > right ? right : x1) |
                    (uint32_t)(y1 > 511 ? 511 : y1) << 10);
    } else {
        emit_area(r, g);
    }
}

/* Texels, palettes and a mask bit here and there, all over VRAM, to start
   from. */
static void emit_start(run *r, rng *g)
{
    static const uint32_t state[] = {0xe5000000u, 0xe6000000u};
    int i;

    emit_area(r, g);
    emit_all(r, state, 2);
    emit(r, 0xa0000000u);
    emit(r, xy(0, 0));
    emit(r, xy(0, 0)); /* 1024 x 512 */
    for (i = 0; i < VRAM_PIXELS / 2; ++i)
        emit(r, next(g) & (i % 7 == 0 ? 0xffffffffu : 0x7fff7fffu));
}

static uint64_t source_random(int which, int scale, int options, int threads, int cut,
                              int cycling)
{
    run r = {0};
    rng g;
    int i, commands = which == 3 ? 400 : 1500;

    g.s = 0x9e3779b97f4a7c15ull * (uint64_t)(which + 1);
    g.big = which == 3;
    g.calm = 0;
    run_start(&r, scale, options, cycling ? cycle[0] : threads, cut,
              (uint64_t)(which * 977 + threads * 31 + cut));
    r.cycling = cycling;
    r.cycle_at = 0;
    emit_start(&r, &g);
    send(&r);
    gte_setup(&r, &g);
    for (i = 0; i < commands; ++i) {
        int k, cx, cy;

        if (i % STRETCH == 0) {
            /* A new stretch, and a new frame for the GTE's projections. */
            g.calm = below(&g, 2) == 0;
            emit_area(&r, &g);
            send(&r);
            run_check(&r, "jb_gte_begin_frame", jb_gte_begin_frame(r.jb));
        }
        if (i % 250 == 249)
            maybe_cycle(&r);
        if (i == commands / 3 || i == 2 * commands / 3)
            hash_vram(&r);
        k = below(&g, 100);
        cx = below(&g, AREA_W);
        cy = below(&g, AREA_H);
        if (k < 30)
            emit_poly(&r, &g, cx, cy);
        else if (k < 45)
            emit_gte_poly(&r, &g);
        else if (k < 62)
            emit_rect(&r, &g, cx, cy);
        else if (k < 72)
            emit_line(&r, &g, cx, cy);
        else if (k < 86 && !g.calm)
            emit_transfer(&r, &g);
        else
            emit_state(&r, &g);
    }
    return run_end(&r);
}

/* ------------------------------------------------------------------------ */
/* flood: more than a queue of small blended primitives */

static uint64_t source_flood(int scale, int options, int threads, int cut, int cycling)
{
    static const uint32_t area[] = {0xe3000000u, 0xe4000000u | 1023u | 511u << 10, 0xe5000000u};
    run r = {0};
    rng g;
    int i;

    g.s = 77;
    g.big = 0;
    g.calm = 0;
    run_start(&r, scale, options, cycling ? cycle[0] : threads, cut, (uint64_t)(threads * 31 + cut));
    r.cycling = cycling;
    r.cycle_at = 0;
    emit_all(&r, area, 3);
    for (i = 0; i < 20000; ++i) {
        int x = below(&g, AREA_W);
        int y = below(&g, AREA_H);
        int k = below(&g, 4);

        if (i % 5000 == 4999) /* drawn up to here, mid-queue */
            maybe_cycle(&r);
        if (i % 1000 == 0) {
            uint32_t mode = (uint32_t)below(&g, 4);
            uint32_t dither = (uint32_t)below(&g, 2);
            emit(&r, 0xe1000000u | mode << 5 | dither << 9);
        }
        if (k == 0) { /* a blended 8 x 8 rectangle */
            emit(&r, 0x72000000u | colour(&g));
            emit(&r, xy(x, y));
        } else if (k == 1) { /* a variable one */
            emit(&r, 0x62000000u | colour(&g));
            emit(&r, xy(x, y));
            emit(&r, draw_xy(&g, 1, 12, 1, 12));
        } else if (k == 2) { /* a blended Gouraud triangle */
            emit(&r, 0x32000000u | colour(&g));
            emit(&r, xy(x, y));
            emit(&r, colour(&g));
            emit(&r, draw_xy(&g, x - 10, x + 10, y + 2, y + 12));
            emit(&r, colour(&g));
            emit(&r, draw_xy(&g, x - 10, x + 10, y - 12, y - 2));
        } else { /* a blended line */
            emit(&r, 0x42000000u | colour(&g));
            emit(&r, xy(x, y));
            emit(&r, draw_xy(&g, x - 16, x + 16, y - 16, y + 16));
        }
    }
    return run_end(&r);
}

/* ------------------------------------------------------------------------ */
/* seed: the recorded scenes, their ops as api_golden.c expresses them */

static int ntraces;
static char **traces;

static void seed_load(run *r, int x, int y, int w, int h, const uint16_t *pix, size_t n)
{
    size_t i;

    emit(r, 0xa0000000u);
    emit(r, xy(x, y));
    emit(r, xy(w, h));
    for (i = 0; i < n; i += 2)
        emit(r, pix[i] | (i + 1 < n ? (uint32_t)pix[i + 1] << 16 : 0));
}

static void seed_op(run *r, trace_reader *t, uint32_t code)
{
    int v[6], i;

    switch (code) {
    case TRACE_OP_GP0: {
        uint32_t n = trace_u32(t), k;
        (void)trace_i32(t);
        for (k = 0; k < n; ++k)
            emit(r, trace_u32(t));
        break;
    }
    case TRACE_OP_FILL:
        for (i = 0; i < 5; ++i)
            v[i] = trace_i32(t);
        emit(r, 0x02000000u | ((uint32_t)v[4] & 0xffffffu));
        emit(r, xy(v[0], v[1]));
        emit(r, xy(v[2], v[3]));
        break;
    case TRACE_OP_LOAD: {
        uint32_t n;
        uint16_t *pix;
        for (i = 0; i < 4; ++i)
            v[i] = trace_i32(t);
        n = trace_u32(t);
        pix = trace_u16s(t, n);
        seed_load(r, v[0], v[1], v[2], v[3], pix, n);
        free(pix);
        break;
    }
    case TRACE_OP_STORE: {
        int w, h;
        size_t n, k;
        for (i = 0; i < 4; ++i)
            v[i] = trace_i32(t);
        w = xfer_w(v[2]);
        h = xfer_h(v[3]);
        n = ((size_t)w * (size_t)h + 1) / 2;
        emit(r, 0xc0000000u);
        emit(r, xy(v[0], v[1]));
        emit(r, xy(v[2], v[3]));
        send(r);
        for (k = 0; k < n; ++k)
            r->h = hash_lane(r->h, jb_gpuread(r->jb));
        t->pos = t->end; /* the recorded pixels: api_golden.c compares them */
        break;
    }
    case TRACE_OP_COPY:
        for (i = 0; i < 6; ++i)
            v[i] = trace_i32(t);
        emit(r, 0x80000000u);
        emit(r, xy(v[0], v[1]));
        emit(r, xy(v[2], v[3]));
        emit(r, xy(v[4], v[5]));
        break;
    case TRACE_OP_RESET_STATE: {
        static const uint32_t w[6] = {0xe1000000u, 0xe2000000u, 0xe3000000u,
                                      0xe4000000u, 0xe5000000u, 0xe6000000u};
        emit_all(r, w, 6);
        break;
    }
    case TRACE_OP_POKE: {
        uint32_t plane = trace_u32(t), w = trace_u32(t), h = trace_u32(t), runs = trace_u32(t);
        uint32_t k, j, stat;
        uint16_t *pix;
        size_t at = 0;
        if (plane != 0 || w != VRAM_W || h != VRAM_H)
            trace_die(t, "a POKE of the scaled VRAM, which has no public equivalent");
        pix = (uint16_t *)must_alloc((size_t)VRAM_PIXELS * 2);
        for (k = 0; k < runs; ++k) {
            uint32_t count = trace_u32(t), value = trace_u32(t);
            if (count > VRAM_PIXELS - at)
                trace_die(t, "POKE runs overflow the plane");
            for (j = 0; j < count; ++j)
                pix[at++] = (uint16_t)value;
        }
        /* With the mask settings off for the write, then back. */
        send(r);
        stat = jb_gpustat(r->jb);
        emit(r, 0xe6000000u);
        seed_load(r, 0, 0, 0, 0, pix, VRAM_PIXELS);
        emit(r, 0xe6000000u | ((stat >> 11) & 1) | ((stat >> 12) & 1) << 1);
        free(pix);
        break;
    }
    case TRACE_OP_CAPTURE: {
        unsigned kind = trace_u32(t);
        t->pos = t->end;
        if (kind == 5) /* the scene's end */
            hash_vram(r);
        break;
    }
    default: /* nothing public to do (api_golden.c, at its top) */
        t->pos = t->end;
        break;
    }
    trace_op_done(t);
}

static uint64_t source_seed(int scale, int options, int threads, int cut, int cycling)
{
    run r = {0};
    int i;

    run_start(&r, scale, options, cycling ? cycle[0] : threads, cut, (uint64_t)(threads * 31 + cut));
    r.cycling = cycling;
    r.cycle_at = 0;
    for (i = 0; i < ntraces; ++i) {
        trace_reader t;
        trace_prelude pre;
        char name[96];
        uint32_t code;

        trace_open(&t, traces[i], name, sizeof name, &pre);
        while ((code = trace_next_op(&t)) != TRACE_OP_END)
            seed_op(&r, &t, code);
        trace_close(&t);
        hash_frame(&r);
        maybe_cycle(&r);
    }
    return run_end(&r);
}

/* ------------------------------------------------------------------------ */
/* jb-bench's workloads */

static uint64_t source_workload(const wl_info *wl, int frames, int scale, int options,
                                int threads, int cycling)
{
    wl_options o;
    workload *w;
    jb_result res = JB_ERROR_UNSUPPORTED;
    run r = {0};
    int f, at = 0;

    o.scale = scale;
    o.threads = cycling ? cycle[0] : threads;
    o.precise = options;
    o.persp = options;
    o.log = log_fn;
    o.log_user = NULL;
    w = wl->create(&o, &res);
    if (w == NULL) {
        printf("FAIL %s at %dx, %d threads: %s\n", wl->name, scale, threads,
               jb_result_string(res));
        exit(2);
    }
    r.jb = w->jb;
    r.h = HASH_INIT;
    for (f = 0; f < frames; ++f) {
        run_check(&r, "draw", w->draw(w, (unsigned)f, NULL));
        hash_frame(&r);
        if (cycling) {
            at = (at + 1) % CYCLE;
            run_check(&r, "jb_set_threads", jb_set_threads(r.jb, cycle[at]));
        }
    }
    hash_vram(&r);
    w->destroy(w);
    free(r.pending);
    return r.h;
}

/* ------------------------------------------------------------------------ */
/* The cases */

enum { SRC_RANDOM, SRC_FLOOD, SRC_SEED, SRC_WORKLOAD };

typedef struct {
    const char *name;
    int kind, which, frames, with_options;
    const wl_info *wl;
} source;

static const source sources[] = {
    {"random0", SRC_RANDOM, 0, 0, 1, NULL},
    {"random1", SRC_RANDOM, 1, 0, 1, NULL},
    {"random2", SRC_RANDOM, 2, 0, 1, NULL},
    {"random3", SRC_RANDOM, 3, 0, 1, NULL},
    {"flood", SRC_FLOOD, 0, 0, 0, NULL},
    {"seed", SRC_SEED, 0, 0, 0, NULL},
    {"first-light", SRC_WORKLOAD, 0, 8, 1, &wl_first_light},
    {"heavy", SRC_WORKLOAD, 0, 3, 1, &wl_heavy},
    {"fill", SRC_WORKLOAD, 0, 2, 1, &wl_fill},
};
enum { SOURCES = sizeof sources / sizeof sources[0] };

static uint64_t draw_case(const source *s, int scale, int options, int threads, int cut,
                          int cycling)
{
    switch (s->kind) {
    case SRC_RANDOM: return source_random(s->which, scale, options, threads, cut, cycling);
    case SRC_FLOOD: return source_flood(scale, options, threads, cut, cycling);
    case SRC_SEED: return source_seed(scale, options, threads, cut, cycling);
    default: return source_workload(s->wl, s->frames, scale, options, threads, cycling);
    }
}

/* EXPECTED: "name scale options hash" a line, '#' starting a comment. */
typedef struct {
    char name[32];
    int scale;
    char options[8];
    uint64_t hash;
    int used;
} expected;

enum { MAX_EXPECTED = 128 };

static int read_expected(const char *path, expected *e, int *n)
{
    FILE *fp = fopen(path, "r");
    char line[256];

    *n = 0;
    if (fp == NULL) {
        printf("FAIL cannot open %s\n", path);
        return 1;
    }
    while (fgets(line, sizeof line, fp) != NULL) {
        char hash[32], *end;

        if (line[0] == '#' || strspn(line, " \t\r\n") == strlen(line))
            continue;
        if (*n == MAX_EXPECTED || sscanf(line, "%31s %d %7s %31s", e[*n].name, &e[*n].scale,
                                         e[*n].options, hash) != 4) {
            printf("FAIL %s: cannot read the line: %s", path, line);
            fclose(fp);
            return 1;
        }
        e[*n].hash = strtoull(hash, &end, 16);
        e[*n].used = 0;
        if (*end != '\0' || strlen(hash) != 16) {
            printf("FAIL %s: bad hash %s\n", path, hash);
            fclose(fp);
            return 1;
        }
        ++*n;
    }
    fclose(fp);
    return 0;
}

int main(int argc, char **argv)
{
    expected want[MAX_EXPECTED];
    int nwant = 0, record = 0, i = 1, si, scale, options, k, cases = 0, bad = 0;
    const char *path;

    if (i < argc && strcmp(argv[i], "--record") == 0) {
        record = 1;
        ++i;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: determinism_threads [--record] EXPECTED SEED_TRACE...\n");
        return 2;
    }
    path = argv[i++];
    traces = argv + i;
    ntraces = argc - i;
    if (ntraces == 0) {
        fprintf(stderr, "determinism_threads: no seed traces given\n");
        return 2;
    }
    if (!record && read_expected(path, want, &nwant) != 0)
        return 1;
    if (record)
        printf("# name scale options hash\n");

    for (si = 0; si < SOURCES; ++si) {
        const source *s = &sources[si];

        for (scale = 1; scale <= 3; ++scale)
            for (options = 0; options <= s->with_options; ++options) {
                const char *opt = options ? "on" : "off";
                uint64_t got[COUNTS + 1];
                int cut0 = cases % CUTS, differ = 0;
                expected *e = NULL;

                fewer.in_case = 0;
                for (k = 0; k <= COUNTS; ++k) {
                    int cycling = k == COUNTS;
                    int threads = cycling ? 0 : thread_counts[k];
                    got[k] = draw_case(s, scale, options, threads, (cut0 + k) % CUTS, cycling);
                    differ |= got[k] != got[0];
                }
                ++cases;
#ifndef JB_TEST_SINGLE_THREADED
                if (fewer.in_case > 0) {
                    printf("FAIL %s %dx options %s: %u thread requests not met, the last: %s\n",
                           s->name, scale, opt, fewer.in_case, fewer.last);
                    ++bad;
                    for (k = 0; k < nwant; ++k)
                        if (strcmp(want[k].name, s->name) == 0 && want[k].scale == scale &&
                            strcmp(want[k].options, opt) == 0)
                            want[k].used = 1;
                    continue;
                }
#endif
                if (record) {
                    printf("%-12s %d %-3s %016llx\n", s->name, scale, opt,
                           (unsigned long long)got[0]);
                    if (differ)
                        ++bad;
                    continue;
                }
                for (k = 0; k < nwant; ++k)
                    if (strcmp(want[k].name, s->name) == 0 && want[k].scale == scale &&
                        strcmp(want[k].options, opt) == 0)
                        e = &want[k];
                if (e == NULL) {
                    printf("FAIL %s %dx options %s: not in %s\n", s->name, scale, opt, path);
                    ++bad;
                    continue;
                }
                e->used = 1;
                printf("%-12s %dx options %-3s %016llx", s->name, scale, opt,
                       (unsigned long long)e->hash);
                for (k = 0; k <= COUNTS; ++k)
                    if (got[k] != e->hash)
                        break;
                if (k > COUNTS) {
                    printf(" ok\n");
                    continue;
                }
                printf(" FAIL\n");
                ++bad;
                for (k = 0; k <= COUNTS; ++k) {
                    int cut = (cut0 + k) % CUTS;
                    if (got[k] == e->hash)
                        continue;
                    if (k == COUNTS)
                        printf("    threads changing (1, 16, 2, 8, 3, 4), cut %s: %016llx\n",
                               cut_names[cut], (unsigned long long)got[k]);
                    else
                        printf("    %2d thread%s, cut %s: %016llx\n", thread_counts[k],
                               thread_counts[k] == 1 ? " " : "s", cut_names[cut],
                               (unsigned long long)got[k]);
                }
            }
    }
    if (record) {
        if (bad)
            printf("# FAIL: %d cases differ between thread counts\n", bad);
        return bad != 0 || api_t.failures != 0;
    }
    for (k = 0; k < nwant; ++k)
        if (!want[k].used) {
            printf("FAIL %s: %s %dx options %s is not a case\n", path, want[k].name,
                   want[k].scale, want[k].options);
            ++bad;
        }
#ifdef JB_TEST_SINGLE_THREADED
    printf("determinism_threads: single-threaded build: every thread count ran as one thread "
           "(%u requests for more were clamped and logged)\n",
           fewer.total);
    printf("determinism_threads: %d cases on one thread, %d failed; %d other checks failed\n",
           cases, bad, api_t.failures);
#else
    printf("determinism_threads: %d cases at %d thread counts and changing, %s; %d failed; "
           "%d other checks failed\n",
           cases, COUNTS,
           fewer.total == 0 ? "every count requested started" : "thread requests NOT met", bad,
           api_t.failures);
#endif
    return bad != 0 || api_t.failures != 0;
}

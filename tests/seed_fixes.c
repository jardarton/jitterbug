/*
 * The seed's small defects, each fixed without changing a pixel:
 *
 * (a) A VRAM-to-CPU command (C0h) cut off at the end of a packet made the
 *     decoder report more words consumed than it was given; it is now
 *     dropped like any other truncated trailing command.
 * (b) The software backend queued fills in a record it had only partly
 *     written; every byte of it is now set.
 * (c) A polyline whose second vertex word matched the terminator pattern
 *     drew nothing while the decoder still took the words up to a later
 *     terminator; the second vertex is now always a vertex, as the length
 *     rule (and psx-spx: at least two vertices) has it.
 * (d) With several rasterising threads, a primitive that samples texels it
 *     draws itself saw other workers' bands half drawn, so its pixels
 *     depended on the thread count; it is now drawn alone, in row order,
 *     as one thread draws it.
 * (e) With several threads, a primitive that writes texels a queued
 *     primitive samples could overwrite them before another worker's band
 *     of the earlier one read them; the queue is now drawn first. Found by
 *     the public API's random streams (api_stream.c).
 *
 * (f) Above 1x a precise vertex can sit just over a pixel from its integer
 *     position, and a scaled line is a 1x pixel thick, so a primitive could
 *     draw 1x rows and columns outside the bounds the decoder gave it. The
 *     workers and the tile bookkeeping go by those bounds: with several
 *     threads such pixels were not drawn, and a 1x tile they fell in was
 *     not refreshed from the scaled picture. The bounds now come from the
 *     precise positions too.
 *
 * The third, worker threads that were never joined, is context_lifecycle.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/context.h"
#include "soft/soft.h"

static int failures, checks;

static void check(const char *what, long long got, long long want)
{
    ++checks;
    if (got != want) {
        ++failures;
        printf("FAIL %s: got %lld, want %lld\n", what, got, want);
    }
}

/* (a) */
static void truncated_vram_to_cpu(jb_context *ctx)
{
    const uint32_t alone[2] = {0xc0000000u, 0x00100010u};
    const uint32_t after[3] = {0xe1000000u, 0xc0000000u, 0x00100010u};
    const uint32_t whole[4] = {0xe1000000u, 0xc0000000u, 0x00100010u, 0x00080008u};

    check("C0h with its size word missing: words consumed", jbi_gp0(ctx, alone, 2), 2);
    check("C0h with only its command word: words consumed", jbi_gp0(ctx, alone, 1), 1);
    check("C0h truncated after a draw mode: words consumed", jbi_gp0(ctx, after, 3), 3);
    check("C0h complete: words consumed", jbi_gp0(ctx, whole, 4), 4);
}

/* (b) */
static void fill_record(void)
{
    static const int args[][5] = {
        {0, 0, 16, 1, 0x0000}, {1008, 511, 16, 1, 0x7fff}, {512, 400, 1024, 200, 0x1234},
    };
    unsigned k;

    for (k = 0; k < sizeof args / sizeof args[0]; ++k) {
        jbi_prim a, b;
        char what[96];

        memset(&a, 0x00, sizeof a);
        memset(&b, 0xa5, sizeof b);
        jbi_soft_fill_record(&a, args[k][0], args[k][1], args[k][2], args[k][3],
                             (uint16_t)args[k][4]);
        jbi_soft_fill_record(&b, args[k][0], args[k][1], args[k][2], args[k][3],
                             (uint16_t)args[k][4]);
        snprintf(what, sizeof what, "fill record %u: bytes left from before", k);
        check(what, memcmp(&a, &b, sizeof a) != 0, 0);
        snprintf(what, sizeof what, "fill record %u: kind", k);
        check(what, a.kind, JBI_PRIM_FILL);
        snprintf(what, sizeof what, "fill record %u: color", k);
        check(what, a.u.fill.color, args[k][4]);
    }
}

static uint32_t xy(int x, int y)
{
    return ((uint32_t)x & 0xffff) | ((uint32_t)y & 0xffff) << 16;
}

/* (c) */
static void polyline_second_vertex(void)
{
    const uint32_t w[] = {
        0xe3000000u, 0xe4000000u | 1023u | 511u << 10, 0xe5000000u,
        0x4800ff00u, xy(2, 2), 0x50105010u /* (16, 16) */, xy(30, 4), 0x55555555u,
        0x6000ff00u, xy(100, 100), xy(4, 4), /* a rectangle after it is drawn */
    };
    jb_context *ctx = jbi_context_create(NULL);
    uint16_t px[3];

    jbi_reset_state(ctx);
    check("polyline: words consumed", jbi_gp0(ctx, w, (int)(sizeof w / sizeof w[0])),
          (long long)(sizeof w / sizeof w[0]));
    jbi_store(ctx, 2, 2, 1, 1, &px[0]);
    jbi_store(ctx, 30, 4, 1, 1, &px[1]);
    jbi_store(ctx, 101, 101, 1, 1, &px[2]);
    check("polyline: its first vertex is drawn", px[0], 0x03e0);
    check("polyline: its third vertex is drawn", px[1], 0x03e0);
    check("polyline: the command after it is drawn", px[2], 0x03e0);
    jbi_context_destroy(ctx);
}

/* The 1x VRAM's hash after w, on `threads` threads at `scale`. */
static uint64_t vram_after(const uint32_t *w, int n, int threads, int scale)
{
    static uint16_t v[JBI_VRAM_W * JBI_VRAM_H];
    jbi_config cfg;
    jb_context *ctx;
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;

    jbi_config_default(&cfg);
    cfg.threads = threads;
    cfg.scale = scale;
    cfg.exact_1x = 1;
    ctx = jbi_context_create(&cfg);
    jbi_reset_state(ctx);
    jbi_gp0(ctx, w, n);
    jbi_store_1x(ctx, 0, 0, 0, 0, v);
    jbi_context_destroy(ctx);
    for (i = 0; i < sizeof v / sizeof v[0]; ++i) {
        h ^= v[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/* (d), (e): each at 2 to 8 threads must equal 1 thread, at 1x and 2x. */
static void same_at_every_thread_count(const char *what, const uint32_t *w, int n)
{
    int scale, threads, k;
    char msg[128];

    for (scale = 1; scale <= 2; ++scale) {
        uint64_t one = vram_after(w, n, 1, scale);
        for (threads = 2; threads <= 8; threads += 3)
            for (k = 0; k < 3; ++k) {
                snprintf(msg, sizeof msg, "%s at %dx: %d threads differ from 1", what, scale,
                         threads);
                check(msg, vram_after(w, n, threads, scale) != one, 0);
            }
    }
}

/* (d) A raw textured rectangle whose page is its own destination: row r
   samples row r - 1, so one thread copies row 0 down the whole height. */
static void self_sampling(void)
{
    const uint32_t w[] = {
        0xe1000000u | 2u << 7, 0xe3000000u, 0xe4000000u | 1023u | 511u << 10, 0xe5000000u,
        0x02000000u, xy(0, 0), xy(256, 256),
        0x02ffffffu, xy(0, 0), xy(256, 1),
        0x65808080u, xy(0, 1), 0x00000000u, xy(64, 200),
    };
    same_at_every_thread_count("self-sampling rectangle", w, (int)(sizeof w / sizeof w[0]));
}

/* (e) A textured triangle samples a page that a fill queued after it
   overwrites. */
static void write_after_read(void)
{
    uint32_t w[64];
    int n = 0, i;

    w[n++] = 0xe1000000u | 2u << 7;
    w[n++] = 0xe3000000u;
    w[n++] = 0xe4000000u | 1023u | 511u << 10;
    w[n++] = 0xe5000000u;
    /* A striped page at (0, 0). */
    for (i = 0; i < 8; ++i) {
        w[n++] = 0x02000000u | (0x1f1f1fu * (uint32_t)(i + 1) & 0xffffffu);
        w[n++] = xy(0, i * 32);
        w[n++] = xy(256, 32);
    }
    /* A triangle over rows 100-400 that samples all of it. */
    w[n++] = 0x25808080u;
    w[n++] = xy(300, 100);
    w[n++] = 0x00000000u;
    w[n++] = xy(700, 110);
    w[n++] = ((2u << 7) << 16) | 0x00ffu;
    w[n++] = xy(320, 400);
    w[n++] = 0xff00u;
    /* Then the page overwritten. */
    w[n++] = 0x02ff0000u;
    w[n++] = xy(0, 0);
    w[n++] = xy(256, 256);
    same_at_every_thread_count("write after read", w, n);
}

/* (f) A precise source that moves every vertex by the same offset. */
typedef struct {
    int dx, dy; /* 1/JBI_PRECISE_ONE pixels */
} Shift;

static int shift_enabled(void *user)
{
    (void)user;
    return 1;
}

static int shift_lookup(void *user, uint32_t sxy, jbi_precise_vertex *v)
{
    const Shift *s = user;

    v->x = (int16_t)(sxy & 0xffff) * JBI_PRECISE_ONE + s->dx;
    v->y = (int16_t)(sxy >> 16) * JBI_PRECISE_ONE + s->dy;
    v->z = 0;
    return 1;
}

static unsigned shift_lookup_poly(void *user, const uint32_t *sxy, int n, jbi_precise_vertex *v)
{
    int i;

    for (i = 0; i < n; ++i)
        shift_lookup(user, sxy[i], &v[i]);
    return (1u << n) - 1;
}

/*
 * Draws the n words with every vertex moved by (dx, dy), the 1x picture
 * taken from the scaled one. Returns the scaled VRAM's hash on `threads`
 * threads, and in *wrong the number of 1x pixels (of the top left 320 x
 * 256) that disagree with their block of the scaled picture: black where
 * the block is lit, or lit where it is black.
 */
static uint64_t shifted(int scale, int threads, int dx, int dy, const uint32_t *words, int n,
                        int *wrong)
{
    static const uint32_t area[2] = {0xe3000000u, 0xe4000000u | 1023u | 511u << 10};
    Shift shift;
    jbi_precise_source src = {shift_enabled, shift_lookup, shift_lookup_poly, NULL};
    jbi_config cfg;
    jb_context *ctx;
    jbi_soft *soft;
    const uint16_t *v, *hv;
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i, count;
    int y, x, sx, sy;

    shift.dx = dx;
    shift.dy = dy;
    src.user = &shift;
    jbi_config_default(&cfg);
    cfg.threads = threads;
    cfg.scale = scale;
    cfg.exact_1x = 0;
    cfg.precise = &src;
    ctx = jbi_context_create(&cfg);
    jbi_reset_state(ctx);
    jbi_gp0(ctx, area, 2);
    jbi_gp0(ctx, words, n);
    soft = jbi_soft_from(jbi_context_backend(ctx));
    v = jbi_soft_vram(soft); /* flushes, and refreshes stale 1x tiles */
    hv = jbi_soft_scaled_raw(soft);
    *wrong = 0;
    for (y = 0; y < 256; ++y)
        for (x = 0; x < 320; ++x) {
            int sum = 0;

            for (sy = 0; sy < scale; ++sy)
                for (sx = 0; sx < scale; ++sx)
                    sum += hv[(size_t)(y * scale + sy) * JBI_VRAM_W * scale + x * scale + sx] & 31;
            if (sum >= scale * scale) /* an average of at least 1 */
                *wrong += v[y * JBI_VRAM_W + x] == 0;
            else if (sum == 0)
                *wrong += v[y * JBI_VRAM_W + x] != 0;
        }
    count = (size_t)JBI_VRAM_W * scale * JBI_VRAM_H * scale;
    for (i = 0; i < count; ++i) {
        h ^= hv[i];
        h *= 0x100000001b3ull;
    }
    jbi_context_destroy(ctx);
    return h;
}

static void precise_case(const char *what, int scale, int dx, int dy, const uint32_t *w, int n)
{
    char msg[200];
    int wrong, threads;
    uint64_t one = shifted(scale, 1, dx, dy, w, n, &wrong);

    snprintf(msg, sizeof msg, "%s moved (%d, %d)/256 at %dx, 1 thread: 1x pixels that disagree "
             "with the scaled picture", what, dx, dy, scale);
    check(msg, wrong, 0);
    for (threads = 2; threads <= 8; threads += 3) {
        snprintf(msg, sizeof msg, "%s moved (%d, %d)/256 at %dx: %d threads differ from 1", what,
                 dx, dy, scale, threads);
        check(msg, shifted(scale, threads, dx, dy, w, n, &wrong) != one, 0);
        snprintf(msg, sizeof msg, "%s moved (%d, %d)/256 at %dx, %d threads: 1x pixels that "
                 "disagree with the scaled picture", what, dx, dy, scale, threads);
        check(msg, wrong, 0);
    }
}

static void precise_bounds(void)
{
    static const int scales[] = {2, 3, 8};
    /* Just inside the decoder's slack of 1.125 pixels, each way. */
    static const int shifts[][2] = {
        {0, 288}, {0, -288}, {288, 0}, {-288, 0}, {288, 288}, {-288, -288}, {0, 0},
    };
    /* One primitive each, placed so that the moved edge crosses into a
       32 x 32 bookkeeping tile that its integer position does not reach. */
    static const uint32_t line_down[] = {0x40ffffffu, 50 | 30u << 16, 80 | 30u << 16};
    static const uint32_t line_right[] = {0x40ffffffu, 10 | 200u << 16, 30 | 200u << 16};
    static const uint32_t tri_up[] = {0x20ffffffu, 100 | 33u << 16, 160 | 33u << 16,
                                      100 | 40u << 16};
    static const uint32_t tri_left[] = {0x20ffffffu, 33 | 100u << 16, 33 | 160u << 16,
                                        40 | 100u << 16};
    uint32_t grid[16 * 7];
    unsigned k, j;
    int n = 0, y;

    /* Lines and flat triangles down rows 20 to 99, in steps that put some
       edges on the boundaries of the workers' 16-row bands. */
    for (y = 20; y < 100; y += 5) {
        int x0 = 25 + (y - 20) / 5 * 7;

        grid[n++] = 0x40ffffffu;
        grid[n++] = xy(x0, y);
        grid[n++] = xy(x0 + 37, y);
        grid[n++] = 0x20ffffffu;
        grid[n++] = xy(x0 + 100, y);
        grid[n++] = xy(x0 + 160, y);
        grid[n++] = xy(x0 + 100, y + 2);
    }
    for (k = 0; k < sizeof scales / sizeof scales[0]; ++k)
        for (j = 0; j < sizeof shifts / sizeof shifts[0]; ++j)
            precise_case("lines and triangles", scales[k], shifts[j][0], shifts[j][1], grid, n);
    precise_case("a line", 8, 0, 288, line_down, 3);
    precise_case("a line", 8, 288, 0, line_right, 3);
    precise_case("a triangle", 8, 0, -288, tri_up, 4);
    precise_case("a triangle", 8, -288, 0, tri_left, 4);
}

int main(void)
{
    jb_context *ctx = jbi_context_create(NULL);

    if (ctx == NULL)
        return 2;
    truncated_vram_to_cpu(ctx);
    fill_record();
    polyline_second_vertex();
    self_sampling();
    write_after_read();
    precise_bounds();
    jbi_context_destroy(ctx);
    printf("seed_fixes: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}

/*
 * fuzz_robust: crash fuzzing through the public API, seeded and
 * deterministic, with no reference to compare with (tools/fuzz-diff does
 * the differential half). It complements api_robust.c, whose stream is
 * mostly well-formed commands, with what that one does not do:
 *
 *   - any 32-bit word at all, and every command byte 00h-FFh with random
 *     parameters cut off after 0 to 20 words;
 *   - the largest transfers (GP0(A0h) and GP0(C0h) of 1024 x 512) cut
 *     short by other commands, GP1(01h) or GP1(00h), and GPUREAD past the
 *     end of the data;
 *   - the word stream split at random, down to one word per call;
 *   - between the words: jb_gpuread, jb_gpustat, jb_vram_read, _load,
 *     _fill and _copy (in range and out of it), jb_set_scale (1 to 4, and
 *     out of range), jb_set_threads (1 to 4), the two float options on
 *     and off, GTE commands (so that precise vertices have something to
 *     match), GP1 words and jb_frame;
 *   - one context starting at 1x on one thread, one at 3x on three;
 *   - a batch of 9000 small untextured primitives in one call, with no
 *     synchronisation point, on four threads and on one: more than the
 *     8192 the software backend queues, so that a full queue is drawn while
 *     the call goes on; the two VRAMs must be the same.
 *
 * The controlled cut-short cases (a command byte with too few words, the
 * largest transfers cut short) reset the command buffer (GP1(01h)) first,
 * so that they start a command rather than land in an earlier one's
 * parameters or pixel data.
 *
 * What it checks: nothing crashes, and under the sanitizer build nothing
 * is reported (no access outside any buffer, VRAM included, no undefined
 * behaviour, no leak); jb_gp0 and every call with valid arguments
 * succeeds, and calls with invalid ones fail without effect on later
 * calls; the same seed gives the same VRAM twice. Strict C99 against
 * <jitterbug.h>.
 *
 *   fuzz_robust [ACTIONS]   (default 3000 per context)
 */
#include "api_test.h"

static uint64_t fnv(uint64_t h, const unsigned char *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/* Words to jb_gp0, cut into random lengths (one word a call now and then). */
static void feed(jb_context *jb, gen *g, const uint32_t *w, size_t n)
{
    size_t at = 0;
    int words_only = rnd_below(g, 8) == 0;

    while (at < n) {
        size_t chunk = words_only ? 1 : 1 + (size_t)rnd_below(g, 64);
        if (chunk > n - at)
            chunk = n - at;
        check("jb_gp0", jb_gp0(jb, w + at, chunk), JB_OK);
        at += chunk;
    }
}

/* A rectangle for the jb_vram_* calls: in range mostly, sometimes not. */
static void rect_args(gen *g, int *x, int *y, int *w, int *h, int *valid)
{
    int bad = rnd_below(g, 8) == 0;

    *x = rnd_below(g, 1024);
    *y = rnd_below(g, 512);
    *w = 1 + rnd_below(g, 48);
    *h = 1 + rnd_below(g, 24);
    if (rnd_below(g, 16) == 0) {
        *w = 1024;
        *h = 1 + rnd_below(g, 512);
    }
    if (bad) {
        int which = rnd_below(g, 4);
        if (which == 0)
            *x = rnd_below(g, 2) ? -1 : 1024 + rnd_below(g, 100);
        else if (which == 1)
            *y = rnd_below(g, 2) ? -1 : 512 + rnd_below(g, 100);
        else if (which == 2)
            *w = rnd_below(g, 2) ? 0 : 1025;
        else
            *h = rnd_below(g, 2) ? -3 : 513;
    }
    *valid = !bad;
}

static uint64_t run(uint64_t seed, int scale0, int actions)
{
    jb_config c;
    jb_context *jb;
    gen g;
    uint32_t *w = must_alloc(300000 * 4);
    uint16_t *pix = must_alloc((size_t)VRAM_PIXELS * 2);
    uint16_t *vram;
    uint64_t h;
    int i, scale = scale0;

    memset(&c, 0, sizeof c);
    c.scale = scale0;
    c.threads = scale0 == 1 ? 1 : 3;
    jb = must_create(&c);
    g.s = seed;
    g.cx = 512;
    g.cy = 256;
    for (i = 0; i < actions; ++i) {
        int k = rnd_below(&g, 100), n = 0, j;

        if (k < 20) { /* any words */
            int count = 1 + rnd_below(&g, 200);
            for (j = 0; j < count; ++j)
                w[n++] = rnd(&g);
            feed(jb, &g, w, (size_t)n);
        } else if (k < 40) { /* every command byte, cut short */
            uint32_t op = (uint32_t)(i & 0xff);
            int count = rnd_below(&g, 21);
            gp1(jb, 0x01000000u); /* so that it starts a command */
            w[n++] = op << 24 | (rnd(&g) & 0xffffffu);
            for (j = 0; j < count; ++j)
                w[n++] = rnd_below(&g, 4) ? gen_xy(&g) : rnd(&g);
            feed(jb, &g, w, (size_t)n);
        } else if (k < 55) { /* well-formed commands (api_test.h) */
            n = gen_command(&g, w, rnd_below(&g, 2));
            feed(jb, &g, w, (size_t)n);
        } else if (k < 58) { /* the largest load, cut short */
            int count = rnd_below(&g, 2) ? rnd_below(&g, 3000) : rnd_below(&g, 262145);
            int end = rnd_below(&g, 3);
            gp1(jb, 0x01000000u);
            w[n++] = 0xa0000000u;
            w[n++] = rnd_below(&g, 4) ? 0 : rnd(&g);
            w[n++] = 0; /* 1024 x 512 */
            for (j = 0; j < count; ++j)
                w[n++] = rnd(&g);
            feed(jb, &g, w, (size_t)n);
            if (end == 0)
                gp1(jb, 0x01000000u);
            else if (end == 1)
                gp1(jb, 0x00000000u);
        } else if (k < 61) { /* the largest read, read in part or past its end */
            int count = rnd_below(&g, 2) ? rnd_below(&g, 2000) : 262144 + rnd_below(&g, 8);
            gp1(jb, 0x01000000u);
            w[0] = 0xc0000000u;
            w[1] = rnd_below(&g, 4) ? 0 : rnd(&g);
            w[2] = 0;
            feed(jb, &g, w, 3);
            for (j = 0; j < count; ++j)
                (void)jb_gpuread(jb);
            if (rnd_below(&g, 2))
                gp1(jb, 0x01000000u);
        } else if (k < 66) {
            int count = rnd_below(&g, 64);
            for (j = 0; j < count; ++j)
                (void)jb_gpuread(jb);
            (void)jb_gpustat(jb);
        } else if (k < 72) { /* the VRAM calls */
            int x, y, rw, rh, valid, op = rnd_below(&g, 4);
            jb_result r = JB_OK, want;
            rect_args(&g, &x, &y, &rw, &rh, &valid);
            want = valid ? JB_OK : JB_ERROR_INVALID_ARGUMENT;
            if (op == 0) {
                r = jb_vram_read(jb, x, y, rw, rh, pix);
            } else if (op == 1) {
                for (j = 0; j < rw * rh && j < VRAM_PIXELS; ++j)
                    pix[j] = (uint16_t)rnd(&g);
                r = jb_vram_load(jb, x, y, rw, rh, pix);
            } else if (op == 2) {
                /* fill's own ranges: w 0..1023, h 0..511 */
                int fw = rw > 1023 ? 1023 : rw, fh = rh > 511 ? 511 : rh;
                if (valid)
                    r = jb_vram_fill(jb, x, y, fw, fh, rnd(&g) & 0xffffffu);
                else
                    r = jb_vram_fill(jb, x, y, rw > 1023 ? 1024 : -1, fh, 0);
            } else {
                int dx = rnd_below(&g, 1024), dy = rnd_below(&g, 512);
                r = jb_vram_copy(jb, x, y, dx, dy, rw, rh);
            }
            check("a VRAM call's result", r, want);
        } else if (k < 76) { /* scale, threads, options */
            int what = rnd_below(&g, 5);
            if (what == 0) {
                int s = 1 + rnd_below(&g, 4);
                if (rnd_below(&g, 8) == 0) {
                    check("jb_set_scale out of range", jb_set_scale(jb, -1),
                          JB_ERROR_INVALID_ARGUMENT);
                    check("jb_set_scale above 8", jb_set_scale(jb, 9), JB_ERROR_UNSUPPORTED);
                } else {
                    check("jb_set_scale", jb_set_scale(jb, s), JB_OK);
                    scale = s;
                }
            } else if (what == 1) {
                check("jb_set_threads", jb_set_threads(jb, 1 + rnd_below(&g, 4)), JB_OK);
            } else if (what == 2) {
                check("jb_set_precise_vertices", jb_set_precise_vertices(jb, rnd_below(&g, 2)),
                      JB_OK);
            } else if (what == 3) {
                check("jb_set_perspective_correct",
                      jb_set_perspective_correct(jb, rnd_below(&g, 2)), JB_OK);
            } else {
                int fw = 0, fh = 0;
                const uint8_t *f = jb_frame(jb, &fw, &fh);
                check("jb_frame", f != NULL && fw > 0 && fh > 0 && fw % scale == 0, 1);
            }
        } else if (k < 82) { /* the GTE: projections the options may match */
            int r;
            for (r = 0; r < 4; ++r) {
                int reg = rnd_below(&g, 64);
                uint32_t value = rnd(&g);
                (void)jb_gte_write(jb, reg, value);
            }
            (void)jb_gte_write(jb, JB_GTE_H, 0x100 + (uint32_t)rnd_below(&g, 0x400));
            (void)jb_gte_command(jb, rnd_below(&g, 2) ? JB_GTE_RTPT : rnd(&g) & 0x1ffffffu);
            if (rnd_below(&g, 8) == 0)
                (void)jb_gte_begin_frame(jb);
        } else if (k < 90) { /* GP1 */
            uint32_t v;
            if (rnd_below(&g, 4)) {
                uint32_t cmd = (uint32_t)rnd_below(&g, 0x12);
                uint32_t arg = rnd(&g) & 0xffffffu;
                v = cmd << 24 | arg;
            } else {
                v = rnd(&g);
            }
            gp1(jb, v);
        } else { /* a drawing area somewhere, so that drawing lands */
            int x0 = rnd_below(&g, 1024), y0 = rnd_below(&g, 512);
            uint32_t x1 = (uint32_t)rnd_below(&g, 1024);
            uint32_t y1 = (uint32_t)rnd_below(&g, 512);
            w[0] = 0xe3000000u | (uint32_t)x0 | (uint32_t)y0 << 10;
            w[1] = 0xe4000000u | x1 | y1 << 10;
            w[2] = 0xe5000000u | (rnd(&g) & 0x3fffffu);
            feed(jb, &g, w, 3);
            g.cx = x0;
            g.cy = y0;
        }
    }
    gp1(jb, 0x01000000u);
    vram = read_vram(jb);
    h = fnv(0xcbf29ce484222325ull, (const unsigned char *)vram, (size_t)VRAM_PIXELS * 2);
    free(vram);
    free(pix);
    free(w);
    jb_destroy(jb);
    return h;
}

/* The batch: 9000 small flat triangles and rectangles, untextured and
   opaque (nothing they draw is read, so nothing makes the backend draw its
   queue early), in one jb_gp0 call, on `threads` threads. Returns the
   VRAM's hash. */
static uint64_t batch(int threads)
{
    enum { PRIMS = 9000 };
    jb_config c;
    jb_context *jb;
    uint32_t *w = must_alloc((size_t)PRIMS * 4 * 4 + 16);
    uint16_t *vram;
    uint64_t h;
    size_t n = 0;
    int i;
    gen g;

    memset(&c, 0, sizeof c);
    c.scale = 1;
    c.threads = threads;
    jb = must_create(&c);
    g.s = 0xba7c4u;
    g.cx = 512;
    g.cy = 256;
    w[n++] = 0xe3000000u;
    w[n++] = 0xe4000000u | 1023u | 511u << 10;
    w[n++] = 0xe5000000u;
    w[n++] = 0xe6000000u;
    for (i = 0; i < PRIMS; ++i) {
        int x = rnd_below(&g, 1000), y = rnd_below(&g, 500);
        uint32_t rgb = rnd(&g) & 0xffffffu;
        if (i % 2) {
            w[n++] = 0x78000000u | rgb; /* 16 x 16 */
            w[n++] = xy(x, y);
        } else {
            w[n++] = 0x20000000u | rgb;
            w[n++] = xy(x, y);
            w[n++] = xy(x + 12, y + 3);
            w[n++] = xy(x + 4, y + 10);
        }
    }
    check("the batch", jb_gp0(jb, w, n), JB_OK);
    vram = read_vram(jb);
    h = fnv(0xcbf29ce484222325ull, (const unsigned char *)vram, (size_t)VRAM_PIXELS * 2);
    free(vram);
    free(w);
    jb_destroy(jb);
    return h;
}

int main(int argc, char **argv)
{
    int actions = argc > 1 ? atoi(argv[1]) : 3000, i;
    static const int scales[2] = {1, 3};

    for (i = 0; i < 2; ++i) {
        uint64_t seed = 0xf0225eedu + (uint64_t)i;
        uint64_t a = run(seed, scales[i], actions);
        uint64_t b = run(seed, scales[i], actions);
        printf("  from %dx: %d actions, VRAM %016llx\n", scales[i], actions,
               (unsigned long long)a);
        check("the same seed gives the same VRAM", a == b, 1);
    }
    {
        uint64_t one = batch(1), four = batch(4);
        printf("  a batch of 9000 primitives: VRAM %016llx\n", (unsigned long long)four);
        check("the batch on four threads gives one thread's VRAM", one == four, 1);
    }
    return finish("fuzz_robust");
}

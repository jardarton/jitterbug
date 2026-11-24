/*
 * jb_gp0 is a word stream: the same words give the same result however
 * they are split across calls.
 *
 * (a) A fixed list covering every command class (fills, every polygon,
 *     rectangle and line opcode, polylines, loads with odd pixel counts, a
 *     copy, a VRAM-to-CPU read, drawing state, interrupt and no-op
 *     commands) is fed to a public context whole, one word at a time, and
 *     in chunks of random sizes from seeded generators. The data of the
 *     list's read (GPUREAD), the whole VRAM read back (GP0(C0h) and
 *     GPUREAD), GPUSTAT and jb_frame (a 1024 x 512 display, so the frame
 *     is all of VRAM at the context's scale) must be identical in every
 *     split.
 * (b) Deliberately against the internals: the same words given in one
 *     call to the core decoder (jbi_gp0, src/core/context.h, which takes
 *     complete commands and decodes them itself) on a context configured
 *     as jb_create configures one. The VRAM and the frame (jbi_scanout,
 *     widened as jb_frame does) must equal the public context's. This
 *     checks the public accumulator's command lengths, polyline ends and
 *     load streaming against an independent decoder.
 * (c) The same for a seeded random stream of commands, biased towards
 *     complete valid ones over every opcode, split in random chunks.
 *
 * Each runs at 1x and 2x.
 */
#include "api_test.h"

#include "core/context.h"

/* ------------------------------------------------------------------------ */
/* The fixed list */

enum { LIST_MAX = 4096 };

static int put(uint32_t *w, int n, uint32_t v)
{
    w[n] = v;
    return n + 1;
}

/* A 15-bit texture page at x 512 and a 4-bit one at x 576 with its CLUT
   at (0, 480). */
#define PAGE15 (8u | 2u << 7)
#define PAGE4 (9u)
#define CLUT4 (0u | 480u << 6)

static int list_build(uint32_t *w)
{
    int n = 0, i, k;

    n = put(w, n, 0xe1000000u | PAGE15 | 1u << 9 | 1u << 10);
    n = put(w, n, 0xe2000000u);
    n = put(w, n, 0xe3000000u);
    n = put(w, n, 0xe4000000u | 1023u | 511u << 10);
    n = put(w, n, 0xe5000000u);
    n = put(w, n, 0xe6000000u);
    /* Fill. */
    n = put(w, n, 0x02203040u);
    n = put(w, n, xy(0, 0));
    n = put(w, n, xy(640, 480));
    /* A 7 x 5 15-bit texture (35 pixels: the last word padded). */
    n = put(w, n, 0xa0000000u);
    n = put(w, n, xy(512, 0));
    n = put(w, n, xy(7, 5));
    for (i = 0; i < 18; ++i)
        n = put(w, n, (uint32_t)(i * 0x0421 + 0x1234) | (uint32_t)(0x7fff - i * 0x0843) << 16);
    /* A CLUT and a 4-bit texture of 16 x 8 texels (4 x 8 pixels). */
    n = put(w, n, 0xa0000000u);
    n = put(w, n, xy(0, 480));
    n = put(w, n, xy(16, 1));
    for (i = 0; i < 8; ++i)
        n = put(w, n, (uint32_t)(i * 0x0c63 + 0x0010) | (uint32_t)(i * 0x1084 + 0x8001) << 16);
    n = put(w, n, 0xa0000000u);
    n = put(w, n, xy(576, 0));
    n = put(w, n, xy(4, 8));
    for (i = 0; i < 16; ++i)
        n = put(w, n, 0x76543210u + (uint32_t)i * 0x11111111u);

    /* Every polygon opcode, each in a cell of its own. */
    for (k = 0; k < 32; ++k) {
        uint32_t op = 0x20u + (uint32_t)k;
        int verts = (op & 0x08) ? 4 : 3, gouraud = (op & 0x10) != 0, tex = (op & 0x04) != 0;
        int cx = (k % 8) * 64 + 8, cy = (k / 8) * 56 + 8;
        static const int vx[4] = {0, 44, 4, 46}, vy[4] = {0, 3, 42, 40};
        uint32_t page = (k & 1) ? PAGE4 : PAGE15;

        n = put(w, n, op << 24 | (0x102030u * (uint32_t)(k + 1) & 0xffffffu));
        for (i = 0; i < verts; ++i) {
            if (gouraud && i > 0)
                n = put(w, n, 0x10ffu * (uint32_t)(i + k) & 0xffffffu);
            n = put(w, n, xy(cx + vx[i], cy + vy[i]));
            if (tex) {
                uint32_t uv = (uint32_t)(vx[i] / 6) | (uint32_t)(vy[i] / 6) << 8;
                if (i == 0)
                    uv |= ((k & 1) ? CLUT4 : 0u) << 16;
                if (i == 1)
                    uv |= (page | (uint32_t)(k & 3) << 5) << 16;
                n = put(w, n, uv);
            }
        }
    }
    /* Back to the 15-bit page for the rectangles. */
    n = put(w, n, 0xe1000000u | PAGE15 | 1u << 10);
    /* Every rectangle opcode. */
    for (k = 0; k < 32; ++k) {
        uint32_t op = 0x60u + (uint32_t)k;
        int x = (k % 16) * 40 + 4, y = 240 + (k / 16) * 30;

        n = put(w, n, op << 24 | (0x0a1b2cu * (uint32_t)(k + 3) & 0xffffffu));
        n = put(w, n, xy(x, y));
        if (op & 0x04)
            n = put(w, n, (uint32_t)(k & 3) | (uint32_t)(k & 1) << 8);
        if (((op >> 3) & 3) == 0)
            n = put(w, n, xy(20 + k % 5, 12 + k % 3));
    }
    /* Single lines. */
    for (k = 0; k < 32; ++k) {
        uint32_t op = 0x40u + (uint32_t)k;
        if (op & 0x08)
            continue;
        n = put(w, n, op << 24 | 0x40c080u);
        n = put(w, n, xy(4 + k * 18, 310));
        if (op & 0x10)
            n = put(w, n, 0x2080f0u);
        n = put(w, n, xy(12 + k * 18, 345 - k));
    }
    /* Polylines, flat and Gouraud, with each kind of terminator. */
    for (k = 0; k < 4; ++k) {
        uint32_t op = 0x48u | (k & 1 ? 0x10u : 0u) | (k & 2 ? 0x02u : 0u);
        n = put(w, n, op << 24 | 0xff8040u);
        for (i = 0; i < 5; ++i) {
            if (i > 0 && (op & 0x10))
                n = put(w, n, 0x204060u * (uint32_t)(i + 1) & 0xffffffu);
            n = put(w, n, xy(20 + k * 150 + i * 25, 360 + (i & 1) * 30 + k * 3));
        }
        n = put(w, n, k & 1 ? 0x55555555u : 0x5a3c5f00u);
    }
    /* A polyline whose second vertex matches the terminator pattern: it is
       a vertex (a polyline has at least two), at (16, 16) + offset. */
    n = put(w, n, 0xe5000000u | 600u | 400u << 11);
    n = put(w, n, 0x4800ff00u);
    n = put(w, n, xy(2, 2));
    n = put(w, n, 0x50105010u);
    n = put(w, n, xy(30, 4));
    n = put(w, n, 0x55555555u);
    n = put(w, n, 0xe5000000u);
    /* Texture window and drawing offset under a textured rectangle. */
    n = put(w, n, 0xe2000000u | 0x1fu | 0x1fu << 5 | 0x02u << 10 | 0x01u << 15);
    n = put(w, n, 0xe5000000u | 0x7f0u | 0x7f8u << 11); /* (-16, -8) */
    n = put(w, n, 0x64808080u);
    n = put(w, n, xy(700, 20));
    n = put(w, n, 0x00000000u);
    n = put(w, n, xy(40, 40));
    n = put(w, n, 0xe2000000u);
    n = put(w, n, 0xe5000000u);
    /* Copy. */
    n = put(w, n, 0x80000000u);
    n = put(w, n, xy(0, 0));
    n = put(w, n, xy(700, 300));
    n = put(w, n, xy(64, 64));
    /* A read of 5 x 3 (15 pixels), then a fill over it: the read gives the
       pixels as they were. */
    n = put(w, n, 0xc0000000u);
    n = put(w, n, xy(8, 8));
    n = put(w, n, xy(5, 3));
    n = put(w, n, 0x02ffffffu);
    n = put(w, n, xy(0, 0));
    n = put(w, n, xy(16, 16));
    /* Mask bits: set, then check. */
    n = put(w, n, 0xe6000001u);
    n = put(w, n, 0x60ff0000u);
    n = put(w, n, xy(800, 100));
    n = put(w, n, xy(30, 30));
    n = put(w, n, 0xe6000002u);
    n = put(w, n, 0x6000ff00u);
    n = put(w, n, xy(815, 115));
    n = put(w, n, xy(30, 30));
    n = put(w, n, 0xe6000000u);
    /* Interrupt, no-op and unknown commands. */
    n = put(w, n, 0x1f000000u);
    n = put(w, n, 0x00123456u);
    n = put(w, n, 0x01000000u);
    n = put(w, n, 0x03000000u);
    n = put(w, n, 0xe0abcdefu);
    n = put(w, n, 0xe7000000u);
    n = put(w, n, 0xff000000u);
    /* A last triangle, so the stream ends with a command. */
    n = put(w, n, 0x30ff0000u);
    n = put(w, n, xy(900, 400));
    n = put(w, n, 0x0000ff00u);
    n = put(w, n, xy(1000, 420));
    n = put(w, n, 0x000000ffu);
    n = put(w, n, xy(940, 500));
    return n;
}

/* ------------------------------------------------------------------------ */
/* Feeding and results */

enum { WHOLE, WORDS, CHUNKS };

static void feed(jb_context *jb, const uint32_t *w, int n, int how, uint64_t seed)
{
    gen g = {seed, 0, 0};
    int i = 0;

    if (how == WHOLE) {
        gp0(jb, w, (size_t)n);
        return;
    }
    while (i < n) {
        int k = how == WORDS ? 1 : 1 + rnd_below(&g, 40);
        if (k > n - i)
            k = n - i;
        gp0(jb, w + i, (size_t)k);
        i += k;
    }
}

typedef struct {
    uint16_t read[16];
    uint16_t *vram;
    uint8_t *frame;
    int fw, fh;
    uint32_t stat;
} Result;

static void result_free(Result *r)
{
    free(r->vram);
    free(r->frame);
}

/* What a public context shows after the words: pending read data first. */
static void collect(jb_context *jb, Result *r, int pending)
{
    const uint8_t *f;
    int i;

    memset(r->read, 0, sizeof r->read);
    r->stat = jb_gpustat(jb);
    for (i = 0; i < pending; i += 2) {
        uint32_t v = jb_gpuread(jb);
        r->read[i] = (uint16_t)v;
        r->read[i + 1] = (uint16_t)(v >> 16);
    }
    r->vram = read_vram(jb);
    gp1(jb, 0x03000000u);
    f = jb_frame(jb, &r->fw, &r->fh);
    r->frame = must_alloc((size_t)r->fw * r->fh * 4);
    if (f != NULL)
        memcpy(r->frame, f, (size_t)r->fw * r->fh * 4);
}

static void compare(const char *what, const Result *a, const Result *b)
{
    char msg[160];
    size_t i, bad = 0;

    snprintf(msg, sizeof msg, "%s: GPUSTAT", what);
    check(msg, b->stat, a->stat);
    snprintf(msg, sizeof msg, "%s: read data differs", what);
    check(msg, memcmp(a->read, b->read, sizeof a->read) != 0, 0);
    for (i = 0; i < VRAM_PIXELS; ++i)
        bad += a->vram[i] != b->vram[i];
    snprintf(msg, sizeof msg, "%s: VRAM pixels that differ", what);
    check(msg, (long long)bad, 0);
    snprintf(msg, sizeof msg, "%s: frame size", what);
    check(msg, b->fw * 10000 + b->fh, a->fw * 10000 + a->fh);
    if (a->fw == b->fw && a->fh == b->fh) {
        snprintf(msg, sizeof msg, "%s: frame bytes differ", what);
        check(msg, memcmp(a->frame, b->frame, (size_t)a->fw * a->fh * 4) != 0, 0);
    }
}

/* The internal decoder's VRAM and frame for the same words, in one call. */
static void reference(int scale, const uint32_t *w, int n, Result *r)
{
    jbi_config cfg;
    jb_context *ctx;
    uint8_t *buf;
    int fw, fh, stride, fmt;
    size_t i;

    jbi_config_default(&cfg);
    cfg.scale = scale;
    cfg.exact_1x = 1;
    ctx = jbi_context_create(&cfg);
    if (ctx == NULL)
        exit(2);
    jbi_reset_state(ctx);
    check("jbi_gp0 consumed every word", jbi_gp0(ctx, w, n), n);
    memset(r, 0, sizeof *r);
    r->vram = must_alloc((size_t)VRAM_PIXELS * 2);
    jbi_store_1x(ctx, 0, 0, 0, 0, r->vram);
    buf = must_alloc(jbi_scanout_bytes(ctx));
    fmt = jbi_scanout(ctx, 0, 0, VRAM_W, VRAM_H, 0, 1, buf, &fw, &fh, &stride);
    check("reference scanout format", fmt, JBI_SCANOUT_BGR555);
    r->fw = fw;
    r->fh = fh;
    r->frame = must_alloc((size_t)fw * fh * 4);
    for (i = 0; i < (size_t)fw * fh; ++i) {
        unsigned p = buf[2 * i] | (unsigned)buf[2 * i + 1] << 8;
        r->frame[4 * i] = (uint8_t)widen5(p & 31);
        r->frame[4 * i + 1] = (uint8_t)widen5((p >> 5) & 31);
        r->frame[4 * i + 2] = (uint8_t)widen5((p >> 10) & 31);
        r->frame[4 * i + 3] = 255;
    }
    free(buf);
    jbi_context_destroy(ctx);
}

static jb_context *display_context(int scale)
{
    jb_config cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.scale = scale;
    cfg.width = VRAM_W;
    cfg.height = VRAM_H;
    cfg.threads = 1 + scale; /* the pixels must not depend on it */
    return must_create(&cfg);
}

static void fixed_list(int scale)
{
    static uint32_t w[LIST_MAX];
    int n = list_build(w), k;
    Result base, other, ref;
    char what[96];
    jb_context *jb;

    if (n > LIST_MAX)
        exit(2);
    jb = display_context(scale);
    feed(jb, w, n, WHOLE, 0);
    check("GPUSTAT bit 27 (read data waiting)", (jb_gpustat(jb) >> 27) & 1, 1);
    check("GPUSTAT bit 24 (GP0(1Fh))", (jb_gpustat(jb) >> 24) & 1, 1);
    collect(jb, &base, 15);
    jb_destroy(jb);
    /* The read saw the rectangle before the fill over it. */
    check("read data is from before the later fill", base.read[0] != 0x7fff, 1);
    check("the read's padding", base.read[15], 0);

    for (k = 0; k < 5; ++k) {
        int how = k == 0 ? WORDS : CHUNKS;
        jb = display_context(scale);
        feed(jb, w, n, how, 0x5eed0000u + (uint64_t)k * 7919);
        collect(jb, &other, 15);
        jb_destroy(jb);
        snprintf(what, sizeof what, "fixed list at %dx, %s (seed %d)", scale,
                 how == WORDS ? "word by word" : "random chunks", k);
        compare(what, &base, &other);
        result_free(&other);
    }
    reference(scale, w, n, &ref);
    memcpy(ref.read, base.read, sizeof ref.read);
    ref.stat = base.stat;
    snprintf(what, sizeof what, "fixed list at %dx against the core decoder", scale);
    compare(what, &base, &ref);
    result_free(&ref);
    result_free(&base);
}

static void random_stream(int scale, uint64_t seed, int words)
{
    uint32_t *w = must_alloc(((size_t)words + GEN_MAX) * 4);
    gen g = {seed, 512, 256};
    int n = 0;
    Result pub, ref;
    char what[96];
    jb_context *jb;

    while (n < words)
        n += gen_command(&g, w + n, 1);
    jb = display_context(scale);
    feed(jb, w, n, CHUNKS, seed ^ 0xabcdef);
    collect(jb, &pub, 0);
    jb_destroy(jb);
    reference(scale, w, n, &ref);
    ref.stat = pub.stat;
    snprintf(what, sizeof what, "random stream %llx at %dx against the core decoder",
             (unsigned long long)seed, scale);
    compare(what, &pub, &ref);
    result_free(&ref);
    result_free(&pub);
    free(w);
}

int main(void)
{
    fixed_list(1);
    fixed_list(2);
    random_stream(1, 0x1234u, 60000);
    random_stream(1, 0x9876u, 60000);
    random_stream(2, 0x4242u, 40000);
    return finish("api_stream");
}

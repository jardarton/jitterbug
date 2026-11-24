/*
 * api_golden: the seed's recorded scenes (tests/seed, FORMAT.md) replayed
 * through the public API alone, each scene on a fresh context from its
 * prelude ("Running a scene alone"), checked against what the recording
 * holds wherever the public API can produce it.
 *
 * The ops become public calls:
 *   GP0           jb_gp0 with the packet's words, in one call
 *   FILL          GP0(02h);  LOAD  GP0(A0h) and the pixels;  COPY  GP0(80h)
 *   STORE         GP0(C0h) and jb_gpuread; the pixels are compared
 *   RESET_STATE   GP0(E1h) to GP0(E6h), each with 0 (what the reset sets)
 *   POKE          a GP0(A0h) of the whole VRAM with GP0(E6h) 0 first and the
 *                 mask settings put back after, from GPUSTAT bits 11 and 12
 *   CAPTURE       the whole VRAM read back (GP0(C0h), jb_gpuread), its hash
 *                 compared with the capture's 1x hash; at the scene's end
 *                 also against the 1x golden image, pixel for pixel
 *   PRESENT       GP1(05h) and GP1(03h), then jb_frame (below)
 *   the rest      nothing: FLUSH, SYNC, TRACE_*, VRAM_GET, HIRES_REPLICATE,
 *                 LOWRES_* and the PRECISE_* ops act only on the scaled VRAM,
 *                 on queue timing, or on stale 1x tiles, none of which exists
 *                 or shows publicly with exact 1x on; PRECISE_STATS
 *                 (counters of the fake precise table) cannot be compared.
 *
 * The context: jb_create with the highest scale the scene uses (its
 * prelude's or a CONFIGURE's; --scale S makes every scale above 1x S), the
 * prelude's thread count, and the display size of the scene's PRESENT ops
 * (512 x 256 here). A CONFIGURE is followed only in the bookkeeping below,
 * not replayed with jb_set_scale; that is enough because the 1x VRAM of a
 * public context is the console's whatever the scale (exact 1x).
 *
 * What the recording's 1x hashes say depends on how the seed drew at 1x
 * then. Its 1x VRAM was the console's (and so must equal ours) except
 * after drawing with exact 1x off above 1x (primitives went to the scaled
 * VRAM only), with perspective-correct texturing at 1x, or with dithering
 * off at 1x. A capture or store after such drawing, until a POKE rewrote
 * all of VRAM, is still compared but only reported ("unexact"): it is not
 * a failure either way.
 *
 * PRESENT: a frame of the display size in 15-bit mode at the context's
 * scale. Its expected hash is over the 15-bit pixels with their mask bits,
 * which jb_frame does not show:
 *   - at 1x the frame must be the display area read back (bit-replicated),
 *     and the area read back must hash as the recording's frame;
 *   - above 1x the frame, narrowed back to 15 bits with no mask bit, must
 *     hash as the recording's frame, which holds where the shown pixels have
 *     no mask bit (they have none in these scenes).
 * A 24-bit PRESENT is not compared: 24-bit display is set only by GP1(08h),
 * whose sizes are the console's (512 x 240 for these 512 x 256 areas).
 * Nor is one above 1x whose area holds masked pixels (seen in the 1x VRAM
 * read back): the scaled pixels' mask bits are in the hash and nowhere
 * public.
 *
 * Frames against the golden images: at the scene's end, frames tiled over
 * all of VRAM (moving the display start with GP1(05h)) are compared, pixel
 * by pixel on the 15 colour bits, with the golden image of the context's
 * scale: NN-name.1x.png at 1x, NN-name.2x.png at 2x, and under --scale S
 * scaled/NN-name.Sx.png where one exists. Above 1x that needs the
 * recording's scaled VRAM to be what a public context draws, which it is
 * not after drawing at 2x with what the public API cannot set up the same
 * way (the fake precise table enabled: a public context's table holds only
 * its own GTE's projections; perspective-correct texturing from that fake
 * table; dithering above 1x; exact 1x off), after a CONFIGURE or
 * HIRES_REPLICATE rebuilt it
 * from the 1x VRAM over drawn detail, or while a POKE had not been copied
 * into it; such a comparison is reported only, as above.
 *
 *   api_golden [--scale S] TRACE.jbt...
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_test.h"
#include "trace.h"

typedef struct {
    unsigned compared, matched;
} Tally;

static struct {
    Tally capture, capture_unexact, store, store_unexact, present, golden, golden_unexact, gp0;
    Tally frame_golden, frame_golden_unexact;
    unsigned capture_not_alone, present_24bit, present_other, present_masked, stats;
    unsigned scale_changes, scenes, goldens_not_alone, frame_goldens_none;
} total;

static int override_scale; /* --scale */

typedef struct {
    trace_reader r;
    char name[96];
    unsigned op, code;
    int failures;
    jb_context *jb;
    int scale, disp_w, disp_h;
    /* The recording's state, followed. */
    int t_scale, t_dither, t_exact, t_texmap;
    int unexact; /* the recorded 1x VRAM may not be the console's */
    /* The recorded scaled VRAM: may differ from a public context's; a
       POKE not yet copied into it; primitives drawn since the last POKE;
       the fake precise table enabled. */
    int scaled_unexact, poke_pending, drawn, precise_on;
    int end_seen, end_alone;
} Scene;

static int map_scale(int s)
{
    return override_scale > 0 && s > 1 ? override_scale : s;
}

static void fail(Scene *s, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#include <stdarg.h>

static void fail(Scene *s, const char *fmt, ...)
{
    va_list ap;

    if (++s->failures > 30)
        return;
    printf("FAIL %s op %u (%s): ", s->name, s->op, trace_op_name(s->code));
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static int tally(Tally *t, int ok)
{
    ++t->compared;
    t->matched += ok != 0;
    return ok;
}

/* Whether the seed's 1x pass drew as the console does in this state. */
static int drawing_exact(const Scene *s)
{
    if (s->t_scale > 1)
        return s->t_exact;
    return !s->t_texmap && s->t_dither != 0;
}

/* Whether the seed's scaled pass drew as a public context's does. */
static int scaled_drawing_same(const Scene *s)
{
    return s->t_scale <= 1 || (s->t_exact && !s->t_texmap && s->t_dither <= 0 && !s->precise_on);
}

/* Bookkeeping for the scaled VRAM, before an op that writes VRAM. */
static void scaled_write(Scene *s, int primitives)
{
    if (s->poke_pending)
        s->scaled_unexact = 1;
    if (primitives) {
        s->drawn = 1;
        if (!scaled_drawing_same(s))
            s->scaled_unexact = 1;
    }
}

static void words(Scene *s, const uint32_t *w, size_t n)
{
    if (!tally(&total.gp0, jb_gp0(s->jb, w, n) == JB_OK))
        fail(s, "jb_gp0: %s", jb_error_string(s->jb));
}

static void load(Scene *s, int x, int y, int w, int h, const uint16_t *pix, size_t n)
{
    uint32_t cmd[3], *data = must_alloc((n + 1) / 2 * 4 + 4);
    size_t i;

    cmd[0] = 0xa0000000u;
    cmd[1] = xy(x, y);
    cmd[2] = xy(w, h);
    for (i = 0; i < n; i += 2)
        data[i / 2] = pix[i] | (i + 1 < n ? (uint32_t)pix[i + 1] << 16 : 0);
    words(s, cmd, 3);
    words(s, data, (n + 1) / 2);
    free(data);
}

static uint64_t vram_hash(Scene *s, uint16_t **keep)
{
    uint16_t *v = read_vram(s->jb);
    uint64_t h = trace_fnv_pixels(v, VRAM_PIXELS);

    if (keep != NULL)
        *keep = v;
    else
        free(v);
    return h;
}

static void op_capture(Scene *s)
{
    trace_reader *r = &s->r;
    unsigned kind = trace_u32(r), alone = trace_u32(r);
    uint64_t h1;
    int ok;

    (void)trace_u32(r); /* gpu_scale, scaled width and height: the scaled VRAM */
    (void)trace_u32(r);
    (void)trace_u32(r);
    h1 = trace_u64(r);
    (void)trace_u64(r);
    if (kind == 5) {
        s->end_seen = 1;
        s->end_alone = (int)alone;
    }
    if (!alone) {
        ++total.capture_not_alone;
        return;
    }
    ok = vram_hash(s, NULL) == h1;
    if (s->unexact) {
        tally(&total.capture_unexact, ok);
        return;
    }
    if (!tally(&total.capture, ok))
        fail(s, "capture kind %u: the VRAM read back does not hash as the recording's 1x VRAM",
             kind);
}

static void op_store(Scene *s)
{
    trace_reader *r = &s->r;
    int x = trace_i32(r), y = trace_i32(r), w = trace_i32(r), h = trace_i32(r);
    uint32_t n = trace_u32(r), i, bad = 0;
    uint16_t *want = trace_u16s(r, n), *got = must_alloc((size_t)n * 2 + 2);

    if (n != (uint32_t)xfer_w(w) * (uint32_t)xfer_h(h))
        trace_die(r, "STORE pixel count disagrees with its size");
    read_rect(s->jb, x, y, w, h, got);
    for (i = 0; i < n; ++i)
        bad += got[i] != want[i];
    if (s->unexact)
        tally(&total.store_unexact, bad == 0);
    else if (!tally(&total.store, bad == 0))
        fail(s, "store (%d, %d) %d x %d: %u of %u pixels differ", x, y, w, h, bad, n);
    free(want);
    free(got);
}

static void op_present(Scene *s)
{
    trace_reader *r = &s->r;
    int a[8], want[4], i, w = 512, h = 256, rgb = 0, n, fw = 0, fh = 0, ok;
    uint64_t want_hash, got_hash;
    const uint8_t *f;
    unsigned char *raw;
    size_t px, k;

    for (i = 0; i < 8; ++i)
        a[i] = trace_i32(r);
    for (i = 0; i < 4; ++i)
        want[i] = trace_i32(r);
    want_hash = trace_u64(r);
    if (a[1] && a[4] > 0 && a[5] > 0) {
        w = a[4];
        h = a[5];
        rgb = a[6] != 0;
    }
    if (rgb) {
        ++total.present_24bit;
        return;
    }
    n = map_scale(a[7]);
    if (override_scale > 0 || n != s->scale || w != s->disp_w || h != s->disp_h) {
        ++total.present_other;
        return;
    }
    if (n > 1 && a[0] && a[1]) {
        uint16_t *area = must_alloc((size_t)w * h * 2 + 2);
        size_t masked = 0;

        read_rect(s->jb, a[2] & 0x3ff, a[3] & 0x1ff, w, h, area);
        for (k = 0; k < (size_t)w * h; ++k)
            masked += (area[k] & 0x8000) != 0;
        free(area);
        if (masked > 0) {
            ++total.present_masked;
            return;
        }
    }
    check("GP1(05h)", jb_gp1(s->jb, 0x05000000u | ((uint32_t)a[2] & 0x3ff) |
                                        ((uint32_t)a[3] & 0x1ff) << 10),
          JB_OK);
    check("GP1(03h)", jb_gp1(s->jb, 0x03000000u | (a[0] && a[1] ? 0u : 1u)), JB_OK);
    f = jb_frame(s->jb, &fw, &fh);
    px = (size_t)fw * fh;
    raw = must_alloc(px * 2 + 2);
    ok = f != NULL && fw == want[0] && fh == want[1] && want[2] == fw * 2 && want[3] == 0;
    if (ok && n == 1 && a[0] && a[1]) {
        /* The area read back is the frame. */
        uint16_t *area = must_alloc(px * 2 + 2);
        read_rect(s->jb, a[2] & 0x3ff, a[3] & 0x1ff, w, h, area);
        for (k = 0; k < px; ++k) {
            unsigned p = area[k];
            raw[2 * k] = (unsigned char)p;
            raw[2 * k + 1] = (unsigned char)(p >> 8);
            ok &= f[4 * k] == widen5(p & 31) && f[4 * k + 1] == widen5((p >> 5) & 31) &&
                  f[4 * k + 2] == widen5((p >> 10) & 31) && f[4 * k + 3] == 255;
        }
        free(area);
        if (!ok)
            fail(s, "jb_frame is not the display area read back");
    } else if (ok) {
        /* The frame narrowed to 15 bits, without mask bits. */
        for (k = 0; k < px; ++k) {
            unsigned p = (unsigned)(f[4 * k] >> 3) | (unsigned)(f[4 * k + 1] >> 3) << 5 |
                         (unsigned)(f[4 * k + 2] >> 3) << 10;
            raw[2 * k] = (unsigned char)p;
            raw[2 * k + 1] = (unsigned char)(p >> 8);
        }
    }
    got_hash = trace_fnv_bytes(TRACE_FNV_INIT, raw, px * 2);
    if (!tally(&total.present, ok && got_hash == want_hash))
        fail(s, "frame %d x %d (want %d x %d): hash %016llx, want %016llx", fw, fh, want[0],
             want[1], (unsigned long long)got_hash, (unsigned long long)want_hash);
    free(raw);
}

static void op_poke(Scene *s)
{
    trace_reader *r = &s->r;
    uint32_t plane = trace_u32(r), w = trace_u32(r), h = trace_u32(r), runs = trace_u32(r), i, k;
    uint16_t *pix;
    size_t at = 0;
    uint32_t stat;

    if (plane != 0 || w != VRAM_W || h != VRAM_H)
        trace_die(r, "a POKE of the scaled VRAM, which has no public equivalent");
    pix = must_alloc((size_t)VRAM_PIXELS * 2);
    for (i = 0; i < runs; ++i) {
        uint32_t count = trace_u32(r), value = trace_u32(r);
        if (count > VRAM_PIXELS - at)
            trace_die(r, "POKE runs overflow the plane");
        for (k = 0; k < count; ++k)
            pix[at++] = (uint16_t)value;
    }
    if (at != VRAM_PIXELS)
        trace_die(r, "POKE runs do not cover the plane");
    s->poke_pending = 1;
    s->drawn = 0;
    /* A direct write: no mask settings, which come back afterwards. */
    stat = jb_gpustat(s->jb);
    {
        const uint32_t neutral = 0xe6000000u;
        uint32_t restore = 0xe6000000u | ((stat >> 11) & 1) | ((stat >> 12) & 1) << 1;
        words(s, &neutral, 1);
        load(s, 0, 0, 0, 0, pix, VRAM_PIXELS);
        words(s, &restore, 1);
    }
    check("POKE: the mask settings are back", (long long)(jb_gpustat(s->jb) & 0x1800),
          (long long)(stat & 0x1800));
    s->unexact = 0;
    free(pix);
}

static void op_apply(Scene *s)
{
    trace_reader *r = &s->r;
    int v[6], i;

    switch (s->code) {
    case TRACE_OP_GP0: {
        uint32_t n = trace_u32(r), k, *w;
        (void)trace_i32(r); /* the core decoder's return */
        w = must_alloc((size_t)n * 4 + 4);
        for (k = 0; k < n; ++k)
            w[k] = trace_u32(r);
        scaled_write(s, 1);
        words(s, w, n);
        if (!drawing_exact(s))
            s->unexact = 1;
        free(w);
        break;
    }
    case TRACE_OP_FILL: {
        uint32_t w[3];
        for (i = 0; i < 5; ++i)
            v[i] = trace_i32(r);
        w[0] = 0x02000000u | ((uint32_t)v[4] & 0xffffffu);
        w[1] = xy(v[0], v[1]);
        w[2] = xy(v[2], v[3]);
        scaled_write(s, 0);
        words(s, w, 3);
        break;
    }
    case TRACE_OP_LOAD: {
        uint32_t n;
        uint16_t *pix;
        for (i = 0; i < 4; ++i)
            v[i] = trace_i32(r);
        n = trace_u32(r);
        if (n != (uint32_t)xfer_w(v[2]) * (uint32_t)xfer_h(v[3]))
            trace_die(r, "LOAD pixel count disagrees with its size");
        pix = trace_u16s(r, n);
        scaled_write(s, 0);
        load(s, v[0], v[1], v[2], v[3], pix, n);
        free(pix);
        break;
    }
    case TRACE_OP_STORE: op_store(s); break;
    case TRACE_OP_COPY: {
        uint32_t w[4];
        for (i = 0; i < 6; ++i)
            v[i] = trace_i32(r);
        w[0] = 0x80000000u;
        w[1] = xy(v[0], v[1]);
        w[2] = xy(v[2], v[3]);
        w[3] = xy(v[4], v[5]);
        scaled_write(s, 0);
        words(s, w, 4);
        break;
    }
    case TRACE_OP_RESET_STATE: {
        const uint32_t w[6] = {0xe1000000u, 0xe2000000u, 0xe3000000u,
                               0xe4000000u, 0xe5000000u, 0xe6000000u};
        words(s, w, 6);
        break;
    }
    case TRACE_OP_HIRES_REPLICATE:
        /* The recording's scaled VRAM becomes the 1x one replicated: a
           public context's too, unless primitives drew detail since the
           last POKE. */
        if (s->drawn)
            s->scaled_unexact = 1;
        else if (s->poke_pending)
            s->scaled_unexact = 0;
        s->poke_pending = 0;
        break;
    case TRACE_OP_FLUSH:
    case TRACE_OP_SYNC:
    case TRACE_OP_TRACE_CALL:
    case TRACE_OP_VRAM_GET:
    case TRACE_OP_PRECISE_BEGIN_FRAME:
        break;
    case TRACE_OP_TRACE_TIME:
        (void)trace_i32(r);
        break;
    case TRACE_OP_PRECISE_ENABLE:
        s->precise_on = trace_i32(r) != 0;
        break;
    case TRACE_OP_LOWRES_MARK:
    case TRACE_OP_LOWRES_RESOLVE:
    case TRACE_OP_PRECISE_RECORD:
        for (i = 0; i < 4; ++i)
            (void)trace_i32(r);
        break;
    case TRACE_OP_PRECISE_STATS:
        (void)trace_i32(r);
        (void)trace_i32(r);
        ++total.stats;
        break;
    case TRACE_OP_CONFIGURE:
        for (i = 0; i < 5; ++i)
            v[i] = trace_i32(r);
        if (map_scale(v[0] > 1 ? v[0] : 1) != map_scale(s->t_scale))
            ++total.scale_changes;
        /* The recording's scaled VRAM is rebuilt from the 1x one. */
        if (s->drawn)
            s->scaled_unexact = 1;
        s->t_scale = v[0] > 1 ? v[0] : 1;
        s->t_dither = v[1];
        break;
    case TRACE_OP_SET_EXACT_1X: s->t_exact = trace_i32(r) != 0; break;
    case TRACE_OP_SET_TEXTURE_MAPPING: s->t_texmap = trace_i32(r) != 0; break;
    case TRACE_OP_PRESENT: op_present(s); break;
    case TRACE_OP_CAPTURE: op_capture(s); break;
    case TRACE_OP_POKE: op_poke(s); break;
    default: trace_die(r, "unknown op");
    }
    trace_op_done(r);
}

/* The highest scale and the display size the scene's ops use. */
static void survey(Scene *s, const trace_prelude *p)
{
    trace_reader r = s->r;
    uint32_t code;

    s->scale = p->scale > 1 ? p->scale : 1;
    s->disp_w = s->disp_h = 0;
    while ((code = trace_next_op(&r)) != TRACE_OP_END) {
        if (code == TRACE_OP_CONFIGURE) {
            int scale = trace_i32(&r);
            if (scale > s->scale)
                s->scale = scale;
        } else if (code == TRACE_OP_PRESENT && s->disp_w == 0) {
            int on = trace_i32(&r), env = trace_i32(&r), w, h;
            (void)on;
            (void)trace_i32(&r);
            (void)trace_i32(&r);
            w = trace_i32(&r);
            h = trace_i32(&r);
            s->disp_w = env && w > 0 && h > 0 ? w : 512;
            s->disp_h = env && w > 0 && h > 0 ? h : 256;
        }
        r.pos = r.end;
    }
    if (s->disp_w == 0) { /* no PRESENT: jb_config's default */
        s->disp_w = 320;
        s->disp_h = 240;
    }
    s->scale = map_scale(s->scale);
}

/* Frames tiled over all of VRAM against the golden image of the
   context's scale, on the colour bits. */
static void frame_golden(Scene *s, const char *path)
{
    char img[4096];
    const char *why = "", *slash = strrchr(path, '/');
    int n = s->scale, W = VRAM_W * n, H = VRAM_H * n, x0, y0, unexact;
    size_t stem = strlen(path) - 4, bad = 0;
    uint16_t *golden;

    if (override_scale > 0 && n > 1)
        snprintf(img, sizeof img, "%.*sscaled/%s.%dx.png",
                 slash ? (int)(slash - path + 1) : 0, path, s->name, n);
    else
        snprintf(img, sizeof img, "%.*s.%dx.png", (int)stem, path, n);
    {
        /* No image at this scale is expected for most scenes; one that is
           there and can't be used is a failure. */
        FILE *there = fopen(img, "rb");

        if (there == NULL && errno == ENOENT) {
            ++total.frame_goldens_none;
            return;
        }
        if (there == NULL) {
            printf("FAIL %s: golden %s: %s\n", s->name, img, strerror(errno));
            check("a golden image that is there opens", 0, 1);
            return;
        }
        fclose(there);
    }
    golden = trace_golden_load(img, W, H, &why);
    if (golden == NULL) {
        printf("FAIL %s: golden %s: %s\n", s->name, img, why);
        check("a golden image that is there loads", 0, 1);
        return;
    }
    unexact = n > 1 ? s->scaled_unexact || s->poke_pending : s->unexact;
    check("GP1(03h)", jb_gp1(s->jb, 0x03000000u), JB_OK);
    for (y0 = 0; y0 < VRAM_H; y0 += s->disp_h)
        for (x0 = 0; x0 < VRAM_W; x0 += s->disp_w) {
            const uint8_t *f;
            int fw, fh, i, j;

            check("GP1(05h)", jb_gp1(s->jb, 0x05000000u | (uint32_t)x0 | (uint32_t)y0 << 10),
                  JB_OK);
            f = jb_frame(s->jb, &fw, &fh);
            if (f == NULL || fw != s->disp_w * n || fh != s->disp_h * n) {
                ++bad;
                continue;
            }
            for (j = 0; j < fh; ++j)
                for (i = 0; i < fw; ++i) {
                    int X = (x0 * n + i) % W, Y = ((y0 + j / n) & (VRAM_H - 1)) * n + j % n;
                    const uint8_t *p = f + ((size_t)j * fw + i) * 4;
                    unsigned c = (unsigned)(p[0] >> 3) | (unsigned)(p[1] >> 3) << 5 |
                                 (unsigned)(p[2] >> 3) << 10;
                    bad += c != (golden[(size_t)Y * W + X] & 0x7fffu);
                }
        }
    if (unexact)
        tally(&total.frame_golden_unexact, bad == 0);
    else if (!tally(&total.frame_golden, bad == 0))
        fail(s, "frames against %s: %zu pixels differ", img, bad);
    free(golden);
}

static int run_scene(const char *path)
{
    Scene s;
    trace_prelude pre;
    jb_config cfg;
    jb_result res = JB_OK;
    uint16_t *end_vram = NULL;

    memset(&s, 0, sizeof s);
    s.code = TRACE_OP_LIMIT;
    trace_open(&s.r, path, s.name, sizeof s.name, &pre);
    survey(&s, &pre);
    memset(&cfg, 0, sizeof cfg);
    cfg.scale = s.scale;
    cfg.width = s.disp_w;
    cfg.height = s.disp_h;
    cfg.threads = pre.threads > 0 ? pre.threads : 1;
    s.jb = jb_create(&cfg, &res);
    if (s.jb == NULL) {
        printf("FAIL %s: jb_create: %s\n", s.name, jb_result_string(res));
        trace_close(&s.r);
        return 1;
    }
    s.t_scale = pre.scale > 1 ? pre.scale : 1;
    s.t_dither = pre.dither;
    s.t_exact = pre.exact_1x != 0;
    s.t_texmap = pre.texture_mapping != 0;
    s.precise_on = pre.precise_enabled != 0;

    for (s.op = 0;; ++s.op) {
        s.code = trace_next_op(&s.r);
        if (s.code == TRACE_OP_END)
            break;
        if (s.end_seen)
            trace_die(&s.r, "an op after the scene-end capture");
        op_apply(&s);
    }
    if (!s.end_seen)
        trace_die(&s.r, "no scene-end capture");

    /* The end state against the 1x golden image. */
    if (s.end_alone) {
        char img[4096];
        const char *why = "";
        size_t stem = strlen(path) - 4, i, bad = 0;
        uint16_t *golden;

        snprintf(img, sizeof img, "%.*s.1x.png", (int)stem, path);
        golden = trace_golden_load(img, VRAM_W, VRAM_H, &why);
        (void)vram_hash(&s, &end_vram);
        if (golden == NULL) {
            fail(&s, "golden %s: %s", img, why);
        } else {
            for (i = 0; i < VRAM_PIXELS; ++i)
                bad += golden[i] != end_vram[i];
            if (s.unexact)
                tally(&total.golden_unexact, bad == 0);
            else if (!tally(&total.golden, bad == 0))
                fail(&s, "golden %s: %zu pixels differ", img, bad);
        }
        free(golden);
        free(end_vram);
        frame_golden(&s, path);
    } else {
        ++total.goldens_not_alone;
    }
    ++total.scenes;
    printf("%s at %dx: %u ops, %s%s%s\n", s.name, s.scale, s.op + 1,
           s.failures ? "FAILED" : "every comparable check matched",
           s.unexact ? " (1x ends unexact)" : "",
           s.scale > 1 && (s.scaled_unexact || s.poke_pending) ? " (scaled ends unexact)" : "");
    jb_destroy(s.jb);
    trace_close(&s.r);
    return s.failures;
}

static void print_tally(const char *what, const Tally *t)
{
    printf("  %-44s %u compared, %u matched\n", what, t->compared, t->matched);
}

int main(int argc, char **argv)
{
    int i = 1, failures = 0;

    if (i + 1 < argc && strcmp(argv[i], "--scale") == 0) {
        override_scale = atoi(argv[i + 1]);
        i += 2;
        if (override_scale < 2 || override_scale > 8) {
            fprintf(stderr, "api_golden: --scale takes 2 to 8\n");
            return 2;
        }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: api_golden [--scale S] TRACE.jbt...\n");
        return 2;
    }
    for (; i < argc; ++i)
        failures += run_scene(argv[i]) != 0;
    printf("%u scenes alone through the public API%s:\n", total.scenes,
           override_scale ? ", scales above 1x replaced" : "");
    print_tally("captures (1x VRAM read back)", &total.capture);
    print_tally("  unexact recording, reported only", &total.capture_unexact);
    printf("  %-44s %u\n", "  skipped (not standalone)", total.capture_not_alone);
    print_tally("stores (GP0(C0h) and GPUREAD)", &total.store);
    print_tally("  unexact recording, reported only", &total.store_unexact);
    print_tally("presents (jb_frame)", &total.present);
    printf("  %-44s %u\n", "  skipped: 24-bit at a non-console size", total.present_24bit);
    printf("  %-44s %u\n", "  skipped: other size or scale", total.present_other);
    printf("  %-44s %u\n", "  skipped: above 1x with masked pixels shown", total.present_masked);
    print_tally("1x golden images (VRAM read back)", &total.golden);
    print_tally("  unexact recording, reported only", &total.golden_unexact);
    printf("  %-44s %u\n", "  skipped (end not standalone)", total.goldens_not_alone);
    print_tally("golden images of the scale, by jb_frame", &total.frame_golden);
    print_tally("  unexact recording, reported only", &total.frame_golden_unexact);
    printf("  %-44s %u\n", "  none at this scale", total.frame_goldens_none);
    print_tally("jb_gp0 calls", &total.gp0);
    printf("  %-44s %u\n", "precise stats not comparable", total.stats);
    printf("  %-44s %u\n", "scale changes (one context per scene)", total.scale_changes);
    if (api_t.failures)
        printf("  %d other checks failed\n", api_t.failures);
    return failures != 0 || api_t.failures != 0;
}

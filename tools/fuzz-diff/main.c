/*
 * fuzz-diff: seeded GP0 streams through jitterbug (public API, 1x, the
 * exact path) and the reference (psxstack's GPU, ref.h), VRAM and GPUREAD
 * compared after every command (docs/testing.md, "Differential fuzzing").
 *
 *   fuzz-diff strict [options]     the strict subset: any difference fails
 *   fuzz-diff bounded [options]    the bounded-residual subset: any pixel
 *                                  outside the bound fails
 *   fuzz-diff run [options]        exploratory: everything, differences
 *                                  labelled (never a pass criterion)
 *   fuzz-diff repro SEED INDEX [options]
 *                                  one stream, shrunk to a minimal
 *                                  reproducer, printed and saved as .jbc
 *   fuzz-diff play LIST.jbc...     lists through both, compared at the end
 *   fuzz-diff conformance DIR HW   tests/conformance's lists through the
 *                                  reference, against the console's dumps
 *                                  (HW = third_party/ps1-tests/gpu)
 *
 * Options: --seed S, --first I, --streams N, --commands C (per stream),
 * --focus all|xflip|lineties|lineg|quad|v, --no-filter (report the known
 * divergences too), --unmodelled (also generate what psxstack does not
 * model: rectangle flips, the CLUT cache), --out DIR (reproducers),
 * --repros K (shrink and save up to K per class), --quiet.
 *
 * Exit status: strict and bounded, 0 when nothing failed, 1 when something
 * did; run, 0 when it ran (a label assigns no correctness); 2 for an error.
 */
#include "fd.h"
#include "jbc.h"
#include "ref.h"

#include <jitterbug.h>

/* The software backend's 1x VRAM as a program reading it sees it
   (jbi_soft_vram: queued primitives finished, stale tiles filled in; at 1x
   there are none), read in place for the comparison after every command,
   where jb_vram_read's copy of all of VRAM would cost ten times as much;
   checked against jb_vram_read at the end of every stream (run). */
#include "core/context.h"
#include "soft/soft.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *must(void *p)
{
    if (p == NULL) {
        fprintf(stderr, "fuzz-diff: out of memory\n");
        exit(2);
    }
    return p;
}

/* ------------------------------------------------------------------------ */
/* The two sides */

static jb_context *jb;
static jbi_soft *jb_soft;
static uint16_t *jb_pix; /* a VRAM-sized buffer for reads */

static void jb_gp0_or_die(const uint32_t *w, size_t n)
{
    if (jb_gp0(jb, w, n) != JB_OK) {
        fprintf(stderr, "fuzz-diff: jb_gp0 failed: %s\n", jb_error_string(jb));
        exit(2);
    }
}

/* A new jitterbug context (1x, one thread), as jbc_play makes one for a
   list: its VRAM is all zero (jb_create). */
static void jb_new(void)
{
    jb_config c;
    jb_result r = JB_OK;

    if (jb != NULL)
        jb_destroy(jb);
    memset(&c, 0, sizeof c);
    c.scale = 1;
    c.threads = 1;
    jb = jb_create(&c, &r);
    if (jb == NULL) {
        fprintf(stderr, "fuzz-diff: jb_create: %s\n", jb_result_string(r));
        exit(2);
    }
    jb_soft = jbi_soft_from(jbi_context_backend(jb));
    if (jb_soft == NULL) {
        fprintf(stderr, "fuzz-diff: jitterbug's backend is not the software one\n");
        exit(2);
    }
}

static void sides_open(void)
{
    jb_new();
    jb_pix = must(malloc((size_t)FD_VRAM_PIXELS * 2));
}

/* The transcript (what `repro --out` saves): while rec is set, every GP0
   command run() sends to both sides, in order, as one fd_cmd each (a C0h
   with its GPUREAD count). What run() sends to one side only, it records
   as the command that has the same effect sent to both (the reference
   taking jitterbug's pixels: a load of those pixels, which leaves
   jitterbug's VRAM as it is). The ablation's trials are left out
   (rec_paused): they end with each side's VRAM as it was. Saved, the
   transcript is a list whose play leaves both sides as run() left them
   where it stopped. */
static fd_stream *rec;
static int rec_paused;

static void rec_words(const uint32_t *w, int n, int reads)
{
    if (rec != NULL && !rec_paused && n > 0)
        fd_stream_add(rec, w, n)->reads = reads;
}

static long rec_at(void)
{
    return rec != NULL ? rec->n : 0;
}

/* Drops what was recorded after the first n commands. */
static void rec_cut(long n)
{
    if (rec != NULL && rec->n > n)
        fd_stream_remove(rec, (int)n, rec->n - (int)n);
}

/* All of jitterbug's VRAM, 1024 x 512 row-major, as jb_vram_read would
   return it; valid until the next command. */
static const uint16_t *jb_all(void)
{
    return jbi_soft_vram(jb_soft);
}

/* The prelude every stream starts with, on both sides and in every saved
   list: GP1(00h), then VRAM filled with 0 (four fills: one covers at most
   1024 x 511). */
static const uint32_t prelude_fills[12] = {
    0x02000000u, 0x00000000u, 0x010003f0u, 0x02000000u, 0x000003f0u, 0x01000010u,
    0x02000000u, 0x01000000u, 0x010003f0u, 0x02000000u, 0x010003f0u, 0x01000010u,
};

/* For mode_assumptions' cases, which are not compared command by command. */
static void sides_reset(void)
{
    jb_gp1(jb, 0x00000000u);
    jb_gp0_or_die(prelude_fills, 12);
    ref_reset();
    ref_gp0(prelude_fills, 12);
}

static void jb_vram(int x, int y, int w, int h, uint16_t *dst)
{
    if (jb_vram_read(jb, x, y, w, h, dst) != JB_OK) {
        fprintf(stderr, "fuzz-diff: jb_vram_read failed: %s\n", jb_error_string(jb));
        exit(2);
    }
}

/* Differences in a rectangle inside VRAM: colour bits and mask bit apart;
   outside: differing pixels outside the rectangle compare_all was given. */
typedef struct {
    long colour, mask, outside;
    int x0, y0, x1, y1;
} diffs;

static void compare(int x, int y, int w, int h, diffs *d)
{
    const uint16_t *ref = ref_vram();
    int i, j;

    memset(d, 0, sizeof *d);
    d->x0 = 1 << 20, d->y0 = 1 << 20, d->x1 = -1, d->y1 = -1;
    if (w <= 0 || h <= 0)
        return;
    jb_vram(x, y, w, h, jb_pix);
    for (j = 0; j < h; ++j) {
        const uint16_t *a = jb_pix + (size_t)j * (size_t)w;
        const uint16_t *b = ref + (size_t)(y + j) * FD_VRAM_W + (size_t)x;
        if (memcmp(a, b, (size_t)w * 2) == 0)
            continue;
        for (i = 0; i < w; ++i) {
            uint16_t p = a[i], q = b[i];
            if (p == q)
                continue;
            if ((p ^ q) & 0x7fff)
                ++d->colour;
            if ((p ^ q) & 0x8000)
                ++d->mask;
            if (x + i < d->x0)
                d->x0 = x + i;
            if (x + i > d->x1)
                d->x1 = x + i;
            if (y + j < d->y0)
                d->y0 = y + j;
            if (y + j > d->y1)
                d->y1 = y + j;
        }
    }
}

/* The whole of VRAM, all 16 bits of every pixel, with the differing
   pixels outside [ax0, ax1] x [ay0, ay1] counted apart (an empty
   rectangle, ax1 < ax0, counts every difference as outside). */
static void compare_all(int ax0, int ay0, int ax1, int ay1, diffs *d)
{
    const uint16_t *a = jb_all(), *b = ref_vram();
    int x, y;

    memset(d, 0, sizeof *d);
    d->x0 = 1 << 20, d->y0 = 1 << 20, d->x1 = -1, d->y1 = -1;
    for (y = 0; y < FD_VRAM_H; ++y) {
        const uint16_t *p = a + (size_t)y * FD_VRAM_W, *q = b + (size_t)y * FD_VRAM_W;
        if (memcmp(p, q, FD_VRAM_W * 2) == 0)
            continue;
        for (x = 0; x < FD_VRAM_W; ++x) {
            if (p[x] == q[x])
                continue;
            if ((p[x] ^ q[x]) & 0x7fff)
                ++d->colour;
            if ((p[x] ^ q[x]) & 0x8000)
                ++d->mask;
            if (x < ax0 || x > ax1 || y < ay0 || y > ay1)
                ++d->outside;
            d->x0 = x < d->x0 ? x : d->x0;
            d->x1 = x > d->x1 ? x : d->x1;
            d->y0 = y < d->y0 ? y : d->y0;
            d->y1 = y > d->y1 ? y : d->y1;
        }
    }
}

/* The reference's VRAM-to-CPU words for a C0h command (psxstack has none):
   its VRAM read as psx-spx describes the transfer, sizes ((n - 1) & limit)
   + 1, wrapping at the edges, two pixels a word, the first in the low half,
   the last word's high half 0 when the count is odd. */
static long ref_read_count(const uint32_t *w)
{
    long rw = (long)((((w[2] & 0xffff) - 1) & 0x3ff) + 1);
    long rh = (long)(((((w[2] >> 16) & 0xffff) - 1) & 0x1ff) + 1);

    return (rw * rh + 1) / 2;
}

/* Word k (0 .. ref_read_count - 1) of the transfer. */
static uint32_t ref_read_word(const uint32_t *w, long k)
{
    const uint16_t *v = ref_vram();
    long x = (long)(w[1] & 0x3ff), y = (long)((w[1] >> 16) & 0x1ff);
    long rw = (long)((((w[2] & 0xffff) - 1) & 0x3ff) + 1);
    long rh = (long)(((((w[2] >> 16) & 0xffff) - 1) & 0x1ff) + 1);
    long n = rw * rh, i;
    uint32_t out = 0;

    for (i = 2 * k; i < 2 * k + 2 && i < n; ++i) {
        uint16_t p = v[(size_t)((y + i / rw) & 511) * FD_VRAM_W + (size_t)((x + i % rw) & 1023)];
        out |= (uint32_t)p << (16 * (i - 2 * k));
    }
    return out;
}

/* Loads a rectangle of pixels into one side or both, with the mask
   settings off; the caller sends E6h again. */
static uint32_t *load_words(int x, int y, int w, int h, const uint16_t *pix, int *count)
{
    long n = (long)w * h, i;
    uint32_t *words = must(malloc((size_t)(n / 2 + 8) * 4));
    int k = 0;

    words[k++] = 0xe6000000u;
    words[k++] = 0xa0000000u;
    words[k++] = (uint32_t)x | (uint32_t)y << 16;
    words[k++] = (uint32_t)w | (uint32_t)h << 16;
    for (i = 0; i < n; i += 2)
        words[k++] = (uint32_t)pix[i] | (i + 1 < n ? (uint32_t)pix[i + 1] << 16 : 0);
    *count = k;
    return words;
}

/* Recorded as two commands, E6h and the load, as load_side sends them. */
static void rec_load(int x, int y, int w, int h, const uint16_t *pix)
{
    int k;
    uint32_t *words;

    if (rec == NULL || rec_paused)
        return;
    words = load_words(x, y, w, h, pix, &k);
    rec_words(words, 1, 0);
    rec_words(words + 1, k - 1, 0);
    free(words);
}

static void load_side(int jb_side, int ref_side, int x, int y, int w, int h, const uint16_t *pix)
{
    int k;
    uint32_t *words = load_words(x, y, w, h, pix, &k);

    if (jb_side)
        jb_gp0_or_die(words, (size_t)k);
    if (ref_side)
        ref_gp0(words, (size_t)k);
    if (jb_side && ref_side) {
        rec_words(words, 1, 0);
        rec_words(words + 1, k - 1, 0);
    }
    free(words);
}

static void ref_rect(int x, int y, int w, int h, uint16_t *dst)
{
    const uint16_t *v = ref_vram();
    int j;

    for (j = 0; j < h; ++j)
        memcpy(dst + (size_t)j * (size_t)w, v + (size_t)(y + j) * FD_VRAM_W + (size_t)x,
               (size_t)w * 2);
}

/* Commands to both sides (one or more whole ones, recorded as one). */
static void send_both(const uint32_t *w, int n)
{
    jb_gp0_or_die(w, (size_t)n);
    ref_gp0(w, (size_t)n);
    rec_words(w, n, 0);
}

/* The drawing state words, E1h-E6h (E1h as the command left it). */
static void send_state(const fd_state *st, unsigned drop)
{
    uint32_t w[6];
    int i;

    w[0] = st->e1 & ~((drop & FD_F_DITHER) ? 0x200u : 0u);
    w[1] = (drop & FD_F_WINDOW) ? 0xe2000000u : st->e2;
    w[2] = st->e3;
    w[3] = st->e4;
    w[4] = st->e5;
    w[5] = st->e6 & ~((drop & FD_F_MASKCHECK) ? 2u : 0u) & ~((drop & FD_F_MASKSET) ? 1u : 0u);
    jb_gp0_or_die(w, 6);
    ref_gp0(w, 6);
    for (i = 0; i < 6; ++i) /* one command each in the transcript */
        rec_words(w + i, 1, 0);
}

/* ------------------------------------------------------------------------ */
/* Ablation: at a difference, the command is played again from the same
   VRAM (its drawing area as it was before) and state, with features taken
   away one at a time; what is left is what the difference needs. */

/* The command without the features in drop (FD_F_SEMI, MODULATE, GOURAUD,
   TEXTURE). Returns its length. */
static int variant(const fd_cmd *c, unsigned drop, uint32_t *out)
{
    uint32_t op = c->w[0] >> 24, nop = op;
    int n = 0, k, i;

    if (drop & FD_F_SEMI)
        nop &= ~0x02u;
    switch (op >> 5) {
    case 1: { /* polygons */
        int verts = (op & 0x08) ? 4 : 3, g = (op & 0x10) != 0, t = (op & 0x04) != 0;
        int ng, nt, t0 = 2; /* the first vertex's texture word */
        if (drop & FD_F_GOURAUD)
            nop &= ~0x10u;
        if (drop & FD_F_TEXTURE)
            nop &= ~0x05u;
        else if ((drop & FD_F_MODULATE) && t)
            nop |= 0x01u;
        ng = (nop & 0x10) != 0;
        nt = (nop & 0x04) != 0;
        out[n++] = nop << 24 | (c->w[0] & 0xffffffu);
        k = 1;
        for (i = 0; i < verts; ++i) {
            if (g && i > 0) {
                if (ng && (drop & FD_F_COLOUR))
                    out[n++] = c->w[0] & 0xffffffu;
                else if (ng)
                    out[n++] = c->w[k];
                ++k;
            }
            out[n++] = c->w[k++];
            if (t) {
                if (nt && (drop & FD_F_UV))
                    out[n++] = (c->w[k] & 0xffff0000u) | (c->w[t0] & 0xffffu);
                else if (nt)
                    out[n++] = c->w[k];
                ++k;
            }
        }
        return n;
    }
    case 2: { /* lines: the colours kept or dropped, the rest as it was */
        int g = (op & 0x10) != 0;
        if (drop & FD_F_GOURAUD)
            nop &= ~0x10u;
        out[n++] = nop << 24 | (c->w[0] & 0xffffffu);
        out[n++] = c->w[1];
        for (k = 2; k < c->n;) {
            if ((op & 0x08) && (c->w[k] & 0xf000f000u) == 0x50005000u && k >= 2 + (g ? 2 : 1)) {
                out[n++] = c->w[k++];
                break;
            }
            if (g) {
                if ((nop & 0x10) && (drop & FD_F_COLOUR))
                    out[n++] = c->w[0] & 0xffffffu;
                else if (nop & 0x10)
                    out[n++] = c->w[k];
                ++k;
                if (k >= c->n)
                    break;
            }
            out[n++] = c->w[k++];
        }
        while (k < c->n)
            out[n++] = c->w[k++];
        return n;
    }
    case 3: { /* rectangles */
        int t = (op & 0x04) != 0;
        if (drop & FD_F_TEXTURE)
            nop &= ~0x05u;
        else if ((drop & FD_F_MODULATE) && t)
            nop |= 0x01u;
        out[n++] = nop << 24 | (c->w[0] & 0xffffffu);
        out[n++] = c->w[1];
        k = 2;
        if (t) {
            if (nop & 0x04)
                out[n++] = c->w[k];
            ++k;
        }
        while (k < c->n)
            out[n++] = c->w[k++];
        return n;
    }
    default:
        memcpy(out, c->w, (size_t)c->n * 4);
        return c->n;
    }
}

typedef struct {
    int x, y, w, h;
    uint16_t *before, *after_jb, *after_ref;
} area_save;

/* One trial: the area as it was, the state, the command without drop. */
static int trial(const fd_cmd *c, const fd_state *st, const area_save *a, unsigned drop)
{
    uint32_t words[1200];
    int n = variant(c, drop, words);
    diffs d;
    uint32_t clear = 0x01000000u;

    load_side(1, 1, a->x, a->y, a->w, a->h, a->before);
    send_state(st, drop);
    send_both(&clear, 1); /* the CLUT cache reloads, as the guard before it made it */
    send_both(words, n);
    compare(a->x, a->y, a->w, a->h, &d);
    return d.colour != 0 || d.mask != 0;
}

static unsigned present_features(const fd_desc *d)
{
    unsigned f = 0;

    if (d->dither && d->kind != FD_K_RECT)
        f |= FD_F_DITHER;
    if (d->mask_check)
        f |= FD_F_MASKCHECK;
    if (d->mask_set)
        f |= FD_F_MASKSET;
    if (d->window && d->textured)
        f |= FD_F_WINDOW;
    if (d->semi)
        f |= FD_F_SEMI;
    if (d->textured && !d->raw)
        f |= FD_F_MODULATE;
    if (d->gouraud)
        f |= FD_F_GOURAUD;
    if (d->textured)
        f |= FD_F_TEXTURE;
    if (d->gouraud) {
        int i;
        for (i = 1; i < d->nv; ++i)
            if (d->r[i] != d->r[0] || d->g[i] != d->g[0] || d->b[i] != d->b[0])
                f |= FD_F_COLOUR;
    }
    if (d->textured && (d->kind == FD_K_TRI || d->kind == FD_K_QUAD)) {
        int i;
        for (i = 1; i < d->nv; ++i)
            if (d->u[i] != d->u[0] || d->v[i] != d->v[0])
                f |= FD_F_UV;
    }
    return f;
}

static int small_diff(const area_save *a);

/* One trial with different words on the two sides: jitterbug gets wj
   (with the drawing offset 0 when jb_offset_zero), the reference wr.
   0: the same; 1: within one step per channel, mask bits equal (what an
   interpolation rule leaves); 2: more. */
static int trial_sides(const uint32_t *wj, int nj, const uint32_t *wr, int nr, const fd_state *st,
                       const area_save *a, int jb_offset_zero)
{
    diffs d;
    uint32_t clear = 0x01000000u, e5 = 0xe5000000u;

    load_side(1, 1, a->x, a->y, a->w, a->h, a->before);
    send_state(st, 0);
    if (jb_offset_zero)
        jb_gp0_or_die(&e5, 1);
    send_both(&clear, 1);
    jb_gp0_or_die(wj, (size_t)nj);
    ref_gp0(wr, (size_t)nr);
    compare(a->x, a->y, a->w, a->h, &d);
    if (d.colour == 0 && d.mask == 0)
        return 0;
    {
        area_save t = *a;
        int small;
        t.after_jb = must(malloc((size_t)a->w * (size_t)a->h * 2));
        t.after_ref = must(malloc((size_t)a->w * (size_t)a->h * 2));
        jb_vram(a->x, a->y, a->w, a->h, t.after_jb);
        ref_rect(a->x, a->y, a->w, a->h, t.after_ref);
        small = small_diff(&t);
        free(t.after_jb);
        free(t.after_ref);
        return small ? 1 : 2;
    }
}

static int sext11(uint32_t v)
{
    return (int)((v & 0x7ff) ^ 0x400) - 0x400;
}

/* A vertex word with the offset added and the sum wrapped to 11 bits, as
   psxstack places it. */
static uint32_t wrapped(uint32_t w, const fd_state *st)
{
    int x = sext11(w) + sext11(st->e5), y = sext11(w >> 16) + sext11(st->e5 >> 11);
    return ((uint32_t)x & 0x7ffu) | ((uint32_t)y & 0x7ffu) << 16;
}

/* The word indices of a polygon's vertex positions, colours and texture
   words (-1: none). */
static void poly_layout(uint32_t op, int *pos, int *col, int *tex)
{
    int verts = (op & 0x08) ? 4 : 3, g = (op & 0x10) != 0, t = (op & 0x04) != 0, k = 1, i;

    for (i = 0; i < verts; ++i) {
        col[i] = i == 0 ? 0 : g ? k++ : -1;
        pos[i] = k++;
        tex[i] = t ? k++ : -1;
    }
}

/* Whether jitterbug, given the vertices placed as psxstack places them
   (the sum wrapped to 11 bits), matches psxstack but for at most one step
   a channel: then the offset-wrap rule is the likely cause of all but an
   interpolation-sized rest (a hypothesis, as every label). */
static int wrap_explains(const fd_cmd *c, const fd_state *st, const area_save *a)
{
    uint32_t w[1200], op = c->w[0] >> 24;
    int k;

    if (c->n > 1200)
        return 0;
    memcpy(w, c->w, (size_t)c->n * 4);
    switch (op >> 5) {
    case 1: {
        int pos[4], col[4], tex[4], i;
        poly_layout(op, pos, col, tex);
        for (i = 0; i < ((op & 0x08) ? 4 : 3); ++i)
            w[pos[i]] = wrapped(c->w[pos[i]], st);
        break;
    }
    case 2: {
        int per = (op & 0x10) ? 2 : 1;
        w[1] = wrapped(c->w[1], st);
        for (k = 1 + per; k < c->n; k += per) {
            if ((op & 0x08) && (c->w[k] & 0xf000f000u) == 0x50005000u)
                break;
            w[k] = wrapped(c->w[k], st);
            if (!(op & 0x08))
                break;
        }
        break;
    }
    case 3:
        w[1] = wrapped(c->w[1], st);
        break;
    default:
        return 0;
    }
    return trial_sides(w, c->n, c->w, c->n, st, a, 1) < 2;
}

/* Whether jitterbug, drawing a quad's triangles in psxstack's order (v1
   v2 v3, then v0 v1 v2) as two triangle commands, matches psxstack but for
   at most one step a channel: then the order is the likely cause. */
static int order_explains(const fd_cmd *c, const fd_state *st, const area_save *a)
{
    uint32_t w[32], op = c->w[0] >> 24, top = op & ~0x08u;
    int pos[4], col[4], tex[4], n = 0, t = (op & 0x04) != 0, g = (op & 0x10) != 0;
    uint32_t clut = 0;
    static const int tri[2][3] = {{1, 2, 3}, {0, 1, 2}};
    int j, i;

    if ((op >> 5) != 1 || !(op & 0x08))
        return 0;
    poly_layout(op, pos, col, tex);
    if (t)
        clut = c->w[tex[0]] & 0xffff0000u;
    for (j = 0; j < 2; ++j) {
        for (i = 0; i < 3; ++i) {
            int v = tri[j][i];
            uint32_t rgb = (col[v] >= 0 ? c->w[col[v]] : c->w[0]) & 0xffffffu;
            if (i == 0)
                w[n++] = top << 24 | rgb;
            else if (g)
                w[n++] = rgb;
            w[n++] = c->w[pos[v]];
            if (t) {
                uint32_t hi = i == 0 ? clut : i == 1 ? (c->w[tex[1]] & 0xffff0000u) : 0;
                w[n++] = (c->w[tex[v]] & 0xffffu) | hi;
            }
        }
    }
    return trial_sides(w, n, c->w, c->n, st, a, 0) < 2;
}

static void ablate(const fd_cmd *c, const fd_state *st, area_save *a, fd_desc *d)
{
    unsigned present = present_features(d), drop = 0;
    int i;

    ++rec_paused; /* each side ends with its VRAM as it was */
    if (d->offset_wraps)
        d->wrap_explains = wrap_explains(c, st, a);
    if (d->kind == FD_K_QUAD)
        d->order_explains = order_explains(c, st, a);
    d->isolated = trial(c, st, a, 0);
    if (d->kind == FD_K_LINE || d->kind == FD_K_POLYLINE) {
        /* which pixels: each segment alone, flat, opaque, undithered, white,
           on zeros (alone, so that a polyline's shared vertices count) */
        uint32_t op = c->w[0] >> 24, pos[64], clear = 0x01000000u;
        int per = (op & 0x10) ? 2 : 1, np = 0, k = 1, j;
        uint16_t *zero = must(calloc((size_t)a->w * (size_t)a->h, 2));
        pos[np++] = c->w[1];
        for (k = 1 + per; k < c->n && np < 64; k += per) {
            if ((op & 0x08) && np >= 2 && (c->w[k - per + 1] & 0xf000f000u) == 0x50005000u)
                break;
            if (k >= c->n)
                break;
            pos[np++] = c->w[k];
            if (!(op & 0x08))
                break;
        }
        for (j = 0; j + 1 < np && !d->coverage; ++j) {
            uint32_t words[3];
            diffs dc;
            words[0] = 0x40f8f8f8u;
            words[1] = pos[j];
            words[2] = pos[j + 1];
            load_side(1, 1, a->x, a->y, a->w, a->h, zero);
            send_state(st, FD_F_DITHER | FD_F_MASKCHECK | FD_F_MASKSET);
            send_both(&clear, 1);
            send_both(words, 3);
            compare(a->x, a->y, a->w, a->h, &dc);
            d->coverage = dc.colour != 0 || dc.mask != 0;
        }
        free(zero);
    }
    if (d->isolated) {
        /* the attributes' variation first: what is left is not interpolation */
        static const unsigned order[FD_F_COUNT] = {FD_F_COLOUR,   FD_F_UV,     FD_F_DITHER,
                                                   FD_F_MASKCHECK, FD_F_MASKSET, FD_F_WINDOW,
                                                   FD_F_SEMI,     FD_F_MODULATE, FD_F_GOURAUD,
                                                   FD_F_TEXTURE};
        for (i = 0; i < FD_F_COUNT; ++i) {
            unsigned f = order[i];
            if ((present & f) && trial(c, st, a, drop | f))
                drop |= f;
        }
        d->needs = present & ~drop;
    }
    /* back to the state the stream is in */
    load_side(1, 0, a->x, a->y, a->w, a->h, a->after_jb);
    load_side(0, 1, a->x, a->y, a->w, a->h, a->after_ref);
    send_state(st, 0);
    --rec_paused;
}

/* Whether every differing pixel is within one step per 5-bit channel,
   with equal mask bits. */
static int small_diff(const area_save *a)
{
    long i, n = (long)a->w * a->h;

    for (i = 0; i < n; ++i) {
        uint16_t p = a->after_jb[i], q = a->after_ref[i];
        int s;
        if (p == q)
            continue;
        if ((p ^ q) & 0x8000)
            return 0;
        for (s = 0; s < 15; s += 5) {
            int dp = (p >> s) & 31, dq = (q >> s) & 31;
            if (dp - dq > 1 || dq - dp > 1)
                return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Bounded residual: the footprint trial (docs/testing.md, "Three modes",
   bounded). The command itself, words unchanged, played again on both sides
   with the drawing state as it was but for E6h bit 0 (set the mask bit)
   forced on, from VRAM as it was before the command in its box (b: the
   polygon's bounds within the drawing area) with every pixel's bit 15
   cleared there, and VRAM elsewhere as the command left it (which run has
   found identical on both sides, or fails on). Every pixel of the box the
   command writes then has bit 15 set after it, whatever colour it is
   given, and every one it does not write keeps bit 15 clear: those pixels
   are its footprint, which must be the same on both sides; and outside the
   box the two sides must stay identical. The box never meets the texels or
   CLUT a bounded primitive reads (gen_check.c), so clearing bit 15 there
   changes none of them. Each side then gets its own pixels after the
   actual command back in the box, and the drawing state as it was.
   1: the footprints differ. The transcript keeps the trial (its first
   *trial_end commands end with the trial's command: a reproducer of a
   footprint failure ends there), and records the restoring as jitterbug's
   pixels loaded on both sides (the same, once run() has given the
   reference jitterbug's pixels, which it does whenever the stream goes
   on). */

typedef struct {
    int x0, y0, x1, y1;
} box;

/* A box's pixels out of an area_save buffer (row-major over a). */
static void sub_rect(const area_save *a, const uint16_t *src, const box *b, uint16_t *dst)
{
    int w = b->x1 - b->x0 + 1, y;

    for (y = b->y0; y <= b->y1; ++y)
        memcpy(dst + (size_t)(y - b->y0) * (size_t)w,
               src + (size_t)(y - a->y) * (size_t)a->w + (size_t)(b->x0 - a->x), (size_t)w * 2);
}

static int footprint_differs(const fd_cmd *c, const fd_state *st, const area_save *a, const box *b,
                             long *trial_end)
{
    static uint16_t *bg;
    long n, i;
    uint32_t clear = 0x01000000u;
    fd_state forced = *st;
    const uint16_t *ref, *jbv;
    int x, y, bw = b->x1 - b->x0 + 1, bh = b->y1 - b->y0 + 1, differs = 0;

    if (bg == NULL)
        bg = must(malloc((size_t)FD_VRAM_PIXELS * 2));
    n = (long)bw * bh;
    sub_rect(a, a->before, b, bg);
    for (i = 0; i < n; ++i)
        bg[i] = (uint16_t)(bg[i] & 0x7fffu);
    load_side(1, 1, b->x0, b->y0, bw, bh, bg);
    forced.e6 |= 1u;
    send_state(&forced, 0);
    send_both(&clear, 1); /* the CLUT cache reloads, as before the command */
    send_both(c->w, c->n);
    *trial_end = rec_at();
    jbv = jb_all();
    ref = ref_vram();
    for (y = 0; y < FD_VRAM_H && !differs; ++y) {
        if (memcmp(jbv + (size_t)y * FD_VRAM_W, ref + (size_t)y * FD_VRAM_W, FD_VRAM_W * 2) == 0)
            continue;
        for (x = 0; x < FD_VRAM_W; ++x) {
            size_t at = (size_t)y * FD_VRAM_W + (size_t)x;
            int inside = x >= b->x0 && x <= b->x1 && y >= b->y0 && y <= b->y1;
            uint16_t p = jbv[at], q = ref[at];
            if (inside ? ((p ^ q) & 0x8000) != 0 : p != q) {
                differs = 1;
                break;
            }
        }
    }
    sub_rect(a, a->after_jb, b, bg);
    load_side(1, 0, b->x0, b->y0, bw, bh, bg);
    rec_load(b->x0, b->y0, bw, bh, bg);
    sub_rect(a, a->after_ref, b, bg);
    load_side(0, 1, b->x0, b->y0, bw, bh, bg);
    send_state(st, 0);
    return differs;
}

/* Whether a pixel of the drawing area outside the box differs between the
   two sides after the command (no bounded rule reaches past a polygon's
   bounds). */
static int outside_box_differs(const area_save *a, const box *b)
{
    int x, y;

    for (y = a->y; y < a->y + a->h; ++y)
        for (x = a->x; x < a->x + a->w; ++x) {
            size_t at = (size_t)(y - a->y) * (size_t)a->w + (size_t)(x - a->x);
            if ((x < b->x0 || x > b->x1 || y < b->y0 || y > b->y1) &&
                a->after_jb[at] != a->after_ref[at])
                return 1;
        }
    return 0;
}

/* A polygon's bounds (its vertices as jitterbug places them, which in the
   bounded subset is where psxstack places them too) within the drawing
   area a. 0: empty. */
static int poly_box(const fd_desc *d, const area_save *a, box *b)
{
    int i;

    b->x0 = b->x1 = d->vx[0];
    b->y0 = b->y1 = d->vy[0];
    for (i = 1; i < d->nv; ++i) {
        b->x0 = d->vx[i] < b->x0 ? d->vx[i] : b->x0;
        b->x1 = d->vx[i] > b->x1 ? d->vx[i] : b->x1;
        b->y0 = d->vy[i] < b->y0 ? d->vy[i] : b->y0;
        b->y1 = d->vy[i] > b->y1 ? d->vy[i] : b->y1;
    }
    b->x0 = b->x0 < a->x ? a->x : b->x0;
    b->y0 = b->y0 < a->y ? a->y : b->y0;
    b->x1 = b->x1 > a->x + a->w - 1 ? a->x + a->w - 1 : b->x1;
    b->y1 = b->y1 > a->y + a->h - 1 ? a->y + a->h - 1 : b->y1;
    return b->x0 <= b->x1 && b->y0 <= b->y1;
}

/* ------------------------------------------------------------------------ */
/* The mask check's invariant (docs/testing.md, "Differential fuzzing"):
   with E6h bit 1 set, a command that honours the check (a polygon, line,
   rectangle, VRAM-to-VRAM copy or CPU-to-VRAM load; not a fill) leaves
   every pixel whose bit 15 was set before it unchanged (psx-spx, GP0(E6h)).
   It is a property of one renderer's before and after alone: checked on
   each side apart, it fails a protected pixel that both sides change the
   same way. */

/* Whether cmd, with the drawing state st before it, honours the check. */
static int honours_mask_check(const fd_desc *d, const fd_cmd *c, const fd_state *st)
{
    if (!(st->e6 & 2u) || c->background)
        return 0;
    return (d->kind >= FD_K_TRI && d->kind <= FD_K_RECT) || d->kind == FD_K_COPY ||
           d->kind == FD_K_LOAD;
}

/* The pixels of a w x h rectangle (rows bs and as apart in before and
   after) whose bit 15 was set in before and which differ in after. */
static long protected_changed(const uint16_t *before, size_t bs, const uint16_t *after, size_t as,
                              int w, int h)
{
    long n = 0;
    int x, y;

    for (y = 0; y < h; ++y) {
        const uint16_t *p = before + (size_t)y * bs, *q = after + (size_t)y * as;
        if (memcmp(p, q, (size_t)w * 2) == 0)
            continue;
        for (x = 0; x < w; ++x)
            if ((p[x] & 0x8000) && q[x] != p[x])
                ++n;
    }
    return n;
}

/* fuzz-diff invariants: protected_changed on synthetic before and after
   images, the cases a differential comparison cannot see among them (both
   sides changing a protected pixel the same way). 0 when every case gives
   what it must. */
static int mode_invariants(void)
{
    enum { W = 8, H = 4 };
    uint16_t before[W * H], a[W * H], b[W * H];
    int bad = 0, i;
    struct {
        const char *what;
        int at;          /* the pixel changed, -1 none */
        uint16_t was;    /* its value before */
        uint16_t now;    /* after, on both sides */
        long want;
    } cases[] = {
        {"nothing changed", -1, 0, 0, 0},
        {"an unprotected pixel changed", 9, 0x1234, 0x4321, 0},
        {"an unprotected pixel given bit 15", 10, 0x1234, 0x9234, 0},
        {"a protected pixel's colour changed", 11, 0x9234, 0x9235, 1},
        {"a protected pixel's bit 15 cleared", 12, 0x9234, 0x1234, 1},
        {"a protected pixel written with its own value", 13, 0x8001, 0x8001, 0},
        {"the last pixel, protected, changed", W * H - 1, 0xffff, 0x8000, 1},
    };

    for (i = 0; i < (int)(sizeof cases / sizeof cases[0]); ++i) {
        long got_a, got_b, got_rows;
        int k;
        for (k = 0; k < W * H; ++k)
            before[k] = (uint16_t)(k * 0x0421u & 0x7fffu); /* none protected */
        before[3] = 0x8003; /* protected, left alone in every case */
        if (cases[i].at >= 0)
            before[cases[i].at] = cases[i].was;
        memcpy(a, before, sizeof a);
        if (cases[i].at >= 0)
            a[cases[i].at] = cases[i].now;
        memcpy(b, a, sizeof b); /* the other side wrong the same way */
        got_a = protected_changed(before, W, a, W, W, H);
        got_b = protected_changed(before, W, b, W, W, H);
        /* a sub-rectangle with other strides: the last two rows, from column 1 */
        got_rows = protected_changed(before + 2 * W + 1, W, a + 2 * W + 1, W, W - 1, 2);
        if (got_a != cases[i].want || got_b != cases[i].want) {
            printf("FAIL: invariants: %s: %ld and %ld, want %ld on each side\n", cases[i].what,
                   got_a, got_b, cases[i].want);
            bad = 1;
        } else if (got_rows != (cases[i].at >= 2 * W + 1 && cases[i].at % W != 0 ? cases[i].want : 0)) {
            printf("FAIL: invariants: %s: %ld in the sub-rectangle\n", cases[i].what, got_rows);
            bad = 1;
        } else {
            printf("invariants: %-46s %ld on each side, as it must\n", cases[i].what, got_a);
        }
    }
    return bad;
}

/* ------------------------------------------------------------------------ */
/* Running a stream */

enum { MAX_DIV = 64 };

typedef struct {
    int first;          /* the first command with an unfiltered difference, or -1;
                           0 also for a difference in the prelude */
    int prelude;        /* 0, or the prelude's step that differed (run_prelude) */
    int unattributed;   /* found only by the final comparison */
    diffs d;
    fd_desc desc;
    char cls[256];
    long read_bad;      /* GPUREAD words that differ at the failing command */
    long read_first;    /* the first of them */
    long protected_jb, protected_ref; /* protected pixels each side changed */
    long div[MAX_DIV];  /* differences counted under each known divergence */
    long prims;         /* drawing commands run */
    long bounded_ok;    /* bounded commands that differed within the bound */
    long bounded_same;  /* bounded commands that did not differ */
    long drew;          /* drawing commands that changed a pixel (psxstack's VRAM) */
} result;

static int is_draw(const fd_desc *d)
{
    return d->kind >= FD_K_TRI && d->kind <= FD_K_RECT;
}

static uint16_t *area_buf[3];
static uint16_t *prot_buf; /* all of VRAM before a command under the mask check */
/* A divergence class reported as a difference rather than filtered (to
   shrink a reproducer of it), or -1. */
static int unfilter_div = -1;
static long best_other = 1L << 40; /* still(): the fewest other differences so far */

/* How run() treats a difference: exploratory with the divergence table
   as labels (RUN_FILTER), every difference a failure (RUN_EXACT: strict
   mode, and exploratory --no-filter), or bounded commands checked against
   the bounded residual and every other difference a failure (RUN_BOUNDED). */
enum { RUN_EXACT = 0, RUN_FILTER = 1, RUN_BOUNDED = 2 };

/* The start of every stream, compared step by step like any command: a
   new jitterbug context and the reference's power-on (both VRAMs must then
   be the same, all zero), GP1(00h) on jitterbug (psxstack models no GP1),
   then the four fills of prelude_fills one at a time. 0 when every step
   left the two VRAMs identical; else the step (1 the start, 2 GP1(00h),
   3 to 6 the fills), with rr filled in. */
static int run_prelude(result *rr)
{
    static const char *const steps[6] = {
        "the two VRAMs at the start", "GP1(00h)", "fill 1 of 4 (1008 x 256 at 0,0)",
        "fill 2 of 4 (16 x 256 at 1008,0)", "fill 3 of 4 (1008 x 256 at 0,256)",
        "fill 4 of 4 (16 x 256 at 1008,256)"};
    int k;

    jb_new();
    ref_reset();
    for (k = 0; k < 6; ++k) {
        if (k == 1)
            jb_gp1(jb, 0x00000000u);
        else if (k >= 2)
            send_both(prelude_fills + 3 * (k - 2), 3);
        compare_all(0, 0, -1, -1, &rr->d);
        if (rr->d.colour || rr->d.mask) {
            rr->first = 0;
            rr->prelude = k + 1;
            snprintf(rr->cls, sizeof rr->cls, "prelude: %s differ%s", steps[k], k ? "s" : "");
            return k + 1;
        }
    }
    return 0;
}

/* Runs the stream; stops at the first difference not in a known
   divergence (RUN_FILTER), at the first difference (RUN_EXACT), or at the
   first outside the bound (RUN_BOUNDED). After every step of the prelude
   and after every command, the background loads and the state words
   included, the whole of VRAM (all 16 bits of each of its 1024 x 512
   pixels) is compared, and every GPUREAD word of a C0h as it is read; a
   drawing command's differences outside its drawing area are a failure of
   their own in every mode ("writes outside the drawing area"), never
   labelled or bounded, and so is a protected pixel changed on either side
   under the mask check (honours_mask_check), whether or not the two sides
   agree. With rec set, the transcript ends where run() stopped: after the
   failing command (after the footprint trial for a footprint failure). */
static void run(const fd_stream *s, int filter, result *rr)
{
    fd_state st;
    int i, j;

    if (area_buf[0] == NULL) {
        for (j = 0; j < 3; ++j)
            area_buf[j] = must(malloc((size_t)FD_VRAM_PIXELS * 2));
        prot_buf = must(malloc((size_t)FD_VRAM_PIXELS * 2));
    }
    memset(rr, 0, sizeof *rr);
    rr->first = -1;
    if (run_prelude(rr))
        return;
    fd_state_reset(&st);
    for (i = 0; i < s->n; ++i) {
        const fd_cmd *c = &s->c[i];
        fd_desc d;
        diffs df;
        int x0 = 0, y0 = 0, x1 = 1023, y1 = 511, draw, has_area, bounded, fp, guarded;
        int outbox, has_box;
        long read_bad = 0, read_first = -1, prot_jb = 0, prot_ref = 0, rec_cmd, rec_trial = 0;
        area_save a;
        box bx;

        fd_describe(c, &st, &d);
        draw = is_draw(&d) && !c->background;
        if (draw) {
            x0 = d.area_x0, y0 = d.area_y0, x1 = d.area_x1, y1 = d.area_y1;
            ++rr->prims;
        }
        has_area = x1 >= x0 && y1 >= y0;
        if (draw && has_area)
            ref_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, area_buf[0]);
        /* st is the state before the command here */
        guarded = honours_mask_check(&d, c, &st);
        if (guarded)
            memcpy(prot_buf, ref_vram(), (size_t)FD_VRAM_PIXELS * 2);
        jb_gp0_or_die(c->w, (size_t)c->n);
        ref_gp0(c->w, (size_t)c->n);
        rec_words(c->w, c->n, c->reads);
        if (c->reads > 0) {
            /* every word, compared as it is read */
            long k, total;
            if ((c->w[0] >> 29) != 6 || c->n < 3) {
                fprintf(stderr, "fuzz-diff: command %d has GPUREAD words but is not a C0h\n", i);
                exit(2);
            }
            total = ref_read_count(c->w);
            if (c->reads > total) {
                fprintf(stderr,
                        "fuzz-diff: command %d reads %d GPUREAD words, past the end of its "
                        "transfer (%ld words): not compared, so not allowed\n",
                        i, c->reads, total);
                exit(2);
            }
            for (k = 0; k < c->reads; ++k)
                if (jb_gpuread(jb) != ref_read_word(c->w, k)) {
                    if (read_bad++ == 0)
                        read_first = k;
                }
        }
        rec_cmd = rec_at();
        fd_state_apply(&st, c);
        compare_all(x0, y0, x1, y1, &df);
        if (guarded) {
            prot_jb = protected_changed(prot_buf, FD_VRAM_W, jb_all(), FD_VRAM_W, FD_VRAM_W,
                                        FD_VRAM_H);
            prot_ref = protected_changed(prot_buf, FD_VRAM_W, ref_vram(), FD_VRAM_W, FD_VRAM_W,
                                         FD_VRAM_H);
        }
        a.x = x0, a.y = y0, a.w = x1 - x0 + 1, a.h = y1 - y0 + 1;
        a.before = area_buf[0];
        a.after_jb = area_buf[1];
        a.after_ref = area_buf[2];
        if (draw && has_area) {
            const uint16_t *v = ref_vram();
            int row;
            for (row = y0; row <= y1; ++row)
                if (memcmp(v + (size_t)row * FD_VRAM_W + (size_t)x0,
                           area_buf[0] + (size_t)(row - y0) * (size_t)a.w, (size_t)a.w * 2) != 0) {
                    ++rr->drew;
                    break;
                }
        }
        bounded = filter == RUN_BOUNDED && c->bounded && draw && has_area;
        fp = outbox = 0;
        if (bounded) {
            jb_vram(a.x, a.y, a.w, a.h, a.after_jb);
            ref_rect(a.x, a.y, a.w, a.h, a.after_ref);
            has_box = poly_box(&d, &a, &bx);
            if (has_box)
                fp = footprint_differs(c, &st, &a, &bx, &rec_trial);
            else
                bx.x0 = 0, bx.x1 = -1, bx.y0 = 0, bx.y1 = -1;
            outbox = outside_box_differs(&a, &bx);
        }
        if (df.colour == 0 && df.mask == 0 && !read_bad && !fp && !outbox && !prot_jb &&
            !prot_ref) {
            if (bounded)
                ++rr->bounded_same;
            continue;
        }
        d.read = read_bad && df.colour == 0 && df.mask == 0;
        rr->read_bad = read_bad;
        rr->read_first = read_first;
        rr->protected_jb = prot_jb;
        rr->protected_ref = prot_ref;
        if (draw && df.outside > 0) {
            char cls[200];
            d.isolated = -1; /* not ablated: no rule lets a primitive write there */
            fd_class(&d, cls, sizeof cls);
            rr->first = i;
            rr->d = df;
            rr->desc = d;
            snprintf(rr->cls, sizeof rr->cls, "writes outside the drawing area: %s", cls);
            rec_cut(rec_cmd);
            return;
        }
        if (prot_jb || prot_ref) {
            char cls[200];
            d.isolated = -1;
            fd_class(&d, cls, sizeof cls);
            rr->first = i;
            rr->d = df;
            rr->desc = d;
            snprintf(rr->cls, sizeof rr->cls,
                     "a protected pixel changed under the mask check by %s: %.150s",
                     prot_jb && prot_ref ? "both" : prot_jb ? "jitterbug" : "psxstack", cls);
            rec_cut(rec_cmd);
            return;
        }
        if (bounded) {
            /* Bounded residual (docs/testing.md, "Three modes"): the same
               footprint, then in the drawing area the mask bits equal and
               every channel within one
               5-bit step, and nothing different outside it (above); then the
               reference takes jitterbug's pixels and the stream goes on. */
            int small = small_diff(&a);
            const char *why;
            char cls[200];
            if (!fp && !outbox && small) {
                ++rr->bounded_ok;
                if (df.colour || df.mask) {
                    int w = df.x1 - df.x0 + 1, h = df.y1 - df.y0 + 1;
                    jb_vram(df.x0, df.y0, w, h, jb_pix);
                    load_side(0, 1, df.x0, df.y0, w, h, jb_pix);
                    rec_load(df.x0, df.y0, w, h, jb_pix);
                }
                send_state(&st, 0);
                continue;
            }
            why = fp       ? "bounded, footprint differs"
                  : outbox ? "bounded, differs outside the polygon's bounds"
                           : "bounded, residual over one step";
            d.small = !fp && !outbox && small;
            rec_cut(fp ? rec_trial : rec_cmd);
            ablate(c, &st, &a, &d);
            rr->first = i;
            rr->d = df;
            rr->desc = d;
            fd_class(&d, cls, sizeof cls);
            snprintf(rr->cls, sizeof rr->cls, "%s: %s", why, cls);
            return;
        }
        if (draw && has_area && !d.read) {
            jb_vram(a.x, a.y, a.w, a.h, a.after_jb);
            ref_rect(a.x, a.y, a.w, a.h, a.after_ref);
            d.small = small_diff(&a);
            /* st is the state after the command: E1h with its page */
            ablate(c, &st, &a, &d);
        }
        if (filter == RUN_FILTER && !read_bad) {
            int k = fd_divergence_of(&d);
            if (k >= 0 && k == unfilter_div) {
                rr->first = i;
                rr->d = df;
                rr->desc = d;
                snprintf(rr->cls, sizeof rr->cls, "divergence %d", k);
                rec_cut(rec_cmd);
                return;
            }
            if (k >= 0) {
                ++rr->div[k];
                if (df.colour || df.mask) {
                    int w = df.x1 - df.x0 + 1, h = df.y1 - df.y0 + 1;
                    jb_vram(df.x0, df.y0, w, h, jb_pix);
                    load_side(0, 1, df.x0, df.y0, w, h, jb_pix);
                    rec_load(df.x0, df.y0, w, h, jb_pix);
                    send_state(&st, 0);
                }
                continue;
            }
        }
        rr->first = i;
        rr->d = df;
        rr->desc = d;
        fd_class(&d, rr->cls, sizeof rr->cls);
        rec_cut(rec_cmd);
        return;
    }
    /* jb_all against the public read, once a stream (jb_all) */
    jb_vram(0, 0, FD_VRAM_W, FD_VRAM_H, jb_pix);
    if (memcmp(jb_pix, jb_all(), (size_t)FD_VRAM_PIXELS * 2) != 0) {
        fprintf(stderr, "fuzz-diff: jitterbug's VRAM read in place differs from jb_vram_read\n");
        exit(2);
    }
    compare(0, 0, 1024, 512, &rr->d);
    if (rr->d.colour || rr->d.mask) {
        rr->first = s->n;
        rr->unattributed = 1;
        snprintf(rr->cls, sizeof rr->cls, "unattributed");
    }
}

/* ------------------------------------------------------------------------ */
/* Shrinking */

static int still(const fd_stream *s, int filter, const char *cls, result *rr)
{
    long other = 0;
    int k;

    run(s, filter, rr);
    if (rr->first < 0 || strcmp(rr->cls, cls) != 0)
        return 0;
    /* a divergence's reproducer: other classes in it may only go away */
    for (k = 0; k < MAX_DIV; ++k)
        other += rr->div[k];
    if (unfilter_div >= 0 && other > best_other)
        return 0;
    best_other = other;
    return 1;
}

static void truncate_after(fd_stream *s, int last)
{
    if (last + 1 < s->n)
        fd_stream_remove(s, last + 1, s->n - last - 1);
}

/* Tries replacing word k of command c with v; keeps it if the class
   stays. */
static int try_word(fd_stream *s, int c, int k, uint32_t v, int filter, const char *cls)
{
    uint32_t old = s->c[c].w[k];
    result rr;

    if (old == v)
        return 0;
    s->c[c].w[k] = v;
    if (still(s, filter, cls, &rr) && rr.first == s->n - 1)
        return 1;
    s->c[c].w[k] = old;
    return 0;
}

/* Tries removing words [k, k + n) of command c (a polyline's vertex). */
static int try_cut(fd_stream *s, int c, int k, int n, int filter, const char *cls)
{
    fd_cmd *cm = &s->c[c];
    uint32_t *save = must(malloc((size_t)cm->n * 4));
    int old_n = cm->n;
    result rr;

    memcpy(save, cm->w, (size_t)cm->n * 4);
    memmove(cm->w + k, cm->w + k + n, (size_t)(cm->n - k - n) * 4);
    cm->n -= n;
    if (still(s, filter, cls, &rr) && rr.first == s->n - 1) {
        free(save);
        return 1;
    }
    memcpy(cm->w, save, (size_t)old_n * 4);
    cm->n = old_n;
    free(save);
    return 0;
}

static void shrink_culprit(fd_stream *s, int filter, const char *cls)
{
    int c = s->n - 1, changed = 1, rounds = 0;

    while (changed && rounds++ < 8) {
        fd_cmd *cm = &s->c[c];
        uint32_t op = cm->w[0] >> 24;
        int k;

        changed = 0;
        /* polylines: drop vertices */
        if ((op >> 5) == 2 && (op & 0x08)) {
            int per = (op & 0x10) ? 2 : 1;
            for (k = cm->n - 1 - per; k >= 1 + 2 * per - (per == 2 ? 1 : 0); k -= per)
                if (cm->n - per > 2 + 2 * per && try_cut(s, c, k, per, filter, cls))
                    changed = 1;
            cm = &s->c[c];
        }
        /* the colour of the command word, and any word to a simpler one */
        for (k = 0; k < cm->n; ++k) {
            uint32_t w = s->c[c].w[k];
            if (k == 0) {
                if (try_word(s, c, 0, (w & 0xff000000u) | 0x808080u, filter, cls) ||
                    try_word(s, c, 0, (w & 0xff000000u), filter, cls))
                    changed = 1;
                continue;
            }
            /* halve each 16-bit half (positions, sizes, texture words) */
            {
                int lo = (int)(int16_t)(w & 0xffff), hi = (int)(int16_t)(w >> 16);
                uint32_t a = ((uint32_t)(lo / 2) & 0xffffu) | (w & 0xffff0000u);
                uint32_t b = (w & 0xffffu) | ((uint32_t)(hi / 2) & 0xffffu) << 16;
                if (try_word(s, c, k, a, filter, cls))
                    changed = 1, w = s->c[c].w[k];
                if (try_word(s, c, k, (w & 0xffffu) | (b & 0xffff0000u), filter, cls))
                    changed = 1;
            }
        }
    }
}

/* Reduces a stream whose first difference (class cls) is at command
   `first` to a small one with the same class at its last command. */
static void shrink(fd_stream *s, int first, int filter, const char *cls)
{
    int chunk, i;
    result rr;

    best_other = 1L << 40;
    truncate_after(s, first);
    if (first >= s->n) /* unattributed */
        return;
    for (chunk = (s->n - 1) / 2; chunk >= 1; chunk /= 2) {
        for (i = 0; i + chunk <= s->n - 1;) {
            fd_stream t;
            fd_stream_copy(&t, s);
            fd_stream_remove(&t, i, chunk);
            if (still(&t, filter, cls, &rr)) {
                truncate_after(&t, rr.first);
                fd_stream_free(s);
                *s = t;
            } else {
                fd_stream_free(&t);
                i += chunk;
            }
        }
    }
    shrink_culprit(s, filter, cls);
}

/* Strict and bounded streams: drops whole groups (fd.h), never the failing
   command's, and changes no word, so that the reproducer stays inside the
   subset it was generated in. Each background load is a group of its own. */
static int unit_starts(const fd_stream *s, int *starts)
{
    int i, n = 0;

    for (i = 0; i < s->n; ++i)
        if (i == 0 || s->c[i].group <= 0 || s->c[i].group != s->c[i - 1].group)
            starts[n++] = i;
    starts[n] = s->n;
    return n;
}

static void shrink_groups(fd_stream *s, int first, int filter, const char *cls)
{
    int chunk, i, n, *starts;
    result rr;

    best_other = 1L << 40;
    truncate_after(s, first);
    if (first >= s->n)
        return;
    starts = must(malloc((size_t)(s->n + 1) * sizeof *starts));
    n = unit_starts(s, starts);
    for (chunk = (n - 1) / 2; chunk >= 1; chunk /= 2) {
        for (i = 0; i + chunk <= n - 1;) {
            fd_stream t;
            fd_stream_copy(&t, s);
            fd_stream_remove(&t, starts[i], starts[i + chunk] - starts[i]);
            if (still(&t, filter, cls, &rr)) {
                truncate_after(&t, rr.first);
                fd_stream_free(s);
                *s = t;
                n = unit_starts(s, starts);
            } else {
                fd_stream_free(&t);
                i += chunk;
            }
        }
    }
    free(starts);
}

/* ------------------------------------------------------------------------ */
/* Lists (tests/conformance/FORMAT.md) */

typedef struct {
    uint32_t *w;
    size_t n, cap;
} wbuf;

static void wput(wbuf *b, uint32_t v)
{
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 1024;
        b->w = must(realloc(b->w, b->cap * 4));
    }
    b->w[b->n++] = v;
}

static void wstr(wbuf *b, const char *s)
{
    size_t len = strlen(s), i;
    uint32_t v = 0;

    wput(b, (uint32_t)len);
    for (i = 0; i < len; ++i) {
        v |= (uint32_t)(unsigned char)s[i] << (8 * (i % 4));
        if (i % 4 == 3) {
            wput(b, v);
            v = 0;
        }
    }
    if (len % 4)
        wput(b, v);
}

/* Writes s as a list (tests/conformance/FORMAT.md): GP1(00h), then, with
   prelude, the fills that clear VRAM and the stream (the open lists);
   without, s alone (a transcript, which starts with those fills: run()). */
static int save_list(const char *path, const char *name, const char *source, const fd_stream *s,
                     int prelude)
{
    wbuf b = {0};
    FILE *f;
    int i;

    wput(&b, 0x4c43424au); /* "JBCL" */
    wput(&b, 0x00545349u); /* "IST\0" */
    wput(&b, JBC_VERSION);
    wput(&b, 0);
    wstr(&b, name);
    wstr(&b, source);
    b.w[3] = (uint32_t)(b.n * 4);
    wput(&b, (uint32_t)JBC_OP(JBC_GP1, 1));
    wput(&b, 0);
    if (prelude) {
        size_t at, before;
        wput(&b, (uint32_t)JBC_OP(JBC_CALL, 0));
        at = b.n - 1;
        before = b.n;
        wstr(&b, "fuzz-diff: VRAM cleared");
        b.w[at] = (uint32_t)JBC_OP(JBC_CALL, b.n - before);
        wput(&b, (uint32_t)JBC_OP(JBC_GP0, 12));
        for (i = 0; i < 12; ++i)
            wput(&b, prelude_fills[i]);
    }
    {
        size_t at, before;
        wput(&b, (uint32_t)JBC_OP(JBC_CALL, 0));
        at = b.n - 1;
        before = b.n;
        wstr(&b, prelude ? "fuzz-diff: the stream"
                         : "fuzz-diff: what both sides were sent, as run (repro --out)");
        b.w[at] = (uint32_t)JBC_OP(JBC_CALL, b.n - before);
    }
    for (i = 0; i < s->n; ++i) {
        const fd_cmd *c = &s->c[i];
        int k;
        if (c->n == 0)
            continue;
        wput(&b, (uint32_t)JBC_OP(JBC_GP0, c->n));
        for (k = 0; k < c->n; ++k)
            wput(&b, c->w[k]);
        if (c->reads > 0) {
            wput(&b, (uint32_t)JBC_OP(JBC_READ, 2));
            wput(&b, JBC_PORT_GPUREAD);
            wput(&b, (uint32_t)c->reads);
        }
    }
    wput(&b, (uint32_t)JBC_OP(JBC_END, 0));
    f = fopen(path, "wb");
    if (f == NULL || fwrite(b.w, 4, b.n, f) != b.n) {
        fprintf(stderr, "fuzz-diff: cannot write %s\n", path);
        if (f)
            fclose(f);
        free(b.w);
        return -1;
    }
    fclose(f);
    free(b.w);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Playing lists on the reference: GP0 words with a small decoder that
   finds C0h commands (for GPUREAD, which psxstack does not model) */

typedef struct {
    int need, have, polyline, gouraud, pv;
    long load_left;
    uint32_t cmd[16];
    uint32_t c0[3];
    int c0_ready;
    uint32_t pend[FD_VRAM_PIXELS / 2]; /* the largest transfer's words */
    long pend_n, pend_at;
} tracker;

static int cmd_len(uint32_t op)
{
    switch (op >> 5) {
    case 1: {
        int verts = (op & 8) ? 4 : 3, per = 1 + ((op & 4) ? 1 : 0) + ((op & 0x10) ? 1 : 0);
        return 1 + verts * per - ((op & 0x10) ? 1 : 0);
    }
    case 2:
        return (op & 0x10) ? 4 : 3;
    case 3:
        return 2 + ((op & 4) ? 1 : 0) + (((op >> 3) & 3) == 0 ? 1 : 0);
    case 4:
        return 4;
    case 5:
    case 6:
        return 3;
    default:
        return op == 0x02 ? 3 : 1;
    }
}

static void track(tracker *t, uint32_t w)
{
    ref_gp0(&w, 1);
    if (t->load_left > 0) {
        t->load_left -= 2;
        return;
    }
    if (t->polyline) {
        /* psxstack's rule: the terminator where a vertex (or its colour)
           starts, after the first segment */
        if ((w & 0xf000f000u) == 0x50005000u && (!t->gouraud || t->pv == 0)) {
            t->polyline = 0;
            return;
        }
        t->pv = (t->pv + 1) % (t->gouraud ? 2 : 1);
        return;
    }
    t->cmd[t->have++] = w;
    if (t->have == 1)
        t->need = cmd_len(w >> 24);
    if (t->have < t->need)
        return;
    t->have = 0;
    {
        uint32_t op = t->cmd[0] >> 24;
        if ((op >> 5) == 2 && (op & 8)) {
            t->polyline = 1;
            t->gouraud = (op & 0x10) != 0;
            t->pv = 0;
        } else if ((op >> 5) == 5) {
            long rw = (long)((((t->cmd[2] & 0xffff) - 1) & 0x3ff) + 1);
            long rh = (long)(((((t->cmd[2] >> 16) & 0xffff) - 1) & 0x1ff) + 1);
            t->load_left = rw * rh;
        } else if ((op >> 5) == 6) {
            long rw = (long)((((t->cmd[2] & 0xffff) - 1) & 0x3ff) + 1);
            long rh = (long)(((((t->cmd[2] >> 16) & 0xffff) - 1) & 0x1ff) + 1);
            long n = (rw * rh + 1) / 2, k;
            for (k = 0; k < n; ++k)
                t->pend[k] = ref_read_word(t->cmd, k);
            t->pend_n = n;
            t->pend_at = 0;
        }
    }
}

static uint32_t track_read(tracker *t)
{
    static uint32_t last;
    if (t->pend_at < t->pend_n)
        last = t->pend[t->pend_at++];
    return last;
}

/* Plays a list on the reference: GP0 and GP0_DMA words, GP1(00h) as
   psxstack's drawing-state reset (ref_reset_state), GP1(00h) and GP1(01h)
   dropping the tracker's command in progress, READs of GPUREAD (others
   read 0), the main loop until a pass changes nothing. Checks EXPECTs on
   GPUREAD words. Every value read is kept, in order, in *reads_out
   (port_out: whether it was GPUREAD's) when they are not NULL (free
   both). */
static void ref_play(const jbc_list *l, unsigned *checks, unsigned *failed, unsigned *loops,
                     unsigned *skipped, uint32_t **reads_out, unsigned char **port_out,
                     unsigned *nreads_out)
{
    static tracker t;
    uint32_t *reads = NULL;
    unsigned char *port_ok = NULL;
    unsigned nreads = 0, cap = 0, pass;
    uint16_t *before = must(malloc((size_t)FD_VRAM_PIXELS * 2));

    memset(&t, 0, sizeof t);
    ref_reset();
    *checks = *failed = *skipped = 0;
    *loops = 0;
    for (pass = 0; pass < 1000; ++pass) {
        size_t i = pass == 0 ? l->first : l->loop;
        if (pass > 0)
            memcpy(before, ref_vram(), (size_t)FD_VRAM_PIXELS * 2);
        while (i < l->n) {
            uint32_t op = l->w[i];
            unsigned code = JBC_OP_CODE(op);
            unsigned long n = JBC_OP_COUNT(op), k;
            const uint32_t *p = l->w + i + 1;
            if (code == JBC_END)
                break;
            if (code == JBC_LOOP && pass == 0) {
                i += 1 + n;
                continue;
            }
            switch (code) {
            case JBC_GP0:
            case JBC_GP0_DMA:
                for (k = 0; k < n; ++k)
                    track(&t, p[k]);
                break;
            case JBC_GP1:
                for (k = 0; k < n; ++k) {
                    unsigned c = (p[k] >> 24) & 0x3f;
                    if (c == 0 || c == 1) {
                        /* psxstack models no GP1: what a reset drops */
                        t.have = 0, t.polyline = 0, t.load_left = 0, t.pend_n = 0;
                    }
                    if (c == 0) /* and GP1(00h)'s drawing state (ref.h) */
                        ref_reset_state();
                }
                break;
            case JBC_READ:
                for (k = 0; k < p[1]; ++k) {
                    uint32_t v = p[0] == JBC_PORT_GPUREAD ? track_read(&t) : 0;
                    if (nreads == cap) {
                        cap = cap ? cap * 2 : 4096;
                        reads = must(realloc(reads, (size_t)cap * sizeof *reads));
                        port_ok = must(realloc(port_ok, (size_t)cap));
                    }
                    reads[nreads] = v;
                    port_ok[nreads] = p[0] == JBC_PORT_GPUREAD;
                    ++nreads;
                }
                break;
            case JBC_EXPECT:
                /* psxstack models GPUREAD only (through the tracker): an
                   EXPECT on any other port has no value to check, and is
                   counted apart */
                if (p[0] < nreads && !port_ok[p[0]]) {
                    ++*skipped;
                } else if (p[0] < nreads) {
                    ++*checks;
                    if ((reads[p[0]] & p[1]) != p[2])
                        ++*failed;
                }
                break;
            default:
                break;
            }
            i += 1 + n;
        }
        if (l->loop == 0)
            break;
        if (pass > 0 && memcmp(before, ref_vram(), (size_t)FD_VRAM_PIXELS * 2) == 0)
            break;
        ++*loops;
    }
    free(before);
    if (reads_out != NULL) {
        *reads_out = reads;
        *port_out = port_ok;
        *nreads_out = nreads;
    } else {
        free(reads);
        free(port_ok);
    }
}

/* ------------------------------------------------------------------------ */
/* Modes */

/* The largest stream index and count (--first, --streams, repro's
   INDEX), and commands a stream (--commands), the command line accepts. */
#define MAX_INDEX 0x7fffffffL
enum { MAX_COMMANDS = 100000 };

typedef struct {
    uint64_t seed;
    long first, streams;
    fd_gen_options gen;
    int filter, repros, quiet, save_divergences;
    int whole;        /* repro: the stream as it is, saved whatever it shows */
    int check;        /* 0 exploratory (run), 1 strict, 2 bounded */
    long max_failures; /* strict, bounded: stop after this many failing streams */
    const char *out;
} options;

static void generate(const options *o, long index, fd_stream *s)
{
    if (o->check == 1)
        fd_generate_strict(o->seed, (uint64_t)index, o->gen.commands, s);
    else if (o->check == 2)
        fd_generate_bounded(o->seed, (uint64_t)index, o->gen.commands, s);
    else
        fd_generate(o->seed, (uint64_t)index, &o->gen, s);
}

/* How run() is to treat a difference. */
static int how(const options *o)
{
    return o->check == 2 ? RUN_BOUNDED : o->check == 1 ? RUN_EXACT : o->filter ? RUN_FILTER : RUN_EXACT;
}

typedef struct {
    char cls[256];
    long count, colour, mask;
    long seed_index; /* the first stream it was seen in */
    int saved;
    char repro[1024];
} class_entry;

enum { MAX_CLASSES = 4096 };
static class_entry classes[MAX_CLASSES];
static int nclasses;

static class_entry *class_of(const char *cls)
{
    int i;

    for (i = 0; i < nclasses; ++i)
        if (strcmp(classes[i].cls, cls) == 0)
            return &classes[i];
    if (nclasses == MAX_CLASSES)
        return &classes[MAX_CLASSES - 1];
    memset(&classes[nclasses], 0, sizeof classes[0]);
    snprintf(classes[nclasses].cls, sizeof classes[0].cls, "%s", cls);
    classes[nclasses].seed_index = -1;
    return &classes[nclasses++];
}

static void slug(const char *cls, char *out, size_t size)
{
    size_t k = 0;
    const char *p;
    int dash = 0;

    for (p = cls; *p && k + 1 < size; ++p) {
        char ch = *p;
        int ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z');
        if (ok) {
            out[k++] = ch;
            dash = 0;
        } else if (!dash && k > 0) {
            out[k++] = '-';
            dash = 1;
        }
    }
    while (k > 0 && out[k - 1] == '-')
        --k;
    out[k] = 0;
}

static void print_stream(const fd_stream *s)
{
    int i, k;
    fd_state st;

    fd_state_reset(&st);
    for (i = 0; i < s->n; ++i) {
        const fd_cmd *c = &s->c[i];
        fd_desc d;
        char cls[256];
        fd_describe(c, &st, &d);
        d.isolated = -1; /* no ablation here: the plain description */
        fd_class(&d, cls, sizeof cls);
        if (c->background) {
            printf("  [%d] background load at %u,%u (%d words)\n", i, c->w[1] & 0xffff,
                   c->w[1] >> 16, c->n);
        } else {
            printf("  [%d] %-40s", i, cls);
            for (k = 0; k < c->n && k < 24; ++k)
                printf(" %08x", c->w[k]);
            if (c->n > 24)
                printf(" ... (%d words)", c->n);
            if (c->reads)
                printf(" + %d GPUREAD", c->reads);
            printf("\n");
        }
        fd_state_apply(&st, c);
    }
}

static int parse_u64(const char *s, uint64_t min, uint64_t max, uint64_t *out);

/* FD_SHOW=x,y,w,h: both sides' pixels there, jitterbug's first; a
   rectangle inside VRAM, or exit 2. */
static void show(void)
{
    const char *env = getenv("FD_SHOW");
    char buf[64], *f[4], *p;
    uint64_t v[4];
    int x, y, w, h, j, k, nf = 0;

    if (env == NULL)
        return;
    snprintf(buf, sizeof buf, "%s", env);
    f[nf++] = buf;
    for (p = buf; *p; ++p)
        if (*p == ',') {
            if (nf == 4) {
                nf = 5; /* too many */
                break;
            }
            *p = 0;
            f[nf++] = p + 1;
        }
    if (strlen(env) >= sizeof buf || nf != 4 || parse_u64(f[0], 0, FD_VRAM_W - 1, &v[0]) ||
        parse_u64(f[1], 0, FD_VRAM_H - 1, &v[1]) || parse_u64(f[2], 1, FD_VRAM_W, &v[2]) ||
        parse_u64(f[3], 1, FD_VRAM_H, &v[3]) || v[0] + v[2] > FD_VRAM_W ||
        v[1] + v[3] > FD_VRAM_H) {
        fprintf(stderr, "fuzz-diff: FD_SHOW must be x,y,w,h, a rectangle inside the 1024 x 512 "
                        "VRAM, not '%s'\n",
                env);
        exit(2);
    }
    x = (int)v[0], y = (int)v[1], w = (int)v[2], h = (int)v[3];
    jb_vram(x, y, w, h, jb_pix);
    for (j = 0; j < h; ++j) {
        printf("  y %3d jb :", y + j);
        for (k = 0; k < w; ++k)
            printf(" %04x", jb_pix[j * w + k]);
        printf("\n  y %3d ref:", y + j);
        for (k = 0; k < w; ++k)
            printf(" %04x", ref_vram()[(y + j) * FD_VRAM_W + x + k]);
        printf("\n");
    }
}

/* Where and what a failing run found, on one line (no newline). */
static void print_failure(const result *rr)
{
    if (rr->prelude)
        printf("%s", rr->cls);
    else
        printf("%s at command %d", rr->cls, rr->first);
    printf(": %ld colour, %ld mask pixels differ in (%d,%d)-(%d,%d)", rr->d.colour, rr->d.mask,
           rr->d.x0, rr->d.y0, rr->d.x1, rr->d.y1);
    if (rr->read_bad)
        printf("; %ld GPUREAD words differ, the first word %ld", rr->read_bad, rr->read_first);
    if (rr->protected_jb || rr->protected_ref)
        printf("; protected pixels changed: %ld by jitterbug, %ld by psxstack", rr->protected_jb,
               rr->protected_ref);
}

/* Numbers from the command line, FD_SHOW and words files: decimal digits
   (or 0x and hexadecimal ones), no sign, no space, no leading zero before
   another digit (strtol's base 0 read those as octal), no overflow, and
   within [min, max]; 0 if s is one. */
static int parse_u64(const char *s, uint64_t min, uint64_t max, uint64_t *out)
{
    uint64_t v = 0;
    int base = 10, digits = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        base = 16, s += 2;
    else if (s[0] == '0' && s[1] != 0)
        return -1;
    for (; *s; ++s, ++digits) {
        int d;
        if (*s >= '0' && *s <= '9')
            d = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f')
            d = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F')
            d = *s - 'A' + 10;
        else
            return -1;
        if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base)
            return -1;
        v = v * (uint64_t)base + (uint64_t)d;
    }
    if (digits == 0 || v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

/* A number argument (what names it) in [min, max], or exit 2. */
static uint64_t arg_number(const char *what, const char *s, uint64_t min, uint64_t max)
{
    uint64_t v;

    if (s == NULL || parse_u64(s, min, max, &v) != 0) {
        fprintf(stderr,
                "fuzz-diff: %s must be a decimal (or 0x hexadecimal) number from %llu to %llu, "
                "not '%s'\n",
                what, (unsigned long long)min, (unsigned long long)max, s ? s : "");
        exit(2);
    }
    return v;
}

/* One line of a words file, of any length, into *buf (newline dropped);
   0 at the end of the file, 2 for a line with a zero byte in it (which
   would hide the rest of the line from the parser), 1 otherwise. */
static int read_line(FILE *f, char **buf, size_t *cap)
{
    size_t n = 0;
    int ch, zero = 0;

    if (*buf == NULL) {
        *cap = 4096;
        *buf = must(malloc(*cap));
    }
    while ((ch = fgetc(f)) != EOF && ch != '\n') {
        if (n + 1 == *cap) {
            *cap *= 2;
            *buf = must(realloc(*buf, *cap));
        }
        if (ch == 0)
            zero = 1;
        (*buf)[n++] = (char)ch;
    }
    (*buf)[n] = 0;
    if (ch == EOF && n == 0)
        return 0;
    return zero ? 2 : 1;
}

static int is_hex(char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

/* fuzz-diff words FILE: one command per line, as hexadecimal words (a
   C0h line may end with "r N": N GPUREAD words); run once, unfiltered.
   Each word is 1 to 8 hexadecimal digits (0x allowed), N decimal, at most
   the largest transfer's words; a # starts a comment to the end of the
   line; anything else on a line fails (exit 2). */
static int mode_words(const char *path, const char *out)
{
    FILE *f = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0, wcap = 0;
    uint32_t *w = NULL;
    long lineno = 0;
    int got;
    fd_stream s;
    result rr;

    memset(&s, 0, sizeof s);
    if (f == NULL) {
        fprintf(stderr, "fuzz-diff: cannot read %s\n", path);
        return 2;
    }
    while ((got = read_line(f, &line, &cap)) != 0) {
        size_t n = 0;
        int reads = 0;
        char *p = line;
        const char *bad = got == 2 ? "a zero byte" : NULL;
        ++lineno;
        if (!bad && line[0] == '#')
            continue;
        while (!bad) {
            char *t;
            while (*p == ' ' || *p == '\t' || *p == '\r')
                ++p;
            if (*p == 0 || *p == '#') /* the end, or a comment to it */
                break;
            if (*p == 'r') {
                uint64_t v;
                char save;
                ++p;
                while (*p == ' ' || *p == '\t')
                    ++p;
                for (t = p; *t >= '0' && *t <= '9'; ++t)
                    ;
                save = *t;
                *t = 0;
                if (n == 0)
                    bad = "a read count with no command";
                else if (parse_u64(p, 0, FD_VRAM_PIXELS / 2, &v) != 0)
                    bad = "a read count that is not a decimal number from 0 to 262144";
                else
                    reads = (int)v;
                *t = save;
                p = t;
                while (*p == ' ' || *p == '\t' || *p == '\r')
                    ++p;
                if (!bad && *p != 0 && *p != '#')
                    bad = "something after the read count";
                break;
            }
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
                p += 2;
            for (t = p; is_hex(*t); ++t)
                ;
            if (t == p || t - p > 8 ||
                (*t != 0 && *t != ' ' && *t != '\t' && *t != '\r' && *t != '#')) {
                bad = "a word that is not 1 to 8 hexadecimal digits";
                break;
            }
            if (n == wcap) {
                wcap = wcap ? wcap * 2 : 1024;
                w = must(realloc(w, wcap * sizeof *w));
            }
            w[n++] = (uint32_t)strtoul(p, NULL, 16);
            p = t;
        }
        if (bad || n > (size_t)INT32_MAX) {
            fprintf(stderr, "fuzz-diff: %s:%ld: %s\n", path, lineno,
                    bad ? bad : "too many words");
            fclose(f);
            free(line);
            free(w);
            fd_stream_free(&s);
            return 2;
        }
        if (n > 0)
            fd_stream_add(&s, w, (int)n)->reads = reads;
    }
    fclose(f);
    free(line);
    free(w);
    {
        fd_stream tr;
        memset(&tr, 0, sizeof tr);
        rec = out ? &tr : NULL;
        run(&s, 0, &rr);
        rec = NULL;
        if (rr.first < 0)
            printf("no difference\n");
        else {
            print_failure(&rr);
            printf("\n");
        }
        if (out) {
            /* the transcript, as `repro --out` saves one */
            char file[1024], sl[200], src[1200];
            const char *base = strrchr(path, '/');
            slug(base ? base + 1 : path, sl, sizeof sl);
            snprintf(file, sizeof file, "%s/words-%s.jbc", out, sl);
            snprintf(src, sizeof src,
                     "tools/fuzz-diff words %s; what both sides were sent, as run; against "
                     "psxstack " PSXSTACK_REV,
                     base ? base + 1 : path);
            if (save_list(file, "fuzz/words", src, &tr, 0) == 0)
                printf("saved %s (%d commands, %ld words)\n", file, tr.n, fd_stream_words(&tr));
        }
        fd_stream_free(&tr);
    }
    show();
    fd_stream_free(&s);
    return rr.first >= 0;
}

/* One command on zeros, full drawing area, no offset, dithering and mask
   settings off: whether the two sides differ. */
static int alone(const uint32_t *w, int n)
{
    static const uint32_t state[] = {0xe1000000u, 0xe2000000u, 0xe3000000u, 0xe407ffffu,
                                     0xe5000000u, 0xe6000000u};
    diffs d;

    sides_reset();
    send_both(state, 6);
    send_both(w, n);
    compare(0, 0, 1024, 512, &d);
    return d.colour != 0 || d.mask != 0;
}

/* fuzz-diff assumptions: where psxstack can speak to docs/conformance.md's
   "Assumptions beyond the evidence", the cases built so
   that its rules refuted elsewhere do not interfere. */
static int mode_assumptions(void)
{
    static const char *const oct[8] = {"x-major right-down", "x-major right-up",
                                       "x-major left-down",  "x-major left-up",
                                       "y-major down-right", "y-major down-left",
                                       "y-major up-right",   "y-major up-left"};
    int o, major, agree, total;

    printf("Line ties: flat lines with a minor length of 1 and an even major length, so that "
           "the one inexact step is an exact half (psxstack's bias is 0 there)\n");
    for (o = 0; o < 8; ++o) {
        agree = total = 0;
        for (major = 2; major <= 60; major += 2) {
            int x0 = 300, y0 = 200, dx, dy;
            uint32_t w[3];
            int ymaj = o >= 4, s1 = (o & 1) ? -1 : 1, s0 = (o & 2) ? -1 : 1;
            dx = ymaj ? s1 * 1 : s0 * major;
            dy = ymaj ? s0 * major : s1 * 1;
            w[0] = 0x40f8f8f8u;
            w[1] = (uint32_t)x0 | (uint32_t)y0 << 16;
            w[2] = (uint32_t)(x0 + dx) | (uint32_t)(y0 + dy) << 16;
            ++total;
            agree += !alone(w, 3);
        }
        printf("  %-20s %2d of %2d lines the same\n", oct[o], agree, total);
    }
    printf("Gouraud line colours, x-major lines drawn from their first vertex (left to right, "
           "horizontal), red from c0 to c1, undithered:\n");
    {
        int dir;
        for (dir = 0; dir < 3; ++dir) {
            long px = 0;
            agree = total = 0;
            for (major = 1; major <= 255; major += 3) {
                int c0, c1;
                uint32_t w[4];
                diffs d;
                c0 = dir == 0 ? 0 : 255;
                c1 = dir == 0 ? 255 : 0;
                w[0] = 0x50000000u | (uint32_t)c0;
                w[1] = dir == 2 ? (uint32_t)(300 + major) | 200u << 16 : 300u | 200u << 16;
                w[2] = (uint32_t)c1;
                w[3] = dir == 2 ? 300u | 200u << 16 : (uint32_t)(300 + major) | 200u << 16;
                ++total;
                agree += !alone(w, 4);
                compare(0, 0, 1024, 512, &d);
                px += d.colour;
            }
            printf("  %-34s %2d of %2d lines the same (%ld pixels differ)\n",
                   dir == 0   ? "steps up (the dump's case)"
                   : dir == 1 ? "steps down"
                              : "steps down, drawn right to left",
                   agree, total, px);
        }
    }
    return 0;
}

/* Shrinks and saves stream (seed, index), whose class is cls. */
static void reproduce(const options *o, long index, const char *cls, class_entry *ce, int verbose)
{
    fd_stream s;
    result rr;
    char name[256], path[1024], sl[200], source[512];

    fd_stream tr;

    generate(o, index, &s);
    run(&s, how(o), &rr);
    if (!o->whole && (rr.first < 0 || strcmp(rr.cls, cls) != 0)) {
        printf("  (stream %ld no longer shows %s)\n", index, cls);
        fd_stream_free(&s);
        return;
    }
    if (rr.prelude) /* no command of the stream is needed */
        fd_stream_remove(&s, 0, s.n);
    else if (o->whole)
        ;
    else if (o->check)
        shrink_groups(&s, rr.first, how(o), cls);
    else
        shrink(&s, rr.first, how(o), cls);
    /* the last run, recorded: what is saved */
    memset(&tr, 0, sizeof tr);
    rec = &tr;
    run(&s, how(o), &rr);
    rec = NULL;
    show();
    if (strncmp(cls, "divergence ", 11) == 0)
        snprintf(sl, sizeof sl, "%s", fd_divergences[atoi(cls + 11)].id);
    else
        slug(rr.first >= 0 ? rr.cls : "no-difference", sl, sizeof sl);
    if (strlen(sl) > 60)
        sl[60] = 0;
    snprintf(name, sizeof name, "fuzz/%s", sl);
    snprintf(source, sizeof source,
             "tools/fuzz-diff %s seed %llu stream %ld focus %s%s%s, %s; what both sides were "
             "sent, as run; against psxstack " PSXSTACK_REV,
             o->check == 1 ? "strict" : o->check == 2 ? "bounded" : "run",
             (unsigned long long)o->seed, index, fd_focus_names[o->gen.focus],
             o->gen.avoid_unmodelled ? "" : " --unmodelled", o->filter ? "" : " --no-filter",
             o->whole ? "whole" : "shrunk");
    if (o->out) {
        if (o->whole)
            snprintf(path, sizeof path, "%s/whole-%llu-%ld.jbc", o->out,
                     (unsigned long long)o->seed, index);
        else
            snprintf(path, sizeof path, "%s/%s.jbc", o->out, sl);
        if (save_list(path, name, source, &tr, 0) == 0) {
            if (ce)
                snprintf(ce->repro, sizeof ce->repro, "%s", path);
            printf("  saved %s (%d commands, %ld words)\n", path, tr.n, fd_stream_words(&tr));
        }
    }
    fd_stream_free(&tr);
    if (verbose) {
        printf("  minimal (%d commands, %ld words): %ld colour, %ld mask pixels differ in "
               "(%d,%d)-(%d,%d)\n",
               s.n, fd_stream_words(&s), rr.d.colour, rr.d.mask, rr.d.x0, rr.d.y0, rr.d.x1,
               rr.d.y1);
        print_stream(&s);
    }
    fd_stream_free(&s);
}

static int mode_run(const options *o)
{
    long i, prims = 0, words = 0, differing = 0, div_total[MAX_DIV] = {0};
    int c, unknown = 0;

    for (i = o->first; i < o->first + o->streams; ++i) {
        fd_stream s;
        result rr;

        generate(o, i, &s);
        words += fd_stream_words(&s);
        run(&s, how(o), &rr);
        prims += rr.prims;
        for (c = 0; c < fd_divergence_count && c < MAX_DIV; ++c) {
            if (o->save_divergences && o->filter && div_total[c] == 0 && rr.div[c] > 0) {
                char cls[64];
                snprintf(cls, sizeof cls, "divergence %d", c);
                unfilter_div = c;
                printf("divergence %d (%s) first in stream %ld:\n", c, fd_divergences[c].name, i);
                reproduce(o, i, cls, NULL, !o->quiet);
                unfilter_div = -1;
            }
            div_total[c] += rr.div[c];
        }
        if (rr.first >= 0) {
            class_entry *ce = class_of(rr.cls);
            ++differing;
            ++ce->count;
            ce->colour += rr.d.colour;
            ce->mask += rr.d.mask;
            if (ce->seed_index < 0)
                ce->seed_index = i;
            if (!o->quiet && ce->count == 1)
                printf("stream %ld: new class: %s\n", i, rr.cls);
            if (ce->saved < o->repros) {
                ++ce->saved;
                reproduce(o, i, rr.cls, ce, !o->quiet);
            }
        }
        fd_stream_free(&s);
    }
    printf("\nseed %llu, streams %ld..%ld, focus %s%s%s: %ld primitives, %ld words\n",
           (unsigned long long)o->seed, o->first, o->first + o->streams - 1,
           fd_focus_names[o->gen.focus], o->gen.avoid_unmodelled ? "" : ", unmodelled too",
           o->filter ? "" : ", no filter", prims, words);
    printf("labels (commands that differed, then resynchronised; each label a hypothesis "
           "about the cause):\n");
    for (c = 0; c < fd_divergence_count; ++c)
        printf("  %8ld  %s\n", div_total[c], fd_divergences[c].name);
    printf("streams stopped at a difference no label matches: %ld\n", differing);
    for (c = 0; c < nclasses; ++c) {
        printf("  %6ld  %-60s first in stream %ld%s%s\n", classes[c].count, classes[c].cls,
               classes[c].seed_index, classes[c].repro[0] ? ", " : "", classes[c].repro);
        ++unknown;
    }
    printf("exploratory: a label above is a hypothesis about the cause of a difference, not a "
           "waiver;\nfiltered differences are no evidence that jitterbug is right (docs/testing.md). "
           "Exit 0: it ran.\n");
    (void)unknown;
    return 0;
}

/* The family a strict or bounded group's last command belongs to, for the
   counts (docs/testing.md, "Three modes"). */
static const char *family(const fd_cmd *c, const fd_state *st)
{
    fd_desc d;

    fd_describe(c, st, &d);
    switch (d.kind) {
    case FD_K_TRI:
    case FD_K_QUAD:
        if (d.textured)
            return d.gouraud ? "polygon, textured, shading bit" : "polygon, textured, flat";
        return d.gouraud ? "polygon, Gouraud" : "polygon, flat";
    case FD_K_RECT:
        return d.textured ? (d.raw ? "rectangle, textured raw" : "rectangle, textured modulated")
                          : "rectangle, untextured";
    case FD_K_LINE:
        return d.gouraud ? "line, Gouraud" : "line, flat";
    case FD_K_POLYLINE:
        return d.gouraud ? "polyline, Gouraud" : "polyline, flat";
    case FD_K_FILL:
        return "fill";
    case FD_K_COPY:
        return "copy";
    case FD_K_LOAD:
        return "CPU to VRAM";
    case FD_K_READ:
        return "VRAM to CPU";
    default:
        return "other (GP0(01h), NOP)";
    }
}

/* fuzz-diff strict | bounded: the checked subsets; any failure fails. */
static int mode_check(const options *o)
{
    enum { NFAM = 24 };
    const char *fam[NFAM];
    long famn[NFAM] = {0}, i, prims = 0, words = 0, failing = 0, within = 0, same = 0, drew = 0;
    int nfam = 0, f;

    for (i = o->first; i < o->first + o->streams; ++i) {
        fd_stream s;
        result rr;
        fd_state st;
        int k;

        generate(o, i, &s);
        words += fd_stream_words(&s);
        fd_state_reset(&st);
        for (k = 0; k < s.n; ++k) {
            const fd_cmd *c = &s.c[k];
            if (c->group > 0 && (k + 1 == s.n || s.c[k + 1].group != c->group)) {
                const char *name = family(c, &st);
                for (f = 0; f < nfam && strcmp(fam[f], name) != 0; ++f)
                    ;
                if (f == nfam && nfam < NFAM)
                    fam[nfam++] = name;
                if (f < NFAM)
                    ++famn[f];
            }
            fd_state_apply(&st, c);
        }
        run(&s, how(o), &rr);
        prims += rr.prims;
        drew += rr.drew;
        within += rr.bounded_ok;
        same += rr.bounded_same;
        if (rr.first >= 0) {
            ++failing;
            printf("FAIL: stream %ld (seed %llu): ", i, (unsigned long long)o->seed);
            print_failure(&rr);
            printf("\n");
            if (failing <= o->repros)
                reproduce(o, i, rr.cls, NULL, 1);
            if (failing >= o->max_failures) {
                fd_stream_free(&s);
                ++i;
                break;
            }
        }
        fd_stream_free(&s);
    }
    printf("\n%s mode, seed %llu, streams %ld..%ld: %ld drawing commands (%ld changed a pixel), "
           "%ld words\n",
           o->check == 1 ? "strict" : "bounded", (unsigned long long)o->seed, o->first, i - 1,
           prims, drew, words);
    printf("groups by what they end with:\n");
    for (f = 0; f < nfam; ++f)
        printf("  %8ld  %s\n", famn[f], fam[f]);
    if (o->check == 2)
        printf("bounded commands: %ld drew the same pixels as psxstack, %ld differed within one "
               "step per channel\n",
               same, within);
    printf("streams failing: %ld%s\n", failing,
           failing ? (o->check == 1 ? " (any difference in the strict subset fails)"
                                    : " (outside the bound, or in an exact command)")
                   : "");
    return failing ? 1 : 0;
}

/* fuzz-diff hash: a 64-bit FNV-1a hash of the words (and the per-command
   fields) of streams --first.. of --seed, for each generator: strict,
   bounded, and the exploratory one at every focus. For checking that a
   change to the tool, or another compiler, generates the same streams. */
static uint64_t fnv(uint64_t h, uint32_t v)
{
    int k;

    for (k = 0; k < 4; ++k) {
        h ^= (v >> (8 * k)) & 0xffu;
        h *= 0x100000001b3ull;
    }
    return h;
}

static int mode_hash(const options *o)
{
    int g;

    for (g = 0; g < 2 + FD_FOCUS_COUNT; ++g) {
        options p = *o;
        uint64_t h = 0xcbf29ce484222325ull;
        long i;
        p.check = g == 0 ? 1 : g == 1 ? 2 : 0;
        if (p.check && p.gen.commands == 200)
            p.gen.commands = 150;
        if (g >= 2)
            p.gen.focus = (fd_focus)(g - 2);
        for (i = p.first; i < p.first + p.streams; ++i) {
            fd_stream s;
            int k, j;
            generate(&p, i, &s);
            for (k = 0; k < s.n; ++k) {
                const fd_cmd *c = &s.c[k];
                h = fnv(h, (uint32_t)c->n);
                h = fnv(h, (uint32_t)c->reads);
                h = fnv(h, (uint32_t)c->background);
                h = fnv(h, (uint32_t)c->group);
                h = fnv(h, (uint32_t)c->bounded);
                for (j = 0; j < c->n; ++j)
                    h = fnv(h, c->w[j]);
            }
            fd_stream_free(&s);
        }
        printf("%-8s %016llx\n", g == 0 ? "strict" : g == 1 ? "bounded" : fd_focus_names[g - 2],
               (unsigned long long)h);
    }
    return 0;
}

/* The mask check's invariant through a list, command by command, on each
   side apart (honours_mask_check, protected_changed). The GP0 words, of
   GP0 and GP0_DMA ops alike, are split into GPU commands as both decoders
   split them (whatever the ops' grouping: an op may hold several commands,
   or part of one): fixed lengths by the opcode, a polyline to its
   terminator, a CPU-to-VRAM load with its pixel data. Each command is sent
   to both sides once it is complete and checked under the drawing state
   each side has then. GP1(00h) resets jitterbug's drawing state (E1h-E6h
   0, the mask check off) and the reference is given the same reset
   (ref_reset_state), so each side's tracked state follows what its GPU
   does; the other GP1 commands touch nothing the check depends on. READs,
   WAIT, DRAWSYNC, VSYNC, CALL and EXPECT change no VRAM. Returns 0 and the
   protected pixels each side changed; -1 and why (in why) for a list it
   does not step: one with a LOOP, or a GP1(01h) inside a command (which
   jitterbug drops and psxstack, with no GP1, would finish). */
typedef struct {
    uint32_t *w;
    size_t n, cap, need; /* need: 0 while a polyline's end is unknown */
    int polyline, gouraud, verts, want_xy;
} splitter;

static void split_put(splitter *s, uint32_t w)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->w = must(realloc(s->w, s->cap * sizeof *s->w));
    }
    s->w[s->n++] = w;
}

/* Takes one GP0 word; returns 1 when it completes the command in s. */
static int split_word(splitter *s, uint32_t w)
{
    uint32_t op;

    split_put(s, w);
    if (s->n == 1) {
        op = w >> 24;
        s->polyline = (op >> 5) == 2 && (op & 8);
        s->gouraud = (op & 0x10) != 0;
        s->verts = 0;
        s->want_xy = 1;
        s->need = s->polyline ? 0 : (size_t)cmd_len(op);
        return !s->polyline && s->n == s->need;
    }
    if (s->polyline) {
        /* stream.c's rule, psxstack's GPU's too: the terminator where a
           vertex after the second starts */
        if (s->want_xy) {
            s->verts += s->verts < 2;
            s->want_xy = 0;
            return 0;
        }
        if (s->verts >= 2 && (w & 0xf000f000u) == 0x50005000u)
            return 1;
        if (s->gouraud)
            s->want_xy = 1;
        else
            s->verts += s->verts < 2;
        return 0;
    }
    if (s->n == 3 && ((s->w[0] >> 24) >> 5) == 5) {
        size_t rw = (((s->w[2] & 0xffffu) - 1) & 0x3ffu) + 1;
        size_t rh = ((((s->w[2] >> 16) & 0xffffu) - 1) & 0x1ffu) + 1;
        s->need = 3 + (rw * rh + 1) / 2;
    }
    return s->n == s->need;
}

/* The GP1(00h) words of a list that come after a GP0 word, as its ops
   stand (a main loop's counted once): what ref_play gives psxstack as a
   drawing-state reset, counted here whether or not the list is stepped. */
static unsigned resets_after_gp0(const jbc_list *l)
{
    unsigned count = 0, gp0_seen = 0;
    size_t i;

    for (i = l->first; i < l->n;) {
        unsigned code = JBC_OP_CODE(l->w[i]);
        unsigned long n = JBC_OP_COUNT(l->w[i]), k;
        if (code == JBC_END)
            break;
        if ((code == JBC_GP0 || code == JBC_GP0_DMA) && n > 0)
            gp0_seen = 1;
        if (code == JBC_GP1)
            for (k = 0; k < n; ++k)
                if (((l->w[i + 1 + k] >> 24) & 0x3f) == 0)
                    count += gp0_seen;
        i += 1 + n;
    }
    return count;
}

/* Sends the n words of w to both sides, checked when they are a whole
   command (or a polyline or load cut short, which drew what it got). */
static void step_command(const uint32_t *w, size_t n, int whole, fd_state *sj, fd_state *sr,
                         long *prot_jb, long *prot_ref)
{
    static uint16_t *bj, *br;
    uint32_t op = n ? w[0] >> 24 : 0;
    int gj = 0, gr = 0, check;
    fd_cmd c;

    check = whole || ((op >> 5) == 2 && (op & 8) && n >= 2) || ((op >> 5) == 5 && n >= 3);
    memset(&c, 0, sizeof c);
    c.w = (uint32_t *)(uintptr_t)w;
    c.n = (int)n;
    if (check) {
        fd_desc d;
        fd_describe(&c, sj, &d);
        gj = honours_mask_check(&d, &c, sj);
        fd_describe(&c, sr, &d);
        gr = honours_mask_check(&d, &c, sr);
    }
    if (bj == NULL) {
        bj = must(malloc((size_t)FD_VRAM_PIXELS * 2));
        br = must(malloc((size_t)FD_VRAM_PIXELS * 2));
    }
    if (gj)
        memcpy(bj, jb_all(), (size_t)FD_VRAM_PIXELS * 2);
    if (gr)
        memcpy(br, ref_vram(), (size_t)FD_VRAM_PIXELS * 2);
    jb_gp0_or_die(w, n);
    ref_gp0(w, n);
    if (gj)
        *prot_jb += protected_changed(bj, FD_VRAM_W, jb_all(), FD_VRAM_W, FD_VRAM_W, FD_VRAM_H);
    if (gr)
        *prot_ref += protected_changed(br, FD_VRAM_W, ref_vram(), FD_VRAM_W, FD_VRAM_W, FD_VRAM_H);
    if (whole) {
        fd_state_apply(sj, &c);
        fd_state_apply(sr, &c);
    }
}

static int play_stepped(const jbc_list *l, long *prot_jb, long *prot_ref, unsigned *resets,
                        const char **why)
{
    fd_state sj, sr; /* each side's drawing state */
    splitter sp;
    size_t i;
    int gp0_seen = 0, rc = 0;

    *prot_jb = *prot_ref = 0;
    *resets = 0;
    *why = NULL;
    for (i = l->first; i < l->n; i += 1 + JBC_OP_COUNT(l->w[i]))
        if (JBC_OP_CODE(l->w[i]) == JBC_LOOP) {
            *why = "the list loops";
            return -1;
        }
    memset(&sp, 0, sizeof sp);
    jb_new();
    ref_reset();
    fd_state_reset(&sj);
    fd_state_reset(&sr);
    for (i = l->first; i < l->n && rc == 0;) {
        unsigned code = JBC_OP_CODE(l->w[i]);
        unsigned long n = JBC_OP_COUNT(l->w[i]), k;
        const uint32_t *p = l->w + i + 1;
        if (code == JBC_END)
            break;
        if (code == JBC_GP0 || code == JBC_GP0_DMA) {
            gp0_seen = 1;
            for (k = 0; k < n; ++k)
                if (split_word(&sp, p[k])) {
                    step_command(sp.w, sp.n, 1, &sj, &sr, prot_jb, prot_ref);
                    sp.n = 0;
                }
        } else if (code == JBC_GP1) {
            for (k = 0; k < n; ++k) {
                unsigned g = (p[k] >> 24) & 0x3f;
                if (g == 1 && sp.n > 0) {
                    *why = "a GP1(01h) inside a command";
                    rc = -1;
                    break;
                }
                if (g == 0 && sp.n > 0) {
                    /* what came of the command so far, then dropped */
                    step_command(sp.w, sp.n, 0, &sj, &sr, prot_jb, prot_ref);
                    sp.n = 0;
                }
                jb_gp1(jb, p[k]);
                if (g == 0) {
                    fd_state_reset(&sj);
                    ref_reset_state();
                    fd_state_reset(&sr);
                    *resets += gp0_seen;
                }
            }
        } else if (code == JBC_READ) {
            for (k = 0; k < p[1]; ++k)
                if (p[0] == JBC_PORT_GPUREAD)
                    (void)jb_gpuread(jb);
        }
        i += 1 + n;
    }
    if (rc == 0 && sp.n > 0) /* the list ends inside a command */
        step_command(sp.w, sp.n, 0, &sj, &sr, prot_jb, prot_ref);
    free(sp.w);
    return rc;
}

static int mode_play(int argc, char **argv)
{
    int i, bad = 0;

    for (i = 0; i < argc; ++i) {
        jbc_list l;
        char err[256];
        unsigned checks, failed, loops, skipped, nr = 0, r, compared = 0;
        jbc_options jo;
        jbc_outcome out;
        diffs d;
        int j;
        const uint16_t *rv;
        uint32_t *rreads = NULL;
        unsigned char *rport = NULL;
        long rbad = 0, rfirst = -1;

        if (jbc_load(argv[i], &l, err, sizeof err) != 0) {
            fprintf(stderr, "%s: %s\n", argv[i], err);
            return 2;
        }
        memset(&jo, 0, sizeof jo);
        if (jbc_play(&l, &jo, &out, err, sizeof err) != 0) {
            fprintf(stderr, "%s: %s\n", argv[i], err);
            return 2;
        }
        ref_play(&l, &checks, &failed, &loops, &skipped, &rreads, &rport, &nr);
        if (nr != out.nreads) {
            fprintf(stderr, "%s: %u values read on jitterbug, %u on the reference\n", argv[i],
                    out.nreads, nr);
            return 2;
        }
        for (r = 0; r < nr; ++r)
            if (rport[r]) {
                ++compared;
                if (out.reads[r] != rreads[r] && rbad++ == 0)
                    rfirst = r;
            }
        free(rreads);
        free(rport);
        rv = ref_vram();
        memset(&d, 0, sizeof d);
        for (j = 0; j < FD_VRAM_PIXELS; ++j) {
            if ((out.vram[j] ^ rv[j]) & 0x7fff)
                ++d.colour;
            if ((out.vram[j] ^ rv[j]) & 0x8000)
                ++d.mask;
        }
        printf("%s (%s): %ld colour, %ld mask pixels differ; %ld of %u GPUREAD words", argv[i],
               l.name, d.colour, d.mask, rbad, compared);
        if (rbad)
            printf(" (the first, read %ld)", rfirst);
        printf(" differ");
        bad |= d.colour || d.mask || rbad;
        {
            long pj, pr;
            unsigned resets;
            const char *why;
            if (play_stepped(&l, &pj, &pr, &resets, &why) == 0) {
                printf("; protected pixels changed under the mask check: %ld by jitterbug, %ld by "
                       "psxstack",
                       pj, pr);
                if (resets)
                    printf("; %u GP1(00h) after GP0 words, each given to psxstack as its "
                           "drawing-state reset",
                           resets);
                bad |= pj || pr;
            } else {
                printf("; the mask check's invariant not stepped (%s)", why);
                resets = resets_after_gp0(&l);
                if (resets)
                    printf("; %u GP1(00h) after GP0 words, each given to psxstack as its "
                           "drawing-state reset",
                           resets);
            }
        }
        printf("\n");
        jbc_outcome_free(&out);
        jbc_free(&l);
    }
    return bad;
}

/* tests/conformance's lists through the reference, against the console's
   dumps (15 colour bits, the exclusions left out) and logs. */
static int mode_conformance(const char *dir, const char *hw)
{
    static const char *const programs[] = {"triangle",  "quad",          "lines",
                                           "rectangles", "transparency", "uv-interpolation",
                                           "clipping",  "texture-flip",  "texture-overflow",
                                           "clut-cache", "vram-to-vram-overlap", "mask-bit",
                                           "gp0-e1"};
    size_t i;

    printf("psxstack's GPU against the console (tests/conformance, third_party/ps1-tests):\n");
    for (i = 0; i < sizeof programs / sizeof programs[0]; ++i) {
        char path[1024], err[256];
        jbc_list l;
        unsigned checks, failed, loops, skipped;
        uint16_t *dump;
        const char *why = NULL;
        unsigned char *ex = must(calloc(FD_VRAM_PIXELS, 1));
        unsigned expx = 0, exent = 0;
        jbc_diff d;

        snprintf(path, sizeof path, "%s/%s.jbc", dir, programs[i]);
        {
            /* Three programs' lists are not in the repository
               (third_party/ps1-tests/README.md): absent from DIR, they
               are left out here, and said to be. */
            FILE *f = fopen(path, "rb");
            if (!f) {
                printf("  %-24s not in %s (fetched-only; see nix build .#conformance-lists)\n",
                       programs[i], dir);
                free(ex);
                continue;
            }
            fclose(f);
        }
        if (jbc_load(path, &l, err, sizeof err) != 0) {
            fprintf(stderr, "%s: %s\n", path, err);
            return 2;
        }
        ref_play(&l, &checks, &failed, &loops, &skipped, NULL, NULL, NULL);
        if (getenv("FD_SAVE_REF")) { /* the reference's VRAM, raw, for a closer look */
            FILE *f;
            snprintf(path, sizeof path, "%s/%s.ref.bin", getenv("FD_SAVE_REF"), programs[i]);
            f = fopen(path, "wb");
            if (f) {
                fwrite(ref_vram(), 2, FD_VRAM_PIXELS, f);
                fclose(f);
            }
        }
        snprintf(path, sizeof path, "%s/%s.exclude", dir, programs[i]);
        {
            FILE *f = fopen(path, "r");
            if (f) {
                fclose(f);
                if (jbc_load_exclusions(path, ex, &expx, &exent, err, sizeof err) != 0) {
                    fprintf(stderr, "%s: %s\n", path, err);
                    return 2;
                }
            }
        }
        snprintf(path, sizeof path, "%s/%s/vram.png", hw, programs[i]);
        dump = jbc_load_dump(path, &why);
        if (dump && getenv("FD_SAVE_REF")) {
            FILE *f;
            snprintf(path, sizeof path, "%s/%s.dump.bin", getenv("FD_SAVE_REF"), programs[i]);
            f = fopen(path, "wb");
            if (f) {
                fwrite(dump, 2, FD_VRAM_PIXELS, f);
                fclose(f);
            }
        }
        if (dump) {
            jbc_compare_excluding(ref_vram(), dump, ex, &d);
            printf("  %-22s %7lu pixels differ", programs[i], d.count);
            if (d.count)
                printf(" in (%d,%d)-(%d,%d)", d.x0, d.y0, d.x1, d.y1);
            if (expx)
                printf(" (%u excluded)", expx);
            printf("\n");
            {
                /* where: rows with differences, as a short list */
                int y, shown = 0;
                for (y = 0; y < 512 && shown < 12; ++y) {
                    int x, cnt = 0, first = -1;
                    for (x = 0; x < 1024; ++x) {
                        size_t at = (size_t)y * 1024 + (size_t)x;
                        if (!ex[at] && ((ref_vram()[at] ^ dump[at]) & 0x7fff)) {
                            if (first < 0)
                                first = x;
                            ++cnt;
                        }
                    }
                    if (cnt) {
                        printf("      row %3d: %4d pixels from x %d\n", y, cnt, first);
                        ++shown;
                    }
                }
            }
            free(dump);
        } else {
            printf("  %-22s %u of %u GPUREAD checks fail; %u checks of other ports (GPUSTAT) "
                   "not compared: psxstack does not model them\n",
                   programs[i], failed, checks, skipped);
        }
        free(ex);
        jbc_free(&l);
    }
    return 0;
}

/* fuzz-diff open-lists DIR: the open-question lists (open_lists.c) written
   to DIR, each with its fixture line (tests/conformance/open/fixtures.txt)
   and what the two renderers make of it. */
static int mode_open_lists(const char *dir)
{
    int i;

    for (i = 0; i < fd_open_list_count; ++i) {
        fd_stream s;
        char path[1024], name[128], err[256];
        jbc_list l;

        fd_open_list_build(i, &s);
        snprintf(path, sizeof path, "%s/%s.jbc", dir, fd_open_lists[i].name);
        snprintf(name, sizeof name, "fuzz/%s", fd_open_lists[i].name);
        /* The source string is part of each list's words, so of its WORDS
           and CHECKSUM in tests/conformance/open/fixtures.txt: it stays as
           the lists were recorded, though the README it names is gone (the
           lists are described in docs/testing.md, "Open-question lists and
           console probe results"). */
        if (save_list(path, name,
                      "tools/fuzz-diff open-lists: a hand-built experiment "
                      "(tests/conformance/open/README.md)",
                      &s, 1) != 0)
            return 2;
        fd_stream_free(&s);
        if (jbc_load(path, &l, err, sizeof err) != 0) {
            fprintf(stderr, "%s: %s\n", path, err);
            return 2;
        }
        printf("%-24s open probe 0 0 %lu %016llx\n", fd_open_lists[i].name, (unsigned long)l.n,
               (unsigned long long)jbc_checksum(&l));
        jbc_free(&l);
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: fuzz-diff strict|bounded|run|repro SEED INDEX|play LIST...|"
                    "conformance DIR HW|words FILE|open-lists DIR|assumptions|invariants|hash "
                    "[--seed S] [--first I] [--streams N] [--commands C] [--focus F] "
                    "[--mode strict|bounded] [--whole] "
                    "[--no-filter] [--unmodelled] [--out DIR] [--repros K] "
                    "[--max-failures K] [--quiet]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    options o;
    int i, npos = 0;
    static char **pos; /* the arguments that are not options (static: kept to the exit) */

    if (argc < 2)
        usage();
    memset(&o, 0, sizeof o);
    o.seed = 1;
    o.streams = 100;
    o.gen.commands = 200;
    o.gen.focus = FD_FOCUS_ALL;
    o.gen.avoid_unmodelled = 1;
    o.filter = 1;
    o.repros = 1;
    o.max_failures = 1L << 40;
    pos = must(malloc((size_t)argc * sizeof *pos));
    for (i = 2; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "--seed") == 0 && v)
            o.seed = arg_number("--seed", v, 0, UINT64_MAX), ++i;
        else if (strcmp(a, "--first") == 0 && v)
            o.first = (long)arg_number("--first", v, 0, MAX_INDEX), ++i;
        else if (strcmp(a, "--streams") == 0 && v)
            o.streams = (long)arg_number("--streams", v, 1, MAX_INDEX), ++i;
        else if (strcmp(a, "--commands") == 0 && v)
            o.gen.commands = (int)arg_number("--commands", v, 1, MAX_COMMANDS), ++i;
        else if (strcmp(a, "--repros") == 0 && v)
            o.repros = (int)arg_number("--repros", v, 0, 1000000), ++i;
        else if (strcmp(a, "--max-failures") == 0 && v)
            o.max_failures = (long)arg_number("--max-failures", v, 1, MAX_INDEX), ++i;
        else if (strcmp(a, "--out") == 0 && v)
            o.out = v, ++i;
        else if (strcmp(a, "--focus") == 0 && v) {
            int f;
            for (f = 0; f < FD_FOCUS_COUNT && strcmp(v, fd_focus_names[f]) != 0; ++f)
                ;
            if (f == FD_FOCUS_COUNT) {
                fprintf(stderr, "fuzz-diff: no focus '%s'\n", v);
                exit(2);
            }
            o.gen.focus = (fd_focus)f;
            ++i;
        } else if (strcmp(a, "--no-filter") == 0)
            o.filter = 0;
        else if (strcmp(a, "--unmodelled") == 0)
            o.gen.avoid_unmodelled = 0;
        else if (strcmp(a, "--mode") == 0 && v) {
            if (strcmp(v, "strict") != 0 && strcmp(v, "bounded") != 0) {
                fprintf(stderr, "fuzz-diff: --mode is strict or bounded, not '%s'\n", v);
                exit(2);
            }
            o.check = strcmp(v, "strict") == 0 ? 1 : 2;
            ++i;
        } else if (strcmp(a, "--whole") == 0)
            o.whole = 1;
        else if (strcmp(a, "--save-divergences") == 0)
            o.save_divergences = 1;
        else if (strcmp(a, "--quiet") == 0)
            o.quiet = 1;
        else if (a[0] == '-' && a[1] == '-')
            usage();
        else
            pos[npos++] = argv[i];
    }
    if (o.first > MAX_INDEX - o.streams) {
        fprintf(stderr, "fuzz-diff: --first plus --streams past %ld\n", (long)MAX_INDEX);
        exit(2);
    }
    sides_open();
    if (strcmp(argv[1], "run") == 0)
        return mode_run(&o);
    if (strcmp(argv[1], "strict") == 0 || strcmp(argv[1], "bounded") == 0) {
        o.check = strcmp(argv[1], "strict") == 0 ? 1 : 2;
        if (o.gen.commands == 200)
            o.gen.commands = 150;
        return mode_check(&o);
    }
    if (strcmp(argv[1], "repro") == 0) {
        fd_stream s;
        result rr;
        long index;
        if (npos != 2)
            usage();
        o.seed = arg_number("repro's SEED", pos[0], 0, UINT64_MAX);
        index = (long)arg_number("repro's INDEX", pos[1], 0, MAX_INDEX);
        if (o.check && o.gen.commands == 200)
            o.gen.commands = 150;
        generate(&o, index, &s);
        run(&s, how(&o), &rr);
        if (rr.first < 0) {
            printf("seed %llu stream %ld: no difference\n", (unsigned long long)o.seed, index);
            if (o.whole)
                reproduce(&o, index, "", NULL, 0);
            fd_stream_free(&s);
            return 0;
        }
        printf("seed %llu stream %ld: ", (unsigned long long)o.seed, index);
        print_failure(&rr);
        printf("\n");
        reproduce(&o, index, rr.cls, NULL, !o.whole);
        fd_stream_free(&s);
        return 1;
    }
    if (strcmp(argv[1], "assumptions") == 0)
        return mode_assumptions();
    if (strcmp(argv[1], "hash") == 0)
        return mode_hash(&o);
    if (strcmp(argv[1], "words") == 0 && npos == 1)
        return mode_words(pos[0], o.out);
    if (strcmp(argv[1], "invariants") == 0)
        return mode_invariants();
    if (strcmp(argv[1], "play") == 0)
        return mode_play(npos, pos);
    if (strcmp(argv[1], "open-lists") == 0 && npos == 1)
        return mode_open_lists(pos[0]);
    if (strcmp(argv[1], "conformance") == 0 && npos == 2)
        return mode_conformance(pos[0], pos[1]);
    usage();
    return 2;
}

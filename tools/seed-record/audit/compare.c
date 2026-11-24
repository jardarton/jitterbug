/*
 * The polygon-interpolation audit (docs/testing.md, "Polygon-interpolation audit"):
 * the 1x VRAM of every capture of the seed sequence from the old and the new
 * instrumented
 * replayer (instrument.patch, old.patch), and for every pixel that differs
 * the last JBI_AUDIT_HIST events there in each run (src/soft/audit.h of
 * the patched tree: each write by a triangle, rectangle or line with the
 * pixel before it, the colour before blending, the texel, its coordinate,
 * the colour before texturing and dithering (8 bits a channel), the
 * semi-transparency mode and the primitive's state (semi-transparent,
 * set_mask, check_mask, raw texture, dithered, colour interpolated), and
 * each time one passed over the pixel without writing, and why).
 *
 *   compare OLD_DIR NEW_DIR LIST
 *
 * LIST names the captures, one "NN-name.opK" a line. Fail closed: a
 * changed pixel is inside the envelope only when the check below accounts
 * for its value in both runs; anything else is outside, and is listed
 * (OUTSIDE ...). Exit status 0 when nothing is outside, 1 when anything
 * is, 2 when the input is not what it should be (no capture, a file
 * missing or cut short, a map entry missing for a changed pixel, or one
 * for an unchanged pixel).
 *
 * The check, for the values vo and vn the pixel has in the two runs at the
 * capture (and again, recursively, for the values below a write, down to
 * where they agree):
 *
 * 1. Equal values need nothing more.
 * 2. In each run, the write that left the value: the last recorded write,
 *    every event since leaving the pixel as it is (each event's "after" is
 *    the next one's "before", and the last one's the value). If a value is
 *    not what the event after it found, a fill, copy, upload, poke or the
 *    1x pixels filled in from the scaled VRAM wrote it, which the audit
 *    cannot follow: outside. So is a value no primitive wrote, and one
 *    older than the history kept.
 * 3. Every event is recomputed from what was recorded (consistent(),
 *    pass_consistent()), and one that does not follow is outside. A write:
 *    the colour before blending is, for a raw texture, the texel's 15
 *    colour bits; for a modulated one, texel x colour / 128 a channel
 *    (clamped to 31), or dithered (texel x colour / 16 + the dither value
 *    at the pixel, clamped to 0..255) / 8; untextured, the colour / 8, or
 *    dithered (colour + the dither value, clamped) / 8. It is blended
 *    exactly when the primitive is semi-transparent and (textured) the
 *    texel has bit 15; that colour, blended with the pixel before by the
 *    recorded mode, the texel's bit 15 and set_mask give the value
 *    written; under check_mask the pixel before has bit 15 clear; the
 *    state fits the kind (raw only textured; no dithering on a rectangle
 *    or a raw texture; a Gouraud triangle's colour interpolated, a flat
 *    triangle's, a rectangle's and a raw texture's not). A pass: on a
 *    transparent texel (0x0000) of a textured primitive, the pixel before
 *    without bit 15 if check_mask is on; or through the mask bit, with
 *    check_mask on and bit 15 set.
 * 4. The same primitive wrote it in both runs (same kind and state):
 *      textured triangle   the texel coordinate moved by at most one in u
 *                          and in v, and not by (0, 0) with another texel
 *                          (that would be the texture changing, which this
 *                          fix does not do); with the same texel, the
 *                          colour (the texel modulated) moved by at most
 *                          one step a channel (with a raw texture, by 3.,
 *                          not at all); flat-shaded and modulated, the
 *                          colour before texturing did not change. A texel
 *                          with the other bit 15 is blended in one run
 *                          only.
 *      Gouraud triangle    the colour moved by at most one step a channel.
 *      flat triangle, rectangle, line
 *                          the colour, before and after dithering (and a
 *                          rectangle's texel and coordinate) did not
 *                          change.
 *    If it was blended in either run and the pixels below differ, they are
 *    checked in turn (from 1.), below this write in each run. So a blended
 *    pixel is inside only when its change follows from a colour that moved
 *    within these rules and from a background whose change is itself
 *    inside.
 * 5. Different primitives: P, the later one, wrote in one run; P must be
 *    a textured triangle (recorded as one in both runs: a sprite's texel
 *    coordinate does not move under the fix) with the same state in both;
 *    in the other run P must have
 *    passed over the pixel, after that run's write and with nothing
 *    written since, because its texel was transparent; the
 *    sampled coordinates in the two runs one apart at most in u and in v
 *    (not equal: the same coordinate with another texel is the texture
 *    changing). A pass through the mask bit, no pass at all (a change of
 *    which pixels P covers), or one older than the history kept is outside.
 *    Then the value below P in the first run and the other run's value are
 *    checked in turn (from 1.).
 *
 * Every capture is checked, scenes 11 and 12 (which draw partly with exact
 * 1x off) too: a 1x pixel filled in from the scaled VRAM is a value no
 * recorded event left, so it is outside if it differs.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W = 1024, H = 512, N = W * H };

/* Wrong input: exit status 2 (selftest.c makes it return to the case). */
#ifndef COMPARE_EXIT
#define COMPARE_EXIT(status) exit(status)
#endif

/* As src/soft/audit.h (instrument.patch) lays them out. */
enum { HIST = 16 };
typedef struct {
    uint32_t id;
    uint16_t bg, out, src, texel;
    int16_t u, v;
    int16_t r, g, b; /* the colour before texturing and dithering, 8 bits a channel */
    uint8_t kind, flags;
} ev;
typedef struct {
    uint32_t n;
    ev e[HIST];
} px;

enum { FLAT = 1, GOURAUD, TEXTURED, TILE, SPRITE, LINE };
enum { BLENDED = 0x10, SKIP_T = 0x20, SKIP_M = 0x40, SKIP = SKIP_T | SKIP_M };
/* flags: bits 0-1 the semi-transparency mode, then the primitive's state. */
enum { SET_MASK = 4, RAW = 8, DITHER = 0x10, CHECK_MASK = 0x20, SHADED = 0x40, SEMI = 0x80 };

/* As src/soft/raster.c (psx-spx, "Dithering"). */
static const signed char dither_matrix[4][4] = {
    {-4, 0, -3, 1},
    {2, -2, 3, -1},
    {-3, 1, -4, 0},
    {3, -1, 2, -2},
};

/* The pixel being checked (the dither matrix depends on it). */
static int cur_x, cur_y;

static const char *const kinds[] = {"none",  "flat",   "Gouraud", "textured",
                                    "tile", "sprite", "line",    "?"};

static int is_textured(int kind)
{
    return kind == TEXTURED || kind == SPRITE;
}

/* The reasons a pixel is outside; up to LIST of each are listed. */
enum {
    R_EXTERNAL, R_NONE, R_TRUNC, R_DEEP, R_INCONSISTENT, R_KIND, R_TEX_FAR, R_TEX_SAME_COORD,
    R_MOD_FAR, R_G_FAR, R_FIXED, R_SW_MASK, R_SW_NOSKIP, R_SW_FAR, R_SW_KIND, R_SW_STATE,
    R_REASONS,
    LIST = 25
};
static const char *const reason_names[R_REASONS] = {
    "a value no recorded event left (a fill, copy, upload, poke or scaled fill-in)",
    "a value from before any primitive",
    "more events than the history keeps",
    "more levels below than the check follows",
    "an event the recorded inputs do not give (or not the primitive's kind)",
    "the same primitive, another kind of write, mode, set_mask or blending",
    "texel coordinate more than one away",
    "the same texel coordinate, another texel",
    "the same texel, the modulated colour beyond one step",
    "Gouraud colour beyond one step",
    "a colour (or a rectangle's texel) the fix cannot change, changed",
    "a switch through the mask bit",
    "a switch where the later primitive did not pass over the pixel in the other run",
    "a switch where the transparent texel's coordinate is not one away",
    "a switch where the later primitive is not a textured triangle in both runs",
    "a switch where the later primitive's state differs between the runs",
};

/* What the pixels inside were, by what last wrote them in the two runs. */
enum {
    C_TEX, C_TEX_SAME, C_STP, C_GOURAUD, C_BLEND_BG, C_BLEND_SRC, C_BLEND_BOTH, C_SWITCH,
    C_CLASSES
};

static struct {
    unsigned long changed, inside, outside;
    unsigned long cls[C_CLASSES], cls_kind[C_CLASSES][8];
    unsigned long tex_off[3][3], stp_off[3][3], sw_off[3][3];
    unsigned long sw_old_drew, sw_new_drew, below; /* checks below the top write */
    unsigned long reasons[R_REASONS];
} t;

/* A map as written by instrument.patch: the changed pixels' entries. */
typedef struct {
    uint32_t count;
    uint32_t *index;
    px *p;
} amap;
static amap mo, mn;

/* The capture being compared and the files open. File scope, so that what
   is allocated stays reachable when selftest.c returns from an input
   error (COMPARE_EXIT). */
static uint16_t *vram_old, *vram_new;
static FILE *in, *list;

static void release(void)
{
    if (in != NULL)
        fclose(in);
    in = NULL;
    free(vram_old);
    free(vram_new);
    free(mo.index);
    free(mo.p);
    free(mn.index);
    free(mn.p);
    vram_old = vram_new = NULL;
    memset(&mo, 0, sizeof mo);
    memset(&mn, 0, sizeof mn);
}

static FILE *open_in(const char *dir, const char *name, const char *ext, char *path, size_t size)
{
    snprintf(path, size, "%s/%s.%s", dir, name, ext);
    in = fopen(path, "rb");
    if (in == NULL) {
        fprintf(stderr, "compare: cannot read %s\n", path);
        COMPARE_EXIT(2);
    }
    return in;
}

static void close_in(void)
{
    fclose(in);
    in = NULL;
}

static void read_vram(const char *dir, const char *name, uint16_t **pp)
{
    char path[4096];
    FILE *f = open_in(dir, name, "1x.raw", path, sizeof path);
    uint16_t *p = *pp = malloc((size_t)N * 2);

    if (p == NULL || fread(p, 2, N, f) != N || fgetc(f) != EOF) {
        fprintf(stderr, "compare: %s is not a 1x VRAM dump\n", path);
        COMPARE_EXIT(2);
    }
    close_in();
}

static void read_map(const char *dir, const char *name, amap *m)
{
    char path[4096], magic[8];
    FILE *f = open_in(dir, name, "audit", path, sizeof path);
    uint32_t size, k, prev = 0;

    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "JBAUDIT3", 8) != 0 ||
        fread(&m->count, 4, 1, f) != 1 || fread(&size, 4, 1, f) != 1 || size != sizeof(px) ||
        m->count > N) {
        fprintf(stderr, "compare: %s is not an audit map of this layout\n", path);
        COMPARE_EXIT(2);
    }
    m->index = malloc((size_t)m->count * 4 + 4);
    m->p = malloc((size_t)m->count * sizeof(px) + 1);
    if (m->index == NULL || m->p == NULL) {
        fprintf(stderr, "compare: out of memory\n");
        COMPARE_EXIT(2);
    }
    for (k = 0; k < m->count; ++k) {
        if (fread(&m->index[k], 4, 1, f) != 1 || fread(&m->p[k], sizeof(px), 1, f) != 1 ||
            m->index[k] >= N || (k > 0 && m->index[k] <= prev)) {
            fprintf(stderr, "compare: %s is cut short or out of order\n", path);
            COMPARE_EXIT(2);
        }
        prev = m->index[k];
    }
    if (fgetc(f) != EOF) {
        fprintf(stderr, "compare: %s has more than its entries\n", path);
        COMPARE_EXIT(2);
    }
    close_in();
}

static uint32_t blend(uint32_t back, uint32_t src, int mode)
{
    int c, out = 0;

    for (c = 0; c < 3; ++c) {
        int b = (int)(back >> (5 * c)) & 31, f = (int)(src >> (5 * c)) & 31, r;

        switch (mode) {
        case 0: r = (b + f) >> 1; break;
        case 1: r = b + f > 31 ? 31 : b + f; break;
        case 2: r = b - f < 0 ? 0 : b - f; break;
        default: r = b + (f >> 2) > 31 ? 31 : b + (f >> 2); break;
        }
        out |= r << (5 * c);
    }
    return (uint32_t)out;
}

static int clamp255(int c)
{
    return c < 0 ? 0 : c > 255 ? 255 : c;
}

static int min31(int c)
{
    return c > 31 ? 31 : c;
}

/* The colour before blending a write must have had, from its recorded
   texel, colour (8 bits a channel), state and pixel, as src/soft/raster.c
   computes it: a raw texel as it is; a modulated one (texel x colour) / 128,
   or with dithering (texel x colour) / 16 plus the dither value, clamped,
   / 8; an untextured colour / 8, dithered the same way. */
static uint32_t source(const ev *e)
{
    int kind = e->kind & 7, c, out = 0;
    int d = (e->flags & DITHER) ? dither_matrix[cur_y & 3][cur_x & 3] : 0;
    const int col[3] = {e->r, e->g, e->b};

    if (is_textured(kind) && (e->flags & RAW))
        return e->texel & 0x7fffu;
    for (c = 0; c < 3; ++c) {
        int tx = (e->texel >> (5 * c)) & 31, r;

        if (is_textured(kind))
            r = (e->flags & DITHER) ? clamp255(((tx * col[c]) >> 4) + d) >> 3
                                    : min31((tx * col[c]) >> 7);
        else
            r = (e->flags & DITHER) ? clamp255(col[c] + d) >> 3 : col[c] >> 3;
        out |= r << (5 * c);
    }
    return (uint32_t)out & 0xffffu;
}

/* The state bits a primitive of the kind can have (src/soft/raster.c): raw
   only textured; dithering not on a rectangle nor a raw texture; the colour
   interpolated always for a Gouraud triangle, never for a flat triangle, a
   rectangle or a raw texture. */
static int state_fits(int kind, int flags)
{
    if ((flags & RAW) && !is_textured(kind))
        return 0;
    if ((flags & DITHER) && (kind == TILE || kind == SPRITE || (flags & RAW)))
        return 0;
    if (kind == GOURAUD)
        return (flags & SHADED) != 0;
    if (kind == FLAT || kind == TILE || kind == SPRITE || (flags & RAW))
        return (flags & SHADED) == 0;
    return 1;
}

/* A write gives its recorded value from its recorded inputs: the colour
   before blending from the texel, colour, state and pixel (source()); it is
   blended exactly when the primitive is semi-transparent and, textured,
   the texel has bit 15; the value written from that colour, the pixel
   before and the mode, the texel's bit 15 and set_mask; and no write over a
   pixel with bit 15 under check_mask. */
static int consistent(const ev *e)
{
    int kind = e->kind & 7, textured = is_textured(kind);
    uint32_t stp = textured ? e->texel & 0x8000u : 0, want;

    /* A recorded colour is 8 bits a channel: anything else is not a
       renderer's, and must not reach source(), where a channel above 255
       would spill into the next one. */
    if (e->r < 0 || e->r > 255 || e->g < 0 || e->g > 255 || e->b < 0 || e->b > 255)
        return 0;
    if ((e->src & 0x8000) || kind < FLAT || kind > LINE || (e->kind & SKIP) ||
        !state_fits(kind, e->flags) || (textured && e->texel == 0) ||
        (!textured && e->texel != 0) || e->src != source(e) ||
        ((e->flags & CHECK_MASK) && (e->bg & 0x8000)) ||
        !(e->kind & BLENDED) != !((e->flags & SEMI) && (!textured || stp)))
        return 0;
    want = (e->kind & BLENDED) ? blend(e->bg, e->src, e->flags & 3) : e->src;
    want |= stp | ((e->flags & SET_MASK) ? 0x8000u : 0);
    return e->out == want;
}

/* A pass without a write: a transparent texel (0x0000, of a textured
   primitive, over a pixel check_mask allowed), or the mask bit (check_mask
   on, bit 15 set). Nothing else recorded. */
static int pass_consistent(const ev *e)
{
    int kind = e->kind & 7;

    if (kind < FLAT || kind > LINE || (e->kind & BLENDED) || !state_fits(kind, e->flags) ||
        e->bg != e->out || e->src != 0 || e->texel != 0 || e->r != 0 || e->g != 0 || e->b != 0)
        return 0;
    if ((e->kind & SKIP) == SKIP_M)
        return (e->flags & CHECK_MASK) && (e->bg & 0x8000);
    if ((e->kind & SKIP) == SKIP_T)
        return is_textured(kind) && !((e->flags & CHECK_MASK) && (e->bg & 0x8000));
    return 0;
}

/* The colour of a primitive that is not interpolated: the same in both
   runs (a flat triangle's, a rectangle's, a line's (old.patch changes no
   line), a flat-shaded modulated textured triangle's). */
static int same_colour(const ev *a, const ev *b)
{
    return a->r == b->r && a->g == b->g && a->b == b->b;
}

/* Every channel within one step. */
static int near(uint32_t a, uint32_t b)
{
    int c;

    for (c = 0; c < 3; ++c) {
        int d = (int)((a >> (5 * c)) & 31) - (int)((b >> (5 * c)) & 31);
        if (d > 1 || d < -1)
            return 0;
    }
    return (a & 0x8000) == (b & 0x8000);
}

static int in_one(int d)
{
    return d >= -1 && d <= 1;
}

/* The events of a pixel: event k (from 0) is p->e[k % HIST], kept for
   first(p) <= k < p->n. */
static uint32_t first(const px *p)
{
    return p->n > HIST ? p->n - HIST : 0;
}

static const ev *at(const px *p, uint32_t k)
{
    return &p->e[k % HIST];
}

enum { W_WRITE, W_EXTERNAL, W_NONE, W_TRUNC, W_BAD };

/* The write that left value at the pixel, among events before upto: W_WRITE
   and its index in *w, or why there is none. *lo: the oldest event of the
   unbroken run of passes above it. */
static int walk(const px *p, uint32_t upto, uint32_t value, uint32_t *w, uint32_t *lo)
{
    uint32_t k = upto, f = first(p);

    while (k > f) {
        const ev *e = at(p, k - 1);

        if (e->out != value || ((e->kind & SKIP) && e->bg != e->out)) {
            *lo = k;
            return W_EXTERNAL;
        }
        if ((e->kind & SKIP) && !pass_consistent(e)) {
            *lo = k;
            return W_BAD;
        }
        --k;
        if (!(e->kind & SKIP)) {
            *w = k;
            *lo = k + 1;
            return W_WRITE;
        }
    }
    *lo = k;
    return f > 0 ? W_TRUNC : W_NONE;
}

static int status_reason(int st)
{
    return st == W_EXTERNAL ? R_EXTERNAL
           : st == W_TRUNC  ? R_TRUNC
           : st == W_BAD    ? R_INCONSISTENT
                            : R_NONE;
}

/* Where a reason is found, and the levels gone through to it. */
static int depth_seen;

/* 0 (inside, *cls the class at this level) or 1 + the reason. */
static int explain(const px *po, uint32_t uo, uint32_t vo, const px *pn, uint32_t un,
                   uint32_t vn, int depth, int *cls, int *kind);

static int same_primitive(const px *po, uint32_t wo, const px *pn, uint32_t wn, int depth,
                          int *cls)
{
    const ev *a = at(po, wo), *b = at(pn, wn);
    int kind = a->kind & 7, du = b->u - a->u, dv = b->v - a->v;
    int blended = (a->kind | b->kind) & BLENDED;
    int sub, subk;

    if (kind != (b->kind & 7) || a->flags != b->flags)
        return 1 + R_KIND;
    if ((a->kind ^ b->kind) & BLENDED && !(kind == TEXTURED && ((a->texel ^ b->texel) & 0x8000)))
        return 1 + R_KIND;
    switch (kind) {
    case TEXTURED:
        if (!in_one(du) || !in_one(dv))
            return 1 + R_TEX_FAR;
        if (du == 0 && dv == 0 && a->texel != b->texel)
            return 1 + R_TEX_SAME_COORD;
        /* A raw texel is its own colour (consistent()): with the same texel
           the colour is the same. A flat-shaded modulated triangle's colour
           is not interpolated. */
        if (!(a->flags & (RAW | SHADED)) && !same_colour(a, b))
            return 1 + R_FIXED;
        if (a->texel == b->texel && !near(a->src, b->src))
            return 1 + R_MOD_FAR;
        break;
    case GOURAUD:
        if (!near(a->src, b->src))
            return 1 + R_G_FAR;
        break;
    case SPRITE:
        if (a->src != b->src || a->texel != b->texel || a->u != b->u || a->v != b->v ||
            (!(a->flags & RAW) && !same_colour(a, b)))
            return 1 + R_FIXED;
        break;
    default: /* flat, tile, line: no texel (the recorded coordinate means nothing) */
        if (a->src != b->src || !same_colour(a, b))
            return 1 + R_FIXED;
        break;
    }
    if ((a->kind ^ b->kind) & BLENDED) {
        *cls = C_STP;
    } else if (blended) {
        *cls = a->src != b->src ? (a->bg != b->bg ? C_BLEND_BOTH : C_BLEND_SRC) : C_BLEND_BG;
    } else if (kind == TEXTURED) {
        *cls = a->texel == b->texel ? C_TEX_SAME : C_TEX;
    } else {
        *cls = C_GOURAUD;
    }
    /* What is below matters only to a blended write. */
    if (blended && a->bg != b->bg) {
        int r = explain(po, wo, a->bg, pn, wn, b->bg, depth + 1, &sub, &subk);

        if (r != 0)
            return r;
        ++t.below;
    }
    if (depth == 0 && *cls == C_STP)
        ++t.stp_off[du + 1][dv + 1];
    else if (depth == 0 && kind == TEXTURED && !blended)
        ++t.tex_off[du + 1][dv + 1];
    return 0;
}

static int switched(const px *pa, uint32_t wa, const px *pb, uint32_t ub, uint32_t vb,
                    int stb, uint32_t lob, int depth, int sign)
{
    const ev *p = at(pa, wa), *s = NULL;
    uint32_t k;
    int du, dv, sub, subk, r;

    for (k = ub; k > lob; --k)
        if (at(pb, k - 1)->id == p->id) {
            s = at(pb, k - 1);
            break;
        }
    if (s == NULL)
        return 1 + (stb == W_TRUNC || stb == W_EXTERNAL ? status_reason(stb) : R_SW_NOSKIP);
    if (s->kind & SKIP_M)
        return 1 + R_SW_MASK;
    if (!(s->kind & SKIP_T) || s->texel != 0)
        return 1 + R_SW_NOSKIP;
    /* Only a textured triangle's texel coordinate moves under the fix: a
       sprite's (the instrumentation records its transparent texels the
       same way) does not, nor does anything else. */
    if ((p->kind & 7) != TEXTURED || (s->kind & 7) != TEXTURED)
        return 1 + R_SW_KIND;
    /* The same primitive: the same mode, set_mask, raw, dithering,
       check_mask, shading and semi-transparency in both runs. */
    if (s->flags != p->flags)
        return 1 + R_SW_STATE;
    du = s->u - p->u;
    dv = s->v - p->v;
    if (!in_one(du) || !in_one(dv))
        return 1 + R_SW_FAR;
    if (du == 0 && dv == 0)
        return 1 + R_TEX_SAME_COORD;
    /* Below P in its run against the other run's value there. */
    r = explain(pa, wa, p->bg, pb, k - 1, vb, depth + 1, &sub, &subk);
    if (r == 0 && p->bg != vb)
        ++t.below;
    if (r == 0 && depth == 0)
        ++t.sw_off[sign * du + 1][sign * dv + 1];
    return r;
}

static int explain(const px *po, uint32_t uo, uint32_t vo, const px *pn, uint32_t un,
                   uint32_t vn, int depth, int *cls, int *kind)
{
    uint32_t wo = 0, wn = 0, loo, lon, ido, idn;
    int so, sn;

    *cls = -1;
    *kind = 0;
    if (vo == vn)
        return 0;
    depth_seen = depth;
    if (depth >= HIST)
        return 1 + R_DEEP;
    so = walk(po, uo, vo, &wo, &loo);
    sn = walk(pn, un, vn, &wn, &lon);
    if (so == W_BAD || sn == W_BAD)
        return 1 + R_INCONSISTENT;
    if (so == W_WRITE && !consistent(at(po, wo)))
        return 1 + R_INCONSISTENT;
    if (sn == W_WRITE && !consistent(at(pn, wn)))
        return 1 + R_INCONSISTENT;
    ido = so == W_WRITE ? at(po, wo)->id : 0;
    idn = sn == W_WRITE ? at(pn, wn)->id : 0;
    if (ido == 0 && idn == 0)
        return 1 + status_reason(so != W_NONE ? so : sn);
    if (ido == idn) {
        *kind = at(po, wo)->kind & 7;
        return same_primitive(po, wo, pn, wn, depth, cls);
    }
    *cls = C_SWITCH;
    if (ido > idn) {
        int r = switched(po, wo, pn, un, vn, sn, lon, depth, 1);

        *kind = at(po, wo)->kind & 7;
        t.sw_old_drew += r == 0 && depth == 0;
        return r;
    } else {
        int r = switched(pn, wn, po, uo, vo, so, loo, depth, -1);

        *kind = at(pn, wn)->kind & 7;
        t.sw_new_drew += r == 0 && depth == 0;
        return r;
    }
}

static void show(const char *side, uint16_t val, const px *p)
{
    uint32_t k;

    printf("    %s %04x; %u events, the last %u:\n", side, val, p->n,
           p->n < HIST ? p->n : (unsigned)HIST);
    for (k = first(p); k < p->n; ++k) {
        const ev *e = at(p, k);

        printf("      %u %s%s%s bg %04x out %04x src %04x texel %04x u %d v %d colour %d %d %d "
               "mode %d%s%s%s%s%s%s\n",
               e->id, kinds[e->kind & 7], e->kind & BLENDED ? " blended" : "",
               e->kind & SKIP_T ? " passed (transparent)" : e->kind & SKIP_M ? " passed (mask)" : "",
               e->bg, e->out, e->src, e->texel, e->u, e->v, e->r, e->g, e->b, e->flags & 3,
               e->flags & SEMI ? " semi" : "", e->flags & SET_MASK ? " set_mask" : "",
               e->flags & CHECK_MASK ? " check_mask" : "", e->flags & RAW ? " raw" : "",
               e->flags & DITHER ? " dither" : "", e->flags & SHADED ? " shaded" : "");
    }
}

static void pixel(const char *cap, uint32_t i, const px *po, uint16_t vo, const px *pn,
                  uint16_t vn)
{
    int cls, kind, r;

    depth_seen = 0;
    cur_x = (int)(i % W);
    cur_y = (int)(i / W);
    r = explain(po, po->n, vo, pn, pn->n, vn, 0, &cls, &kind);
    if (r == 0) {
        ++t.inside;
        ++t.cls[cls];
        ++t.cls_kind[cls][kind];
        return;
    }
    ++t.outside;
    if (++t.reasons[r - 1] > LIST)
        return;
    printf("OUTSIDE %s (%u, %u): %s%s\n", cap, i % W, i / W, reason_names[r - 1],
           depth_seen > 0 ? " (below the last write)" : "");
    show("old", vo, po);
    show("new", vn, pn);
}

static void offsets(const char *what, unsigned long off[3][3])
{
    int u, v;

    printf("    %s, (du, dv) new minus old:", what);
    for (v = -1; v <= 1; ++v)
        for (u = -1; u <= 1; ++u)
            if (off[u + 1][v + 1])
                printf(" (%+d, %+d) %lu", u, v, off[u + 1][v + 1]);
    printf("\n");
}

int main(int argc, char **argv)
{
    char cap[256];
    int c;
    unsigned long captures = 0;
    static const char *const cls_names[C_CLASSES] = {
        "textured triangle, unblended, the same in both, another texel",
        "textured triangle, unblended, the same in both, the same texel (modulated colour moved)",
        "textured triangle, the same in both, a texel with the other bit 15 (blended in one run)",
        "Gouraud triangle, unblended, the same in both",
        "the same blended primitive, the same colour, a changed background (checked below)",
        "the same blended primitive, a changed colour, the same background",
        "the same blended primitive, a changed colour and background (checked below)",
        "another primitive: the later one's texel transparent in the other run",
    };

    if (argc != 4) {
        fprintf(stderr, "usage: compare OLD_DIR NEW_DIR LIST\n");
        return 2;
    }
    memset(&t, 0, sizeof t);
    release();
    if (list != NULL)
        fclose(list);
    list = fopen(argv[3], "r");
    if (list == NULL) {
        fprintf(stderr, "compare: cannot read %s\n", argv[3]);
        return 2;
    }
    while (fscanf(list, "%255s", cap) == 1) {
        uint32_t i, k = 0;

        ++captures;
        release();
        read_vram(argv[1], cap, &vram_old);
        read_vram(argv[2], cap, &vram_new);
        read_map(argv[1], cap, &mo);
        read_map(argv[2], cap, &mn);
        for (i = 0; i < N; ++i) {
            if (vram_old[i] == vram_new[i])
                continue;
            /* Both maps have exactly the changed pixels, in order. */
            if (k >= mo.count || k >= mn.count || mo.index[k] != i || mn.index[k] != i) {
                fprintf(stderr, "compare: %s: the maps do not have pixel (%u, %u)\n", cap,
                        i % W, i / W);
                COMPARE_EXIT(2);
            }
            ++t.changed;
            pixel(cap, i, &mo.p[k], vram_old[i], &mn.p[k], vram_new[i]);
            ++k;
        }
        if (k != mo.count || k != mn.count) {
            fprintf(stderr, "compare: %s: the maps have entries for unchanged pixels\n", cap);
            COMPARE_EXIT(2);
        }
    }
    release();
    fclose(list);
    list = NULL;
    if (captures == 0) {
        fprintf(stderr, "compare: %s names no capture\n", argv[3]);
        COMPARE_EXIT(2);
    }

    printf("captures: %lu\nchanged pixels, summed over the captures: %lu\n", captures, t.changed);
    printf("  inside the envelope: %lu, by what last wrote them:\n", t.inside);
    for (c = 0; c < C_CLASSES; ++c) {
        printf("    %s: %lu", cls_names[c], t.cls[c]);
        if (c >= C_BLEND_BG)
            printf(" (flat %lu, Gouraud %lu, textured %lu, tile %lu, sprite %lu, line %lu)",
                   t.cls_kind[c][FLAT], t.cls_kind[c][GOURAUD], t.cls_kind[c][TEXTURED],
                   t.cls_kind[c][TILE], t.cls_kind[c][SPRITE], t.cls_kind[c][LINE]);
        printf("\n");
    }
    offsets("textured, unblended, texel coordinate", t.tex_off);
    offsets("texel with the other bit 15, texel coordinate", t.stp_off);
    offsets("switches, texel coordinate", t.sw_off);
    printf("    switches: the later primitive drew in the old run %lu, in the new %lu\n",
           t.sw_old_drew, t.sw_new_drew);
    printf("    differing values below a blended write or a switch, checked in turn: %lu\n",
           t.below);
    printf("outside the envelope: %lu\n", t.outside);
    for (c = 0; c < R_REASONS; ++c)
        if (t.reasons[c])
            printf("  %s: %lu\n", reason_names[c], t.reasons[c]);
    return t.outside != 0;
}

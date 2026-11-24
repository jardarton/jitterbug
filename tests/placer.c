/*
 * placer: the VRAM placer (src/core/placer.h) against an independent
 * model.
 *
 * The model keeps the list of rectangles taken (display buffers and
 * reservations, which stay; placed rectangles, which come and go) and
 * knows nothing of the placer's bitmap. For every request it works out by
 * brute force, over a summed-area table of its own rectangles, the first
 * position in the documented order (top-down by rows, each left to right;
 * or bottom-up by columns from the left) that meets the request's rules, and the placer must answer
 * exactly that position, or fail exactly when there is none. After every
 * step every placed rectangle is checked against the rules (inside VRAM,
 * one page row, one page span, alignments) and against every other
 * rectangle for overlap.
 *
 * Runs: seeded random sequences of texture-like requests (the sizes
 * jb_texture_create makes at 4, 8 and 15 bits, with and without repeat,
 * and the palettes beside them) and frees, over several display layouts and
 * reservations, until VRAM is full and after; and hand-made cases (an
 * empty VRAM, the page rules at the edges, a full one, bad requests).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/layout.h"
#include "core/placer.h"

static int checks, failures;

static void check(const char *what, long got, long want)
{
    ++checks;
    if (got == want)
        return;
    if (++failures <= 30)
        printf("FAIL %s: got %ld, want %ld\n", what, got, want);
}

/* ------------------------------------------------------------------------ */
/* The model */

enum { MAX_RECTS = 4096, W = 1024, H = 512 };

typedef struct item {
    jbi_rect r;
    jbi_place_req q; /* for placed ones: what it was placed for */
    int placed;      /* 0 for buffers and reservations */
} item;

static item items[MAX_RECTS];
static int n_items;
static int sat[H + 1][W + 1]; /* taken pixels above and left of (x, y) */
static unsigned char px[H][W];
static int sat_dirty = 1;

static void rebuild(void)
{
    int i, x, y;

    memset(px, 0, sizeof px);
    for (i = 0; i < n_items; ++i) {
        const jbi_rect *r = &items[i].r;
        for (y = r->y; y < r->y + r->h; ++y)
            for (x = r->x; x < r->x + r->w; ++x)
                if (x >= 0 && x < W && y >= 0 && y < H)
                    px[y][x] = 1;
    }
    for (y = 0; y <= H; ++y)
        sat[y][0] = 0;
    for (x = 0; x <= W; ++x)
        sat[0][x] = 0;
    for (y = 1; y <= H; ++y)
        for (x = 1; x <= W; ++x)
            sat[y][x] = px[y - 1][x - 1] + sat[y - 1][x] + sat[y][x - 1] - sat[y - 1][x - 1];
    sat_dirty = 0;
}

static int area_taken(int x, int y, int w, int h)
{
    return sat[y + h][x + w] - sat[y][x + w] - sat[y + h][x] + sat[y][x];
}

/* The rules of a position, apart from what is taken. */
static int rules_ok(const jbi_place_req *q, int x, int y)
{
    if (x < 0 || y < 0 || x + q->w > W || y + q->h > H)
        return 0;
    if (x % q->align != 0 || y % q->align_y != 0)
        return 0;
    if (y / 256 != (y + q->h - 1) / 256)
        return 0;
    if (q->span != 0 && x + q->w > (x & ~63) + q->span)
        return 0;
    return 1;
}

/* The documented order, by brute force: from the top, rows ascending and
   in each x ascending; from the bottom, columns ascending and in each y
   descending; positions that break a rule skipped. The first free
   position after the position after (NULL: from the start). */
static int model_next(const jbi_place_req *q, const jbi_rect *after, jbi_rect *out)
{
    int x, y, past = after == NULL;

    if (sat_dirty)
        rebuild();
    if (!q->from_bottom) {
        for (y = 0; y < H; ++y)
            for (x = 0; x + q->w <= W; ++x) {
                if (!past) {
                    past = y == after->y && x == after->x;
                    continue;
                }
                if (rules_ok(q, x, y) && area_taken(x, y, q->w, q->h) == 0)
                    goto found;
            }
        return 0;
    }
    for (x = 0; x < W; ++x)
        for (y = H - 1; y >= 0; --y) {
            if (!past) {
                past = y == after->y && x == after->x;
                continue;
            }
            if (rules_ok(q, x, y) && area_taken(x, y, q->w, q->h) == 0)
                goto found;
        }
    return 0;
found:
    out->x = x, out->y = y, out->w = q->w, out->h = q->h;
    return 1;
}

static int model_find(const jbi_place_req *q, jbi_rect *out)
{
    return model_next(q, NULL, out);
}

/* The first few places in order, from jbi_placer_find_next and from the
   model: the same. */
static void check_next(const jbi_placer *p, const jbi_place_req *q, const char *what)
{
    jbi_rect a, b, prev_a, prev_b;
    int k, fa = jbi_placer_find(p, q, &a), fb = model_find(q, &b);
    char m[128];

    for (k = 0; k < 6; ++k) {
        snprintf(m, sizeof m, "%s: next %d found", what, k);
        check(m, fa, fb);
        if (!fa || !fb)
            return;
        snprintf(m, sizeof m, "%s: next %d at", what, k);
        check(m, a.x == b.x && a.y == b.y, 1);
        prev_a = a, prev_b = b;
        fa = jbi_placer_find_next(p, q, &prev_a, &a);
        fb = model_next(q, &prev_b, &b);
    }
}

static int overlap(jbi_rect a, jbi_rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static void check_all(const char *what)
{
    int i, j, bad_rules = 0, bad_overlap = 0;
    char m[96];

    for (i = 0; i < n_items; ++i) {
        if (!items[i].placed)
            continue;
        if (!rules_ok(&items[i].q, items[i].r.x, items[i].r.y))
            ++bad_rules;
        for (j = 0; j < n_items; ++j)
            if (j != i && (!items[j].placed || j > i) && overlap(items[i].r, items[j].r))
                ++bad_overlap;
    }
    snprintf(m, sizeof m, "%s: rectangles breaking a rule", what);
    check(m, bad_rules, 0);
    snprintf(m, sizeof m, "%s: overlapping rectangles", what);
    check(m, bad_overlap, 0);
}

static void add_fixed(jbi_placer *p, jbi_rect r)
{
    items[n_items].r = r;
    items[n_items].placed = 0;
    ++n_items;
    jbi_placer_mark(p, r, 1);
    sat_dirty = 1;
}

/* One request through the placer and the model: they must agree. Returns
   the item, or -1 for none. */
static int place(jbi_placer *p, const jbi_place_req *q, const char *what)
{
    jbi_rect got, want;
    int found = jbi_placer_find(p, q, &got), expect = model_find(q, &want);
    char m[160];

    snprintf(m, sizeof m, "%s: found (%dx%d align %d,%d span %d %s)", what, q->w, q->h, q->align,
             q->align_y, q->span, q->from_bottom ? "bottom" : "top");
    check(m, found, expect);
    if (!found || !expect)
        return -1;
    snprintf(m, sizeof m, "%s: x", what);
    check(m, got.x, want.x);
    snprintf(m, sizeof m, "%s: y", what);
    check(m, got.y, want.y);
    if (n_items == MAX_RECTS)
        return -1;
    jbi_placer_mark(p, got, 1);
    items[n_items].r = got;
    items[n_items].q = *q;
    items[n_items].placed = 1;
    sat_dirty = 1;
    return n_items++;
}

static void release(jbi_placer *p, int i)
{
    jbi_placer_mark(p, items[i].r, 0);
    items[i] = items[--n_items];
    sat_dirty = 1;
}

/* ------------------------------------------------------------------------ */
/* Requests as jb_texture_create makes them */

static uint64_t seed;

static unsigned rnd(void)
{
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(seed >> 33);
}

static int below(int n)
{
    return (int)(rnd() % (unsigned)n);
}

/* The texels' request (src/api/texture.c); returns the palette's entries. */
static int texture_req(jbi_place_req *q, int depth, int w, int h, int repeat)
{
    int per = depth == 4 ? 4 : depth == 8 ? 2 : 1;

    q->w = (w + per - 1) / per;
    q->h = h;
    q->align = 1;
    q->align_y = 1;
    if (repeat) {
        int wpx = w / per;
        q->align = wpx < 64 ? wpx : 64;
        q->align_y = h;
    }
    q->span = 256 / per;
    q->from_bottom = 0;
    return depth == 4 ? 16 : depth == 8 ? 256 : 0;
}

static int random_size(void)
{
    int k = below(8);

    if (k == 0)
        return 1 + below(256);
    if (k == 1)
        return 256;
    return 1 + below(64);
}

static jbi_placer fresh; /* 64 KB: not on the stack (wasm's is small) */
static jbi_placer with_extra;
static uint64_t bands[JBI_PLACER_H][JBI_PLACER_WORDS];

/* jbi_placer_find_banded against jbi_placer_find on a copy of the placer
   with the extra rectangle marked: the same answer. */
static void check_banded(const jbi_placer *p, const jbi_place_req *q, const char *what)
{
    jbi_rect extra, a, b;
    int k, fa, fb;
    char m[128];

    if (q->from_bottom)
        return;
    jbi_placer_bands(p, q->h, bands);
    for (k = 0; k < 3; ++k) {
        extra.x = below(W - 256), extra.y = below(H);
        extra.w = k == 0 ? 0 : k == 1 ? 16 : 256, extra.h = 1;
        with_extra = *p;
        jbi_placer_mark(&with_extra, extra, 1);
        fa = jbi_placer_find_banded((const uint64_t(*)[JBI_PLACER_WORDS])bands, q, extra, &a);
        fb = jbi_placer_find(&with_extra, q, &b);
        snprintf(m, sizeof m, "%s: banded, extra %d", what, k);
        check(m, fa, fb);
        if (fa && fb)
            check(m, a.x == b.x && a.y == b.y, 1);
    }
}

static void random_sequence(int layout, int steps)
{
    static const int sizes[][2] = {{320, 240}, {640, 480}, {512, 300}, {256, 240},
                                   {1024, 512}, {427, 240}, {368, 240}, {1, 1}};
    jbi_placer *p = malloc(sizeof *p);
    jbi_rect buf[2];
    int n, i, step, fails = 0, placed = 0;
    char what[64];

    if (p == NULL) {
        printf("out of memory\n");
        exit(2);
    }
    jbi_placer_init(p);
    n_items = 0;
    sat_dirty = 1;
    n = jbi_frame_buffers(sizes[layout][0], sizes[layout][1], buf);
    for (i = 0; i < n; ++i)
        add_fixed(p, buf[i]);
    for (i = below(4); i > 0; --i) { /* reservations, overlapping or not */
        jbi_rect r;
        r.x = below(W);
        r.y = below(H);
        r.w = 1 + below(W - r.x < 200 ? W - r.x : 200);
        r.h = 1 + below(H - r.y < 100 ? H - r.y : 100);
        add_fixed(p, r);
    }
    for (step = 0; step < steps; ++step) {
        snprintf(what, sizeof what, "layout %d step %d", layout, step);
        if (n_items > 0 && below(4) == 0) {
            /* free a placed one */
            int k = below(n_items), tries;
            for (tries = 0; tries < n_items && !items[k].placed; ++tries)
                k = (k + 1) % n_items;
            if (items[k].placed)
                release(p, k);
        } else {
            static const int depths[3] = {4, 8, 15};
            jbi_place_req q;
            int depth = depths[below(3)], repeat = below(4) == 0, w, h, entries, t;
            if (repeat) {
                w = 8 << below(6);
                h = 8 << below(6);
            } else {
                w = random_size();
                h = random_size();
            }
            entries = texture_req(&q, depth, w, h, repeat);
            if (step % 7 == 0)
                check_next(p, &q, what);
            if (step % 5 == 0)
                check_banded(p, &q, what);
            t = place(p, &q, what);
            if (t < 0)
                ++fails;
            else
                ++placed;
            if (t >= 0 && entries != 0) {
                q.w = entries;
                q.h = 1;
                q.align = 16;
                q.align_y = 1;
                q.span = 0;
                q.from_bottom = 1;
                if (step % 7 == 3)
                    check_next(p, &q, what);
                if (place(p, &q, what) < 0)
                    release(p, t); /* as jb_texture_create gives the texels back */
            }
        }
        check_all(what);
    }
    /* Once every placed one is freed, what remains taken is exactly the
       fixed rectangles. */
    for (i = n_items - 1; i >= 0; --i)
        if (items[i].placed)
            release(p, i);
    jbi_placer_init(&fresh);
    for (i = 0; i < n_items; ++i)
        jbi_placer_mark(&fresh, items[i].r, 1);
    snprintf(what, sizeof what, "layout %d: freed back to the fixed ones", layout);
    check(what, memcmp(fresh.used, p->used, sizeof fresh.used) == 0, 1);
    /* The run filled VRAM at some point, and placed plenty. */
    snprintf(what, sizeof what, "layout %d: some requests failed", layout);
    check(what, fails > 0, 1);
    snprintf(what, sizeof what, "layout %d: some requests placed", layout);
    check(what, placed > 20, sizes[layout][0] != 1024); /* 1024 x 512: none */
    free(p);
}

/* ------------------------------------------------------------------------ */
/* Hand-made cases */

static jbi_rect rect(int x, int y, int w, int h)
{
    jbi_rect r;

    r.x = x, r.y = y, r.w = w, r.h = h;
    return r;
}

static jbi_place_req req(int w, int h, int align, int align_y, int span, int from_bottom)
{
    jbi_place_req q;

    q.w = w, q.h = h, q.align = align, q.align_y = align_y, q.span = span;
    q.from_bottom = from_bottom;
    return q;
}

static void hand_cases(void)
{
    static jbi_placer p;
    jbi_place_req q = req(64, 256, 1, 1, 64, 0);
    jbi_rect r;
    int i;

    jbi_placer_init(&p);
    check("empty: found", jbi_placer_find(&p, &q, &r), 1);
    check("empty: at 0, 0", r.x == 0 && r.y == 0 && r.w == 64 && r.h == 256, 1);

    /* A 4-bit texture 16 pixels wide, with x 0..59 taken: not across the
       boundary at 64, so at 64. */
    jbi_placer_mark(&p, rect(0, 0, 60, 256), 1);
    q = req(16, 256, 1, 1, 64, 0);
    check("span 64: found", jbi_placer_find(&p, &q, &r), 1);
    check("span 64: x", r.x, 64);
    /* 8-bit, 100 pixels: page 0's span ends at 128, so at 64 (page 64,
       up to 192). */
    q = req(100, 256, 1, 1, 128, 0);
    check("span 128: found", jbi_placer_find(&p, &q, &r), 1);
    check("span 128: x", r.x, 64);
    /* 15-bit at the right edge: x + w <= 1024, page 768. */
    jbi_placer_init(&p);
    jbi_placer_mark(&p, rect(0, 0, 800, 512), 1);
    q = req(224, 10, 1, 1, 256, 0);
    check("edge: found", jbi_placer_find(&p, &q, &r), 1);
    check("edge: x", r.x, 800);
    q.w = 225;
    check("edge: one more is too wide", jbi_placer_find(&p, &q, &r), 0);
    /* Never across row 256. */
    jbi_placer_init(&p);
    jbi_placer_mark(&p, rect(0, 0, 1024, 200), 1);
    q = req(10, 100, 1, 1, 256, 0);
    check("rows: found", jbi_placer_find(&p, &q, &r), 1);
    check("rows: y", r.y, 256);
    /* Repeat alignment, with pixel (0, 0) taken. */
    jbi_placer_init(&p);
    jbi_placer_mark(&p, rect(0, 0, 1, 1), 1);
    q = req(8, 32, 8, 32, 64, 0);
    check("align: found", jbi_placer_find(&p, &q, &r), 1);
    check("align: x", r.x, 8);
    check("align: y", r.y, 0);
    /* Palettes from the bottom. */
    q = req(256, 1, 16, 1, 0, 1);
    check("palette: found", jbi_placer_find(&p, &q, &r), 1);
    check("palette: at 0, 511", r.x == 0 && r.y == 511, 1);
    jbi_placer_mark(&p, rect(0, 511, 250, 1), 1);
    check("palette: found again", jbi_placer_find(&p, &q, &r), 1);
    check("palette: column 0, above", r.x == 0 && r.y == 510, 1);
    jbi_placer_mark(&p, rect(0, 0, 1, 511), 1);
    check("palette: found a third time", jbi_placer_find(&p, &q, &r), 1);
    check("palette: column 16, at the bottom", r.x == 16 && r.y == 510, 1);
    /* Requests outside the ranges find nothing. */
    {
        const jbi_place_req bad[] = {req(0, 1, 1, 1, 0, 0),    req(1, 0, 1, 1, 0, 0),
                                     req(1, 257, 1, 1, 0, 0),  req(1, 1, 3, 1, 0, 0),
                                     req(1, 1, 128, 1, 0, 0),  req(1, 1, 1, 0, 0, 0),
                                     req(1, 1, 1, 3, 0, 0),    req(1, 1, 1, 1, 32, 0),
                                     req(65, 1, 1, 1, 64, 0),  req(1025, 1, 1, 1, 0, 0)};
        for (i = 0; i < (int)(sizeof bad / sizeof bad[0]); ++i)
            check("bad request", jbi_placer_find(&p, &bad[i], &r), 0);
    }
    /* Full: nothing found; one pixel freed, a 1 x 1 is found there. */
    jbi_placer_init(&p);
    jbi_placer_mark(&p, rect(0, 0, 1024, 512), 1);
    q = req(1, 1, 1, 1, 0, 0);
    check("full: nothing", jbi_placer_find(&p, &q, &r), 0);
    q.from_bottom = 1;
    check("full: nothing from the bottom", jbi_placer_find(&p, &q, &r), 0);
    jbi_placer_mark(&p, rect(777, 333, 1, 1), 0);
    check("full: one free", jbi_placer_find(&p, &q, &r), 1);
    check("full: there", r.x == 777 && r.y == 333, 1);
    check("is_free", jbi_placer_is_free(&p, rect(777, 333, 1, 1)), 1);
    check("is_free, taken", jbi_placer_is_free(&p, rect(776, 333, 2, 1)), 0);
    /* Marks clip to VRAM. */
    jbi_placer_init(&p);
    jbi_placer_mark(&p, rect(1000, 500, 100, 100), 1);
    check("clipped mark", jbi_placer_is_free(&p, rect(0, 0, 1000, 512)), 1);
    check("clipped mark, taken", jbi_placer_is_free(&p, rect(1023, 511, 1, 1)), 0);
    check("clipped mark, below", jbi_placer_is_free(&p, rect(0, 0, 1024, 500)), 1);
}

/* Rectangles past VRAM, or whose ends do not fit an int, are
   clipped (marking, testing, and the banded search's extra rectangle) as
   VRAM's edges allow, without overflow or reading past a row. */
static void extreme_rects(void)
{
    static jbi_placer p, marked;
    static uint64_t b[JBI_PLACER_H][JBI_PLACER_WORDS];
    static const jbi_rect odd[] = {{1024, 0, 1, 1},           {0x7fffffff, 0, 1, 1},
                                   {-5, 0, 10, 1},            {1000, 3, 0x7fffffff, 1},
                                   {-0x7fffffff, 0, 0x7fffffff, 2}, {10, 511, 5, 0x7fffffff},
                                   {10, -0x7fffffff, 5, 0x7fffffff}, {(int)(-0x7fffffff - 1), 0, 1, 1},
                                   /* empty: no-ops, as jbi_placer_mark takes them */
                                   {0, 4, 20, 0}, {0, 4, 0, 5}, {0, 4, 20, -3}};
    jbi_place_req q;
    jbi_rect a, c;
    int i, h, fa, fb;
    char m[64];

    jbi_placer_init(&p);
    jbi_placer_mark(&p, rect(0, 0, 1024, 3), 1);
    jbi_placer_mark(&p, rect(30, 3, 1, 1), 1);
    for (h = 1; h <= 4; h += 3) {
        q = req(20, h, 1, 1, 0, 0);
        jbi_placer_bands(&p, q.h, b);
        for (i = 0; i < (int)(sizeof odd / sizeof odd[0]); ++i) {
            marked = p;
            jbi_placer_mark(&marked, odd[i], 1);
            fa = jbi_placer_find_banded((const uint64_t(*)[JBI_PLACER_WORDS])b, &q, odd[i], &a);
            fb = jbi_placer_find(&marked, &q, &c);
            snprintf(m, sizeof m, "extreme extra %d, h %d: banded = marked", i, h);
            check(m, fa == fb && (!fa || (a.x == c.x && a.y == c.y)), 1);
            /* is_free and mark on it run without overflow (UBSan) */
            snprintf(m, sizeof m, "extreme rect %d: is_free is 0 or 1", i);
            check(m, (unsigned)jbi_placer_is_free(&p, odd[i]) <= 1u, 1);
            jbi_placer_mark(&marked, odd[i], 0);
        }
    }
    /* What the clipping keeps: {-5, 0, 10, 1} is x 0..4 of row 0. */
    jbi_placer_init(&p);
    jbi_placer_mark(&p, rect(-5, 0, 10, 1), 1);
    check("extreme: -5 + 10 marks 0..4", jbi_placer_is_free(&p, rect(4, 0, 1, 1)), 0);
    check("extreme: and not 5", jbi_placer_is_free(&p, rect(5, 0, 1, 1)), 1);
    jbi_placer_init(&p);
    jbi_placer_mark(&p, rect(1000, 3, 0x7fffffff, 1), 1);
    check("extreme: 1000 + INT_MAX marks 1000..1023", jbi_placer_is_free(&p, rect(1023, 3, 1, 1)), 0);
    check("extreme: and not 999", jbi_placer_is_free(&p, rect(999, 3, 1, 1)), 1);
}

int main(void)
{
    int layout;

    extreme_rects();
    hand_cases();
    for (layout = 0; layout < 8; ++layout) {
        seed = 0x9e3779b97f4a7c15ull * (uint64_t)(layout + 1);
        random_sequence(layout, 400);
    }
    printf("placer: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}

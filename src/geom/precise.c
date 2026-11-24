/*
 * Sub-pixel vertex precision (precise.h): a fixed-size hash from the
 * packed SXY values the GTE produced this frame to their unrounded
 * positions.
 *
 * Table. PRECISE_SLOTS slots, open addressing with linear probing over at
 * most PRECISE_PROBES slots. Each slot carries the frame stamp it was
 * written in, so begin_frame only advances the stamp; nothing is cleared,
 * allocated or freed per frame. When every probed slot holds another
 * record of this frame, a new record evicts the oldest of them, so a
 * crowded frame loses old vertices rather than new ones. The table is sized
 * so that a busy frame leaves it mostly empty and evictions are rare.
 *
 * One key, several vertices. A key is only an integer screen position, so
 * different vertices share keys: two objects that cover the same pixel,
 * or a renderer projecting one vertex in two passes whose vertex
 * coordinates are scaled differently (its depth then differs by the ratio
 * of the scales, the screen position by a fraction of a pixel). The positions
 * differ by under a pixel, but the depths can differ by any factor, and a
 * perspective-correct triangle that takes one vertex's depth from another
 * object warps visibly. So a record for a key already in the table
 * replaces it only if the depths agree (the same vertex projected again);
 * otherwise it takes a slot of its own. Each record keeps the range of
 * projection sequence numbers it was made at (seq counts records within
 * the frame). jbi_precise_lookup returns the latest record for a key;
 * jbi_precise_lookup_poly picks, for each vertex of one primitive, the
 * records projected closest together, since a routine projects the
 * vertices of a mesh together before building its polygons from them.
 */
#include "geom/precise.h"

#include <stdlib.h>

enum {
    PRECISE_BITS = 14,
    PRECISE_SLOTS = 1 << PRECISE_BITS,
    PRECISE_PROBES = 16,
    SEQ_MAX = 0xffff,
};

typedef struct {
    uint32_t key;
    uint16_t stamp;
    uint16_t first, last; /* seq of the first and last projection */
    int16_t dx, dy; /* position minus the key's integer position, 1/256 px */
    uint16_t z;
} Slot;

_Static_assert(sizeof(Slot) == 16, "a slot is 16 bytes");

struct jbi_precise {
    int enabled;
    uint16_t stamp; /* the current frame's */
    unsigned seq;   /* records made since begin_frame, up to SEQ_MAX */
    jbi_precise_stats counters;
    Slot slots[PRECISE_SLOTS];
};

/* Records of one key with depths this close (a 1/16 difference) are the
   same vertex, projected again. */
static inline int same_depth(int a, int b)
{
    int lo = a < b ? a : b, hi = a < b ? b : a;

    return (hi - lo) * 16 <= hi;
}

static inline unsigned slot_of(uint32_t key)
{
    return (key * 0x9e3779b1u) >> (32 - PRECISE_BITS);
}

/* The GPU reads 11-bit signed coordinates; bits 11..15 of each half do
   not matter to it, so they do not matter to the key either. The GTE's
   own values are already in this form. */
static inline uint32_t normalise(uint32_t xy)
{
    return (((uint32_t)((int32_t)(xy << 21) >> 21)) & 0xffffu) |
           ((uint32_t)((int32_t)(xy << 5) >> 21) << 16);
}

static inline int key_x(uint32_t key) { return (int16_t)(key & 0xffffu); }
static inline int key_y(uint32_t key) { return (int16_t)(key >> 16); }

size_t jbi_precise_size(void)
{
    return sizeof(jbi_precise);
}

void jbi_precise_init(jbi_precise *t)
{
    t->stamp = 1;
}

jbi_precise *jbi_precise_create(void)
{
    jbi_precise *t = calloc(1, sizeof *t);

    if (t != NULL)
        jbi_precise_init(t);
    return t;
}

void jbi_precise_destroy(jbi_precise *t)
{
    free(t);
}

void jbi_precise_enable(jbi_precise *t, int on)
{
    t->enabled = on != 0;
}

int jbi_precise_enabled(const jbi_precise *t)
{
    return t->enabled;
}

void jbi_precise_begin_frame(jbi_precise *t)
{
    if (++t->stamp == 0) {
        /* Wrapped after 2^16 frames: forget every stamp. */
        unsigned i;

        for (i = 0; i < PRECISE_SLOTS; i++)
            t->slots[i].stamp = 0;
        t->stamp = 1;
    }
    t->seq = 0;
    t->counters.frames++;
}

void jbi_precise_record(jbi_precise *t, uint32_t sxy, const jbi_precise_vertex *v)
{
    unsigned h, i;
    int dx, dy, z;
    Slot *s;

    if (!t->enabled)
        return;
    sxy = normalise(sxy);
    dx = v->x - key_x(sxy) * JBI_PRECISE_ONE;
    dy = v->y - key_y(sxy) * JBI_PRECISE_ONE;
    /* The interface promises a position within a pixel of the key. */
    if (dx < -JBI_PRECISE_ONE || dx >= 2 * JBI_PRECISE_ONE || dy < -JBI_PRECISE_ONE ||
        dy >= 2 * JBI_PRECISE_ONE) {
        t->counters.rejected++;
        return;
    }

    z = v->z < 0 ? 0 : v->z > 0xffff ? 0xffff : v->z;
    h = slot_of(sxy);
    s = 0;
    for (i = 0; i < PRECISE_PROBES; i++) {
        Slot *c = &t->slots[(h + i) & (PRECISE_SLOTS - 1)];

        if (c->stamp != t->stamp) {
            s = c; /* the end of the chain: a new record */
            s->first = (uint16_t)t->seq;
            break;
        }
        if (c->key == sxy && same_depth(c->z, z)) {
            s = c; /* the same vertex again: the last write wins */
            t->counters.replaced++;
            break;
        }
    }
    if (s == 0) {
        /* A full chain: replace its oldest record. */
        s = &t->slots[h];
        for (i = 1; i < PRECISE_PROBES; i++) {
            Slot *c = &t->slots[(h + i) & (PRECISE_SLOTS - 1)];

            if (c->last < s->last)
                s = c;
        }
        s->first = (uint16_t)t->seq;
        t->counters.evicted++;
    }
    s->key = sxy;
    s->stamp = t->stamp;
    s->last = (uint16_t)t->seq;
    if (t->seq < SEQ_MAX)
        t->seq++;
    s->dx = (int16_t)dx;
    s->dy = (int16_t)dy;
    s->z = (uint16_t)z;
    t->counters.records++;
}

/* The slot of the latest record of key sxy (normalised) this frame, or -1. */
static int latest(const jbi_precise *t, uint32_t sxy)
{
    unsigned h = slot_of(sxy), i;
    int best = -1;

    for (i = 0; i < PRECISE_PROBES; i++) {
        unsigned k = (h + i) & (PRECISE_SLOTS - 1);
        const Slot *c = &t->slots[k];

        if (c->stamp != t->stamp)
            break;
        if (c->key == sxy && (best < 0 || c->last > t->slots[best].last))
            best = (int)k;
    }
    return best;
}

static void fill(uint32_t sxy, const Slot *s, jbi_precise_vertex *v)
{
    v->x = key_x(sxy) * JBI_PRECISE_ONE + s->dx;
    v->y = key_y(sxy) * JBI_PRECISE_ONE + s->dy;
    v->z = s->z;
}

int jbi_precise_lookup(const jbi_precise *t, uint32_t sxy, jbi_precise_vertex *v)
{
    int k;

    sxy = normalise(sxy);
    k = latest(t, sxy);
    if (k < 0)
        return 0;
    fill(sxy, &t->slots[k], v);
    return 1;
}

/* How far apart, in projections, two records were made: 0 if their
   ranges overlap. */
static unsigned seq_gap(const Slot *a, const Slot *b)
{
    if (b->first > a->last)
        return (unsigned)(b->first - a->last);
    if (a->first > b->last)
        return (unsigned)(a->first - b->last);
    return 0;
}

/*
 * Records looked at per vertex, the latest: at most 6^4 combinations.
 *
 * A vertex whose record was made more than POLY_ISOLATED projections from
 * every other vertex's is most likely another object's, the polygon's own
 * vertex having no record (one the program built without projecting it).
 * The vertices of one polygon are normally projected close together, and a
 * record from far away tends to have an unrelated depth. Such a vertex
 * keeps its position, which is within a pixel either way, but not its
 * depth.
 */
enum { POLY_CANDIDATES = 6, POLY_ISOLATED = 512 };

unsigned jbi_precise_lookup_poly(const jbi_precise *t, const uint32_t *sxy, int n,
                                 jbi_precise_vertex *v)
{
    const Slot *cand[JBI_PRECISE_POLY_MAX][POLY_CANDIDATES];
    int count[JBI_PRECISE_POLY_MAX], pick[JBI_PRECISE_POLY_MAX], best[JBI_PRECISE_POLY_MAX];
    uint32_t key[JBI_PRECISE_POLY_MAX];
    unsigned hits = 0, best_cost = ~0u;
    int i, j, many = 0;

    if (n > JBI_PRECISE_POLY_MAX)
        n = JBI_PRECISE_POLY_MAX;
    for (i = 0; i < n; i++) {
        unsigned h, p;

        key[i] = normalise(sxy[i]);
        h = slot_of(key[i]);
        count[i] = 0;
        /* Candidates, latest first (insertion by last). */
        for (p = 0; p < PRECISE_PROBES; p++) {
            const Slot *c = &t->slots[(h + p) & (PRECISE_SLOTS - 1)];
            int k;

            if (c->stamp != t->stamp)
                break;
            if (c->key != key[i])
                continue;
            k = count[i] < POLY_CANDIDATES ? count[i]++ : POLY_CANDIDATES;
            while (k > 0 && c->last > cand[i][k - 1]->last) {
                if (k < POLY_CANDIDATES)
                    cand[i][k] = cand[i][k - 1];
                k--;
            }
            if (k < POLY_CANDIDATES)
                cand[i][k] = c;
        }
        if (count[i] > 0)
            hits |= 1u << i;
        if (count[i] > 1)
            many = 1;
        pick[i] = best[i] = 0;
    }

    /* Each combination of candidates, latest first, keeping the first with
       the smallest sum of pairwise gaps. Nearly every primitive has one
       combination; more have several when a program projects its scenery
       twice. */
    while (many) {
        unsigned cost = 0;

        for (i = 0; i < n; i++) {
            if (count[i] == 0)
                continue;
            for (j = i + 1; j < n; j++) {
                if (count[j] > 0)
                    cost += seq_gap(cand[i][pick[i]], cand[j][pick[j]]);
            }
        }
        if (cost < best_cost) {
            best_cost = cost;
            for (i = 0; i < n; i++)
                best[i] = pick[i];
        }
        for (i = n - 1; i >= 0; i--) {
            if (count[i] > 1 && ++pick[i] < count[i])
                break;
            pick[i] = 0;
        }
        if (i < 0)
            break;
    }
    for (i = 0; i < n; i++) {
        unsigned gap = ~0u;

        if (count[i] == 0)
            continue;
        fill(key[i], cand[i][best[i]], &v[i]);
        for (j = 0; j < n; j++) {
            if (j != i && count[j] > 0) {
                unsigned g = seq_gap(cand[i][best[i]], cand[j][best[j]]);
                gap = g < gap ? g : gap;
            }
        }
        if (gap != ~0u && gap > POLY_ISOLATED)
            v[i].z = 0;
    }
    return hits;
}

void jbi_precise_derive(jbi_precise *t, uint32_t out, const uint32_t *in, const int *w, int n,
                        int shift)
{
    long long sx = 0, sy = 0, sz = 0, round = shift > 0 ? 1LL << (shift - 1) : 0;
    jbi_precise_vertex v;
    int i;

    if (!t->enabled)
        return;
    for (i = 0; i < n; i++) {
        if (!jbi_precise_lookup(t, in[i], &v)) {
            t->counters.underived++;
            return;
        }
        sx += (long long)w[i] * v.x;
        sy += (long long)w[i] * v.y;
        sz += (long long)w[i] * v.z;
    }
    v.x = (int)((sx + round) >> shift);
    v.y = (int)((sy + round) >> shift);
    v.z = (int)((sz + round) >> shift);
    t->counters.derived++;
    jbi_precise_record(t, out, &v);
}

void jbi_precise_offset(jbi_precise *t, uint32_t out, uint32_t in)
{
    jbi_precise_vertex v;

    if (!t->enabled)
        return;
    if (!jbi_precise_lookup(t, in, &v)) {
        t->counters.underived++;
        return;
    }
    out = normalise(out);
    in = normalise(in);
    v.x += (key_x(out) - key_x(in)) * JBI_PRECISE_ONE;
    v.y += (key_y(out) - key_y(in)) * JBI_PRECISE_ONE;
    t->counters.derived++;
    jbi_precise_record(t, out, &v);
}

void jbi_precise_set_depth(jbi_precise *t, uint32_t sxy, int z)
{
    int k;

    if (!t->enabled)
        return;
    /* The record the routine just made for sxy. */
    k = latest(t, normalise(sxy));
    if (k >= 0)
        t->slots[k].z = (uint16_t)(z < 0 ? 0 : z > 0xffff ? 0xffff : z);
}

int jbi_precise_count(const jbi_precise *t)
{
    unsigned i;
    int n = 0;

    for (i = 0; i < PRECISE_SLOTS; i++)
        n += t->slots[i].stamp == t->stamp;
    return n;
}

void jbi_precise_stats_get(jbi_precise *t, jbi_precise_stats *out, int reset)
{
    if (out != 0)
        *out = t->counters;
    if (reset) {
        jbi_precise_stats zero = { 0 };

        t->counters = zero;
    }
}

jbi_precise_stats *jbi_precise_counters(jbi_precise *t)
{
    return &t->counters;
}

/* ------------------------------------------------------------------------ */
/* As the command decoder's source (core/precise_source.h) */

static int source_enabled(void *user)
{
    return jbi_precise_enabled(user);
}

static int source_lookup(void *user, uint32_t sxy, jbi_precise_vertex *v)
{
    return jbi_precise_lookup(user, sxy, v);
}

static unsigned source_lookup_poly(void *user, const uint32_t *sxy, int n, jbi_precise_vertex *v)
{
    return jbi_precise_lookup_poly(user, sxy, n, v);
}

jbi_precise_source jbi_precise_as_source(jbi_precise *t)
{
    jbi_precise_source s;

    s.enabled = source_enabled;
    s.lookup = source_lookup;
    s.lookup_poly = source_lookup_poly;
    s.user = t;
    return s;
}

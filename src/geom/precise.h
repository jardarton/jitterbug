#ifndef JBI_GEOM_PRECISE_H
#define JBI_GEOM_PRECISE_H
/*
 * Sub-pixel vertex precision shared by the GTE and the GPU, for rendering
 * above the native resolution: the table the GTE records into and the
 * command decoder looks up (as a jbi_precise_source,
 * src/core/precise_source.h).
 *
 * The GTE records the unrounded projection of each SXY it produces, keyed
 * by the packed 32-bit SXY value. When the GPU reads a primitive vertex it
 * looks the packed value up and gets the precise position (and depth) if
 * one was recorded since the last jbi_precise_begin_frame. A vertex the
 * program moved or built itself has no entry and stays on the integer grid.
 * Lookups are by value only, so no tracking through the program is needed.
 * Two vertices with the same integer position but different depths are
 * both kept, and a primitive's lookup (jbi_precise_lookup_poly) picks the
 * records projected together with its other vertices, so that it gets its
 * own vertices' depths.
 *
 * Coordinates are in 1/JBI_PRECISE_ONE pixels, before the drawing offset.
 *
 * Both sides are no-ops unless jbi_precise_enable(t, 1) was called, so the
 * default 1x output stays bit-exact.
 *
 * A table is an object (jbi_precise_create); tables are independent. One
 * table is used from one thread at a time.
 *
 * Who calls what:
 *   - jbi_precise_enable: the program, when the GPU's scale is above 1x.
 *   - jbi_precise_begin_frame: the program, at the start of every draw
 *     pass.
 *   - jbi_precise_record: the GTE (src/geom/gte.h), for every RTPS/RTPT
 *     projection, into the table it was given.
 *   - jbi_precise_derive/offset/set_depth: the program's renderers, for
 *     vertices they derive from projected ones in screen space.
 *   - jbi_precise_lookup_poly: the GPU, per polygon; jbi_precise_lookup
 *     for lines and single vertices (through jbi_precise_as_source).
 */

#include <stddef.h>

#include "core/precise_source.h"

typedef struct jbi_precise jbi_precise;

/* A new, empty and disabled table, or NULL without memory. */
jbi_precise *jbi_precise_create(void);
void jbi_precise_destroy(jbi_precise *t);

/* For a table in memory the caller allocates (the public API's, from the
   context's allocator): the bytes one takes, and making zeroed memory of
   that size into an empty, disabled table, as jbi_precise_create does. */
size_t jbi_precise_size(void);
void jbi_precise_init(jbi_precise *t);

void jbi_precise_enable(jbi_precise *t, int on);
int jbi_precise_enabled(const jbi_precise *t);

/* Forget the recorded vertices; called once per frame. */
void jbi_precise_begin_frame(jbi_precise *t);

/* GTE side: record the unrounded projection of a packed SXY. */
void jbi_precise_record(jbi_precise *t, uint32_t sxy, const jbi_precise_vertex *v);

/* GPU side: look up a packed SXY. Returns 1 and fills *v if recorded.
   With several records for the key (different depths), the latest. */
int jbi_precise_lookup(const jbi_precise *t, uint32_t sxy, jbi_precise_vertex *v);

/*
 * GPU side: look up the n vertex words of one primitive (n at most
 * JBI_PRECISE_POLY_MAX) together. Returns a mask with bit i set if vertex i
 * hit, and fills v[i] for those. Where a key has records of several
 * vertices (several depths), it takes for each vertex the record that was
 * projected nearest in time to the others' (the smallest sum of gaps in
 * projection order), and the latest records on a tie, so a polygon gets its
 * own mesh's vertices rather than another object's on the same pixel. A
 * vertex whose record was projected far from all the others' (most likely
 * another object's, its own having none) comes back with z = 0, depth
 * unknown. Otherwise, with one record per key, it returns what
 * jbi_precise_lookup would.
 */
unsigned jbi_precise_lookup_poly(const jbi_precise *t, const uint32_t *sxy, int n,
                                 jbi_precise_vertex *v);

/* The table as the command decoder's precise source (enabled, lookup,
   lookup_poly above). The table must outlive the source's use. */
jbi_precise_source jbi_precise_as_source(jbi_precise *t);

/*
 * What a lookup guarantees (for the GPU side):
 *   - Pass the vertex word from the packet as it is. Only the 11-bit
 *     coordinates the GPU decodes take part in the key; bits 11..15 of
 *     each half are ignored, as by the hardware.
 *   - A key can have several records this frame, one per distinct depth
 *     (records whose depths agree within 1/16 are the same vertex projected
 *     again, and the last wins). jbi_precise_lookup_poly chooses among them.
 *   - A hit is within one pixel of the key's integer position: x and y lie
 *     in [k - 1, k + 2) pixels for key coordinate k, and in [k, k + 1),
 *     the GTE's truncation, for most vertices. Vertices whose SX
 *     or SY the GTE saturated (to -0x400..0x3ff) are not recorded, so
 *     their keys miss and they stay where the program put them.
 *   - z is the vertex's view-space depth in SZ units: SZ itself, or the
 *     depth the routine actually used where it scaled the vertex for
 *     precision. z can be 0 for a vertex at or behind the camera plane.
 *   - Lookups only read the table. Records are made as the program
 *     projects and dropped by the next jbi_precise_begin_frame. The
 *     decoder looks a primitive's vertices up while the call that submits
 *     it runs and copies them into the primitive record.
 */

/*
 * Program side, for vertices a routine derives in screen space from projected
 * ones. Each does nothing unless enabled, or when a parent has no record.
 *
 * derive: record out at (sum of w[i] * position of in[i]) >> shift, rounded,
 *   with the depth blended the same way (a level-of-detail morph).
 * offset: record out, which is in moved by whole pixels, at in's position
 *   plus that offset, with in's depth (rims, sprites).
 * set_depth: correct the depth of sxy's latest record, where the routine
 *   scaled the vertex before projecting it.
 */
void jbi_precise_derive(jbi_precise *t, uint32_t out, const uint32_t *in, const int *w, int n,
                        int shift);
void jbi_precise_offset(jbi_precise *t, uint32_t out, uint32_t in);
void jbi_precise_set_depth(jbi_precise *t, uint32_t sxy, int z);

/*
 * Perspective-correct texturing, for the GPU side. The PlayStation
 * interpolates texture coordinates affinely in screen space; with every
 * vertex of a textured triangle recorded, the GPU can instead interpolate
 * u * w, v * w and w linearly, with w = jbi_precise_w(z), and divide per
 * pixel (or per span of a few pixels). Use it only when all three vertices
 * hit with z > 0; otherwise draw that triangle affinely. Clamp the result
 * to the bounding box of the three vertices' texture coordinates, so that
 * rounding near the edges cannot reach a neighbouring texture in the page.
 * Quads split into (0, 1, 2) and (1, 2, 3), as now.
 */
#define JBI_PRECISE_W_ONE (1 << 24)
static inline int jbi_precise_w(int z) { return z > 0 ? JBI_PRECISE_W_ONE / z : 0; }

/* Debugging and measurement. */
typedef struct jbi_precise_stats {
    unsigned frames;    /* jbi_precise_begin_frame calls */
    unsigned records;   /* vertices recorded */
    unsigned replaced;  /* records that replaced the same key and depth this frame */
    unsigned evicted;   /* records that evicted another key (table crowded) */
    unsigned refined;   /* projections recorded from the full-precision view position */
    unsigned coarse;    /* projections recorded with the GTE's own fraction */
    unsigned saturated; /* projections not recorded: SX or SY saturated */
    unsigned derived;   /* derive and offset records */
    unsigned underived; /* derive and offset calls with a parent missing */
    unsigned rejected;  /* records refused: more than a pixel from the key */
} jbi_precise_stats;

/* Copies the counters into *out (if not NULL), then zeroes them if reset. */
void jbi_precise_stats_get(jbi_precise *t, jbi_precise_stats *out, int reset);

/* The counters themselves, for the GTE, which counts the refined, coarse
   and saturated projections. */
jbi_precise_stats *jbi_precise_counters(jbi_precise *t);

/* Number of records (distinct keys and depths) since the last begin_frame. */
int jbi_precise_count(const jbi_precise *t);

#endif

/*
 * Where the command decoder gets sub-pixel vertex positions from, for
 * rendering above the native resolution.
 *
 * The GTE projects vertices to integer screen coordinates (SXY), and the
 * program copies those 16-bit values into GPU primitives. Upscaled, that
 * snaps geometry to the 1x pixel grid and makes it wobble. A precise source
 * knows the unrounded projection of the SXY values produced recently, keyed
 * by the packed 32-bit SXY value; gp0.c looks every vertex word up in it.
 * src/geom/precise.h is the table the GTE records into; tests supply their
 * own.
 *
 * A context has at most one source (jbi_set_precise_source,
 * src/core/context.h). Without one, every vertex stays on the integer grid
 * and no lookup is made.
 *
 * Coordinates are in 1/JBI_PRECISE_ONE pixels, before the drawing offset.
 *
 * Threading: the context calls the source only from the thread that is
 * calling into the context (inside jbi_gp0), never from a rasterising
 * worker, and copies what it needs into the primitive record, so a source
 * need only stay unchanged while that call runs.
 */
#ifndef JBI_CORE_PRECISE_SOURCE_H
#define JBI_CORE_PRECISE_SOURCE_H

#include <stdint.h>

#define JBI_PRECISE_SHIFT 8
#define JBI_PRECISE_ONE (1 << JBI_PRECISE_SHIFT)

/* The most vertices lookup_poly takes at once. */
#define JBI_PRECISE_POLY_MAX 4

typedef struct jbi_precise_vertex {
    int x, y; /* screen position, 1/JBI_PRECISE_ONE pixels */
    int z;    /* SZ (view-space depth), 0 if unknown */
} jbi_precise_vertex;

typedef struct jbi_precise_source {
    /*
     * enabled: whether the source has positions to give. The decoder asks
     * before it looks up a polygon's vertices for positions (only above
     * 1x); it may still look them up for depths (perspective-correct
     * texturing) when this returns 0.
     */
    int (*enabled)(void *user);
    /*
     * lookup: a packed SXY, as the vertex word in the packet. Returns 1
     * and fills *v if a position is known. Used for lines.
     */
    int (*lookup)(void *user, uint32_t sxy, jbi_precise_vertex *v);
    /*
     * lookup_poly: the n vertex words of one polygon (n at most
     * JBI_PRECISE_POLY_MAX) together. Returns a mask with bit i set if
     * vertex i hit, and fills v[i] for those.
     */
    unsigned (*lookup_poly)(void *user, const uint32_t *sxy, int n, jbi_precise_vertex *v);
    void *user;
} jbi_precise_source;

#endif

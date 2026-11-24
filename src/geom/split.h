/*
 * Screen-space clipping: makes any polygon or line in screen space into
 * pieces the GPU draws, for the friendly level's 2D draws (src/api/
 * frame.c) and its 3D geometry after near-plane clipping.
 *
 * The GPU's limits (psx-spx, "GPU"; include/jitterbug.h, Drawing): a
 * vertex's coordinates are -1024..1023 each, and a triangle or line with
 * two vertices more than 1023 pixels apart in x or 511 in y is not drawn
 * at all. A quad is its triangles v0 v1 v2 and v1 v2 v3, each checked
 * alone. The drawing area (GP0(E3h), GP0(E4h)) clips what is drawn to a
 * rectangle of pixels exactly, with no rounding, at every scale.
 *
 * DESIGN. A polygon that meets the limits is given unchanged. One that
 * does not is cut only where it leaves a clip rectangle, never inside it:
 * the view (the frame) is one tile, or two where it is more than 511
 * pixels high (or 1023 wide), and each tile's clip rectangle is the tile
 * widened evenly on each side as far as the limits allow (1023 x 511,
 * inside -1024..1023): a guard band, which the drawing area clips away.
 * The polygon is clipped to each rectangle exactly and drawn there as
 * triangles of the clipped polygon, found by ear clipping after every new
 * vertex has been rounded, so no rounding can turn a triangle over. New
 * vertices lie on a rectangle's edges and nowhere else. An edge of the
 * polygon that meets the limits itself is kept whole: where it leaves the
 * rectangle it gets an "ear", a sliver triangle (or two) between the edge
 * and the clipped polygon's side, so that it is drawn exactly as a
 * neighbour passed through draws it; the ear's far part lies outside the
 * rectangle, where the drawing area clips it. All of this is done for
 * each position format apart (INVARIANTS), so a polygon flat in one format
 * still draws in the other; one set of pieces serves both where it can.
 *
 * This replaced cutting polygons into halves (inside the visible area)
 * and then settling the overlaps rounding made, largest piece first:
 * settling dropped pieces that also covered pixels nobody else did (a
 * thin piece in the middle of a thick polygon, the part of a quad's
 * triangle that the other did not overlap), and each fix of one such case
 * left another.
 * Here no triangle is dropped for overlapping a neighbour: a polygon is
 * left out only where it is a sliver that rounding would turn over (N).
 * The guard band puts the cuts, and what rounding does at them (an edge
 * moved by up to half a pixel, a sliver turned over), outside the frame
 * where there is room. Texture coordinates are whole texels at a cut in
 * any design; on random magnified polygons this one samples the wrong
 * texel at about half as many pixels as halving did (31% against 62%,
 * mostly near-misses where the exact coordinate is close to a half).
 *
 * Costs: a polygon beyond the limits is a handful of triangles per tile
 * and format (its clipped polygon's, n + 2 at most, and up to two per edge
 * kept whole) instead of one record per half; where the formats differ (a
 * 3D polygon's integer and sub-pixel positions are separate projections,
 * and a quad's two triangles are always done apart) twice that. Pieces
 * reach outside the frame into the guard band, where the rasteriser skips
 * rows outside the drawing area. In a frame of one tile nothing more is
 * sent. In a frame of two, every piece of a clipped polygon carries its
 * tile's drawing area, and drawing sends GP0(E3h) and GP0(E4h) where the
 * area changes between records and the frame's own after (src/api/
 * record.h, area): two words a change.
 *
 * What the functions do, in order:
 *   1. Cull: a polygon of which every triangle the GPU would draw (a
 *      quad's two in Z order, emit_fan's for more vertices) has no area,
 *      or has the view's samples ([x0, x1 + 1) x [y0, y1 + 1)) wholly on
 *      the outer side of one of its edges or the view's, by its integer
 *      positions and by its sub-pixel ones, gives nothing: exact, so a
 *      triangle whose bounds meet the view but that cannot draw a pixel of
 *      it takes no record. A line is culled by the bounds of what it can
 *      draw: its ends' pixels at 1x, and in the scaled picture up to a
 *      pixel beyond its ends' sub-pixel positions (lines are a pixel
 *      thick), so it misses the view when both say so (jbi_split_line).
 *   2. Pass through: a polygon (every polygon of a primitive) that meets
 *      the limits is given to the callback unchanged: the same vertices,
 *      a quad in the GPU's Z order, more vertices as emit_fan's quads and
 *      triangle. Whatever meets the limits draws exactly what the same
 *      vertices sent as GP0 words draw.
 *   3. Otherwise every polygon of the primitive is clipped to each tile's
 *      rectangle, in each format: the vertices of the clipped polygon are
 *      the polygon's own, the rectangle's corners and the points where its
 *      edges meet the rectangle's sides, each from the edge's two ends
 *      alone, exact (rationals), then rounded (below). It is triangulated
 *      by ear clipping on the rounded positions, exactly, and the
 *      triangulation is checked (pairwise, exact separating axes); ears
 *      are added for kept edges.
 *
 * Rounding. A cut lies on a side of the rectangle and keeps its coordinate
 * across the side; its coordinate along the side is rounded to the
 * format's unit (a pixel, or 1/256 pixel):
 *   - on an edge beyond the limits, to the nearest, halves up: the same
 *     point for every polygon that has the edge, so shared edges match;
 *   - on a kept edge, inward, towards its neighbour along the side, so
 *     that its ear has the polygon's orientation; but where that would
 *     take it past where the neighbour is rounded to (a polygon less than
 *     a unit wide there), the other way, and the edge gets no ear; an
 *     exact cut needs none;
 *   - on a kept edge that the two polygons of a primitive share (a quad's
 *     diagonal), inward for both, each with its ear, only where every
 *     cut of it can go inward on both; else the edge is not kept in that
 *     tile and format, on either: all its cuts go to the nearest for both,
 *     with no ear, so that neither covers the other's ear and they meet
 *     along it, and the other cuts are rounded again against them.
 * Either way a cut moves along its side by less than a unit, and never
 * past another point on the side, so the order of the points along each
 * side is kept.
 *
 * INVARIANTS. A piece is drawn by one of two position formats: its integer
 * positions (format I: the 1x pass, and the scaled pass without precise
 * vertices) or its sub-pixel ones (format Q: the scaled pass with precise
 * vertices). A primitive is one polygon, or the polygons given together
 * (jbi_split_polys: a GPU quad's two triangles, a 3D face cut into two).
 * In each format apart:
 *
 *   (N) No pixel of one primitive is drawn twice, except where its own
 *       polygons overlap exactly (a crossed or concave quad's triangles,
 *       which the console draws twice there too). Passed through, it is the
 *       console's own drawing. Clipped:
 *         - Tiles: each tile's pieces are clipped to its pixels by the
 *           drawing area, so no two tiles draw a pixel twice; in a view of
 *           one tile the drawing area is the view.
 *         - In a tile, rounding moves the cuts along the rectangle's
 *           sides, keeping their order. Moving them there from their exact
 *           places takes no vertex across an edge (a moving edge sweeps a
 *           thin triangle at its cut; towards the cut's neighbour it lies
 *           inside the polygon, its only boundary points on that side;
 *           away from it, outside), unless a cut goes past where its
 *           neighbour was and the swept triangle holds a vertex: then the
 *           whole clipped polygon lies in that triangle, whose side on the
 *           rectangle is less than a unit, a sliver. Otherwise the rounded
 *           polygon is weakly simple, and ear clipping with exact
 *           orientations gives triangles of its orientation (or of no
 *           area) that tile it, pairwise disjoint. An ear lies between its
 *           edge and the cut's chord, on the far side of the chord from
 *           the polygon's other pieces.
 *         - The two polygons of a primitive share their common edge's cut
 *           exactly, or each draws it whole with an ear on its own side;
 *           their triangulations are checked against each other.
 *       A polygon whose triangulation fails (a sliver, above) is left out
 *       of that tile and format; one that overlaps the other polygon of
 *       its primitive leaves the smaller out. *dropped counts them.
 *       tests/split_fuzz.c samples the pieces exactly over seeded runs of
 *       the shapes that broke earlier designs and finds no sample drawn
 *       twice, and no sample more than 1.5 pixels inside a polygon left
 *       out (it leaves out some 40 slivers in 25000 shapes).
 *
 *   (C) Coverage: every sample of a clipped polygon in the view is drawn,
 *       except near its edges beyond the limits where they leave the
 *       rectangle, which a cut moves by less than a unit there (half a
 *       pixel in I, 1/512 pixel in Q, falling to nothing at the edge's
 *       other end), and in a polygon left out (N), which is less than a
 *       unit wide there.
 *
 *   (E) Shared edges between primitives: an edge within the limits is
 *       never moved (passed through, or drawn whole with an ear); an edge
 *       beyond them is cut at the same points by every polygon that has
 *       it. So two primitives that share an edge exactly meet along the
 *       same line, or polyline, in both formats, with no gap along it and
 *       no overlap across it, except where one of them is less than a unit
 *       wide where it leaves the rectangle: a kept edge whose cut had to go
 *       the other way, a polygon left out, or a vertex of one rounded
 *       across the other's edge.
 *
 * Interpolation at a new vertex, both ways, so that the choice can be made
 * when the piece is drawn: u, v, r, g, b linearly in screen space (the
 * affine attributes, which the console's affine rasteriser and the exact
 * 1x pass use, whatever the perspective-correct option); pu, pv, pr, pg,
 * pb, where both ends have z > 0, with attribute / z linear in screen
 * space (perspective-correct, so that a perspective-correct texture and
 * shading run on unbroken across the cut), else linearly too; and z with
 * 1/z linear where both are known, which is how depth varies across a
 * projected surface, else 0. A cut is a fraction t of the way along its
 * edge in its own format. A rectangle's corner inside a triangle takes
 * the triangle's affine attributes from its three corners (its plane);
 * otherwise, and for its depth and perspective-correct attributes, the
 * fraction along the side between the clipped polygon's vertices around
 * it. Each value is the
 * exact one rounded once to the nearest integer, halves away from zero
 * (an attribute 7.5 is 8), so the same from either end of an edge. All of
 * it in integer arithmetic (64 and 128 bits), so the pieces are the same
 * on every platform.
 *
 * A piece for one format carries the other's positions too: a format-I
 * piece its integer positions times 256, a format-Q piece its sub-pixel
 * positions rounded (an input vertex of an ear its own integer one, which
 * meets the limits). A piece for both carries both, within the decoder's
 * slack of each other (src/core/gp0.c), and the sub-pixel pieces'
 * attributes.
 *
 * Everything here is pure: no context, no allocation, no state.
 */
#ifndef JBI_GEOM_SPLIT_H
#define JBI_GEOM_SPLIT_H

#include <stdint.h>

/*
 * A vertex in screen space, in the coordinates the GPU's words carry
 * (before the drawing offset).
 *   x, y     the integer position the 1x pass draws, any int.
 *   qx, qy   the sub-pixel position the scaled pass draws with precise
 *            vertices, in 1/256 pixels. It should lie within 288/256 pixels
 *            of x * 256, y * 256 (the decoder's slack for precise
 *            positions); for integer-only geometry it is x * 256, y * 256.
 *            Where it does not, a clipped polygon uses the sub-pixel
 *            position rounded to the nearest pixel as the integer one. And
 *            where it does not in either coordinate with the integer one
 *            within -1024..1023, the precise pass's pieces of a clipped
 *            polygon are made apart, with the integer position as the
 *            sub-pixel one: where the precise pass draws the same vertex of
 *            a polygon passed through, since the decoder then ignores its
 *            sub-pixel position. The integer format's pieces are made as
 *            ever.
 *   z        the view depth (the GTE's SZ units, or any positive scale), or
 *            0 if unknown; > 0 where perspective-correct texturing should
 *            use it.
 *   r, g, b  0..255 each.
 *   u, v     texel coordinates, 0..255 each.
 *   pu, pv, pr, pg, pb
 *            the perspective-correct u, v, r, g, b (above). On output, a
 *            piece's. On input, read only with the view's
 *            perspective_given set: then they are the caller's, as for a
 *            vertex the caller made itself by cutting a polygon in screen
 *            space (the 3D pipeline's guard band), where the
 *            perspective-correct attributes differ from the affine ones;
 *            0..255 each like the affine ones. Without it they are not
 *            read and start equal to the affine ones, so callers that do
 *            not cut need not set them.
 */
typedef struct jbi_split_vertex {
    int x, y;
    int64_t qx, qy;
    int z;
    int r, g, b;
    int u, v;
    int pu, pv, pr, pg, pb;
} jbi_split_vertex;

/*
 * Where pieces may be drawn: the pixels x0..x1 by y0..y1, inclusive, in
 * the vertices' coordinates (the frame's back buffer is 0, 0, w - 1,
 * h - 1). Each within -1024..1023, at most 1024 wide and 512 high: a
 * display's size. The drawing area is set to it while pieces are drawn
 * (the frame's is the back buffer).
 *   perspective        not read (both interpolations are always made,
 *                      above); kept so that existing callers still compile.
 *   perspective_given  not 0: the input vertices' pu, pv, pr, pg, pb are
 *                      read (jbi_split_vertex). 0, as in a view
 *                      initialised with fewer fields: they are not.
 */
typedef struct jbi_split_view {
    int x0, y0, x1, y1;
    int perspective;
    int perspective_given;
} jbi_split_view;

/* A tile's pixels, inclusive, in the view's coordinates: the drawing area
   a piece needs (an ear, in a view of more than one tile). */
typedef struct jbi_split_area {
    int x0, y0, x1, y1;
} jbi_split_area;

enum {
    JBI_SPLIT_MAX_IN = 10,  /* the most vertices a polygon given may have */
    JBI_SPLIT_MAX_PRIM = 2, /* the most polygons of one primitive */
    JBI_SPLIT_MARGIN = 8    /* pixels around the view a line is clipped to */
};

/*
 * The passes a piece is drawn in: JBI_SPLIT_PASS_1X by its integer
 * positions (the 1x pass, and the scaled one without precise vertices),
 * JBI_SPLIT_PASS_PRECISE by its sub-pixel ones (the scaled pass with
 * precise vertices). A piece passed through has both; a clipped one the
 * formats it was made for and has area in.
 */
enum { JBI_SPLIT_PASS_1X = 1, JBI_SPLIT_PASS_PRECISE = 2 };

/*
 * A piece of polygon poly (0 for jbi_split_poly): n is 3 (a triangle) or
 * 4 (a quad in the GPU's Z order: v[0] v[1] v[2] and v[1] v[2] v[3] are
 * its triangles, so going round it is v[0] v[1] v[3] v[2]); its passes;
 * area NULL, or the tile it must be clipped to. The vertices are valid
 * during the call only.
 */
typedef void (*jbi_split_fn)(void *user, int poly, const jbi_split_vertex *v, int n,
                             unsigned passes, const jbi_split_area *area);
/* A line piece from a to b. */
typedef void (*jbi_split_line_fn)(void *user, const jbi_split_vertex *a,
                                  const jbi_split_vertex *b);

/*
 * The count polygons v[k] (n[k] vertices each, 3..JBI_SPLIT_MAX_IN, each
 * convex, going round it in either direction: cyclic order, not the GPU's
 * Z order, so a quad given as a b c d has the GPU's triangles a b d and
 * b d c when passed through) as one primitive (1..JBI_SPLIT_MAX_PRIM of
 * them): passed through if all meet the limits, else all clipped
 * together (above). Returns the number of pieces given (0 when culled or
 * degenerate), or -1 for count or an n out of range (nothing given).
 * *dropped (when not NULL): the polygons left out in a tile and format
 * (INVARIANTS, (N)).
 */
int jbi_split_polys(const jbi_split_view *view, const jbi_split_vertex *const *v, const int *n,
                    int count, jbi_split_fn fn, void *user, int *dropped);

/* One polygon: jbi_split_polys with count 1. */
int jbi_split_poly(const jbi_split_view *view, const jbi_split_vertex *v, int n,
                   jbi_split_fn fn, void *user);

/*
 * A line from a to b: given unchanged when it meets the limits; else
 * clipped to the view and JBI_SPLIT_MARGIN pixels around it, or if that
 * is still too long for the GPU to the view alone, which a view of at most
 * 1024 x 512 always makes short enough, so a line is always one segment.
 * Returns the segments given to fn (0 or 1).
 */
int jbi_split_line(const jbi_split_view *view, const jbi_split_vertex *a,
                   const jbi_split_vertex *b, jbi_split_line_fn fn, void *user);

/* Whether the triangle a b c meets the GPU's limits by its integer
   positions (in range, and within 1023 x 511 of each other). */
int jbi_split_triangle_fits(const jbi_split_vertex *a, const jbi_split_vertex *b,
                            const jbi_split_vertex *c);

#endif

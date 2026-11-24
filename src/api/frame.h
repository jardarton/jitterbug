/*
 * The frame's internal interface (frame.c), for the friendly level's
 * other parts (the 3D geometry, sprites, text) to record into the frame
 * that jb_begin_frame starts and jb_end_frame draws.
 *
 * Where a record goes ("where"):
 *   0 .. ot_size - 1      a scene bucket. The scene is drawn from bucket
 *                         ot_size - 1 (the farthest) down to 0 (the
 *                         nearest), each bucket in the order its records
 *                         were added (first in, first out).
 *   JBI_FRAME_BACKGROUND  the 2D layer drawn before the scene, in order
 *   JBI_FRAME_OVERLAY     the 2D layer drawn after it, in order
 *   JBI_FRAME_LAYER       the 2D layer jb_set_layer chose
 * Inside it is one jb_ot of ot_size + 2 buckets: bucket ot_size + 1 the
 * background, 1 .. ot_size the scene (scene bucket b in bucket b + 1),
 * 0 the overlay, drawn from the highest.
 *
 * Coordinates are those of the back buffer: (0, 0) is its top-left pixel,
 * and the drawing offset at jb_end_frame is the buffer's corner, so a
 * record's positions are pixels of the frame (jbi_frame_size).
 *
 * Errors: every function here that fails latches the failure (jbi_api_fail
 * with fn as the name in its detail), and the caller returns it:
 * JB_ERROR_INVALID_ARGUMENT outside a frame or for where out of range,
 * JB_ERROR_OUT_OF_MEMORY when the frame holds jb_config.max_primitives
 * records already (the record is dropped; the detail names the field).
 */
#ifndef JBI_API_FRAME_H
#define JBI_API_FRAME_H

#include "api/record.h"
#include "geom/split.h"

enum { JBI_FRAME_BACKGROUND = -1, JBI_FRAME_OVERLAY = -2, JBI_FRAME_LAYER = -3 };

/* Between jb_begin_frame and jb_end_frame. */
int jbi_frame_active(const jb_context *jb);

/* The frame's size in 1x pixels: the display size frames are drawn at (the
   16:9 width while widescreen is on). The camera's aspect is w / h. */
void jbi_frame_size(const jb_context *jb, int *w, int *h);

/* Scene buckets (jb_config.ot_size, or its default). */
int jbi_frame_ot_size(const jb_context *jb);

/* JB_GEOMETRY_GTE or JB_GEOMETRY_FLOAT (jb_set_geometry). */
int jbi_api_geometry(const jb_context *jb);

/* A new record where, zeroed (kind 0) for the caller to fill in, or NULL
   after a failure (above). A record left with kind 0 draws nothing. */
jbi_record *jbi_frame_record(jb_context *jb, const char *fn, int where);

/* 1 when the frame can take another record, else 0 with the failure
   recorded (no frame begun, or the frame is full). Reserves nothing. */
int jbi_frame_has_room(jb_context *jb, const char *fn);

/*
 * The view frame pieces are clipped to (split.h): the frame's pixels.
 * textured is not used (the splitter keeps the affine and the perspective-
 * correct attributes of new vertices both, and the records carry both, so
 * that the option as it is at jb_end_frame decides, and the 1x pass above
 * 1x gets the affine ones); kept so that callers still compile.
 */
jbi_split_view jbi_frame_view(const jb_context *jb, int textured);

/*
 * A convex polygon of n vertices in cyclic order (3..JBI_SPLIT_MAX_IN), in
 * the frame's coordinates, passed through, or clipped to the frame and its
 * tiles and culled outside it (jbi_split_poly), each piece recorded where
 * as a polygon made from tmpl: its kind is JBI_REC_POLY, and u.poly's
 * gouraud, blend and raw, textured with tex, and window with tw are kept;
 * vertices and their colours, texel coordinates, sub-pixel positions and
 * depths come from the pieces (sub set, with the perspective-correct
 * attributes as sp.pattr), and so do the passes and, for a piece that
 * needs it, the drawing area (record.h, area). Flat (gouraud 0): every
 * piece takes v[0]'s colour. Returns JB_OK (also when everything was
 * culled), or the failure of the first piece that could not be recorded;
 * pieces after a full frame are dropped too.
 */
jb_result jbi_frame_add_poly(jb_context *jb, const char *fn, int where, const jbi_record *tmpl,
                             const jbi_split_vertex *v, int n);

/*
 * jbi_frame_add_poly for vertices whose perspective-correct attributes
 * (jbi_split_vertex pu, pv, pr, pg, pb) the caller has set: for polygons
 * the caller has cut in screen space itself (the 3D pipeline's guard band),
 * whose new vertices' perspective-correct attributes differ from their
 * affine ones. The splitter carries them on through its own cuts
 * (jbi_split_view.perspective_given), and the records' perspective-correct
 * pass uses them. At a vertex the caller did not cut they should equal the
 * affine ones.
 */
jb_result jbi_frame_add_poly_persp(jb_context *jb, const char *fn, int where,
                                   const jbi_record *tmpl, const jbi_split_vertex *v, int n);

/*
 * The triangles a and b (3 vertices each) as one primitive, as a GPU quad
 * is its two triangles v0 v1 v2 and v1 v2 v3: passed through if both meet
 * the limits, else clipped together (split.h, jbi_split_polys), so that
 * no pixel is drawn twice across the diagonal they share, in either pass,
 * unless they overlap themselves (a crossed or concave quad, which the
 * console draws twice there too). given: the vertices' perspective-correct
 * attributes are the caller's (as jbi_frame_add_poly_persp). For a quad
 * that does not meet the GPU's limits as one.
 */
jb_result jbi_frame_add_triangles(jb_context *jb, const char *fn, int where,
                                  const jbi_record *tmpl, const jbi_split_vertex *a,
                                  const jbi_split_vertex *b, int given);

/*
 * The count polygons v[k] (n[k] vertices, 1..JBI_SPLIT_MAX_PRIM of them)
 * as one primitive the same way, polygon k recorded at where[k] from
 * tmpl[k], with the caller's perspective-correct attributes: the pieces
 * of one 3D face that the pipeline cut into two (src/geom/pipeline.h).
 */
jb_result jbi_frame_add_polys_persp(jb_context *jb, const char *fn, const int *where,
                                    const jbi_record *const *tmpl,
                                    const jbi_split_vertex *const *v, const int *n, int count);

/* A line from a to b the same way (jbi_split_line), from tmpl's u.line
   gouraud and blend; flat takes a's colour. */
jb_result jbi_frame_add_line(jb_context *jb, const char *fn, int where, const jbi_record *tmpl,
                             const jbi_split_vertex *a, const jbi_split_vertex *b);

/* A rectangle in the frame's coordinates, clipped to the frame exactly
   (texel for texel, the flips kept) and cut into rectangles the GPU takes,
   recorded where; tmpl as for polygons, with u.rect. */
jb_result jbi_frame_add_rect(jb_context *jb, const char *fn, int where, const jbi_record *tmpl);

#endif

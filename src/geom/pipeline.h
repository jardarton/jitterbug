/*
 * The friendly 3D pipeline, without drawing: from a mesh, a model matrix
 * and the scene (scene.h) to screen-space polygons with everything a
 * primitive record needs (include/jitterbug.h, jb_set_geometry). Pure
 * functions: the only state they touch is the GTE they are given (the
 * scene's private one) and their outputs.
 *
 * How a mesh draw uses it (jb_draw_mesh, src/api/draw3d.c):
 *
 *   1. jbi_geom_check_mesh(mesh, &extent, ...): refuse a bad mesh, and
 *      learn its largest coordinate.
 *   2. jbi_geom_begin(&d, scene, model, w, h, ot_size, extent, ...): the
 *      per-draw set-up: model x view in float, and for the GTE path its
 *      quantisation into the GTE's registers (refused, with a detail, when
 *      out of the GTE's range). w x h is the back buffer's size; ot_size
 *      the scene's ordering-table buckets.
 *   3. jbi_geom_load(&scene->gte, &d): those registers into the private
 *      GTE (GTE path; nothing for the float path).
 *   4. For each face: jbi_geom_face(&scene->gte, &d, mesh, i, polys) gives
 *      0 (culled: a back face, or nothing left after clipping), 1 (the
 *      face itself, or a clipped triangle) or 2 (a clipped quad, as its two
 *      GPU triangles): convex polygons of 3 to JBI_GEOM_POLY_MAX vertices
 *      in the face's cyclic order, each with its ordering-table bucket. The polygon's
 *      positions are relative to the buffer's top-left and lie within
 *      -1024..1023, but its extent may be more than the GPU draws (1023 x
 *      511) and it may lie partly or wholly off the display.
 *   5. (jbi_geom_poly_prims(&poly, prims) gives the polygon as the
 *      console's triangles and quads, a quad's vertices in the console's
 *      Z order as jb_poly takes them, for a caller that wants primitives;
 *      the frame's splitter takes the polygon itself.)
 *   6. The polygon then goes through the frame's screen-space splitter
 *      (jbi_frame_add_poly, src/api/frame.h; draw3d.c passes the polygon
 *      itself, or an exact quad's two triangles when it is too big to pass
 *      whole), which cuts it to the GPU's extent limits and
 *      to the display (interpolating colours, u, v, sub-pixel positions
 *      and depths linearly in screen space) and culls what is off screen,
 *      and becomes a record in the scene's ordering table at the polygon's
 *      bucket, with the material's texture (u, v here are within the
 *      texture's own rectangle; the record adds the texture's offset),
 *      blend and raw flag.
 *
 * Billboards and 3D lines use jbi_geom_point and jbi_geom_segment after
 * jbi_geom_begin (with the model matrix of their space, NULL for world).
 *
 * Units. GTE units are the camera's view space scaled by
 * jbi_geom_draw.scale (a power of two, the largest putting z_far at 32767
 * or less), x right, y down, z forward: the space the GTE's IR1-3 and SZ
 * are in, for both paths, so depths in records mean the same either way.
 */
#ifndef JBI_GEOM_PIPELINE_H
#define JBI_GEOM_PIPELINE_H

#include <stddef.h>
#include <stdint.h>

#include <jitterbug.h>

#include "geom/gte.h"
#include "geom/scene.h"

/* The most vertices a clipped face has: a quad cut by six planes. */
enum { JBI_GEOM_POLY_MAX = 10 };
/* The most primitives jbi_geom_poly_prims makes of one polygon. */
enum { JBI_GEOM_PRIMS_MAX = 4 };
/* Sub-pixel positions are in 1/JBI_GEOM_SUBPIXEL pixels. */
enum { JBI_GEOM_SUBPIXEL = 256 };
/* The screen positions the pipeline keeps to: the GTE's SX, SY range, the
   whole of the largest frame (1024 wide). A clipped vertex whose truncated
   projection lands a pixel or two outside is saturated to it, as the GTE
   saturates SX and SY. */
enum { JBI_GEOM_GUARD_MIN = -1024, JBI_GEOM_GUARD_MAX = 1023 };

/* The GTE's registers for one draw, as jbi_geom_load writes them. */
typedef struct jbi_gte_regs {
    jb_gte_matrix rt;  /* model to GTE units, x 2^-shift */
    int32_t tr[3];     /* the model's origin in GTE units */
    int32_t h;         /* 1..65535 */
    int32_t ofx, ofy;  /* 16.16: the display's centre */
    int16_t zsf3, zsf4;
    jb_gte_matrix llm; /* towards each light, in model space (rows) */
    jb_gte_matrix lcm; /* light colours (columns) */
    int32_t bk[3];     /* ambient */
    int32_t fc[3];     /* fog colour, channel x 16 */
    int16_t dqa;
    int32_t dqb;
} jbi_gte_regs;

/* The set-up for one draw (jbi_geom_begin). */
typedef struct jbi_geom_draw {
    int mode; /* JB_GEOMETRY_* */
    int width, height, ot_size;
    float scale; /* GTE units a view-space unit */
    /* GTE path */
    jbi_gte_regs r;
    int shift;          /* vertices are multiplied by 2^shift before the GTE */
    int32_t near_z;     /* ceil(near_f), at most far_z: where ordering starts */
    int32_t far_z;      /* the farthest: floor(far_f) */
    int32_t otz_near, otz_far; /* the OTZ of near_z and far_z, which both
                                  paths' ordering maps onto the buckets */
    double near_f, far_f; /* the GTE path's clip planes, GTE units, not
                             rounded: max(z_near x scale, H / 2 + 1) and
                             z_far x scale */
    int fog;
    /* Per draw, set by the caller after jbi_geom_begin (0 by default):
       bias, GTE units added to the depth a polygon is sorted by (not to
       its geometry); tint, JB_RGB multiplying the vertices' colours / 128
       before lighting (0: none; JB_TINT_BLACK, its channels 0: black). */
    double bias;
    uint32_t tint;
    /* Float path, and both paths' float transforms. g: model to GTE
       units, 3 rows of 4 (the last column the translation). */
    float g[3][4];
    double fh;            /* projection distance, pixels (H unrounded) */
    float fofx, fofy;     /* the centre, pixels */
    float fnear, ffar;    /* GTE units: z_near and z_far scaled */
    float lto[JBI_LIGHTS][3];  /* towards each light, model space (0 off) */
    float lcol[JBI_LIGHTS][3]; /* light levels per channel, 1.0 = 255 */
    float amb[3];
    double fog_near, fog_far; /* GTE units */
    float fog_color[3];
} jbi_geom_draw;

/* A screen vertex, ready for a record. */
typedef struct jbi_geom_vertex {
    int x, y;       /* whole pixels from the buffer's top-left (SX, SY) */
    int32_t sx, sy; /* the same in 1/JBI_GEOM_SUBPIXEL pixels, unrounded
                       to whole pixels (within a pixel of x, y) */
    int32_t z;      /* view depth, GTE units (SZ), at least 1 */
    double zf;      /* the same unrounded (float path; GTE path SZ too) */
    uint32_t color; /* JB_RGB */
    int u, v;       /* texel coordinates within the material's texture */
    /* The perspective-correct attributes: the same as color, u, v at a
       face's own vertices and where a near or far plane cut it; where the
       guard band cut it, color, u, v are screen-linear (affine, as the
       console and the frame's splitter would have them) and these
       view-linear (as perspective-correct texturing needs them). */
    uint32_t pcolor;
    int pu, pv;
} jbi_geom_vertex;

/* A face after the pipeline: a convex polygon, the face's cyclic order. */
typedef struct jbi_geom_poly {
    int n;      /* 3..JBI_GEOM_POLY_MAX */
    int bucket; /* 0..ot_size-1, larger farther */
    /* 1 when the face needed no clipping (GTE path: every vertex inside
       the near and far planes and nothing saturated): its vertices are
       then exactly the GTE's results. */
    int exact;
    /* The outputs that draw it (JBI_GEOM_PASS_*), 0 for both: a face that
       faces the eye at whole pixels but not at its sub-pixel positions,
       or the other way round, is drawn only by the output that sees its
       front (the 1x pass, and the scaled one without precise vertices, by
       the whole-pixel positions; the scaled one with precise vertices by
       the sub-pixel ones). */
    int passes;
    const jb_material *material;
    jbi_geom_vertex v[JBI_GEOM_POLY_MAX];
} jbi_geom_poly;

/* The outputs of jbi_geom_poly.passes, the same values as
   JBI_SPLIT_PASS_1X and JBI_SPLIT_PASS_PRECISE (src/geom/split.h). */
enum { JBI_GEOM_PASS_1X = 1, JBI_GEOM_PASS_PRECISE = 2 };

/* A console polygon: a triangle, or a quad in the console's Z order. */
typedef struct jbi_geom_prim {
    int vertices; /* 3 or 4 */
    jbi_geom_vertex v[4];
} jbi_geom_prim;

/*
 * Checks a mesh for drawing (fn names the public call in the detail):
 * mesh and positions not NULL, counts in range (vertex_count 1..65536,
 * face_count 0 or more, material_count 1..65536 when there are faces),
 * faces of 3 or 4
 * vertices with indices and materials in range, faces and materials not
 * NULL where there are faces, colours (materials' and vertices') at most
 * 0xFFFFFF, blend modes JB_BLEND_*. *extent (extent not NULL) gets the
 * largest absolute position coordinate. JB_OK, or JB_ERROR_INVALID_ARGUMENT
 * with the detail.
 */
jb_result jbi_geom_check_mesh(const jb_mesh *mesh, const char *fn, int *extent, char *detail,
                              size_t n);

/*
 * The per-draw set-up for scene's camera, lights, fog and path, model
 * (NULL: the identity), a w x h buffer (1..1024 x 1..512) and ot_size
 * buckets (1..65536), for a mesh whose largest coordinate is extent (0
 * for none), or with extent JBI_GEOM_NO_MESH for points and segments
 * only, which transform in double and never use the rotation and
 * translation: then those are not set up (left 0) and none of their
 * limits refuses the draw. In the GTE path:
 *   - model x view, scaled by d->scale and turned to the GTE's axes (y
 *     down, z forward), becomes the rotation in 4.12 with the scale
 *     folded in, divided by 2^shift for the smallest shift (0..15) that
 *     brings every element within -32768..32767 (so -8..8); the mesh's
 *     vertices are multiplied by 2^shift, which extent must allow
 *     (extent x 2^shift <= 32767). The translation rounds to an int32 and
 *     must lie within +-(2^31 - 2^20), where the GTE's 44-bit sums cannot
 *     overflow.
 *   - H = round(h / 2 / tan(fov_y / 2)), 1..65535. OFX, OFY the buffer's
 *     centre (w / 2, h / 2) in 16.16.
 *   - near_f = max(z_near x scale, H / 2 + 1) (the GTE's divide limit)
 *     and far_f = z_far x scale, the clip planes, unrounded, near_f <
 *     far_f (GTE path; a range within one integer depth is valid); near_z
 *     and far_z their ceiling and floor (near_z at most far_z), the depths
 *     both paths' ordering maps onto the buckets.
 *   - ZSF3 = 4 k and ZSF4 = 3 k, so AVSZ3 and AVSZ4 both make OTZ =
 *     12 k x average SZ / 4096 (one scale for triangles and quads), k the
 *     largest (at most 8191) keeping far_z's OTZ within 16 bits; otz_near
 *     and otz_far the OTZ at near_z and far_z, between which OTZ is mapped
 *     linearly onto the buckets.
 *   - The light matrix: each light's direction turned into model space
 *     by the transpose of the model's rotation (its 3 x 3 part with each
 *     column normalised), x 4096; the light colour matrix and background
 *     colour (c x 4096 + 127) / 255 a channel, as jb_gte_set_lights.
 *   - Fog: DQA = round(-256 zn zf / ((zf - zn) H)) (16 bits, with the
 *     integer H the GTE is given) and DQB = round(2^24 zf / (zf - zn)) (32
 *     bits) for its start and end in GTE units (computed in double), so
 *     that IR0 = 4096 zf (z - zn) / (z (zf - zn)); FC the colour x 16.
 * The float path computes none of the GTE's registers (and so refuses
 * none of their limits). Every conversion to an integer here and in the
 * faces saturates first, so no input makes one undefined.
 * Errors (GTE path only; the float path needs none of these): any of
 * those out of range, and a model matrix that is not finite, as
 * JB_ERROR_INVALID_ARGUMENT with the detail (prefixed by fn).
 */
enum { JBI_GEOM_NO_MESH = -1 };

jb_result jbi_geom_begin(jbi_geom_draw *d, const jbi_scene *scene, const jb_mat4 *model, int w,
                         int h, int ot_size, int extent, const char *fn, char *detail, size_t n);

/* Writes d's registers into g (GTE path): rotation, translation, H, OFX,
   OFY, ZSF3, ZSF4, light, light colour, background and far colours, DQA,
   DQB. Nothing for the float path. */
void jbi_geom_load(jbi_gte *g, const jbi_geom_draw *d);

/*
 * Face face of mesh (checked by jbi_geom_check_mesh with d's extent)
 * through d's path, into *out; 1 for a polygon, 0 when it is culled.
 *
 * GTE path, on g with d loaded. The face's vertices in the console's Z
 * order (cyclic 0 1 3 2 for a quad), multiplied by 2^shift, are
 * projected: with fog each by RTPS (for its own IR0), else three by RTPT
 * and a quad's fourth by RTPS; NCLIP runs on the first three, then AVSZ3
 * or AVSZ4. If no vertex saturated anything (FLAG's MAC, IR1, IR2, SZ,
 * divide and SX/SY bits) and every vertex is inside the six clipping
 * planes below (tested on the exact sums: depth within near_f..far_f, the
 * exact screen position within JBI_GEOM_GUARD_MIN..MAX), the face is
 * exact: culled when MAC0 >= 0 (front faces go anticlockwise, which on the
 * y-down screen is MAC0 < 0; for a quad whose MAC0 is 0, NCLIP of its
 * second triangle, cyclic 1 2 3) unless double-sided; bucket from OTZ; positions SXY; colours per vertex by NCCS (lit, no
 * fog), NCDS (lit, fog; IR0 that vertex's), DPCS (unlit, fog) or the
 * colour itself (unlit), from the vertex's normal or the face's flat one.
 * Otherwise the face is clipped in GTE units (from the GTE's exact 44-bit
 * sums) as its GPU triangles (a quad's two, cyclic 0 1 3 and 1 2 3, so the
 * GPU's diagonal stays), each culled by the sign of its triple product
 * (the sign NCLIP gives, and right for vertices behind the camera too),
 * against the near and far planes and the planes beyond which a vertex
 * would leave JBI_GEOM_GUARD_MIN..MAX on the screen. At a cut, colours
 * (before lighting), normals and u, v are interpolated twice: in view
 * space (the perspective-correct set, pcolor, pu, pv) and, at the guard
 * band's planes, which stand for cutting in screen space, by the fraction
 * along the screen (the affine set, color, u, v; in view space at the near
 * and far planes too); the point's coordinate across a guard plane is put
 * on the plane exactly; a vertex on a plane is kept once. Then every vertex left is projected by the GTE's
 * own divide (jbi_gte_divide) with the truncations and saturations of
 * RTPS (IR1 and IR2 to 16 bits; SX and SY to -1024..1023: the truncated
 * depth can carry a vertex a few pixels past the guard band near the
 * camera) and lit on the GTE as an exact face's are (NCCS, NCDS, DPCS)
 * with its own IR0 = (DQA q + DQB) >> 12; bucket from the average SZ on
 * the same OTZ scale as AVSZ.
 *
 * Float path: the same steps in floating point (g untouched; the view
 * transform and lighting in float, clipping and projection in double; a
 * face with a coordinate that is not finite is culled),
 * with the triple product's sign for culling, lighting that saturates as
 * the GTE's does and is moved by fog before it saturates to 255 (as NCDS
 * does), fog (1/start - 1/z) / (1/start - 1/end), and positions rounded to
 * 1/256 pixel (whole pixels rounded down from those). exact is 1 when no
 * plane cut the face.
 *
 * Sub-pixel positions: the GTE path's are the projection of its exact
 * sums (OF + x H / z), or the GTE's own 16.16 fraction where those stray
 * more than a pixel from it, computed exactly as the GTE's record for
 * precise vertices computes them (src/geom/gte.c), so the same vertices
 * projected by console code with precise_vertices on get the same ones.
 * Clipping computes a point on an edge from the edge's ends in a fixed
 * order, so faces that share an edge share the points cut on it.
 */
int jbi_geom_face(jbi_gte *g, const jbi_geom_draw *d, const jb_mesh *mesh, int face,
                  jbi_geom_poly out[2]);

/*
 * A polygon as console primitives into out (JBI_GEOM_PRIMS_MAX): 3
 * vertices are a triangle; 4 a quad (cyclic 0 1 2 3 as Z order 0 1 3 2);
 * more a fan of quads from vertex 0 with a triangle last for an odd count.
 * Returns how many.
 */
int jbi_geom_poly_prims(const jbi_geom_poly *p, jbi_geom_prim *out);

/*
 * A point p of the draw's model space (float), for billboards: 1 and *out
 * (position, depth; color 0, u v 0) when it lies within the near and far
 * planes and the guard band, else 0. The GTE path projects it with the
 * GTE's divide and truncations from its float view position.
 */
int jbi_geom_point(const jbi_geom_draw *d, jb_vec3 p, jbi_geom_vertex *out);

/*
 * A segment from a to b (model space, float) with colours ca and cb
 * (JB_RGB), for 3D lines: clipped to the near and far planes and the
 * guard band, projected as jbi_geom_point, fogged (GTE path: by DPCS on
 * g), into out[0..1] with *bucket from its average depth. 1, or 0 when
 * nothing is left.
 */
int jbi_geom_segment(jbi_gte *g, const jbi_geom_draw *d, jb_vec3 a, jb_vec3 b, uint32_t ca,
                     uint32_t cb, jbi_geom_vertex out[2], int *bucket);

/*
 * For billboards: the bucket of something at v's depth (as jbi_geom_point
 * gives it: SZ in the GTE path, the unrounded depth in the float path), as
 * a face whose vertices are all there gets it, with d->bias; and color
 * (JB_RGB) fogged at that depth as a vertex there is (GTE path: DPCS on g
 * with that depth's IR0), or color itself without fog.
 */
int jbi_geom_bucket(const jbi_geom_draw *d, const jbi_geom_vertex *v);
uint32_t jbi_geom_fog(jbi_gte *g, const jbi_geom_draw *d, uint32_t color,
                      const jbi_geom_vertex *v);

#endif

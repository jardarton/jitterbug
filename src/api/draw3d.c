/*
 * The friendly 3D draws (include/jitterbug.h, 3D drawing): jb_draw_mesh,
 * jb_draw_mesh_ex, jb_draw_billboard and jb_draw_line3d. Each runs the
 * geometry pipeline (src/geom/pipeline.h) on the scene's private GTE and
 * records what comes out into the frame's scene buckets (src/api/frame.h),
 * through the frame's screen-space splitter (src/geom/split.h), with the
 * vertices' sub-pixel positions and depths in the records for precise
 * vertices and perspective-correct texturing, and with both the affine and
 * the perspective-correct attributes where the pipeline's guard band cut
 * a face.
 */
#include <string.h>

#include "api/frame.h"
#include "geom/pipeline.h"

/* The splitter takes a face's polygon whole. */
typedef char jbi_poly_max_fits[(int)JBI_GEOM_POLY_MAX <= (int)JBI_SPLIT_MAX_IN ? 1 : -1];
/* A face's outputs are the splitter's passes. */
typedef char jbi_passes_match[(int)JBI_GEOM_PASS_1X == (int)JBI_SPLIT_PASS_1X &&
                                      (int)JBI_GEOM_PASS_PRECISE == (int)JBI_SPLIT_PASS_PRECISE
                                  ? 1
                                  : -1];

static jb_result not_in_frame(jb_context *jb, const char *fn)
{
    return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                        "%s: no frame begun (call jb_begin_frame first)", fn);
}

static int finite(float x)
{
    return x - x == 0.0f;
}

static int finite3(jb_vec3 v)
{
    return finite(v.x) && finite(v.y) && finite(v.z);
}

static int power_of_two(int x)
{
    return x >= 8 && x <= 256 && (x & (x - 1)) == 0;
}

/*
 * A texture's fields: those a textured polygon has, checked as jb_prim_poly
 * checks them, and the friendly ones the 3D draws compute texel coordinates
 * from (jb_texture: u, v 0..255; width, height 0..256, 0 for a hand-made
 * texture, which means "to the page's edge"; the rectangle within the page;
 * a repeat texture laid out as jb_texture_create lays one out), before any
 * arithmetic is done with them.
 */
static jb_result check_texture(jb_context *jb, const char *fn, const jb_texture *t)
{
    jb_poly p;
    jb_result r;

    memset(&p, 0, sizeof p);
    p.vertices = 3;
    p.texture = t;
    p.raw = 1;
    r = jbi_check_poly(jb, fn, &p, 0);
    if (r != JB_OK)
        return r;
    if (t->u < 0 || t->u > 255 || t->v < 0 || t->v > 255 || t->width < 0 || t->width > 256 ||
        t->height < 0 || t->height > 256 || t->u + t->width > 256 || t->v + t->height > 256)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: the texture's rectangle (%d, %d) %d x %d is not within its page "
                            "(u, v 0..255, width and height 0..256)",
                            fn, t->u, t->v, t->width, t->height);
    if (t->repeat && (!power_of_two(t->width) || !power_of_two(t->height) ||
                      t->u % t->width != 0 || t->v % t->height != 0))
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: a repeat texture must be a power of two 8..256 each way, at a "
                            "multiple of its size in the page; this one is (%d, %d) %d x %d",
                            fn, t->u, t->v, t->width, t->height);
    return JB_OK;
}

/* The per-draw set-up, failing with its detail; bias in world units. */
static jb_result begin(jb_context *jb, const char *fn, jbi_geom_draw *d, const jb_mat4 *model,
                       int extent, float bias, uint32_t tint)
{
    char detail[200];
    int w, h;
    jb_result r;

    jbi_frame_size(jb, &w, &h);
    r = jbi_geom_begin(d, &jb->api->scene, model, w, h, jbi_frame_ot_size(jb), extent, fn,
                       detail, sizeof detail);
    if (r != JB_OK)
        return jbi_api_fail(jb, r, "%s", detail);
    d->bias = (double)bias * (double)d->scale;
    d->tint = tint;
    jbi_geom_load(&jb->api->scene.gte, d);
    return JB_OK;
}

/* A texel coordinate in the page: within the texture's rectangle from its
   corner, or as it is for a repeat texture, which the texture window wraps
   (both checked by check_texture, so no overflow). */
static int page_coord(const jb_texture *t, int c, int origin)
{
    int p = t == NULL || t->repeat ? c : origin + c;

    return p > 255 ? 255 : p;
}

/*
 * A pipeline vertex as the splitter takes it, with both attribute sets:
 * the affine (u, v, r, g, b) and the perspective-correct (pu, pv, pr, pg,
 * pb), which differ where the pipeline's guard band cut the face (there
 * the affine set is screen-linear and the perspective one view-linear).
 */
static void split_vertex(const jbi_geom_vertex *g, const jb_texture *t, jbi_split_vertex *o)
{
    memset(o, 0, sizeof *o);
    o->x = g->x;
    o->y = g->y;
    o->qx = g->sx;
    o->qy = g->sy;
    o->z = g->z;
    o->r = (int)(g->color & 0xff);
    o->g = (int)((g->color >> 8) & 0xff);
    o->b = (int)((g->color >> 16) & 0xff);
    o->u = page_coord(t, g->u, t ? t->u : 0);
    o->v = page_coord(t, g->v, t ? t->v : 0);
    o->pu = page_coord(t, g->pu, t ? t->u : 0);
    o->pv = page_coord(t, g->pv, t ? t->v : 0);
    o->pr = (int)(g->pcolor & 0xff);
    o->pg = (int)((g->pcolor >> 8) & 0xff);
    o->pb = (int)((g->pcolor >> 16) & 0xff);
}

/* A template polygon record for a material. */
static void poly_template(jbi_record *t, const jb_material *m, int gouraud)
{
    memset(t, 0, sizeof *t);
    t->kind = JBI_REC_POLY;
    t->u.poly.gouraud = gouraud;
    t->u.poly.blend = m->blend;
    t->u.poly.raw = m->texture != NULL && m->raw;
    if (m->texture != NULL) {
        t->textured = 1;
        t->tex = *m->texture;
        t->window = 1;
        t->tw = jbi_record_window(m->texture);
    }
}

/* A face's polygon from the pipeline as the splitter's vertices, with its
   record template. */
static void face_poly(const jbi_geom_poly *p, jbi_split_vertex *v, jbi_record *tmpl)
{
    const jb_material *m = p->material;
    int i, gouraud = 0;

    for (i = 0; i < p->n; i++) {
        split_vertex(&p->v[i], m->texture, &v[i]);
        gouraud |= p->v[i].color != p->v[0].color || p->v[i].pcolor != p->v[0].color;
    }
    poly_template(tmpl, m, gouraud);
    tmpl->passes = (uint8_t)p->passes; /* the outputs that draw it, 0 for both (frame.c) */
}

/* A face from the pipeline (count polygons) into the scene, as one
   primitive: an exact quad as the console's two triangles, the two
   polygons of a face the pipeline cut as its two GPU triangles together
   (src/api/frame.h), so that no pixel is drawn twice across their
   diagonal, whatever their buckets. */
static jb_result add_face(jb_context *jb, const char *fn, const jbi_geom_poly *p, int count)
{
    jbi_split_vertex v[2][JBI_GEOM_POLY_MAX];
    jbi_record tmpl[2];
    const jbi_split_vertex *pv[2];
    const jbi_record *pt[2];
    int k, n[2], where[2];

    for (k = 0; k < count; k++) {
        face_poly(&p[k], v[k], &tmpl[k]);
        pv[k] = v[k];
        pt[k] = &tmpl[k];
        n[k] = p[k].n;
        where[k] = p[k].bucket;
    }
    if (count == 1 && p->exact && p->n == 4) {
        /* The console's quad: whole when its two triangles (cyclic 0 1 3
           and 1 3 2) meet the GPU's limits, else the two triangles. */
        jbi_split_vertex a[3], b[3];

        if (jbi_split_triangle_fits(&v[0][0], &v[0][1], &v[0][3]) &&
            jbi_split_triangle_fits(&v[0][1], &v[0][3], &v[0][2]))
            return jbi_frame_add_poly_persp(jb, fn, p->bucket, &tmpl[0], v[0], 4);
        a[0] = v[0][0], a[1] = v[0][1], a[2] = v[0][3];
        b[0] = v[0][1], b[1] = v[0][2], b[2] = v[0][3];
        return jbi_frame_add_triangles(jb, fn, p->bucket, &tmpl[0], a, b, 1);
    }
    return jbi_frame_add_polys_persp(jb, fn, where, pt, pv, n, count);
}

static jb_result draw_mesh(jb_context *jb, const char *fn, const jb_mesh_draw *draw)
{
    char detail[200];
    const jb_mesh *mesh;
    jbi_geom_draw d;
    jbi_geom_poly p[2];
    jb_result r;
    int extent, i, count;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (!jbi_frame_active(jb))
        return not_in_frame(jb, fn);
    if (draw == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: draw is NULL", fn);
    if (!finite(draw->depth_bias))
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: depth_bias is not finite", fn);
    if (draw->tint > 0xffffffu && draw->tint != JB_TINT_BLACK)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: tint 0x%lx is above 0xFFFFFF",
                            fn, (unsigned long)draw->tint);
    mesh = draw->mesh;
    r = jbi_geom_check_mesh(mesh, fn, &extent, detail, sizeof detail);
    if (r != JB_OK)
        return jbi_api_fail(jb, r, "%s", detail);
    for (i = 0; mesh->face_count > 0 && i < mesh->material_count; i++)
        if (mesh->materials[i].texture != NULL) {
            r = check_texture(jb, fn, mesh->materials[i].texture);
            if (r != JB_OK)
                return r;
        }
    if (mesh->face_count == 0)
        return JB_OK;
    r = begin(jb, fn, &d, draw->model, extent, draw->depth_bias, draw->tint);
    if (r != JB_OK)
        return r;
    for (i = 0; i < mesh->face_count; i++) {
        count = jbi_geom_face(&jb->api->scene.gte, &d, mesh, i, p);
        if (count > 0) {
            r = add_face(jb, fn, p, count);
            if (r != JB_OK)
                return r;
        }
    }
    return JB_OK;
}

jb_result jb_draw_mesh_ex(jb_context *jb, const jb_mesh_draw *draw)
{
    return draw_mesh(jb, "jb_draw_mesh_ex", draw);
}

jb_result jb_draw_mesh(jb_context *jb, const jb_mesh *mesh, const jb_mat4 *model)
{
    jb_mesh_draw draw;

    memset(&draw, 0, sizeof draw);
    draw.mesh = mesh;
    draw.model = model;
    return draw_mesh(jb, "jb_draw_mesh", &draw);
}

/* ------------------------------------------------------------------------ */
/* Billboards */

/* Nearest integer, halves up; saturated at +-2^62 and 0 for NaN, so the
   conversion is always defined. */
static int64_t round64(double x)
{
    const double lim = 4611686018427387904.0;
    int64_t i;

    x = !(x == x) ? 0.0 : x > lim ? lim : x < -lim ? -lim : x;
    i = (int64_t)(x + 0.5);
    return (double)i > x + 0.5 ? i - 1 : i;
}

/* A span of a billboard on one axis: pixels lo..hi, texel coordinates ta
   at lo and tb at hi (whole numbers). */
typedef struct span {
    double lo, hi, ta, tb;
} span;

/* The pixels a texel step across before a billboard's clipped span is cut
   along texel boundaries rather than moved out to a whole texel (below). */
enum { SPAN_STEP = 16, MAX_SPANS = 96 };

static double floor_dbl(double x)
{
    double i = (double)round64(x);

    return i > x ? i - 1.0 : i;
}

/*
 * The parts of a billboard's span on one axis to draw within cmin..cmax:
 * centred at c, hw either side, its texel coordinates ta at c - hw and tb
 * at c + hw (whole numbers). As spans whose texel coordinates are whole
 * numbers, so that the GPU, which interpolates whole-number coordinates
 * and rounds to the nearest texel, shows each texel where the whole
 * billboard would: clipping must not move a texel boundary (rounding a
 * cut's coordinate would). Uncut, the span itself. Cut, with a texel step
 * of SPAN_STEP pixels or less: moved out from the cut to the next whole
 * texel, at most a step beyond it. With a larger step (a magnified
 * texture): cut at the texel boundaries into spans of one texel each, its
 * coordinate constant, as many as there are texels in view. Returns the
 * spans (0 when none shows), at most MAX_SPANS.
 *
 * Every position is the centre plus an offset from it (the texel at
 * coordinate u is at c + (u - tm) step, tm the middle coordinate), and
 * every coordinate the middle one plus the cut's offset from the centre
 * over the step, never from an end: a billboard so wide that its ends,
 * c -+ hw, do not keep c (hw 1e20 pixels) still puts each boundary where
 * it is, a small offset from the centre, which is in view.
 */
/* The texel (ta..tb) a position x shows, x an offset off from the
   centre: the k with (k - 0.5 - tm) step <= off < (k + 0.5 - tm) step,
   decided by comparing offsets, which are exact enough, not by the
   coordinate tm + off / step, which a huge step rounds onto a boundary. */
static double texel_at(double off, double tm, double step, double ta, double tb)
{
    double k = floor_dbl(tm + off / step + 0.5);
    int i;

    for (i = 0; i < 4 && k > ta && (k - 0.5 - tm) * step > off; i++)
        k -= 1.0;
    for (i = 0; i < 4 && k < tb && (k + 0.5 - tm) * step <= off; i++)
        k += 1.0;
    return k < ta ? ta : k > tb ? tb : k;
}

static int spans(double c, double hw, double ta, double tb, double cmin, double cmax, span *out)
{
    double lo = c - hw, hi = c + hw, tm = (ta + tb) * 0.5, step, vl, vr, k, kf, kl;
    int n = 0;

    if (!(hi > cmin && lo < cmax))
        return 0;
    if ((lo >= cmin && hi <= cmax) || tb == ta) {
        out[0].lo = lo > cmin ? lo : cmin;
        out[0].hi = hi < cmax ? hi : cmax;
        out[0].ta = ta;
        out[0].tb = tb;
        return 1;
    }
    step = 2.0 * hw / (tb - ta);
#define POS(u) (c + ((u) - tm) * step)
#define TEX(x) (tm + ((x) - c) / step)
    if (step <= (double)SPAN_STEP) {
        out[0].lo = lo;
        out[0].ta = ta;
        out[0].hi = hi;
        out[0].tb = tb;
        if (lo < cmin) {
            k = floor_dbl(TEX(cmin));
            k = k < ta ? ta : k;
            out[0].ta = k;
            out[0].lo = POS(k);
        }
        if (hi > cmax) {
            k = -floor_dbl(-TEX(cmax));
            k = k > tb ? tb : k;
            out[0].tb = k;
            out[0].hi = POS(k);
        }
        return 1;
    }
    vl = lo > cmin ? lo : cmin;
    vr = hi < cmax ? hi : cmax;
    kf = texel_at(vl - c, tm, step, ta, tb);
    kl = texel_at(vr - c, tm, step, ta, tb);
    for (k = kf; k <= kl && n < MAX_SPANS; k += 1.0) {
        out[n].lo = k == kf ? vl : POS(k - 0.5);
        out[n].hi = k == kl ? vr : POS(k + 0.5);
        out[n].ta = out[n].tb = k;
        n++;
    }
#undef POS
#undef TEX
    return n;
}

jb_result jb_draw_billboard(jb_context *jb, const jb_billboard *b)
{
    static const char fn[] = "jb_draw_billboard";
    const jb_texture *t;
    jbi_geom_draw d;
    jbi_geom_vertex c;
    jbi_record tmpl;
    jb_result r;
    double k, depth, pw, ph, cx, cy, hei;
    int u0 = 0, v0 = 0, tw = 1, th = 1, bucket, fw, fh;
    uint32_t tint;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (b == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: billboard is NULL", fn);
    if (!jbi_frame_active(jb))
        return not_in_frame(jb, fn);
    t = b->texture;
    if (t != NULL) {
        r = check_texture(jb, fn, t);
        if (r != JB_OK)
            return r;
    }
    if (!finite3(b->position) || !finite(b->width) || !finite(b->height) ||
        !finite(b->depth_bias) || !(b->width > 0.0f) || b->height < 0.0f)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: position, size and depth_bias must be finite, width above 0 "
                            "and height 0 or more",
                            fn);
    if (b->tint > 0xffffffu && b->tint != JB_TINT_BLACK)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: tint 0x%lx is above 0xFFFFFF", fn,
                            (unsigned long)b->tint);
    if (b->blend < JB_BLEND_NONE || b->blend > JB_BLEND_QUARTER)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: blend %d is not a JB_BLEND_* mode",
                            fn, b->blend);
    /* The texels shown: (u, v), tw x th within the texture (0: to its
       edge). The origin is checked first, then the size against what is
       left, so nothing here can overflow. */
    if (t != NULL) {
        int tex_w = t->width > 0 ? t->width : 256 - t->u;
        int tex_h = t->height > 0 ? t->height : 256 - t->v;

        if (b->u < 0 || b->v < 0 || b->u >= tex_w || b->v >= tex_h || b->tw < 0 || b->th < 0 ||
            b->tw > tex_w - b->u || b->th > tex_h - b->v)
            return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                                "%s: texels (%d, %d) %d x %d are not within the texture's %d x %d",
                                fn, b->u, b->v, b->tw, b->th, tex_w, tex_h);
        u0 = b->u;
        v0 = b->v;
        tw = b->tw > 0 ? b->tw : tex_w - b->u;
        th = b->th > 0 ? b->th : tex_h - b->v;
    }
    r = begin(jb, fn, &d, NULL, JBI_GEOM_NO_MESH, b->depth_bias, 0);
    if (r != JB_OK)
        return r;
    if (!jbi_geom_point(&d, b->position, &c))
        return JB_OK; /* nearer than the near plane, beyond the far one, or far off screen */
    /* Pixels a world unit at its depth: the GTE path's SZ, the float
       path's unrounded depth (as its sorting and fog use). */
    depth = d.mode == JB_GEOMETRY_GTE ? (double)c.z : c.zf;
    k = (d.mode == JB_GEOMETRY_GTE ? (double)d.r.h : d.fh) * (double)d.scale / depth;
    hei = b->height > 0.0f ? (double)b->height : (double)b->width * (double)th / (double)tw;
    pw = (double)b->width * k;
    ph = hei * k;
    cx = (double)c.sx / 256.0;
    cy = (double)c.sy / 256.0;
    tint = b->tint == 0 ? JB_NEUTRAL : b->tint == JB_TINT_BLACK ? 0 : b->tint;
    tint = jbi_geom_fog(&jb->api->scene.gte, &d, tint, &c);
    bucket = jbi_geom_bucket(&d, &c);
    memset(&tmpl, 0, sizeof tmpl);
    if (t != NULL) {
        tmpl.textured = 1;
        tmpl.tex = *t;
        tmpl.window = 1;
        tmpl.tw = jbi_record_window(t);
    }
    if (t != NULL && pw < 1e6 && ph < 1e6 && round64(pw) == tw && round64(ph) == th) {
        /* One texel a pixel: a rectangle, as crisp as a sprite. */
        tmpl.kind = JBI_REC_RECT;
        tmpl.u.rect.x = (int)round64(cx - (double)tw * 0.5);
        tmpl.u.rect.y = (int)round64(cy - (double)th * 0.5);
        tmpl.u.rect.w = tw;
        tmpl.u.rect.h = th;
        tmpl.u.rect.color = tint;
        tmpl.u.rect.blend = b->blend;
        tmpl.u.rect.raw = b->raw != 0;
        tmpl.u.rect.u = page_coord(t, u0, t->u);
        tmpl.u.rect.v = page_coord(t, v0, t->v);
        return jbi_frame_add_rect(jb, fn, bucket, &tmpl);
    }
    jbi_frame_size(jb, &fw, &fh);
    {
        /* Quads over the texels' rectangle, the corners at u0 and
           u0 + tw - 1 (v likewise): the GPU rounds an interpolated texel
           coordinate to the nearest, so a corner one further would let the
           last row and column sample the texel beyond the crop. Clipped to
           the frame (with a margin) before it becomes fixed point, so a
           billboard of any size shows the part in view, with its texel
           boundaries where they would be uncut (spans()). */
        span sx[MAX_SPANS], sy[MAX_SPANS];
        int nx = spans(cx, pw * 0.5, (double)u0, (double)(u0 + tw - 1), -16.0, (double)fw + 16.0,
                       sx);
        int ny = spans(cy, ph * 0.5, (double)v0, (double)(v0 + th - 1), -16.0, (double)fh + 16.0,
                       sy);
        int i, ix, iy;

        tmpl.kind = JBI_REC_POLY;
        tmpl.u.poly.gouraud = 0;
        tmpl.u.poly.blend = b->blend;
        tmpl.u.poly.raw = t != NULL && b->raw;
        for (iy = 0; iy < ny; iy++)
            for (ix = 0; ix < nx; ix++) {
                jbi_split_vertex q[4];

                for (i = 0; i < 4; i++) {
                    jbi_split_vertex *o = &q[i];
                    int right = i == 1 || i == 2, bottom = i >= 2;

                    memset(o, 0, sizeof *o);
                    o->qx = round64((right ? sx[ix].hi : sx[ix].lo) * 256.0);
                    o->qy = round64((bottom ? sy[iy].hi : sy[iy].lo) * 256.0);
                    o->x = (int)((o->qx + 128) >> 8);
                    o->y = (int)((o->qy + 128) >> 8);
                    o->z = c.z;
                    o->r = o->pr = (int)(tint & 0xff);
                    o->g = o->pg = (int)((tint >> 8) & 0xff);
                    o->b = o->pb = (int)((tint >> 16) & 0xff);
                    if (t != NULL) {
                        o->u = o->pu =
                            page_coord(t, (int)round64(right ? sx[ix].tb : sx[ix].ta), t->u);
                        o->v = o->pv =
                            page_coord(t, (int)round64(bottom ? sy[iy].tb : sy[iy].ta), t->v);
                    }
                }
                r = jbi_frame_add_poly(jb, fn, bucket, &tmpl, q, 4);
                if (r != JB_OK)
                    return r;
            }
        return JB_OK;
    }
}

/* ------------------------------------------------------------------------ */
/* 3D lines */

jb_result jb_draw_line3d(jb_context *jb, const jb_line3d *line)
{
    static const char fn[] = "jb_draw_line3d";
    jbi_geom_draw d;
    jbi_geom_vertex e[2];
    jbi_split_vertex a, b;
    jbi_record tmpl;
    jb_result r;
    int bucket;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (line == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: line is NULL", fn);
    if (!jbi_frame_active(jb))
        return not_in_frame(jb, fn);
    if (!finite3(line->a) || !finite3(line->b) || !finite(line->depth_bias))
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: a position or depth_bias is not finite", fn);
    if (line->color_a > 0xffffffu || line->color_b > 0xffffffu)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: color_a 0x%lx or color_b 0x%lx is above 0xFFFFFF", fn,
                            (unsigned long)line->color_a, (unsigned long)line->color_b);
    if (line->blend < JB_BLEND_NONE || line->blend > JB_BLEND_QUARTER)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: blend %d is not a JB_BLEND_* mode",
                            fn, line->blend);
    r = begin(jb, fn, &d, NULL, JBI_GEOM_NO_MESH, line->depth_bias, 0);
    if (r != JB_OK)
        return r;
    if (!jbi_geom_segment(&jb->api->scene.gte, &d, line->a, line->b, line->color_a,
                          line->color_b, e, &bucket))
        return JB_OK;
    split_vertex(&e[0], NULL, &a);
    split_vertex(&e[1], NULL, &b);
    memset(&tmpl, 0, sizeof tmpl);
    tmpl.kind = JBI_REC_LINE;
    tmpl.u.line.gouraud = e[0].color != e[1].color;
    tmpl.u.line.blend = line->blend;
    return jbi_frame_add_line(jb, fn, bucket, &tmpl, &a, &b);
}

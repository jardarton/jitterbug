/*
 * The frame (include/jitterbug.h, The frame; frame.h): double buffering
 * in VRAM, the frame's records (one jb_ot, record.h), the 2D draws that
 * clip instead of refusing, widescreen, and the options that have no other
 * home: the CLUT cache and the geometry path.
 *
 * jb_begin_frame: the back buffer (core/layout.h's rule, fixed when the
 * context was made); the drawing area and offset set to it with GP0(E3h-
 * E5h); the buffer cleared in the frame's current size (fills where VRAM
 * columns are on 16-pixel boundaries, which is all a fill can do exactly,
 * and opaque flat rectangles in the drawing area for the columns at the
 * ends, with the mask settings off for them and put back after, so no
 * pixel outside the buffer is written); the records emptied; the 2D layer
 * the overlay; the precise table emptied (jb_gte_begin_frame).
 *
 * jb_end_frame: the area and offset set to the back buffer again (a
 * console-level call during the frame may have moved them), every record
 * drawn (record.c), the texture window put back as it was if records
 * changed it, the buffer shown with GP1(05h), the display enabled with
 * GP1(03h) after the first frame, and the sticky error taken.
 *
 * Everything is sent as GP0 and GP1 words through the same paths jb_gp0
 * and jb_gp1 use, so the console-level state (GPUSTAT, GP1(10h)) shows
 * what the frame did.
 */
#include <stdio.h>
#include <string.h>

#include "api/frame.h"

/* ------------------------------------------------------------------------ */
/* Set-up */

void jbi_frame_init(jb_context *jb, const jb_config *c)
{
    struct jbi_api *a = jb->api;

    a->narrow_w = a->cfg_w;
    a->res_h = a->cfg_h;
    a->wide_w = jbi_widescreen_width(a->cfg_h);
    a->ws_reserved = c->widescreen != 0;
    a->widescreen = a->ws_reserved;
    a->res_w = a->narrow_w;
    if (a->ws_reserved && a->wide_w > a->res_w)
        a->res_w = a->wide_w;
    a->nbuffers = jbi_frame_buffers(a->res_w, a->res_h, a->buffer);
    a->frame_w = a->widescreen ? a->wide_w : a->narrow_w;
    a->shown_w = a->frame_w;
    a->cfg_w = a->frame_w;
    a->max_records = c->max_primitives > 0 ? c->max_primitives : JB_DEFAULT_MAX_PRIMITIVES;
    a->ot_size = c->ot_size > 0 ? c->ot_size : JB_DEFAULT_OT_SIZE;
    a->scene.geometry = c->geometry;
    a->layer = JB_LAYER_OVERLAY;
    if (c->clut_cache_off)
        jbi_set_clut_cache(jb, 0);
}

void jbi_frame_free(jb_context *jb)
{
    jbi_ot_destroy(jb->api->frame_ot);
    jb->api->frame_ot = NULL;
}

/* ------------------------------------------------------------------------ */
/* The internal interface (frame.h) */

int jbi_frame_active(const jb_context *jb)
{
    return jb->api->in_frame;
}

void jbi_frame_size(const jb_context *jb, int *w, int *h)
{
    *w = jb->api->frame_w;
    *h = jb->api->res_h;
}

int jbi_frame_ot_size(const jb_context *jb)
{
    return jb->api->ot_size;
}

int jbi_api_geometry(const jb_context *jb)
{
    return jb->api->scene.geometry;
}

jbi_split_view jbi_frame_view(const jb_context *jb, int textured)
{
    jbi_split_view v;

    v.x0 = 0;
    v.y0 = 0;
    v.x1 = jb->api->frame_w - 1;
    v.y1 = jb->api->res_h - 1;
    v.perspective = 0; /* not read: both interpolations are kept */
    v.perspective_given = 0;
    (void)textured;
    return v;
}

static int not_in_frame(jb_context *jb, const char *fn)
{
    if (jb->api->in_frame)
        return 0;
    jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: no frame begun (call jb_begin_frame first)",
                 fn);
    return 1;
}

/* jbi_frame_record, with the failure's result in *res. */
static jbi_record *record_at(jb_context *jb, const char *fn, int where, jb_result *res)
{
    struct jbi_api *a = jb->api;
    jbi_record *r;
    int bucket;

    *res = JB_ERROR_INVALID_ARGUMENT;
    if (not_in_frame(jb, fn))
        return NULL;
    if (where == JBI_FRAME_LAYER)
        where = a->layer == JB_LAYER_BACKGROUND ? JBI_FRAME_BACKGROUND : JBI_FRAME_OVERLAY;
    if (where == JBI_FRAME_BACKGROUND) {
        bucket = a->ot_size + 1;
    } else if (where == JBI_FRAME_OVERLAY) {
        bucket = 0;
    } else if (where >= 0 && where < a->ot_size) {
        bucket = where + 1;
    } else {
        jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: scene bucket %d is outside 0..%d", fn,
                     where, a->ot_size - 1);
        return NULL;
    }
    r = jbi_ot_add(a->frame_ot, bucket);
    if (r == NULL)
        *res = jbi_api_fail(jb, JB_ERROR_OUT_OF_MEMORY,
                            "%s: the frame is full (%d records); raise jb_config.max_primitives",
                            fn, a->max_records);
    else
        *res = JB_OK;
    return r;
}

int jbi_frame_has_room(jb_context *jb, const char *fn)
{
    struct jbi_api *a = jb->api;

    if (not_in_frame(jb, fn))
        return 0;
    if (a->frame_ot->count < a->frame_ot->max_records)
        return 1;
    jbi_api_fail(jb, JB_ERROR_OUT_OF_MEMORY,
                 "%s: the frame is full (%d records); raise jb_config.max_primitives", fn,
                 a->max_records);
    return 0;
}

jbi_record *jbi_frame_record(jb_context *jb, const char *fn, int where)
{
    jb_result r;

    return record_at(jb, fn, where, &r);
}

/* A record where that is a copy of tmpl, or NULL with *res. */
static jbi_record *copy_at(jb_context *jb, const char *fn, int where, const jbi_record *tmpl,
                           jb_result *res)
{
    jbi_record *rec = record_at(jb, fn, where, res);
    int32_t next;

    if (rec == NULL)
        return NULL;
    next = rec->next;
    *rec = *tmpl;
    rec->next = next;
    return rec;
}

/* ------------------------------------------------------------------------ */
/* Pieces into records */

typedef struct {
    jb_context *jb;
    const char *fn;
    const int *where;  /* by polygon */
    const jbi_record *const *tmpl; /* by polygon */
    uint32_t flat[JBI_SPLIT_MAX_PRIM]; /* the colour of a flat primitive, by polygon */
    jb_result r;       /* the first failure */
} adder;

static uint32_t rgb_of(const jbi_split_vertex *v)
{
    return JB_RGB(v->r, v->g, v->b);
}

static int clamp_int(int64_t q)
{
    return q < -0x7fffffff ? -0x7fffffff : q > 0x7fffffff ? 0x7fffffff : (int)q;
}

static jbi_record *piece_record(adder *ad, int poly)
{
    if (ad->r != JB_OK)
        return NULL; /* the frame is full: the rest are dropped, once reported */
    return copy_at(ad->jb, ad->fn, ad->where[poly], ad->tmpl[poly], &ad->r);
}

static void sub_vertex(jbi_record *rec, int i, const jbi_split_vertex *v)
{
    rec->sp.v[i].x = clamp_int(v->qx);
    rec->sp.v[i].y = clamp_int(v->qy);
    rec->sp.v[i].z = v->z > 0 ? v->z : 0;
}

static uint8_t byte(int a)
{
    return (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
}

/* The GP0(E3h) and GP0(E4h) words of the back buffer's pixels x0..x1 by
   y0..y1 (frame coordinates). */
static void area_words(const jb_context *jb, int x0, int y0, int x1, int y1, uint32_t *e3,
                       uint32_t *e4)
{
    const jbi_rect *b = &jb->api->buffer[jb->api->back];

    *e3 = 0xe3000000u | (uint32_t)(b->x + x0) | (uint32_t)(b->y + y0) << 10;
    *e4 = 0xe4000000u | (uint32_t)(b->x + x1) | (uint32_t)(b->y + y1) << 10;
}

static void poly_piece(void *user, int poly, const jbi_split_vertex *v, int n, unsigned passes,
                       const jbi_split_area *area)
{
    adder *ad = user;
    jbi_record *rec;
    uint32_t flat = ad->flat[poly];
    int i;

    /* Only in the passes its template allows too (a 3D face drawn by one
       output alone: src/geom/pipeline.h, jbi_geom_poly.passes), 0 there
       for every pass; in none, nothing is recorded. */
    if (ad->tmpl[poly]->passes != 0)
        passes &= ad->tmpl[poly]->passes;
    if (passes == 0)
        return;
    if ((rec = piece_record(ad, poly)) == NULL)
        return;
    rec->kind = JBI_REC_POLY;
    rec->sub = 1;
    /* Both passes is the record's usual 0 (record.h, passes). */
    rec->passes = (uint8_t)(passes == (JBI_SPLIT_PASS_1X | JBI_SPLIT_PASS_PRECISE) ? 0 : passes);
    if (area != NULL) {
        rec->area = 1;
        area_words(ad->jb, area->x0, area->y0, area->x1, area->y1, &rec->e3, &rec->e4);
    }
    rec->sp.known = (1u << n) - 1;
    rec->u.poly.vertices = n;
    rec->u.poly.texture = NULL;
    for (i = 0; i < n; ++i) {
        jb_vertex *o = &rec->u.poly.v[i];

        o->x = v[i].x;
        o->y = v[i].y;
        o->color = rec->u.poly.gouraud ? rgb_of(&v[i]) : flat;
        o->u = v[i].u;
        o->v = v[i].v;
        sub_vertex(rec, i, &v[i]);
        /* The perspective-correct pass's attributes, apart from the
           affine ones the words carry: which of the two draws is decided
           when the frame ends (the option then), and the 1x pass above 1x
           keeps the affine ones (src/core/context.h, jbi_subpixel). */
        rec->sp.pattr[i][0] = byte(v[i].pu);
        rec->sp.pattr[i][1] = byte(v[i].pv);
        if (rec->u.poly.gouraud) {
            rec->sp.pattr[i][2] = byte(v[i].pr);
            rec->sp.pattr[i][3] = byte(v[i].pg);
            rec->sp.pattr[i][4] = byte(v[i].pb);
        } else {
            rec->sp.pattr[i][2] = (uint8_t)(flat & 0xff);
            rec->sp.pattr[i][3] = (uint8_t)(flat >> 8 & 0xff);
            rec->sp.pattr[i][4] = (uint8_t)(flat >> 16 & 0xff);
        }
    }
    rec->sp.has_pattr = 1;
    for (; i < 4; ++i)
        memset(&rec->u.poly.v[i], 0, sizeof rec->u.poly.v[i]);
}

/* The polygons v[k] (ns[k] vertices each, count of them, each recorded at
   where[k] from tmpl[k]) as one primitive (split.h: passed through if all meet the
   limits, else clipped together); given: the vertices'
   perspective-correct attributes are the caller's
   (jbi_split_view.perspective_given). */
static jb_result add_polys(jb_context *jb, const char *fn, const int *where,
                           const jbi_record *const *tmpl, const jbi_split_vertex *const *v,
                           const int *ns, int count, int given)
{
    adder ad;
    jbi_split_view view;
    int k;

    if (not_in_frame(jb, fn))
        return JB_ERROR_INVALID_ARGUMENT;
    for (k = 0; k < count; ++k) {
        if (ns[k] < 3 || ns[k] > JBI_SPLIT_MAX_IN)
            return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: %d vertices is outside 3..%d",
                                fn, ns[k], JBI_SPLIT_MAX_IN);
        ad.flat[k] = rgb_of(&v[k][0]);
    }
    ad.jb = jb;
    ad.fn = fn;
    ad.where = where;
    ad.tmpl = tmpl;
    ad.r = JB_OK;
    view = jbi_frame_view(jb, tmpl[0]->textured);
    view.perspective_given = given;
    jbi_split_polys(&view, v, ns, count, poly_piece, &ad, NULL);
    return ad.r;
}

jb_result jbi_frame_add_poly(jb_context *jb, const char *fn, int where, const jbi_record *tmpl,
                             const jbi_split_vertex *v, int n)
{
    return add_polys(jb, fn, &where, &tmpl, &v, &n, 1, 0);
}

jb_result jbi_frame_add_poly_persp(jb_context *jb, const char *fn, int where,
                                   const jbi_record *tmpl, const jbi_split_vertex *v, int n)
{
    return add_polys(jb, fn, &where, &tmpl, &v, &n, 1, 1);
}

jb_result jbi_frame_add_triangles(jb_context *jb, const char *fn, int where,
                                  const jbi_record *tmpl, const jbi_split_vertex *a,
                                  const jbi_split_vertex *b, int given)
{
    const jbi_split_vertex *v[2];
    const jbi_record *t[2];
    int ns[2] = {3, 3}, at[2];

    v[0] = a;
    v[1] = b;
    t[0] = t[1] = tmpl;
    at[0] = at[1] = where;
    return add_polys(jb, fn, at, t, v, ns, 2, given);
}

jb_result jbi_frame_add_polys_persp(jb_context *jb, const char *fn, const int *where,
                                    const jbi_record *const *tmpl,
                                    const jbi_split_vertex *const *v, const int *n, int count)
{
    if (count < 1 || count > JBI_SPLIT_MAX_PRIM)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: %d polygons is outside 1..%d", fn,
                            count, JBI_SPLIT_MAX_PRIM);
    return add_polys(jb, fn, where, tmpl, v, n, count, 1);
}

static void line_piece(void *user, const jbi_split_vertex *a, const jbi_split_vertex *b)
{
    adder *ad = user;
    jbi_record *rec = piece_record(ad, 0);

    if (rec == NULL)
        return;
    rec->kind = JBI_REC_LINE;
    rec->sub = 1;
    rec->sp.known = 3;
    rec->u.line.v[0].x = a->x;
    rec->u.line.v[0].y = a->y;
    rec->u.line.v[1].x = b->x;
    rec->u.line.v[1].y = b->y;
    rec->u.line.v[0].color = rec->u.line.gouraud ? rgb_of(a) : ad->flat[0];
    rec->u.line.v[1].color = rec->u.line.gouraud ? rgb_of(b) : ad->flat[0];
    rec->u.line.v[0].u = rec->u.line.v[0].v = rec->u.line.v[1].u = rec->u.line.v[1].v = 0;
    sub_vertex(rec, 0, a);
    sub_vertex(rec, 1, b);
}

jb_result jbi_frame_add_line(jb_context *jb, const char *fn, int where, const jbi_record *tmpl,
                             const jbi_split_vertex *a, const jbi_split_vertex *b)
{
    adder ad;
    jbi_split_view view;

    if (not_in_frame(jb, fn))
        return JB_ERROR_INVALID_ARGUMENT;
    ad.jb = jb;
    ad.fn = fn;
    ad.where = &where;
    ad.tmpl = &tmpl;
    ad.flat[0] = rgb_of(a);
    ad.r = JB_OK;
    view = jbi_frame_view(jb, 0);
    jbi_split_line(&view, a, b, line_piece, &ad);
    return ad.r;
}

/* u stepped by d pixels, mirrored or not, modulo 256. */
static int texel_step(int u, int64_t d, int flip)
{
    return (int)((flip ? (int64_t)u - d : (int64_t)u + d) & 255);
}

jb_result jbi_frame_add_rect(jb_context *jb, const char *fn, int where, const jbi_record *tmpl)
{
    const jb_rect *r = &tmpl->u.rect;
    int64_t x0 = r->x, y0 = r->y, x1 = (int64_t)r->x + r->w, y1 = (int64_t)r->y + r->h;
    int64_t fw = jb->api->frame_w, fh = jb->api->res_h, px, py;
    jb_result res;

    if (not_in_frame(jb, fn))
        return JB_ERROR_INVALID_ARGUMENT;
    if (r->w <= 0 || r->h <= 0 || x1 <= 0 || y1 <= 0 || x0 >= fw || y0 >= fh)
        return JB_OK; /* nothing in the frame */
    if (x0 >= -1024 && y0 >= -1024 && x0 <= 1023 && y0 <= 1023 && r->w <= 1023 && r->h <= 511) {
        copy_at(jb, fn, where, tmpl, &res);
        return res;
    }
    /* Clipped to the frame exactly: a rectangle draws one texel a pixel,
       so the part in the frame starts at the texel its first pixel had. */
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > fw)
        x1 = fw;
    if (y1 > fh)
        y1 = fh;
    for (py = y0; py < y1; py += 511) {
        for (px = x0; px < x1; px += 1023) {
            jbi_record *rec = copy_at(jb, fn, where, tmpl, &res);

            if (rec == NULL)
                return res;
            rec->u.rect.x = (int)px;
            rec->u.rect.y = (int)py;
            rec->u.rect.w = (int)(x1 - px < 1023 ? x1 - px : 1023);
            rec->u.rect.h = (int)(y1 - py < 511 ? y1 - py : 511);
            /* Texel coordinates only for a textured one: an untextured
               rectangle's are not used, and need not be set. */
            if (tmpl->textured) {
                rec->u.rect.u = texel_step(r->u, px - r->x, r->flip_x);
                rec->u.rect.v = texel_step(r->v, py - r->y, r->flip_y);
            }
        }
    }
    return JB_OK;
}

/* ------------------------------------------------------------------------ */
/* Begin and end */

static void word(jb_context *jb, uint32_t w)
{
    jbi_api_command(jb, &w, 1);
}

/* The drawing area and offset: the back buffer at its current size. */
static void target(jb_context *jb)
{
    struct jbi_api *a = jb->api;
    const jbi_rect *b = &a->buffer[a->back];
    uint32_t x0 = (uint32_t)b->x, y0 = (uint32_t)b->y;
    uint32_t x1 = x0 + (uint32_t)a->frame_w - 1, y1 = y0 + (uint32_t)a->res_h - 1;

    word(jb, 0xe3000000u | x0 | y0 << 10);
    word(jb, 0xe4000000u | x1 | y1 << 10);
    word(jb, 0xe5000000u | (x0 & 0x7ffu) | (y0 & 0x7ffu) << 11);
}

/* An opaque flat rectangle, x and y relative to the drawing offset, w and
   h > 0, in rows of at most 511. */
static void clear_columns(jb_context *jb, int x, int y, int w, int h, uint32_t color)
{
    while (h > 0) {
        int n = h < 511 ? h : 511;
        uint32_t c[3];

        c[0] = 0x60000000u | color;
        c[1] = ((uint32_t)x & 0xffffu) | ((uint32_t)y & 0xffffu) << 16;
        c[2] = (uint32_t)w | (uint32_t)n << 16;
        jbi_api_command(jb, c, 3);
        y += n;
        h -= n;
    }
}

static void clear_back(jb_context *jb, uint32_t color)
{
    struct jbi_api *a = jb->api;
    const jbi_rect *b = &a->buffer[a->back];
    int x0 = b->x, x1 = b->x + a->frame_w, h = a->res_h;
    int a0 = (x0 + 15) & ~15, a1 = x1 & ~15, x, y;

    if (a1 <= a0)
        a0 = a1 = x1; /* no whole 16 columns: all rectangles */
    for (x = a0; x < a1; x += 1008) {
        int w = a1 - x < 1008 ? a1 - x : 1008;

        for (y = 0; y < h; y += 511) {
            uint32_t f[3];
            int n = h - y < 511 ? h - y : 511;

            f[0] = 0x02000000u | color;
            f[1] = (uint32_t)x | (uint32_t)(b->y + y) << 16;
            f[2] = (uint32_t)w | (uint32_t)n << 16;
            jbi_api_command(jb, f, 3);
        }
    }
    if (a0 > x0 || x1 > a1) {
        jbi_draw_state *d = &jb->draw;
        uint32_t mask = 0xe6000000u | (d->set_mask ? 1u : 0) | (d->check_mask ? 2u : 0);

        word(jb, 0xe6000000u);
        if (a0 > x0)
            clear_columns(jb, 0, 0, a0 - x0, h, color);
        if (x1 > a1)
            clear_columns(jb, a1 - x0, 0, x1 - a1, h, color);
        word(jb, mask);
    }
}

/* Widescreen as asked for, taking effect for drawing: the width frames
   are drawn at (jb_begin_frame). */
static void apply_width(jb_context *jb)
{
    struct jbi_api *a = jb->api;

    a->frame_w = a->widescreen ? a->wide_w : a->narrow_w;
}

/* And for the display, with the buffer drawn at that width (jb_end_frame):
   what GP1(00h) returns the display to, and the display's width itself,
   unless the program set a display mode (GP1(08h)) since the last
   GP1(00h): that mode's size and pixel format stay as they are until
   GP1(00h) (include/jitterbug.h, jb_set_widescreen). Only a change of the
   frame's width touches them. */
static void publish_width(jb_context *jb)
{
    struct jbi_api *a = jb->api;

    if (a->frame_w == a->shown_w)
        return;
    a->shown_w = a->frame_w;
    a->cfg_w = a->frame_w;
    if (!a->mode_explicit)
        a->disp_w = a->frame_w;
}

jb_result jb_begin_frame(jb_context *jb, uint32_t clear_color)
{
    struct jbi_api *a;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    a = jb->api;
    if (a->in_frame)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_begin_frame: a frame is already begun (call jb_end_frame)");
    if (clear_color > 0xffffffu)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_begin_frame: clear_color 0x%lx is above 0xffffff",
                            (unsigned long)clear_color);
    if (a->frame_ot == NULL) {
        a->frame_ot = jbi_ot_create(jb, a->ot_size + 2, a->max_records);
        if (a->frame_ot == NULL)
            return jbi_api_fail(jb, JB_ERROR_OUT_OF_MEMORY,
                                "jb_begin_frame: no memory for %d records and %d buckets "
                                "(jb_config.max_primitives, ot_size)",
                                a->max_records, a->ot_size);
    }
    apply_width(jb);
    a->back = a->nbuffers == 2 ? a->parity : 0;
    target(jb);
    clear_back(jb, clear_color);
    jbi_ot_clear(a->frame_ot);
    a->layer = JB_LAYER_OVERLAY;
    jbi_precise_begin_frame(a->precise);
    a->in_frame = 1;
    return JB_OK;
}

jb_result jb_end_frame(jb_context *jb)
{
    struct jbi_api *a;
    const jbi_rect *b;
    uint32_t window, e3, e4;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    a = jb->api;
    if (!a->in_frame)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_end_frame: no frame begun (call jb_begin_frame first)");
    target(jb);
    window = a->env[0];
    area_words(jb, 0, 0, a->frame_w - 1, a->res_h - 1, &e3, &e4);
    jbi_ot_draw_area(jb, a->frame_ot, e3, e4);
    if (a->env[0] != window)
        word(jb, 0xe2000000u | window);
    b = &a->buffer[a->back];
    jb_gp1(jb, 0x05000000u | (uint32_t)b->x | (uint32_t)b->y << 10);
    publish_width(jb);
    if (!a->ended)
        jb_gp1(jb, 0x03000000u);
    a->ended = 1;
    a->parity ^= 1;
    a->in_frame = 0;
    {
        /* The failure returned, with its detail as jb_error_string's (not
           latched again). */
        const char *detail;
        jb_result r = jbi_api_take_error(jb, &detail);

        if (r != JB_OK)
            snprintf(a->error, sizeof a->error, "%s", detail);
        return r;
    }
}

/* ------------------------------------------------------------------------ */
/* Options */

jb_result jb_frame_size(const jb_context *jb, int *width, int *height)
{
    const struct jbi_api *a;
    int w;

    if (width != NULL)
        *width = 0;
    if (height != NULL)
        *height = 0;
    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    a = jb->api;
    /* In a frame, its own width; outside, the width the next frame takes
       at jb_begin_frame (apply_width), widescreen as last asked for. */
    w = a->in_frame ? a->frame_w : a->widescreen ? a->wide_w : a->narrow_w;
    if (width != NULL)
        *width = w;
    if (height != NULL)
        *height = a->res_h;
    return JB_OK;
}

jb_result jb_set_layer(jb_context *jb, int layer)
{
    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (layer != JB_LAYER_OVERLAY && layer != JB_LAYER_BACKGROUND)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_set_layer: layer %d is neither JB_LAYER_OVERLAY (0) nor "
                            "JB_LAYER_BACKGROUND (1)",
                            layer);
    jb->api->layer = layer;
    return JB_OK;
}

jb_result jb_set_widescreen(jb_context *jb, int on)
{
    struct jbi_api *a;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    a = jb->api;
    if (!a->ws_reserved) {
        if (!on)
            return JB_OK;
        return jbi_api_fail(jb, JB_ERROR_UNSUPPORTED,
                            "jb_set_widescreen: the context was made without "
                            "jb_config.widescreen, so it has no room for %d-pixel buffers",
                            a->wide_w);
    }
    a->widescreen = on != 0;
    /* Before the first frame nothing is shown yet: the display follows at
       once. Otherwise the next frame is drawn at the new width and shown
       with it at its jb_end_frame. */
    if (!a->in_frame && !a->ended) {
        apply_width(jb);
        publish_width(jb);
    }
    return JB_OK;
}

jb_result jb_set_clut_cache(jb_context *jb, int on)
{
    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    jbi_set_clut_cache(jb, on != 0);
    return JB_OK;
}

/* ------------------------------------------------------------------------ */
/* The 2D draws */

/* A 2D vertex for the splitter. Texel coordinates only when textured:
   otherwise the struct's u and v are ignored, as jb_prim_* ignores them,
   and may hold anything (or nothing). */
static void split_vertex(const jb_vertex *in, uint32_t color, int textured, jbi_split_vertex *out)
{
    out->x = in->x;
    out->y = in->y;
    out->qx = (int64_t)in->x * JBI_PRECISE_ONE;
    out->qy = (int64_t)in->y * JBI_PRECISE_ONE;
    out->z = 0;
    out->r = (int)(color & 0xff);
    out->g = (int)((color >> 8) & 0xff);
    out->b = (int)((color >> 16) & 0xff);
    out->u = textured ? in->u : 0;
    out->v = textured ? in->v : 0;
}

/* A friendly primitive's template: the window its texture needs. */
static void friendly(jbi_record *t, const jb_texture *tex)
{
    if (tex != NULL) {
        t->textured = 1;
        t->tex = *tex;
        t->window = 1;
        t->tw = jbi_record_window(tex);
    }
}

jb_result jb_draw_poly(jb_context *jb, const jb_poly *poly)
{
    static const char fn[] = "jb_draw_poly";
    jbi_record t;
    jbi_split_vertex v[4], c[4];
    jb_result r;
    int i, n, fits;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (poly == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_draw_poly: poly is NULL");
    if (not_in_frame(jb, fn))
        return JB_ERROR_INVALID_ARGUMENT;
    r = jbi_check_poly(jb, fn, poly, 0);
    if (r != JB_OK)
        return r;
    memset(&t, 0, sizeof t);
    t.kind = JBI_REC_POLY;
    t.u.poly = *poly;
    t.u.poly.texture = NULL;
    t.u.poly.gouraud = poly->gouraud != 0;
    t.u.poly.raw = poly->texture != NULL && poly->raw;
    friendly(&t, poly->texture);
    n = poly->vertices;
    /* The colours a primitive uses: none when raw (they need not be set,
       as for jb_prim_poly), v[0]'s when flat. */
    for (i = 0; i < n; ++i)
        split_vertex(&poly->v[i],
                     t.u.poly.raw ? 0 : poly->gouraud ? poly->v[i].color : poly->v[0].color,
                     poly->texture != NULL, &v[i]);
    if (n == 3)
        return jbi_frame_add_poly(jb, fn, JBI_FRAME_LAYER, &t, v, 3);
    /* A quad as the GPU draws it, v0 v1 v2 and v1 v2 v3: whole when both
       triangles meet the limits (it is passed on unchanged, in cyclic
       order v0 v1 v3 v2), else its two triangles, which is right whatever
       its shape (a quad's corners need not be convex). */
    fits = jbi_split_triangle_fits(&v[0], &v[1], &v[2]) &&
           jbi_split_triangle_fits(&v[1], &v[2], &v[3]);
    if (fits) {
        c[0] = v[0];
        c[1] = v[1];
        c[2] = v[3];
        c[3] = v[2];
        return jbi_frame_add_poly(jb, fn, JBI_FRAME_LAYER, &t, c, 4);
    }
    /* One primitive: the two clipped together, so that neither draws
       over the other across the diagonal they share (src/geom/split.h). */
    return jbi_frame_add_triangles(jb, fn, JBI_FRAME_LAYER, &t, v, v + 1, 0);
}

static jb_result draw_line(jb_context *jb, const char *fn, const jb_line *l)
{
    jbi_record t;
    jbi_split_vertex a, b;

    memset(&t, 0, sizeof t);
    t.kind = JBI_REC_LINE;
    t.u.line = *l;
    t.u.line.gouraud = l->gouraud != 0;
    split_vertex(&l->v[0], l->v[0].color, 0, &a);
    split_vertex(&l->v[1], l->gouraud ? l->v[1].color : l->v[0].color, 0, &b);
    return jbi_frame_add_line(jb, fn, JBI_FRAME_LAYER, &t, &a, &b);
}

jb_result jb_draw_line(jb_context *jb, const jb_line *line)
{
    static const char fn[] = "jb_draw_line";
    jb_result r;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (line == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_draw_line: line is NULL");
    if (not_in_frame(jb, fn))
        return JB_ERROR_INVALID_ARGUMENT;
    r = jbi_check_line(jb, fn, line->gouraud, line->blend, line->v, 2, 0);
    if (r != JB_OK)
        return r;
    return draw_line(jb, fn, line);
}

jb_result jb_draw_polyline(jb_context *jb, const jb_polyline *line)
{
    static const char fn[] = "jb_draw_polyline";
    jb_result r;
    size_t i;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (line == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_draw_polyline: line is NULL");
    if (not_in_frame(jb, fn))
        return JB_ERROR_INVALID_ARGUMENT;
    r = jbi_check_polyline(jb, fn, line, 0);
    if (r != JB_OK)
        return r;
    for (i = 0; i + 1 < line->count; ++i) {
        jb_line s = jbi_polyline_segment(line, i);

        r = draw_line(jb, fn, &s);
        if (r != JB_OK)
            return r;
    }
    return JB_OK;
}

jb_result jb_draw_rect(jb_context *jb, const jb_rect *rect)
{
    static const char fn[] = "jb_draw_rect";
    jbi_record t;
    jb_result r;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (rect == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_draw_rect: rect is NULL");
    if (not_in_frame(jb, fn))
        return JB_ERROR_INVALID_ARGUMENT;
    r = jbi_check_rect(jb, fn, rect, 0);
    if (r != JB_OK)
        return r;
    memset(&t, 0, sizeof t);
    t.kind = JBI_REC_RECT;
    t.u.rect = *rect;
    t.u.rect.texture = NULL;
    t.u.rect.flip_x = rect->texture != NULL && rect->flip_x;
    t.u.rect.flip_y = rect->texture != NULL && rect->flip_y;
    friendly(&t, rect->texture);
    return jbi_frame_add_rect(jb, fn, JBI_FRAME_LAYER, &t);
}

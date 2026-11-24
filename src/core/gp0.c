/*
 * GP0 command decoding and the VRAM transfer entry points (context.h).
 *
 * Commands handled:
 *   00h        NOP                   01h  clear the cache (the CLUT cache here)
 *   02h        fill rectangle        1Fh  IRQ request (ignored)
 *   20h-3Fh    polygons              40h-5Fh  lines and polylines
 *   60h-7Fh    rectangles            80h-9Fh  VRAM to VRAM copy
 *   A0h-BFh    CPU to VRAM (data inline in the packet)
 *   C0h-DFh    VRAM to CPU (no reader in a packet; skipped)
 *   E1h-E6h    draw mode, texture window, drawing area, offset, mask
 *
 * Decoding reads and writes only the context's drawing state; primitives
 * and transfers go to the backend (backend.h).
 */
#include <string.h>

#include "core/context.h"

static void set_draw_mode(jbi_draw_state *d, uint32_t w)
{
    d->tpage_x = (int)(w & 0xf) * 64;
    d->tpage_y = (int)((w >> 4) & 1) * 256;
    d->semi_mode = (int)((w >> 5) & 3);
    d->tex_depth = (int)((w >> 7) & 3);
    d->dither = (int)((w >> 9) & 1);
    d->draw_display = (int)((w >> 10) & 1);
    d->rect_flip_x = (int)((w >> 12) & 1);
    d->rect_flip_y = (int)((w >> 13) & 1);
    d->tex_disable = d->allow_tex_disable && ((w >> 11) & 1);
}

/*
 * A textured polygon's page attribute replaces the draw mode's page,
 * semi-transparency mode and depth (bits 0-8), and bit 11 as GP0(E1h)
 * writes it (the console's log of ps1-tests' gp0-e1); dithering, drawing
 * to the display area and the flips stay.
 */
static void set_tpage_attr(jbi_draw_state *d, uint32_t attr)
{
    d->tpage_x = (int)(attr & 0xf) * 64;
    d->tpage_y = (int)((attr >> 4) & 1) * 256;
    d->semi_mode = (int)((attr >> 5) & 3);
    d->tex_depth = (int)((attr >> 7) & 3);
    d->tex_disable = d->allow_tex_disable && ((attr >> 11) & 1);
}

static void set_tex_window(jbi_draw_state *d, uint32_t w)
{
    uint32_t mask_x = w & 0x1f, mask_y = (w >> 5) & 0x1f;
    uint32_t off_x = (w >> 10) & 0x1f, off_y = (w >> 15) & 0x1f;

    d->tw_and_x = ~(mask_x << 3) & 0xff;
    d->tw_or_x = (off_x & mask_x) << 3;
    d->tw_and_y = ~(mask_y << 3) & 0xff;
    d->tw_or_y = (off_y & mask_y) << 3;
}

void jbi_reset_state(jb_context *ctx)
{
    jbi_draw_state *d = &ctx->draw;

    set_draw_mode(d, 0);
    set_tex_window(d, 0);
    d->clip_x0 = d->clip_y0 = 0;
    d->clip_x1 = d->clip_y1 = 0;
    d->off_x = d->off_y = 0;
    d->set_mask = 0;
    d->check_mask = 0;
}

static int sext11(uint32_t v)
{
    return (int)((v & 0x7ff) ^ 0x400) - 0x400;
}

/* Vertex word: x in bits 0-10, y in bits 16-26, both signed. */
static void decode_xy(const jbi_draw_state *d, uint32_t w, jbi_vertex *v)
{
    v->x = sext11(w) + d->off_x;
    v->y = sext11(w >> 16) + d->off_y;
}

/*
 * The vertex's position in 1/256 pixels for the scaled pass: where the
 * precise source (precise_source.h) says it was projected, if it knows this
 * packed SXY and the position is within PRECISE_SLACK of the vertex, else
 * the integer position.
 */
#define PRECISE_SLACK (JBI_PRECISE_ONE + JBI_PRECISE_ONE / 8)

/* Whether the scaled pass uses precise positions. */
static int precise_positions(const jb_context *ctx)
{
    return ctx->scale > 1 && ctx->has_precise && ctx->precise.enabled(ctx->precise.user);
}

/* The position for vertex v from its record pv, if hit and positions are
   used (use: precise_positions for the precise source; for given data,
   jbi_gp0_subpixel, its own switch). */
static void precise_xy_from(const jb_context *ctx, jbi_precise_vertex pv, int hit, int use,
                            const jbi_vertex *v, int *qx, int *qy)
{
    *qx = v->x * JBI_PRECISE_ONE;
    *qy = v->y * JBI_PRECISE_ONE;
    if (!hit || !use)
        return;
    pv.x += ctx->draw.off_x * JBI_PRECISE_ONE;
    pv.y += ctx->draw.off_y * JBI_PRECISE_ONE;
    /* The GTE's refined position can sit slightly over a pixel from its
       truncated SXY (up to about 1.05 px); accept that, reject chance
       matches further away. */
    if (pv.x - *qx > PRECISE_SLACK || *qx - pv.x > PRECISE_SLACK ||
        pv.y - *qy > PRECISE_SLACK || *qy - pv.y > PRECISE_SLACK)
        return;
    *qx = pv.x;
    *qy = pv.y;
}

/* The same for a vertex looked up on its own (lines). */
static void precise_xy(const jb_context *ctx, uint32_t w, const jbi_vertex *v, int *qx, int *qy)
{
    jbi_precise_vertex pv = { 0, 0, 0 };
    int use = precise_positions(ctx);
    int hit = use && ctx->precise.lookup(ctx->precise.user, w, &pv);

    precise_xy_from(ctx, pv, hit, use, v, qx, qy);
}

/* Whether given data's positions are used: its switch, above 1x. */
static int given_positions(const jb_context *ctx, const jbi_subpixel *sp)
{
    return ctx->scale > 1 && sp->positions;
}

/* A line's vertex k (0 or 1): from given data, or the precise source
   without it. */
static void line_xy(const jb_context *ctx, const jbi_subpixel *sp, int k, uint32_t w,
                    const jbi_vertex *v, int *qx, int *qy)
{
    if (sp == NULL)
        precise_xy(ctx, w, v, qx, qy);
    else
        precise_xy_from(ctx, sp->v[k], (sp->known >> k) & 1, given_positions(ctx, sp), v, qx,
                        qy);
}

static void decode_rgb(uint32_t w, jbi_vertex *v)
{
    v->r = (int)(w & 0xff);
    v->g = (int)((w >> 8) & 0xff);
    v->b = (int)((w >> 16) & 0xff);
}

static int tex_depth_bits(const jbi_draw_state *d)
{
    switch (d->tex_depth) {
    case JBI_TEX_4BIT:
        return 4;
    case JBI_TEX_8BIT:
        return 8;
    default:
        return 15;
    }
}

/* The common part of a primitive: the current drawing state. The dither
   bit is the draw mode's; a backend drawing without dithering ignores it. */
static void prim_state(const jbi_draw_state *d, jbi_prim *p, int kind, int flags, int clut_x,
                       int clut_y)
{
    p->kind = (uint8_t)kind;
    p->flags = (uint8_t)flags;
    p->depth = (uint8_t)((flags & JBI_F_TEXTURED) ? tex_depth_bits(d) : 0);
    p->semi_mode = (uint8_t)d->semi_mode;
    p->dither = (uint8_t)d->dither;
    p->check_mask = (uint8_t)d->check_mask;
    p->flip_x = (uint8_t)d->rect_flip_x;
    p->flip_y = (uint8_t)d->rect_flip_y;
    p->set_mask = d->set_mask;
    p->clip_x0 = (int16_t)d->clip_x0;
    p->clip_y0 = (int16_t)d->clip_y0;
    p->clip_x1 = (int16_t)d->clip_x1;
    p->clip_y1 = (int16_t)d->clip_y1;
    p->tex.tpx = d->tpage_x;
    p->tex.tpy = d->tpage_y;
    p->tex.clut_x = clut_x;
    p->tex.clut_y = clut_y;
    p->tex.and_x = d->tw_and_x;
    p->tex.or_x = d->tw_or_x;
    p->tex.and_y = d->tw_and_y;
    p->tex.or_y = d->tw_or_y;
    p->tex.clut = NULL;
}

/* Rows [y0, y1] clipped to the drawing area, for skipping bands. */
static void prim_rows(jbi_prim *p, int y0, int y1)
{
    if (y0 < p->clip_y0)
        y0 = p->clip_y0;
    if (y1 > p->clip_y1)
        y1 = p->clip_y1;
    p->row0 = (int16_t)y0;
    p->row1 = (int16_t)y1;
}

static int min3(int a, int b, int c)
{
    return a < b ? (a < c ? a : c) : (b < c ? b : c);
}

static int max3(int a, int b, int c)
{
    return a > b ? (a > c ? a : c) : (b > c ? b : c);
}

static void submit_triangle(jb_context *ctx, jbi_prim *p, const jbi_vertex *v, const int *qx,
                            const int *qy, int i0, int i1, int i2)
{
    p->u.tri.v[0] = v[i0];
    p->u.tri.v[1] = v[i1];
    p->u.tri.v[2] = v[i2];
    p->u.tri.qx[0] = qx[i0];
    p->u.tri.qx[1] = qx[i1];
    p->u.tri.qx[2] = qx[i2];
    p->u.tri.qy[0] = qy[i0];
    p->u.tri.qy[1] = qy[i1];
    p->u.tri.qy[2] = qy[i2];
    /* One pixel of margin, and the rows of the sub-pixel positions, which
       can be further away than that (PRECISE_SLACK). */
    {
        int y0 = min3(v[i0].y, v[i1].y, v[i2].y) - 1, y1 = max3(v[i0].y, v[i1].y, v[i2].y) + 1;
        int q0 = jbi_q_pixel(min3(qy[i0], qy[i1], qy[i2]));
        int q1 = jbi_q_pixel(max3(qy[i0], qy[i1], qy[i2]));

        prim_rows(p, q0 < y0 ? q0 : y0, q1 > y1 ? q1 : y1);
    }
    ctx->backend->submit(ctx->backend, p);
}

/* ------------------------------------------------------------------------ */
/*
 * Perspective-correct texturing (jbi_set_texture_mapping, prim.h):
 * the depths of a textured polygon's vertices, from the same precise
 * records as their positions.
 */

/* The depth of a vertex from its record, or 0 without a record, at z <= 0
   or where the polygon lookup found no record of its own. Unlike
   precise_xy_from this takes records more than a pixel from the word's
   position (the lookup allows up to two): the depth is the vertex's either
   way, and precise_xy rejects records that lie just over a pixel right of
   or below their keys. */
static int persp_depth(const jbi_precise_vertex *pv, int hit)
{
    return hit && pv->z > 0 ? pv->z : 0;
}

/* Marks the triangle (i0, i1, i2) perspective-correct if all three
   vertices have depths; otherwise it stays affine. */
static void persp_triangle(jbi_prim *p, const int *z, const uint8_t (*pa)[5], int i0, int i1,
                           int i2)
{
    p->flags &= (uint8_t)~JBI_F_PERSP;
    if (z[i0] <= 0 || z[i1] <= 0 || z[i2] <= 0)
        return;
    p->flags |= JBI_F_PERSP;
    p->u.tri.z[0] = z[i0];
    p->u.tri.z[1] = z[i1];
    p->u.tri.z[2] = z[i2];
    memcpy(p->u.tri.pattr[0], pa[i0], 5);
    memcpy(p->u.tri.pattr[1], pa[i1], 5);
    memcpy(p->u.tri.pattr[2], pa[i2], 5);
}

/* The perspective pass's attributes of the polygon's vertices: given ones,
   or the vertices' own. */
static void persp_attrs(const jbi_vertex *v, int verts, const jbi_subpixel *sp,
                        uint8_t (*pa)[5])
{
    int i;

    for (i = 0; i < verts; ++i) {
        if (sp != NULL && sp->has_pattr) {
            memcpy(pa[i], sp->pattr[i], 5);
        } else {
            pa[i][0] = (uint8_t)v[i].u;
            pa[i][1] = (uint8_t)v[i].v;
            pa[i][2] = (uint8_t)v[i].r;
            pa[i][3] = (uint8_t)v[i].g;
            pa[i][4] = (uint8_t)v[i].b;
        }
    }
}

/* ------------------------------------------------------------------------ */

static int polygon_words(uint32_t cmd)
{
    int verts = (cmd & 0x08) ? 4 : 3;
    int per = 1 + ((cmd & 0x04) ? 1 : 0) + ((cmd & 0x10) ? 1 : 0);
    /* The first vertex's colour is in the command word. */
    return verts * per + ((cmd & 0x10) ? 0 : 1);
}

static void draw_polygon(jb_context *ctx, const uint32_t *w, uint32_t cmd,
                         const jbi_subpixel *sp)
{
    jbi_draw_state *d = &ctx->draw;
    jbi_vertex v[4];
    int qx[4], qy[4];
    uint32_t xyw[4]; /* vertex words, for the precise lookup */
    jbi_precise_vertex pv[4] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
    unsigned hits = 0;
    int use;
    int z[4];
    uint8_t pa[4][5];
    jbi_prim prim;
    int verts = (cmd & 0x08) ? 4 : 3;
    int gouraud = (cmd & 0x10) != 0, textured = (cmd & 0x04) != 0;
    int flags = 0, clut_x = 0, clut_y = 0, i;
    const uint32_t *p = w;

    if (gouraud)
        flags |= JBI_F_GOURAUD;
    if (textured)
        flags |= JBI_F_TEXTURED | ((cmd & 0x01) ? JBI_F_RAW : 0);
    if (cmd & 0x02)
        flags |= JBI_F_SEMI;
    if (sp != NULL)
        flags |= (int)(sp->skip & (JBI_F_NO_1X | JBI_F_NO_SCALED));

    for (i = 0; i < verts; ++i) {
        uint32_t xy;

        if (i == 0 || gouraud)
            decode_rgb(*p++, &v[i]);
        else
            decode_rgb(w[0], &v[i]);
        xy = *p++;
        xyw[i] = xy;
        decode_xy(d, xy, &v[i]);
        v[i].u = v[i].v = 0;
        if (textured) {
            uint32_t uv = *p++;
            v[i].u = (int)(uv & 0xff);
            v[i].v = (int)((uv >> 8) & 0xff);
            if (i == 0) {
                clut_x = (int)((uv >> 16) & 0x3f) * 16;
                clut_y = (int)((uv >> 22) & 0x1ff);
            } else if (i == 1) {
                set_tpage_attr(d, uv >> 16);
            }
        }
    }
    /* Positions and depths from the same records: the polygon's vertices
       looked up together (lookup_poly), so that a key another object also
       projected to gives this polygon's own vertex. Given data
       (jbi_gp0_subpixel) takes the place of the lookup: its positions
       where known, its depths wherever they are above 0. */
    if (sp != NULL) {
        for (i = 0; i < verts; ++i)
            pv[i] = sp->v[i];
        hits = sp->known;
        use = given_positions(ctx, sp);
    } else {
        use = precise_positions(ctx);
        if (ctx->has_precise && (use || (textured && ctx->texture_perspective)))
            hits = ctx->precise.lookup_poly(ctx->precise.user, xyw, verts, pv);
    }
    for (i = 0; i < verts; ++i)
        precise_xy_from(ctx, pv[i], (hits >> i) & 1, use, &v[i], &qx[i], &qy[i]);
    prim_state(d, &prim, JBI_PRIM_TRI, flags, clut_x, clut_y);
    if (textured && ctx->texture_perspective) {
        for (i = 0; i < verts; ++i)
            z[i] = persp_depth(&pv[i], sp != NULL || ((hits >> i) & 1));
        persp_attrs(v, verts, sp, pa);
        persp_triangle(&prim, z, (const uint8_t(*)[5])pa, 0, 1, 2);
    }
    submit_triangle(ctx, &prim, v, qx, qy, 0, 1, 2);
    if (verts == 4) {
        if (textured && ctx->texture_perspective)
            persp_triangle(&prim, z, (const uint8_t(*)[5])pa, 1, 2, 3);
        submit_triangle(ctx, &prim, v, qx, qy, 1, 2, 3);
    }
}

static int is_polyline_end(uint32_t w)
{
    return (w & 0xf000f000u) == 0x50005000u;
}

/*
 * Words a line command uses, including a polyline's terminator. The
 * terminator is looked for where the next vertex would start, after at
 * least two vertices; a polyline without one ends with the packet.
 */
static int line_words(const uint32_t *w, int count)
{
    uint32_t cmd = w[0] >> 24;
    int per = (cmd & 0x10) ? 2 : 1;
    int i;

    if (!(cmd & 0x08))
        return 1 + per * 2 - ((cmd & 0x10) ? 1 : 0);
    for (i = 2 + per; i < count; i += per) {
        if (is_polyline_end(w[i]))
            return i + 1;
    }
    return count;
}

static void submit_line(jb_context *ctx, const jbi_vertex *a, const jbi_vertex *b, const int *q,
                        int flags)
{
    jbi_prim prim;

    prim_state(&ctx->draw, &prim, JBI_PRIM_LINE, flags, 0, 0);
    prim.u.line.v[0] = *a;
    prim.u.line.v[1] = *b;
    prim.u.line.qx[0] = q[0];
    prim.u.line.qy[0] = q[1];
    prim.u.line.qx[1] = q[2];
    prim.u.line.qy[1] = q[3];
    {
        /* As for triangles; a scaled line reaches one row further down. */
        int y0 = (a->y < b->y ? a->y : b->y) - 1, y1 = (a->y > b->y ? a->y : b->y) + 1;
        int q0 = jbi_q_pixel(q[1] < q[3] ? q[1] : q[3]);
        int q1 = jbi_q_pixel(q[1] > q[3] ? q[1] : q[3]) + 1;

        prim_rows(&prim, q0 < y0 ? q0 : y0, q1 > y1 ? q1 : y1);
    }
    ctx->backend->submit(ctx->backend, &prim);
}

static void draw_lines(jb_context *ctx, const uint32_t *w, int nwords, uint32_t cmd,
                       const jbi_subpixel *sp)
{
    int gouraud = (cmd & 0x10) != 0;
    int flags = (gouraud ? JBI_F_GOURAUD : 0) | ((cmd & 0x02) ? JBI_F_SEMI : 0);
    jbi_vertex a, b;
    int q[4]; /* a's and b's precise positions */
    const uint32_t *p = w + 1, *end = w + nwords;
    uint32_t xy;
    int verts = 1;

    decode_rgb(w[0], &a);
    xy = *p++;
    decode_xy(&ctx->draw, xy, &a);
    /* Given data is for a single line's two vertices; a polyline's are
       looked up. */
    if (cmd & 0x08)
        sp = NULL;
    line_xy(ctx, sp, 0, xy, &a, &q[0], &q[1]);
    for (;;) {
        /* As in line_words: the terminator ends a polyline from its third
           vertex on; the second is always a vertex. */
        if (p >= end || ((cmd & 0x08) && verts >= 2 && is_polyline_end(*p)))
            break;
        if (gouraud) {
            decode_rgb(*p++, &b);
            if (p >= end)
                break;
        } else {
            decode_rgb(w[0], &b);
        }
        xy = *p++;
        decode_xy(&ctx->draw, xy, &b);
        line_xy(ctx, sp, 1, xy, &b, &q[2], &q[3]);
        submit_line(ctx, &a, &b, q, flags);
        verts = 2;
        a = b;
        q[0] = q[2];
        q[1] = q[3];
        if (!(cmd & 0x08))
            break;
    }
}

static int rect_words(uint32_t cmd)
{
    return 2 + ((cmd & 0x04) ? 1 : 0) + (((cmd >> 3) & 3) == 0 ? 1 : 0);
}

/* Rectangles keep integer positions in the scaled pass: they are mostly
   screen-space sprites and text that the program places itself. */
static void draw_rect(jb_context *ctx, const uint32_t *w, uint32_t cmd)
{
    const jbi_draw_state *d = &ctx->draw;
    static const int fixed_size[4] = {0, 1, 8, 16};
    int size = (int)((cmd >> 3) & 3);
    int flags = 0, x, y, wd, ht, u = 0, v = 0, clut_x = 0, clut_y = 0;
    const uint32_t *p = w + 2;
    jbi_prim prim;

    x = sext11(w[1]) + d->off_x;
    y = sext11(w[1] >> 16) + d->off_y;
    if (cmd & 0x04) {
        uint32_t uv = *p++;
        flags |= JBI_F_TEXTURED | ((cmd & 0x01) ? JBI_F_RAW : 0);
        u = (int)(uv & 0xff);
        v = (int)((uv >> 8) & 0xff);
        clut_x = (int)((uv >> 16) & 0x3f) * 16;
        clut_y = (int)((uv >> 22) & 0x1ff);
    }
    if (cmd & 0x02)
        flags |= JBI_F_SEMI;
    if (size == 0) {
        wd = (int)(*p & 0x3ff);
        ht = (int)((*p >> 16) & 0x1ff);
    } else {
        wd = ht = fixed_size[size];
    }
    prim_state(d, &prim, JBI_PRIM_RECT, flags, clut_x, clut_y);
    prim.u.rect.x = x;
    prim.u.rect.y = y;
    prim.u.rect.w = wd;
    prim.u.rect.h = ht;
    prim.u.rect.r = (int)(w[0] & 0xff);
    prim.u.rect.g = (int)((w[0] >> 8) & 0xff);
    prim.u.rect.b = (int)((w[0] >> 16) & 0xff);
    /* With the x-flip the console starts one texel to the right of u0 and
       steps left: ps1-tests' texture-flip dump shows texel (1 - x) & 255
       for the x-th pixel of every row with u0 = 0. Only u0 = 0 is tested;
       u0 + 1 is the simplest rule that fits (docs/conformance.md,
       "Assumptions beyond the evidence"). The y-flip starts at v0. */
    prim.u.rect.u = u + (d->rect_flip_x && (flags & JBI_F_TEXTURED) ? 1 : 0);
    prim.u.rect.v = v;
    prim_rows(&prim, y, y + ht - 1);
    ctx->backend->submit(ctx->backend, &prim);
}

/* ------------------------------------------------------------------------ */
/* VRAM transfers */

/*
 * Fill: not clipped, not offset, ignores the mask settings. The hardware
 * rounds x down and the width up to multiples of 16 pixels. Queued like a
 * primitive (an isbg DR_ENV fills inside a primitive list).
 */
void jbi_fill(jb_context *ctx, int x, int y, int w, int h, uint32_t rgb24)
{
    uint16_t color = (uint16_t)(((rgb24 >> 3) & 0x1f) | ((rgb24 >> 11) & 0x1f) << 5 |
                                ((rgb24 >> 19) & 0x1f) << 10);
    int fx = x & 0x3f0, fy = y & 0x1ff, fw = ((w & 0x3ff) + 15) & ~15, fh = h & 0x1ff;

    if (fw == 0 || fh == 0)
        return;
    ctx->backend->fill(ctx->backend, fx, fy, fw, fh, color);
}

/* Transfer sizes: 0 means the maximum, as on hardware. */
static void transfer_size(int *w, int *h)
{
    *w = ((*w - 1) & 0x3ff) + 1;
    *h = ((*h - 1) & 0x1ff) + 1;
}

static jbi_mask mask_of(const jbi_draw_state *d)
{
    jbi_mask m;

    m.check = d->check_mask;
    m.set = d->set_mask;
    return m;
}

/* CPU to VRAM writes honour the mask settings. */
void jbi_load(jb_context *ctx, int x, int y, int w, int h, const uint16_t *src)
{
    transfer_size(&w, &h);
    ctx->backend->load(ctx->backend, x & (JBI_VRAM_W - 1), y & (JBI_VRAM_H - 1), w, h, src,
                       mask_of(&ctx->draw));
}

/*
 * A load's data inside a GP0 packet: a little-endian stream of 16-bit
 * pixels in the words (pixel k is the low half of word k / 2 when k is
 * even, the high half when odd), row-major, the last word padded. Handed
 * to the backend a row at a time, which writes the same pixels in the same
 * order as one call for the whole rectangle.
 */
static void load_words(jb_context *ctx, int x, int y, int w, int h, const uint32_t *words)
{
    uint16_t row[JBI_VRAM_W];
    jbi_mask mask = mask_of(&ctx->draw);
    size_t k = 0;
    int i, j;

    for (j = 0; j < h; ++j) {
        for (i = 0; i < w; ++i, ++k)
            row[i] = (uint16_t)(words[k >> 1] >> ((k & 1) * 16));
        ctx->backend->load(ctx->backend, x, (y + j) & (JBI_VRAM_H - 1), w, 1, row, mask);
    }
}

/* Above 1x the software backend reads back the scaled VRAM, averaged
   (src/soft/transfer.c). */
void jbi_store(jb_context *ctx, int x, int y, int w, int h, uint16_t *dst)
{
    transfer_size(&w, &h);
    ctx->backend->store(ctx->backend, x & (JBI_VRAM_W - 1), y & (JBI_VRAM_H - 1), w, h, dst);
}

void jbi_store_1x(jb_context *ctx, int x, int y, int w, int h, uint16_t *dst)
{
    transfer_size(&w, &h);
    ctx->backend->store_1x(ctx->backend, x & (JBI_VRAM_W - 1), y & (JBI_VRAM_H - 1), w, h, dst);
}

void jbi_copy(jb_context *ctx, int sx, int sy, int dx, int dy, int w, int h)
{
    transfer_size(&w, &h);
    ctx->backend->copy(ctx->backend, sx & (JBI_VRAM_W - 1), sy & (JBI_VRAM_H - 1),
                       dx & (JBI_VRAM_W - 1), dy & (JBI_VRAM_H - 1), w, h, mask_of(&ctx->draw));
}

/* ------------------------------------------------------------------------ */

static int run(jb_context *ctx, const uint32_t *words, int count, const jbi_subpixel *sp);

int jbi_gp0(jb_context *ctx, const uint32_t *words, int count)
{
    return run(ctx, words, count, NULL);
}

int jbi_gp0_subpixel(jb_context *ctx, const uint32_t *words, int count, const jbi_subpixel *sp)
{
    return run(ctx, words, count, sp);
}

static int run(jb_context *ctx, const uint32_t *words, int count, const jbi_subpixel *sp)
{
    jbi_draw_state *d = &ctx->draw;
    int pos = 0;

    while (pos < count) {
        const uint32_t *w = words + pos;
        uint32_t cmd = w[0] >> 24;
        int n;

        switch (cmd >> 5) {
        case 1: /* 20h-3Fh polygon */
            n = polygon_words(cmd);
            if (pos + n > count)
                return count;
            draw_polygon(ctx, w, cmd, sp);
            break;
        case 2: /* 40h-5Fh line */
            n = line_words(w, count - pos);
            if (pos + n > count)
                return count;
            draw_lines(ctx, w, n, cmd, sp);
            break;
        case 3: /* 60h-7Fh rectangle */
            n = rect_words(cmd);
            if (pos + n > count)
                return count;
            draw_rect(ctx, w, cmd);
            break;
        case 4: /* 80h-9Fh VRAM to VRAM */
            n = 4;
            if (pos + n > count)
                return count;
            jbi_copy(ctx, (int)(w[1] & 0x3ff), (int)((w[1] >> 16) & 0x1ff),
                     (int)(w[2] & 0x3ff), (int)((w[2] >> 16) & 0x1ff),
                     (int)(w[3] & 0xffff), (int)(w[3] >> 16));
            break;
        case 5: { /* A0h-BFh CPU to VRAM, data follows */
            int wd, ht;
            if (pos + 3 > count)
                return count;
            wd = (int)(w[2] & 0xffff);
            ht = (int)(w[2] >> 16);
            transfer_size(&wd, &ht);
            n = 3 + (wd * ht + 1) / 2;
            if (pos + n > count)
                return count;
            load_words(ctx, (int)(w[1] & 0x3ff), (int)((w[1] >> 16) & 0x1ff), wd, ht, w + 3);
            break;
        }
        case 6: /* C0h-DFh VRAM to CPU: nothing reads the data here */
            n = 3;
            if (pos + n > count)
                return count;
            break;
        case 7: /* E0h-FFh environment */
            n = 1;
            switch (cmd) {
            case 0xe1:
                set_draw_mode(d, w[0]);
                break;
            case 0xe2:
                set_tex_window(d, w[0]);
                break;
            case 0xe3:
                d->clip_x0 = (int)(w[0] & 0x3ff);
                d->clip_y0 = (int)((w[0] >> 10) & 0x1ff);
                break;
            case 0xe4:
                d->clip_x1 = (int)(w[0] & 0x3ff);
                d->clip_y1 = (int)((w[0] >> 10) & 0x1ff);
                break;
            case 0xe5:
                d->off_x = sext11(w[0]);
                d->off_y = sext11(w[0] >> 11);
                break;
            case 0xe6:
                d->set_mask = (w[0] & 1) ? 0x8000 : 0;
                d->check_mask = (int)((w[0] >> 1) & 1);
                break;
            default:
                break;
            }
            break;
        default: /* 00h-1Fh */
            n = 1;
            if (cmd == 0x01)
                jbi_clear_cache(ctx);
            if (cmd == 0x02) {
                n = 3;
                if (pos + n > count)
                    return count;
                jbi_fill(ctx, (int)(w[1] & 0xffff), (int)(w[1] >> 16),
                         (int)(w[2] & 0xffff), (int)(w[2] >> 16), w[0] & 0xffffff);
            }
            break;
        }
        pos += n;
    }
    return pos;
}

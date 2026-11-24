/*
 * jb_prim_* and the drawing-state calls (include/jitterbug.h, Drawing).
 *
 * Every call checks all of its arguments first (the range rule: each must
 * fit the field of the GP0 words that carries it), then builds those words
 * and hands each command to jbi_api_command, which is what jb_gp0 does with
 * a complete command. So a call draws exactly what its words draw through
 * jb_gp0, and a refused call sends nothing.
 *
 * The checks and the words are separate functions (jbi_check_*,
 * jbi_emit_*, api.h), which the recorded primitives use too (record.c): a
 * record is checked when it is recorded and drawn later by the same
 * emitter, so it draws what the call would have drawn then. An emitter may
 * be given sub-pixel data for its polygon or line, which takes the place
 * of the precise table (jbi_gp0_subpixel, src/core/context.h).
 *
 * The draw mode's word, GP0(E1h), is rebuilt from the context's drawing
 * state (draw_mode_word) where a call must change part of it: the blend
 * mode of an untextured primitive that blends, and the page, depth, blend
 * mode and flips of a textured rectangle, all of which the console takes
 * from the draw mode. The state keeps every E1h bit that has an effect
 * (0-10, 12, 13), so the rebuilt word changes nothing else. Bit 11 is
 * different: an E1h stores it (GPUSTAT bit 15) only while GP1(09h)'s gate
 * is set, and stores 0 while it is shut whatever the word's bit 11, so no
 * word keeps the stored bit in every case. The rebuilt word has bit 11
 * clear and send_draw_mode puts the stored bit back after it: a
 * helper-only update, unlike the same word sent through jb_gp0, which
 * stores 0 (include/jitterbug.h, jb_gp1 09h). A textured jb_prim_poly's
 * page is sent as it is, bit 11 clear, so it stores 0 as through jb_gp0.
 * jb_set_dither changes only the dither field and sends nothing.
 *
 * Nothing here allocates.
 */
#include <stdio.h>

#include "api/api.h"

enum { MAX_WORDS = 12 }; /* a textured Gouraud quad */

typedef struct {
    jb_context *jb;
    const char *fn;
    jb_result r; /* JB_OK until a check fails; the first failure is kept */
} checker;

static void range(checker *c, const char *what, int index, long long v, long long lo,
                  long long hi)
{
    char name[48];

    if (c->r != JB_OK || (v >= lo && v <= hi))
        return;
    if (index >= 0)
        snprintf(name, sizeof name, "v[%d].%s", index, what);
    else
        snprintf(name, sizeof name, "%s", what);
    c->r = jbi_api_fail(c->jb, JB_ERROR_INVALID_ARGUMENT, "%s: %s %lld is outside %lld..%lld",
                        c->fn, name, v, lo, hi);
}

static void multiple(checker *c, const char *what, int v, int step)
{
    if (c->r != JB_OK || v % step == 0)
        return;
    c->r = jbi_api_fail(c->jb, JB_ERROR_INVALID_ARGUMENT, "%s: %s %d is not a multiple of %d",
                        c->fn, what, v, step);
}

static void position(checker *c, int index, int x, int y)
{
    range(c, "x", index, x, -1024, 1023);
    range(c, "y", index, y, -1024, 1023);
}

static void color(checker *c, int index, uint32_t rgb)
{
    char name[24];

    if (c->r != JB_OK || rgb <= 0xffffffu)
        return;
    if (index >= 0)
        snprintf(name, sizeof name, "v[%d].color", index);
    else
        snprintf(name, sizeof name, "color");
    c->r = jbi_api_fail(c->jb, JB_ERROR_INVALID_ARGUMENT, "%s: %s 0x%lx is above 0xffffff",
                        c->fn, name, (unsigned long)rgb);
}

static void texel(checker *c, int index, int u, int v)
{
    range(c, "u", index, u, 0, 255);
    range(c, "v", index, v, 0, 255);
}

static void blend(checker *c, int b)
{
    range(c, "blend", -1, b, JB_BLEND_NONE, JB_BLEND_QUARTER);
}

static void texture(checker *c, const jb_texture *t)
{
    if (t == NULL)
        return;
    range(c, "texture->page_x", -1, t->page_x, 0, 960);
    multiple(c, "texture->page_x", t->page_x, 64);
    if (c->r == JB_OK && t->page_y != 0 && t->page_y != 256)
        c->r = jbi_api_fail(c->jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: texture->page_y %d is neither 0 nor 256", c->fn, t->page_y);
    if (c->r == JB_OK && t->depth != JB_TEX_4BIT && t->depth != JB_TEX_8BIT &&
        t->depth != JB_TEX_15BIT)
        c->r = jbi_api_fail(c->jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: texture->depth %d is not 4, 8 or 15", c->fn, t->depth);
    if (t->depth == JB_TEX_15BIT)
        return; /* no palette: the CLUT position is not used */
    range(c, "texture->clut_x", -1, t->clut_x, 0, 1008);
    multiple(c, "texture->clut_x", t->clut_x, 16);
    range(c, "texture->clut_y", -1, t->clut_y, 0, 511);
}

/* The CLUT attribute of a textured primitive's first texel word. */
static uint32_t clut_attr(const jb_texture *t)
{
    if (t->depth == JB_TEX_15BIT)
        return 0;
    return (uint32_t)(t->clut_x / 16) | (uint32_t)t->clut_y << 6;
}

/* ------------------------------------------------------------------------ */
/* Words */

static uint32_t pos_word(int x, int y)
{
    return ((uint32_t)x & 0xffffu) | ((uint32_t)y & 0xffffu) << 16;
}

static uint32_t depth_code(int depth)
{
    return depth == JB_TEX_4BIT ? 0u : depth == JB_TEX_8BIT ? 1u : 2u;
}

/* The console's semi-transparency mode of a blend that is not NONE. */
static uint32_t semi_mode(int b)
{
    return (uint32_t)(b - 1);
}

/* GP0(E1h) as the drawing state holds it, bit 11 clear (send_draw_mode). */
static uint32_t draw_mode_word(const jbi_draw_state *d)
{
    return 0xe1000000u | (uint32_t)(d->tpage_x / 64) | (uint32_t)(d->tpage_y / 256) << 4 |
           (uint32_t)d->semi_mode << 5 | (uint32_t)d->tex_depth << 7 |
           (uint32_t)d->dither << 9 | (uint32_t)d->draw_display << 10 |
           (uint32_t)d->rect_flip_x << 12 | (uint32_t)d->rect_flip_y << 13;
}

static void send(jb_context *jb, const uint32_t *w, int n)
{
    jbi_api_command(jb, w, n);
}

/* A polygon or single line, with sub-pixel data or (sp NULL) the precise
   table. Either way the words reach the core decoder as jbi_api_command
   hands them on: these commands touch none of the stream's own state. */
static void send_sub(jb_context *jb, const uint32_t *w, int n, const jbi_subpixel *sp)
{
    if (sp == NULL)
        send(jb, w, n);
    else
        jbi_gp0_subpixel(jb, w, n, sp);
}

/* A rebuilt GP0(E1h) (bit 11 clear), then the bit 11 the state stored put
   back, which the word through jb_gp0 would not keep (the top). */
static void send_draw_mode(jb_context *jb, uint32_t e1)
{
    int kept = jb->draw.tex_disable;

    send(jb, &e1, 1);
    jb->draw.tex_disable = kept;
}

/* An untextured primitive that blends: the draw mode with its mode. */
static void send_blend_mode(jb_context *jb, int b)
{
    uint32_t e1;

    if (b == JB_BLEND_NONE)
        return;
    e1 = (draw_mode_word(&jb->draw) & ~(3u << 5)) | semi_mode(b) << 5;
    send_draw_mode(jb, e1);
}

/* ------------------------------------------------------------------------ */
/* Primitives */

jb_result jbi_check_poly(jb_context *jb, const char *fn, const jb_poly *p, int positions)
{
    checker c;
    int i, gouraud, textured, raw;

    c.jb = jb;
    c.fn = fn;
    c.r = JB_OK;
    if (p->vertices != 3 && p->vertices != 4)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: vertices %d is neither 3 nor 4",
                            fn, p->vertices);
    gouraud = p->gouraud != 0;
    textured = p->texture != NULL;
    raw = textured && p->raw; /* the colours are then not used: sent as 0 */
    blend(&c, p->blend);
    texture(&c, p->texture);
    for (i = 0; i < p->vertices; ++i) {
        if (positions)
            position(&c, i, p->v[i].x, p->v[i].y);
        if ((i == 0 || gouraud) && !raw)
            color(&c, i, p->v[i].color);
        if (textured)
            texel(&c, i, p->v[i].u, p->v[i].v);
    }
    return c.r;
}

void jbi_emit_poly(jb_context *jb, const jb_poly *p, const jbi_subpixel *sp)
{
    uint32_t w[MAX_WORDS], op;
    int i, n = 0, verts = p->vertices, gouraud = p->gouraud != 0;
    int textured = p->texture != NULL, raw = textured && p->raw;

    if (!textured)
        send_blend_mode(jb, p->blend);
    op = 0x20u | (gouraud ? 0x10u : 0) | (verts == 4 ? 0x08u : 0) | (textured ? 0x04u : 0) |
         (p->blend != JB_BLEND_NONE ? 0x02u : 0) | (raw ? 0x01u : 0);
    w[n++] = op << 24 | (raw ? 0 : p->v[0].color);
    for (i = 0; i < verts; ++i) {
        if (gouraud && i > 0)
            w[n++] = raw ? 0 : p->v[i].color;
        w[n++] = pos_word(p->v[i].x, p->v[i].y);
        if (textured) {
            const jb_texture *t = p->texture;
            uint32_t attr = 0;

            if (i == 0) {
                attr = clut_attr(t);
            } else if (i == 1) {
                uint32_t mode = p->blend != JB_BLEND_NONE ? semi_mode(p->blend)
                                                          : (uint32_t)jb->draw.semi_mode;

                attr = (uint32_t)(t->page_x / 64) | (uint32_t)(t->page_y / 256) << 4 |
                       mode << 5 | depth_code(t->depth) << 7;
            }
            w[n++] = (uint32_t)p->v[i].u | (uint32_t)p->v[i].v << 8 | attr << 16;
        }
    }
    send_sub(jb, w, n, sp);
}

jb_result jb_prim_poly(jb_context *jb, const jb_poly *p)
{
    jb_result r;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (p == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_prim_poly: poly is NULL");
    r = jbi_check_poly(jb, "jb_prim_poly", p, 1);
    if (r != JB_OK)
        return r;
    jbi_emit_poly(jb, p, NULL);
    return JB_OK;
}

/* The line command from a to b. */
static void send_segment(jb_context *jb, int gouraud, int b, const jb_vertex *a,
                         const jb_vertex *z, const jbi_subpixel *sp)
{
    uint32_t w[4];
    int n = 0;
    uint32_t op = 0x40u | (gouraud ? 0x10u : 0) | (b != JB_BLEND_NONE ? 0x02u : 0);

    w[n++] = op << 24 | a->color;
    w[n++] = pos_word(a->x, a->y);
    if (gouraud)
        w[n++] = z->color;
    w[n++] = pos_word(z->x, z->y);
    send_sub(jb, w, n, sp);
}

jb_result jbi_check_line(jb_context *jb, const char *fn, int gouraud, int b, const jb_vertex *v,
                         size_t count, int positions)
{
    checker c;
    size_t i;

    c.jb = jb;
    c.fn = fn;
    c.r = JB_OK;
    gouraud = gouraud != 0;
    blend(&c, b);
    for (i = 0; i < count && c.r == JB_OK; ++i) {
        int k = i < 0x7fffffff ? (int)i : 0x7fffffff;

        if (positions)
            position(&c, k, v[i].x, v[i].y);
        if (i == 0 || gouraud)
            color(&c, k, v[i].color);
    }
    return c.r;
}

void jbi_emit_line(jb_context *jb, const jb_line *l, const jbi_subpixel *sp)
{
    send_blend_mode(jb, l->blend);
    send_segment(jb, l->gouraud != 0, l->blend, &l->v[0], &l->v[1], sp);
}

jb_result jb_prim_line(jb_context *jb, const jb_line *l)
{
    jb_result r;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (l == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_prim_line: line is NULL");
    r = jbi_check_line(jb, "jb_prim_line", l->gouraud, l->blend, l->v, 2, 1);
    if (r != JB_OK)
        return r;
    jbi_emit_line(jb, l, NULL);
    return JB_OK;
}

jb_result jbi_check_polyline(jb_context *jb, const char *fn, const jb_polyline *l, int positions)
{
    if (l->v == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: line->v is NULL", fn);
    if (l->count < 2)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: count %zu is below 2", fn,
                            l->count);
    return jbi_check_line(jb, fn, l->gouraud, l->blend, l->v, l->count, positions);
}

jb_line jbi_polyline_segment(const jb_polyline *l, size_t i)
{
    jb_line s;

    /* As jb_gp0 splits a polyline (stream.c): each segment's command
       carries the colour its first vertex has, which for a flat line is
       the first vertex's. */
    s.gouraud = l->gouraud != 0;
    s.blend = l->blend;
    s.v[0] = l->v[i];
    s.v[1] = l->v[i + 1];
    if (!s.gouraud)
        s.v[0].color = s.v[1].color = l->v[0].color;
    return s;
}

jb_result jb_prim_polyline(jb_context *jb, const jb_polyline *l)
{
    jb_result r;
    size_t i;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (l == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_prim_polyline: line is NULL");
    r = jbi_check_polyline(jb, "jb_prim_polyline", l, 1);
    if (r != JB_OK)
        return r;
    send_blend_mode(jb, l->blend);
    for (i = 0; i + 1 < l->count; ++i) {
        jb_line s = jbi_polyline_segment(l, i);

        send_segment(jb, s.gouraud, s.blend, &s.v[0], &s.v[1], NULL);
    }
    return JB_OK;
}

jb_result jbi_check_rect(jb_context *jb, const char *fn, const jb_rect *r, int positions)
{
    checker c;
    int textured = r->texture != NULL, raw = textured && r->raw;

    c.jb = jb;
    c.fn = fn;
    c.r = JB_OK;
    if (positions) {
        position(&c, -1, r->x, r->y);
        range(&c, "w", -1, r->w, 0, 1023);
        range(&c, "h", -1, r->h, 0, 511);
    } else {
        range(&c, "w", -1, r->w, 0, 0x7fffffff);
        range(&c, "h", -1, r->h, 0, 0x7fffffff);
    }
    if (!raw)
        color(&c, -1, r->color);
    blend(&c, r->blend);
    texture(&c, r->texture);
    if (textured)
        texel(&c, -1, r->u, r->v);
    return c.r;
}

void jbi_emit_rect(jb_context *jb, const jb_rect *r)
{
    uint32_t w[4], op, size;
    int n = 0, textured = r->texture != NULL, raw = textured && r->raw;

    if (textured) {
        const jb_texture *t = r->texture;
        uint32_t e1 = draw_mode_word(&jb->draw) & ~(0x1ffu | 3u << 12);
        uint32_t mode = r->blend != JB_BLEND_NONE ? semi_mode(r->blend)
                                                  : (uint32_t)jb->draw.semi_mode;

        e1 |= (uint32_t)(t->page_x / 64) | (uint32_t)(t->page_y / 256) << 4 | mode << 5 |
              depth_code(t->depth) << 7 | (r->flip_x ? 1u : 0) << 12 |
              (r->flip_y ? 1u : 0) << 13;
        send_draw_mode(jb, e1);
    } else {
        send_blend_mode(jb, r->blend);
    }
    if (r->w == r->h && r->w == 1)
        size = 1;
    else if (r->w == r->h && r->w == 8)
        size = 2;
    else if (r->w == r->h && r->w == 16)
        size = 3;
    else
        size = 0;
    op = 0x60u | size << 3 | (textured ? 0x04u : 0) | (r->blend != JB_BLEND_NONE ? 0x02u : 0) |
         (raw ? 0x01u : 0);
    w[n++] = op << 24 | (raw ? 0 : r->color);
    w[n++] = pos_word(r->x, r->y);
    if (textured)
        w[n++] = (uint32_t)r->u | (uint32_t)r->v << 8 | clut_attr(r->texture) << 16;
    if (size == 0)
        w[n++] = pos_word(r->w, r->h);
    send(jb, w, n);
}

jb_result jb_prim_rect(jb_context *jb, const jb_rect *r)
{
    jb_result res;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (r == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_prim_rect: rect is NULL");
    res = jbi_check_rect(jb, "jb_prim_rect", r, 1);
    if (res != JB_OK)
        return res;
    jbi_emit_rect(jb, r);
    return JB_OK;
}

/* ------------------------------------------------------------------------ */
/* Drawing state */

jb_result jb_set_draw_area(jb_context *jb, int left, int top, int right, int bottom)
{
    checker c = {jb, "jb_set_draw_area", JB_OK};
    uint32_t w[2];

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    range(&c, "left", -1, left, 0, 1023);
    range(&c, "top", -1, top, 0, 511);
    range(&c, "right", -1, right, 0, 1023);
    range(&c, "bottom", -1, bottom, 0, 511);
    if (c.r != JB_OK)
        return c.r;
    w[0] = 0xe3000000u | (uint32_t)left | (uint32_t)top << 10;
    w[1] = 0xe4000000u | (uint32_t)right | (uint32_t)bottom << 10;
    send(jb, &w[0], 1);
    send(jb, &w[1], 1);
    return JB_OK;
}

jb_result jb_set_draw_offset(jb_context *jb, int x, int y)
{
    checker c = {jb, "jb_set_draw_offset", JB_OK};
    uint32_t w;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    range(&c, "x", -1, x, -1024, 1023);
    range(&c, "y", -1, y, -1024, 1023);
    if (c.r != JB_OK)
        return c.r;
    w = 0xe5000000u | ((uint32_t)x & 0x7ffu) | ((uint32_t)y & 0x7ffu) << 11;
    send(jb, &w, 1);
    return JB_OK;
}

jb_result jb_set_texture_window(jb_context *jb, int mask_x, int mask_y, int offset_x,
                                int offset_y)
{
    checker c = {jb, "jb_set_texture_window", JB_OK};
    uint32_t w;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    range(&c, "mask_x", -1, mask_x, 0, 31);
    range(&c, "mask_y", -1, mask_y, 0, 31);
    range(&c, "offset_x", -1, offset_x, 0, 31);
    range(&c, "offset_y", -1, offset_y, 0, 31);
    if (c.r != JB_OK)
        return c.r;
    w = 0xe2000000u | (uint32_t)mask_x | (uint32_t)mask_y << 5 | (uint32_t)offset_x << 10 |
        (uint32_t)offset_y << 15;
    send(jb, &w, 1);
    return JB_OK;
}

jb_result jb_set_mask(jb_context *jb, int set, int check)
{
    uint32_t w;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    w = 0xe6000000u | (set ? 1u : 0) | (check ? 2u : 0);
    send(jb, &w, 1);
    return JB_OK;
}

/* Only the field: a GP0(E1h) would also write the stored bit 11 (the
   top). */
jb_result jb_set_dither(jb_context *jb, int on)
{
    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    jb->draw.dither = on != 0;
    return JB_OK;
}

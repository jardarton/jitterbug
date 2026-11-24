/*
 * Sprites and text (include/jitterbug.h, Sprites and text): 2D draws over
 * the frame's records (frame.h) and the textures placed in VRAM
 * (texture.h).
 *
 * A sprite shows a rectangle of a texture's texels (the source: u, v
 * within the texture, src_w x src_h) in a rectangle of the frame (x, y,
 * w x h):
 *   - 1:1 (the same size) it is a textured rectangle record, the
 *     jb_prim_rect a program would send for it: u, v the source's page
 *     coordinates (texture u + u, v + v), shifted for the console's
 *     mirroring when flipped (flip_x: u + src_w - 2, since a mirrored
 *     rectangle's first pixel takes texel u + 1; flip_y: v + src_h - 1).
 *     jbi_frame_add_rect clips it exactly and cuts what the GPU cannot take.
 *   - scaled it is a textured quad record whose corners carry the source's
 *     first and last texels (u and u + src_w - 1, swapped when flipped),
 *     at (x, y) and (x + w, y + h), so that no pixel samples past the
 *     source; jbi_frame_add_poly clips and splits it.
 * Either way a repeat texture's records carry the window that tiles it
 * (jbi_record_window), so a source longer than the texture repeats it.
 *
 * Text is one 8 x 8 rectangle record a visible character from the font
 * (font.c), modulated by the colour: the font's texel is 0x4210, so colour
 * c shows c >> 3, exactly a fill's colour. Blended text uses the font's
 * second CLUT, whose texel has bit 15 set.
 */
#include <string.h>

#include "api/frame.h"
#include "api/texture.h"

/* A friendly record's texture and window (as frame.c's 2D draws). */
static void textured(jbi_record *t, const jb_texture *tex)
{
    t->textured = 1;
    t->tex = *tex;
    t->window = 1;
    t->tw = jbi_record_window(tex);
}

static int power_of_two(int n)
{
    return n > 0 && (n & (n - 1)) == 0;
}

/* A position kept within an int, far outside any frame. */
static int clamp_position(int64_t v)
{
    return v < -0x40000000 ? -0x40000000 : v > 0x3fffffff ? 0x3fffffff : (int)v;
}

/* A corner of a scaled sprite. Its sub-pixel position is exact (it fits
   64 bits for any int position and size); the integer one is kept within
   an int, and where that changes it the clipping takes the sub-pixel one
   (split.h), so the source maps onto the whole quad as given. */
static void corner(jbi_split_vertex *o, int64_t x, int64_t y, uint32_t c, int u, int v)
{
    o->x = clamp_position(x);
    o->y = clamp_position(y);
    o->qx = x * JBI_PRECISE_ONE;
    o->qy = y * JBI_PRECISE_ONE;
    o->z = 0;
    o->r = (int)(c & 0xff);
    o->g = (int)((c >> 8) & 0xff);
    o->b = (int)((c >> 16) & 0xff);
    o->u = u;
    o->v = v;
}

jb_result jb_draw_sprite(jb_context *jb, const jb_sprite *s)
{
    static const char fn[] = "jb_draw_sprite";
    const jb_texture *t;
    jbi_record rec;
    jb_rect check;
    jb_result r;
    int tw, th, limit_w, limit_h, sw, sh, dw, dh, fw, fh, rep;
    uint32_t tint;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (s == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: sprite is NULL", fn);
    if (!jbi_frame_active(jb))
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: no frame begun (call jb_begin_frame first)", fn);
    t = s->texture;
    if (t == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: sprite->texture is NULL", fn);
    tint = s->tint == 0 ? JB_NEUTRAL : s->tint == JB_TINT_BLACK ? 0 : s->tint;
    /* The texture, blend mode and tint, as a rectangle checks them. */
    memset(&check, 0, sizeof check);
    check.color = tint;
    check.blend = s->blend;
    check.texture = t;
    check.raw = s->raw;
    r = jbi_check_rect(jb, fn, &check, 0);
    if (r != JB_OK)
        return r;
    /* The friendly fields, each in its range before any sum: u, v 0..255,
       width, height 0..256 (0, a texture made by hand: its page from u, v),
       and the rectangle within the page. */
    if (t->u < 0 || t->u > 255 || t->v < 0 || t->v > 255 || t->width < 0 || t->width > 256 ||
        t->height < 0 || t->height > 256 || t->u + t->width > 256 || t->v + t->height > 256)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: the texture's u, v (%d, %d) and size %d x %d leave its page", fn,
                            t->u, t->v, t->width, t->height);
    tw = t->width > 0 ? t->width : 256 - t->u;
    th = t->height > 0 ? t->height : 256 - t->v;
    /* A repeat texture, made by hand too, must be one the texture window
       can wrap: power-of-two sizes 8..256 at a multiple of them. */
    if (t->repeat && (!power_of_two(t->width) || !power_of_two(t->height) || t->width < 8 ||
                      t->height < 8 || t->u % t->width != 0 || t->v % t->height != 0))
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: a repeat texture needs power-of-two sizes 8..256 at a multiple "
                            "of them, not %d x %d at (%d, %d)",
                            fn, t->width, t->height, t->u, t->v);
    if (s->u < 0 || s->u > tw - 1 || s->v < 0 || s->v > th - 1)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: u, v (%d, %d) is outside the texture's 0..%d x 0..%d", fn, s->u,
                            s->v, tw - 1, th - 1);
    /* The source: within the texture, or for a repeat texture within the
       page's 256 texels from (u, v), which the window tiles. */
    rep = t->repeat != 0;
    limit_w = rep ? 256 - s->u : tw - s->u;
    limit_h = rep ? 256 - s->v : th - s->v;
    if (s->src_w < 0 || s->src_w > limit_w || s->src_h < 0 || s->src_h > limit_h)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: source %d x %d is outside 0..%d x 0..%d from (%d, %d)", fn,
                            s->src_w, s->src_h, limit_w, limit_h, s->u, s->v);
    if (s->w < 0 || s->h < 0)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: size %d x %d is negative", fn,
                            s->w, s->h);
    sw = s->src_w != 0 ? s->src_w : tw - s->u;
    sh = s->src_h != 0 ? s->src_h : th - s->v;
    dw = s->w != 0 ? s->w : sw;
    dh = s->h != 0 ? s->h : sh;
    /* Wholly outside the frame: nothing (and the corners below fit in an
       int: x + w is computed in 64 bits). */
    jbi_frame_size(jb, &fw, &fh);
    if ((int64_t)s->x + dw <= 0 || (int64_t)s->y + dh <= 0 || s->x >= fw || s->y >= fh)
        return JB_OK;

    memset(&rec, 0, sizeof rec);
    textured(&rec, t);
    if (dw == sw && dh == sh) {
        int u = t->u + s->u, v = t->v + s->v;

        rec.kind = JBI_REC_RECT;
        rec.u.rect.x = s->x;
        rec.u.rect.y = s->y;
        rec.u.rect.w = dw;
        rec.u.rect.h = dh;
        rec.u.rect.color = tint;
        rec.u.rect.blend = s->blend;
        rec.u.rect.raw = s->raw != 0;
        rec.u.rect.flip_x = s->flip_x != 0;
        rec.u.rect.flip_y = s->flip_y != 0;
        rec.u.rect.u = (s->flip_x ? u + sw - 2 : u) & 255;
        rec.u.rect.v = (s->flip_y ? v + sh - 1 : v) & 255;
        return jbi_frame_add_rect(jb, fn, JBI_FRAME_LAYER, &rec);
    }
    {
        jbi_split_vertex c[4];
        int u0 = t->u + s->u, v0 = t->v + s->v, u1 = u0 + sw - 1, v1 = v0 + sh - 1, k;
        int64_t x0 = s->x, y0 = s->y, x1 = x0 + dw, y1 = y0 + dh;

        if (rep) {
            /* The window replaces the high bits: start within the first
               tile, so that the last texel stays within 0..255. */
            u0 = s->u % t->width;
            v0 = s->v % t->height;
            u1 = u0 + sw - 1;
            v1 = v0 + sh - 1;
        }
        if (s->flip_x)
            k = u0, u0 = u1, u1 = k;
        if (s->flip_y)
            k = v0, v0 = v1, v1 = k;
        rec.kind = JBI_REC_POLY;
        rec.u.poly.vertices = 4;
        rec.u.poly.blend = s->blend;
        rec.u.poly.raw = s->raw != 0;
        /* Cyclic order, as jbi_frame_add_poly takes a polygon. */
        corner(&c[0], x0, y0, tint, u0, v0);
        corner(&c[1], x1, y0, tint, u1, v0);
        corner(&c[2], x1, y1, tint, u1, v1);
        corner(&c[3], x0, y1, tint, u0, v1);
        return jbi_frame_add_poly(jb, fn, JBI_FRAME_LAYER, &rec, c, 4);
    }
}

/* ------------------------------------------------------------------------ */
/* Text */

jb_result jb_text_size(const char *text, int *width, int *height)
{
    return jbi_text_size_most(text, 0x7fffffff / JBI_FONT_CELL, width, height);
}

jb_result jbi_text_size_most(const char *text, size_t most, int *width, int *height)
{
    size_t line = 0, widest = 0, lines = 0;
    const char *p;

    if (width != NULL)
        *width = 0;
    if (height != NULL)
        *height = 0;
    if (text == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (*text != '\0')
        lines = 1;
    for (p = text; *p != '\0'; ++p) {
        if (*p == '\n') {
            ++lines;
            line = 0;
        } else if (++line > widest) {
            widest = line;
        }
    }
    /* A size past an int, in either direction: refused, both sizes 0. */
    if (widest > most || lines > most)
        return JB_ERROR_INVALID_ARGUMENT;
    if (width != NULL)
        *width = (int)widest * JBI_FONT_CELL;
    if (height != NULL)
        *height = (int)lines * JBI_FONT_CELL;
    return JB_OK;
}

/* Whether character ch's glyph at (px, py) has a set texel inside the
   frame (fw x fh): a space, a cell off the frame, or a cell whose part in
   the frame is all blank draws nothing, and needs neither a record nor
   the font. A byte outside 32..126 is drawn as '?'. */
static int glyph_shows(unsigned char ch, int64_t px, int64_t py, int fw, int fh)
{
    int r, c;

    if (ch < JBI_FONT_FIRST || ch > JBI_FONT_LAST)
        ch = '?';
    if (px + JBI_FONT_CELL <= 0 || py + JBI_FONT_CELL <= 0 || px >= fw || py >= fh)
        return 0;
    for (r = 0; r < JBI_FONT_CELL; ++r) {
        unsigned bits = jbi_font_row(ch, r);
        if (py + r < 0 || py + r >= fh)
            continue;
        for (c = 0; c < JBI_FONT_CELL; ++c)
            if ((bits >> (7 - c) & 1) && px + c >= 0 && px + c < fw)
                return 1;
    }
    return 0;
}

static jb_result draw_text(jb_context *jb, const char *fn, int x, int y, uint32_t color,
                           int blend, const char *text)
{
    const jb_texture *font = NULL;
    jb_texture tex;
    jbi_record rec;
    jb_rect check;
    jb_result r;
    int64_t px = x, py = y;
    int fw, fh;
    const unsigned char *p;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (text == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: text is NULL", fn);
    if (!jbi_frame_active(jb))
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: no frame begun (call jb_begin_frame first)", fn);
    memset(&check, 0, sizeof check);
    check.color = color;
    check.blend = blend;
    r = jbi_check_rect(jb, fn, &check, 0);
    if (r != JB_OK)
        return r;
    memset(&rec, 0, sizeof rec);
    rec.kind = JBI_REC_RECT;
    rec.u.rect.w = JBI_FONT_CELL;
    rec.u.rect.h = JBI_FONT_CELL;
    rec.u.rect.color = color;
    rec.u.rect.blend = blend;
    jbi_frame_size(jb, &fw, &fh);
    for (p = (const unsigned char *)text; *p != '\0'; ++p) {
        int u, v;

        if (*p == '\n') {
            px = x;
            py += JBI_FONT_CELL;
            continue;
        }
        if (glyph_shows(*p, px, py, fw, fh)) {
            jbi_font_glyph(*p, &u, &v);
            rec.u.rect.x = (int)px;
            rec.u.rect.y = (int)py;
            if (font == NULL) {
                /* The font is placed at the first glyph that shows, so text
                   that draws nothing takes no VRAM; and only once the frame
                   has room for that glyph, so text a full frame drops writes
                   nothing either, and a failed placement takes no record.
                   A glyph that shows lies within the GPU's limits (its cell
                   is partly in the frame), so it is one record as it is. */
                if (!jbi_frame_has_room(jb, fn))
                    return JB_ERROR_OUT_OF_MEMORY; /* the frame is full: recorded */
                font = jbi_font(jb, fn);
                if (font == NULL)
                    return JB_ERROR_OUT_OF_MEMORY; /* recorded by jbi_font */
                tex = blend != JB_BLEND_NONE ? jbi_font_blend(jb) : *font;
                textured(&rec, &tex);
            }
            rec.u.rect.u = tex.u + u;
            rec.u.rect.v = tex.v + v;
            if ((r = jbi_frame_add_rect(jb, fn, JBI_FRAME_LAYER, &rec)) != JB_OK)
                return r; /* the frame is full: the rest is dropped */
        }
        px += JBI_FONT_CELL;
        if (px > 0x3fffffff)
            px = 0x3fffffff; /* far past the frame: stays there */
    }
    return JB_OK;
}

jb_result jb_draw_text(jb_context *jb, int x, int y, uint32_t color, const char *text)
{
    return draw_text(jb, "jb_draw_text", x, y, color, JB_BLEND_NONE, text);
}

jb_result jb_draw_text_blended(jb_context *jb, int x, int y, uint32_t color, int blend,
                               const char *text)
{
    return draw_text(jb, "jb_draw_text_blended", x, y, color, blend, text);
}

/*
 * Textures placed in VRAM (include/jitterbug.h, Textures; texture.h).
 *
 * The context's table holds each live texture: the jb_texture handed out,
 * and the VRAM rectangles of its texels and palette, which are taken in
 * the placer (src/core/placer.h) while it lives. A texture is found again
 * by its fields and its id (jbi_texture_find), since jb_texture is passed
 * by value: the id is new for every texture made, so a copy of a destroyed
 * texture's handle never names the texture made in its place.
 *
 * Texels and palettes are written a row at a time with jbi_load, which is
 * what jb_vram_load does with the whole rectangle (rows in order:
 * src/api/vram.c), from a row buffer on the stack, so nothing here
 * allocates after jbi_textures_create. They are written with the mask
 * settings off (jbi_mask_off), so that every texel is written as given:
 * the set bit would otherwise add bit 15 to whole packed words (changing
 * the texel in their top bits) and the check bit would leave old words.
 */
#include <stdio.h>
#include <string.h>

#include "api/api.h"
#include "api/texture.h"
#include "core/alloc.h"

static const char *depth_name(int depth)
{
    return depth == JB_TEX_4BIT ? "4-bit" : depth == JB_TEX_8BIT ? "8-bit" : "15-bit";
}

/* Texels a VRAM pixel holds at a depth. */
static int per_pixel(int depth)
{
    return depth == JB_TEX_4BIT ? 4 : depth == JB_TEX_8BIT ? 2 : 1;
}

/* VRAM pixels a row of w texels takes. */
static int row_pixels(int depth, int w)
{
    int n = per_pixel(depth);

    return (w + n - 1) / n;
}

struct jbi_textures *jbi_textures_create(jb_context *jb)
{
    struct jbi_textures *t = jbi_calloc(&jb->alloc, 1, sizeof *t);

    if (t == NULL)
        return NULL;
    jbi_placer_init(&t->placer);
    t->font = -1;
    return t;
}

void jbi_textures_mark_buffers(jb_context *jb, const jbi_rect *buffers, int n)
{
    int i;

    for (i = 0; i < n; ++i)
        jbi_placer_mark(&jb->api->textures->placer, buffers[i], 1);
}

uint32_t jbi_mask_off(jb_context *jb)
{
    uint32_t saved = 0xe6000000u | (jb->draw.set_mask ? 1u : 0) | (jb->draw.check_mask ? 2u : 0);

    if (saved != 0xe6000000u) {
        uint32_t off = 0xe6000000u;
        jbi_api_command(jb, &off, 1);
    }
    return saved;
}

void jbi_mask_restore(jb_context *jb, uint32_t saved)
{
    if (saved != 0xe6000000u)
        jbi_api_command(jb, &saved, 1);
}

void jbi_textures_destroy(jb_context *jb)
{
    jbi_free(&jb->alloc, jb->api->textures);
    jb->api->textures = NULL;
}

static int same(const jb_texture *a, const jb_texture *b)
{
    return a->page_x == b->page_x && a->page_y == b->page_y && a->depth == b->depth &&
           a->clut_x == b->clut_x && a->clut_y == b->clut_y && a->u == b->u && a->v == b->v &&
           a->width == b->width && a->height == b->height && a->repeat == b->repeat &&
           a->id == b->id && a->owner == b->owner;
}

int jbi_texture_find(const jb_context *jb, const jb_texture *t)
{
    const struct jbi_textures *s = jb->api->textures;
    int i;

    for (i = 0; i < JBI_MAX_TEXTURES; ++i)
        if (s->slot[i].live && same(&s->slot[i].tex, t))
            return i;
    return -1;
}

void jbi_texture_window(const jb_texture *t, int *mask_x, int *mask_y, int *offset_x,
                        int *offset_y)
{
    *mask_x = *mask_y = *offset_x = *offset_y = 0;
    if (!t->repeat)
        return;
    if (t->width >= 8 && t->width < 256) {
        *mask_x = ((256 - t->width) / 8) & 31;
        *offset_x = (t->u / 8) & 31;
    }
    if (t->height >= 8 && t->height < 256) {
        *mask_y = ((256 - t->height) / 8) & 31;
        *offset_y = (t->v / 8) & 31;
    }
}

static int power_of_two(int n)
{
    return n > 0 && (n & (n - 1)) == 0;
}

/* The first 4-bit texel above 15 in a w x h block, as its index, or -1. */
static long bad_nibble(const uint8_t *t, int w, int h)
{
    long i, n = (long)w * h;

    for (i = 0; i < n; ++i)
        if (t[i] > 15)
            return i;
    return -1;
}

/*
 * Writes the texels of rows [y, y + h) and columns [x, x + w) of a texture
 * whose texels start at VRAM pixel (px, py), from src (w x h texels, or
 * NULL for 0). With keep, when the columns do not cover whole VRAM pixels
 * the pixels are read back first, so the texels beside them stay; without
 * (a new texture), the rest of a row's last pixel is index 0.
 */
static void put_texels(jb_context *jb, int depth, int px, int py, int x, int y, int w, int h,
                       const void *src, int keep)
{
    uint16_t line[256];
    int n = per_pixel(depth), p0 = x / n, p1 = (x + w - 1) / n, count = p1 - p0 + 1;
    int partial = keep && n > 1 && (x % n != 0 || (x + w) % n != 0);
    int r, i;

    for (r = 0; r < h; ++r) {
        if (partial)
            jbi_store_1x(jb, px + p0, py + y + r, count, 1, line);
        else
            memset(line, 0, sizeof line);
        for (i = 0; i < w; ++i) {
            int tx = x + i, k = tx / n - p0, shift = (tx % n) * (16 / n);
            unsigned value;
            if (depth == JB_TEX_15BIT) {
                line[k] = src ? ((const uint16_t *)src)[(size_t)r * w + i] : 0;
                continue;
            }
            value = src ? ((const uint8_t *)src)[(size_t)r * w + i] : 0;
            line[k] = (uint16_t)((line[k] & ~(((1u << (16 / n)) - 1) << shift)) | value << shift);
        }
        jbi_load(jb, px + p0, py + y + r, count, 1, line);
    }
}

/* The entries of a palette, or NULL for 0x0000. */
static void put_palette(jb_context *jb, jbi_rect at, int first, int count,
                        const uint16_t *colors)
{
    static const uint16_t zero[256];

    jbi_load(jb, at.x + first, at.y, count, 1, colors != NULL ? colors : zero);
}

static int overlap(jbi_rect a, jbi_rect b)
{
    return a.w > 0 && b.w > 0 && a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h &&
           b.y < a.y + a.h;
}

/*
 * Places the texels (q) and the palette (pq, or NULL for none), marking
 * both taken: 0, or 1 for no room for the texels, 2 for no room for the
 * palette beside them (nothing marked then).
 *
 * The common path: the texels' first fit, then the palette's first fit
 * around them. When the palette finds no room there, the texels may be
 * sitting on its only places, so every pair is tried, deterministically:
 * the palette's places in its search order, each with the texels' first
 * fit around it (the texels' first fit with that palette taken too); the
 * first pair wins. The row bands of the texels' request are built once
 * (jbi_placer_bands), and each palette's texel search reads them with the
 * palette's pixels added (jbi_placer_find_banded), so a palette costs a
 * band and a scan per row, not a new search over VRAM; and a palette with
 * no place at all ends at once.
 */
static int place(struct jbi_textures *s, const jbi_place_req *q, const jbi_place_req *pq,
                 jbi_rect *texels, jbi_rect *clut)
{
    jbi_rect first, p, prev, t;
    int have;

    ++s->texel_searches;
    if (!jbi_placer_find(&s->placer, q, &first))
        return 1;
    jbi_placer_mark(&s->placer, first, 1);
    *texels = first;
    if (pq == NULL)
        return 0;
    if (jbi_placer_find(&s->placer, pq, clut)) {
        jbi_placer_mark(&s->placer, *clut, 1);
        return 0;
    }
    jbi_placer_mark(&s->placer, first, 0);
    ++s->texel_searches;
    jbi_placer_bands(&s->placer, q->h, s->bands);
    for (have = jbi_placer_find(&s->placer, pq, &p); have;
         prev = p, have = jbi_placer_find_next(&s->placer, pq, &prev, &p)) {
        if (!jbi_placer_find_banded((const uint64_t(*)[JBI_PLACER_WORDS])s->bands, q, p, &t))
            continue;
        jbi_placer_mark(&s->placer, t, 1);
        jbi_placer_mark(&s->placer, p, 1);
        *texels = t;
        *clut = p;
        return 0;
    }
    return 2;
}

/*
 * The font's texels (q) and two CLUTs (pq each) when the CLUTs cannot lie
 * side by side: the first texel place, in the texels' order, that leaves
 * two CLUT places (each the first fit, the second around the first), all
 * three marked taken: 0; or 2 for no CLUT place at all, 3 for no texel
 * place that leaves two (nothing marked then). The texels have a place:
 * the caller found it.
 * The two first CLUT places with no texels placed are found first: if
 * there are not two, no texel place can help; if there are, the first
 * texel place clear of both succeeds, so the loop ends there at the
 * latest, after the few places that overlap them.
 */
static int place_apart(struct jbi_textures *s, const jbi_place_req *q, const jbi_place_req *pq,
                       jbi_rect *texels, jbi_rect *clut, jbi_rect *clut2)
{
    jbi_rect t, prev;
    int have;

    if (!jbi_placer_find(&s->placer, pq, clut))
        return 2;
    jbi_placer_mark(&s->placer, *clut, 1);
    have = jbi_placer_find(&s->placer, pq, clut2);
    jbi_placer_mark(&s->placer, *clut, 0);
    if (!have)
        return 3;
    ++s->texel_searches;
    for (have = jbi_placer_find(&s->placer, q, &t); have;
         prev = t, ++s->texel_searches, have = jbi_placer_find_next(&s->placer, q, &prev, &t)) {
        jbi_placer_mark(&s->placer, t, 1);
        if (jbi_placer_find(&s->placer, pq, clut)) {
            jbi_placer_mark(&s->placer, *clut, 1);
            if (jbi_placer_find(&s->placer, pq, clut2)) {
                jbi_placer_mark(&s->placer, *clut2, 1);
                *texels = t;
                return 0;
            }
            jbi_placer_mark(&s->placer, *clut, 0);
        }
        jbi_placer_mark(&s->placer, t, 0);
    }
    return 3; /* every texel place takes one of the two */
}

jb_result jbi_texture_create(jb_context *jb, const char *fn, const jb_texture_desc *d,
                             int internal, jb_texture *out)
{
    static const uint32_t clear_cache = 0x01000000u;
    struct jbi_textures *s = jb->api->textures;
    jbi_place_req q, pq;
    jbi_rect texels, clut = {0, 0, 0, 0}, blend = {0, 0, 0, 0}, clut2 = {0, 0, 0, 0};
    jbi_tex_slot *slot;
    jb_texture t;
    int i, free_slot = -1, entries, r;
    uint32_t mask;
    char what[64];

    if (d->width < 1 || d->width > 256 || d->height < 1 || d->height > 256)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: size %d x %d is outside 1..256 x 1..256", fn, d->width,
                            d->height);
    if (d->depth != JB_TEX_4BIT && d->depth != JB_TEX_8BIT && d->depth != JB_TEX_15BIT)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: depth %d is not 4, 8 or 15", fn,
                            d->depth);
    if (d->repeat && (!power_of_two(d->width) || !power_of_two(d->height) || d->width < 8 ||
                      d->height < 8))
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: a repeat texture needs power-of-two sizes 8..256, not %d x %d",
                            fn, d->width, d->height);
    if (d->depth == JB_TEX_4BIT && d->texels != NULL) {
        long bad = bad_nibble(d->texels, d->width, d->height);
        if (bad >= 0)
            return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                                "%s: 4-bit texel (%ld, %ld) is %u, above 15", fn,
                                bad % d->width, bad / d->width,
                                (unsigned)((const uint8_t *)d->texels)[bad]);
    }
    for (i = 0; i < JBI_MAX_TEXTURES && free_slot < 0; ++i)
        if (!s->slot[i].live)
            free_slot = i;
    if (free_slot < 0)
        return jbi_api_fail(jb, JB_ERROR_OUT_OF_MEMORY,
                            "%s: the context already holds %d textures (jb_texture_destroy "
                            "frees one)",
                            fn, JBI_MAX_TEXTURES);
    if (s->serial == 0xffffffffu)
        return jbi_api_fail(jb, JB_ERROR_OUT_OF_MEMORY,
                            "%s: the context has made %lu textures, every id it has", fn,
                            (unsigned long)s->serial);
    snprintf(what, sizeof what, "%d x %d %s%s texture", d->width, d->height,
             depth_name(d->depth), d->repeat ? " repeat" : "");

    q.w = row_pixels(d->depth, d->width);
    q.h = d->height;
    q.align = 1;
    q.align_y = 1;
    if (d->repeat) {
        int wpx = d->width / per_pixel(d->depth);
        q.align = wpx < 64 ? wpx : 64;
        q.align_y = d->height;
    }
    q.span = 256 / per_pixel(d->depth);
    q.from_bottom = 0;
    entries = d->depth == JB_TEX_4BIT ? 16 : d->depth == JB_TEX_8BIT ? 256 : 0;
    pq.w = internal ? 2 * entries : entries; /* the font: two CLUTs */
    pq.h = 1;
    pq.align = 16;
    pq.align_y = 1;
    pq.span = 0;
    pq.from_bottom = 1;
    r = place(s, &q, entries != 0 ? &pq : NULL, &texels, &clut);
    if (internal && r == 0) {
        /* The font's two CLUTs together: the second is the run's half. */
        blend = clut;
        blend.x += entries;
        blend.w = clut.w = entries;
        clut2 = blend;
    } else if (internal && r == 2) {
        /* Apart: the first atlas place that leaves room for both. */
        pq.w = entries;
        r = place_apart(s, &q, &pq, &texels, &clut, &blend);
        if (r == 3)
            return jbi_api_fail(jb, JB_ERROR_OUT_OF_MEMORY,
                                "%s: no room in VRAM for the second %d-entry palette (for "
                                "blended text) of a %s",
                                fn, entries, what);
        if (r == 0)
            clut2 = blend;
    }
    if (r == 1)
        return jbi_api_fail(jb, JB_ERROR_OUT_OF_MEMORY, "%s: no room in VRAM for a %s", fn,
                            what);
    if (r == 2)
        return jbi_api_fail(jb, JB_ERROR_OUT_OF_MEMORY,
                            "%s: no room in VRAM for the %d-entry palette of a %s", fn, entries,
                            what);

    memset(&t, 0, sizeof t);
    t.page_x = texels.x & ~63;
    t.page_y = texels.y & ~255;
    t.depth = d->depth;
    t.clut_x = clut.x;
    t.clut_y = clut.y;
    t.u = (texels.x - t.page_x) * per_pixel(d->depth);
    t.v = texels.y - t.page_y;
    t.width = d->width;
    t.height = d->height;
    t.repeat = d->repeat != 0;
    t.id = ++s->serial;
    t.owner = jb;

    slot = &s->slot[free_slot];
    slot->live = 1;
    slot->internal = internal;
    slot->tex = t;
    slot->texels = texels;
    slot->clut = clut;
    slot->clut2 = clut2;
    if (internal)
        s->font_blend = blend;

    mask = jbi_mask_off(jb);
    put_texels(jb, d->depth, texels.x, texels.y, 0, 0, d->width, d->height, d->texels, 0);
    if (entries != 0) {
        put_palette(jb, clut, 0, entries, d->palette);
        /* The CLUT cache may hold a palette drawn from these pixels before
           (a destroyed texture's, or a program's own): it must not stand
           in for this one. */
        jbi_api_command(jb, &clear_cache, 1);
    }
    jbi_mask_restore(jb, mask);
    *out = t;
    return JB_OK;
}

jb_result jb_texture_create(jb_context *jb, const jb_texture_desc *desc, jb_texture *out)
{
    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (desc == NULL || out == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_texture_create: %s is NULL",
                            desc == NULL ? "desc" : "out");
    return jbi_texture_create(jb, "jb_texture_create", desc, 0, out);
}

/* The slot of the context's live texture equal to *t, or -1 with the
   failure recorded. */
static int find_or_fail(jb_context *jb, const char *fn, const jb_texture *t, jb_result *r)
{
    int i = jbi_texture_find(jb, t);

    if (i < 0)
        *r = jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                          "%s: the texture at page (%d, %d), u, v (%d, %d), %d x %d is not one "
                          "jb_texture_create made on this context (or it was destroyed)",
                          fn, t->page_x, t->page_y, t->u, t->v, t->width, t->height);
    return i;
}

jb_result jb_texture_destroy(jb_context *jb, const jb_texture *texture)
{
    struct jbi_textures *s;
    jb_result r = JB_OK;
    int i;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (texture == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_texture_destroy: texture is NULL");
    s = jb->api->textures;
    i = find_or_fail(jb, "jb_texture_destroy", texture, &r);
    if (i < 0)
        return r;
    if (s->slot[i].internal)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_texture_destroy: that texture is the library's font");
    jbi_placer_mark(&s->placer, s->slot[i].texels, 0);
    if (s->slot[i].clut.w != 0)
        jbi_placer_mark(&s->placer, s->slot[i].clut, 0);
    memset(&s->slot[i], 0, sizeof s->slot[i]);
    return JB_OK;
}

jb_result jb_palette_write(jb_context *jb, const jb_texture *texture, int first, int count,
                           const uint16_t *colors)
{
    static const uint32_t clear_cache = 0x01000000u;
    jb_result r = JB_OK;
    int i, entries;
    uint32_t mask;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (texture == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_palette_write: texture is NULL");
    i = find_or_fail(jb, "jb_palette_write", texture, &r);
    if (i < 0)
        return r;
    if (jb->api->textures->slot[i].internal)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_palette_write: that texture is the library's font");
    if (texture->depth == JB_TEX_15BIT)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_palette_write: a 15-bit texture has no palette");
    entries = texture->depth == JB_TEX_4BIT ? 16 : 256;
    if (first < 0 || first > entries - 1)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_palette_write: first %d is outside 0..%d", first, entries - 1);
    if (count < 0 || count > entries - first)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_palette_write: count %d is outside 0..%d", count, entries - first);
    if (count == 0)
        return JB_OK;
    if (colors == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_palette_write: colors is NULL but count is %d", count);
    mask = jbi_mask_off(jb);
    put_palette(jb, jb->api->textures->slot[i].clut, first, count, colors);
    jbi_api_command(jb, &clear_cache, 1);
    jbi_mask_restore(jb, mask);
    return JB_OK;
}

jb_result jb_texture_write(jb_context *jb, const jb_texture *texture, int x, int y, int w, int h,
                           const void *texels)
{
    jb_result r = JB_OK;
    int i;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (texture == NULL || texels == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_texture_write: %s is NULL",
                            texture == NULL ? "texture" : "texels");
    i = find_or_fail(jb, "jb_texture_write", texture, &r);
    if (i < 0)
        return r;
    if (jb->api->textures->slot[i].internal)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_texture_write: that texture is the library's font");
    if (x < 0 || x > texture->width - 1 || y < 0 || y > texture->height - 1)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_texture_write: (%d, %d) is outside 0..%d x 0..%d", x, y,
                            texture->width - 1, texture->height - 1);
    if (w < 1 || w > texture->width - x || h < 1 || h > texture->height - y)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_texture_write: size %d x %d is outside 1..%d x 1..%d", w, h,
                            texture->width - x, texture->height - y);
    if (texture->depth == JB_TEX_4BIT) {
        long bad = bad_nibble(texels, w, h);
        if (bad >= 0)
            return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                                "jb_texture_write: 4-bit texel (%ld, %ld) is %u, above 15",
                                bad % w, bad / w, (unsigned)((const uint8_t *)texels)[bad]);
    }
    jbi_texture_put(jb, i, x, y, w, h, texels);
    return JB_OK;
}

void jbi_texture_put(jb_context *jb, int slot, int x, int y, int w, int h, const void *texels)
{
    const jbi_tex_slot *t = &jb->api->textures->slot[slot];
    uint32_t mask = jbi_mask_off(jb);

    put_texels(jb, t->tex.depth, t->texels.x, t->texels.y, x, y, w, h, texels, 1);
    jbi_mask_restore(jb, mask);
}

jb_result jb_vram_reserve(jb_context *jb, int x, int y, int w, int h)
{
    struct jbi_textures *s;
    jbi_rect r;
    int i;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (x < 0 || x > JBI_VRAM_W - 1 || y < 0 || y > JBI_VRAM_H - 1)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_vram_reserve: position (%d, %d) is outside 0..%d x 0..%d", x, y,
                            JBI_VRAM_W - 1, JBI_VRAM_H - 1);
    if (w < 1 || w > JBI_VRAM_W - x || h < 1 || h > JBI_VRAM_H - y)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_vram_reserve: size %d x %d is outside 1..%d x 1..%d", w, h,
                            JBI_VRAM_W - x, JBI_VRAM_H - y);
    s = jb->api->textures;
    r.x = x, r.y = y, r.w = w, r.h = h;
    for (i = 0; i < JBI_MAX_TEXTURES; ++i) {
        const jbi_tex_slot *t = &s->slot[i];
        if (!t->live)
            continue;
        if (overlap(r, t->texels) || overlap(r, t->clut) || overlap(r, t->clut2))
            return jbi_api_fail(
                jb, JB_ERROR_INVALID_ARGUMENT,
                "jb_vram_reserve: (%d, %d) %d x %d overlaps the %s of the texture at page "
                "(%d, %d), u, v (%d, %d)%s",
                x, y, w, h, overlap(r, t->texels) ? "texels" : "palette", t->tex.page_x,
                t->tex.page_y, t->tex.u, t->tex.v, t->internal ? " (the library's font)" : "");
    }
    jbi_placer_mark(&s->placer, r, 1);
    return JB_OK;
}

jb_result jb_palette_from_rgba(const uint8_t *rgba, size_t count, uint16_t *out)
{
    size_t i;

    if (count == 0)
        return JB_OK;
    if (rgba == NULL || out == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    for (i = 0; i < count; ++i) {
        const uint8_t *c = rgba + 4 * i;
        unsigned v = (unsigned)(c[0] >> 3) | (unsigned)(c[1] >> 3) << 5 |
                     (unsigned)(c[2] >> 3) << 10;
        if (c[3] == 0)
            v = 0;
        else if (c[3] < 128 || v == 0)
            v |= 0x8000u;
        out[i] = (uint16_t)v;
    }
    return JB_OK;
}

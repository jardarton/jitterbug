/*
 * Texture files, .jbt (docs/formats.md; include/jitterbug.h, Textures):
 * jb_texture_parse, jb_texture_load_mem, jb_texture_load.
 *
 * Everything in a file is checked before anything is placed: the header,
 * the sections against the file's size, and every 4-bit texel. Loading
 * then places the texture with jbi_texture_create and needs no memory of
 * its own: the palette is copied to the stack (the file's bytes need not
 * be aligned), 4- and 8-bit texels are read where they are (bytes), and
 * 15-bit texels are written a row at a time from the stack after the
 * texture is placed.
 */
#include <stdint.h>
#include <string.h>

#include "api/api.h"
#include "api/files.h"
#include "api/formats.h"
#include "api/texture.h"
#include "core/alloc.h"

typedef struct jbt_info {
    int width, height, depth, repeat, palettes, entries;
    const unsigned char *texels;  /* width x height bytes, or u16 at 15 bits */
    const unsigned char *palette; /* palettes x entries u16, or NULL */
} jbt_info;

static int power_of_two(unsigned n)
{
    return n > 0 && (n & (n - 1)) == 0;
}

static jb_result parse(jb_context *jb, const char *fn, const void *data, size_t size,
                       jbt_info *o)
{
    const unsigned char *p = data;
    jbi_section s[2];
    uint32_t version, file_size, texel_off, palette_off, reserved;
    unsigned w, h, depth, flags, palettes, entries, texel_bytes;
    jb_result r;

    if (data == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: data is NULL", fn);
    if (size < JBI_JBT_HEADER)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: %lu bytes is shorter than a .jbt header (%d bytes)", fn,
                            (unsigned long)size, JBI_JBT_HEADER);
    if (memcmp(p + JBI_JBT_O_MAGIC, JBI_JBT_MAGIC, 4) != 0)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: not a .jbt file (no \"%s\")", fn,
                            JBI_JBT_MAGIC);
    version = jbi_le32(p + JBI_JBT_O_VERSION);
    if (version != JBI_JBT_VERSION)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: .jbt version %lu; this library reads version %d", fn,
                            (unsigned long)version, JBI_JBT_VERSION);
    file_size = jbi_le32(p + JBI_JBT_O_SIZE);
    if (file_size != size)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: the header says %lu bytes, but there are %lu", fn,
                            (unsigned long)file_size, (unsigned long)size);
    w = jbi_le16(p + JBI_JBT_O_WIDTH);
    h = jbi_le16(p + JBI_JBT_O_HEIGHT);
    depth = p[JBI_JBT_O_DEPTH];
    flags = p[JBI_JBT_O_FLAGS];
    palettes = jbi_le16(p + JBI_JBT_O_PALETTES);
    texel_off = jbi_le32(p + JBI_JBT_O_TEXELS);
    palette_off = jbi_le32(p + JBI_JBT_O_PALETTE);
    reserved = jbi_le32(p + JBI_JBT_O_RESERVED);
    if (w < 1 || w > 256 || h < 1 || h > 256)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: size %u x %u is outside 1..256 x 1..256", fn, w, h);
    if (depth != JB_TEX_4BIT && depth != JB_TEX_8BIT && depth != JB_TEX_15BIT)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: depth %u is not 4, 8 or 15", fn,
                            depth);
    if (flags & ~(unsigned)JBI_JBT_REPEAT)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: flags 0x%02x has bits other than repeat (0x01)", fn, flags);
    if (reserved != 0)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: the reserved header field is 0x%08lx, not 0", fn,
                            (unsigned long)reserved);
    if ((flags & JBI_JBT_REPEAT) && (!power_of_two(w) || !power_of_two(h) || w < 8 || h < 8))
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: a repeat texture needs power-of-two sizes 8..256, not %u x %u",
                            fn, w, h);
    entries = depth == JB_TEX_4BIT ? 16 : depth == JB_TEX_8BIT ? 256 : 0;
    if (entries == 0 && (palettes != 0 || palette_off != 0))
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: a 15-bit texture has no palette, but the header gives %u at "
                            "offset %lu",
                            fn, palettes, (unsigned long)palette_off);
    if (entries != 0 && palettes == 0)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: a %u-bit texture needs at least one palette", fn, depth);
    texel_bytes = w * h * (depth == JB_TEX_15BIT ? 2u : 1u);
    s[0].name = "texels";
    s[0].offset = texel_off;
    s[0].bytes = texel_bytes;
    s[1].name = "palettes";
    s[1].offset = palette_off;
    s[1].bytes = (uint64_t)palettes * entries * 2u;
    r = jbi_check_sections(jb, fn, s, 2, size, JBI_JBT_HEADER);
    if (r != JB_OK)
        return r;
    if (depth == JB_TEX_4BIT) {
        unsigned i;
        for (i = 0; i < texel_bytes; i++)
            if (p[texel_off + i] > 15)
                return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                                    "%s: 4-bit texel (%u, %u) is %u, above 15", fn, i % w, i / w,
                                    (unsigned)p[texel_off + i]);
    }
    o->width = (int)w;
    o->height = (int)h;
    o->depth = (int)depth;
    o->repeat = (flags & JBI_JBT_REPEAT) != 0;
    o->palettes = (int)palettes;
    o->entries = (int)entries;
    o->texels = p + texel_off;
    o->palette = entries != 0 ? p + palette_off : NULL;
    return JB_OK;
}

jb_result jb_texture_parse(jb_context *jb, const void *data, size_t size, jb_texture_desc *desc,
                           int *palettes)
{
    static const char fn[] = "jb_texture_parse";
    jbt_info t;
    jb_result r;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (desc == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: desc is NULL", fn);
    r = parse(jb, fn, data, size, &t);
    if (r != JB_OK)
        return r;
    /* desc points into data, as uint16_t where it is 15-bit texels or a
       palette (every .jbt has one or the other). */
    if (((uintptr_t)data & 1u) != 0)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: data is not aligned to 2 bytes, as its uint16_t texels or "
                            "palette need (jb_texture_load_mem takes any)",
                            fn);
    memset(desc, 0, sizeof *desc);
    desc->width = t.width;
    desc->height = t.height;
    desc->depth = t.depth;
    desc->texels = t.texels;
    desc->palette = (const uint16_t *)(const void *)t.palette;
    desc->repeat = t.repeat;
    if (palettes != NULL)
        *palettes = t.palettes;
    return JB_OK;
}

static jb_result load(jb_context *jb, const char *fn, const void *data, size_t size,
                      jb_texture *out)
{
    uint16_t pal[256], row[256];
    jb_texture_desc d;
    jb_texture tex;
    jbt_info t;
    jb_result r;
    int i, y, slot;

    if (out == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: out is NULL", fn);
    r = parse(jb, fn, data, size, &t);
    if (r != JB_OK)
        return r;
    for (i = 0; i < t.entries; i++)
        pal[i] = (uint16_t)jbi_le16(t.palette + 2 * i);
    memset(&d, 0, sizeof d);
    d.width = t.width;
    d.height = t.height;
    d.depth = t.depth;
    d.texels = t.depth == JB_TEX_15BIT ? NULL : t.texels;
    d.palette = t.entries != 0 ? pal : NULL;
    d.repeat = t.repeat;
    r = jbi_texture_create(jb, fn, &d, 0, &tex);
    if (r != JB_OK)
        return r;
    if (t.depth == JB_TEX_15BIT) {
        slot = jbi_texture_find(jb, &tex);
        for (y = 0; y < t.height; y++) {
            const unsigned char *src = t.texels + (size_t)y * t.width * 2;
            for (i = 0; i < t.width; i++)
                row[i] = (uint16_t)jbi_le16(src + 2 * i);
            jbi_texture_put(jb, slot, 0, y, t.width, 1, row);
        }
    }
    *out = tex;
    return JB_OK;
}

jb_result jb_texture_load_mem(jb_context *jb, const void *data, size_t size, jb_texture *out)
{
    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    return load(jb, "jb_texture_load_mem", data, size, out);
}

jb_result jb_texture_load(jb_context *jb, const char *path, jb_texture *out)
{
    static const char fn[] = "jb_texture_load";
    unsigned char *data;
    size_t size;
    jb_result r;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (path == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: path is NULL", fn);
    if (out == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: out is NULL", fn);
    r = jbi_read_file(jb, fn, path, &data, &size);
    if (r != JB_OK)
        return r;
    r = load(jb, fn, data, size, out);
    jbi_free(&jb->alloc, data);
    return r;
}

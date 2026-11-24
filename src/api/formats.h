/*
 * The texture (.jbt) and mesh (.jbm) file formats, as docs/formats.md
 * describes them byte for byte: the magics, header sizes, field offsets,
 * record sizes and limits. The library's loaders (src/api/jbt.c,
 * src/api/jbm.c) read them; the tools (tools/jb-tex, tools/jb-mesh) write
 * them. Everything is little-endian. C99, no other include, so the tools
 * can use it as it is.
 */
#ifndef JBI_API_FORMATS_H
#define JBI_API_FORMATS_H

/* .jbt: one texture, as jb_texture_desc takes it. */
#define JBI_JBT_MAGIC "JBTX"
enum {
    JBI_JBT_VERSION = 1,
    JBI_JBT_HEADER = 32,        /* bytes */
    /* Header fields: offsets. */
    JBI_JBT_O_MAGIC = 0,        /* char[4] */
    JBI_JBT_O_VERSION = 4,      /* u32 */
    JBI_JBT_O_SIZE = 8,         /* u32: the whole file */
    JBI_JBT_O_WIDTH = 12,       /* u16: 1..256 */
    JBI_JBT_O_HEIGHT = 14,      /* u16: 1..256 */
    JBI_JBT_O_DEPTH = 16,       /* u8: 4, 8 or 15 */
    JBI_JBT_O_FLAGS = 17,       /* u8: JBI_JBT_REPEAT */
    JBI_JBT_O_PALETTES = 18,    /* u16: 0 at 15 bits, else 1..65535 */
    JBI_JBT_O_TEXELS = 20,      /* u32: offset of the texels */
    JBI_JBT_O_PALETTE = 24,     /* u32: offset of the palettes, 0 for none */
    JBI_JBT_O_RESERVED = 28,    /* u32: 0 */
    JBI_JBT_REPEAT = 1          /* flags bit 0 */
};

/* .jbm: one mesh, as jb_mesh takes it, with texture slots for the caller
   to bind. */
#define JBI_JBM_MAGIC "JBMS"
enum {
    JBI_JBM_VERSION = 1,
    JBI_JBM_HEADER = 64,
    JBI_JBM_O_MAGIC = 0,        /* char[4] */
    JBI_JBM_O_VERSION = 4,      /* u32 */
    JBI_JBM_O_SIZE = 8,         /* u32: the whole file */
    JBI_JBM_O_FLAGS = 12,       /* u32: JBI_JBM_NORMALS | _COLORS | _UV */
    JBI_JBM_O_VERTICES = 16,    /* u32: 1..65536 */
    JBI_JBM_O_FACES = 20,       /* u32: 0..JBI_JBM_MAX_FACES */
    JBI_JBM_O_MATERIALS = 24,   /* u32: 0..65536, at least 1 with faces */
    JBI_JBM_O_SLOTS = 28,       /* u32: 0..65535 */
    /* Section offsets (u32), in this order; 0 for an absent section. */
    JBI_JBM_O_SECTIONS = 32,
    JBI_JBM_SECTIONS = 7,
    JBI_JBM_S_POSITIONS = 0,
    JBI_JBM_S_NORMALS = 1,
    JBI_JBM_S_COLORS = 2,
    JBI_JBM_S_UV = 3,
    JBI_JBM_S_FACES = 4,
    JBI_JBM_S_MATERIALS = 5,
    JBI_JBM_S_SLOTS = 6,
    JBI_JBM_O_RESERVED = 60,    /* u32: 0 */
    JBI_JBM_NORMALS = 1,        /* flags */
    JBI_JBM_COLORS = 2,
    JBI_JBM_UV = 4,
    /* Record sizes. */
    JBI_JBM_POSITION = 6,       /* i16 x, y, z */
    JBI_JBM_NORMAL = 6,         /* i16 x, y, z: 4.12, each -4096..4096 */
    JBI_JBM_COLOR = 4,          /* u32 JB_RGB */
    JBI_JBM_UVREC = 2,          /* u8 u, v */
    JBI_JBM_FACE = 12,          /* u16 v[4], vertices (3, 4), material */
    JBI_JBM_MATERIAL = 8,       /* u32 color, u8 blend, u8 flags, u16 slot */
    JBI_JBM_SLOT = 4,           /* u16 width, height */
    /* Material flags. */
    JBI_JBM_UNLIT = 1,
    JBI_JBM_RAW = 2,
    JBI_JBM_DOUBLE_SIDED = 4,
    JBI_JBM_NO_SLOT = 0xffff,
    JBI_JBM_MAX_VERTICES = 65536,
    JBI_JBM_MAX_FACES = 1048576,
    JBI_JBM_MAX_MATERIALS = 65536,
    JBI_JBM_MAX_SLOTS = 65535
};

/* The largest file either loader's file form reads. A .jbt is at most
   32 + 256 x 256 x 2 + 65535 x 512 bytes, about 33 MB; a .jbm at most
   64 + 65536 x 18 + 1048576 x 12 + 65536 x 8 + 65535 x 4 bytes, about
   14 MB. */
#define JBI_FILE_MAX ((size_t)64 << 20)

#endif

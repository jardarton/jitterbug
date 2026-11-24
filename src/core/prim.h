/*
 * The primitive records the command decoder (gp0.c) hands to a backend
 * (backend.h), and the VRAM they are drawn into. Written from the
 * documented behaviour of the hardware.
 *
 * VRAM is 1024x512 16-bit pixels, each A1B5G5R5 with bit 15 the mask bit.
 *
 * Primitives become jbi_prim records that carry the drawing state they
 * need, so a backend can rasterise them later and in parallel (the
 * software backend does, src/soft/soft.c).
 */
#ifndef JBI_CORE_PRIM_H
#define JBI_CORE_PRIM_H

#include <stdint.h>

enum {
    JBI_VRAM_W = 1024,
    JBI_VRAM_H = 512,
    JBI_MAX_SCALE = 8,
};

/* Texture colour depths in the texture page attribute. */
enum { JBI_TEX_4BIT = 0, JBI_TEX_8BIT = 1, JBI_TEX_15BIT = 2 };

/*
 * The 1x pixel a sub-pixel coordinate (1/256 pixel, jbi_prim's qx and qy)
 * lies in. At any scale, a triangle's scaled pixels lie in the 1x pixels
 * from that of its smallest coordinate to that of its largest, and a line's
 * reach one pixel further up the axis: it is a 1x pixel thick and its ends
 * round to the nearest scaled pixel. Bounds of what a primitive can write
 * (jbi_prim's row0 and row1, the software backend's tiles) must cover that.
 */
static inline int jbi_q_pixel(int q)
{
    return q >> 8;
}

/* Primitive input after decoding; coordinates already include the offset. */
typedef struct jbi_vertex {
    int x, y;
    int r, g, b;
    int u, v;
} jbi_vertex;

/* Flags for primitives. */
enum {
    JBI_F_GOURAUD = 1 << 0,
    JBI_F_TEXTURED = 1 << 1,
    JBI_F_RAW = 1 << 2,      /* texture colour unmodulated */
    JBI_F_SEMI = 1 << 3,
    JBI_F_PERSP = 1 << 4,    /* textured triangle with depths: see u.tri.z */
    /* A triangle drawn in one pass only (a piece of a clipped polygon whose
       integer and sub-pixel shapes differ: src/geom/split.h): not into the
       1x VRAM, or not into the scaled one. */
    JBI_F_NO_1X = 1 << 5,
    JBI_F_NO_SCALED = 1 << 6,
};

/* Kinds. The decoder submits triangles, rectangles and lines; fills reach
   a backend through its fill operation (the software backend queues them
   as JBI_PRIM_FILL records of its own). */
enum { JBI_PRIM_TRI, JBI_PRIM_RECT, JBI_PRIM_LINE, JBI_PRIM_FILL };

/*
 * Where a primitive's texels come from, and the texture window. clut is
 * the palette in force for a 4- or 8-bit texture: the backend's CLUT cache
 * (src/soft/soft.c), entry i the colour of index i, fixed when the
 * primitive was queued; NULL reads the CLUT from VRAM at (clut_x, clut_y)
 * as each texel is drawn (the cache off). The decoder leaves it NULL.
 */
typedef struct jbi_tex_source {
    int tpx, tpy;
    int clut_x, clut_y;
    uint32_t and_x, or_x, and_y, or_y;
    const uint16_t *clut;
} jbi_tex_source;

/*
 * One primitive with the drawing state it uses. Vertex positions are
 * integers (v[].x, v[].y, including the drawing offset) for the 1x pass
 * and, for triangles and lines, also in 1/256 pixels (qx, qy), which the
 * scaled pass uses: the precise source's unrounded projection when it had
 * one (precise_source.h), else the integer position.
 *
 * Perspective-correct texturing (jbi_set_texture_mapping, context.h): the
 * decoder sets JBI_F_PERSP on a textured triangle when the option is on and
 * every vertex has a precise record with z > 0; u.tri.z then holds the
 * depths. Otherwise the triangle is drawn affinely, as on the console.
 */
typedef struct jbi_prim {
    uint8_t kind;       /* JBI_PRIM_* */
    uint8_t flags;      /* JBI_F_* */
    uint8_t depth;      /* texture depth in bits (4, 8, 15), 0 untextured */
    uint8_t semi_mode;
    uint8_t dither;     /* draw mode's dither bit */
    uint8_t check_mask;
    uint8_t flip_x, flip_y;
    uint16_t set_mask;
    int16_t clip_x0, clip_y0, clip_x1, clip_y1;
    int16_t row0, row1;  /* 1x rows the primitive can touch, inclusive */
    jbi_tex_source tex;
    union {
        struct {
            jbi_vertex v[3];
            int qx[3], qy[3];
            int z[3];  /* view depths (SZ) with JBI_F_PERSP, else unset */
            /* With JBI_F_PERSP: each vertex's u, v, r, g, b for the
               perspective-correct pass, which the affine pass does not
               read. The decoder copies them from v[] unless it is given
               others (jbi_subpixel: a clipped polygon's new vertices
               interpolated perspective-correctly, src/geom/split.h). */
            uint8_t pattr[3][5];
        } tri;
        /* u, v: the texel of the top-left pixel, from which u steps by +1
           or (flip_x) -1 a pixel and v by +1 or (flip_y) -1 a row. u may be
           256 (u0 255, x-flipped); texel fetches take it modulo 256. */
        struct {
            int x, y, w, h, r, g, b, u, v;
        } rect;
        struct {
            jbi_vertex v[2];
            int qx[2], qy[2];
        } line;
        struct {
            int x, y, w, h;
            uint16_t color;
        } fill;
    } u;
} jbi_prim;

#endif

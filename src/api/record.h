/*
 * Recorded primitives and the ordering table (jb_ot, include/jitterbug.h).
 *
 * A record is one primitive or state change kept to be drawn later: by
 * jb_ot_draw for a public ordering table, by jb_end_frame for the frame's
 * own (src/api/frame.h). It holds the structured call's arguments by value
 * (a texture is copied in, so no pointer outlives the call that recorded
 * it), and optionally sub-pixel data and a texture window. Drawing a
 * record sends exactly the words the matching jb_prim_* call sends
 * (prim.c's emitters) into the context as it is when it is drawn: the
 * drawing offset, area, draw mode, mask settings, dithering and CLUT cache
 * all apply as for those words through jb_gp0.
 *
 * Kinds and fields:
 *   JBI_REC_POLY   u.poly, a jb_poly; its texture pointer is ignored and
 *                  tex used when textured is set.
 *   JBI_REC_LINE   u.line, a single line (a polyline is recorded as one
 *                  line a segment, jbi_polyline_segment, which draws the
 *                  same pixels: api.h).
 *   JBI_REC_RECT   u.rect, likewise with tex.
 *   JBI_REC_STATE  u.word, one GP0 word of a state command: 01h (clear the
 *                  CLUT cache), E1h (draw mode), E2h (texture window) or
 *                  E6h (mask settings), carried out as jb_gp0 would. Not
 *                  E3h-E5h: the frame owns the area and offset.
 *
 * Optional parts:
 *   sub      (polygons and lines) sp holds the vertices' sub-pixel
 *            positions and view depths (jbi_subpixel, src/core/context.h),
 *            used instead of the context's precise table: sp.positions is
 *            set when drawn, from the precise-vertices option, and the
 *            depths serve perspective-correct texturing when that option
 *            is on. Without sub the record is looked up in the precise
 *            table like any jb_prim_* call (the console level).
 *   window   tw is a GP0(E2h) word; before the primitive is drawn the
 *            texture window is set to it, unless it already is (so the
 *            word is sent only where the window changes between records).
 *            Without window the primitive uses the window as it is.
 *   passes   (a polygon with sub) 0 for drawn in every pass, as usual;
 *            else JBI_SPLIT_PASS_1X and JBI_SPLIT_PASS_PRECISE
 *            (src/geom/split.h), the position formats the piece of a
 *            clipped polygon may be drawn by. When drawn, the 1x pass draws
 *            it if its integer form may be drawn, and the scaled pass if
 *            the form it uses may be: the sub-pixel one with precise
 *            vertices on (above 1x), else the integer one. Drawn in no pass
 *            the record sends nothing.
 *   area     (the frame's records) e3 and e4 are GP0(E3h) and GP0(E4h)
 *            words: the drawing area the record is drawn with, a tile of
 *            the back buffer for a piece that must be clipped to it
 *            (src/geom/split.h). The frame draws its records with
 *            jbi_ot_draw_area, which sets the area to these words, or to
 *            the frame's own for a record without area, only where it
 *            changes, and to the frame's own after.
 *
 * A record is checked when it is recorded (jbi_check_*, api.h); drawing
 * cannot fail.
 */
#ifndef JBI_API_RECORD_H
#define JBI_API_RECORD_H

#include "api/api.h"
#include "geom/split.h"

enum { JBI_REC_POLY = 1, JBI_REC_LINE, JBI_REC_RECT, JBI_REC_STATE };

typedef struct jbi_record {
    uint8_t kind;     /* JBI_REC_* */
    uint8_t sub;      /* sp is given */
    uint8_t window;   /* tw is given */
    uint8_t textured; /* tex is the texture (polygons, rectangles) */
    uint8_t passes;   /* JBI_SPLIT_PASS_*, 0 for all (above) */
    uint8_t area;     /* e3, e4 are given */
    uint32_t tw;      /* GP0(E2h) word */
    uint32_t e3, e4;  /* GP0(E3h), GP0(E4h) words */
    int32_t next;     /* the next record in its bucket, or -1 (record.c) */
    jb_texture tex;
    jbi_subpixel sp;
    union {
        jb_poly poly;
        jb_line line;
        jb_rect rect;
        uint32_t word;
    } u;
} jbi_record;

/* GP0(E2h) with no window: every texel coordinate as it is. */
#define JBI_NO_WINDOW 0xe2000000u

/*
 * The texture window a friendly draw with texture t uses, as a GP0(E2h)
 * word: for a repeat texture the window that wraps its texel coordinates
 * within its rectangle (jbi_texture_window, texture.h), else none
 * (JBI_NO_WINDOW). t may be NULL (untextured).
 */
uint32_t jbi_record_window(const jb_texture *t);

/* Draws one record into the context (no checks: it was checked when
   recorded). */
void jbi_record_draw(jb_context *jb, const jbi_record *r);

/*
 * The ordering table. buckets lists of records, each first in first out,
 * drawn from bucket buckets - 1 down to 0. Records come from one array of
 * max_records, allocated when the table is made, so adding never
 * allocates.
 */
struct jb_ot {
    jb_context *jb; /* the context it was made with (allocator, errors) */
    int buckets, max_records, count;
    int32_t *head, *tail; /* per bucket, -1 when empty */
    jbi_record *rec;
};

/* A table, or NULL without memory (nothing logged). buckets and
   max_records > 0. */
jb_ot *jbi_ot_create(jb_context *jb, int buckets, int max_records);
void jbi_ot_destroy(jb_ot *ot);
void jbi_ot_clear(jb_ot *ot);

/* A new record at the end of bucket (0..buckets - 1), zeroed but for next,
   or NULL when the table is full. */
jbi_record *jbi_ot_add(jb_ot *ot, int bucket);

/* Draws every record, bucket buckets - 1 first, each bucket in the order
   its records were added. */
void jbi_ot_draw(jb_context *jb, const jb_ot *ot);

/* The same with the drawing area of each record (area, above): its own,
   or e3 and e4 (the frame's, GP0(E3h) and GP0(E4h) words) for one without,
   sent only where the area as last sent differs; and e3 and e4 after. With
   no record that has an area, nothing more is sent than jbi_ot_draw
   sends, once the area is e3 and e4 before. */
void jbi_ot_draw_area(jb_context *jb, const jb_ot *ot, uint32_t e3, uint32_t e4);

#endif

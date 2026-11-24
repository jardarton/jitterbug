/*
 * GP0 as a word stream (jb_gp0) and GPUREAD (jb_gpuread).
 *
 * The core decoder (jbi_gp0, src/core/gp0.c) takes complete commands, so
 * this accumulates words until a command is complete and hands it over:
 *
 *   - Fixed-length commands are passed straight from the caller's words
 *     when the whole command is there, and otherwise collected in
 *     api->cmd across calls. Their lengths, by opcode (psx-spx, "GPU"):
 *       00h-1Fh  1 word, except 02h (fill) 3
 *       20h-3Fh  polygons: per vertex 1 position word, plus a texture
 *                word when textured (bit 2) and a colour word when Gouraud
 *                (bit 4), the first vertex's colour being in the command
 *                word; a flat one has one extra word (the command), so
 *                3 or 4 vertices (bit 3) give 4 to 12 words
 *       40h-5Fh  single lines: 3 words, 4 Gouraud (bit 4); polylines
 *                (bit 3) below
 *       60h-7Fh  rectangles: command and position, plus a texture word
 *                (bit 2), plus a size word for size 0 (bits 3-4)
 *       80h-9Fh  VRAM to VRAM: 4
 *       A0h-BFh  CPU to VRAM: 3, then the pixel data below
 *       C0h-DFh  VRAM to CPU: 3
 *       E0h-FFh  1 (E1h-E6h set the drawing state; the rest do nothing)
 *   - A polyline is passed on one segment at a time, as the single line
 *     command between each vertex and the next (the same bits without the
 *     polyline bit), which the core draws exactly as it draws a whole
 *     polyline's segments. A word w with (w & 0xF000F000) == 0x50005000
 *     where the third or a later vertex would start (its colour word when
 *     Gouraud) ends it; the first two vertices are always taken, as
 *     psx-spx says a polyline has at least two.
 *   - CPU-to-VRAM pixels stream into one row (api->ld_line), written with
 *     jbi_load as each row completes: the same calls, with the same mask
 *     settings, as the core makes for a load inside a packet.
 *   - VRAM-to-CPU copies the 1x VRAM rectangle into api->rd_pix when the
 *     command completes (jbi_store_1x), for jb_gpuread. The core skips the
 *     command.
 *   - GP0(1Fh) sets the interrupt flag; E2h-E5h are also kept as sent for
 *     GP1(10h).
 *
 * Nothing here allocates.
 */
#include <string.h>

#include "api/api.h"

static int is_polyline_end(uint32_t w)
{
    return (w & 0xf000f000u) == 0x50005000u;
}

/* The words of a command of fixed length, by its opcode; 0 for a
   polyline. A0h's pixel data is not counted. */
static int command_words(uint32_t op)
{
    switch (op >> 5) {
    case 0: /* 00h-1Fh */
        return op == 0x02 ? 3 : 1;
    case 1: { /* 20h-3Fh */
        int verts = (op & 0x08) ? 4 : 3;
        int per = 1 + ((op & 0x04) ? 1 : 0) + ((op & 0x10) ? 1 : 0);
        return verts * per + ((op & 0x10) ? 0 : 1);
    }
    case 2: /* 40h-5Fh */
        if (op & 0x08)
            return 0;
        return (op & 0x10) ? 4 : 3;
    case 3: /* 60h-7Fh */
        return 2 + ((op & 0x04) ? 1 : 0) + (((op >> 3) & 3) == 0 ? 1 : 0);
    case 4: /* 80h-9Fh */
        return 4;
    case 5: /* A0h-BFh */
    case 6: /* C0h-DFh */
        return 3;
    default: /* E0h-FFh */
        return 1;
    }
}

/* Transfer sizes: 0 means the maximum, as on hardware. */
static int size_w(uint32_t v)
{
    return (int)(((v & 0xffffu) - 1) & 0x3ff) + 1;
}

static int size_h(uint32_t v)
{
    return (int)((((v >> 16) & 0xffffu) - 1) & 0x1ff) + 1;
}

void jbi_api_reset_input(struct jbi_api *a)
{
    a->state = JBI_IN_COMMAND;
    a->have = a->need = 0;
    a->ld_left = 0;
    a->rd_count = a->rd_next = 0;
}

/* A0h's three words: the pixels follow. */
static void start_load(struct jbi_api *a, const uint32_t *w)
{
    a->ld_x = (int)(w[1] & 0x3ff);
    a->ld_y = (int)((w[1] >> 16) & 0x1ff);
    a->ld_w = size_w(w[2]);
    a->ld_left = (size_t)a->ld_w * (size_t)size_h(w[2]);
    a->ld_row = a->ld_col = 0;
    a->state = JBI_IN_LOAD;
}

/* C0h's three words: the rectangle as it is now. */
static void start_read(jb_context *jb, const uint32_t *w)
{
    struct jbi_api *a = jb->api;
    int x = (int)(w[1] & 0x3ff), y = (int)((w[1] >> 16) & 0x1ff);
    int wd = size_w(w[2]), ht = size_h(w[2]);

    jbi_store_1x(jb, x, y, wd, ht, a->rd_pix);
    a->rd_count = (size_t)wd * (size_t)ht;
    a->rd_next = 0;
}

/* A complete command of command_words(op) words (api.h). */
void jbi_api_command(jb_context *jb, const uint32_t *w, int n)
{
    struct jbi_api *a = jb->api;
    uint32_t op = w[0] >> 24;

    switch (op >> 5) {
    case 5:
        start_load(a, w);
        return;
    case 6:
        start_read(jb, w);
        return;
    case 7:
        if (op >= 0xe2 && op <= 0xe5)
            a->env[op - 0xe2] = w[0] & (op == 0xe5 ? 0x3fffffu : 0xfffffu);
        break;
    case 0:
        if (op == 0x1f)
            a->irq = 1;
        break;
    default:
        break;
    }
    jbi_gp0(jb, w, n);
}

static void start_polyline(struct jbi_api *a, uint32_t w)
{
    a->pl_cmd = w;
    a->pl_color = w; /* the first vertex's colour */
    a->pl_verts = 0;
    a->pl_want_xy = 1;
    a->state = JBI_IN_POLYLINE;
}

static void polyline_word(jb_context *jb, uint32_t w)
{
    struct jbi_api *a = jb->api;
    int gouraud = (a->pl_cmd & 0x10000000u) != 0;
    uint32_t seg[4], color = a->pl_color;

    if (!a->pl_want_xy) {
        /* The first word of a vertex after the first. */
        if (a->pl_verts >= 2 && is_polyline_end(w)) {
            a->state = JBI_IN_COMMAND;
            return;
        }
        if (gouraud) {
            a->pl_next_color = w;
            a->pl_want_xy = 1;
            return;
        }
    }
    /* w is a position. */
    if (gouraud && a->pl_verts > 0)
        color = a->pl_next_color;
    if (a->pl_verts > 0) {
        uint32_t line = (a->pl_cmd & 0xf7000000u); /* the single line command */

        seg[0] = line | (a->pl_color & 0xffffffu);
        seg[1] = a->pl_xy;
        if (gouraud) {
            seg[2] = color;
            seg[3] = w;
            jbi_gp0(jb, seg, 4);
        } else {
            seg[2] = w;
            jbi_gp0(jb, seg, 3);
        }
    }
    a->pl_color = color;
    a->pl_xy = w;
    if (a->pl_verts < 2)
        ++a->pl_verts;
    a->pl_want_xy = 0;
}

static void load_pixel(jb_context *jb, uint16_t p)
{
    struct jbi_api *a = jb->api;

    a->ld_line[a->ld_col++] = p;
    --a->ld_left;
    if (a->ld_col == a->ld_w) {
        jbi_load(jb, a->ld_x, (a->ld_y + a->ld_row) & (JBI_VRAM_H - 1), a->ld_w, 1,
                 a->ld_line);
        ++a->ld_row;
        a->ld_col = 0;
    }
}

/* Pixel data from n words; returns the words used. */
static size_t load_words(jb_context *jb, const uint32_t *w, size_t n)
{
    struct jbi_api *a = jb->api;
    size_t i;

    for (i = 0; i < n && a->ld_left > 0; ++i) {
        load_pixel(jb, (uint16_t)(w[i] & 0xffffu));
        if (a->ld_left > 0)
            load_pixel(jb, (uint16_t)(w[i] >> 16));
    }
    if (a->ld_left == 0)
        a->state = JBI_IN_COMMAND;
    return i;
}

jb_result jb_gp0(jb_context *jb, const uint32_t *words, size_t count)
{
    struct jbi_api *a;
    size_t i = 0;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (words == NULL && count > 0)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_gp0: words is NULL but count is %zu",
                            count);
    a = jb->api;
    while (i < count) {
        switch (a->state) {
        case JBI_IN_FIXED: {
            size_t take = (size_t)(a->need - a->have);

            if (take > count - i)
                take = count - i;
            memcpy(a->cmd + a->have, words + i, take * sizeof *words);
            a->have += (int)take;
            i += take;
            if (a->have == a->need) {
                a->state = JBI_IN_COMMAND;
                jbi_api_command(jb, a->cmd, a->need);
            }
            break;
        }
        case JBI_IN_POLYLINE:
            polyline_word(jb, words[i++]);
            break;
        case JBI_IN_LOAD:
            i += load_words(jb, words + i, count - i);
            break;
        default: {
            int n = command_words(words[i] >> 24);

            if (n == 0) {
                start_polyline(a, words[i++]);
            } else if ((size_t)n <= count - i) {
                jbi_api_command(jb, words + i, n);
                i += (size_t)n;
            } else {
                a->have = (int)(count - i);
                a->need = n;
                memcpy(a->cmd, words + i, (size_t)a->have * sizeof *words);
                i = count;
                a->state = JBI_IN_FIXED;
            }
            break;
        }
        }
    }
    return JB_OK;
}

uint32_t jb_gpuread(jb_context *jb)
{
    struct jbi_api *a;

    if (jb == NULL)
        return 0;
    a = jb->api;
    if (a->rd_next < a->rd_count) {
        uint32_t v = a->rd_pix[a->rd_next++];

        if (a->rd_next < a->rd_count)
            v |= (uint32_t)a->rd_pix[a->rd_next++] << 16;
        a->latch = v;
    }
    return a->latch;
}

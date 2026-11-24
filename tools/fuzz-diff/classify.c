/*
 * Commands described and classified (fd.h): the drawing state they run
 * under, their kind, flags and coordinate class.
 */
#include "fd.h"

#include <stdio.h>
#include <string.h>

const char *const fd_kind_names[FD_K_COUNT] = {"state", "other", "tri",  "quad", "line", "polyline",
                                               "rect",  "fill",  "copy", "load", "read"};

void fd_state_reset(fd_state *st)
{
    memset(st, 0, sizeof *st);
    st->e1 = 0xe1000000u;
    st->e2 = 0xe2000000u;
    st->e3 = 0xe3000000u;
    st->e4 = 0xe4000000u;
    st->e5 = 0xe5000000u;
    st->e6 = 0xe6000000u;
}

void fd_state_apply(fd_state *st, const fd_cmd *c)
{
    uint32_t op = c->w[0] >> 24;

    switch (op) {
    case 0xe1:
        st->e1 = c->w[0];
        return;
    case 0xe2:
        st->e2 = c->w[0];
        return;
    case 0xe3:
        st->e3 = c->w[0];
        return;
    case 0xe4:
        st->e4 = c->w[0];
        return;
    case 0xe5:
        st->e5 = c->w[0];
        return;
    case 0xe6:
        st->e6 = c->w[0];
        return;
    default:
        break;
    }
    if ((op >> 5) == 1 && (op & 0x04) && c->n >= 5) {
        /* a textured polygon's page: the second vertex's texture word
           (words: command, v0, uv0, [c1], v1, uv1, ...) */
        int at = (op & 0x10) ? 5 : 4;
        st->e1 = (st->e1 & ~0x1ffu) | ((c->w[at] >> 16) & 0x1ffu);
    }
}

static int sext11(uint32_t v)
{
    return (int)((v & 0x7ff) ^ 0x400) - 0x400;
}

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

static void rgb(fd_desc *d, int i, uint32_t w)
{
    d->r[i] = (int)(w & 0xff);
    d->g[i] = (int)((w >> 8) & 0xff);
    d->b[i] = (int)((w >> 16) & 0xff);
}

static int overlaps(int ax0, int ay0, int ax1, int ay1, int bx0, int by0, int bx1, int by1)
{
    return ax0 <= bx1 && bx0 <= ax1 && ay0 <= by1 && by0 <= ay1;
}

void fd_describe(const fd_cmd *c, const fd_state *st, fd_desc *d)
{
    uint32_t op = c->w[0] >> 24;
    int ox = sext11(st->e5), oy = sext11(st->e5 >> 11), i;
    uint32_t e1 = st->e1;

    memset(d, 0, sizeof *d);
    d->op = (int)op;
    d->area_x0 = (int)(st->e3 & 0x3ff);
    d->area_y0 = (int)((st->e3 >> 10) & 0x1ff);
    d->area_x1 = (int)(st->e4 & 0x3ff);
    d->area_y1 = (int)((st->e4 >> 10) & 0x1ff);
    d->dither = (int)((e1 >> 9) & 1);
    d->mask_set = (int)(st->e6 & 1);
    d->mask_check = (int)((st->e6 >> 1) & 1);
    d->window = (st->e2 & 0xfffff) != 0;
    d->kind = FD_K_OTHER;
    if (op >= 0xe1 && op <= 0xe6) {
        d->kind = FD_K_STATE;
        return;
    }
    switch (op >> 5) {
    case 1: { /* polygons */
        int verts = (op & 0x08) ? 4 : 3, k = 0;
        d->kind = verts == 4 ? FD_K_QUAD : FD_K_TRI;
        d->gouraud = (op & 0x10) != 0;
        d->textured = (op & 0x04) != 0;
        d->raw = d->textured && (op & 0x01);
        d->semi = (op & 0x02) != 0;
        d->nv = verts;
        for (i = 0; i < verts; ++i) {
            uint32_t col = (i == 0 || d->gouraud) ? c->w[k++] : c->w[0];
            uint32_t pos = c->w[k++];
            rgb(d, i, col);
            d->vx[i] = sext11(pos) + ox;
            d->vy[i] = sext11(pos >> 16) + oy;
            if (d->textured) {
                uint32_t uv = c->w[k++];
                d->u[i] = (int)(uv & 0xff);
                d->v[i] = (int)((uv >> 8) & 0xff);
                if (i == 0) {
                    d->clut_x = (int)((uv >> 16) & 0x3f) * 16;
                    d->clut_y = (int)((uv >> 22) & 0x1ff);
                } else if (i == 1) {
                    e1 = (e1 & ~0x1ffu) | ((uv >> 16) & 0x1ffu);
                }
            }
        }
        break;
    }
    case 2: { /* lines */
        int gouraud = (op & 0x10) != 0, k = 1, n = 0;
        d->kind = (op & 0x08) ? FD_K_POLYLINE : FD_K_LINE;
        d->gouraud = gouraud;
        d->semi = (op & 0x02) != 0;
        rgb(d, 0, c->w[0]);
        d->vx[0] = sext11(c->w[k]) + ox;
        d->vy[0] = sext11(c->w[k] >> 16) + oy;
        ++k;
        n = 1;
        while (k < c->n && n < 64) {
            uint32_t col = c->w[0];
            if ((op & 0x08) && n >= 2 && (c->w[k] & 0xf000f000u) == 0x50005000u)
                break;
            if (gouraud) {
                col = c->w[k++];
                if (k >= c->n)
                    break;
            }
            rgb(d, n, col);
            d->vx[n] = sext11(c->w[k]) + ox;
            d->vy[n] = sext11(c->w[k] >> 16) + oy;
            ++k;
            ++n;
            if (!(op & 0x08))
                break;
        }
        d->nv = n;
        for (i = 1; i < n; ++i) {
            int dx = iabs(d->vx[i] - d->vx[i - 1]), dy = iabs(d->vy[i] - d->vy[i - 1]);
            int major = dx > dy ? dx : dy, minor = dx > dy ? dy : dx;
            if (dx != 0 && dy != 0)
                d->line_sloped = 1;
            if (major > 0 && minor > 0 && major % 2 == 0 && minor % 2 == 1)
                d->line_tie = 1;
            else if (major > 0 && minor > 0) {
                /* i * minor / major has an exact half for some i */
                int j;
                for (j = 1; j < major && !d->line_tie; ++j)
                    if ((2 * j * minor) % (2 * major) == major)
                        d->line_tie = 1;
            }
            if (gouraud && (d->r[i] < d->r[i - 1] || d->g[i] < d->g[i - 1] || d->b[i] < d->b[i - 1]))
                d->line_neg = 1;
        }
        break;
    }
    case 3: { /* rectangles */
        int size = (int)((op >> 3) & 3), k = 2;
        d->kind = FD_K_RECT;
        d->textured = (op & 0x04) != 0;
        d->raw = d->textured && (op & 0x01);
        d->semi = (op & 0x02) != 0;
        rgb(d, 0, c->w[0]);
        d->vx[0] = sext11(c->w[1]) + ox;
        d->vy[0] = sext11(c->w[1] >> 16) + oy;
        if (d->textured) {
            uint32_t uv = c->w[k++];
            d->u[0] = (int)(uv & 0xff);
            d->v[0] = (int)((uv >> 8) & 0xff);
            d->clut_x = (int)((uv >> 16) & 0x3f) * 16;
            d->clut_y = (int)((uv >> 22) & 0x1ff);
            d->flip_x = (int)((e1 >> 12) & 1);
            d->flip_y = (int)((e1 >> 13) & 1);
        }
        if (size == 0) {
            d->w = (int)(c->w[k] & 0x3ff);
            d->h = (int)((c->w[k] >> 16) & 0x1ff);
        } else {
            d->w = d->h = size == 1 ? 1 : size == 2 ? 8 : 16;
        }
        d->nv = 2;
        d->vx[1] = d->vx[0] + d->w - 1;
        d->vy[1] = d->vy[0] + d->h - 1;
        break;
    }
    case 4: {
        int sx = (int)(c->w[1] & 0x3ff), sy = (int)((c->w[1] >> 16) & 0x1ff);
        int dx = (int)(c->w[2] & 0x3ff), dy = (int)((c->w[2] >> 16) & 0x1ff);
        int w = (int)((((c->w[3] & 0xffff) - 1) & 0x3ff) + 1);
        int h = (int)(((((c->w[3] >> 16) & 0xffff) - 1) & 0x1ff) + 1);
        int offx = ((dx - sx) % 1024 + 1024) % 1024, offy = ((dy - sy) % 512 + 512) % 512;
        d->kind = FD_K_COPY;
        d->w = w;
        d->h = h;
        /* some pixel is both read and written: the offsets, taken either way
           round, are within the size */
        d->copy_overlap = (offx < w || 1024 - offx < w) && (offy < h || 512 - offy < h);
        d->copy_same = offx == 0 && offy == 0;
        return;
    }
    case 5:
        d->kind = FD_K_LOAD;
        d->w = (int)(c->w[2] & 0xffff);
        d->h = (int)(c->w[2] >> 16);
        return;
    case 6:
        d->kind = FD_K_READ;
        d->w = (int)(c->w[2] & 0xffff);
        d->h = (int)(c->w[2] >> 16);
        return;
    default:
        if (op == 0x02) {
            d->kind = FD_K_FILL;
            d->w = (int)(c->w[2] & 0xffff);
            d->h = (int)(c->w[2] >> 16);
        }
        return;
    }
    if (d->textured) {
        int depth = (int)((e1 >> 7) & 3);
        d->depth = depth == 0 ? 4 : depth == 1 ? 8 : 15;
        d->tpx = (int)(e1 & 0xf) * 64;
        d->tpy = (int)((e1 >> 4) & 1) * 256;
    }
    d->semi_mode = (int)((e1 >> 5) & 3);
    for (i = 0; i < d->nv; ++i) {
        int m = d->r[i] > d->g[i] ? d->r[i] : d->g[i];
        m = m > d->b[i] ? m : d->b[i];
        d->max_channel = m > d->max_channel ? m : d->max_channel;
    }
    {
        int x0 = d->vx[0], x1 = d->vx[0], y0 = d->vy[0], y1 = d->vy[0], a, b;
        for (i = 0; i < d->nv; ++i) {
            /* a rectangle's second "vertex" is its far corner, not sent */
            if ((d->kind != FD_K_RECT || i == 0) &&
                (d->vx[i] < -1024 || d->vx[i] > 1023 || d->vy[i] < -1024 || d->vy[i] > 1023))
                d->offset_wraps = 1;
            if (d->vx[i] < d->area_x0 || d->vx[i] > d->area_x1 || d->vy[i] < d->area_y0 ||
                d->vy[i] > d->area_y1)
                d->outside = 1;
            x0 = d->vx[i] < x0 ? d->vx[i] : x0;
            x1 = d->vx[i] > x1 ? d->vx[i] : x1;
            y0 = d->vy[i] < y0 ? d->vy[i] : y0;
            y1 = d->vy[i] > y1 ? d->vy[i] : y1;
        }
        if (d->kind == FD_K_QUAD) {
            /* v0 and v3 on the same side of v1-v2: the triangles overlap */
            long ex = d->vx[2] - d->vx[1], ey = d->vy[2] - d->vy[1];
            long c0 = ex * (d->vy[0] - d->vy[1]) - ey * (d->vx[0] - d->vx[1]);
            long c3 = ex * (d->vy[3] - d->vy[1]) - ey * (d->vx[3] - d->vx[1]);
            d->quad_overlap = (c0 > 0 && c3 > 0) || (c0 < 0 && c3 < 0);
        }
        if (d->kind == FD_K_TRI || d->kind == FD_K_QUAD) {
            for (a = 0; a < d->nv; ++a)
                for (b = a + 1; b < d->nv; ++b)
                    if (iabs(d->vx[a] - d->vx[b]) > 1023 || iabs(d->vy[a] - d->vy[b]) > 511)
                        d->too_big = 1;
        } else if (d->kind == FD_K_LINE || d->kind == FD_K_POLYLINE) {
            for (a = 1; a < d->nv; ++a)
                if (iabs(d->vx[a] - d->vx[a - 1]) > 1023 || iabs(d->vy[a] - d->vy[a - 1]) > 511)
                    d->too_big = 1;
        }
        /* what it can draw: its bounds within the drawing area */
        x0 = x0 < d->area_x0 ? d->area_x0 : x0;
        y0 = y0 < d->area_y0 ? d->area_y0 : y0;
        x1 = x1 > d->area_x1 ? d->area_x1 : x1;
        y1 = y1 > d->area_y1 ? d->area_y1 : y1;
        if (x0 > x1 || y0 > y1)
            ;
        else if (d->depth == 4 || d->depth == 8) {
            int n = d->depth == 4 ? 16 : 256;
            if (overlaps(x0, y0, x1, y1, d->clut_x, d->clut_y, d->clut_x + n - 1, d->clut_y) ||
                (d->clut_x + n > 1024 && overlaps(x0, y0, x1, y1, 0, d->clut_y, d->clut_x + n - 1025,
                                                  d->clut_y)))
                d->self_clut = 1;
        }
        if (d->textured && x0 <= x1 && y0 <= y1) {
            int span = d->depth == 4 ? 64 : d->depth == 8 ? 128 : 256;
            if (overlaps(x0, y0, x1, y1, d->tpx, d->tpy, d->tpx + span - 1, d->tpy + 255) ||
                (d->tpx + span > 1024 &&
                 overlaps(x0, y0, x1, y1, 0, d->tpy, d->tpx + span - 1025, d->tpy + 255)))
                d->self_tex = 1;
        }
    }
}

const char *const fd_feature_names[FD_F_COUNT] = {"dither", "mask-check", "mask-set", "window",
                                                  "semi",   "modulate",   "gouraud",  "texture",
                                                  "uv-varies", "colour-varies"};

void fd_class(const fd_desc *d, char *buf, size_t size)
{
    char f[256] = "";
    const char *coords;
    int i, any = 0;

    if (d->read) {
        snprintf(buf, size, "%s GPUREAD", fd_kind_names[d->kind]);
        return;
    }
    if (d->kind == FD_K_FILL || d->kind == FD_K_COPY || d->kind == FD_K_LOAD ||
        d->kind == FD_K_READ) {
        snprintf(buf, size, "%s%s%s%s", fd_kind_names[d->kind], d->mask_set ? " mask-set" : "",
                 d->mask_check ? " mask-check" : "", d->copy_overlap ? " overlapping" : "");
        return;
    }
    if (d->kind < FD_K_TRI || d->kind > FD_K_RECT) {
        snprintf(buf, size, "%s", fd_kind_names[d->kind]);
        return;
    }
    if (d->textured) {
        char t[32];
        snprintf(t, sizeof t, " tex%d", d->depth);
        strcat(f, t);
    }
    if (d->isolated < 0)
        ;
    else if (!d->isolated)
        strcat(f, " not-isolated");
    else {
        strcat(f, " needs{");
        for (i = 0; i < FD_F_COUNT; ++i)
            if (d->needs & (1u << i)) {
                if (any++)
                    strcat(f, ",");
                strcat(f, fd_feature_names[i]);
            }
        strcat(f, "}");
    }
    if ((d->needs & FD_F_SEMI) || (d->needs & FD_F_MODULATE)) {
        char t[16];
        snprintf(t, sizeof t, " mode%d", d->semi_mode);
        strcat(f, t);
    }
    if (d->small)
        strcat(f, " small");
    if (d->flip_x)
        strcat(f, " flip-x");
    if (d->flip_y)
        strcat(f, " flip-y");
    if (d->self_clut)
        strcat(f, " over-own-clut");
    else if (d->self_tex && (d->needs & FD_F_TEXTURE))
        strcat(f, " over-own-texels");
    if (d->kind == FD_K_LINE || d->kind == FD_K_POLYLINE) {
        if (d->coverage)
            strcat(f, " other-pixels");
        if (d->line_tie)
            strcat(f, " tie");
        if (d->line_neg && (d->needs & FD_F_GOURAUD))
            strcat(f, " step-down");
    }
    if (d->quad_overlap)
        strcat(f, " triangles-overlap");
    if (d->offset_wraps)
        coords = " | offset-wraps";
    else if (d->too_big)
        coords = " | past-limit";
    else if (d->outside)
        coords = " | crosses-area";
    else
        coords = " | inside";
    snprintf(buf, size, "%s%s%s", fd_kind_names[d->kind], f, coords);
}

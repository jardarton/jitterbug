/*
 * GP1 (jb_gp1), GPUSTAT (jb_gpustat) and the frame (jb_frame), from
 * psx-spx's "GPU" chapter.
 *
 * GPUSTAT, bit by bit, and where each comes from here:
 *   0-3    texture page X base / 64       draw state (GP0(E1h) bits 0-3, or
 *   4      texture page Y base / 256        a textured polygon's page,
 *   5-6    semi-transparency mode           which the core keeps in the
 *   7-8    texture colour depth             same state)
 *   9      dither                         GP0(E1h) bit 9
 *   10     drawing to the display area    GP0(E1h) bit 10
 *   11     set the mask bit               GP0(E6h) bit 0
 *   12     do not draw to masked pixels   GP0(E6h) bit 1
 *   13     interlace field                fixed 1 (psx-spx: always 1 without
 *                                         interlace; with it the field
 *                                         alternates, which is timing)
 *   14     "reverse" (flip horizontally)  GP1(08h) bit 7, as sent
 *   15     texture page Y base 2          GP0(E1h) bit 11, or a textured
 *          ("texture disable")              polygon's page bit 11, as
 *                                           written while GP1(09h) bit 0
 *                                           was set; written as 0 while it
 *                                           was clear
 *   16     horizontal resolution 2 (368)  GP1(08h) bit 6
 *   17-18  horizontal resolution 1        GP1(08h) bits 0-1
 *   19     vertical resolution            GP1(08h) bit 2
 *   20     video mode (PAL)               GP1(08h) bit 3
 *   21     display colour depth (24-bit)  GP1(08h) bit 4
 *   22     vertical interlace             GP1(08h) bit 5
 *   23     display disabled               GP1(03h) bit 0
 *   24     interrupt request              set by GP0(1Fh), cleared by
 *                                         GP1(02h) and GP1(00h)
 *   25     DMA / data request             by the DMA direction: 0 for 0,
 *                                         1 for 1 (the FIFO is never full),
 *                                         bit 28 for 2, bit 27 for 3
 *   26     ready to receive a command     fixed 1 (no timing)
 *   27     ready to send VRAM to CPU      1 while GP0(C0h) data remains
 *                                         unread (jb_gpuread), else 0
 *   28     ready to receive a DMA block   fixed 1 (no timing)
 *   29-30  DMA direction                  GP1(04h) bits 0-1
 *   31     drawing even/odd lines         fixed 0 (psx-spx: 0 in vblank;
 *                                         it alternates per line or field,
 *                                         which is timing)
 * After GP1(00h) that is 0x14802000, the value psx-spx gives.
 */
#include <string.h>

#include "api/api.h"
#include "core/alloc.h"

static void set_mode(struct jbi_api *a, uint32_t mode)
{
    static const int widths[4] = {256, 320, 512, 640};

    a->mode = mode & 0xff;
    a->disp_w = (mode & 0x40) ? 368 : widths[mode & 3];
    /* psx-spx: 480 lines only with interlace on (bit 5). */
    a->disp_h = (mode & 0x04) && (mode & 0x20) ? 480 : 240;
    a->rgb24 = (mode & 0x10) != 0;
}

void jbi_api_reset(jb_context *jb)
{
    struct jbi_api *a = jb->api;

    jbi_api_reset_input(a);
    jbi_clear_cache(jb); /* as GP1(01h) */
    a->irq = 0;
    a->display_off = 1;
    a->dma = 0;
    a->disp_x = a->disp_y = 0;
    /* psx-spx's reset values: x1 200h, x2 200h + 256 * 10; y1 10h,
       y2 10h + 240. */
    a->hrange = 0x200u | (0x200u + 256u * 10u) << 12;
    a->vrange = 0x10u | (0x10u + 240u) << 10;
    a->mode = 0;
    a->mode_explicit = 0;
    a->disp_w = a->cfg_w;
    a->disp_h = a->cfg_h;
    a->rgb24 = 0;
    memset(a->env, 0, sizeof a->env);
    jbi_reset_state(jb); /* GP0(E1h-E6h) all 0 */
}

/* GP1(10h): v2 GPU registers by the index's low 4 bits. */
static void latch_info(struct jbi_api *a, uint32_t index)
{
    switch (index & 0xf) {
    case 2: a->latch = a->env[0]; break; /* E2h texture window */
    case 3: a->latch = a->env[1]; break; /* E3h drawing area top left */
    case 4: a->latch = a->env[2]; break; /* E4h drawing area bottom right */
    case 5: a->latch = a->env[3]; break; /* E5h drawing offset */
    case 7: a->latch = 2; break;         /* GPU version */
    case 8: a->latch = 0; break;
    default: break;                      /* GPUREAD unchanged */
    }
}

jb_result jb_gp1(jb_context *jb, uint32_t word)
{
    struct jbi_api *a;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    a = jb->api;
    switch ((word >> 24) & 0x3f) { /* 40h-FFh mirror 00h-3Fh */
    case 0x00: jbi_api_reset(jb); break;
    case 0x01: /* psx-spx: "Resets the command buffer and CLUT cache" */
        jbi_api_reset_input(a);
        jbi_clear_cache(jb);
        break;
    case 0x02: a->irq = 0; break;
    case 0x03: a->display_off = (int)(word & 1); break;
    case 0x04: a->dma = (int)(word & 3); break;
    case 0x05:
        a->disp_x = (int)(word & 0x3ff);
        a->disp_y = (int)((word >> 10) & 0x1ff);
        break;
    case 0x06: a->hrange = word & 0xffffffu; break;
    case 0x07: a->vrange = word & 0xfffffu; break;
    case 0x08:
        set_mode(a, word);
        a->mode_explicit = 1; /* until GP1(00h): the frame leaves it (frame.c) */
        break;
    case 0x09: jb->draw.allow_tex_disable = (int)(word & 1); break;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        latch_info(a, word);
        break;
    default: break; /* the unused ones */
    }
    return JB_OK;
}

uint32_t jb_gpustat(const jb_context *jb)
{
    const jbi_draw_state *d;
    const struct jbi_api *a;
    uint32_t s, read_ready;

    if (jb == NULL)
        return 0;
    d = &jb->draw;
    a = jb->api;
    read_ready = a->rd_next < a->rd_count;
    s = (uint32_t)(d->tpage_x / 64) & 0xf;
    s |= (uint32_t)(d->tpage_y / 256 & 1) << 4;
    s |= (uint32_t)(d->semi_mode & 3) << 5;
    s |= (uint32_t)(d->tex_depth & 3) << 7;
    s |= (uint32_t)(d->dither & 1) << 9;
    s |= (uint32_t)(d->draw_display & 1) << 10;
    s |= (uint32_t)(d->set_mask != 0) << 11;
    s |= (uint32_t)(d->check_mask & 1) << 12;
    s |= 1u << 13;
    s |= ((a->mode >> 7) & 1) << 14;
    s |= (uint32_t)(d->tex_disable & 1) << 15;
    s |= ((a->mode >> 6) & 1) << 16;
    s |= (a->mode & 3) << 17;
    s |= ((a->mode >> 2) & 1) << 19;
    s |= ((a->mode >> 3) & 1) << 20;
    s |= ((a->mode >> 4) & 1) << 21;
    s |= ((a->mode >> 5) & 1) << 22;
    s |= (uint32_t)(a->display_off & 1) << 23;
    s |= (uint32_t)(a->irq & 1) << 24;
    switch (a->dma) {
    case 1: s |= 1u << 25; break;          /* write FIFO not full */
    case 2: s |= 1u << 25; break;          /* as bit 28 */
    case 3: s |= read_ready << 25; break;  /* as bit 27 */
    default: break;
    }
    s |= 1u << 26;
    s |= read_ready << 27;
    s |= 1u << 28;
    s |= (uint32_t)(a->dma & 3) << 29;
    return s;
}

/* 5 bits to 8 by repeating the top bits. */
static uint8_t widen5(unsigned c)
{
    return (uint8_t)(c << 3 | c >> 2);
}

/* n 15-bit pixels (2 bytes each, low byte first) from src into R, G, B, A
   bytes at dst; the two do not overlap. */
static void widen555(const uint8_t *restrict src, uint8_t *restrict dst, size_t n)
{
    size_t i;

    for (i = 0; i < n; ++i) {
        unsigned p = (unsigned)src[2 * i] | (unsigned)src[2 * i + 1] << 8;
        dst[4 * i] = widen5(p & 31);
        dst[4 * i + 1] = widen5((p >> 5) & 31);
        dst[4 * i + 2] = widen5((p >> 10) & 31);
        dst[4 * i + 3] = 255;
    }
}

const uint8_t *jb_frame(jb_context *jb, int *width, int *height)
{
    struct jbi_api *a;
    size_t need, reserved, i, n;
    int scale, fw = 0, fh = 0, stride = 0, format;
    uint8_t *f;

    if (width != NULL)
        *width = 0;
    if (height != NULL)
        *height = 0;
    if (jb == NULL)
        return NULL;
    a = jb->api;
    scale = jbi_scale(jb);
    need = (size_t)a->disp_w * scale * (size_t)a->disp_h * scale * 4;
    /* The frame's reserved size at this scale too (frame.c: both widths
       when widescreen is reserved), checked on every call, so that a
       change of scale since the buffer was made is caught here and not at
       a later widescreen switch. */
    reserved = (size_t)a->res_w * scale * (size_t)a->res_h * scale * 4;
    if (need > a->frame_bytes || reserved > a->frame_bytes) {
        /* The old buffer is given up only once there is a bigger one, so a
           failure here leaves every smaller frame still available. It is
           made large enough for the frame's reserved size too (frame.c:
           both widths when widescreen is reserved), so that switching
           widescreen does not allocate again. */
        size_t size = need > reserved ? need : reserved;
        uint8_t *bigger = jbi_calloc(&jb->alloc, size, 1);

        if (bigger == NULL) {
            jbi_api_fail(jb, JB_ERROR_OUT_OF_MEMORY, "jb_frame: no memory for a %d x %d frame",
                         a->disp_w * scale, a->disp_h * scale);
            return NULL;
        }
        jbi_free(&jb->alloc, a->frame);
        a->frame = bigger;
        a->frame_bytes = size;
    }
    /* The backend writes 2 (15-bit) or 3 (24-bit) bytes a pixel into the
       start of the buffer (backend.h, scanout), widened here in place,
       from the last pixel back, so no byte is written before it is
       read. */
    f = a->frame;
    format = jbi_scanout(jb, a->disp_x, a->disp_y, a->disp_w, a->disp_h, a->rgb24,
                         !a->display_off, f, &fw, &fh, &stride);
    n = (size_t)fw * (size_t)fh;
    if (format == JBI_SCANOUT_RGB24) {
        for (i = n; i-- > 0;) {
            uint8_t r = f[3 * i], g = f[3 * i + 1], b = f[3 * i + 2];
            f[4 * i] = r;
            f[4 * i + 1] = g;
            f[4 * i + 2] = b;
            f[4 * i + 3] = 255;
        }
    } else {
        /* From the back in halves: pixels lo..hi-1 with lo = ceil(hi / 2)
           read bytes 2 lo..2 hi and write 4 lo..4 hi, which do not
           overlap, so each half is one loop over separate buffers that
           the compiler can vectorise. The last few go one by one. */
        size_t hi = n;

        while (hi > 16) {
            size_t lo = (hi + 1) / 2;
            widen555(f + 2 * lo, f + 4 * lo, hi - lo);
            hi = lo;
        }
        for (i = hi; i-- > 0;) {
            unsigned p = (unsigned)f[2 * i] | (unsigned)f[2 * i + 1] << 8;
            f[4 * i] = widen5(p & 31);
            f[4 * i + 1] = widen5((p >> 5) & 31);
            f[4 * i + 2] = widen5((p >> 10) & 31);
            f[4 * i + 3] = 255;
        }
    }
    (void)stride; /* fw x 2 or x 3: the rows are not padded */
    if (width != NULL)
        *width = fw;
    if (height != NULL)
        *height = fh;
    return f;
}

/*
 * The SDK calls the recorded programs make, as the words they send: what
 * each sends was read from the PSn00bSDK source at the pinned commit
 * (pins.sh), and the recorded lists matching the console's dumps and logs
 * check it (docs/testing.md, "How `tools/conformance-record` works"). Each
 * port write is a rec_* call, in the order the SDK makes them, and each
 * status poll is a WAIT op saying what it waited for. Written for the
 * recorder from that reading; the data formats (TIM) from psx-spx.
 */
#include "rec.h"

#include <jbrec_prelude.h>
#include <psxgpu.h>
#include <psxgte.h>

#include <jbc_format.h>

#include <stdlib.h>
#include <string.h>

/* The video standard ResetGraph reads from GPUSTAT bit 20 for PutDispEnv.
   The recording reads 0 there, so NTSC (docs/testing.md, "How
   `tools/conformance-record` works",
   "Unknowns recorded"). */
static int pal;

static uint32_t halves(unsigned lo, unsigned hi)
{
    return (uint32_t)(lo & 0xffffu) | (uint32_t)(hi & 0xffffu) << 16;
}

void ResetGraph(int mode)
{
    rec_enter("ResetGraph");
    pal = (rec_read(JBC_PORT_GPUSTAT) >> 20) & 1;
    /* GP1(00h): mode 0 is the full reset; modes 1 and 3 write the word 1,
       which is GP1(00h) again as the GPU decodes it (psx-spx: GP1 commands
       are bits 24-29). */
    rec_gp1(mode == 1 || mode == 3 ? 1u : 0u);
    rec_leave();
}

int DrawSync(int m)
{
    if (m != 0)
        rec_fail("DrawSync(%d) is not modelled", m);
    rec_enter("DrawSync");
    rec_drawsync();
    rec_leave();
    return 0;
}

int VSync(int m)
{
    if (m != 0)
        rec_fail("VSync(%d) is not modelled", m);
    mmio_flush();
    rec_vsync();
    return 0;
}

void SetDispMask(int mask)
{
    rec_enter("SetDispMask");
    /* GP1(03h): bit 0 set turns the display off. */
    rec_gp1(0x03000000u | (uint32_t)((mask & 1) ^ 1));
    rec_leave();
}

void DrawPrim(void *pri)
{
    const unsigned char *bytes = pri;
    uint32_t w[255];
    unsigned n = bytes[3], i;

    rec_enter("DrawPrim");
    rec_drawsync();
    rec_gp1(0x04000000u); /* GP1(04h): no DMA */
    /* The words after the tag, and one even when the length says none. */
    n = n ? n : 1;
    for (i = 0; i < n; ++i)
        memcpy(&w[i], bytes + 4 * (i + 1), 4);
    rec_gp0(w, n, 0);
    rec_drawsync();
    rec_leave();
}

void LoadImage(RECT *rect, unsigned int *data)
{
    uint32_t head[4], pixels = (uint32_t)(uint16_t)rect->w * (uint16_t)rect->h;

    rec_enter("LoadImage");
    rec_wait(0x14000000u, 0x14000000u); /* ready for commands (26) and DMA (28) */
    rec_gp1(0x04000000u);               /* no DMA */
    head[0] = 0x01000000u;              /* GP0(01h), clear the texture cache */
    head[1] = 0xa0000000u;              /* GP0(A0h), CPU to VRAM */
    head[2] = halves((unsigned)rect->x, (unsigned)rect->y);
    head[3] = halves((unsigned)rect->w, (unsigned)rect->h);
    rec_gp0(head, 4, 0);
    rec_gp1(0x04000002u); /* DMA, CPU to GP0 */
    /* Blocks of 16 pixels (8 words), as many as fit, in a 16-bit count: a
       count of 0 would be taken as 65536 blocks, so fewer than 16 pixels is
       refused. */
    if (((pixels / 16) & 0xffffu) == 0)
        rec_fail("LoadImage of %d x %d: fewer than 16 pixels is not modelled", rect->w, rect->h);
    rec_gp0((const uint32_t *)data, ((pixels / 16) & 0xffffu) * 8u, 1);
    rec_leave();
}

void PutDrawEnv(DRAWENV *draw)
{
    uint32_t pkt[7];
    unsigned left = (uint16_t)draw->clip.x, top = (uint16_t)draw->clip.y;
    unsigned width = (uint16_t)draw->clip.w, height = (uint16_t)draw->clip.h;
    size_t n = 0;

    rec_enter("PutDrawEnv");
    /* GP0(E3h) and (E4h), the drawing area's corners; (E5h), the offset,
       11 bits each, from the area's corner; (E1h), the draw mode. */
    pkt[n++] = 0xe3000000u | (left & 0x3ffu) | (top & 0x1ffu) << 10;
    pkt[n++] = 0xe4000000u | ((left + width - 1) & 0x3ffu) | ((top + height - 1) & 0x1ffu) << 10;
    pkt[n++] = 0xe5000000u | ((left + (uint16_t)draw->ofs[0]) & 0x7ffu) |
               ((top + (uint16_t)draw->ofs[1]) & 0x7ffu) << 11;
    pkt[n++] = 0xe1000000u | draw->tpage | (draw->dtd & 1u) << 9 | (draw->dfe & 1u) << 10;
    if (draw->isbg) {
        /* GP0(02h), a fill of the area; its height is cut to 511. */
        pkt[n++] = 0x02000000u | draw->r0 | (uint32_t)draw->g0 << 8 | (uint32_t)draw->b0 << 16;
        pkt[n++] = halves(left, top);
        pkt[n++] = halves(width, height < 511 ? height : 511);
    }
    rec_wait(0x04000000u, 0x04000000u); /* ready for commands */
    /* Sent as a one-packet linked list (DrawOTag). */
    rec_gp1(0x04000002u);
    rec_wait(0x04000000u, 0x04000000u);
    rec_gp0(pkt, n, 1);
    rec_leave();
}

/* The SDK's horizontal set-up for each display width: the widest first;
   a width above `over` takes the row. The GP1(08h) bits are the
   horizontal resolution (psx-spx: bits 0-1 and 6); `dots` is the video
   clock's dots per pixel, used for the screen width; `x1` is the start
   of the display range; `span` the range's length when the screen width
   is 0. The 368-wide row also sets bit 5 (interlace) and bit 2 (480
   lines), as the SDK does; they are or'ed in with the rest. */
static const struct {
    int over;
    uint32_t mode, dots, x1, span;
} horizontal[] = {
    {560, 0x03, 4, 0x26c, 0xa00}, /* 640 */
    {400, 0x02, 5, 0x267, 0xa00}, /* 512 */
    {352, 0x64, 7, 0x21b, 0xa80}, /* 368 */
    {280, 0x01, 8, 0x258, 0xa00}, /* 320 */
    {-32769, 0x00, 10, 0x24e, 0xa00}, /* 256 */
};

void PutDispEnv(DISPENV *disp)
{
    size_t row = 0;
    uint32_t mode, x1, x2, y1, y2;

    rec_enter("PutDispEnv");
    while (disp->disp.w <= horizontal[row].over)
        ++row;
    mode = horizontal[row].mode;

    /* GP1(06h), the horizontal display range, 12 bits each. */
    x1 = horizontal[row].x1 + 4u * (uint32_t)disp->screen.x;
    x2 = horizontal[row].dots * (uint32_t)disp->screen.w;
    x2 = x1 + (x2 ? x2 : horizontal[row].span);
    rec_gp1(0x06000000u | (x1 & 0xfffu) | x2 << 12);

    /* GP1(07h), the vertical range from line 16, 240 lines when the screen
       height is 0. */
    y1 = 16u + (uint32_t)disp->screen.y;
    y2 = y1 + (disp->screen.h ? (uint32_t)disp->screen.h : 240u);
    rec_gp1(0x07000000u | (y1 & 0x3ffu) | y2 << 10);

    /* GP1(08h), the display mode: 480 lines (bit 2), PAL (3), 24-bit colour
       (4), interlace (5); bit 7 is set with interlace too, as the SDK
       does (its `reverse` field is not used). */
    if (disp->disp.h > 256)
        mode |= 0x04;
    if (pal)
        mode |= 0x08;
    if (disp->isrgb24)
        mode |= 0x10;
    if (disp->isinter)
        mode |= 0x20 | 0x80;
    rec_gp1(0x08000000u | mode);

    /* GP1(05h), the display's start in VRAM. */
    rec_gp1(0x05000000u | ((uint32_t)(uint16_t)disp->disp.x & 0x3ffu) |
            ((uint32_t)(uint16_t)disp->disp.y & 0x1ffu) << 10);
    rec_leave();
}

DISPENV *SetDefDispEnv(DISPENV *disp, int x, int y, int w, int h)
{
    /* The given area shown, the defaults (0) for everything else. */
    DISPENV d;

    memset(&d, 0, sizeof d);
    d.disp.x = (short)x;
    d.disp.y = (short)y;
    d.disp.w = (short)w;
    d.disp.h = (short)h;
    *disp = d;
    return disp;
}

DRAWENV *SetDefDrawEnv(DRAWENV *draw, int x, int y, int w, int h)
{
    /* The given drawing area, no offset, no texture window, no fill;
       dithering on and the texture page at (640, 0), the SDK's defaults. */
    DRAWENV d;

    memset(&d, 0, sizeof d);
    d.clip.x = (short)x;
    d.clip.y = (short)y;
    d.clip.w = (short)w;
    d.clip.h = (short)h;
    d.tpage = 0x0a;
    d.dtd = 1;
    *draw = d;
    return draw;
}

/*
 * TIM, the console's image file (psx-spx, "CDROM File Formats", TIM). All
 * fields are little-endian. A file starts with two words: the ID word
 * (10h in its low byte, version 0 in the next) and the flags word (the
 * pixel mode in bits 0-2, bit 3 set when a CLUT block follows). Then the
 * CLUT block, if any, and the image block, each made of a word giving the
 * block's length in bytes (this word and the next two included), the
 * block's place in VRAM (X and Y, 16 bits each), its size in halfwords
 * (width and height, 16 bits each), and the data.
 *
 * GetTimInfo gives where those parts are inside the file: 0, or 1 for a
 * file without the ID, 2 for another version.
 */
typedef struct {
    const unsigned char *rect; /* X, Y, width, height */
    const unsigned char *data;
} tim_block;

static uint32_t le32(const unsigned char *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static const unsigned char *tim_block_at(const unsigned char *at, tim_block *b)
{
    enum { LENGTH = 4, RECT_BYTES = 8 };

    b->rect = at + LENGTH;
    b->data = at + LENGTH + RECT_BYTES;
    return at + le32(at); /* the next block */
}

int GetTimInfo(unsigned int *tim, TIM_IMAGE *timimg)
{
    const unsigned char *file = (const unsigned char *)tim, *at;
    uint32_t id = le32(file), flags = le32(file + 4);
    tim_block clut = {NULL, NULL}, image;

    if ((id & 0xff) != 0x10)
        return 1;
    if ((id >> 8 & 0xff) != 0)
        return 2;
    at = file + 8;
    if (flags & 8)
        at = tim_block_at(at, &clut);
    tim_block_at(at, &image);

    timimg->mode = flags;
    timimg->crect = (RECT *)(uintptr_t)clut.rect;
    timimg->caddr = (unsigned int *)(uintptr_t)clut.data;
    timimg->prect = (RECT *)(uintptr_t)image.rect;
    timimg->paddr = (unsigned int *)(uintptr_t)image.data;
    return 0;
}

/* The SDK's debug font: a 4-bit TIM (128 x 32 texels and a 16-colour
   CLUT), read once from JBREC_FONT, the SDK's psxgpu/dbugfont.tim at the
   pin, as words. Only vram-to-vram-overlap loads it (FntLoad), so its
   list holds PSn00bSDK's data (MPL-2.0) and is fetched-only. */
static unsigned int *debug_font(void)
{
    static unsigned int *words;
    const char *path = getenv("JBREC_FONT");
    TIM_IMAGE parts;
    unsigned char *bytes;
    size_t size;

    if (words != NULL)
        return words;
    if (path == NULL)
        rec_fail("FntLoad: JBREC_FONT is not set");
    bytes = rec_read_file(path, &size);
    words = calloc(size / 4 + 1, 4);
    if (words == NULL)
        rec_fail("out of memory");
    memcpy(words, bytes, size);
    free(bytes);
    /* Two block headers at least, and both blocks' data in the file. */
    if (size < 8 + 2 * 12 || GetTimInfo(words, &parts) != 0 || parts.caddr == NULL ||
        (const unsigned char *)parts.paddr > (const unsigned char *)words + size)
        rec_fail("FntLoad: %s is not a TIM with a CLUT", path);
    return words;
}

/* FntLoad puts the font's image with its top-left corner at (x, y) and its
   CLUT at x on the line after the image's last, each by a LoadImage of the
   part's own size followed by a DrawSync, the image first. */
void FntLoad(int x, int y)
{
    TIM_IMAGE font;
    struct {
        const RECT *size;
        unsigned int *data;
        int below; /* lines from y */
    } part[2];
    size_t i;

    GetTimInfo(debug_font(), &font);
    rec_enter("FntLoad");
    part[0].size = font.prect;
    part[0].data = font.paddr;
    part[0].below = 0;
    part[1].size = font.crect;
    part[1].data = font.caddr;
    part[1].below = font.prect->h;
    for (i = 0; i < 2; ++i) {
        RECT to = {(short)x, (short)(y + part[i].below), part[i].size->w, part[i].size->h};
        LoadImage(&to, part[i].data);
        DrawSync(0);
    }
    rec_leave();
}

void InitGeom(void)
{
    /* The GTE's set-up: nothing for the GPU. */
}

/*
 * The SDK's integer sine (4096 a full turn, 4096 for 1). It is not the
 * nearest integer to 4096 sin: the SDK evaluates the fourth-order
 * polynomial approximation that J. Vijn describes ("Another fast
 * fixed-point sine approximation", coranac.com, 2009, the "S4" form),
 *
 *   sin = 1 - d^2 (B - d^2 C),  d the distance from the nearest peak in
 *                               quarter turns (-1 to 1),
 *
 * with B = 19900 / 2^14 and C = 3516 / 2^14 scaled as below, truncating at
 * each step, and the sign of the half turn. The values matter: lines draws
 * its circle with them. Checked against the exact sine, 4096 sin(2 pi a /
 * 4096) truncated, which gives the same 64 circle coordinates after lines'
 * division by 140; over a whole turn isin is within 12 of 4096 sin; and
 * lines' VRAM matches the console's dump.
 */
int isin(int a)
{
    uint32_t u = (uint32_t)a;
    /* The distance from the peak (a quarter turn, 1024, into the half turn
       a is in), from -1024 to 1023. */
    int32_t d = (int32_t)(u & 2047u) - 1024;
    uint32_t d2 = (uint32_t)(d * d) / 64u;            /* d^2, 14 fraction bits */
    uint32_t inner = 19900u - d2 * 3516u / 16384u;    /* B - d^2 C */
    int32_t y = 4096 - (int32_t)(d2 * inner / 65536u); /* 1 - d^2 (B - d^2 C) */

    return (u & 2048u) ? -y : y;
}

int icos(int a)
{
    return isin((int)((uint32_t)a + 1024u));
}

/* The SDK's rand: a linear congruential generator from a seed of 1 that
   returns the low 15 bits of the new state (not the high bits, as many C
   libraries do). */
static uint32_t rand_state = 1;

int jbrec_rand(void)
{
    rand_state = rand_state * 1103515245u + 12345u;
    return (int)(rand_state & 0x7fffu);
}

void jbrec_srand(unsigned int seed)
{
    rand_state = seed;
}

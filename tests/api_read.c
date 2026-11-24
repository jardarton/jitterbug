/*
 * The read side and the status register of the public API:
 *   - CPU-to-VRAM then VRAM-to-CPU round trips: odd sizes, size 0 as the
 *     maximum, wrapping at the right and bottom edges, the padding of an
 *     odd pixel count, coordinates and sizes reduced as the console does,
 *     and the mask settings applied to loads;
 *   - GPUREAD's latch: the last word read, GP1(10h)'s registers and
 *     mirrors, a VRAM-to-CPU snapshot unaffected by later drawing, GP1(01h)
 *     dropping unread data;
 *   - GP1(01h) dropping a partial command and the rest of a load;
 *   - GPUSTAT after each GP0 and GP1 command that changes it, bit by bit
 *     (include/jitterbug.h, src/api/display.c), the interrupt flag, and
 *     the reset value.
 */
#include "api_test.h"

static jb_context *fresh(void)
{
    jb_config c;

    memset(&c, 0, sizeof c);
    return must_create(&c);
}

static uint16_t pattern(int k)
{
    return (uint16_t)(k * 0x9e37u + 0x1234u);
}

/* Loads w x h at (x, y) and reads it back; also checks each pixel landed
   where the wrapping puts it, by reading pixels one at a time. */
static void round_trip(int x, int y, int w, int h)
{
    jb_context *jb = fresh();
    size_t n = (size_t)xfer_w(w) * (size_t)xfer_h(h), i;
    uint16_t *src = must_alloc(n * 2), *got = must_alloc(n * 2 + 2);
    char msg[128];
    int bad = 0, bad_place = 0, k;

    for (i = 0; i < n; ++i)
        src[i] = pattern((int)i);
    load_rect(jb, x, y, w, h, src);
    snprintf(msg, sizeof msg, "round trip (%d, %d) %d x %d: pixels read", x, y, w, h);
    check(msg, (long long)read_rect(jb, x, y, w, h, got), (long long)n);
    for (i = 0; i < n; ++i)
        bad += got[i] != src[i];
    snprintf(msg, sizeof msg, "round trip (%d, %d) %d x %d: pixels that differ", x, y, w, h);
    check(msg, bad, 0);
    /* A sample of single pixels, where wrapping put them. */
    for (k = 0; k < 64; ++k) {
        size_t at = (size_t)k * (n - 1) / 63;
        int px = (int)(at % (size_t)xfer_w(w)), py = (int)(at / (size_t)xfer_w(w));
        uint16_t one[2];
        read_rect(jb, ((x & 1023) + px) & 1023, ((y & 511) + py) & 511, 1, 1, one);
        bad_place += one[0] != src[at];
    }
    snprintf(msg, sizeof msg, "round trip (%d, %d) %d x %d: pixels in their wrapped places", x,
             y, w, h);
    check(msg, bad_place, 0);
    check("no read data left", (jb_gpustat(jb) >> 27) & 1, 0);
    free(src);
    free(got);
    jb_destroy(jb);
}

static void round_trips(void)
{
    round_trip(0, 0, 1, 1);
    round_trip(10, 20, 3, 1);
    round_trip(100, 50, 7, 5);
    round_trip(512, 256, 16, 16);
    round_trip(0, 0, 0, 1);        /* 1024 wide */
    round_trip(0, 3, 1, 0);        /* 512 high */
    round_trip(1020, 100, 9, 3);   /* wraps right */
    round_trip(40, 509, 5, 7);     /* wraps at the bottom */
    round_trip(1021, 510, 11, 5);  /* both */
    round_trip(1024 + 5, 512 + 6, 1025, 515); /* reduced: (5, 6), 1 x 3 */
    round_trip(300, 200, 0, 0);    /* all of VRAM, from (300, 200) */
}

static void padding_and_latch(void)
{
    jb_context *jb = fresh();
    const uint16_t px[4] = {0x1111, 0x2222, 0x3333, 0x4444};
    uint32_t cmd[3] = {0xc0000000u, xy(0, 0), xy(3, 1)};
    uint32_t a, b;

    load_rect(jb, 0, 0, 4, 1, px);
    gp0(jb, cmd, 3);
    check("GPUSTAT bit 27 set while data remains", (jb_gpustat(jb) >> 27) & 1, 1);
    a = jb_gpuread(jb);
    check("GPUSTAT bit 27 still set", (jb_gpustat(jb) >> 27) & 1, 1);
    b = jb_gpuread(jb);
    check("first word: pixels 0 and 1", (long long)a, 0x22221111);
    check("last word of an odd count: 0 in the high half", (long long)b, 0x00003333);
    check("GPUSTAT bit 27 clear when all is read", (jb_gpustat(jb) >> 27) & 1, 0);
    check("GPUREAD then repeats the last word", (long long)jb_gpuread(jb), 0x00003333);
    check("and again", (long long)jb_gpuread(jb), 0x00003333);
    /* A new C0h replaces unread data. */
    cmd[2] = xy(4, 1);
    gp0(jb, cmd, 3);
    cmd[1] = xy(2, 0);
    gp0(jb, cmd, 3);
    check("a new read replaces the old", (long long)jb_gpuread(jb), 0x44443333);
    /* GP1(01h) drops unread data; GPUREAD keeps its last value. */
    cmd[1] = xy(0, 0);
    gp0(jb, cmd, 3);
    gp1(jb, 0x01000000u);
    check("GP1(01h): no data left (bit 27)", (jb_gpustat(jb) >> 27) & 1, 0);
    check("GP1(01h): GPUREAD keeps its value", (long long)jb_gpuread(jb), 0x44443333);
    jb_destroy(jb);
}

/* The read is the rectangle as it was when C0h completed. */
static void snapshot(void)
{
    jb_context *jb = fresh();
    const uint16_t px[2] = {0x0123, 0x0456};
    const uint32_t read[3] = {0xc0000000u, xy(8, 8), xy(2, 1)};
    const uint32_t fill[3] = {0x02ffffffu, xy(0, 0), xy(16, 16)};
    uint16_t now[2];

    load_rect(jb, 8, 8, 2, 1, px);
    gp0(jb, read, 3);
    gp0(jb, fill, 3);
    check("data from before the fill", (long long)jb_gpuread(jb), 0x04560123);
    read_rect(jb, 8, 8, 2, 1, now);
    check("a new read sees the fill", now[0] * 0x10000 + now[1], 0x7fff7fff);
    jb_destroy(jb);
}

/* Loads with the mask settings: set adds bit 15, check skips masked
   pixels. A read never changes anything. */
static void load_masks(void)
{
    jb_context *jb = fresh();
    const uint16_t a[2] = {0x0001, 0x8002}, b[2] = {0x0003, 0x0004};
    uint16_t got[2];

    load_rect(jb, 0, 0, 2, 1, a);
    gp0_1(jb, 0xe6000002u); /* check */
    load_rect(jb, 0, 0, 2, 1, b);
    read_rect(jb, 0, 0, 2, 1, got);
    check("check mask: unmasked pixel written", got[0], 0x0003);
    check("check mask: masked pixel kept", got[1], 0x8002);
    gp0_1(jb, 0xe6000001u); /* set */
    load_rect(jb, 0, 0, 2, 1, b);
    read_rect(jb, 0, 0, 2, 1, got);
    check("set mask: bit 15 added", got[0], 0x8003);
    check("set mask: bit 15 added (2)", got[1], 0x8004);
    jb_destroy(jb);
}

static void gp1_info(void)
{
    jb_context *jb = fresh();

    check("GPUREAD at first", (long long)jb_gpuread(jb), 0);
    gp0_1(jb, 0xe2fedcbau);
    gp0_1(jb, 0xe3fffffeu); /* 20 bits on a v2 GPU */
    gp0_1(jb, 0xe4abcdefu);
    gp0_1(jb, 0xe5ffffffu);
    gp1(jb, 0x10000002u);
    check("GP1(10h) 2: texture window, 20 bits", (long long)jb_gpuread(jb), 0xedcba);
    gp1(jb, 0x10000003u);
    check("GP1(10h) 3: draw area top left, 20 bits", (long long)jb_gpuread(jb), 0xffffe);
    gp1(jb, 0x10000004u);
    check("GP1(10h) 4: draw area bottom right", (long long)jb_gpuread(jb), 0xbcdef);
    gp1(jb, 0x10000005u);
    check("GP1(10h) 5: draw offset, 22 bits", (long long)jb_gpuread(jb), 0x3fffff);
    gp1(jb, 0x10000007u);
    check("GP1(10h) 7: GPU version", (long long)jb_gpuread(jb), 2);
    gp1(jb, 0x10000000u);
    check("GP1(10h) 0: unchanged", (long long)jb_gpuread(jb), 2);
    gp1(jb, 0x10000001u);
    gp1(jb, 0x10000006u);
    gp1(jb, 0x10000009u);
    gp1(jb, 0x1000000fu);
    check("GP1(10h) 1, 6, 9, Fh: unchanged", (long long)jb_gpuread(jb), 2);
    gp1(jb, 0x10000008u);
    check("GP1(10h) 8: zero", (long long)jb_gpuread(jb), 0);
    gp1(jb, 0x10000012u);
    check("GP1(10h) 12h mirrors 2", (long long)jb_gpuread(jb), 0xedcba);
    gp1(jb, 0x1fabcd05u);
    check("GP1(1Fh) mirrors 10h; index ABCD05h mirrors 5", (long long)jb_gpuread(jb), 0x3fffff);
    gp1(jb, 0x50000007u);
    check("GP1(50h) mirrors 10h", (long long)jb_gpuread(jb), 2);
    /* The latch is not updated by later GP0 commands. */
    gp1(jb, 0x10000002u);
    gp0_1(jb, 0xe2000000u);
    check("the latch keeps its value", (long long)jb_gpuread(jb), 0xedcba);
    /* GP1(00h) resets E2h-E5h. */
    gp1(jb, 0x00000000u);
    gp1(jb, 0x10000005u);
    check("after GP1(00h): draw offset 0", (long long)jb_gpuread(jb), 0);
    jb_destroy(jb);
}

/* GP1(01h) mid-command and mid-load. */
static void reset_command_buffer(void)
{
    jb_context *jb = fresh();
    const uint32_t part[2] = {0x20ff0000u, xy(0, 0)};
    const uint32_t rect[3] = {0x6000ff00u, xy(50, 50), xy(2, 2)};
    const uint32_t load[3] = {0xa0000000u, xy(100, 100), xy(4, 4)};
    uint16_t got[16];
    int i;

    gp0_1(jb, 0xe4000000u | 1023u | 511u << 10); /* the drawing area: all of VRAM */
    gp0(jb, part, 2);
    gp1(jb, 0x01000000u);
    gp0(jb, rect, 3); /* taken as a command, not the triangle's vertices */
    read_rect(jb, 50, 50, 1, 1, got);
    check("GP1(01h) dropped the partial triangle", got[0], 0x03e0);
    gp0(jb, load, 3);
    for (i = 0; i < 4; ++i)
        gp0_1(jb, 0x7c007c00u); /* two rows of 4 */
    gp1(jb, 0x01000000u);
    gp0(jb, rect, 3);
    read_rect(jb, 100, 100, 4, 4, got);
    check("rows that arrived were written (row 1)", got[4], 0x7c00);
    check("rows after GP1(01h) were not (row 2)", got[8], 0);
    read_rect(jb, 50, 50, 1, 1, got);
    check("the next word is a command again", got[0], 0x03e0);
    jb_destroy(jb);
}

static void status(void)
{
    jb_context *jb = fresh();
    const uint32_t reset_value = 0x14802000u;
    uint32_t s;
    int v;

    check("GPUSTAT of a new context", (long long)jb_gpustat(jb), reset_value);

    /* GP0(E1h): bits 0-10; E1h bits 12-13 (rectangle flips) are not
       shown, bit 11 (2 MB only) reads 0 in bit 15. */
    for (v = 0; v < 0x4000; v += 0x155) {
        gp0_1(jb, 0xe1000000u | (uint32_t)v);
        s = jb_gpustat(jb);
        check("GP0(E1h) in bits 0-10", s & 0x7ff, v & 0x7ff);
        check("bits 11-15 untouched by E1h", (long long)(s & 0xf800u), 0x2000);
    }
    gp0_1(jb, 0xe1000fffu);
    check("GP0(E1h) bit 11 without GP1(09h): GPUSTAT bit 15 stays 0", (jb_gpustat(jb) >> 15) & 1,
          0);
    /* GP1(09h) bit 0 lets bit 11 through (ps1-tests' gp0-e1 has the
       console's answers): stored by E1h and by a textured polygon's page,
       kept when GP1(09h) is cleared, written as 0 by the next E1h, and not
       reset by GP1(00h). */
    gp1(jb, 0x09000001u);
    check("GP1(09h) alone changes nothing", (jb_gpustat(jb) >> 15) & 1, 0);
    gp0_1(jb, 0xe1000800u);
    check("GP0(E1h) bit 11 with GP1(09h): GPUSTAT bit 15", (jb_gpustat(jb) >> 15) & 1, 1);
    gp1(jb, 0x09000000u);
    check("GP1(09h) cleared: bit 15 kept", (jb_gpustat(jb) >> 15) & 1, 1);
    gp0_1(jb, 0xe1000800u);
    check("GP0(E1h) bit 11 after GP1(09h) cleared: bit 15 written 0", (jb_gpustat(jb) >> 15) & 1,
          0);
    gp1(jb, 0x09000001u);
    gp1(jb, 0x00000000u);
    gp0_1(jb, 0xe1000800u);
    check("GP1(09h) survives GP1(00h)", (jb_gpustat(jb) >> 15) & 1, 1);
    {
        const uint32_t tri[7] = {0x24808080u, xy(0, 0),  0x00000000u,          xy(4, 0),
                                 0x00000000u, xy(0, 4), 0x00000000u};
        gp0(jb, tri, 7);
        check("a textured polygon's page bit 11 clear: bit 15 0", (jb_gpustat(jb) >> 15) & 1, 0);
    }
    gp1(jb, 0x09000000u);
    gp0_1(jb, 0xe1000000u);
    /* A textured polygon's page sets bits 0-8 and keeps 9-10. */
    gp0_1(jb, 0xe1000600u); /* dither, draw to display */
    {
        const uint32_t tri[7] = {0x24808080u, xy(0, 0),  0x00000000u,          xy(4, 0),
                                 (0x1a5u << 16),  xy(0, 4), 0x00000000u};
        gp0(jb, tri, 7);
    }
    check("a textured polygon's page in bits 0-8", (long long)(jb_gpustat(jb) & 0x7ff),
          0x600 | 0x1a5);
    /* GP0(E6h): bits 11-12. */
    gp0_1(jb, 0xe6000001u);
    check("GP0(E6h) set mask: bit 11", (jb_gpustat(jb) >> 11) & 3, 1);
    gp0_1(jb, 0xe6000002u);
    check("GP0(E6h) check mask: bit 12", (jb_gpustat(jb) >> 11) & 3, 2);
    gp0_1(jb, 0xe6000003u);
    check("GP0(E6h) both", (jb_gpustat(jb) >> 11) & 3, 3);
    /* GP1(08h): bit 7 in 14, bit 6 in 16, bits 0-5 in 17-22. */
    for (v = 0; v < 256; ++v) {
        gp1(jb, 0x08000000u | (uint32_t)v);
        s = jb_gpustat(jb);
        check("GP1(08h) bits 0-5 in 17-22", (s >> 17) & 0x3f, v & 0x3f);
        check("GP1(08h) bit 6 in 16", (s >> 16) & 1, (v >> 6) & 1);
        check("GP1(08h) bit 7 in 14", (s >> 14) & 1, (v >> 7) & 1);
    }
    gp1(jb, 0x08000000u);
    /* GP1(03h): bit 23. */
    gp1(jb, 0x03000000u);
    check("GP1(03h) 0: display enabled (bit 23 clear)", (jb_gpustat(jb) >> 23) & 1, 0);
    gp1(jb, 0x03000001u);
    check("GP1(03h) 1: display disabled (bit 23 set)", (jb_gpustat(jb) >> 23) & 1, 1);
    gp1(jb, 0x03fffffeu);
    check("GP1(03h) only bit 0 counts", (jb_gpustat(jb) >> 23) & 1, 0);
    /* The interrupt: GP0(1Fh) sets bit 24, GP1(02h) clears it. */
    gp0_1(jb, 0x1f000000u);
    check("GP0(1Fh) sets bit 24", (jb_gpustat(jb) >> 24) & 1, 1);
    gp0_1(jb, 0x1f000000u);
    check("twice: still set", (jb_gpustat(jb) >> 24) & 1, 1);
    gp1(jb, 0x02000000u);
    check("GP1(02h) clears it", (jb_gpustat(jb) >> 24) & 1, 0);
    gp0_1(jb, 0x1f123456u);
    gp1(jb, 0x01000000u);
    check("GP1(01h) leaves it", (jb_gpustat(jb) >> 24) & 1, 1);
    /* GP1(04h): bits 29-30, and bit 25 by direction. */
    for (v = 0; v < 4; ++v) {
        uint32_t read[3] = {0xc0000000u, xy(0, 0), xy(2, 1)};

        gp1(jb, 0x04000000u | (uint32_t)v | 0xfffffcu);
        s = jb_gpustat(jb);
        check("GP1(04h) in bits 29-30", (s >> 29) & 3, v);
        check("bit 25 without read data", (s >> 25) & 1, v == 1 || v == 2);
        gp0(jb, read, 3);
        check("bit 25 with read data", (jb_gpustat(jb) >> 25) & 1, v != 0);
        jb_gpuread(jb);
    }
    /* Fixed bits: 13, 26 and 28 set, 31 clear. */
    s = jb_gpustat(jb);
    check("bit 13 (interlace field) set", (s >> 13) & 1, 1);
    check("bit 26 (ready for a command) set", (s >> 26) & 1, 1);
    check("bit 28 (ready for DMA) set", (s >> 28) & 1, 1);
    check("bit 31 clear", (s >> 31) & 1, 0);
    /* Unknown GP1 commands change nothing. */
    {
        static const uint32_t unknown[] = {0x0a000000u, 0x0bffffffu, 0x0f000000u,
                                           0x20000504u, 0x3f000000u, 0x4a000001u};
        unsigned k;
        uint32_t before = jb_gpustat(jb), latch = jb_gpuread(jb);

        for (k = 0; k < sizeof unknown / sizeof unknown[0]; ++k)
            gp1(jb, unknown[k]);
        check("unknown GP1 commands: GPUSTAT", (long long)jb_gpustat(jb), (long long)before);
        check("unknown GP1 commands: GPUREAD", (long long)jb_gpuread(jb), (long long)latch);
    }
    /* GP1(40h-FFh) mirror 00h-3Fh. */
    gp1(jb, 0x83000001u);
    check("GP1(83h) is GP1(03h)", (jb_gpustat(jb) >> 23) & 1, 1);
    gp1(jb, 0xc8000025u);
    check("GP1(C8h) is GP1(08h)", (jb_gpustat(jb) >> 17) & 0x3f, 0x25);
    /* GP1(00h) resets everything above. */
    gp0_1(jb, 0x1f000000u);
    gp0_1(jb, 0xe1000fffu);
    gp0_1(jb, 0xe6000003u);
    gp1(jb, 0x04000003u);
    gp1(jb, 0x40000000u); /* GP1(00h) as its mirror 40h */
    check("GPUSTAT after GP1(00h)", (long long)jb_gpustat(jb), reset_value);
    jb_destroy(jb);
}

int main(void)
{
    round_trips();
    padding_and_latch();
    snapshot();
    load_masks();
    gp1_info();
    reset_command_buffer();
    status();
    return finish("api_read");
}

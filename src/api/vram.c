/*
 * jb_vram_* (include/jitterbug.h): VRAM transfers as calls.
 *
 * Each does what its GP0 command does through jb_gp0 (stream.c):
 *   load  GP0(A0h): jbi_load with the whole rectangle. The stream writes it
 *         a row at a time as the pixels arrive, with the same mask
 *         settings; the backend writes rows in order and nothing is queued
 *         between them, so the pixels are the same.
 *   read  GP0(C0h): jbi_store_1x, as the stream's start_read, into the
 *         caller's buffer rather than GPUREAD's.
 *   fill  GP0(02h) and copy GP0(80h): their words, through jbi_api_command.
 * Arguments outside the fields' ranges are refused (the range rule).
 * Nothing here allocates.
 */
#include "api/api.h"

/* The range rule for a transfer's rectangle; fills have their own sizes. */
static jb_result check_corner(jb_context *jb, const char *fn, const char *what, int x, int y)
{
    if (x < 0 || x > JBI_VRAM_W - 1 || y < 0 || y > JBI_VRAM_H - 1)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: %s (%d, %d) is outside 0..%d x 0..%d", fn, what, x, y,
                            JBI_VRAM_W - 1, JBI_VRAM_H - 1);
    return JB_OK;
}

static jb_result check_size(jb_context *jb, const char *fn, int w, int h)
{
    if (w < 1 || w > JBI_VRAM_W || h < 1 || h > JBI_VRAM_H)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: size %d x %d is outside 1..%d x 1..%d", fn, w, h, JBI_VRAM_W,
                            JBI_VRAM_H);
    return JB_OK;
}

static jb_result check_rect(jb_context *jb, const char *fn, int x, int y, int w, int h,
                            const void *pixels)
{
    jb_result r = check_corner(jb, fn, "position", x, y);

    if (r == JB_OK)
        r = check_size(jb, fn, w, h);
    if (r == JB_OK && pixels == NULL)
        r = jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: pixels is NULL", fn);
    return r;
}

static uint32_t pack(int lo, int hi)
{
    return ((uint32_t)lo & 0xffffu) | ((uint32_t)hi & 0xffffu) << 16;
}

jb_result jb_vram_load(jb_context *jb, int x, int y, int w, int h, const uint16_t *pixels)
{
    jb_result r;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    r = check_rect(jb, "jb_vram_load", x, y, w, h, pixels);
    if (r != JB_OK)
        return r;
    jbi_load(jb, x, y, w, h, pixels);
    return JB_OK;
}

jb_result jb_vram_read(jb_context *jb, int x, int y, int w, int h, uint16_t *pixels)
{
    jb_result r;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    r = check_rect(jb, "jb_vram_read", x, y, w, h, pixels);
    if (r != JB_OK)
        return r;
    jbi_store_1x(jb, x, y, w, h, pixels);
    return JB_OK;
}

jb_result jb_vram_fill(jb_context *jb, int x, int y, int w, int h, uint32_t color)
{
    uint32_t words[3];
    jb_result r;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    r = check_corner(jb, "jb_vram_fill", "position", x, y);
    if (r != JB_OK)
        return r;
    if (w < 0 || w > 0x3ff || h < 0 || h > 0x1ff)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_vram_fill: size %d x %d is outside 0..1023 x 0..511", w, h);
    if (color > 0xffffffu)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_vram_fill: color 0x%lx is above 0xffffff", (unsigned long)color);
    words[0] = 0x02000000u | color;
    words[1] = pack(x, y);
    words[2] = pack(w, h);
    jbi_api_command(jb, words, 3);
    return JB_OK;
}

jb_result jb_vram_copy(jb_context *jb, int src_x, int src_y, int dst_x, int dst_y, int w, int h)
{
    uint32_t words[4];
    jb_result r;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    r = check_corner(jb, "jb_vram_copy", "source", src_x, src_y);
    if (r == JB_OK)
        r = check_corner(jb, "jb_vram_copy", "destination", dst_x, dst_y);
    if (r == JB_OK)
        r = check_size(jb, "jb_vram_copy", w, h);
    if (r != JB_OK)
        return r;
    words[0] = 0x80000000u;
    words[1] = pack(src_x, src_y);
    words[2] = pack(dst_x, dst_y);
    words[3] = pack(w, h);
    jbi_api_command(jb, words, 4);
    return JB_OK;
}

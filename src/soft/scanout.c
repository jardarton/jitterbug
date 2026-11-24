/*
 * Display output (backend.h, scanout): the display area read out of VRAM
 * as the console's display fetch does, at the internal resolution.
 *
 * Taken from the seed's libgpu present without PsyQ's DISPENV: the display
 * area and mode are arguments, and the frame goes to the caller's buffer.
 */
#include <string.h>

#include "soft/soft_state.h"

enum { MAX_DISP_W = 1024, MAX_DISP_H = 512 };

/*
 * Copies the display area into pixels, wrapping at the VRAM edges like the
 * display fetch. In 24-bit mode the area's bytes are R, G, B triples read
 * straight through VRAM, starting at the byte address of (x, y). With the
 * display disabled the frame is black.
 *
 * The frame is written as bytes (a 15-bit pixel low byte first), so the
 * caller's buffer may be of any type.
 *
 * At scale N the frame is N times as wide and high: 15-bit frames come
 * from the scaled VRAM, and 24-bit ones (decoded video, uploaded by the
 * CPU) from the 1x VRAM, each pixel repeated N x N.
 */
int jbi_soft_scanout(jbi_backend *b, int x0, int y0, int w0, int h0, int rgb24_mode, int enabled,
                     void *pixels, int *width, int *height, int *stride_out)
{
    jbi_soft *s = (jbi_soft *)b;
    int w = 512, h = 256, rgb24 = 0, row, stride, n = s->scale;
    uint8_t *frame = pixels;

    jbi_soft_flush(s);
    if (w0 > 0 && h0 > 0) {
        w = w0;
        h = h0;
        rgb24 = rgb24_mode != 0;
    }
    if (h > MAX_DISP_H)
        h = MAX_DISP_H;
    if (rgb24 && w > MAX_DISP_W * 2 / 3)
        w = MAX_DISP_W * 2 / 3;
    if (!rgb24 && w > MAX_DISP_W)
        w = MAX_DISP_W;
    stride = (rgb24 ? w * 3 : w * 2) * n;

    if (!enabled) {
        int i;
        for (i = 0; i < stride * h * n; ++i)
            frame[i] = 0;
    } else if (n > 1 && !rgb24) {
        int x = x0 & (JBI_VRAM_W - 1), y = y0, hw = JBI_VRAM_W * n;

        for (row = 0; row < h * n; ++row) {
            const uint16_t *src =
                s->hvram + (size_t)((((y + row / n) & (JBI_VRAM_H - 1)) * n) + row % n) * hw;
            uint8_t *d = frame + (size_t)row * stride;
            int first = w * n, start = x * n;

            if (start + first > hw)
                first = hw - start;
            memcpy(d, src + start, (size_t)first * 2);
            memcpy(d + (size_t)first * 2, src, (size_t)(w * n - first) * 2);
        }
    } else {
        int x = x0 & (JBI_VRAM_W - 1), y = y0;

        /* 24-bit display above 1x reads the 1x VRAM (soft.c). */
        jbi_soft_lowres_resolve(s, x, y, rgb24 ? (w * 3 + 1) / 2 : w, h);
        for (row = 0; row < h; ++row) {
            const uint16_t *src = s->vram + (size_t)((y + row) & (JBI_VRAM_H - 1)) * JBI_VRAM_W;
            uint8_t *dst = frame + (size_t)row * n * stride;
            int i, k;

            if (rgb24) {
                /* The row's bytes as the display fetch sees them: each
                   pixel low byte first. */
                int start = x * 2;
                for (i = 0; i < w * 3; ++i) {
                    int at = (start + i) & (JBI_VRAM_W * 2 - 1);
                    uint8_t c = (uint8_t)(src[at >> 1] >> ((at & 1) * 8));
                    for (k = 0; k < n; ++k)
                        dst[(i / 3 * n + k) * 3 + i % 3] = c;
                }
            } else {
                for (i = 0; i < w; ++i) {
                    uint16_t p = src[(x + i) & (JBI_VRAM_W - 1)];
                    dst[2 * i] = (uint8_t)p;
                    dst[2 * i + 1] = (uint8_t)(p >> 8);
                }
            }
            for (k = 1; k < n; ++k)
                memcpy(dst + (size_t)k * stride, dst, (size_t)stride);
        }
    }
    *width = w * n;
    *height = h * n;
    *stride_out = stride;
    return rgb24 ? JBI_SCANOUT_RGB24 : JBI_SCANOUT_BGR555;
}

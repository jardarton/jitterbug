#include "core/layout.h"

int jbi_widescreen_width(int h)
{
    return (h * 16 + 4) / 9;
}

int jbi_frame_buffers(int w, int h, jbi_rect out[2])
{
    out[0].x = 0;
    out[0].y = 0;
    out[0].w = w;
    out[0].h = h;
    if (2 * h <= 512) {
        out[1] = out[0];
        out[1].y = h;
        return 2;
    }
    if (2 * w <= 1024) {
        out[1] = out[0];
        out[1].x = w;
        return 2;
    }
    return 1;
}

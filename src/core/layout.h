/*
 * Where the friendly frame's display buffers lie in VRAM (jb_begin_frame,
 * include/jitterbug.h). One rule, used by the frame and by the VRAM
 * placer, which keeps textures and palettes out of the buffers.
 */
#ifndef JBI_CORE_LAYOUT_H
#define JBI_CORE_LAYOUT_H

typedef struct jbi_rect {
    int x, y, w, h;
} jbi_rect;

/* The 16:9 width for a display h pixels high, rounded to the nearest
   pixel: 427 for 240. */
int jbi_widescreen_width(int h);

/*
 * The buffers for a reserved display of w x h (1x pixels, 1..1024 by
 * 1..512): two at (0, 0) and (0, h) when 2h <= 512; else two side by side
 * at (0, 0) and (w, 0) when 2w <= 1024; else one at (0, 0), drawn while it
 * is shown. Fills out[0] and, for two, out[1]; returns how many.
 */
int jbi_frame_buffers(int w, int h, jbi_rect out[2]);

#endif

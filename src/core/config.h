/*
 * The options a context is created with (context.h). Internal: the public
 * jb_config will be built on it.
 */
#ifndef JBI_CORE_CONFIG_H
#define JBI_CORE_CONFIG_H

#include "core/alloc.h"
#include "core/precise_source.h"

/* A message from the library (it never prints by itself). Called on the
   thread that made the call into the context. */
typedef void (*jbi_log_fn)(void *user, const char *message);

typedef struct jbi_config {
    /*
     * Internal resolution: 1..JBI_MAX_SCALE renders at that multiple of
     * the native resolution (clamped). Changeable later (jbi_configure).
     */
    int scale;
    /*
     * -1: the default (the console's dithering at 1x, none above); 0 off;
     * 1 on (scaled over 1x pixels above 1x).
     */
    int dither;
    /* Rasterising threads; 0 for one per CPU, up to 16. */
    int threads;
    /*
     * Above 1x, drawn pixels reach the 1x VRAM only from the scaled one,
     * smoothed, unless exact_1x keeps drawing them at 1x as the console
     * would, which costs a second rasterisation (src/soft/soft.c). 0 or 1.
     */
    int exact_1x;
    /*
     * 0 maps textures affinely, as the console (the default); 1
     * perspective-correct where every vertex has a precise record with a
     * depth, which needs an enabled precise source.
     */
    int texture_perspective;
    /*
     * Perspective-correct texturing computes exact values every
     * persp_span pixels along a row in the scaled pass, every
     * persp_span_1x pixels at 1x, and interpolates linearly in between;
     * 1 is every pixel. 1..64; out of range means the default (8 and 4).
     */
    int persp_span;
    int persp_span_1x;
    /* Sub-pixel vertex positions (precise_source.h): copied when the
       context is created; NULL for none. */
    const jbi_precise_source *precise;
    /* Messages (no memory for a scale, a worker thread that would not
       start); NULL drops them. */
    jbi_log_fn log;
    void *log_user;
    /* Where the context's memory comes from (alloc.h); all zero for the C
       library's. */
    jbi_allocator alloc;
} jbi_config;

/* The defaults: scale 1, dither -1, 1 thread, exact_1x 0, affine
   textures, spans 8 and 4, no precise source, no log, the C library's
   allocator. */
void jbi_config_default(jbi_config *cfg);

enum { JBI_PERSP_SPAN = 8, JBI_PERSP_SPAN_1X = 4, JBI_PERSP_SPAN_MAX = 64 };

#endif

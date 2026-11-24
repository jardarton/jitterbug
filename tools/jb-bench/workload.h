/*
 * jb-bench's workloads: what each draws, behind one interface, so the timer
 * (bench.c) can draw any of them at any scale, thread count and options.
 *
 *   first-light  the console-level first-light scene as it is
 *                (tests/console_scene/scene.c: the example as first written)
 *   heavy        a busy frame: terrain, objects, water, particles, lines,
 *                a HUD, about 2,000 primitives (heavy.c)
 *   fill         fill rate: four full-screen textured, shaded,
 *                semi-transparent quads (heavy.c)
 *
 * Every frame follows from its number alone, in integer arithmetic, so any
 * frame can be drawn in any order and every machine draws the same pixels.
 * C99, the public API only.
 */
#ifndef JB_BENCH_WORKLOAD_H
#define JB_BENCH_WORKLOAD_H

#include <jitterbug.h>

/* What a workload's frames contain, counted by the tool over the primitives
   it sends (not by the library): a pixel is covered when its centre is
   inside the primitive (top-left rule) and inside the 320 x 240 display.
   The console's own edge rules differ from that by a pixel along some
   edges, so the areas are close, not exact. */
typedef struct wl_stats {
    long frames;
    long prims;       /* drawing commands sent (a quad is one) */
    long triangles;   /* polygon triangles sent (a quad is two) */
    long tri_pixels;  /* pixels the triangles cover, on screen */
    long tri_visible; /* triangles covering at least one pixel */
    long textured;    /* textured triangles */
    long tex4, tex8, tex15; /* of those, by texture depth */
    long blended;     /* semi-transparent primitives */
    long rects;       /* rectangles and sprites */
    long rect_pixels;
    long lines;       /* line segments */
    long line_pixels;
    long min_prims, max_prims; /* per frame */
    long mark;                 /* prims when the frame began */
} wl_stats;

typedef struct wl_options {
    int scale;   /* 1..8 */
    int threads; /* jb_config.threads: 0 for one per CPU */
    int precise; /* precise vertices */
    int persp;   /* perspective-correct textures */
    jb_log_fn log; /* jb_config.log and log_user: NULL for none */
    void *log_user;
} wl_options;

typedef struct workload workload;

/* Starts a workload: its context, textures and palettes. NULL on failure,
   with *result saying why. */
typedef workload *(*wl_create_fn)(const wl_options *o, jb_result *result);

struct workload {
    jb_context *jb;
    /* Draws frame `frame` (any order) and shows it: everything a program
       does for a frame up to jb_frame. With stats not NULL, also counts
       what it sends into it. Returns the first error. */
    jb_result (*draw)(workload *w, unsigned frame, wl_stats *stats);
    void (*destroy)(workload *w);
};

typedef struct wl_info {
    const char *name;
    const char *about;
    wl_create_fn create;
} wl_info;

extern const wl_info wl_first_light, wl_heavy, wl_fill;

/* Counting (stats.c). Positions are display pixels (before the drawing
   offset); each call adds one command. */
void wl_count_poly(wl_stats *st, const jb_poly *p);
void wl_count_rect(wl_stats *st, const jb_rect *r);
void wl_count_line(wl_stats *st, int x0, int y0, int x1, int y1, int blend);
void wl_stats_begin_frame(wl_stats *st);
void wl_stats_end_frame(wl_stats *st);

/* The display every workload draws, in 1x pixels. */
enum { WL_WIDTH = 320, WL_HEIGHT = 240 };

#endif

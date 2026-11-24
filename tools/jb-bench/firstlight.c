/*
 * jb-bench's first-light workload: the console-level first-light scene
 * (tests/console_scene, the example as it was first written, on the
 * console's API), built from its own scene.c (not a copy), drawn exactly
 * as its runner draws it.
 */
#include <stdlib.h>

#include "scene.h"
#include "workload.h"

typedef struct {
    workload base;
    fl_scene scene;
} fl_workload;

/* The scene's sky is drawn outside its ordering table (scene.c, draw_sky):
   a Gouraud quad from (0, 0) to (320, 150) and 24 1 x 1 rectangles. The
   rest is in scene.prims after a frame, which the statistics read. */
static void count_sky(wl_stats *st)
{
    jb_poly sky = {0};
    jb_rect dot = {0};
    int i;

    sky.vertices = 4;
    sky.gouraud = 1;
    sky.v[1].x = 320, sky.v[2].y = 150, sky.v[3].x = 320, sky.v[3].y = 150;
    wl_count_poly(st, &sky);
    dot.w = dot.h = 1;
    for (i = 0; i < 24; ++i)
        wl_count_rect(st, &dot);
}

static void count_prims(const fl_scene *s, wl_stats *st)
{
    int i, k;

    count_sky(st);
    for (i = 0; i < s->nprims; ++i) {
        const fl_prim *p = &s->prims[i];

        switch (p->kind) {
        case FL_POLY: wl_count_poly(st, &p->u.poly); break;
        case FL_RECT: wl_count_rect(st, &p->u.rect); break;
        case FL_LINE:
            wl_count_line(st, p->u.line.v[0].x, p->u.line.v[0].y, p->u.line.v[1].x,
                          p->u.line.v[1].y, p->u.line.blend);
            break;
        case FL_POLYLINE:
            for (k = 0; k + 1 < p->u.polyline.count; ++k)
                wl_count_line(st, p->u.polyline.v[k].x, p->u.polyline.v[k].y,
                              p->u.polyline.v[k + 1].x, p->u.polyline.v[k + 1].y,
                              p->u.polyline.blend);
            break;
        }
    }
}

static jb_result fl_draw(workload *w, unsigned frame, wl_stats *st)
{
    fl_workload *f = (fl_workload *)w;
    jb_result r = fl_scene_draw(&f->scene, frame);

    if (st != NULL) {
        wl_stats_begin_frame(st);
        count_prims(&f->scene, st);
        wl_stats_end_frame(st);
    }
    return r;
}

static void fl_destroy(workload *w)
{
    fl_workload *f = (fl_workload *)w;

    fl_scene_free(&f->scene);
    free(f);
}

static workload *fl_create(const wl_options *o, jb_result *result)
{
    fl_workload *f = calloc(1, sizeof *f);
    fl_switches sw = FL_SWITCHES_CONSOLE;
    jb_result r;

    if (f == NULL) {
        *result = JB_ERROR_OUT_OF_MEMORY;
        return NULL;
    }
    sw.scale = o->scale;
    sw.precise = o->precise;
    sw.perspective = o->persp;
    sw.log = o->log;
    sw.log_user = o->log_user;
    if ((r = fl_scene_init(&f->scene, &sw, o->threads)) != JB_OK) {
        free(f);
        *result = r;
        return NULL;
    }
    f->base.jb = f->scene.jb;
    f->base.draw = fl_draw;
    f->base.destroy = fl_destroy;
    *result = JB_OK;
    return &f->base;
}

const wl_info wl_first_light = {
    "first-light", "examples/first-light's scene: a lit cube, floor, sprite, ring, panel",
    fl_create};

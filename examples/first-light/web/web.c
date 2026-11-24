/*
 * first-light in a browser: the scene behind five functions that the page
 * (index.html) calls through the module emscripten builds
 * (first-light.js and first-light.wasm). No SDL, no files, one thread.
 * The switches are, in order: scale, precise, perspective, dither,
 * widescreen, float (the float geometry path), CLUT cache.
 *
 *   fl_web_init(switches...)          the scene; 0 or a jb_result
 *   fl_web_set_switches(switches...)
 *   fl_web_draw(frame)   draws frame number `frame` and returns where its
 *                        RGBA bytes are in the module's memory (0 on error)
 *   fl_web_width(), fl_web_height()   that frame's size in pixels
 */
#include <emscripten.h>

#include "scene.h"

/* What the module exports (EMSCRIPTEN_KEEPALIVE keeps them). */
int fl_web_init(int scale, int precise, int perspective, int dither, int wide, int flt,
                int cache);
int fl_web_set_switches(int scale, int precise, int perspective, int dither, int wide, int flt,
                        int cache);
const uint8_t *fl_web_draw(unsigned frame);
int fl_web_width(void);
int fl_web_height(void);

/* The page has one scene. */
static fl_scene scene;
static int ready, frame_w, frame_h;

static fl_switches switches(int scale, int precise, int perspective, int dither, int wide,
                            int flt, int cache)
{
    fl_switches sw;

    sw.scale = scale;
    sw.precise = precise;
    sw.perspective = perspective;
    sw.dither = dither;
    sw.widescreen = wide;
    sw.geometry = flt ? JB_GEOMETRY_FLOAT : JB_GEOMETRY_GTE;
    sw.clut_cache = cache;
    sw.log = NULL;
    sw.log_user = NULL;
    return sw;
}

EMSCRIPTEN_KEEPALIVE int fl_web_init(int scale, int precise, int perspective, int dither,
                                     int wide, int flt, int cache)
{
    fl_switches sw = switches(scale, precise, perspective, dither, wide, flt, cache);
    jb_result r;

    if (ready)
        fl_scene_free(&scene);
    /* One thread: threads in a browser need cross-origin isolation, and
       this build has none (a request for more would be clamped to 1). */
    r = fl_scene_init(&scene, &sw, 1);
    ready = r == JB_OK;
    return (int)r;
}

EMSCRIPTEN_KEEPALIVE int fl_web_set_switches(int scale, int precise, int perspective, int dither,
                                             int wide, int flt, int cache)
{
    fl_switches sw = switches(scale, precise, perspective, dither, wide, flt, cache);

    return ready ? (int)fl_scene_set_switches(&scene, &sw) : (int)JB_ERROR_INVALID_ARGUMENT;
}

EMSCRIPTEN_KEEPALIVE const uint8_t *fl_web_draw(unsigned frame)
{
    if (!ready || fl_scene_draw(&scene, frame) != JB_OK)
        return 0;
    return fl_scene_frame(&scene, &frame_w, &frame_h);
}

EMSCRIPTEN_KEEPALIVE int fl_web_width(void)
{
    return frame_w;
}

EMSCRIPTEN_KEEPALIVE int fl_web_height(void)
{
    return frame_h;
}

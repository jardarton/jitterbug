/*
 * The demo in a browser: the scene behind a few functions that the page
 * (index.html) calls through the module emscripten builds (demo.js and
 * demo.wasm). No SDL, no files, one thread. The keyboard is the scene's
 * own (dm_key), so the page's keys and buttons, the window's keys and a
 * headless script all do the same thing.
 *
 *   dm_web_init(scale, precise, perspective, dither, widescreen, float,
 *               clut cache, fog)
 *                        the scene, at frame 0, not paused; 0 or a jb_result
 *   dm_web_key(key)      presses a key (a character code, as dm_key takes
 *                        it): 1, 0 for a key that is not one, or minus the
 *                        jb_result of switches the scene refused (the old
 *                        ones kept)
 *   dm_web_advance(step) the frame counter on by step unless paused
 *   dm_web_draw()        draws the current frame and returns where its RGBA
 *                        bytes are in the module's memory (0 on error)
 *   dm_web_width(), dm_web_height()   that frame's size in pixels
 *   dm_web_state()       the switches as bits: scale in bits 0-3, then
 *                        precise 4, perspective 5, dither 6, widescreen 7,
 *                        float 8, CLUT cache 9, fog 10, panel 11, paused 12
 *   dm_web_frame()       the current frame number
 */
#include <emscripten.h>

#include "scene.h"

/* What the module exports (EMSCRIPTEN_KEEPALIVE keeps them). */
int dm_web_init(int scale, int precise, int perspective, int dither, int wide, int flt, int cache,
                int fog);
int dm_web_key(int key);
void dm_web_advance(unsigned step);
const uint8_t *dm_web_draw(void);
int dm_web_width(void);
int dm_web_height(void);
int dm_web_state(void);
unsigned dm_web_frame(void);

/* The page has one scene. */
static dm_scene scene;
static dm_switches sw;
static int ready, paused, frame_w, frame_h;
static unsigned frame;

EMSCRIPTEN_KEEPALIVE int dm_web_init(int scale, int precise, int perspective, int dither, int wide,
                                     int flt, int cache, int fog)
{
    dm_switches console = DM_SWITCHES_CONSOLE;
    jb_result r;

    if (ready)
        dm_scene_free(&scene);
    sw = console;
    sw.scale = scale, sw.precise = precise, sw.perspective = perspective, sw.dither = dither;
    sw.widescreen = wide, sw.geometry = flt ? JB_GEOMETRY_FLOAT : JB_GEOMETRY_GTE;
    sw.clut_cache = cache, sw.fog = fog;
    paused = 0, frame = 0;
    /* One thread: threads in a browser need cross-origin isolation, and
       this build has none (a request for more would be clamped to 1). */
    r = dm_scene_init(&scene, &sw, 1);
    ready = r == JB_OK;
    return (int)r;
}

EMSCRIPTEN_KEEPALIVE int dm_web_key(int key)
{
    dm_switches want = sw;
    jb_result r;

    if (!ready || !dm_key(key, &want, &paused, &frame))
        return 0;
    if ((r = dm_scene_set_switches(&scene, &want)) != JB_OK)
        return -(int)r;
    sw = want;
    return 1;
}

EMSCRIPTEN_KEEPALIVE void dm_web_advance(unsigned step)
{
    if (!paused)
        frame += step;
}

EMSCRIPTEN_KEEPALIVE const uint8_t *dm_web_draw(void)
{
    if (!ready || dm_scene_draw(&scene, frame, paused) != JB_OK)
        return 0;
    return dm_scene_frame(&scene, &frame_w, &frame_h);
}

EMSCRIPTEN_KEEPALIVE int dm_web_width(void)
{
    return frame_w;
}

EMSCRIPTEN_KEEPALIVE int dm_web_height(void)
{
    return frame_h;
}

EMSCRIPTEN_KEEPALIVE int dm_web_state(void)
{
    return sw.scale | sw.precise << 4 | sw.perspective << 5 | sw.dither << 6 | sw.widescreen << 7 |
           (sw.geometry == JB_GEOMETRY_FLOAT) << 8 | sw.clut_cache << 9 | sw.fog << 10 |
           sw.panel << 11 | paused << 12;
}

EMSCRIPTEN_KEEPALIVE unsigned dm_web_frame(void)
{
    return frame;
}

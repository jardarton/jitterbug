/*
 * The demo in an SDL3 window. The scene advances one frame every 1/60 s
 * (a fixed timestep: an accumulator over the clock), and each new frame is
 * shown with nearest-neighbour scaling by a whole number where the window
 * allows it. Presentation waits for vsync where the renderer can.
 *
 * Keys (scene.h, dm_key; the window title shows the switches too):
 *   1-4     internal scale          V  precise vertices (no jitter)
 *   T       perspective-correct     D  dithering
 *   C       the CLUT cache          W  widescreen (16:9)
 *   G       geometry: GTE or float  F  fog
 *   H       the panel               P, Space  pause
 *   N, Right (paused)  one frame on Esc, Q  quit
 *
 *   demo [--threads T] [--frames N]
 * --frames N quits after drawing N frames (for tests); --threads as
 * jb_config.threads (0, the default, is one per CPU).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "scene.h"

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int tex_w, tex_h;
} display;

static void set_title(SDL_Window *window, const dm_switches *sw, int paused)
{
    char title[320];

    snprintf(title, sizeof title,
             "jitterbug demo  |  [1-4] %dx  [V] precise %s  [T] perspective %s  [D] dither %s  "
             "[C] CLUT cache %s  [W] %s  [G] %s  [F] fog %s  [P] %s",
             sw->scale, sw->precise ? "on" : "off", sw->perspective ? "on" : "off",
             sw->dither ? "on" : "off", sw->clut_cache ? "on" : "off",
             sw->widescreen ? "16:9" : "4:3", sw->geometry == JB_GEOMETRY_FLOAT ? "float" : "GTE",
             sw->fog ? "on" : "off", paused ? "paused" : "pause");
    SDL_SetWindowTitle(window, title);
}

/* Shows w x h RGBA bytes: as large as a whole multiple fits, or, for a
   frame larger than the window, shrunk to fit. */
static int show(display *d, const uint8_t *px, int w, int h)
{
    SDL_FRect dst;
    int out_w, out_h, k;

    if (d->texture == NULL || d->tex_w != w || d->tex_h != h) {
        SDL_DestroyTexture(d->texture);
        /* RGBA32: the bytes R, G, B, A in memory, as jb_frame gives them. */
        d->texture = SDL_CreateTexture(d->renderer, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STREAMING, w, h);
        if (d->texture == NULL)
            return 0;
        SDL_SetTextureScaleMode(d->texture, SDL_SCALEMODE_NEAREST);
        d->tex_w = w, d->tex_h = h;
    }
    if (!SDL_UpdateTexture(d->texture, NULL, px, w * 4))
        return 0;
    SDL_GetCurrentRenderOutputSize(d->renderer, &out_w, &out_h);
    k = out_w / w < out_h / h ? out_w / w : out_h / h;
    if (k >= 1) {
        dst.w = (float)(w * k), dst.h = (float)(h * k);
    } else if (out_w * h < out_h * w) {
        dst.w = (float)out_w, dst.h = (float)out_w * (float)h / (float)w;
    } else {
        dst.h = (float)out_h, dst.w = (float)out_h * (float)w / (float)h;
    }
    dst.x = ((float)out_w - dst.w) / 2, dst.y = ((float)out_h - dst.h) / 2;
    SDL_SetRenderDrawColor(d->renderer, 0, 0, 0, 255);
    SDL_RenderClear(d->renderer);
    SDL_RenderTexture(d->renderer, d->texture, NULL, &dst);
    return SDL_RenderPresent(d->renderer);
}

/* The window's key as dm_key's character, or 0. */
static int key_of(SDL_Keycode k)
{
    if (k >= SDLK_1 && k <= SDLK_4)
        return '1' + (int)(k - SDLK_1);
    if (k >= SDLK_A && k <= SDLK_Z)
        return 'a' + (int)(k - SDLK_A);
    if (k == SDLK_SPACE)
        return 'p';
    if (k == SDLK_RIGHT)
        return 'n';
    return 0;
}

int main(int argc, char **argv)
{
    const Uint64 step = SDL_NS_PER_SECOND / 60;
    dm_switches sw = DM_SWITCHES_CONSOLE;
    display d;
    static dm_scene scene;
    jb_result r;
    Uint64 last, acc = 0;
    unsigned frame = 0, drawn = 0, max_frames = 0;
    int threads = 0, paused = 0, running = 1, redraw = 1, status = 0, i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            threads = atoi(argv[++i]);
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            max_frames = (unsigned)strtoul(argv[++i], NULL, 10);
        else {
            fprintf(stderr, "usage: demo [--threads T] [--frames N]\n");
            return 2;
        }
    }

    memset(&d, 0, sizeof d);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "demo: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!SDL_CreateWindowAndRenderer("jitterbug demo", 320 * 3, 240 * 3,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                     &d.window, &d.renderer)) {
        fprintf(stderr, "demo: SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(d.renderer, 1); /* not every renderer can; then the clock paces */
    if ((r = dm_scene_init(&scene, &sw, threads)) != JB_OK) {
        fprintf(stderr, "demo: %s\n", jb_result_string(r));
        SDL_Quit();
        return 1;
    }
    set_title(d.window, &sw, paused);

    last = SDL_GetTicksNS();
    while (running) {
        dm_switches want = sw;
        SDL_Event e;
        Uint64 now;

        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT)
                running = 0;
            /* A window shown again or resized is presented again, the same
               frame when paused (the scene does not move on). */
            if (e.type == SDL_EVENT_WINDOW_EXPOSED || e.type == SDL_EVENT_WINDOW_RESIZED ||
                e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                redraw = 1;
            if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat)
                continue;
            if (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_Q)
                running = 0;
            else if (dm_key(key_of(e.key.key), &want, &paused, &frame))
                redraw = 1;
        }
        if (memcmp(&want, &sw, sizeof sw) != 0) {
            if ((r = dm_scene_set_switches(&scene, &want)) != JB_OK)
                fprintf(stderr, "demo: switches refused: %s\n", jb_result_string(r));
            else
                sw = want;
        }
        if (redraw)
            set_title(d.window, &sw, paused);

        /* One scene frame per 1/60 s of the clock, at most four at once
           (after a stall the scene skips ahead rather than racing). */
        now = SDL_GetTicksNS();
        acc += now - last;
        last = now;
        if (acc > 4 * step)
            acc = 4 * step;
        for (; acc >= step; acc -= step)
            if (!paused)
                ++frame, redraw = 1;

        if (redraw) {
            const uint8_t *px;
            int w, h;

            if ((r = dm_scene_draw(&scene, frame, paused)) != JB_OK)
                fprintf(stderr, "demo: frame %u: %s\n", frame, jb_error_string(scene.jb));
            px = dm_scene_frame(&scene, &w, &h);
            if (px == NULL || !show(&d, px, w, h)) {
                fprintf(stderr, "demo: cannot show frame %u: %s\n", frame, SDL_GetError());
                status = 1;
                break;
            }
            redraw = 0;
            if (max_frames != 0 && ++drawn == max_frames)
                running = 0;
        } else {
            SDL_Delay(1);
        }
    }

    printf("demo: drew %u frames, last frame %u\n", drawn, frame);
    dm_scene_free(&scene);
    SDL_DestroyTexture(d.texture);
    SDL_DestroyRenderer(d.renderer);
    SDL_DestroyWindow(d.window);
    SDL_Quit();
    return status;
}

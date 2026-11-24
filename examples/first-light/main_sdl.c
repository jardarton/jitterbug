/*
 * first-light in an SDL3 window. The scene advances one frame every 1/60 s
 * (a fixed timestep: an accumulator over the clock), and each new frame is
 * shown with nearest-neighbour scaling by a whole number where the window
 * allows it. Presentation waits for vsync where the renderer can.
 *
 * Keys (the window title shows the switches):
 *   1-4     internal scale          V  precise vertices
 *   T       perspective-correct     D  dithering
 *   W       widescreen (16:9)       G  geometry: the GTE or float
 *   C       the CLUT cache
 *   Space   pause                   Right (paused)  one frame on
 *   Esc, Q  quit
 *
 *   first-light [--threads T] [--frames N]
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

static void set_title(SDL_Window *window, const fl_switches *sw, int paused)
{
    char title[300];

    snprintf(title, sizeof title,
             "first-light  |  [1-4] scale %dx  [V] precise %s  [T] perspective %s  "
             "[D] dither %s  [W] %s  [G] %s  [C] CLUT cache %s  [Space] %s",
             sw->scale, sw->precise ? "on" : "off", sw->perspective ? "on" : "off",
             sw->dither ? "on" : "off", sw->widescreen ? "16:9" : "4:3",
             sw->geometry == JB_GEOMETRY_FLOAT ? "float" : "GTE", sw->clut_cache ? "on" : "off",
             paused ? "paused" : "pause");
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

int main(int argc, char **argv)
{
    const Uint64 step = SDL_NS_PER_SECOND / 60;
    fl_switches sw = FL_SWITCHES_CONSOLE;
    display d;
    fl_scene scene;
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
            fprintf(stderr, "usage: first-light [--threads T] [--frames N]\n");
            return 2;
        }
    }

    memset(&d, 0, sizeof d);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "first-light: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!SDL_CreateWindowAndRenderer("first-light", 320 * 3, 240 * 3,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                     &d.window, &d.renderer)) {
        fprintf(stderr, "first-light: SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(d.renderer, 1); /* not every renderer can; then the clock paces */
    if ((r = fl_scene_init(&scene, &sw, threads)) != JB_OK) {
        fprintf(stderr, "first-light: %s\n", jb_result_string(r));
        SDL_Quit();
        return 1;
    }
    set_title(d.window, &sw, paused);
    printf("first-light: renderer %s\n", SDL_GetRendererName(d.renderer));

    last = SDL_GetTicksNS();
    while (running) {
        fl_switches want = sw;
        SDL_Event e;
        Uint64 now;

        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT)
                running = 0;
            if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat)
                continue;
            switch (e.key.key) {
            case SDLK_ESCAPE:
            case SDLK_Q: running = 0; break;
            case SDLK_1:
            case SDLK_2:
            case SDLK_3:
            case SDLK_4: want.scale = (int)(e.key.key - SDLK_1) + 1; break;
            case SDLK_V: want.precise = !want.precise; break;
            case SDLK_T: want.perspective = !want.perspective; break;
            case SDLK_D: want.dither = !want.dither; break;
            case SDLK_W: want.widescreen = !want.widescreen; break;
            case SDLK_G:
                want.geometry = want.geometry == JB_GEOMETRY_GTE ? JB_GEOMETRY_FLOAT
                                                                 : JB_GEOMETRY_GTE;
                break;
            case SDLK_C: want.clut_cache = !want.clut_cache; break;
            case SDLK_SPACE:
                paused = !paused;
                set_title(d.window, &sw, paused);
                break;
            case SDLK_RIGHT:
                if (paused)
                    ++frame, redraw = 1;
                break;
            default: break;
            }
        }
        if (memcmp(&want, &sw, sizeof sw) != 0) {
            if ((r = fl_scene_set_switches(&scene, &want)) != JB_OK)
                fprintf(stderr, "first-light: %s\n", jb_error_string(scene.jb));
            else
                sw = want;
            set_title(d.window, &sw, paused);
            redraw = 1;
        }

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

            if ((r = fl_scene_draw(&scene, frame)) != JB_OK)
                fprintf(stderr, "first-light: frame %u: %s\n", frame, jb_error_string(scene.jb));
            px = fl_scene_frame(&scene, &w, &h);
            if (px == NULL || !show(&d, px, w, h)) {
                fprintf(stderr, "first-light: cannot show frame %u: %s\n", frame, SDL_GetError());
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

    printf("first-light: drew %u frames, last frame %u\n", drawn, frame);
    fl_scene_free(&scene);
    SDL_DestroyTexture(d.texture);
    SDL_DestroyRenderer(d.renderer);
    SDL_DestroyWindow(d.window);
    SDL_Quit();
    return status;
}

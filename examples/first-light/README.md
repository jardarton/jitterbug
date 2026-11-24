# first-light

A compact example built on jitterbug's friendly API ([include/jitterbug.h](../../include/jitterbug.h)): a lit, textured cube spinning over a checkered floor and shadow, an orbiting glowing star on a ring of additive lines, a starry sky and a status panel. All art is generated in code (`scene.c`, 292 lines), and motion uses only `jb_sinf` and `jb_cosf`, so its frames hash identically across every tested native and WebAssembly build ([expected.txt](expected.txt); see [docs/determinism.md](../../docs/determinism.md)).

An equivalent console-level version (manual GTE calls, custom ordering table, `jb_prim_*` and manual VRAM layout, 877 lines) is kept as a test in `tests/console_scene/`.

## Building and running

### Native (SDL3 window and headless)

From the repository root (SDL3 is provided by `nix develop`):

```sh
nix develop -c cmake -S . -B build -G Ninja -DJB_EXAMPLES=ON
nix develop -c cmake --build build
build/examples/first-light/first-light
build/examples/first-light/first-light-headless --hash 90 --png 90 frame90.png
```

`first-light` is built when CMake finds SDL3:
- `first-light [--threads T] [--frames N]` (`--threads 0` defaults to one per CPU; `--frames N` exits after `N` frames; `SDL_VIDEODRIVER=dummy` runs without a display).
- `first-light-headless`: renders frames `0..N-1` without a window and prints 64-bit FNV-1a RGBA hashes. Options: `--frames`, `--scale` (1–8), `--precise`, `--perspective`, `--dither`, `--widescreen`, `--float`, `--clut-cache`, `--pause-at F`, `--threads`, `--hash F`, `--png F PATH`, `--expect FILE [NAME]` and `--record FILE` (see the top comment in `main_headless.c`).

### Browser (WebAssembly)

With Nix:

```sh
nix build .#first-light-web -o first-light-web   # index.html, first-light.js, first-light.wasm
python3 -m http.server -d first-light-web 8000
# open http://localhost:8000/
```

Or by hand with Emscripten:

```sh
nix develop -c emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DJB_EXAMPLES=ON -DJB_TESTS=OFF -DJB_VULKAN=OFF
nix develop -c cmake --build build-web
python3 -m http.server -d build-web/examples/first-light 8000
```

Serve over HTTP (browsers refuse to load `.wasm` over `file://`). The page has no external dependencies. Check mode runs via URL query parameters: `?frames=300` with optional `&scale=`, `&precise=`, `&perspective=`, `&dither=`, `&widescreen=`, `&float=` and `&clutcache=`.

## What it shows

| Element | Console feature demonstrated | API used |
|---|---|---|
| Cube (4 sides) | 8-bit sine-plasma texture with a 256-colour palette cycled every frame | `jb_texture_create`, `jb_palette_write` |
| Cube (front & back) | 15-bit direct-colour texture | `jb_texture_create` |
| Cube shading | Gouraud vertex colours from GTE lighting (two directional lights + ambient) modulated onto texels, dithered at 1x | `jb_draw_mesh`, `jb_set_light`, `jb_set_ambient` |
| Floor | Single large 4-bit repeating quad (via texture window) showing affine texture wobble along its diagonal | `jb_draw_mesh`, repeat texture |
| Shadow | Flat untextured quad with subtractive blending (`B - F`), depth-sorted with the cube | `jb_draw_mesh`, `JB_BLEND_SUB` |
| Star | 4-bit textured billboard (entry 0 transparent, glow entries semi-transparent, `B + F`), swapping between 32 x 32 and 16 x 16 textures by distance | `jb_draw_billboard`, `JB_BLEND_ADD` |
| Ring | 16 additive Gouraud 3D lines, each depth-sorted | `jb_draw_line3d` |
| Sky | Dithered Gouraud gradient quad and 1 x 1 star rectangles in the background layer | `jb_draw_poly`, `jb_draw_rect` |
| Panel | Half-blended box, Gouraud polyline border, bar, title and switch indicators in the overlay layer | `jb_draw_rect`, `jb_draw_polyline`, `jb_draw_text` |

Each frame calls `jb_begin_frame`, updates the cube's palette, draws background, 3D scene and overlay layers, and finishes with `jb_end_frame`. The context reserves 427 x 240 double buffers so widescreen can be toggled at run time, and jitterbug's VRAM placer arranges all textures, CLUTs and the bitmap font automatically.

## Keys, buttons and switches

| Key | Page button | Switch | What changes |
|---|---|---|---|
| `1` `2` `3` `4` | `1x` `2x` `3x` `4x` | Internal scale | Output resolution (`first-light-headless --scale` goes up to 8x); 1x VRAM is still maintained alongside it |
| `V` | `Precise vertices` | Sub-pixel vertices | Above 1x, removes vertex snapping jitter on the rotating cube. No effect at 1x |
| `T` | `Perspective-correct` | Texture mapping | Removes affine warp on the floor checks and cube faces. Works at 1x too |
| `D` | `Dithering` | 4 x 4 dithering | At 1x, toggles dithering on shaded faces and the sky gradient. No effect above 1x |
| `W` | `Widescreen` | 16:9 display | Switches between 320 x 240 (4:3) and 427 x 240 (16:9) |
| `G` | `Float geometry` | Geometry path | Switches between fixed-point GTE (default) and deterministic float geometry |
| `C` | `CLUT cache` | Palette cache | Toggles the CLUT cache (changes only the panel indicator here, since `jb_palette_write` flushes the cache; see [examples/demo/README.md](../demo/README.md) for a visible CLUT-cache demonstration) |
| `Space` | `Pause` | Pause | Pauses the frame counter (`--pause-at F` in headless mode) |
| `Right` | — | Step | Advances one frame while paused |
| `Esc`, `Q` | — | Quit | Closes the SDL3 window |

## Tests and stored hashes

[expected.txt](expected.txt) pins 11 runs: `base` (300 frames at 1x covering a full turn of the cube and star) plus 30-frame runs toggling each switch (`short`, `scale2`, `scale3`, `precise2`, `persp`, `nodither`, `wide`, `float`, `nocache`, `all3`).

- `first_light` (1 thread) and `first_light_4t` (4 threads): verify all 11 runs, check that same-length runs differ, and verify GPUSTAT dithering, frame dimensions and a CLUT-cache probe after each run.
- `first_light_web_module` (`web/test-module.cjs`, wasm builds): exercises the page's Emscripten exports and JavaScript FNV-1a hasher under Node against `expected.txt`.
- `first_light_scene_errors` (`test_scene.c`): checks error handling when `jb_begin_frame` fails.
- `first_light_headless_cli` (`test-headless.cmake`): checks `--record` formatting preservation, concurrent recording safety, `--pause-at`, and status-2 CLI refusals.
- `web/check-browser.mjs`: manual headless Chromium check for all runs, keys, frame rate and pause.

To re-record `expected.txt` after a deliberate change to the scene or renderer:

```sh
nix develop -c cmake --build build --target first_light_record
```

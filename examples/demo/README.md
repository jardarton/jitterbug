# demo

An original scene showing jitterbug's PlayStation GPU look: a camera flying over a sunset archipelago of textured hills, beaches, seabed, conifer groves, a ring of standing stones, a wooden pier, a lighthouse sweeping its beam, fireflies, a low sun and drifting clouds. Every hardware artefact is bound to a key and a page button so you can toggle it live and see its effect.

A live WebAssembly build is published at [https://jardarton.github.io/jitterbug/](https://jardarton.github.io/jitterbug/).

Everything is drawn with jitterbug's friendly API ([include/jitterbug.h](../../include/jitterbug.h)) plus the console-level API for the sky. Motion uses only integers and the library's own maths (`jb_sinf`, `jb_cosf`), so frames hash identically across every tested native and WebAssembly build ([expected.txt](expected.txt)).

## Building and running

### Native (SDL3 window and headless)

From the repository root (SDL3 is included in `nix develop`):

```sh
nix develop -c cmake -S . -B build -G Ninja -DJB_EXAMPLES=ON
nix develop -c cmake --build build
build/examples/demo/demo
build/examples/demo/demo-headless --start 1350 --frames 1 --png 0 frame.png
```

`demo` is built whenever CMake finds SDL3. Options:
- `demo [--threads T] [--frames N]` (`--threads 0`, the default, uses one thread per CPU; `--frames N` quits after `N` frames).
- `demo-headless`: runs without a window, hashes RGBA frames (64-bit FNV-1a), writes PNGs (`--png T PATH`), checks `expected.txt` (`--expect`), and supports `--start`, `--frames`, `--step`, `--scale` (1–8), `--precise`, `--perspective`, `--dither`, `--widescreen`, `--float`, `--clut-cache`, `--fog`, `--panel`, `--script`, `--threads`, `--gaps`, `--fade`, `--pier` and `--record`. See the top comment in `main_headless.c` for full option ranges.

### Browser (WebAssembly)

With Nix:

```sh
nix build .#demo-web -o demo-web     # index.html, demo.js, demo.wasm
python3 -m http.server -d demo-web 8000
# open http://localhost:8000/
```

Or by hand with Emscripten:

```sh
nix develop -c emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DJB_EXAMPLES=ON -DJB_TESTS=OFF -DJB_VULKAN=OFF
nix develop -c cmake --build build-web
python3 -m http.server -d build-web/examples/demo 8000
```

Serve over HTTP (browsers block fetching `.wasm` from `file://`). All assets are compiled into the module, so the page makes no extra network requests and includes touch-friendly buttons for every key.

Check mode runs headless-equivalent scripts in the browser via query parameters: `?frames=240` with optional `&step=`, `&scale=`, `&precise=`, `&perspective=`, `&dither=`, `&widescreen=`, `&float=`, `&clutcache=`, `&fog=` and `&script=`.

## Keys, buttons and switches

The SDL3 window, the web page buttons and scripted headless runs share the same key table (`dm_key` in `scene.h`):

| Key | Page button | Switch | What changes |
|---|---|---|---|
| `1` `2` `3` `4` | `1x` `2x` `3x` `4x` | Internal scale | Output resolution (`demo-headless --scale` goes up to 8x); 1x VRAM is still maintained alongside it |
| `V` | `Precise vertices` | Sub-pixel vertices | Above 1x, polygons stop snapping to the 1x pixel grid, removing vertex jitter on hills, trees, stones and the pier. No effect at 1x |
| `T` | `Perspective-correct` | Texture mapping | Removes affine texture wobble so the pier's planks, sea ripples and terrain textures stop bending across polygon diagonals. Works at 1x too |
| `D` | `Dithering` | 4 x 4 dithering | Toggles the console's 4 x 4 dither pattern on shaded pixels (sky gradient and lit terrain band when off). No effect above 1x, where the scaled pass does not dither |
| `C` | `CLUT cache` | Palette cache | Shows the console's CLUT cache on the sky's cloud rings (see [The CLUT cache](#the-clut-cache)) |
| `W` | `Widescreen` | 16:9 display | Switches between 320 x 240 (4:3) and 427 x 240 (16:9), widening the camera's field of view |
| `G` | `Float geometry` | Geometry path | Switches between the console's fixed-point GTE arithmetic (default) and deterministic float geometry |
| `F` | `Fog` | Depth cue / haze | Toggles GTE depth cueing and palette fading towards dusk rose; without it, distant terrain ends sharply at the far plane |
| `H` | `Panel` | On-screen panel | Toggles the corner status panel showing active switches |
| `P`, `Space` | `Pause` | Pause | Freezes the frame counter |
| `N`, `Right` | `Step` | Step | Advances one frame while paused |
| `Esc`, `Q` | — | Quit | Closes the SDL3 window |

### Where to look

The flight loop is `2768` frames (~46 seconds at 60 fps) over a `2048`-unit repeating path, with two `640`-frame (~11-second) slow passages where the camera slows to roughly one-sixth speed, descends and turns towards a landmark:

| Frames | Approx. time | Landmark | Switches to try |
|---|---|---|---|
| `603`–`1243` (nearest at `923`) | `10`–`21 s` | The wooden pier, viewed from above and beside | `T`: planks zig-zag along each quad's diagonal when off (at 1x too); `V` at 2x+: deck edges and posts step from pixel to pixel |
| `1675`–`2315` (nearest at `1995`) | `28`–`39 s` | The ring of standing stones | `V` at 2x+: stone edges snap and crawl; `T`: rock textures shift |

## What the scene shows

| Element | Console feature demonstrated | API used |
|---|---|---|
| Terrain | Repeating integer value-noise heightmap, 256 chunks of 8 x 8 cells (two triangles per cell); 4-bit `sand`, `grass` and `rock` textures with 16 haze-fade palettes each; GTE lighting (warm sun, cool sky, violet ambient) and mottled vertex colours; all visible chunks drawn with one shared model matrix so chunk seams stay closed | `jb_draw_mesh_ex`, `jb_set_light`, `jb_set_ambient` |
| Sea | Repeating 4-bit ripple texture with 15 phase-shifted palettes cycled every 4 frames via `jb_palette_write`; 4 x 4 grid of quads per chunk animated by three travelling waves (13 units trough to crest) with Gouraud wave-slope lighting; half-blended (`B/2 + F/2`) over the shaded seabed | `jb_palette_write`, repeat texture, `JB_BLEND_HALF` |
| Sun glitter | Flickering glints along the sun reflection path via integer spatial/frame hashing, fogged to black | `jb_draw_billboard`, `JB_BLEND_ADD` |
| Sky & clouds | Dithered Gouraud gradient and five azimuth-elevation rings of 4-bit drifting clouds drawn directly at the console level beneath the scene | `jb_prim_poly`, `jb_vram_load` |
| Sun, lamp, fireflies | 4-bit semi-transparent glow sprite added (`B + F`) | `jb_draw_billboard`, `JB_BLEND_ADD` |
| Lighthouse beams | Two rotating, double-sided, unlit Gouraud quads added (`B + F`) | `jb_draw_mesh_ex`, `JB_BLEND_ADD` |
| Pier | Two large 64 x 288 deck quads (deliberately unsubdivided to showcase affine wobble) with 4-bit `planks` texture over two rows of depth-biased posts | `jb_draw_mesh_ex` |
| Lighthouse | Tapering 8-sided tower with 15-bit direct-colour texture and unlit lamp room | `jb_draw_mesh_ex` |
| Trees & stones | Flat-lit untextured conifer meshes; tapered rock-textured standing stones | `jb_draw_mesh_ex` |
| Haze | GTE depth cue (1200–3800 units) combined with 16-level per-texture palette fading towards dusk rose | `jb_set_fog`, per-level CLUTs |
| Panel | Subtractive darkened box (`B - F`) with bitmap text | `jb_draw_rect`, `jb_draw_text` |

**Draw order:** because the camera stays above the sea plane, the sun, seabed and sea are biased into the farthest ordering-table bucket (in that call order, farthest chunk first), and land, structures and glows sort by depth in front of them.

### The CLUT cache

The five cloud rings share a single 16-entry palette row. Before each ring, the demo overwrites that row via `jb_vram_load` without flushing the CLUT cache (`GP0(01h)`). With the cache on (console default), the first ring (horizon) loads the cache and every higher ring reuses its sunset-gold palette; with the cache off (`C`), each ring reads its updated row so the clouds grade smoothly from gold to violet. The sea's palette animation uses `jb_palette_write`, which flushes the cache, so the sea animates regardless of the `C` switch.

### Haze and palette fading

Because the GPU modulates texels by vertex colour over 128, depth-cueing a textured face directly towards the sky haze tints bright and dark textures unevenly and leaves texel noise visible. Each terrain and sea texture therefore uses a tailored fog colour (scaled by its mean texel) plus 16 precomputed CLUTs blending its texels smoothly towards the haze colour; each chunk picks its fade level by distance.

## How the assets are made

Every texture and fixed mesh is converted from original procedural sources by `jb-tex` and `jb-mesh` (see [docs/tools.md](../../docs/tools.md) and [docs/formats.md](../../docs/formats.md)) and embedded into the binary at build time (`assets/embed.cmake` -> `demo_files.c`, loaded by `assets.c` via `jb_texture_load_mem`, `jb_texture_parse` and `jb_mesh_load_mem`). The terrain mesh is generated at run time in `terrain.c`.

1. **Generator (`assets/make-sources.c` -> `demo-make-sources`):** generates all source PNGs and OBJ/MTL files in `assets/sources/` using integer noise and `jb_sinf`/`jb_cosf`.
2. **Converters (`assets/conversions.txt`):** lists the exact `jb-tex` and `jb-mesh` commands that turn `assets/sources/` into the `.jbt` and `.jbm` files in `assets/`.

| Asset | Source | Format & notes |
|---|---|---|
| `sand.jbt`, `grass.jbt`, `rock.jbt` | `sand.png`, `grass.png`, `rock.png` | 4-bit 64 x 64 (<= 16 exact colours) |
| `water.jbt` | `water00.png`..`water14.png` | 4-bit 64 x 64 `--repeat`, 15 phase-shifted palettes (`jb-tex --frame`) |
| `cloud.jbt` | `cloud.png` | 4-bit 256 x 64 (entry 0 transparent, 15 densities; thin cloud alpha 64 -> semi-transparent) |
| `glow.jbt` | `glow.png` | 4-bit 32 x 32 (entry 0 transparent, 15 semi-transparent greys) |
| `tower.jbt` | `tower.png` | 15-bit 32 x 64 direct colour |
| `planks.jbt` | `planks.png` | 4-bit 64 x 256 pier deck planks |
| `tree.jbm` | `tree.obj` | Flat-lit cones and trunk with vertex colours |
| `stone.jbm` | `stone.obj` | Tapered block bound to `rock.jbt` |
| `lighthouse.jbm` | `lighthouse.obj` | Tower bound to `tower.jbt`, gallery, `--unlit lamp`, roof |
| `beam.jbm` | `beam.obj` | Crossed quads, `--blend beam=add --unlit beam --double-sided beam` |
| `pier.jbm` | `pier.obj` | Deck (two 64 x 288 quads bound to `planks.jbt`) and posts |

To regenerate sources, converted assets and hashes after a deliberate art or converter change:

```sh
nix develop -c cmake --build build --target demo_assets_regen
nix develop -c cmake --build build --target demo_record
```

## Tests and stored hashes

- `demo` (1 thread) and `demo_4t` (4 threads) check all 12 runs in [expected.txt](expected.txt) (`base`, `tour`, per-switch 24-tick runs against `short`/`scale2`, and `all3`). Same-length runs must produce distinct hashes, and `--expect` checks GPUSTAT dithering, frame size and a CLUT-cache probe after each run.
- `demo_web_module` (`web/test-module.cjs`, wasm builds): runs all 12 runs through `web/index.html`'s own JavaScript parser, tick loop and FNV-1a hasher under Node.
- `demo_assets` and `demo_sources`: verify that `assets/*.{jbt,jbm}` match `conversions.txt` and `assets/sources/*` match `demo-make-sources` byte for byte.
- `demo_gaps_*`: draws sampled ticks without the sky over two clear colours (and the sea opaque alone) to catch any crack, culled ridge face, missing chunk or beach slit: 44 samples at stride 64 across the loop at 1x and 2x (4:3 and widescreen, GTE and float), plus with precise vertices on in 4:3 only (GTE and float), 22 samples at stride 64 at 2x and 4 samples at stride 64 at 4x.
- `demo_fade_1x`, `demo_fade_2x`: samples 44 ticks at stride 64 at 1x and 2x, checking that crossing a terrain fade level changes no channel by more than 17 of 255 (two 5-bit steps).
- `demo_pier`: samples 40 ticks at stride 16 across the pier slow passage (starting at frame 603, at 2x), checking that no post draws in front of the deck.
- `demo_headless_cli` (`test-headless.cmake`): checks `--record`, pause/step scripts and status-2 CLI refusals.
- `web/check-browser.mjs`: manual headless Chromium check covering all runs, keys, buttons, phone layout, pause/step and frame rate.

## Frame time

Measured with `demo-headless` (CMake Release, Clang) over the full loop on an AMD Ryzen 7 7700X (shared machine, upper bounds; see [docs/performance.md](../../docs/performance.md) for dedicated benchmark measurements):

| Scale | 1 thread | 16 threads | wasm32 (Node, 1 thread) |
|---|---|---|---|
| 1x (320 x 240) | 4.7 ms | 4.4 ms | 6.4 ms |
| 2x (640 x 480) | 8.7 ms | 6.0 ms | 11.4 ms |
| 4x (1280 x 960) | 18.7 ms | 10.0 ms | 23.9 ms |

In single-threaded WebAssembly under Node, 3x measured 16.7 ms/frame (right at 60 fps) and 4x ~42 fps. The terrain submits roughly 10,400 triangles per frame across ~80 visible chunks (most back-face culled).

# Design and architecture

jitterbug is a standalone C library that renders 3D and 2D scenes with the PlayStation GPU's arithmetic, memory model and visual rules. This page describes how the codebase is structured, how its subsystems work together, the design choices made for 0.1, and what is intentionally out of scope.

## Architecture

```text
your program ── include/jitterbug.h (jb_*) ──────────────────────────────┐
                  │                                                      │
     ┌────────────┴────────────┐   ┌──────────────────┐   ┌───────────┐ │
     │ friendly API (src/api)  │   │ geometry         │   │ VRAM &    │ │
     │ frames, layers, textures│   │ (src/geom)       │   │ console   │ │
     │ sprites, font, files    │◄─►│ GTE, float maths,│   │ GP0/GP1,  │ │
     └────────────┬────────────┘   │ clip, splitter   │   │ OT        │ │
                  │                └────────┬─────────┘   └─────┬─────┘ │
                  └───────────────┬─────────┴───────────────────┘       │
                                  ▼                                     │
                       ┌──────────────────────┐                         │
                       │ core (src/core)      │                         │
                       │ GPU state, GP0/GP1   │                         │
                       │ decode, prim records,│                         │
                       │ VRAM placer          │                         │
                       └──────────┬───────────┘                         │
                          jbi_backend (src/core/backend.h)              │
                       ┌──────────┴───────────┐                         │
                       │ software backend     │   (future: GPU backend) │
                       │ (src/soft)           │                         │
                       │ 1x exact + 2x–8x,    │                         │
                       │ queue, tile hazards, │                         │
                       │ worker bands, scanout│                         │
                       └──────────┬───────────┘                         │
                                  └──── RGBA8 frame (jb_frame) ─────────┘
```

The public header (`include/jitterbug.h`) is strict C99 and compiles as C++. The implementation (`src/`) is C11 (for `<stdatomic.h>` in the worker pool) compiled with hidden symbol visibility so a shared library exports only `JB_API` functions. All state lives inside `jb_context`; there are no globals and no writable static data.

## Core

Directory: `src/core/`.

- **State and decode (`context.c`, `gp0.c`, `prim.h`):** holds the GPU's drawing state (`GP0(E1h)`–`GP0(E6h)`: texture page, texture window, draw area, draw offset, mask settings, dithering), the display state (`GP1(00h)`–`GP1(1Fh)`), the GP0 command stream state machine (so words can be split arbitrarily across `jb_gp0` calls), and decodes drawing commands into internal `jbi_prim` records.
- **VRAM layout and placement (`layout.c`, `placer.c`):** tracks the 1024x512 16-bit VRAM budget. It computes the display buffer rectangles for single- or double-buffered frames (including optional 16:9 widescreen reservation) and places friendly textures and palettes using deterministic first-fit search: texels top-down within 64/128/256-pixel texture pages, palettes column-by-column from row 511 upwards on 16-pixel x boundaries, falling back to pairwise search when texels and palette contend for the same space.
- **Backend interface (`backend.h`):** the core never touches pixel memory directly. All rendering and VRAM operations cross `jbi_backend`: `submit`, `fill`, `load`, `store`, `store_1x`, `copy`, `flush`, `scanout`, plus configuration (`configure`, `set_threads`, `set_exact_1x`, `set_clut_cache`, `clear_cache`). The interface is shaped so a future GPU backend whose VRAM lives on the graphics card can implement the same boundary without leaking pixel pointers into the core.

## Display and frame output

Files: `src/api/display.c`, `src/api/frame.c`, `src/soft/scanout.c`.

- **Arbitrary display rectangles:** on hardware, `GP1(08h)` selects one of a few fixed modes (256, 320, 368, 512 or 640 wide; 240 or 480 high). jitterbug supports those modes via `jb_gp1`, and also allows any rectangle up to 1024x512 via `jb_config.width` and `height` (for example, 427x240 for 16:9 widescreen). The 24-bit display mode (`GP1(08h)` bit 4) is supported as well.
- **Double buffering in VRAM:** `jb_begin_frame` and `jb_end_frame` alternate between two display-sized buffers inside VRAM (stacked vertically when `2 * height <= 512`, side by side when `2 * width <= 1024`, or single-buffered otherwise, as 640x480 console games did). The texture placer automatically stays out of these buffers.
- **Frame output:** `jb_frame` flushes pending work, scans out the display rectangle at the active scale, and converts 15-bit BGR555 (widening each 5-bit channel by `c << 3 | c >> 2`) or 24-bit RGB into tightly packed RGBA8 (`A = 255`). The widening loop converts in disjoint halves from the back so the compiler can vectorise it in place. The library never creates a window or talks to a display server.

## Software backend and threading

Directory: `src/soft/`.

The software backend is both the 0.1 renderer and the reference implementation:

- **Dual VRAM above 1x (`soft.c`, `raster.c`, `raster_hires.c`, `transfer.c`):** every context owns a 1 MB 1024x512 16-bit VRAM at 1x. Above 1x (2x–8x), a second scaled VRAM (`1 MB * scale * scale`) sits beside it. Public contexts (`jb_create`) always enable exact 1x read-back, so primitives draw into both VRAMs (lazy reconstruction of stale 1x tiles from scaled VRAM exists only as an internal backend mode, `jbi_set_exact_1x`). 4- and 8-bit textures and palettes are always read from 1x VRAM, while 15-bit textures drawn in VRAM can be sampled at sub-texel detail from scaled VRAM.
- **Exact integer path and float options:** with `precise_vertices` and `perspective_correct` off, the 1x rasteriser (`raster.c`) uses integer arithmetic only. Above 1x, `raster_hires.c` applies the same 12-bit gradient and half-unit rules sampled at scaled pixel centres, using 64-bit arithmetic for ordinary triangles and 128-bit (`__int128`) intermediates for very thin precise triangles.
- **Deterministic multi-threading (`soft.c`, `workers.c`):**
  - With 1 thread at 1x, primitives rasterise immediately as submitted.
  - With multiple threads (or above 1x), primitives collect in a queue (up to 8192 entries) until a sync point: an explicit flush, a VRAM transfer, a full queue, or a tile hazard.
  - Hazards are tracked in 32x32-pixel tiles of 1x VRAM. The queue flushes before any primitive that reads texels or a CLUT a queued primitive may have written, or writes into tiles a queued primitive reads. A primitive that samples its own destination tiles cannot be split safely across concurrent bands and is rasterised alone on the calling thread, row by row.
  - On flush, worker `i` of `K` rasterises every queued primitive in queue order for interleaved 16-row bands `i, i + K, ...`. Because every pixel belongs to exactly one worker and receives primitives in submission order, the pixels never depend on thread count.
- **The CLUT cache (`soft.c`):** because worker threads rasterise later than commands arrive, a CLUT load copies the active palette (16 entries for 4-bit, 256 for 8-bit) out of 1x VRAM into the backend's palette pool when the primitive is submitted. Subsequent primitives reusing the cached palette point to that copy and create no tile hazard on the CLUT's VRAM address.
- **Thread shim (`workers.c`):** all platform threading is isolated to `workers.c` (POSIX pthreads plus C11 atomics, spinning briefly before sleeping on a condition variable between bursts). Built with `-DJB_THREADS=OFF` (always so for Emscripten), `workers.c` compiles with `JBI_NO_THREADS`: no thread headers, no thread library, every job runs on the calling thread.

## Geometry and the polygon splitter

Directory: `src/geom/`.

- **GTE (`gte.c`, `fixed.c`, `precise.c`):** software implementation of all 22 documented GTE commands with the console's integer precision, register packing, saturation and `FLAG` behaviour. Each context has two independent GTE instances: the public one driven by `jb_gte_*`, and a private one used by the friendly 3D pipeline so the two never clobber each other's registers. For console-level `jb_prim_*` calls with `precise_vertices` or `perspective_correct` enabled, `precise.c` records projected sub-pixel positions and depths keyed by integer screen coordinate; friendly-level records carry their own sub-pixel positions and depths directly without a lookup table.
- **Deterministic float maths (`fmath.c`):** `jb_sinf`, `jb_cosf`, `jb_tanf` and the vector/matrix helpers use only `+`, `-`, `*`, `/` and `sqrtf` (correctly rounded IEEE 754 operations), with `-ffp-contract=off` and canonical quiet NaN (`0x7FC00000`), avoiding libm's platform-dependent trigonometric implementations.
- **3D pipeline (`pipeline.c`, `scene.c`):** transforms, lights (up to 3 directional lights + ambient), fogs (linear in `1/z`), culls, and sorts meshes, billboards and 3D lines into the frame's ordering table. Facing is judged per output (whole pixels for 1x and non-precise scaled output; drawn sub-pixel positions for the precise scaled pass) so nearly edge-on neighbours do not crack apart. Faces that cross the near/far planes or the GTE's guard band are clipped cleanly in view space (near/far) and screen space (guard band), keeping shared edges consistent between neighbours.
- **Screen-space polygon splitter (`split.c`, `split.h`):** hardware primitives are limited to coordinates in `-1024..1023` and spans of at most 1023x511 pixels. Any friendly 2D or 3D polygon within those limits passes through untouched. Any polygon exceeding them is clipped exactly to a tile guard-band rectangle (up to 1023x511 pixels centred on the frame, or on each half of a frame taller than 511 pixels), then triangulated by exact ear clipping after rounding cut vertices. Edges that fit within the GPU's limits are kept whole (using small "ear" triangles whose excess lies outside the drawing area) so clipped polygons meet passed-through neighbours without seams. All splitter arithmetic is 64- and 128-bit integer maths.

## Runtime options

All non-console enhancements are off by default so a fresh context starts with the console's look (except dithering, which starts off just as after a hardware `GP1(00h)` reset and is enabled with `jb_set_dither(jb, 1)`):

- `scale` (1x–8x)
- `precise_vertices` (sub-pixel vertex positions above 1x)
- `perspective_correct` (perspective-correct texture and colour interpolation)
- `dither` (4x4 dithering on shaded/modulated polygons and lines at 1x)
- `clut_cache` (hardware palette caching on/off)
- `widescreen` (16:9 display width reservation and toggle)
- `geometry` (`JB_GEOMETRY_GTE` fixed-point vs `JB_GEOMETRY_FLOAT`)

## Design choices for 0.1

1. **One software backend.** The software renderer is the exact reference and already scales to 8x across CPU cores. Shipping 0.1 with software alone keeps the library small, pure C, dependency-free, and straightforward to verify; a Vulkan backend for higher resolutions (up to 16x) is deferred until after 0.1 ([todo.md](../todo.md)).
2. **WebAssembly as a first-class target.** Hobby and game-jam developers frequently publish on the web, and a browser demo lets anyone inspect the renderer without building anything. Because browser threads require cross-origin isolation headers that static hosts often omit, the Emscripten build is single-threaded by default.
3. **Claim only what is tested.** Hardware conformance and determinism statements are bounded strictly by what the automated test suite and real-console dumps verify ([conformance.md](conformance.md), [determinism.md](determinism.md)).
4. **No global state and no steady-state per-frame allocation.** Contexts are self-contained and accept custom allocators. Frame records are allocated on demand on the first `jb_begin_frame` and scanout storage is allocated (and grown if scale or display size increases) on demand in `jb_frame`, so steady-state frame loops never call `malloc`.

## Out of scope

- **Engine features:** no scene graph, physics, audio, input, windowing, editor, or asset pipeline beyond `jb-tex` and `jb-mesh`.
- **Console emulation:** jitterbug does not emulate a CPU, BIOS, CD-ROM, DMA controller, or run existing PlayStation software.
- **Hardware timing and revision quirks:** GPUSTAT busy/ready timing, memory bandwidth, the texture cache (which affects speed rather than pixels), display interlacing, and differences between GPU silicon revisions are not modelled.
- **Game-specific code or assets:** everything authored by this project (examples, demo art, bitmap font, seed traces and converter test inputs) is original. The conformance test suite also includes real-console reference dumps, logs and GTE cases from `ps1-tests` under its MIT licence, and fetches three lists embedding third-party images locally for testing without checking them into the repository ([third_party/ps1-tests/README.md](../third_party/ps1-tests/README.md)).

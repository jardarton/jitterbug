# API tour

The public API lives in a single C99 header, [include/jitterbug.h](../include/jitterbug.h), which is also the complete reference. Every symbol begins with `jb_` or `JB_`, the header compiles as strict C99 and C++11, and there is no global state.

This page groups the API by task and points to the matching section of [include/jitterbug.h](../include/jitterbug.h) for full parameter ranges, edge cases and error codes.

## Two levels of API

The library is organised in layers that share one context and one 1024x512 VRAM:

- **The friendly level** (`The frame`, `Textures`, `2D drawing`, `Sprites and text`, `Scene geometry`, `3D drawing`): manages double-buffered display areas, places textures and palettes in VRAM, records 2D layers and depth-sorted 3D draws, clips geometry against the near/far planes and the GPU's coordinate limits, and flushes everything at `jb_end_frame`. Most new programs start here.
- **The console level** (`The GPU's ports`, `VRAM`, `Drawing`, `The ordering table`, `Geometry`, `Helpers`): exposes the PlayStation GPU's GP0/GP1 command ports, raw VRAM transfers, unclipped primitives, explicit ordering tables and the fixed-point GTE coprocessor. Console-level calls execute immediately against VRAM and refuse values outside the hardware's bit widths.

Both levels work on the same `jb_context`. For example, [examples/demo](../examples/demo/README.md) draws its sky at the console level right after `jb_begin_frame` and everything else at the friendly level.

## Version and errors

Header section: `Results`.

- `JB_VERSION_MAJOR`, `JB_VERSION_MINOR`, `JB_VERSION_PATCH`, `JB_VERSION` — compile-time version macros (`0.1.0`).
- `jb_version()`, `jb_version_string()` — runtime version check.
- `jb_result` — `JB_OK`, `JB_ERROR_INVALID_ARGUMENT`, `JB_ERROR_OUT_OF_MEMORY`, `JB_ERROR_UNSUPPORTED`. `jb_result_string(r)` gives a short constant string.
- `jb_error_string(jb)` — detail of the most recent failed call on `jb`.
- `jb_check(jb, &detail)` — sticky error. Every failing call on a context latches the *first* failure until `jb_check` or `jb_end_frame` consumes and clears it, so frame rendering code can draw unchecked and inspect `jb_end_frame(jb)` once at the end.

The library never aborts and never prints to `stdout` or `stderr`; diagnostics go only to `jb_config.log` if provided.

## Contexts, memory and threading

Header section: `Contexts`.

- `jb_create(const jb_config *config, jb_result *result)` — allocates and resets a context (`config == NULL` selects all defaults: 1x scale, 320x240 display, auto thread count, console options on/off as on hardware, GTE geometry path, 8192 frame primitive records, 1024 ordering-table buckets).
- `jb_destroy(jb_context *jb)` — joins worker threads and frees all context memory.
- **Custom allocator and logging:** pass `alloc`/`free`/`alloc_user` and `log`/`log_user` in `jb_config`. After warm-up (first `jb_begin_frame` and first `jb_frame` at a given scale/size), drawing a frame makes no allocations.
- **Threading contract:** use a single `jb_context` from one thread at a time. Distinct contexts are completely independent and may run concurrently on different threads. Internal rasterisation worker threads are hidden inside calls and never invoke user callbacks.

## Runtime switches

Header section: `Contexts` (and `Drawing` for `jb_set_dither`).

The renderer's runtime switches can be changed without recreating the context (15-bit colour, the 1024x512 VRAM model and back-to-front depth sorting have no switches in 0.1):

| Function | Config field | Default | At 1x | Above 1x |
|---|---|---|---|---|
| `jb_set_scale(jb, scale)` | `scale` | `1` | 1x VRAM | 2x–8x scaled picture beside 1x VRAM |
| `jb_set_threads(jb, n)` | `threads` | `0` (auto, up to 16) | 1–64 threads (clamped to 1 if `JB_THREADS=OFF`) | Same |
| `jb_set_precise_vertices(jb, on)` | `precise_vertices` | `0` (off) | No effect | Eligible polygons and lines (friendly 3D draws, cut vertices of clipped 2D polygons, and console-level vertices matching a recent GTE `RTPS`/`RTPT` projection) draw at sub-pixel positions; rectangles and unclipped friendly 2D vertices keep integer positions |
| `jb_set_perspective_correct(jb, on)` | `perspective_correct` | `0` (off) | Eligible textured triangles with vertex depths (friendly 3D faces, and console-level textured triangles whose vertices match recent GTE projections) draw perspective-correctly into 1x VRAM (no longer console output); friendly 2D vertices (which have no depth) and other textured triangles stay affine | Scaled picture only for eligible textured triangles (1x VRAM stays affine) |
| `jb_set_dither(jb, on)` | — | `0` (off, as after GPU reset) | 4x4 dithering on shaded/modulated polygons and all lines | Affects 1x VRAM only; scaled pass never dithers |
| `jb_set_clut_cache(jb, on)` | `clut_cache_off` | on (`clut_cache_off = 0`) | Console CLUT cache on/off | Same |
| `jb_set_widescreen(jb, on)` | `widescreen` | `0` (off) | Switches between `width` and 16:9 (`round(height * 16 / 9)`) next frame | Same |
| `jb_set_geometry(jb, mode)` | `geometry` | `JB_GEOMETRY_GTE` | `JB_GEOMETRY_GTE` or `JB_GEOMETRY_FLOAT` for friendly 3D draws | Same |

To enable `jb_set_widescreen(jb, 1)` at run time, reserve room for the wider buffers by setting `jb_config.widescreen = 1` at `jb_create`.

## Frames, layers and output

Header sections: `The frame`, `Output`.

- `jb_begin_frame(jb, clear_color)` — selects the back buffer (double-buffered in VRAM when two buffers fit, single-buffered for modes such as 640x480), sets the drawing area and offset, clears the back buffer to `clear_color`, resets the frame's record lists, sets the 2D layer to `JB_LAYER_OVERLAY`, and clears remembered GTE projections.
- `jb_set_layer(jb, layer)` — directs subsequent 2D draws to `JB_LAYER_BACKGROUND` (behind 3D) or `JB_LAYER_OVERLAY` (in front of 3D).
- `jb_end_frame(jb)` — flushes background draws (call order), 3D scene buckets (farthest to nearest, call order within each bucket), and overlay draws (call order), switches the display start (`GP1(05h)`), enables the display on the first frame, and returns and clears the sticky error.
- `jb_frame_size(jb, &w, &h)` — returns the frame dimensions in 1x pixels (useful for 2D layout across 4:3 and widescreen).
- `jb_frame(jb, &w, &h)` — waits for pending work, scans out the display area at the current scale, and returns a pointer to tightly packed `R, G, B, A` bytes (`A = 255`). The pointer stays valid until the next `jb_frame`, `GP1(00h)` or `jb_destroy`.

## Textures and VRAM placement

Header section: `Textures` (and `Helpers` for colour macros).

- `jb_texture_create(jb, &desc, &out)` — finds free space in VRAM (avoiding the display buffers and reserved regions), packs 4-, 8- or 15-bit texels into one texture page, uploads the texels and (for 4/8-bit) palette, clears the CLUT cache (`GP0(01h)`), and populates `jb_texture`.
- `jb_texture_destroy(jb, &tex)` — returns a placed texture's VRAM space to the placer.
- `jb_palette_write(jb, &tex, first, count, colors)` — updates palette entries in place and sends `GP0(01h)` so the new colours take effect immediately even with the CLUT cache on.
- `jb_texture_write(jb, &tex, x, y, w, h, texels)` — updates a sub-rectangle of texels in place.
- `jb_vram_reserve(jb, x, y, w, h)` — permanently reserves a VRAM rectangle so the placer never puts friendly textures there (for custom render targets or manual VRAM layouts).
- `jb_palette_from_rgba(rgba, count, out)` — converts RGBA8 colours to 15-bit VRAM colours: alpha `0` becomes `0x0000` (`JB_TEXEL_TRANSPARENT`, never drawn), alpha `1..127` sets bit 15 (`JB_TEXEL_SEMI`, blends when the primitive blends), alpha `128..255` clears bit 15 except opaque black which becomes `0x8000` (`JB_TEXEL_BLACK`).

Set `jb_texture_desc.repeat = 1` for power-of-two textures (8..256 each way) that tile under the hardware texture window; friendly draws manage the texture window automatically and restore the console-level window at `jb_end_frame`.

## 2D drawing, sprites and text

Header sections: `2D drawing`, `Sprites and text`.

- `jb_draw_poly`, `jb_draw_line`, `jb_draw_polyline`, `jb_draw_rect` — record the same structs as `jb_prim_*` (`jb_poly`, `jb_line`, `jb_polyline`, `jb_rect`) into the current 2D layer, with positions in frame pixels of any `int` value. Primitives within the GPU's limits pass through unchanged; larger or off-screen ones are clipped and split into valid hardware pieces. Note that 2D quads in `jb_poly` use the GPU's Z vertex order (top-left, top-right, bottom-left, bottom-right), whereas 3D mesh quads in `jb_mesh_face` go anticlockwise around the face.
- `jb_draw_sprite(jb, &sprite)` — draws a texture or sub-rectangle at `(x, y)` with optional scaling, tint (`0` = `JB_NEUTRAL`, `JB_TINT_BLACK` = black), blend mode (`JB_BLEND_*`), `raw`, and `flip_x`/`flip_y`. 1:1 sprites emit a crisp hardware rectangle; scaled sprites emit a textured quad.
- `jb_draw_text(jb, x, y, color, text)`, `jb_draw_text_blended(jb, x, y, color, blend, text)` — draws ASCII text (with `\n` line breaks) in jitterbug's original 8x8 bitmap font. The font is placed into VRAM lazily on the first visible character drawn. `jb_text_size(text, &w, &h)` computes the pixel bounding box without a context.

## 3D scene geometry and drawing

Header sections: `Scene geometry`, `3D drawing`.

The friendly 3D world is right-handed with `+y` up, and the camera looks down `-z` in view space.

- **Deterministic float maths:** `jb_sinf`, `jb_cosf`, `jb_tanf`, `jb_vec3_dot`, `jb_vec3_cross`, `jb_vec3_normalize`, `jb_mat4_identity`, `jb_mat4_translate`, `jb_mat4_scale`, `jb_mat4_rotate_x/y/z`, `jb_mat4_mul`, `jb_mat4_trs`, `jb_mat4_look_at`, `jb_mat4_transform_point`. All avoid libm trigonometry so motion and transforms hash identically across platforms.
- **Scene state:** `jb_set_camera(jb, &cam)` (defaults: origin looking down `-z`, `fov_y = 60`, `z_near = 32`, `z_far = 4096`, optional explicit `projection` distance `H`), `jb_set_ambient(jb, color)` (defaults to full white so untextured/unlit setups are visible until lowered), `jb_set_light(jb, index, &light)` (up to 3 directional lights, `intensity` up to `7.99`), and `jb_set_fog(jb, color, start, end)` (depth cue linear in `1/z`).
- **3D draws:**
  - `jb_draw_mesh(jb, &mesh, &model)` and `jb_draw_mesh_ex(jb, &draw)` (adds `depth_bias` and per-draw `tint`). Faces use `jb_mesh_face` (3 or 4 anticlockwise vertex indices) and `jb_material` (`texture`, `color`, `blend`, `unlit`, `raw`, `double_sided`).
  - `jb_draw_billboard(jb, &billboard)` — camera-facing textured or flat quad/rectangle with fog and `depth_bias`.
  - `jb_draw_line3d(jb, &line)` — Gouraud 3D line clipped at near/far planes and the frame, with fog and `depth_bias`.

In `JB_GEOMETRY_GTE` mode, unclipped faces go through the context's internal GTE arithmetic (without disturbing the user-facing `jb_gte_*` registers); in `JB_GEOMETRY_FLOAT` mode, the pipeline transforms positions in single-precision `float`, performs view-space calculations and clipping in `double`, and rounds to the same screen formats and depth buckets.

## Asset files

Header sections: `Textures` (texture files), `3D drawing` (mesh files). Formats are specified in [docs/formats.md](formats.md) and produced by the tools in [docs/tools.md](tools.md).

- **Textures (`.jbt`):**
  - `jb_texture_parse(jb, data, size, &desc, &palettes)` — validates a 2-byte-aligned `.jbt` buffer in place and fills a `jb_texture_desc` pointing into it, plus the count of animation palettes (for `jb_palette_write`).
  - `jb_texture_load_mem(jb, data, size, &out)` — validates and uploads into VRAM (any alignment, no allocation).
  - `jb_texture_load(jb, path, &out)` — reads a file (up to 64 MB) and loads it.
- **Meshes (`.jbm`):**
  - `jb_mesh_load_mem(jb, data, size, textures, texture_count, &out)` — validates a `.jbm` buffer, checks bound texture sizes against each slot, and allocates a `jb_mesh` from the context's allocator.
  - `jb_mesh_load(jb, path, textures, texture_count, &out)` — file convenience wrapper.
  - `jb_mesh_destroy(jb, &mesh)` — frees a loaded mesh (any remaining meshes are freed on `jb_destroy`).

## Console level: GP0/GP1, VRAM, primitives, ordering tables and GTE

Header sections: `The GPU's ports`, `VRAM`, `Drawing`, `The ordering table`, `Geometry`, `Helpers`.

For homebrew code, console-style pipelines, or exact hardware testing:

- **Ports:** `jb_gp0(jb, words, count)` (streamable word-by-word or in arbitrary chunks), `jb_gp1(jb, word)`, `jb_gpuread(jb)`, `jb_gpustat(jb)`. Word builder macros live in `Helpers` (`JB_GP0_*`, `JB_GP1_*`).
- **VRAM transfers:** `jb_vram_load` (`GP0(A0h)`), `jb_vram_read` (`GP0(C0h)`), `jb_vram_fill` (`GP0(02h)`), `jb_vram_copy` (`GP0(80h)`). All four obey hardware VRAM wrapping; `jb_vram_load` and `jb_vram_copy` obey the mask settings (`jb_set_mask`), `jb_vram_fill` ignores mask settings and writes mask bit `0`, and `jb_vram_read` reads 1x VRAM without applying mask settings. Remember to send `JB_GP0_CLEAR_CACHE` (`GP0(01h)`) after rewriting a palette in place with `jb_vram_load` or `jb_vram_fill` while the CLUT cache is enabled.
- **Immediate primitives and GPU state:** `jb_prim_poly`, `jb_prim_line`, `jb_prim_polyline`, `jb_prim_rect`, plus `jb_set_draw_area`, `jb_set_draw_offset`, `jb_set_texture_window`, `jb_set_mask`, `jb_set_dither`. Arguments outside hardware bit ranges return `JB_ERROR_INVALID_ARGUMENT`.
- **Ordering table:** `jb_ot_create`, `jb_ot_destroy`, `jb_ot_clear`, `jb_ot_add_poly`, `jb_ot_add_line`, `jb_ot_add_polyline`, `jb_ot_add_rect`, `jb_ot_draw`. Draws buckets from highest to lowest, FIFO within each bucket.
- **GTE coprocessor:** `jb_gte_write`, `jb_gte_read`, `jb_gte_command` (all 22 documented `JB_GTE_*` commands, exact `FLAG` bits), `jb_gte_begin_frame` (clears remembered sub-pixel projections for `precise_vertices` / `perspective_correct`). Convenience helpers: `jb_gte_set_matrix`, `jb_gte_set_vector`, `jb_gte_project`, `jb_gte_light`, `jb_gte_set_lights`, `jb_gte_set_light_directions`, pure `jb_gte_nclip` / `jb_gte_avsz3` / `jb_gte_avsz4`, and fixed-point maths `jb_isin`, `jb_icos`, `jb_gte_rotation_matrix`, `jb_gte_mul_matrix`, `jb_gte_mul_vector`.

## Known limits

Documented in [include/jitterbug.h](../include/jitterbug.h) (`Contexts`, `2D drawing`, `3D drawing`):

- **Column 1023 (and 1022 in GTE mode) on 1024-wide frames:** hardware polygon vertices stop at `1023` and the fill rule omits right edges, so polygons cannot fill column `1023` of a 1024-wide frame (lines and rectangles can). In GTE mode, a face with an edge lying on the right edge of the guard band at `x = 1023` also stops at column `1022`.
- **Sub-unit slivers:** when a polygon exceeds the GPU's limits and is clipped, a sliver less than about one unit wide where it crosses the clip rectangle may lose pixels or be dropped rather than fold over.
- **Extra records for cut polygons:** a clipped polygon with a vertex whose sub-pixel position is more than a pixel away from its whole-pixel position uses a second set of records for the precise-vertices pass (at any scale, because switches can change before `jb_end_frame`), which counts against `jb_config.max_primitives`.
- **GTE register bounds:** in `JB_GEOMETRY_GTE` mode, extreme fog sharpness (`DQA`/`DQB` overflow), tiny `z_far` below `H/2 + 1` GTE units, or mesh coordinates that overflow `int16_t` after view-scale folding are refused with a diagnostic naming the limit.

# Tools

jitterbug includes three command-line tools:

- **`jb-tex`** — converts a PNG image into a `.jbt` texture (`4`-, `8`- or `15`-bit, with quantisation, optional Floyd-Steinberg dithering and palette-animation frames).
- **`jb-mesh`** — converts an OBJ or glTF 2.0 model into a `.jbm` mesh with integer positions, 4.12 normals, vertex colours, texel coordinates and texture slots.
- **`jb-bench`** — times the software renderer across fixed workloads, internal scales, thread counts and runtime options.

The binary formats written by `jb-tex` and `jb-mesh` are documented in [docs/formats.md](formats.md).

## Building and installing

Pass `-DJB_TOOLS=ON` when configuring CMake (`jb-tex` and `jb-mesh` are also built automatically when `JB_TESTS=ON`):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJB_TOOLS=ON
cmake --build build
cmake --install build   # installs into <prefix>/bin/
```

With Nix:
- `nix build` (`packages.default`) installs the library and all three tools under `result/bin/`.
- `nix build .#bench` installs a Release `jb-bench` binary on its own.

All three tools also compile under Emscripten and run under Node (`node jb-tex.js ...`).

## `jb-tex`

Converts a PNG image (`1..256` pixels in each dimension) into a `.jbt` file for `jb_texture_load`, `jb_texture_load_mem` or `jb_texture_parse`.

```sh
jb-tex [--depth 4|8|15] [--dither] [--repeat] [--frame PNG]... INPUT.png OUTPUT.jbt
```

| Option | Effect |
|---|---|
| `--depth 4\|8\|15` | Texel colour depth: `4`-bit (16-colour palette), `8`-bit (256-colour palette, the default) or `15`-bit direct colour. |
| `--dither` | Diffuse quantisation error to neighbouring pixels using Floyd-Steinberg dithering. |
| `--repeat` | Mark the texture as repeating (tiling) under the hardware texture window. Width and height must be powers of two in `8..256`. |
| `--frame PNG` | Derive an additional palette for the same texels from another PNG of the same dimensions, for palette animation (`4`- and `8`-bit only). May be given up to 256 times, in frame order; `jb_texture_parse` exposes all palettes for `jb_palette_write`. |
| `-h`, `--help` | Print usage and exit. |

### How a PNG becomes a texture

1. **15-bit colour conversion:** each pixel is converted to a 15-bit VRAM colour identically to `jb_palette_from_rgba`: each RGB channel keeps its top 5 bits; alpha `0` becomes `0x0000` (transparent, never drawn); alpha `1..127` sets bit 15 (semi-transparent when the primitive blends); alpha `128..255` is opaque, with opaque black encoded as `0x8000` so it is not treated as transparent.
2. **15-bit output (`--depth 15`):** the converted colours are written directly as texels. With `--dither`, each channel selects the 5-bit value whose displayed 8-bit expansion (`c << 3 | c >> 2`, matching `jb_frame`) is nearest to the pixel's 8-bit value plus accumulated error, diffusing `7/16` right, `3/16` down-left, `5/16` down (each truncated toward zero) and the remaining error (`1/16` plus the division remainder) down-right. Transparent pixels neither receive nor diffuse error.
3. **Paletted output (`--depth 4` or `8`):** if any pixel is transparent, palette entry `0` is reserved as `0x0000` and transparent pixels take index `0`. If the remaining distinct colours fit within the available entries, they are kept verbatim in sorted value order. Otherwise, jitterbug's own median-cut quantiser selects the palette: colours with and without the semi-transparency bit start in separate boxes, and the box with the largest `(widest channel range) * (pixel count)` is split at the pixel-weighted median of that channel until the target number of boxes is reached; each entry is its box's pixel-weighted mean rounded to the nearest integer. Without `--dither`, every pixel maps to the nearest entry with the matching semi-transparency bit by least squared difference in 5-bit channels (ties to the lower index). With `--dither`, matching compares the pixel's 8-bit channels plus accumulated error against each entry's expanded 8-bit channels (`c << 3 | c >> 2`), diffusing the error with the same Floyd-Steinberg weights and down-right remainder.
4. **Animation frames (`--frame`):** for each additional PNG, palette entry `i` is the mean of that frame's 5-bit colours over all pixels assigned index `i`, preserving entry `i`'s semi-transparency bit; if an opaque entry averages to `0x0000`, bit 15 is set (`0x8000`) so it stays opaque black rather than encoding transparency. The transparent entry and any unused entry remain unchanged from the first palette; the frame PNG's alpha channel is ignored.

### Determinism

`jb-tex` uses integer arithmetic throughout (including PNG decoding via `third_party/stb`), so the same PNG and flags produce identical `.jbt` bytes on every supported platform. Its outputs are pinned byte for byte in `tests/files/expected/` and checked by `convert_*` tests.

## `jb-mesh`

Converts Wavefront OBJ (with MTL) or glTF 2.0 (`.gltf` or `.glb`) into a `.jbm` file for `jb_mesh_load` or `jb_mesh_load_mem`.

```sh
jb-mesh [options] INPUT.obj|INPUT.gltf|INPUT.glb OUTPUT.jbm
```

| Option | Effect |
|---|---|
| `--texture MATERIAL=FILE` | Assign texture `FILE` (`.jbt` or `.png`) to `MATERIAL` so its `u, v` coordinates are mapped to that texture's size (and, for a `.jbt`, its repeat flag). Each distinct `FILE` creates a texture slot numbered from `0` in command-line order; pass textures to `jb_mesh_load` in that same order. |
| `--scale S` | Multiply vertex positions by `S` (default `1`). |
| `--fit N` | Scale positions uniformly so the largest absolute coordinate equals `N`. |
| `--no-normals` | Omit vertex normals (faces are lit using their flat face normals). |
| `--no-colors` | Omit vertex colours. |
| `--blend MATERIAL=MODE` | Set semi-transparency mode: `none`, `half`, `add`, `sub` or `quarter`. |
| `--unlit MATERIAL`, `--lit MATERIAL` | Disable or enable lighting for `MATERIAL` (lit by default). |
| `--raw MATERIAL` | Draw textured faces without colour modulation. |
| `--double-sided MATERIAL`, `--single-sided MATERIAL` | Disable or enable backface culling for `MATERIAL` (single-sided by default). |
| `-h`, `--help` | Print usage and exit. |

`MATERIAL` names a material as given in the input file, `default` for OBJ faces without a material, `#k` for unnamed (or duplicate-named) glTF material index `k`, or `*` for every material. Options apply in command-line order over settings from the input file. Naming a material that does not exist is an error that lists the available material names. On success, `jb-mesh` prints a summary of what it wrote, including the texture slot list.

### Supported input formats

- **Wavefront OBJ** (`tools/jb-mesh/obj.c`, jitterbug's own reader):
  - Statements read: `v` (with optional vertex colour `v x y z r g b`), `vt`, `vn`, `f` (`v`, `v/vt`, `v//vn`, `v/vt/vn`, including negative indices), `usemtl`, `mtllib` (MTL: `newmtl`, `Kd`, `d`, `Tr`, and `map_Kd`, which only records that a texture was mentioned), `#` comments, and backslash line continuations (before LF or CRLF). Unknown directives (`o`, `g`, `s`, etc.) are ignored.
  - On `usemtl`, `newmtl` and `mtllib` lines, `#` starts a comment only when preceded by a space or tab and followed by a space, a tab or the end of the line (so `usemtl red # note` names `red`, while `Material #1` and `mat#1` stay whole). Only the numeric directives (`v`, `vt`, `vn`, `Kd`, `d`, `Tr`) and `f` stop at any `#` that begins a token; `map_Kd` only records that a texture was mentioned, and unknown directives are ignored.
  - Triangles and quads are kept as triangles and quads, bypassing ear clipping and its area and normal checks. Faces of 5 to 1024 corners are cut into triangles by ear clipping, projected along the dominant axis of the face's Newell normal, so concave faces convert cleanly (the old first-corner fan is preserved only for strictly convex faces; collinear corners can change the triangulation).
  - `vt` vertical coordinates are flipped (`1 - v`) because OBJ places `(0, 0)` at the bottom-left.
  - Refused with `path:line`: faces of 5 to 1024 corners with zero area or a non-finite Newell normal (`a face of N corners with no area`), faces of 5 to 1024 corners where ear clipping fails (`a face of N corners that cannot be cut into triangles (does it cross itself?)`), faces with more than 1024 corners, and `usemtl` or `newmtl` lines with no name.
  - Known reader limits: self-intersection is not checked explicitly (some self-crossing faces are refused, while others can produce overlapping triangles); faces with holes are not supported; and collinear corners can cause a refusal.
- **glTF 2.0** (`tools/jb-mesh/gltf.c`, via `third_party/cgltf` 1.15):
  - Reads `.gltf` (external or data-URI buffers) and `.glb`.
  - Traverses the default scene's nodes, baking each node's world transform into positions (and inverse transpose into normals, reversing triangle winding on mirroring transforms).
  - Reads `POSITION`, `NORMAL`, `TEXCOORD_0`, `COLOR_0` (RGB), triangle lists/strips/fans, base colour factor, `doubleSided`, `alphaMode` `BLEND`, and `KHR_materials_unlit`.
  - Points and lines are refused; skins, morph targets and other extensions are ignored.

If a material references a texture in its MTL or glTF file but is given no `--texture` option, `jb-mesh` prints a warning and writes the material untextured.

### Output quantisation

- **Positions:** scaled and rounded to the nearest integer (halves away from zero). Coordinates outside `-32768..32767` fail with a message naming the face and suggesting a smaller `--scale` or `--fit`.
- **Normals:** normalised, multiplied by `4096`, and rounded to `i16` (`-4096..4096`). If some corners lack normals, they receive their face's flat normal.
- **Colours:** for textured materials, a factor of `1.0` maps to `128` (`JB_NEUTRAL`); for untextured materials, `1.0` maps to `255`. Vertex colours multiply the material colour; corners without a vertex colour inherit the material colour. Pure black material tints are encoded as `JB_TINT_BLACK`.
- **Texel coordinates (`u, v`):** mapped to `[0, width - 1]` and `[0, height - 1]` with `(0, 0)` at the top-left (`1.0` maps to the last texel). Values within half a texel outside `0..1` clamp to the edge; larger excursions are errors unless the assigned `.jbt` has `--repeat`, in which case each face's coordinates are shifted by whole repeats so its minimum lies in the first tile and its span fits within 256 texels.
- **Deduplication:** identical corners (position, normal, colour, `u, v`) are merged into shared vertices in order of first appearance, up to the 65,536-vertex limit.

### Determinism

`jb-mesh` uses jitterbug's own decimal number parser built from IEEE 754 `double` arithmetic (also plugged into `cgltf` via `CGLTF_ATOF`) instead of the C library's `strtod`/`atof`, compiles with `-ffp-contract=off`, and uses only `sqrt` from libm. The same inputs produce identical `.jbm` bytes across compilers and architectures (verified by the `convert_*` tests in `tests/files/`).

## `jb-bench`

Times the software renderer on three deterministic 320x240 workloads (`first-light`, `heavy` and `fill`) through the public API. See [docs/performance.md](performance.md) for workload descriptions and published results.

```sh
jb-bench [options]
```

| Option | Default | Effect |
|---|---|---|
| `--workloads LIST` | `first-light,heavy,fill` | Comma-separated subset of workloads to run. |
| `--scales LIST` | `1,2,3,4,6,8` (`1,2,3` under Emscripten) | Comma-separated internal scales (`1..8`). |
| `--threads LIST` | `1,2,4,8,0` (`1` under Emscripten) | Comma-separated thread counts (`0` = auto, one per CPU up to 16). |
| `--options LIST` | `off,on` | `off` (console defaults) and/or `on` (precise vertices + perspective-correct textures). |
| `--frames N` | `120` | Timed frames per configuration. |
| `--warmup N` | `10` | Untimed warm-up frames before timing. |
| `--max-time S` | `10` | Stop a configuration early after `S` seconds once at least 30 frames have been timed. |
| `--markdown` | — | Format results as Markdown tables. |
| `--csv` | — | Format results as CSV. |
| `--stats` | — | Print primitive counts, pixel areas and overdraw for each workload without timing. |
| `--quick` | — | Short run: `--scales 1,2 --threads 1,0 --frames 20 --warmup 3` (`--threads 1` under Emscripten). |
| `--check` | — | Draw short deterministic runs and verify their frame hashes against stored values (exit status `1` on mismatch). Times nothing; registered as `jb_bench_check` in CTest. |

# File formats

jitterbug reads two binary formats of its own: `.jbt` for a texture and `.jbm` for a mesh. The converters `jb-tex` and `jb-mesh` ([docs/tools.md](tools.md)) write them from PNG, OBJ and glTF files; the library loads them via `jb_texture_parse`, `jb_texture_load_mem`, `jb_texture_load`, `jb_mesh_load_mem`, `jb_mesh_load` and `jb_mesh_destroy` ([include/jitterbug.h](../include/jitterbug.h)). Both formats map closely to the library's drawing structs (`jb_texture_desc`, `jb_mesh`): `.jbt` texels and palettes match `jb_texture_desc` directly (with 4-bit and 8-bit indices packed into 16-bit VRAM words on upload), and `.jbm` stores little-endian records that the loader decodes into a `jb_mesh` while resolving each material's texture slot to a bound `jb_texture`.

The constants and offsets below match [src/api/formats.h](../src/api/formats.h). (The `.jbt` files under `tests/seed/` are a separate, test-only trace format with magic `JBSEEDT`; see [tests/seed/FORMAT.md](../tests/seed/FORMAT.md).)

## Conventions

- **Byte order and types:** little-endian throughout. `u8`, `u16` and `u32` are unsigned 1-, 2- and 4-byte integers; `i16` is a two's-complement signed 16-bit integer.
- **Header and size:** every file starts with a 4-byte magic, a `u32` version (`1`), and a `u32` total file size in bytes. The header's `size` field must match the exact byte length of the file or memory buffer; any mismatch (truncated input or extra bytes past `size`) is refused.
- **Sections:** the remainder of the header gives element counts and byte offsets from the start of the file. Every present section must start at a multiple of 4, at or after the end of the header, lie wholly within `size`, and not overlap any other section. Writers emit sections in header order, padded with zero bytes to 4-byte boundaries, ending at the last section. Readers accept any valid non-overlapping layout within `size`, including gaps between sections or unused bytes after the last section when included in `size`.
- **Strict validation:** every reserved field and undefined flag bit must be `0`, and unknown versions are refused. Texture loaders validate the header, sections and 4-bit texel values before placing the texture in VRAM; the mesh loader validates the header, section bounds and texture-slot sizes, allocates the `jb_mesh` block from the context's allocator, and then validates the decoded records (freeing the block on any failure). Invalid input returns `JB_ERROR_INVALID_ARGUMENT` with a detail naming the failing field; file-read or mesh allocation failures, and VRAM placement or texture-table limits, return `JB_ERROR_OUT_OF_MEMORY`.

## `.jbt` — texture

### Header (32 bytes)

| Offset | Type | Field | Description |
|---|---|---|---|
| 0 | `u8[4]` | `magic` | `"JBTX"` (`4A 42 54 58`) |
| 4 | `u32` | `version` | `1` |
| 8 | `u32` | `size` | Total file size in bytes |
| 12 | `u16` | `width` | Width in texels, `1..256` |
| 14 | `u16` | `height` | Height in texels, `1..256` |
| 16 | `u8` | `depth` | `4`, `8` or `15` (`JB_TEX_4BIT`, `JB_TEX_8BIT`, `JB_TEX_15BIT`) |
| 17 | `u8` | `flags` | Bit 0: repeat (`jb_texture_desc.repeat`); when set, `width` and `height` must be powers of two in `8..256`. Bits 1–7 must be `0`. |
| 18 | `u16` | `palettes` | `0` at 15 bits; `1..65535` at 4 and 8 bits |
| 20 | `u32` | `texels` | Byte offset of the texels section |
| 24 | `u32` | `palette` | Byte offset of the palettes section; `0` at 15 bits |
| 28 | `u32` | `reserved` | `0` |

### Sections

- **Texels:** `width * height` texels, rows top to bottom with no row padding, matching `jb_texture_desc.texels`.
  - At 4 and 8 bits: one `u8` per texel (palette index: `0..15` at 4 bits, `0..255` at 8 bits). 4-bit texels are stored unpacked (one per byte) so the file bytes can be passed straight to `jb_texture_desc`; the library packs four per VRAM pixel on upload.
  - At 15 bits: one `u16` per texel (15-bit VRAM colour).
- **Palettes:** `palettes * entries` `u16` colours (`entries` is `16` at 4 bits and `256` at 8 bits), stored sequentially. The first palette is loaded with the texture; additional palettes are animation frames for `jb_palette_write` (accessed via `jb_texture_parse`), recolouring the same texels.

VRAM colours use the console's 15-bit format: red in bits 0–4, green in bits 5–9, blue in bits 10–14, and the semi-transparency bit in bit 15. `0x0000` is transparent (never drawn); opaque black is `0x8000` (see `Textures` in [include/jitterbug.h](../include/jitterbug.h)).

## `.jbm` — mesh

### Header (64 bytes)

| Offset | Type | Field | Description |
|---|---|---|---|
| 0 | `u8[4]` | `magic` | `"JBMS"` (`4A 42 4D 53`) |
| 4 | `u32` | `version` | `1` |
| 8 | `u32` | `size` | Total file size in bytes |
| 12 | `u32` | `flags` | Bit 0: normals; bit 1: vertex colours; bit 2: texel coordinates. Bits 3–31 must be `0`. |
| 16 | `u32` | `vertices` | Vertex count, `1..65536` |
| 20 | `u32` | `faces` | Face count, `0..1048576` |
| 24 | `u32` | `materials` | Material count, `0..65536`; at least `1` when `faces > 0` |
| 28 | `u32` | `slots` | Texture slot count, `0..65535` |
| 32 | `u32` | `positions` | Byte offset of positions |
| 36 | `u32` | `normals` | Byte offset of normals; `0` without flag bit 0 |
| 40 | `u32` | `colours` | Byte offset of vertex colours; `0` without flag bit 1 |
| 44 | `u32` | `uv` | Byte offset of texel coordinates; `0` without flag bit 2 |
| 48 | `u32` | `faces` | Byte offset of faces; `0` when `faces == 0` |
| 52 | `u32` | `materials` | Byte offset of materials; `0` when `materials == 0` |
| 56 | `u32` | `slots` | Byte offset of texture slots; `0` when `slots == 0` |
| 60 | `u32` | `reserved` | `0` |

### Sections

| Section | Count | Record bytes | Layout |
|---|---|---|---|
| `positions` | `vertices` | 6 | `i16 x, y, z` in model space (`jb_mesh.positions`) |
| `normals` | `vertices` | 6 | `i16 x, y, z` in 4.12 fixed point (`4096` is `1.0`), each `-4096..4096` |
| `colours` | `vertices` | 4 | `u32` `JB_RGB` (`0..0xFFFFFF`), used in place of the material colour |
| `uv` | `vertices` | 2 | `u8 u, v` texel coordinates within the face's texture |
| `faces` | `faces` | 12 | `u16 v[4]`, `u16 vertices` (`3` or `4`), `u16 material` (`jb_mesh_face`); indices `< vertices`, `material < materials`, `v[3] == 0` for triangles |
| `materials` | `materials` | 8 | `u32 colour`, `u8 blend`, `u8 flags`, `u16 slot` |
| `slots` | `slots` | 4 | `u16 width, height` (`1..256` each) |

Face vertices go anticlockwise around the face when viewed from the front, matching `jb_mesh_face` (quads are reordered into the GPU's Z order when drawn).

Material fields:
- `colour`: `JB_RGB` (`0..0xFFFFFF`, where `0` means `JB_NEUTRAL`), or `JB_TINT_BLACK` (`0xFF000000`).
- `blend`: a `JB_BLEND_*` mode (`0..4`).
- `flags`: bit 0 unlit, bit 1 raw, bit 2 double-sided; bits 3–7 must be `0`.
- `slot`: texture slot index (`< slots`), or `0xFFFF` for an untextured material.

### Texture slots

A `.jbm` file does not embed textures. Instead, textured materials refer to numbered texture slots, and the caller supplies one `jb_texture` per slot in order when calling `jb_mesh_load` or `jb_mesh_load_mem`. Each slot stores the `width` and `height` the converter mapped the mesh's `u, v` coordinates to, and the loader checks that the bound texture has that exact size. Texel coordinates lie within the texture's bounds, or anywhere in `0..255` for a repeat texture (`.jbt` flag bit 0), which wraps under the texture window.

## Limits

- **Texture dimensions (`1..256`):** one hardware texture page is 256x256 texels (`jb_texture_desc`).
- **Vertex count (`1..65536`):** matches 16-bit vertex indices in `jb_mesh_face`.
- **Face count (`0..1048576`):** keeps all counts and byte offsets safely inside 32-bit unsigned and signed `int` ranges.
- **File size cap (`64 MB`):** `jb_texture_load` and `jb_mesh_load` refuse files larger than 64 MB (`JBI_FILE_MAX`), well above the maximum header-plus-section payload of a `.jbt` (~33 MB) or `.jbm` (~14 MB) (files with padding or trailing bytes inside `size` may be larger up to the 64 MB cap).

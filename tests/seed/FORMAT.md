# Seed trace format

The files in this directory carry 28 recorded scenes from the GPU test suite of the software GPU that jitterbug's core was seeded from (an earlier private project). Each trace records every call that crossed into the GPU in order, together with what the test read back. A replayer executes the same operations with the same arguments and verifies every expected result. See [docs/testing.md](../../docs/testing.md) for how the traces are measured and re-blessed.

What the traces expect is the seed's output plus the hardware fixes recorded in [CHANGES.md](CHANGES.md). The operations, preludes and standalone flags are as recorded; the VRAM hashes, frame hashes, `STORE` pixels and golden PNGs reflect the re-blessed values.

- `NN-name.jbt`: the trace for scene `NN` (`00` through `27`).
- `NN-name.1x.png`: the 1x VRAM at the end of scene `NN`; `NN-name.2x.png`: the scaled VRAM for scenes that end at 2x.
- `scaled/`: reference files (`3x.txt`, `4x.txt`, `8x.txt`) and 3x golden PNGs for replaying the sequence with every scale above 1x replaced by 3, 4 or 8.

The readers in this repository are `tests/trace.h`, `tests/trace.c`, `tests/seed_replay.c` (against the internals) and `tests/api_golden.c` (against the public API); `tools/seed-record/seedcheck.c` is an independent C99 reader written from this specification alone.

## Conventions

All integers are little-endian. `u32` and `i32` are 4 bytes; `u64` is 8 bytes. An array `u16[n]` stores `n` 2-byte little-endian values followed by 2 zero bytes whenever `n` is odd, so every field starts on a 4-byte boundary.

### Hash

All hashes use 64-bit FNV-1a: start at `h = 0xcbf29ce484222325`, and for each byte `b` compute `h = ((h ^ b) * 0x100000001b3) mod 2^64`.

A VRAM hash runs over every pixel in row-major order, each 16-bit pixel hashed low byte first:

- 1x VRAM: `1024 x 512` pixels (`524,288` pixels, `1,048,576` bytes).
- Scaled VRAM at scale `N > 1`: `1024N x 512N` pixels. When scale is 1 (no scaled VRAM), the scaled hash field is `0`.

A `PRESENT` frame hash runs over the raw frame bytes as specified under [PRESENT (0x30)](#present-0x30).

## File layout

### Header

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | `char[8]` | `magic` | Exact bytes `"JBSEEDT\0"` |
| 8 | `u32` | `version` | `1` |
| 12 | `u32` | `header_bytes` | Byte offset of the first op (must equal the byte offset immediately after the prelude) |
| 16 | `u32` | `scene` | Scene index `NN` (`0`..`27`) |
| 20 | `u32` | `name_len` | Byte length `L` of the scene name (`L < 64` in `trace.c`) |
| 24 | `char[L]` | `name` | Scene name bytes, followed by `(4 - L % 4) % 4` zero padding bytes to a 4-byte boundary |

### Prelude

Immediately after the padded name comes the prelude describing the GPU and fake precise-vertex table state at the start of the scene:

| Type | Field | Meaning |
|---|---|---|
| `i32` | `scale` | `scale` argument of the last `CONFIGURE` (`1` if none) |
| `i32` | `dither` | `dither` argument of the last `CONFIGURE` (`-1` if none) |
| `i32` | `threads` | `threads` argument of the last `CONFIGURE` (`1` if none) |
| `i32` | `exact_1x` | Last `SET_EXACT_1X` value, `0` or `1` (`0` if none) |
| `i32` | `texture_mapping` | Last `SET_TEXTURE_MAPPING` value, `0` (affine) or `1` (perspective) |
| `i32` | `persp_span` | Perspective span length above 1x (always `8` in these traces) |
| `i32` | `persp_span_1x` | Perspective span length at 1x (always `4` in these traces) |
| `i32` | `precise_enabled` | Fake precise table's `enabled` flag (`0` or `1`) |
| `u32` | `precise_lookups` | Fake precise table's lookup counter |
| `u32` | `precise_hits` | Fake precise table's hit counter |
| `u32` | `precise_count` | Number of entries `K` in the fake precise table, followed by `K` entries of (`u32 sxy`, `i32 x`, `i32 y`, `i32 z`) in table order (16 bytes per entry) |
| `u32` | `gpu_scale` | Active GPU scale at scene start (checked in sequence replay) |
| `u64` | `start_hash_1x` | FNV-1a hash of 1x VRAM at scene start (checked in sequence replay) |
| `u64` | `start_hash_scaled` | FNV-1a hash of scaled VRAM at scene start, or `0` if scale is 1 |

Scene `00` begins from a fresh state: VRAM all zero, defaults as listed above (`CONFIGURE(1, -1, 1)`), and an empty precise table.

### Operation stream

Starting at `header_bytes`, the file consists of operations until an `END` op that ends the file with no trailing bytes:

| Type | Field | Notes |
|---|---|---|
| `u32` | `code` | Operation code |
| `u32` | `length` | Payload length in bytes (must be a multiple of 4) |
| `uint8_t[length]` | `payload` | Op-specific fields; readers must consume all `length` bytes |

A reader can skip any op using `length`. A replayer must carry out every version 1 op except `SYNC`, `TRACE_CALL` and `TRACE_TIME` (see notes below).

## Operations

Unless noted, each op corresponds to one call on the main thread. Fields marked *expected* are verified by the replayer against what the implementation returns or produces.

| Code | Op | Payload | Action in jitterbug (`tests/seed_replay.c`) |
|---|---|---|---|
| `0x00` | `END` | none (`length = 0`, must end file) | End of trace |
| `0x01` | `GP0` | `u32 count`, `i32 expected_ret`, `u32[count] words` | `jbi_gp0(ctx, words, count)`; return must equal `expected_ret` |
| `0x02` | `FILL` | `i32 x, y, w, h`, `u32 rgb24` | `jbi_fill(ctx, x, y, w, h, rgb24)` |
| `0x03` | `LOAD` | `i32 x, y, w, h`, `u32 n`, `u16[n] pixels` | `jbi_load(ctx, x, y, w, h, pixels)` |
| `0x04` | `STORE` | `i32 x, y, w, h`, `u32 n`, `u16[n] expected` | `jbi_store(ctx, x, y, w, h, out)`; `out` must equal `expected` |
| `0x05` | `COPY` | `i32 sx, sy, dx, dy, w, h` | `jbi_copy(ctx, sx, sy, dx, dy, w, h)` |
| `0x06` | `FLUSH` | none | `jbi_flush(ctx)` |
| `0x07` | `SYNC` | none | No-op (the seed's draw-only passes are not present in jitterbug) |
| `0x08` | `RESET_STATE` | none | `jbi_reset_state(ctx)` |
| `0x09` | `HIRES_REPLICATE` | none | `jbi_soft_replicate(soft)` |
| `0x0a` | `LOWRES_MARK` | `i32 x, y, w, h` | `jbi_soft_lowres_mark(soft, x, y, w, h)` |
| `0x0b` | `LOWRES_RESOLVE` | `i32 x, y, w, h` | `jbi_soft_lowres_resolve(soft, x, y, w, h)` |
| `0x0c` | `TRACE_CALL` | none | No-op (timing trace is not present) |
| `0x0d` | `TRACE_TIME` | `i32 phase` | No-op (timing trace is not present) |
| `0x10` | `CONFIGURE` | `i32 scale, dither, threads, expected_ret, expected_threads` | `jbi_configure(ctx, scale, dither, threads)`; return must equal `expected_ret` |
| `0x11` | `SET_EXACT_1X` | `i32 exact` | `jbi_set_exact_1x(ctx, exact)` |
| `0x12` | `SET_TEXTURE_MAPPING` | `i32 perspective` | `jbi_set_texture_mapping(ctx, perspective)` |
| `0x13` | `VRAM_GET` | none | `jbi_soft_vram(soft)` (flushes and resolves stale 1x tiles; followed by a `CAPTURE`) |
| `0x20` | `PRECISE_ENABLE` | `i32 on` | Fake table `enable(on)` |
| `0x21` | `PRECISE_BEGIN_FRAME` | none | Fake table `begin_frame()` |
| `0x22` | `PRECISE_RECORD` | `u32 sxy`, `i32 x, y, z` | Fake table `record(sxy, {x, y, z})` |
| `0x23` | `PRECISE_STATS` | `i32 expected_lookups, expected_hits` | Read and zero fake table counters; both must match |
| `0x30` | `PRESENT` | see [PRESENT (0x30)](#present-0x30) | Build display frame and compare dimensions, stride, format and hash |
| `0x31` | `CAPTURE` | see [CAPTURE (0x31)](#capture-0x31) | Compare 1x and scaled VRAM hashes |
| `0x32` | `POKE` | see [POKE (0x32)](#poke-0x32) | Overwrite a VRAM plane directly |

Notes:

- In `LOAD` and `STORE`, `n` is the wrapped transfer pixel count `(((w - 1) & 0x3ff) + 1) * (((h - 1) & 0x1ff) + 1)`.
- `GP0` words are the recorded command word array submitted in a single `jbi_gp0` call (without ordering-table tag words), which is not limited to one packet (for example, op 9 of `04-texture_window.jbt` submits two one-word commands, `E1h` and `E2h`, together).
- `expected_threads` in `CONFIGURE` records the worker pool size the original recording host created (capped by CPU count); rendered pixels do not depend on thread count.

### CAPTURE (0x31)

Payload (36 bytes): `u32 kind`, `u32 standalone`, `u32 gpu_scale`, `u32 scaled_width`, `u32 scaled_height`, `u64 hash_1x`, `u64 hash_scaled`.

Records a point where the test read VRAM. Hashes are computed directly over the raw 1x and scaled VRAM arrays immediately after the preceding operation (no extra flush or resolve is performed for the capture itself). When scale is 1, `scaled_width`, `scaled_height` and `hash_scaled` are `0`.

| `kind` | Taken after |
|---|---|
| `1` | `VRAM_GET` |
| `2` | A scaled VRAM read (preceded by a `FLUSH` op) |
| `3` | `STORE` |
| `4` | A vertical blank (`SYNC`, `FLUSH`, ..., `PRESENT`, `TRACE_TIME 2`) |
| `5` | End of scene (last op before `END`) |

`standalone` is `1` when the capture produces the same VRAM whether the scene runs alone or in sequence, and `0` when it reflects VRAM left behind by an earlier scene (typically a read before the scene's first clear).

### PRESENT (0x30)

Payload (56 bytes): `i32 display_on`, `i32 env_valid`, `i32 disp_x`, `i32 disp_y`, `i32 disp_w`, `i32 disp_h`, `i32 rgb24`, `i32 scale`, `i32 expected_width`, `i32 expected_height`, `i32 expected_stride`, `i32 expected_format`, `u64 expected_frame_hash`.

Represents display scanout at a vertical blank. `display_on` is the display enable flag; `env_valid` indicates whether a display environment was set (`disp_*` and `rgb24` are zero when `0`); `scale` is the active GPU scale. The frame is constructed from VRAM as it stands without further GPU calls:

```text
w = 512; h = 256; rgb = 0; n = scale
if env_valid != 0 and disp_w > 0 and disp_h > 0:
    w = disp_w; h = disp_h; rgb = (rgb24 != 0)
h = min(h, 512)
w = min(w, 682 if rgb else 1024)
stride = (3 * w if rgb else 2 * w) * n          # bytes per row, no padding
width = w * n; height = h * n
format = 1 if rgb else 0                        # 0 = BGR555, 1 = RGB24
x = disp_x & 1023; y = disp_y

if display_on == 0 or env_valid == 0:
    fill all (height * stride) bytes with 0
elif n > 1 and not rgb:                         # from scaled VRAM
    for r in 0 .. h * n - 1:
        src = scaled row ((y + r / n) & 511) * n + (r % n)
        for i in 0 .. w * n - 1:
            frame row r, pixel i = src[(x * n + i) mod (1024 * n)]
else:                                           # from 1x VRAM
    for r in 0 .. h - 1:
        src = 1x row (y + r) & 511              # 2048 bytes, each pixel low byte first
        if rgb:
            for i in 0 .. 3 * w - 1, k in 0 .. n - 1:
                frame row r * n, byte (i / 3 * n + k) * 3 + (i % 3) = src_bytes[(2 * x + i) & 2047]
        else:
            for i in 0 .. w - 1:
                frame row r * n, pixel i = src[(x + i) & 1023]
        copy frame row r * n into rows r * n + 1 .. r * n + n - 1
```

Integer division `/` truncates towards zero for these non-negative values. A 15-bit pixel is stored as two little-endian bytes (mask bit included); a 24-bit frame stores `R, G, B` bytes. `expected_width`, `expected_height`, `expected_stride`, `expected_format` and the FNV-1a hash of the `height * stride` bytes must all match.

`jbi_soft_scanout` in `src/soft/scanout.c` implements this exact algorithm (with `display_on && env_valid` as `enabled`, and `0 x 0` display size when `env_valid == 0`). `tests/seed_replay.c` verifies both this algorithm and `jbi_scanout` agree.

### POKE (0x32)

Payload: `u32 plane`, `u32 width`, `u32 height`, `u32 runs`, followed by `runs` pairs of (`u32 count`, `u32 value`).

Overwrites a VRAM plane directly (used where the original test cleared VRAM through a raw pointer): `plane == 0` is 1x VRAM (`1024 x 512`), `plane == 1` is scaled VRAM (`width x height`). The `count` fields must sum to `width * height`, and each `value` (low 16 bits) is repeated `count` times in row-major order. No `plane == 1` `POKE` occurs in the 28 traces.

## Fake precise-vertex table

During replay, `tests/seed_replay.c` supplies a fake precise-vertex lookup table matching the original test harness:

- State: `enabled` (`0`), `count` (`0`), up to 2048 entries of `(u32 key, int x, int y, int z)`, and `u32` counters `lookups` and `hits`.
- `enable(on)`: sets `enabled = (on != 0)`.
- `is_enabled()`: returns `enabled`.
- `begin_frame()`: sets `count = 0`.
- `record(sxy, v)`: if `!enabled`, does nothing. Otherwise, if an entry already has exact 32-bit key `sxy`, overwrites its vertex with `*v`; else if `count < 2048`, appends `(sxy, *v)`.
- `lookup(sxy, v)`: increments `lookups`; searches entries in order for exact 32-bit key `sxy`. On match, increments `hits`, writes `*v`, returns `1`; else returns `0`. Does not check `enabled`.
- `lookup_poly(sxy, n, v)`: calls `lookup(sxy[i], &v[i])` for `i = 0 .. n - 1` and sets bit `i` of the returned mask on each hit.
- `PRECISE_STATS`: checks `lookups` and `hits` against expected values, then zeroes both counters.

Unlike `src/geom/precise.c`, keys are compared across all 32 bits without 11-bit masking, with one entry per key and no depth matching.

## Running a scene alone

Scenes leave state behind in VRAM, drawing state, configuration and the fake table. To replay scene `NN` alone, start from a fresh context (all VRAM zero) and apply the prelude in order:

```text
jbi_configure(ctx, scale, dither, threads)
jbi_reset_state(ctx)
jbi_set_exact_1x(ctx, exact_1x)
jbi_set_texture_mapping(ctx, texture_mapping)
fake table: enabled = 1, begin_frame(), record each prelude entry in order,
            enabled = precise_enabled, lookups = precise_lookups, hits = precise_hits
```

Then replay the scene's ops, checking only `CAPTURE` ops with `standalone == 1`. All other expectations (`STORE`, `PRESENT`, `PRECISE_STATS`, `GP0` and `CONFIGURE` return values) hold in standalone mode; `start_hash_*` are skipped. Calling `jbi_reset_state` matters because a reset sets the texture-window AND masks to `0xff` rather than `0`, matching the state from which the `standalone` flags were computed.

When replaying all 28 scenes in order (`00` through `27`) in one process, compare everything, including `start_hash_*` and `standalone == 0` captures.

## Golden images

`NN-name.1x.png` holds 1x VRAM (`1024 x 512`) at the end of scene `NN` (`CAPTURE` kind `5`); `NN-name.2x.png` holds scaled VRAM (`2048 x 1024`) when the scene ends at 2x. Each golden PNG is checked pixel for pixel in addition to the hashes.

Encoding is uncropped 8-bit RGBA:

```text
R = (p & 31) << 3
G = ((p >> 5) & 31) << 3
B = ((p >> 10) & 31) << 3
A = 128 if (p & 0x8000) else 255
```

Because the low 3 bits of `R`, `G`, `B` are zero, every 16-bit VRAM pixel is recovered losslessly:

```text
p = (R >> 3) | ((G >> 3) << 5) | ((B >> 3) << 10) | (0x8000 if A == 128 else 0)
```

## Other scales (`scaled/`)

The traces themselves configure only 1x and 2x. To test scales `S = 3, 4, 8`, a replayer runs the full 28-scene sequence in order (standalone single-scene replay at another scale is refused) with every scale above `1` replaced by `S` in `CONFIGURE` (both argument and expected return), in preludes (`scale` and `gpu_scale`) and in `PRESENT`. Scales of `1` remain `1` (scenes `00`–`10` and `21`–`23` run as recorded).

Expectations that change at scale `S` are listed in `scaled/Sx.txt`, a line-based text file where `#` starts a comment line. Scenes are named `NN-name`, ops are zero-based indices after the prelude (`-1` for prelude/scene-end entries), hashes are 16 lowercase hex digits, and `=` for a 1x hash means the trace's recorded 1x hash still holds:

| Line | Replaces |
|---|---|
| `start NN-name -1 S H1 HS` | Prelude `gpu_scale`, `start_hash_1x` and `start_hash_scaled` |
| `capture NN-name OP S H1 HS` | `CAPTURE` `OP`'s `gpu_scale`, `hash_1x` and `hash_scaled` (scaled size `1024S x 512S`, or `0 x 0` when `S == 1`) |
| `store NN-name OP H` | `STORE` `OP`'s expected pixels (FNV-1a pixel hash `H`) |
| `present NN-name OP S W H STRIDE FORMAT HASH` | `PRESENT` `OP`'s scale, dimensions, stride, format and frame hash |
| `golden NN-name -1 S` | Checks `scaled/NN-name.Sx.png` against scaled VRAM at scene end |

Any expectation without a line holds as recorded, and every line in `Sx.txt` must be reached during replay. At scene end, the recorded 1x golden PNG is still checked whenever the scene-end 1x hash matches the trace's; the recorded 2x PNGs do not apply. Golden PNGs exist at 3x for scenes `12`, `13`, `15`, `17` and `24`; 4x and 8x check hashes only.

The recorded 1x VRAM holds identically at all scales `S` except in scene `11` (`11-hires_skip_1x`) at ops `871`, `875` and `1308`, and at scene `12`'s start and op `2` before its first clear. Scene `11` is the only scene that draws with `exact_1x` off, so reads at ops `870` (`VRAM_GET`) and `1307` (`SET_EXACT_1X(1)`) resolve stale 1x tiles by averaging each `S x S` block (`jbi_soft_hires_average` in `src/soft/soft.c`), which depends on `S`. With `exact_1x` on, primitives are also rasterised directly into 1x VRAM by the 1x rasteriser regardless of scale. No `STORE`, precise counter or `GP0` return differs at 3x, 4x or 8x.

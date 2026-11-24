# Conformance

This page states what hardware exactness means in jitterbug, how disagreements are settled, what the test suite currently verifies against real PlayStation hardware, every rule fitted from console dumps, the assumptions that extend beyond the dumps, and the open questions waiting for our own hardware probe to run on a console.

Hardware rules cited below come from the public documentation [psx-spx](https://psx-spx.consoledev.net/graphicsprocessingunitgpu/). Where psx-spx is silent, the evidence is the real-console dumps alone. No copyleft or non-commercial emulator source code was read or copied.

## What "exact" means

- **At 1x (the target, and what is evidenced):** the design target at 1x (with non-console options off) is that a given GP0/GP1 command stream produces the same 16-bit VRAM (including the mask bit) and the same GTE registers and `FLAG` as a real PlayStation. What the test suite actually verifies against real console hardware today is narrower: the 13 named [ps1-tests](https://github.com/JaCzekanski/ps1-tests) GPU programs — 11 VRAM dumps compared pixel for pixel on the 15 colour bits those PNG dumps carry (excluding 64 uninitialised-colour pixels in `lines`), plus 2 console logs (`mask-bit`, 5 checks covering the mask bit; `gp0-e1`, 10 checks) — and the 1150 distinct GTE cases in `gte/test-all`. jitterbug's own hardware probe (which checks all 16 VRAM bits, mask bit included) has not yet been run on a physical console ([docs/probe.md](probe.md)).
- **Above 1x (2x–8x):** no PlayStation hardware renders above 1x, so scaled output cannot be called exact. It is *faithful*: the same coverage, gradient and colour rules applied at finer resolution (`src/soft/raster_hires.c`), while the 1x VRAM kept beside it remains checked against the console dumps on the same terms as at 1x.
- **The CLUT cache is in scope.** The GPU retains the last palette loaded and only reloads from VRAM when a primitive names a different CLUT, needs more entries, or follows a cache clear (`GP0(01h)`, `GP1(01h)`, `GP1(00h)`). It is visible in VRAM, affects palette animation, and is tested by ps1-tests' `clut-cache` dump.
- **Out of scope:** GPU timing and memory bandwidth (including `GPUSTAT` busy/ready bits), the texture cache (which affects speed rather than pixels), display interlacing, and differences between GPU silicon revisions. jitterbug follows the behaviour recorded in ps1-tests' dumps (two of whose results — `gp0-e1` and `texture-flip` — correspond to a v2 GPU as psx-spx describes it; see [third_party/ps1-tests/README.md](../third_party/ps1-tests/README.md)).

## Order of authority

When two sources disagree, they are ranked strictly in this order — never by majority vote:

1. **Real console hardware:**
   - The VRAM dumps, logs and GTE cases published by [ps1-tests](https://github.com/JaCzekanski/ps1-tests).
   - jitterbug's own freestanding MIPS hardware probe ([docs/probe.md](probe.md)), which captures all 16 bits of VRAM (including the mask bit) over serial. The probe is built and verified in host simulation and in PCSX-Redux, **but has not yet been run on a physical console**.
2. **psx-spx:** public PlayStation hardware documentation.
3. **Emulators as proxies:** [psxstack](https://github.com/gascarcella/psxstack)'s MIT-licensed software GPU (used in differential fuzzing, `tools/fuzz-diff`), and output screenshots or VRAM dumps from Mednafen or DuckStation (output only, never source code). Proxies find candidate bugs quickly; any discrepancy is decided by (1) or (2).

## Results on ps1-tests

### GPU programs (13 programs)

Thirteen ps1-tests GPU programs are recorded into `.jbc` command lists (`tools/conformance-record/programs.txt`, `tests/conformance/fixtures.txt`, format in [tests/conformance/FORMAT.md](../tests/conformance/FORMAT.md)), each from the upstream ps1-tests commit that produced its console dump or log (for `mask-bit`, the immediately following commit, verified by the recorder to emit identical GPU words).

Eleven programs are compared pixel for pixel against the console's 1024x512 `vram.png` dump on the 15 colour bits (the PNG dumps store each 5-bit channel shifted left by 3 and carry no mask bit). Two programs (`mask-bit` and `gp0-e1`) are compared against the expected values in the console's `psx.log`. Every program is played on a 1x context (`conformance_NAME`) and also on 2x and 3x contexts requesting 4 and 3 threads (`conformance_NAME_2x`, `conformance_NAME_3x`; single-threaded builds such as `wasm` clamp both thread requests to 1), whose 1x VRAM (`jb_vram_read`) must match the same reference. Each list is compared after the main-loop pass named in `tests/conformance/fixtures.txt`, and the following two passes must leave VRAM unchanged.

Ten programs' lists and references are checked into the repository. Three (`rectangles`, `texture-overflow`, `vram-to-vram-overlap`) embed third-party images (a photograph without an established redistribution licence, and PSn00bSDK's MPL-2.0 debug font), so they are not checked in; `nix build .#conformance-lists` fetches and records them for local testing (`-DJB_CONFORMANCE_FETCHED=...`), and every renderer test build in `nix flake check` tests all 13 ([third_party/ps1-tests/README.md](../third_party/ps1-tests/README.md)).

| Program | Reference | Pass | 1x | 1x VRAM at 2x & 3x | Fixes applied |
|---|---|---|---|---|---|
| `triangle` | checked in (dump) | 1 | matches | matches | colour interpolation |
| `quad` | checked in (dump) | 1 | matches | matches | — |
| `lines` | checked in (dump) | 1 | matches (64 px excluded) | matches (same 64 excluded) | flat-line dithering, line ties, colour stepping |
| `rectangles` | fetched (dump) | 6 | matches | matches | — |
| `transparency` | checked in (dump) | 1 | matches | matches | — |
| `uv-interpolation` | checked in (dump) | 1 | matches | matches | colour and texture-coordinate interpolation |
| `clipping` | checked in (dump) | 1 | matches | matches | — |
| `texture-flip` | checked in (dump) | 1 | matches | matches | x-flipped rectangle start texel |
| `texture-overflow` | fetched (dump) | 1 | matches | matches | — |
| `clut-cache` | checked in (dump) | 1 | matches | matches | CLUT cache |
| `vram-to-vram-overlap` | fetched (dump) | 1 | matches | matches | — |
| `mask-bit` | checked in (log) | 1 | matches (5 / 5 checks) | matches | — |
| `gp0-e1` | checked in (log) | 1 | matches (10 / 10 checks) | matches | `GP1(09h)`, `GPUSTAT` bit 15 |

`tests/conformance/expected-failures.txt` is empty. Prior to the fixes listed above, six programs differed: `triangle` (26,162 pixels), `lines` (1,012), `uv-interpolation` (23,616), `texture-flip` (134,144), `clut-cache` (921) and `gp0-e1` (4 of 10 checks).

### Excluded pixels (`lines`)

Only one exclusion exists anywhere in the suite: `tests/conformance/lines.exclude`, bound to `lines` by pixel count (`64`) and checksum in `tests/conformance/fixtures.txt` (the runner refuses any other exclusion file or command-line override; see [docs/testing.md](testing.md)).

The 64 excluded pixels are the final segment of each of the two Gouraud polylines in `lines` (32 pixels each, `(181, 171)` to `(150, 140)` and `(241, 171)` to `(210, 140)`). The upstream test program calls `setRGB2` twice and never `setRGB3`, leaving the fourth vertex colour uninitialised: on the console it read whatever happened to be on the stack (fading towards roughly `(200, 0, 100)`), whereas the deterministic recorder sends black. The first pixel of each segment uses the initialised third vertex colour and is compared. The runner prints that 64 pixels were excluded and that all 64 differ.

### GTE (1150 cases)

`tests/gte/conformance.c` (`gte_test_all`) replays all 1150 distinct console cases from ps1-tests' `gte/test-all` (50 cases for each of the 22 documented GTE commands plus 50 register-only cases with no command) through the public `jb_gte_*` API, checking every data register, control register and `FLAG` bit with non-console options off and again with `precise_vertices` and `perspective_correct` on. (ps1-tests' `gte-fuzz` log holds the same 1150 cases in a different order, verified by `tools/gte-cases/convert.py`, so the evidence is 1150 cases rather than 2300.) All 1150 pass.

As noted in [third_party/ps1-tests/README.md](../third_party/ps1-tests/README.md), because the upstream inputs are random, 65 of the 100 `RTPS`/`RTPT` cases overflow the divide (`FLAG` bit 17); only two `RTPT` cases are sensitive to dropping the divide's rounding term (`+0x8000`) or shifting a reciprocal table entry by 1, and no case catches changing the rounding term by 1.

## Rules fitted from the console results

### `gp0-e1`: `GP1(09h)` and `GPUSTAT` bit 15

All 10 console log checks match. When `GP1(09h)` bit 0 is set, bit 11 of `GP0(E1h)` (or of a textured polygon's texture-page word) is stored and reported in `GPUSTAT` bit 15; when `GP1(09h)` bit 0 is clear, those writes store `0` into bit 15; clearing `GP1(09h)` preserves the stored bit 15 until the next `GP0(E1h)` or textured-polygon write, and `GP1(00h)` reset does not clear `GP1(09h)` (`src/core/gp0.c`, `src/api/display.c`). psx-spx notes that setting both bits disables textures on a retail console because the second VRAM chip select is unwired, but no dump shows what is drawn in that state, so jitterbug stores and reports the bit without altering drawing.

### `texture-flip`: x-flipped rectangle start texel

With x-flip enabled (`GP0(E1h)` bit 12) and `u0 = 0`, the console dump samples texel `(1 - x) & 255` across all three x-flipped rectangles (134,144 pixels), while y-flip samples `(v0 - y) & 255`. psx-spx states only that flip bits decrement coordinates. jitterbug implements the simplest rule fitting the dump: an x-flipped rectangle starts at `(u0 + 1)` (`src/core/gp0.c`, `draw_rect`).

### `lines`: dithering, ties and Gouraud colour stepping

Of the 1,012 initial mismatches in `lines`, 948 were renderer differences (now 0) and 64 are the uninitialised-colour exclusion above:

- **Flat lines are dithered (624 pixels):** psx-spx notes that when dithering is enabled, both monochrome and shaded lines are dithered, and the dump confirms this. `src/soft/raster.c` and `src/soft/raster_hires.c` dither all lines.
- **Line stepping ties (68 pixels):** at major step `i` of `n`, the minor coordinate is `start + i * d / n` rounded to the nearest integer. At exact halves, the dump has 34 pixels in x-major lines going right and down (all taking the larger `y`) and 25 pixels in y-major lines going down and right (all taking the smaller `x`). jitterbug rounds `y` halves up and `x` halves down in every direction, so a line draws the same pixels from either endpoint.
- **Gouraud line colours (256 pixels):** with `s = trunc((c1 - c0) * 4096 / n)` (truncated toward zero), the colour at step `i` is the integer part of `c0 + 1/2 + i * s / 4096` (`raster_line`). This matches all 10,631 compared line pixels.

### `clut-cache`: hardware CLUT caching

All 921 mismatches across five rows are resolved. Matching psx-spx and the dump row by row:
- A 256x1 8-bit sprite drawn over its own CLUT retains the palette loaded before its first pixel (rows 52 and 68).
- A CLUT overwritten by a fill between two sprites using the same CLUT is not reloaded without `GP0(01h)` (row 88).
- A 4-bit primitive following an 8-bit primitive with the same CLUT reuses the cached entries (row 152).
- An 8-bit primitive following a 4-bit primitive with the same CLUT reloads all entries (row 136).
- Toggling 8-bit and back to 4-bit via `GP0(E1h)` without drawing leaves the cache intact (row 200).

jitterbug models a 4-bit load as 16 entries and an 8-bit load as 256 entries, reloading whenever a primitive names a different CLUT, needs more entries than cached, or follows `GP0(01h)`, `GP1(01h)` or `GP1(00h)`. On submission, the active palette is copied out of 1x VRAM into the backend's palette pool so deferred worker threads draw with the palette in force at command time (`src/soft/soft.c`).

### `triangle` and `uv-interpolation`: polygon attribute interpolation

All 26,162 mismatches in `triangle` and 23,616 in `uv-interpolation` are resolved. Testing candidate integer interpolators against the console dumps isolated one combination that matches every pixel across all dumps:

- Each attribute's `x` and `y` gradients are the plane's derivatives with **12 fractional bits**, quotient **truncated toward zero**.
- The value at a pixel is the **first vertex's** value, plus **`1/2`**, plus the gradients times the offset from that vertex, taking the integer part before dithering and 5-bit truncation.

| Dump subset | Pixels | Fitted rule | 11 bits | 13 bits | Floored | `1/2 - 2^-12` | 2nd vertex origin | Previous rule |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `triangle` (3 Gouraud triangles) | 157,440 | 0 | 220 | 1,102 | 1,637 | 17 | 3,054 | 26,162 |
| `uv-interpolation` (Gouraud quads) | 65,280 | 0 | 103 | 40 | 251 | 118 | 303 | 7,756 |
| `uv-interpolation` (textured quads, `u`) | 32,640 | 0 | 335 | 148 | 0 | 25 | 690 | 15,860 |

The scaled pass (`src/soft/raster_hires.c`) applies the same 12-bit gradients and half-unit bias sampled at each scaled pixel's centre relative to its 1x pixel, clamping out-of-edge samples to the vertices' attribute range (`hires_slivers` and `hires_lines` verify this against exact 128-bit and rational evaluations).

## Assumptions beyond the evidence

Where the console dumps cover only a subset of cases, jitterbug extends the fitted rule consistently and pins its choice in `tests/rules.c` and `tests/clut_cache.c`. Each can be tested on hardware with the probe ([docs/probe.md](probe.md)):

- **x-flip start for `u0 != 0`:** `texture-flip` only tests `u0 = 0` (starting at `1`). jitterbug starts at `u0 + 1` for every `u0`; whether hardware uses `u0 + 1`, `u0 | 1`, or something else is untested.
- **Line ties in six of eight octants:** `lines` has exact halves only going right-and-down. jitterbug applies "y halves up, x halves down" symmetrically regardless of direction; a direction-dependent rule fits the dump equally well.
- **Gouraud line colour precision and negative steps:** 12 or more fractional bits fit `lines` (jitterbug uses 12, matching polygons), and the dump does not distinguish truncation toward zero from flooring on negative colour steps (jitterbug truncates, matching polygons).
- **Quad second-triangle origin:** for a quad's second triangle (`v1, v2, v3`), `uv-interpolation`'s 1-row quads allow either `v1` or `v3` as the interpolation origin; jitterbug uses `v1`.
- **`v` coordinate interpolation:** `uv-interpolation` varies `u` only; jitterbug applies the same rule to `v`.
- **CLUT cache load width:** jitterbug loads 16 entries for 4-bit primitives and 256 for 8-bit primitives. A wider 4-bit load whose reuse is blocked on a 4-to-8-bit transition fits `clut-cache` equally well.
- **CLUT cache side-effects where the dump is silent:** jitterbug counts every decoded 4- or 8-bit primitive (even if completely clipped), leaves the cache untouched across untextured/15-bit primitives, transfers, copies and overwrites (for subsequent primitives), and clears it on `GP1(01h)` and `GP1(00h)`.
- **Uniqueness of fitted rules:** the polygon and line rules are unique among the candidate families searched, not proven unique over all possible hardware implementations.
- **Texture-disable bit (`GP1(09h)` + `GP0(E1h)` bit 11):** stored and reported in `GPUSTAT` bit 15, without blanking textures during drawing.
- **Scaled output above 1x:** no hardware reference exists; only the accompanying 1x VRAM is compared against the console dumps.

## Open questions awaiting hardware

Differential fuzzing against psxstack's GPU (`tools/fuzz-diff`, see [docs/testing.md](testing.md)) identified seven behavioural differences that no ps1-tests dump decides. Each has a dedicated, minimal command list in `tests/conformance/open/` (`tests/conformance/open/fixtures.txt`), ready to run on a real console via the hardware probe ([docs/probe.md](probe.md)).

### Directed by psx-spx (awaiting hardware confirmation)

- **Quad triangle draw order (`quad-order.jbc`):** psx-spx states quads are processed as `(v1, v2, v3)` then `(v2, v3, v4)`. jitterbug draws them in that order; psxstack draws the second first.
- **Raw-textured Gouraud dithering (`raw-gouraud-dither.jbc`):** psx-spx states raw-textured Gouraud polygons ignore vertex colours and that polygons are dithered only when using Gouraud shading or modulation. jitterbug does not dither them; psxstack dithers based on the shading bit.
- **Mode 3 (`B + F/4`) order with modulation (`modulated-semi-quarter.jbc`):** jitterbug modulates first and quarters the result (`B + floor(floor(t * c / 128) / 4)`), following psx-spx's definition of `F`; psxstack quarters the texel before modulation for flat polygons and rectangles.

### Open (neither hardware dumps nor psx-spx decide)

- **Vertex plus drawing offset outside `-1024..1023` (`offset-wrap.jbc`):** jitterbug keeps the signed sum without wrapping to 11 bits; psxstack wraps it modulo 2048.
- **Order of dithering and semi-transparency (`dither-semi.jbc`):** jitterbug dithers to 5 bits before blending; psxstack blends in 8 bits then dithers.
- **Saturation of modulated semi-transparent texels (`modulated-semi-saturate.jbc`):** jitterbug saturates modulated channels to 31 before blending; psxstack blends before saturating.
- **Gouraud-textured colour stepping (`gouraud-texture-pairs.jbc`):** jitterbug interpolates Gouraud colour at every pixel on textured polygons; psxstack steps once per pair of pixels on opaque undithered unchecked Gouraud-textured polygons (matching the emulator it was tuned against). None of the 13 recorded ps1-tests programs draws a Gouraud-textured polygon.

## ps1-tests GPU programs not in scope

Five other GPU programs in ps1-tests have no comparable in-scope hardware result:

- **`gpustat`:** tests `GPUSTAT` bits 25–30 before, during and after CPU and DMA drawing; the busy/ready transitions are timing (out of scope), and its log aggregates them with DMA register programming that the recorder does not model.
- **`bandwidth`:** times transfers and drawing with random colours; no dump.
- **`benchmark`:** interactive pad-controlled benchmark; its `screenshot.png` depends on input and is not a VRAM dump.
- **`animated-triangle`:** rotating GTE triangle changing every frame; no dump or log.
- **`version-detect`:** queries GPU silicon revision via `GP1(10h)` and `GP0(E1h)`; no log, and revision differences are out of scope.

## What is not claimed

Passing all 13 ps1-tests GPU programs and 1150 GTE cases is strong evidence, not proof of bit-exact equivalence to every PlayStation console across all inputs. The upstream console model is unidentified, the PNG dumps do not carry the mask bit, the seven open-question lists and the assumptions listed above have not yet been executed on real hardware, and jitterbug's own hardware probe has not yet run on a console.

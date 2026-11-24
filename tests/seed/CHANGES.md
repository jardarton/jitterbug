# Seed golden change log

The traces and images in `tests/seed/` were originally recorded from the seed software GPU ([docs/testing.md](../../docs/testing.md)). Subsequent deliberate fixes aligned the renderer with real-console dumps from `ps1-tests` ([docs/conformance.md](../../docs/conformance.md)), which outrank the seed. The seed goldens therefore represent **the seed's output plus the fixes logged below**.

Each fix was measured on its own before re-blessing (`tools/seed-record/measure.sh`, then `tools/seed-record/rebless.sh`): the VRAM at all 183 captures of the 28-scene sequence, at the recorded scales (1x and 2x) and with every scale above 1x replaced by 3, was compared pixel for pixel between the builds before and after the fix, and checked against the fix's allowed envelope. Only embedded hashes, `STORE`/`PRESENT` expectations and golden PNGs were rewritten; no op, prelude or standalone flag changed. After re-blessing, every scene is verified alone at its recorded scale and in sequence at recorded scales and at 3x, 4x and 8x (`seed_scale3`, `seed_scale4`, `seed_scale8`; single-scene replay with `--scale` is refused with exit status 2).

In the tables below, "Changed" counts differing pixels in that before/after comparison, "End" is the scene's final capture (its golden PNG), "Most" is the capture with the largest pixel count, and "step" means one 5-bit colour channel step.

## Fixes that changed no seed golden

| Fix | Evidence | Seed pixels changed |
|---|---|---|
| `GP1(09h)` and `GPUSTAT` bit 15 | `gp0-e1` console log | None (no seed trace sends `GP1`) |
| X-flipped textured rectangles start at `u0 + 1` | `texture-flip` console dump | None (no seed scene flips a textured rectangle) |
| Flat lines dithered | `lines` console dump, psx-spx | None of 283 images at 1x/2x or 3x |
| CLUT cache | `clut-cache` console dump, psx-spx | None of 283 images at 1x/2x or 3x |

## Line ties

**Evidence:** `lines` console dump (68 pixels; see [docs/conformance.md](../../docs/conformance.md)). At an exact half, a line's minor `y` rounds to the larger row and its minor `x` rounds to the smaller column; the scaled pass rounds exact halves in the same directions.

| Scene | Captures changed | End, 1x | End, scaled | Most |
|---|---|---|---|---|
| `07-lines` | 2 | 10, `(1, 0)`–`(9, 5)` | — | 10 |
| `08-transfers_and_order` | 1 (start: `07`'s VRAM) | — | — | 10 |
| `10-scene` | 3 | 12, `(6, 199)`–`(443, 241)` | — | 12 |
| `11-hires_skip_1x` | 8 | 75 (167 at 3x) | 75 at 2x, 176 at 3x | 300 at 2x, 1503 at 3x |
| `12-hires_coverage` | 1 (start: `11`'s VRAM) | — | — | 300 at 2x, 1503 at 3x |
| `17-hires_display` | 4 | 12 | 21 at 2x, 54 at 3x | 21 at 2x, 54 at 3x |
| `18-hires_coverage_3t` | 1 (start: `17`'s VRAM) | — | — | 48 at 2x, 108 at 3x |
| `20-hires_threads` | 9 | 43, `(72, 13)`–`(488, 246)` | — | 172 at 2x, 387 at 3x |
| `21-persp_floor_1x` | 1 (start: `20`'s VRAM) | — | — | 43 |

**Envelope:** pixels move only where a line hits an exact half. Verified: every changed 1x pixel is the old or new position of a tie on a drawn line, and every changed scaled pixel is the old or new edge of a scaled line band at an exact half. The remainder is scene `11` (drawn with `exact_1x` off, so 1x pixels come from scaled block averages: every one of its 75 changed 1x pixels at 2x and 167 at 3x corresponds to a changed scaled block), carried into scene `12`'s start.

## Polygon colours and texture coordinates

**Evidence:** `triangle` and `uv-interpolation` console dumps (every pixel of their Gouraud triangles, Gouraud quads and textured quads; see [docs/conformance.md](../../docs/conformance.md)). Gradients carry 12 fractional bits truncated towards zero, starting half a unit above vertex 0; the scaled pass evaluates the same gradients and half-unit bias at the centre of each scaled pixel relative to its 1x pixel.

| Scene | Captures changed | End, 1x | End, scaled | Most |
|---|---|---|---|---|
| `02-gouraud_and_dither` | 1 (not end) | — | — | 514 |
| `10-scene` | 3 | 38,211 | — | 38,211 |
| `11-hires_skip_1x` | 8 | 53,341 (45,236 at 3x) | 165,920 at 2x, 339,012 at 3x | 224,452 at 2x, 505,017 at 3x |
| `12-hires_coverage` | 1 (start) | — | — | 213,364 at 2x, 407,124 at 3x |
| `17-hires_display` | 4 | 38,209 | 126,742 at 2x, 267,662 at 3x | 126,756 at 2x, 267,692 at 3x |
| `18-hires_coverage_3t` | 1 (start) | — | — | 152,836 at 2x, 343,881 at 3x |
| `20-hires_threads` | 9 | 56,234 | — | 224,936 at 2x, 506,106 at 3x |
| `21-persp_floor_1x` | 3 | — | — | 56,234 |
| `22-persp_fallback_1x` | 6 | — | — | 28,329 |
| `24-persp_floor_2x` | 4 | 28,329, `(57, 70)`–`(454, 219)`, at most +1 step | — | 105,738 at 2x, 234,670 at 3x |
| `25-persp_fallback_2x` | 8 | — | — | 105,738 at 2x, 234,670 at 3x |
| `27-persp_threads` | 15 | 77,185 | — | 308,740 at 2x, 694,665 at 3x |

Scenes `21`–`27` change only through their affine triangles (the 1x pass beside a scaled pass is always affine, as are the perspective fallback triangles); the perspective-correct path itself did not change.

**Envelope:** geometric coverage does not change; an untextured Gouraud pixel moves by at most 1 step per channel with its mask bit unchanged; a textured triangle pixel may sample a texel at most 1 away in `u` and/or `v` (taking that texel's colour and bit 15, which may toggle semi-transparent blending), or the same texel modulated by a colour at most 1 step away; where the sampled texel becomes or stops being transparent (`0x0000`), an earlier primitive shows through instead.

**1x pixel-by-pixel audit** (`tools/seed-record/audit/`, [docs/testing.md](../../docs/testing.md)): both the pre-fix 1x triangle interpolator (untruncated 32-bit fractional gradients, start value `2^-20` above vertex 0, restored by `old.patch`) and the fixed interpolator were instrumented to record the last 16 primitive events at every 1x pixel (writes and skipped transparent/masked passes, with primitive index, kind, previous pixel, written value, pre-blend colour, pre-texture/pre-dither 8-bit colour, texel, texel coordinates, blend mode and state flags). Replaying the 28-scene sequence on one thread at the recorded scales and checking every changed 1x pixel across all 183 captures with `compare.c` (recomputing every write and blend from its recorded inputs, failing closed; tested by `seed_audit_compare`) yielded:

| Last writer in both runs | Changed 1x pixels | Checked |
|---|---|---|
| Same textured unblended triangle, different texel | 1,839,864 | Texel coordinate at most 1 away in `u` and `v`, and not equal |
| Same textured unblended triangle, same texel | 99,993 | Modulated colour at most 1 step per channel (interpolated colour only; raw and flat-shaded cannot move) |
| Same textured triangle, opposite texel bit 15 (blended in one run only) | 57,062 | Texel coordinate at most 1 away, blend recomputed; differing backgrounds checked recursively |
| Same untextured Gouraud triangle, unblended | 136,924 | At most 1 step per channel, mask bit preserved |
| Same blended primitive, same colour (132,688 flat triangles, 18,763 textured, 7,317 tiles) | 158,768 | Background change checked recursively |
| Same blended textured triangle, changed colour, same background | 62,844 | Colour within rules, blend recomputed |
| Same blended textured triangle, changed colour and background | 48,411 | Both checked recursively |
| Different primitive (later textured triangle drew in one run, hit transparent texel in the other) | 19,055 | Transparent texel 1 away in `u` and/or `v` (later primitive drew in old run 8,379 times, new run 10,676 times); revealed pixel checked recursively |
| Outside envelope | 0 | — |

All **2,422,921** changed 1x pixels across the 183 captures (including scenes `11` and `12`, whose changed 1x pixels all come from 1x writes; scene `11` alone accounts for 112,230 changed 1x pixels across two captures) fall inside the envelope, together with all 297,819 differing underlying pixels inspected recursively beneath blended writes and writer switches.

Across textured triangle pixels, texel coordinate deltas (new minus old) are:
- Unblended / same bit 15: `(0, 0)` 33,957 (all same texel), `(+1, 0)` 629,899, `(0, +1)` 608,829, `(+1, +1)` 667,172; none negative or larger than 1.
- Opposite bit 15: `(+1, 0)` 14,095, `(0, +1)` 14,255, `(+1, +1)` 28,712.
- Transparent switches (transparent vs drawn): `(+1, 0)` 5,600, `(0, +1)` 5,594, `(+1, +1)` 7,861.

Every texel shift is non-negative, matching the move of the start bias from `2^-20` to `1/2` above vertex 0.

**Audit limits:** the check cannot detect a fill, copy, upload or poke that wrote the exact value a preceding primitive had already left, does not re-fetch texels from VRAM to verify the recorded texel value, does not recompute coordinates or 8-bit colours directly from vertex coordinates (bounding only their movement between runs), and does not cover scaled VRAM (which `old.patch` does not revert and which changes throughout Gouraud and textured triangles by design).

## Gouraud line colours

**Evidence:** `lines` console dump (256 pixels). Colour steps use 12 fractional bits truncated towards zero, starting half a unit up from vertex 0, matching polygons.

| Scene | Captures changed | End, 1x | End, scaled | Most |
|---|---|---|---|---|
| `10-scene` | 3 | 38, at most +1 step | — | 38 |
| `11-hires_skip_1x` | 8 | 224 (173 at 3x) | 844 at 2x, 1,444 at 3x | 896 at 2x, 1,656 at 3x |
| `12-hires_coverage` | 1 (start) | — | — | 896 at 2x, 1,557 at 3x |
| `17-hires_display` | 4 | 38 | 176 at 2x, 302 at 3x | 176 at 2x, 302 at 3x |
| `18-hires_coverage_3t` | 1 (start) | — | — | 152 at 2x, 342 at 3x |
| `20-hires_threads` | 9 | 186 | — | 913 at 2x, 1,674 at 3x |
| `21-persp_floor_1x` | 1 (start) | — | — | 186 |

**Envelope:** Gouraud line pixels only, by at most +1 step per channel, mask bits preserved. Checked at 1x against the last-writer map: 1,894 changed pixels were last drawn by a line, 270 by semi-transparent primitives over changed line pixels, and 1,302 are scene `11`'s scaled averages and their copy at scene `12`'s start; every change is at most +1 step upward with no mask-bit change.

## Other stored values moved by the console-conformance fixes

Every updated value is identical across every build in `nix flake check`:

| File | Moved by |
|---|---|
| `tests/determinism_threads.txt` | X-flip: `random0`–`3` (24 hashes); flat-line dithering: `random0`–`3`, `flood` (3); line ties: `random0`–`3`, `flood`, `seed` (3), `first-light` (6); CLUT cache: `random0`–`3`; polygon interpolation: `random0`–`3`, `flood`, `seed`, `first-light`, `heavy` (6), `fill` (5); line colours: `random0`–`3`, `seed`, `first-light`, `heavy` |
| `tests/seed_rtt.c` | Line ties: 2x and 4x scaled hashes; polygon interpolation and line colours: all four (1x, 2x, 3x, 4x) |
| `examples/first-light/expected.txt` | All 8 runs at the time: line ties (4 pixels/frame on the ring), polygon interpolation (~26,000 pixels/frame on textured faces), line colours (~90 pixels/frame, at most 1 step) |
| `tools/jb-bench/bench.c` (`checks[]`) | Line ties: `first-light`, `heavy` at 1x; polygon interpolation: `first-light`, `heavy`, `fill`; line colours: `first-light`, `heavy` |

`tests/seed_fixes.c` and the conformance lists did not change.

## Scaled gradients, scaled line rounding and the dither setter

Three targeted fixes measured individually with `measure.sh` across recorded 1x/2x, 3x, 4x and 8x (283 VRAM images each):

| Fix | Seed images changed | Other stored values changed |
|---|---|---|
| Scaled gradients kept uncapped in 128-bit intermediates | None, at any scale | None |
| Scaled y-major lines rounded from exact rational coordinates | None, at any scale | `tests/determinism_threads.txt`: `random0`–`3` at 2x and 3x (options off and on, 16 hashes), `heavy` at 2x and 3x (options on, 2 hashes); `tools/jb-bench/bench.c` `checks[]`: `heavy` at 2x with options on (both thread counts) |
| `jb_set_dither` updates `GPUSTAT` bit 9 without sending `GP0(E1h)` | None (no pixel can move: preserves `GPUSTAT` bit 15 latch) | None |

`examples/first-light/expected.txt`, `tests/seed_rtt.c` and all conformance tests (1x, 2x and 3x) did not change.

- **Scaled gradients:** the scaled rasteriser previously clamped each gradient component to `2^30`, which distorted steep slivers produced with precise vertices. Keeping gradients exact in 128-bit arithmetic and splitting spans on range boundaries affects only triangles whose gradient exceeds `2^18` per 1x pixel (impossible for integer-vertex triangles, where the maximum is `255 * 1023`). Instrumented runs logged zero clamped gradients in the seed sequence (all scales), `determinism_threads` (48 cases), `first-light` and `jb-bench`. `tests/hires_slivers.c` tests these slivers directly.
- **Scaled y-major lines:** the scaled pass previously floored a y-major line's minor coordinate to `1/256` before adding `1/2`, causing coordinates in `(k + 1/2, k + 1/2 + 1/256)` to round down as ties. Rounding the exact rational coordinate affects only y-major scaled bands in that interval (shifting them one scaled column right) and never touches 1x VRAM or x-major lines. Instrumented checks logged 812 changed bands (330 at 2x, 482 at 3x, all y-major, none in the seed sequence); across the 372 dumps of `determinism_threads`' 48 cases, 1x VRAM was identical everywhere and frames differed in 388 scaled pixels (46 in `heavy` 2x on, 71 in `heavy` 3x on, the rest in `random0`–`3`: 372 directly on shifted band edges and 16 where 15-bit textured primitives sampled those scaled columns). `tests/hires_lines.c` tests the exact rational rule across all eight octants and scales.
- **Dither setter:** `jb_set_dither` modifies only the dither bit without re-issuing `GP0(E1h)`, keeping `GPUSTAT` bit 15 and the rest of the draw mode intact (`tests/api_draw_mode.c`).

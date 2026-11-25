# Testing and verification

This guide maps jitterbug's test suite and the tools behind it so that when a change breaks a test you can tell what failed, why it failed, and how to re-bless legitimately when a deliberate fix changes pixels or converter output.

## Running the test suite

Inside the pinned Nix shell:

```sh
nix develop -c cmake -S . -B build -G Ninja -DJB_TOOLS=ON -DJB_EXAMPLES=ON
nix develop -c cmake --build build
nix develop -c ctest --test-dir build --output-on-failure
```

A plain CMake build tests the ten checked-in `ps1-tests` conformance programs. To include the three fetched-only programs (`rectangles`, `texture-overflow`, `vram-to-vram-overlap`), build their lists once and pass the path to CMake:

```sh
nix build .#conformance-lists -o conformance-lists
nix develop -c cmake -S . -B build -G Ninja -DJB_TOOLS=ON -DJB_EXAMPLES=ON \
  -DJB_CONFORMANCE_FETCHED="$PWD/conformance-lists"
nix develop -c cmake --build build
nix develop -c ctest --test-dir build --output-on-failure
```

Before committing or opening a pull request, run the full flake check:

```sh
nix flake check -L
```

### Flake checks (`flake.nix`)

| Check | What it builds and verifies |
|---|---|
| `clang` | Static library, tools, examples and full CTest suite with Clang, `-DJB_WERROR=ON`, `JB_VULKAN=ON` (lavapipe), all 13 conformance programs, `JB_REQUIRE_PTY=ON` |
| `gcc` | Same with GCC |
| `sanitize` | Same with Clang + ASan/UBSan/`float-cast-overflow` (`JB_SANITIZE=ON`) and `JB_LIBFUZZER=ON` (`fuzz_files_libfuzzer`, 200,000 runs); also verifies `cmake --install` (whole build and each tool directory) refuses to install before copying any files |
| `shared` | Shared library build (`BUILD_SHARED_LIBS=ON`), verifying only `JB_API` symbols are exported (`exports`) |
| `O0` | Debug (`-O0`) Clang build checked against the exact same stored hashes |
| `wasm` | Single-threaded Emscripten (`wasm32`) build tested under Node, including example web modules |
| `x86-64-v3` | Clang `-O3 -march=x86-64-v3` run under QEMU Haswell-v4 (x86-64 hosts): verifies AVX/AVX2 instructions are emitted, no fused multiply-adds appear in `libjitterbug.a`, `isa --expect x86-64-v3` passes, and every stored hash matches |
| `arm64`, `arm64-clang` | Cross-compiled `aarch64-linux` with GCC and Clang, run under QEMU user-mode emulation (x86-64 hosts) |
| `conformance-record` | Re-records all 13 `ps1-tests` programs from pinned inputs, compares the 10 checked-in recordings (lists, dumps, logs and licence) byte for byte, checks fixture identities in `tests/conformance/fixtures.txt` for all 13, and verifies none of the 3 fetched-only programs is checked in |
| `fuzz-diff` | Differential fuzzing against `psxstack`'s software GPU (strict, bounded, invariants, transcripts, fixed cases, mutation self-test and sanitized run) |
| `gte-cases` | Re-converts `third_party/ps1-tests/gte/test-all.txt` from pinned `ps1-tests` and verifies byte-for-byte identity |
| `probe` | Builds the MIPS I hardware probe (`probe.exe`) and checks its PS-X EXE header, memory map and MIPS I-only disassembly (x86-64 hosts) |
| `package` | Unpacks the source release tarball, builds and runs CTest offline with CMake alone (driven by `flake.nix`; 10 checked-in conformance programs, no fetched fixtures or `noRebless`), then runs `tests/package/check.cmake` on moved installs: native static and shared (each with relative and absolute-inside-prefix layouts, the latter via `cmake --install --prefix`), shared only with `LIBDIR` equal to the prefix (using an explicit `jitterbug_DIR`), static only with a trailing-slash `LIBDIR`, and wasm32 (`find_package` plus a separate source-tree `add_subdirectory` test, no `pkg-config`); native runs test `find_package`, `pkg-config` (C only) and source-tree `add_subdirectory` |
| `package-nix` | Consumes the flake's existing static and shared installs in place (without moving them) via `find_package` and `pkg-config` |
| `browser-first-light` | Serves `packages.first-light-web` on loopback and runs `examples/first-light/web/check-browser.mjs` in headless Chromium in the Nix sandbox (`x86_64-linux` and `aarch64-linux`; not run by hosted CI): verifies every `expected.txt` hash, frame advancement, keys, layout and pause |
| `browser-demo` | Same for `packages.demo-web` and `examples/demo/web/check-browser.mjs`: verifies every `expected.txt` hash, frame advancement, keys, buttons, phone layout, pause and step |

By default `check-browser.mjs` checks that frames advance; when running either script by hand, any non-empty `JB_CHECK_FPS` value (for example `JB_CHECK_FPS=1`) enables a check expecting 30–70 frames of advance between two samples about a second apart (see the header comment in each `check-browser.mjs` for the by-hand command). Hosted CI runs only the `clang` and `wasm` flake checks (and builds the demo for GitHub Pages on pushes to `main`), not the browser checks.

Every library test build (`clang`, `gcc`, `sanitize`, `shared`, `O0`, `wasm`, `x86-64-v3`, `arm64`, `arm64-clang`; not `package`) also verifies (`noRebless`) that no CTest entry invokes `rebless.sh`, `--bless` (except `seed_bless_source`, which verifies refusal) or `--record-scale`.

Single-threaded builds (`JB_THREADS=OFF`, including Emscripten) omit multi-thread-only tests (`context_lifecycle`, `seed_multi`, `seed_sequence`, `seed_sequence_8t`, `seed_scale*_8t`, `first_light_4t`, `demo_4t`, `console_scene_4t`, `probe_pty_*`, `probe_line_*`) and force `--threads 1` on seed replay.

## Map of the test suite

| Area | CTest names | What they check |
|---|---|---|
| Header, build & platform | `version`, `header_cxx`, `assumptions`, `isa`, `exports`, `vk_smoke` | Strict C99 and C++11 header compliance; integer/float assumptions; CPU ISA checks; shared symbol exports and zero writable globals; Mesa lavapipe Vulkan smoke test |
| Core & software backend | `gte`, `gte_precise`, `gpu_smoke`, `gpu_smoke_stub`, `seed_rtt`, `seed_persp`, `seed_fixes`, `context_lifecycle`, `rules`, `clut_cache`, `hires_slivers`, `hires_lines`, `hires_wide` | Hand-worked GTE and precise-vertex cases; basic GP0 drawing; render-to-texture above 1x; perspective helpers; thread lifecycle; rules beyond public dumps (`rules.c`); CLUT cache across queues/threads; 128-bit scaled slivers, rational scaled line rounding, and narrow vs wide triangle set-ups |
| Public API & geometry | `api_stream`, `api_errors`, `api_read`, `api_frame`, `api_robust`, `api_prim`, `api_vram`, `api_gte`, `api_options`, `api_gte_helpers`, `api_threads`, `api_draw_mode`, `placer`, `api_texture`, `font`, `api_draw2d`, `api_friendly`, `api_records`, `api_draw3d`, `geom_float`, `geom_pipeline`, `split_fuzz`, `friendly_scene`, `sprite_scene`, `draw3d_scene`, `*_duplicate`, `console_scene`, `console_scene_4t` | Every public API surface, error/allocator failure handling, VRAM placer against brute-force model, deterministic float maths, screen-space polygon splitter invariants, stored-hash 2D/3D friendly scenes and the console-level scene |
| Seed golden traces | `seed_00-*`..`seed_27-*`, `seed_sequence`, `seed_sequence_1t`, `seed_sequence_8t`, `seed_scale3*`, `seed_scale4*`, `seed_scale8*`, `seed_multi`, `api_golden`, `api_golden_scale3`, `seed_compare_*`, `seed_bless_source`, `seed_measure`, `seed_audit_compare` | 28 recorded scenes at 1x, 2x, 3x, 4x and 8x across thread counts and through the public API, plus re-blessing and audit self-tests |
| Conformance & GTE | `conformance_<name>`, `conformance_<name>_2x`, `conformance_<name>_3x`, `conformance_selftest`, `conformance_refuse_*`, `gte_test_all`, `gte_test_all_partial`, `gte_test_all_no_case` | 10 checked-in (13 with `JB_CONFORMANCE_FETCHED`) `ps1-tests` GPU programs against console dumps/logs at 1x and in the 1x VRAM of 2x/3x contexts, fixture-binding refusals, and 1150 console GTE cases |
| Determinism | `determinism_threads` | Random streams, queue floods, seed scenes and `jb-bench` workloads at 1x–3x, float options off/on, 1, 2, 3, 4, 8 and 16 threads and mid-stream thread changes against `tests/determinism_threads.txt` |
| Fuzzing & robustness | `fuzz_robust`, `line_clip`, `fuzz_files`, `fuzz_files_libfuzzer` | Crash/memory-safety fuzzing of GP0/GP1/API and `.jbt`/`.jbm` loaders, plus exact evaluation of clipped lines |
| Files & converters | `convert_*`, `jb_tex_*`, `jb_mesh_*`, `api_files` | Byte-for-byte pinned `jb-tex` and `jb-mesh` outputs, CLI refusals without leaks, and loader round trips |
| Examples | `first_light*`, `demo*` | Stored frame hashes across threads and wasm modules, asset/source determinism, demo gap/fade/pier checks and headless CLI checks |
| Hardware probe | `probe_sim_*`, `probe_host_*`, `probe_runner_*`, `probe_cli_*`, `probe_pty_*`, `probe_line_*`, `conformance_probe_*` | Loopback simulation of `probe.c` and `jbh_session.c`, fault/malformed/timeout handling, runner binding, PTY/TCP time limits and (once console results exist) real-console references |

## Seed traces and re-blessing

`tests/seed/` holds 28 traces (`00-fill_convention.jbt` through `27-persp_threads.jbt`) and golden PNGs originally captured from the test program of the software GPU that jitterbug's core was seeded from (an earlier private project), plus the hardware-conformance fixes documented in [tests/seed/CHANGES.md](../tests/seed/CHANGES.md). Their binary layout and replay rules are specified in [tests/seed/FORMAT.md](../tests/seed/FORMAT.md).

When a `seed_*` test fails, `test_seed_replay` writes `*.png.out` images into `build/tests/seed-out/` so you can inspect the visual difference.

### Legitimate re-blessing

Re-blessing is permitted **only** for a deliberate renderer fix that is intended to change pixels, backed by evidence (a console dump, or psx-spx where none exists). Never re-bless to make a compiler, thread count, optimisation level or refactoring pass.

`test_seed_replay` refuses `--bless` and `--record-scale` directly into the source tree's `tests/seed/` (and `tests/seed/scaled/`). `seed_bless_source` exercises the `--bless` refusal, and `flake.nix` enforces via `noRebless` that no CTest entry in the library test builds re-blesses.

To measure and re-bless a deliberate fix:

```sh
# 1. Save the replayer from before your fix
cp build/tests/test_seed_replay build/test_seed_replay_before

# 2. Apply the fix and rebuild
nix develop -c cmake --build build

# 3. Measure every changed pixel across the 28-scene sequence (1x/2x and 3x)
tools/seed-record/measure.sh build/test_seed_replay_before build/tests/test_seed_replay

# 4. Verify every changed pixel lies within the fix's envelope,
#    then record the evidence and counts in tests/seed/CHANGES.md.

# 5. Re-bless tests/seed/
tools/seed-record/rebless.sh build
```

What each script does:

- **`tools/seed-record/measure.sh OLD NEW [SCALES]`** (default `SCALES="1 3"`): runs all 28 scenes in `tools/seed-record/scenes.txt` in sequence with both replayers (`--dump` and `--compare`) and prints a `DIFF` line for every capture whose VRAM differs (pixel count, bounding box, per-channel 5-bit delta range, pixels moving by more than 1 step, and mask-bit changes). Raw dumps stay in the scratch directory (`WORK` environment variable) for inspection. It fails if either replayer exits with anything other than 0 or 1, does not replay all 28 scenes, dumps a different image set, or misses any reference (`seed_compare_*` and `seed_measure` test these safeguards).
- **`tools/seed-record/rebless.sh BUILD`**: works on a copy of `tests/seed/` in its scratch directory (`WORK`), runs `test_seed_replay --bless` (rewriting only CAPTURE hashes, prelude start hashes, `STORE` pixels and `PRESENT` frames while preserving ops, preludes and standalone flags), encodes end-state `.raw` dumps to PNG with `tools/seed-record/vram2png.py --check`, regenerates `scaled/{3,4,8}x.txt` and 3x PNGs via `--record-scale`, verifies every scene alone and in sequence at recorded scales, 3x, 4x and 8x, and finally installs the verified directory by staging a checked copy beside `tests/seed/`, renaming the old `tests/seed/` aside and renaming the staged copy into place. If anything fails before the swap, `tests/seed/` is untouched; if the second rename fails, `rebless.sh` attempts to move the old directory back, and if rollback fails too it prints the backup path to restore by hand.
- **`tools/seed-record/seedcheck.c`**: standalone C99 trace reader written from [tests/seed/FORMAT.md](../tests/seed/FORMAT.md) alone (`cc -std=c99 -o seedcheck tools/seed-record/seedcheck.c`).

### Updating other stored hashes

If a deliberate renderer fix changes pixels in other stored-hash tests, inspect the diffs, record the cause in [tests/seed/CHANGES.md](../tests/seed/CHANGES.md), and update each affected file in the same commit:

- `tests/determinism_threads.txt`: check the header comment in `tests/determinism_threads.c` / run `test_determinism_threads --print`.
- `tests/friendly_scene.txt`, `tests/sprite_scene.txt`, `tests/draw3d_scene.txt`: run `test_<name> --print`.
- `tests/seed_rtt.c`: update the four hashes documented in the file.
- `tests/console_scene/expected.txt`: preserved baseline for the console-level scene; `test_console_scene --expect` prints the current hashes to update by hand if needed.
- `examples/first-light/expected.txt`: `nix develop -c cmake --build build --target first_light_record`.
- `examples/demo/expected.txt`: `nix develop -c cmake --build build --target demo_record`.
- `tools/jb-bench/bench.c` (`checks[]`): run `jb-bench --check` and inspect the reported hashes.

### Polygon-interpolation audit (`tools/seed-record/audit/`)

The audit verifies pixel by pixel that the 1x triangle-interpolation fix logged in [tests/seed/CHANGES.md](../tests/seed/CHANGES.md) moved only pixels within its allowed envelope, failing closed on anything else:

```sh
nix develop -c tools/seed-record/audit/audit.sh [TREE]
```

`audit.sh` copies `TREE` (default: a temporary git worktree of `HEAD`) twice under `WORK` (needs ~3 GB of scratch space, cleaned up unless `KEEP=1`), applies `instrument.patch` to both and `old.patch` (pre-fix 1x triangle interpolation) to one, builds `test_seed_replay` in each, replays the 28-scene sequence on one thread with `JB_AUDIT=1` to capture the last 16 primitive events at every differing 1x pixel, and runs `compare.c`. Exit status `1` indicates pixels outside the envelope (up to 25 listed per reason); exit status `2` indicates invalid inputs. `tools/seed-record/audit/selftest.c` tests `compare.c` on synthetic captures in every build as `seed_audit_compare`.

What `compare.c` does not verify: that recorded texels match VRAM at the recorded coordinates, that coordinates/8-bit colours match the primitive's vertices (only movement between runs is bounded), that recorded primitive state flags match GP0 words, or scaled VRAM (which `old.patch` does not revert).

## Conformance tests and the recorder

`tests/conformance/` replays recorded command lists (`.jbc`, [tests/conformance/FORMAT.md](../tests/conformance/FORMAT.md)) of 13 `ps1-tests` GPU programs through the public API at 1x, and again in the 1x VRAM of 2x (4 threads) and 3x (3 threads) contexts. When a run fails a VRAM, check, read, wait or main-loop check, `conformance` writes `NAME.ours.png` into `build/tests/conformance/conformance-out/` (or `/2x`, `/3x`), and writes `NAME.diff.png` as well when a reference VRAM image exists (dump and probe fixtures; log-only fixtures have no reference image).

### Checked-in vs fetched-only programs

This repository never checks in lists or dumps containing third-party images (anything other than geometry, colours or pixels computed by the test program itself):

| Programs | Images loaded | Disposition |
|---|---|---|
| `triangle`, `quad`, `lines`, `transparency`, `uv-interpolation`, `clipping`, `texture-flip`, `clut-cache`, `mask-bit`, `gp0-e1` | None (textures/CLUTs are computed in code) | Checked in (`repo`) |
| `rectangles` | `lena.h` (`lena.tim`, 128 x 128 photograph crop) | Fetched-only (`fetched`) |
| `texture-overflow` | `lena.h` and `cube.h` (`cube.tim`, origin unrecorded) | Fetched-only (`fetched`) |
| `vram-to-vram-overlap` | PSn00bSDK debug font (`FntLoad`, MPL-2.0) | Fetched-only (`fetched`) |

See [third_party/ps1-tests/README.md](../third_party/ps1-tests/README.md) for licence details.

### How `tools/conformance-record` works

Each `ps1-tests` GPU program is a PSn00bSDK program. `tools/conformance-record/` fetches each program and its common GPU helper (`common/gpu.c` or `gpu.cpp`, `common/gpu.h`) from the `ps1-tests` commit that last changed its dump or log (`tools/conformance-record/programs.txt`; `mask-bit` is recorded from commit `79dfb13` and checked word-for-word against its log-era commit `d190785` via `jbc-words`), and compiles them unmodified on the host with Clang against a clean-room shim (`include/`, `src/`) written from psx-spx and observed PSn00bSDK behaviour (pinned at `b0659ad85b7aa6e74d2c3eac29281636a0c2bc5e`):

- **Clang-only hooks:** `-fsanitize-coverage=inline-8bit-counters,trace-loads,trace-stores` intercepts direct MMIO stores/loads on the mapped `1F801000h` page (`clut-cache`), `-finstrument-functions` records `CALL` annotations on `ps1-tests`' common library, and Clang's left-to-right argument evaluation matches `clipping`'s `rand()` colour order.
- **Determinism flags:** `-fsigned-char`, `-ftrivial-auto-var-init=zero`, `-fwrapv`, `-fno-strict-aliasing`, `-ffp-contract=off -O1`, and `-include include/jbrec_prelude.h`.
- **Uninitialised fields:** unset packet fields become `0` instead of console stack garbage. The only unset field reaching VRAM is the fourth vertex colour of `lines`' two Gouraud polylines, handled by `tests/conformance/lines.exclude` (64 pixels; see [docs/conformance.md](conformance.md)).
- **Main loop & checks:** recording stops at the fourth `VSync(0)` (frames 2, 3 and 4 must emit identical words; frame 2 becomes `LOOP`). For `mask-bit` and `gp0-e1`, each `assertEquals` becomes an `EXPECT` op matched in order against `psx.log`.
- **Unknowns recorded:** `ResetGraph` reads NTSC (GPUSTAT bit 20 = 0), unset stack bytes are 0, exact console build timing is not captured, and the upstream console model is unrecorded.

To regenerate the checked-in lists, dumps and logs:

```sh
tools/conformance-record/regen.sh           # from flake inputs (stage recorder edits first)
tools/conformance-record/regen.sh --local   # from local git clones (PS1_TESTS_REPO, PSN00BSDK_REPO)
```

`record.sh` records every program twice and requires byte-identical outputs. If a list or reference changes, update `WORDS`, `CHECKSUM` or `REFSUM` in `tests/conformance/fixtures.txt` from the generated `fixtures-identity.txt`. The `conformance-record` flake check enforces that all checked-in files and `fixtures.txt` match a fresh recording from pinned inputs and that no fetched-only program or PSn00bSDK file is checked in.

## Open-question lists and console probe results

### `tests/conformance/open/`

Seven small hand-built `.jbc` experiments (`tools/fuzz-diff/open_lists.c`, generated by `fuzz-diff open-lists DIR`) isolate rules where jitterbug and `psxstack` disagree and no `ps1-tests` dump decides (see [docs/conformance.md](conformance.md)). Four are **open** (neither dumps nor psx-spx specify the rule) and three are **confirmations** (psx-spx gives the direction jitterbug follows):

| List | Kind | Experiment | Reading VRAM |
|---|---|---|---|
| `offset-wrap` | open | White 16 x 8 control rect at `(8, 8)` with offset `0`; then offset `x = 1000` and red 1023 x 8 rect at vertex `(40, 100)` (`sum = 1040`) | Red at `x = 0..14`, rows `100..107` (120 px): 11-bit wrap (`-1008`, `psxstack`). Nothing there: unwrapped (jitterbug) |
| `quad-order` | confirmation | Opaque Gouraud quad with exact 12-bit gradients (doubled area 4096) and overlapping triangles: `v0=(164,112)` red, `v1=(100,96)` & `v2=(100,160)` green, `v3=(164,144)` blue | Blue in overlap: `v1 v2 v3` drawn last (psx-spx, jitterbug). Red: `v0 v1 v2` drawn last (`psxstack`) |
| `raw-gouraud-dither` | confirmation | Raw-textured Gouraud triangle (`GP0(35h)`) with constant texel `(21, 13, 6)` drawn at `(20, 20)` undithered and at offset `(64, 0)` dithered | Copies equal: raw is undithered (psx-spx, jitterbug). Second copy 1 step lower on negative dither entries: dithered (`psxstack`) |
| `dither-semi` | open | Over background `10`, equal-colour Gouraud triangle `(99, 99, 99)` dithered: opaque control at `(8, 8)`, mode 0 semi-transparent at `(72, 8)` | Semi-transparent `10` only at dither `-4`, else `11`: dither to 5 bits then blend (jitterbug). `10` at `-4, -3, -2`: blend in 8 bits then dither (`psxstack`) |
| `modulated-semi-quarter` | confirmation | Texels `t = 1..31` (bit 15 set) in mode 3 (`B + F/4`) over `8`: colour `(127, 100, 64)` on row 100, control colour `128` on row 102 | Row 100 = `8 + ((t*c/128)/4)`: quarter after modulation (psx-spx, jitterbug). `8 + ((t/4)*c/128)`: quarter before (`psxstack`, differs in 21 of 31 px) |
| `modulated-semi-saturate` | open | Same texels in mode 0 (`(B + F)/2`) over `8` with colour `255` on row 100 (`t * 255 / 128 > 31` for `t >= 17`) | `19` for all `t >= 17`: saturated before blend (jitterbug). Rising up to `31`: saturated after (`psxstack`) |
| `gouraud-texture-pairs` | open | Opaque, undithered Gouraud modulated triangle `(0,200)-(31,200)-(0,216)`, texel `31`, red `0` to `248` (8 per pixel) | Every pixel has its own ramp value (jitterbug). Pixel `2k+1 == 2k`: colour sampled per pixel pair (`psxstack`) |

`tests/conformance/open/fixtures.txt` records each list's size and FNV-1a checksum (`fuzz/NAME`, `PASSES=0`, `EXPECTS=0`). Every build runs all seven through the probe's host simulation (`probe_sim_open_*`, `probe_runner_open_*`), and `fuzz-diff play` confirms each separates jitterbug from `psxstack` solely on its single unknown.

### `tests/conformance/probe/`

No console has run the hardware probe yet, so `tests/conformance/probe/fixtures.txt` currently has no entries. Once console results are captured (see [docs/probe.md](probe.md)):

- Save `NAME.jbpr` (unchanged output of `jbprobe send --fixtures ...`) and `NAME.console.txt` (`jbprobe info NAME.jbpr` plus physical model number, mainboard revision, region, cable, date and operator).
- Add `NAME ps1-tests` or `NAME open` to `tests/conformance/probe/fixtures.txt`. CMake automatically adds `conformance_probe_NAME` comparing all 16 bits of VRAM, every read and every check against the console result, refusing non-console or unclean files.
- Never check in `.jbpr` files for the three fetched-only `ps1-tests` programs (`rectangles`, `texture-overflow`, `vram-to-vram-overlap`), because their VRAM contains the same third-party images.

## Differential fuzzing (`tools/fuzz-diff/`)

`tools/fuzz-diff` drives seeded `GP0` streams through both jitterbug and [psxstack](https://github.com/gascarcella/psxstack)'s `psyq/gpu.c` (MIT, pinned at `40071978ec43f76757a5e7fc5c89ee680bccd3bb` in `tools/fuzz-diff/pin.sh` and `flake.nix`; fetched at build time, never vendored). `psxstack` is an independent proxy matched against PCSX-Redux, **never** ground truth — console dumps and psx-spx always decide disagreements.

After every command (including prelude fills, background loads, state words and `GP0(01h)`), all 16 bits of all `1024 x 512` VRAM pixels and every `GPUREAD` word during `C0h` transfers are compared, and under `GP0(E6h)` bit 1 each side is checked independently to ensure no pixel with bit 15 set changed.

### Three modes

- **`strict`:** a subset of `GP0` constructed so none of the nine known rule differences (D1–D9: interpolation precision, Gouraud-textured pixel pairs, line/raw/flat-modulated dithering, dither-vs-blend order, modulated semi-transparent saturation/quartering, line rounding/clipping, quad triangle order, 11-bit offset wrap, and unmodelled rectangle flips/CLUT cache/overlapping copies) can fire. Any single-bit VRAM or `GPUREAD` difference, or any protected pixel write on either side, fails.
- **`bounded`:** arbitrary Gouraud untextured polygons (non-overlapping quads) and constant-texel textured polygons where D1 (12-bit truncated gradients vs exact plane) may cause up to 1 five-bit channel step difference per pixel. Each bounded polygon must leave VRAM identical outside its bounding box, write the exact same pixel footprint (measured by re-running the command with bit 15 cleared in the box and `E6h` bit 0 forced on), leave all protected pixels untouched, match mask bits, and stay within 1 channel step inside the box. The reference then syncs its box to jitterbug so subsequent commands start from identical VRAM (so bounded mode bounds single commands, not multi-command drift).
- **`run` (exploratory):** full generator with feature ablation and hypothesis labels (`rect-flip`, `offset-wrap`, `own-clut`, `copy-overlap`, `lines`, `colour-interp`, `uv-interp`, `quad-order`, `flat-texture-dither`, `raw-gouraud-dither`, `dither-semi`, `modulated-semi`) for triage only — never a pass criterion.

### Running `fuzz-diff` locally

```sh
nix develop -c cmake -S tools/fuzz-diff -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPSXSTACK_DIR=<path-to-psxstack-checkout>
nix develop -c cmake --build build-fuzz

build-fuzz/fuzz-diff strict --seed 1 --streams 600
build-fuzz/fuzz-diff bounded --seed 1 --streams 200
build-fuzz/fuzz-diff repro 1 0 --mode strict --out repros
build-fuzz/fuzz-diff play repros/*.jbc
build-fuzz/fuzz-diff invariants
build-fuzz/fuzz-diff hash --streams 300 | diff - tools/fuzz-diff/hashes.txt
nix develop -c sh tools/fuzz-diff/selftest.sh <path-to-psxstack-checkout> build-fuzz/selftest
sh tools/fuzz-diff/cases.sh build-fuzz/fuzz-diff build-fuzz/cases
```

Set `FD_SHOW=x,y,w,h` to inspect pixels after `repro` or `words`, and pass `-DJB_SANITIZE=ON` to build jitterbug and `fuzz-diff` (not `psxstack`'s `gpu.c`) under ASan and UBSan.

### Self-tests and what `checks.fuzz-diff` runs

`checks.fuzz-diff` verifies:
1. Stream-generator determinism (`hash --streams 300` against `tools/fuzz-diff/hashes.txt`).
2. `strict` (seed 1, 600 streams), `bounded` (seed 1, 200 streams), `invariants` (synthetic mask-check invariant tests, including the case where both sides violate it identically), whole-stream transcript round trips (`repro --whole` + `play`), and `cases.sh`.
3. **Mutation self-test (`selftest.sh` / `mutate.sh`):** compiles ten temporary copies of jitterbug each injecting one deliberate bug (`line-clip`, `gouraud-tex`, `uv-varies`, `avg-up`, `outside-right`, `stray-above`, `near-drop`, `prelude-stray`, `read-last`, `mask-through`), verifying that each is caught by the exact expected mode (or `tests/line_clip.c` for `line-clip`, which `psxstack` cannot referee) and that its saved `.jbc` reproducer fails under `play`.
4. A sanitized build running `hash`, `strict` (100 streams), `bounded` (40 streams), `invariants`, `cases.sh` and 20 exploratory streams with shrinking.

On seed `20261121`, extended parallel runs executed 30,000 strict streams (2,991,080 drawing commands plus 810,008 transfers) and 10,000 bounded streams (1,196,784 polygons) with 0 failures.

### What differential fuzzing cannot check

Because `psxstack` differs from console dumps on several rules (verified by `fuzz-diff conformance`: it differs on `triangle`, `lines`, `uv-interpolation`, `texture-flip`, `clut-cache` and `vram-to-vram-overlap`), other tests cover what it cannot referee:
- Lines and line clipping: `lines` dump, `tests/rules.c`, `tests/line_clip.c`.
- Rectangle flips, CLUT cache, overlapping copies, exact interpolation: `tests/conformance/`, `tests/rules.c`, `tests/clut_cache.c`.
- Open questions: `tests/conformance/open/` (awaiting console probe runs).
- Crash safety on malformed GP0/GP1 streams: `tests/fuzz_robust.c` and `tests/api_robust.c`.

## File, converter and loader tests

`tests/files/` pins `jb-tex` and `jb-mesh` outputs byte for byte across every platform and build:

- `tests/files/inputs/`: original PNG, OBJ/MTL and glTF/GLB inputs generated deterministically by `tests/files/make-inputs.py` (including deliberately malformed files).
- `tests/files/conversions.txt`: list of `jb-tex` and `jb-mesh` invocations run as `convert_<output>` tests via `tests/files/convert.cmake`.
- `tests/files/expected/`: pinned `.jbt` and `.jbm` files compared byte for byte, also used by `api_files` (loader round trips and frame hashes), `fuzz_files` (seeded mutation fuzzing under CTest), and `fuzz_files_libfuzzer` (when `-DJB_LIBFUZZER=ON` with Clang).
- `jb_tex_*` and `jb_mesh_*`: CLI refusal tests verifying exact exit status `1`, diagnostic text and zero sanitizer/leak reports.

After a deliberate change to a converter (or after running `python3 tests/files/make-inputs.py tests/files/inputs`), regenerate the expected files and inspect the diff:

```sh
nix develop -c cmake --build build
nix develop -c cmake -DBUILD="$PWD/build" -DREGEN=ON -P tests/files/convert.cmake
```

If rendered pixels from the test files change, `build/tests/test_api_files tests/files --print` prints the updated frame hashes and `--ppm PATH` writes frames for visual inspection.

## Package and install tests

`checks.package` and `checks.package-nix` verify what a downstream user gets from an installed jitterbug, using `tests/package/check.cmake`:

- **Tarball build and tests (`checks.package`, driven by `flake.nix`, not `check.cmake`):** unpacks the source release tarball, configures and builds it offline with CMake alone, and runs its CTest suite (without `JB_CONFORMANCE_FETCHED` and without `noRebless`).
- **Moved-install paths (`checks.package`, via `tests/package/check.cmake`):** installs jitterbug into a prefix, **moves** the prefix to verify relocatability, checks that installed files (`include/jitterbug.h`, `jitterbugConfig.cmake`, `jitterbugConfigVersion.cmake`, `jitterbugTargets.cmake`, `jitterbug.pc`, tools and licences under `share/licenses/jitterbug/`) are present and contain no build/source/original-prefix paths or internal targets (`jb_build_flags`, `jb_internal`), across separate paths:
  - **Native static and shared:** each tested with a relative layout and with `CMAKE_INSTALL_LIBDIR`, `INCLUDEDIR`, `BINDIR` and `DATADIR` given as absolute paths inside the prefix (installed via `cmake --install --prefix` and verified to write nothing into the originally configured prefix).
  - **`LIBDIR` equal to the prefix:** shared build only, locating the package with an explicit `jitterbug_DIR`.
  - **Trailing-slash `LIBDIR`:** static build only.
  - **WebAssembly (Emscripten):** `find_package` against the moved install plus a separate source-tree `add_subdirectory` consumer, with no `pkg-config`.
- **Consumers built and run in `checks.package`:**
  - `find_package(jitterbug 0.1)` against the moved install: `tests/package/consumer.c` (strict C99) and `consumer_cxx` (C++11) for all paths above.
  - `pkg-config` against the moved install (native builds only, not Emscripten): `tests/package/consumer.c` only (strict C99; `--static` for static builds, verifying `-lm`).
  - `add_subdirectory` directly against the source tree (not the moved install): `tests/package/consumer.c` and `consumer_cxx`.
- **Existing Nix installs (`checks.package-nix`):** invokes `tests/package/check.cmake` with `-DPREFIX=...` on the flake's existing static (`packages.default`) and shared installs in place without moving them, verifying the installed files and running `find_package` and `pkg-config` consumers (without `add_subdirectory`).

Separately, `checks.sanitize` verifies that `cmake --install` on a `-DJB_SANITIZE=ON` build — both for the full build and for each tool directory (`tools/jb-tex`, `tools/jb-mesh`, `tools/jb-bench`) on its own — fails with an error before copying any files.

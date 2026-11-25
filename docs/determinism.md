# Determinism

Given the same API calls, arguments and runtime options, jitterbug is designed to produce identical pixels and GTE registers on every supported platform and at every thread count. This page states the exact scope of that guarantee, how the implementation preserves it, which builds and tests verify it, and what remains unchecked.

## The guarantee

For identical calls, inputs and options (`scale`, `precise_vertices`, `perspective_correct`, `dither`, `clut_cache`, `widescreen`, `geometry`), a context produces the same 1x VRAM (`GP0(C0h)`, `jb_gpuread`, `jb_vram_read`), the same scaled VRAM, the same `jb_frame` RGBA8 bytes, and the same GTE registers, independent of:

- the number of rasterising threads (`jb_config.threads`, `jb_set_threads`), including changing thread counts mid-draw;
- how GP0 command words are split across `jb_gp0` calls;
- the compiler (`gcc`, `clang`), optimisation level (`-O0`, `-O3`) and instruction-set target (`x86-64` baseline vs `x86-64-v3`);
- the CPU architecture (`x86-64`, `aarch64`, `wasm32`).

"As far as tested" means: across the builds listed in [Where it is checked](#where-it-is-checked), using the compiler versions pinned by `flake.nix`, against one set of stored hashes checked into the repository.

## How the code keeps it

- **Integer-only console path:** with `precise_vertices` and `perspective_correct` off, the console-level rasteriser (`src/soft/raster.c`) and GTE (`src/geom/gte.c`) use integer arithmetic exclusively.
- **Deterministic floating point on the friendly level and float options:** the friendly 3D maths (`src/geom/fmath.c`: `jb_sinf`, `jb_cosf`, `jb_tanf` and matrix helpers), the float geometry path, GTE pipeline set-up and clipping (`src/geom/pipeline.c`, which uses `double` for view-space coordinates and clipping), and the two float rendering options (`precise_vertices`, `perspective_correct`) use only correctly rounded IEEE 754 `float`/`double` `+`, `-`, `*`, `/`, `sqrtf` and `sqrt` — never libm's platform-varying trigonometry.
- **Compiler guards (`CMakeLists.txt`, `src/core/assumptions.c`):** every target compiles with `-ffp-contract=off` so compilers cannot fuse multiply-adds differently. Compile-time checks refuse `-ffast-math`, non-zero `FLT_EVAL_METHOD` (such as x87 extended precision), non-IEEE `float`/`double`, and big-endian hosts.
- **Floating-point environment:** identical float results assume the standard C startup environment (round-to-nearest ties-to-even, subnormals preserved — the only mode WebAssembly supports). Calling the library while `fesetround`, flush-to-zero (FTZ) or denormals-are-zero (DAZ) is active can change float results in the last bit; `tests/geom_float.c` verifies the active environment on startup. The integer console path does not depend on the floating-point environment.
- **Canonical NaN:** documented NaN returns (`jb_sinf`, `jb_cosf`, `jb_tanf` on infinity or NaN) always return quiet NaN `0x7FC00000` rather than hardware-dependent NaN payloads, and the geometry pipeline saturates values before every float-to-integer conversion.
- **Integer assumptions (`src/core/assumptions.c`, `tests/assumptions.c`):** two's complement, 32-bit `int`, arithmetic right shift of negative integers, low-bit-preserving narrowing conversions, and little-endian byte order are verified both at compile time and at run time.
- **Sanitizer cleanliness:** the `sanitize` flake check runs the full suite under ASan, UBSan and `-fsanitize=float-cast-overflow` with `-fno-sanitize-recover=all` and without `-fwrapv`.
- **Queue and band ordering (`src/soft/soft.c`):** multi-threaded rasterisation assigns interleaved 16-row bands exclusively to individual workers and rasterises every queued primitive in submission order. The queue flushes automatically on 32x32 tile read-after-write or write-after-read hazards, and any primitive sampling its own destination tiles runs alone on the calling thread.

## What is compared

Twelve sets of stored values live in the repository and are compared by every renderer test build — no build only checks against itself:

| Stored values | Coverage | Tests |
|---|---|---|
| `tests/seed/` (`*.jbt`, `*.1x.png`, `*.2x.png`, `scaled/*.txt` and PNGs) | 28 golden scenes: VRAM hashes, frame hashes, `GPUREAD` outputs and final state at 1x, 2x, 3x, 4x and 8x ([tests/seed/CHANGES.md](../tests/seed/CHANGES.md)) | `seed_*`, `api_golden`, `api_golden_scale3` |
| `examples/demo/assets/*.jbt`, `*.jbm` and `sources/` | Demo textures, meshes and generated source art, byte for byte | `demo_assets`, `demo_sources` |
| `examples/demo/expected.txt` | Demo flight loop, scripted switch tour, single-switch and `all3` runs | `demo`, `demo_4t`, `demo_web_module` |
| `examples/first-light/expected.txt` | `first-light` at 1x–3x, every switch individually and all together | `first_light`, `first_light_4t`, `first_light_web_module` |
| `tests/console_scene/expected.txt` | Console-level `first-light` scene across scales and options | `console_scene`, `console_scene_4t` |
| `checks[]` in `tools/jb-bench/bench.c` | Short runs of all three benchmark workloads at 1x–3x, options off/on, 1–3 threads | `jb_bench_check` |
| `tests/determinism_threads.txt` | 4 hazard-heavy random GP0 streams, a queue-overflow flood, all 28 seed scenes in sequence, and all 3 benchmark workloads at 1x–3x, options off/on | `determinism_threads` |
| Hashes in `tests/geom_float.c` and `tests/geom_pipeline.c` | `jb_sinf`/`jb_cosf`/`jb_tanf` over 20,000 inputs, `jb_mat4` helpers, and 400 seeded 40-face 3D scenes on both GTE and float paths | `geom_float`, `geom_pipeline` |
| `tests/draw3d_scene.txt` | Friendly 3D scene (`jb_draw_mesh`, `jb_draw_billboard`, `jb_draw_line3d`, near-plane clipping, fog) across GTE/float, 1x/2x, options off/on, 1 and 4 threads | `draw3d_scene` |
| `tests/friendly_scene.txt` | Friendly 2D clipped scene across 1x–3x, options, mid-run widescreen switch and single-buffered 640x480 | `friendly_scene` |
| `tests/files/expected/` and hashes in `tests/api_files.c` | Byte-for-byte `jb-tex` and `jb-mesh` outputs for `tests/files/conversions.txt`, plus rendered frames through `.jbt`/`.jbm` loaders | `convert_*`, `api_files` |
| `tests/sprite_scene.txt` | Sprites (1:1, scaled, flipped, tiled, clipped), palette animation and text across 1x–3x, widescreen and 640x480 | `sprite_scene` |

### Systematic thread-count testing (`determinism_threads`)

`tests/determinism_threads.c` runs every case in `tests/determinism_threads.txt` on fresh contexts at **1, 2, 3, 4, 8 and 16 threads**, plus a run calling `jb_set_threads` mid-draw (`1 -> 16 -> 2 -> 8 -> 3 -> 4`), while cutting GP0 streams three ways (whole, word-by-word, and random chunk sizes). In threaded builds, any context log warning that fewer threads started than requested fails the case, preventing silent single-threaded fallback. Removing any one of the three queue-ordering rules in `src/soft/soft.c` breaks 22–27 cases of this test.

## Where it is checked

`nix flake check` builds and tests all of the following using the pinned nixpkgs toolchains (gcc 16.2.0, clang 21.1.8, Emscripten 6.0.9 under Node 24.22.2, QEMU 11.1.2):

### Renderer test builds

| Check | Compiler and target | Execution environment | Hosts | Threads |
|---|---|---|---|---|
| `clang` | clang, Release (`-O3`), static | native build host | x86-64, aarch64 | 1–16 |
| `gcc` | gcc, Release (`-O3`), static | native build host | x86-64, aarch64 | 1–16 |
| `shared` | clang, Release (`-O3`), shared library | native build host | x86-64, aarch64 | 1–16 |
| `sanitize` | clang, Release, ASan + UBSan + libFuzzer | native build host | x86-64, aarch64 | 1–16 |
| `O0` | clang, Debug (`-O0`) | native build host | x86-64, aarch64 | 1–16 |
| `wasm` | Emscripten, `wasm32`, Release, `JB_THREADS=OFF` | Node | x86-64, aarch64 | 1 |
| `x86-64-v3` | clang, Release, `-march=x86-64-v3` | `qemu-x86_64 -cpu Haswell-v4` | x86-64 | 1–16 |
| `arm64` | gcc, `aarch64-linux` cross, Release | `qemu-aarch64` | x86-64 | 1–16 |
| `arm64-clang` | clang, `aarch64-linux` cross, Release | `qemu-aarch64` | x86-64 | 1–16 |

`nix flake check` also runs six non-renderer checks:

| Check | Purpose | Hosts |
|---|---|---|
| `conformance-record` | Re-records all 13 ps1-tests lists from pinned sources and compares with checked-in files and `fixtures.txt` | x86-64, aarch64 |
| `gte-cases` | Re-converts the 1150 GTE cases (`tools/gte-cases/regen.sh --check`) and compares | x86-64, aarch64 |
| `fuzz-diff` | Differential fuzzing against psxstack's GPU (strict, bounded, invariants, mutation self-test, ASan+UBSan) | x86-64, aarch64 |
| `probe` | Cross-compiles the MIPS I hardware probe PS-X EXE and verifies its header, memory map and ISA | x86-64 |
| `package` | Builds and tests the source tarball offline with CMake alone, then verifies relocatable static, shared and wasm32 installs via `find_package`, `pkg-config` and `add_subdirectory` | x86-64, aarch64 |
| `package-nix` | Consumes `packages.default` and the shared Nix build via `find_package` and `pkg-config` | x86-64, aarch64 |

### Native `aarch64-linux` run on physical arm64 hardware

In November 2026 (0.1.0 plus pending fixes), the full `aarch64-linux` flake check suite (`nix flake check -L` — `clang`, `gcc`, `shared`, `sanitize`, `O0`, `wasm`, `conformance-record`, `gte-cases`, `fuzz-diff`, `package` and `package-nix`) passed natively in an Apple silicon NixOS virtual machine (`aarch64-linux`), matching all checked-in hashes and passing the threaded determinism tests across thread counts 1–16. The native `clang` check was then rebuilt and run 20 times in a row (`nix build --rebuild .#checks.aarch64-linux.clang`) with no failure, to give intermittent memory-ordering bugs in `src/soft/workers.c` a chance to show under arm64's weak memory model. This covers one chip family (Apple silicon); routine checks on x86-64 hosts still use the `qemu-aarch64` cross builds above.

### Thread counts exercised across the suite

| Tests | Thread counts |
|---|---|
| `determinism_threads` | 1, 2, 3, 4, 8, 16, and changing mid-draw |
| `seed_NN-*`, `seed_sequence`, `seed_scale3/4/8`, `api_golden`, `api_golden_scale3` | Recorded count per scene (1–8) |
| `seed_sequence_1t/8t`, `seed_scale3_1t/8t`, `seed_scale4_1t/8t` | 1 and 8 forced |
| `seed_multi` | Two contexts at recorded counts on 1 and 2 threads |
| `seed_fixes` | 1 vs 2, 5 and 8 |
| `gpu_smoke`, `api_prim`, `api_records`, `friendly_scene`, `sprite_scene`, `draw3d_scene`, `api_draw3d` | 1 and 4 |
| `api_threads` | 0, 1, 2, 4, 16, then 8 and 1 via `jb_set_threads` |
| `first_light/_4t`, `demo/_4t`, `console_scene/_4t` | 1 and 4 |
| `jb_bench_check` | 1, 2 and 3 |

### Build-specific test exclusions

No registered test is silently skipped in `nix flake check` (`JB_REQUIRE_PTY=ON` turns missing `/dev/ptmx` in `probe_pty_*` and `probe_line_*` into failures rather than skips). Tests not built in particular configurations:

- **Single-threaded (`wasm` or `JB_THREADS=OFF`):** thread requests clamp to 1; multi-thread-only tests (`context_lifecycle`, `seed_multi`, `*_4t`, `*_8t`, `probe_pty_*`, `probe_line_*`) are omitted, and `seed_NN` runs with `--threads 1`.
- **`wasm` only:** `exports` is omitted (WebAssembly has no separate read-only data segment for `nm`), POSIX-only `probe_cli_*` are omitted, and `first_light_web_module` and `demo_web_module` are added.
- **`vk_smoke`:** runs only in native checks with Vulkan and Mesa lavapipe (it draws nothing and hashes nothing).
- **`seed_measure`:** runs only on non-emulated native builds (tests `tools/seed-record/measure.sh`, not rendering).
- **Emulated checks (`x86-64-v3`, `arm64`, `arm64-clang`):** `flake.nix` verifies that every test command invokes `CMAKE_CROSSCOMPILING_EMULATOR` (or the host `cmake`), so binfmt registration is never relied upon.

## What `x86-64-v3` and `arm64` prove

### `x86-64-v3`

Compiled with `-O3 -march=x86-64-v3` (enabling SSE3–4.2, AVX, AVX2, BMI1/2, F16C, FMA, LZCNT, MOVBE, POPCNT, CMPXCHG16B, LAHF-SAHF, OSXSAVE) and run under `qemu-x86_64 -cpu Haswell-v4`. The check verifies three extra properties beyond the test hashes:

1. `test_isa --expect x86-64-v3` confirms all 15 compiler macros, CPUID flags, `xgetbv` XCR0 state, and inline-assembly execution of every instruction class (and `objdump` verifies every mnemonic is present in `test_isa`).
2. `objdump` confirms `libjitterbug.a` actually emits 256-bit `%ymm` AVX/AVX2 instructions (`grep -q '%ymm'`, logging the instruction count during the build).
3. `objdump` confirms `libjitterbug.a` contains **zero** fused multiply-add instructions (`vfn?m(add|sub)`), proving `-ffp-contract=off` prevented contraction. (In a manual experiment replacing `-ffp-contract=off` with `-ffp-contract=fast`, clang fused 55 multiply-adds in the perspective-correct and precise-GTE paths; the stored hashes still matched on those inputs, so `-ffp-contract=off` remains as a safeguard against inputs where FMA rounding would diverge.)

### `arm64` and `arm64-clang`

Cross-compiled for `aarch64-linux` with both gcc and clang and run under `qemu-aarch64` on an x86-64 host:

- **Integer path:** QEMU implements A64 integer semantics as specified, providing strong evidence that both compilers' aarch64 integer code produces identical pixels.
- **Float options and friendly maths:** under `qemu-aarch64`, floating-point rounding comes from QEMU's IEEE 754 emulation rather than physical arm64 silicon. The native run in an Apple silicon NixOS virtual machine (noted above) confirmed that physical arm64 silicon in default Linux FP mode produces the same hashes; other arm64 microarchitectures have not been tested directly.
- **Worker-pool memory ordering:** x86-64 has a stronger memory model than arm64, so running `qemu-aarch64` on an x86-64 host cannot catch an overly weak atomic memory order in `src/soft/workers.c`. The native `aarch64-linux` suite and 20 consecutive rebuilds of `checks.aarch64-linux.clang` in an Apple silicon NixOS virtual machine ran on physical arm64 cores without failure; as with any concurrency testing, passing runs are evidence rather than proof of absence of races, and non-Apple arm64 CPUs have not been exercised.

## What is not checked

- **Broader arm64 hardware:** the native `aarch64-linux` suite has passed on one chip family (Apple silicon, in a NixOS virtual machine); other arm64 CPUs have not been tested directly, and routine x86-64 checks still use `qemu-aarch64`.
- **Big-endian hosts** (refused at compile time).
- **macOS, Windows and MSVC** (not built or tested).
- **32-bit targets other than `wasm32`** (`i686`, `armv7`).
- **Web browsers in CI:** `examples/first-light/web/check-browser.mjs` and `examples/demo/web/check-browser.mjs` verify identical hashes in headless Chromium by hand (the Nix sandbox has no browser).
- **Multi-threaded WebAssembly** (not built).
- **Compiler versions outside the pinned flake.**

## Rerunning and updating hashes

```sh
nix flake check -L                                      # full suite across all checks
nix build -L .#checks.x86_64-linux.arm64                # one check
nix develop -c ctest --test-dir build -R determinism --output-on-failure
```

To test a native single-threaded build by hand:

```sh
nix develop -c sh -c 'cmake -S . -B build-nothreads -G Ninja -DJB_THREADS=OFF \
  -DJB_EXAMPLES=ON -DJB_TOOLS=ON -DJB_VULKAN=OFF &&
  cmake --build build-nothreads && ctest --test-dir build-nothreads --output-on-failure'
```

When a deliberate rendering fix changes pixels (never to hide a platform or thread-count mismatch), see [CONTRIBUTING.md](../CONTRIBUTING.md) and [docs/testing.md](testing.md) for the re-blessing workflow. To print replacement hash lines for `tests/determinism_threads.txt` to `stdout` (the command does not modify the file in place, and still fails if thread counts disagree):

```sh
build/tests/test_determinism_threads --record tests/determinism_threads.txt tests/seed/*.jbt
```

# Changelog

Notable changes to jitterbug. There is no ABI stability guarantee before 1.0.

## 0.1.0 — 2026-11-25

First public release of jitterbug, a standalone C library that renders 3D and 2D graphics using the original PlayStation GPU's rules, for new games and homebrew.

### Renderer and API

- **Software backend (`libjitterbug`):** 1024x512 16-bit VRAM, affine texture mapping, vertex snapping, 15-bit drawing colour with 4x4 dithering, 4- and 8-bit palette textures with hardware CLUT caching, all four semi-transparency blend modes, mask bits, 24-bit display mode, and internal resolution scaling from 1x to 8x with deterministic multi-threading (or single-threaded with `JB_THREADS=OFF`).
- **Single C99/C++11 public header ([include/jitterbug.h](include/jitterbug.h)):** opaque `jb_context`, no global state, custom allocator and log callbacks, and sticky error reporting, with two API levels sharing one VRAM:
  - *Friendly level:* double-buffered frames in VRAM, automatic texture and palette placement, deterministic float maths (`jb_sinf`, `jb_cosf`, `jb_tanf`, vectors and matrices), camera, up to three directional lights plus ambient, depth-cue fog, GTE fixed-point or float geometry path, near/far and screen-space polygon clipping, meshes, billboards, 3D lines, sprites and an original 8x8 bitmap font.
  - *Console level:* streamable `jb_gp0`, `jb_gp1`, `jb_gpuread`, `jb_gpustat`, VRAM transfers (`jb_vram_load`, `jb_vram_read`, `jb_vram_fill`, `jb_vram_copy`), immediate primitives, ordering tables and all 22 documented GTE commands (`jb_gte_*`).
- **Runtime switches:** toggle internal scale (1x–8x), precise sub-pixel vertices, perspective-correct texturing, 4x4 dithering, the CLUT cache, 16:9 widescreen and GTE vs float geometry on the fly without recreating the context.

### Asset formats and tools

- **Binary formats and loaders ([docs/formats.md](docs/formats.md)):** `.jbt` textures and `.jbm` meshes with strict untrusted-input loaders (`jb_texture_parse`, `jb_texture_load*`, `jb_mesh_load*`).
- **Deterministic CLI tools ([docs/tools.md](docs/tools.md)):** `jb-tex` (PNG to `.jbt` with median-cut quantisation, Floyd-Steinberg dithering and palette-animation frames), `jb-mesh` (Wavefront OBJ/MTL and glTF 2.0 to `.jbm`) and `jb-bench` (renderer benchmarks).

### Examples and documentation

- **Examples:** [examples/first-light](examples/first-light/README.md) and [examples/demo](examples/demo/README.md) (native SDL3, headless and WebAssembly, published live at [https://jardarton.github.io/jitterbug/](https://jardarton.github.io/jitterbug/)), plus [examples/header-images](examples/header-images/README.md) drawing the project's animated beetle logo, favicon and 1x/4x banner through the public API.
- **Documentation:** [README.md](README.md), [FAQ.md](FAQ.md), [SECURITY.md](SECURITY.md), [CONTRIBUTING.md](CONTRIBUTING.md), [todo.md](todo.md), and guides in `docs/` covering getting started, API tour, design, formats, tools, conformance, hardware probe, determinism, 0.1.0 desktop and WebAssembly benchmark figures ([docs/performance.md](docs/performance.md)) and testing.

### Verification, packaging and platforms

- **Console conformance ([docs/conformance.md](docs/conformance.md)):** reproduces the published real-console results from [ps1-tests](https://github.com/JaCzekanski/ps1-tests) for 13 named GPU programs (11 VRAM dumps on their 15 colour bits, excluding 64 uninitialised-colour pixels in `lines`, plus 2 console logs, at 1x and in the 1x VRAM of 2x/3x contexts) and all 1150 distinct GTE cases (`gte_test_all`). jitterbug's own freestanding MIPS hardware probe ([docs/probe.md](docs/probe.md)) is built and verified in simulation and PCSX-Redux, not yet run on a real console.
- **Determinism and fuzzing ([docs/determinism.md](docs/determinism.md), [docs/testing.md](docs/testing.md)):** checked-in hashes match across GCC and Clang, `-O0` and `-O3`, `-march=x86-64-v3` under QEMU Haswell-v4 (`-ffp-contract=off`), static and shared builds, ASan + UBSan, `wasm32` under Node, `aarch64` cross-builds under QEMU and 1–16 rasterising threads; differential fuzzing against `psxstack` (`tools/fuzz-diff`), API/command-stream crash fuzzing (`fuzz_robust`) and file-loader fuzzing (`fuzz_files`, `fuzz_files_libfuzzer`).
- **Packaging and platforms:** relocatable CMake package (`find_package(jitterbug 0.1)`, target `jitterbug::jitterbug`), `pkg-config` (`jitterbug.pc`), `add_subdirectory` support and a Nix flake. Supported in 0.1.0 on Linux x86-64, Linux arm64 (tested under QEMU) and WebAssembly (Emscripten, single-threaded); macOS and Windows are not yet built or tested.

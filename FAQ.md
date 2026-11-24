# Frequently asked questions

Short answers to common questions about jitterbug, with links to the full documentation.

## What is jitterbug?

jitterbug is a standalone C library that renders 3D and 2D graphics using the original PlayStation GPU's integer rasterisation rules, 1024x512 16-bit VRAM and fixed-point GTE geometry, designed for new games, prototypes and homebrew. It provides both a friendly 3D/2D API and a console-level GP0/GP1/GTE API in one C99 header ([include/jitterbug.h](include/jitterbug.h)), and returns an RGBA8 frame buffer. You can try the live WebAssembly demo at [https://jardarton.github.io/jitterbug/](https://jardarton.github.io/jitterbug/).

## Is it a game engine?

No. jitterbug is a renderer only: it has no windowing, input, audio, physics, networking or scene graph. You bring your own game loop and upload the RGBA8 buffer returned by `jb_frame` wherever you want to display it. See [docs/design.md](docs/design.md).

## Why a renderer library rather than a shader?

Post-processing filters and vertex-snapping shaders imitate individual quirks on top of a modern floating-point GPU pipeline, whereas jitterbug implements the console's actual rules: fill convention, 12-bit integer attribute gradients, affine texturing, 15-bit colour with 4x4 dithering, 4- and 8-bit palette textures with a hardware CLUT cache, the four semi-transparency blend modes, mask bits, back-to-front depth sorting and shared 1024x512 VRAM. That makes features such as palette animation, render-to-texture and console-level GP0/GP1 streams behave the same way they do on the hardware, and produces identical pixels across platforms. See [docs/design.md](docs/design.md) and [docs/determinism.md](docs/determinism.md).

## How exact is it to a real PlayStation?

At 1x with non-console enhancements off, jitterbug reproduces the real-console results published by [ps1-tests](https://github.com/JaCzekanski/ps1-tests) for 13 named GPU programs — 11 VRAM dumps compared pixel for pixel on the 15 colour bits they carry (excluding 64 uninitialised-colour pixels in `lines`) plus 2 console logs (`mask-bit` and `gp0-e1`) — and all 1150 distinct GTE cases (`gte_test_all`). That is strong evidence rather than proof of full hardware equivalence: jitterbug's own freestanding hardware probe (which checks all 16 VRAM bits, including the mask bit) has not yet been run on a physical console, and above 1x there is no hardware to compare against, so scaled output (2x–8x) is faithful to the console's rules rather than exact. See [docs/conformance.md](docs/conformance.md) and [docs/probe.md](docs/probe.md).

## What are the runtime switches?

You can toggle seven rendering options on a live context without recreating it: internal resolution (`1x`–`8x`), precise sub-pixel vertices (above 1x), perspective-correct texturing, 4x4 dithering (at 1x), the hardware CLUT cache, 16:9 widescreen (when reserved at context creation) and fixed-point GTE vs deterministic float geometry. All non-console enhancements start off so a fresh context matches the console's look (with the CLUT cache on and dithering off, as after a hardware reset). In 0.1, 15-bit drawing colour, shared 1024x512 VRAM and depth sorting have no switches. See [docs/api.md](docs/api.md).

## Can I use it with SDL, raylib, Godot or a browser canvas?

Yes. `jb_frame` hands back a pointer to tightly packed `R, G, B, A` bytes (`A = 255`) with no windowing or graphics-API dependency, so you can upload it straight into an SDL3 streaming texture, a raylib texture, a Godot `ImageTexture`, a browser canvas `ImageData`, or any other surface. Both [examples/demo](examples/demo/README.md) and [examples/first-light](examples/first-light/README.md) include SDL3 and WebAssembly canvas front-ends, and a raylib example is planned in [todo.md](todo.md). See [docs/getting-started.md](docs/getting-started.md).

## Does it run on a real PlayStation?

No. jitterbug runs on modern CPUs and in WebAssembly; it does not compile to or target PlayStation hardware. A separate backend implementing the low-level calls on real PlayStation hardware via PSn00bSDK is only a post-0.1 idea in [todo.md](todo.md).

## Is there a Vulkan or GPU backend?

Not in 0.1.0, which ships only the CPU software backend (1x–8x, multi-threaded on native builds). The core already talks to the renderer through an internal backend boundary (`src/core/backend.h`), and a Vulkan backend supporting scales up to 16x is planned after 0.1. See [docs/design.md](docs/design.md) and [todo.md](todo.md).

## Which platforms are supported?

In 0.1.0, jitterbug is built and tested on Linux x86-64, Linux arm64 (cross-compiled and tested under QEMU; no physical arm64 hardware has run the suite yet) and WebAssembly (Emscripten, single-threaded). macOS and Windows have not yet been built or tested — the core is plain C11 and builds without a thread library under `JB_THREADS=OFF`, so adding them requires a Win32 thread shim and MSVC fallbacks for GNU extensions (`__int128` and `__attribute__`). See [docs/determinism.md](docs/determinism.md) and [todo.md](todo.md).

## What are the dependencies?

`libjitterbug` itself has no third-party runtime dependencies: only the C standard library, `-lm` and POSIX threads (or no thread library at all with `-DJB_THREADS=OFF`, which is automatic under Emscripten). Vendored headers under `third_party/` (`stb`, `cgltf`) are used only by the CLI converters, tests and headless examples, not linked into the library; SDL3 is optional and used only for the windowed examples. See [docs/getting-started.md](docs/getting-started.md).

## Why C?

A strict C99 public header (`include/jitterbug.h`, also valid C++11) over a small C11 core keeps runtime dependencies at zero, makes memory layout and integer arithmetic explicit, compiles cleanly to WebAssembly, and embeds easily in C, C++ or via FFI from other languages. Idiomatic bindings for Rust, Zig and Odin are noted for once the API stabilises towards 1.0 ([todo.md](todo.md)).

## How fast is the software renderer?

On a desktop x86-64 CPU (AMD Ryzen 7 7700X), the synthetic `heavy` workload (~1,674 primitives/frame) stays above 60 fps up to 4x on 1 thread (10.8 ms) and 8x on 4+ threads (11.2 ms on 4 threads), and holds 60 fps up to 3x in single-threaded WebAssembly under Node (9.0 ms). Actual frame times depend on resolution, overdraw and runtime options; see [docs/performance.md](docs/performance.md) for full tables and how to run `jb-bench` on your own machine.

## How do I load textures and 3D models?

You can supply `jb_texture_desc` and `jb_mesh` structs directly from code, or convert PNGs and Wavefront OBJ / glTF 2.0 models offline into jitterbug's binary `.jbt` and `.jbm` formats using the bundled deterministic converters `jb-tex` and `jb-mesh`, then load them with `jb_texture_load*` and `jb_mesh_load*`. See [docs/tools.md](docs/tools.md) and [docs/formats.md](docs/formats.md).

## Is the API or ABI stable?

Not yet. There is no ABI stability promise before 1.0: CMake package matching uses `SameMinorVersion` (`find_package(jitterbug 0.1)` matches `0.1.x`) and the shared library uses soversion `0.1`. See [docs/getting-started.md](docs/getting-started.md) and [CHANGELOG.md](CHANGELOG.md).

## Can I use jitterbug commercially?

Yes. jitterbug is licensed under the permissive Zlib licence ([LICENSE](LICENSE)), which allows commercial and non-commercial use, modification and redistribution with no copyleft obligations. Vendored dependencies under `third_party/` keep their own licences (and are not linked into `libjitterbug`).

## Can I use assets or code from existing PlayStation games?

No, and jitterbug contains none. Every example scene, texture, mesh, bitmap font and converter test asset in this repository is original (the conformance suite includes only MIT-licensed real-console test results from `ps1-tests`). You are solely responsible for holding the rights to any assets and code you use in your own projects.

## Is jitterbug affiliated with Sony?

No. "PlayStation" is a trademark of Sony Interactive Entertainment. jitterbug is an independent open-source project written clean-room from public hardware documentation ([psx-spx](https://psx-spx.consoledev.net/graphicsprocessingunitgpu/)).

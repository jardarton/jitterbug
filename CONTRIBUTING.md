# Contributing to jitterbug

This page covers the project's philosophy, how to report bugs, how to build and check jitterbug, the rules every change must follow, how the repository is organised and how golden files are updated.

## Philosophy

jitterbug has a narrow, deliberate goal: render 3D and 2D graphics by the original PlayStation GPU's rules in a small, self-contained C library. Changes are judged against five principles:

- **The console's evidenced rules first.** At 1x, hardware behaviour comes from real-console dumps first and [psx-spx](https://psx-spx.consoledev.net/graphicsprocessingunitgpu/) second, never from guesswork or emulator source code ([docs/conformance.md](docs/conformance.md)).
- **Every enhancement behind a switch, defaulting to the console.** All non-console enhancements (`scale > 1`, `precise_vertices`, `perspective_correct`, `widescreen`, `JB_GEOMETRY_FLOAT`) are off by default, the hardware CLUT cache starts on (`clut_cache_off = 0`), and dithering starts off (`jb_set_dither(jb, 0)`) as after a hardware `GP1(00h)` reset, so a fresh context matches the console ([docs/api.md](docs/api.md)). Enhancements must not alter 1x console behaviour when off.
- **A small, stable C99 API with no global state.** The public interface is one strict C99 / C++11 header ([include/jitterbug.h](include/jitterbug.h)) with opaque contexts, zero writable global state and no third-party runtime dependencies. jitterbug stays a renderer, not an engine or emulator ([docs/design.md](docs/design.md)).
- **Same pixels everywhere.** The 1x console path uses integer arithmetic only, and the friendly layer and float options use deterministic IEEE 754 maths (`-ffp-contract=off`), so identical calls produce identical pixels across compilers, architectures and thread counts ([docs/determinism.md](docs/determinism.md)).
- **Original assets only.** Everything authored for this repository is original; no commercial game assets, names or game-specific workarounds belong here.

Contributions that fit these principles — evidence-backed conformance fixes, bug fixes with regression tests, platform support, documentation improvements and items listed in [todo.md](todo.md) — are welcome. Changes that pull in engine features, add runtime dependencies, trade away determinism, break 1x console rules or include non-original assets will be declined.

## Reporting a bug

Before opening an issue, check [FAQ.md](FAQ.md) and [todo.md](todo.md) to see whether the behaviour is an intentional limitation or planned work. For security vulnerabilities (such as crashes or out-of-bounds reads on untrusted `.jbt`/`.jbm` files or GP0/GP1 streams), report them privately following [SECURITY.md](SECURITY.md) rather than opening a public issue.

A useful bug report includes:

- **Version:** `jb_version_string()` (or the release tag / commit; see [CHANGELOG.md](CHANGELOG.md)).
- **Platform and build:** operating system, CPU architecture, compiler and version, and CMake flags (such as `CMAKE_BUILD_TYPE`, `JB_THREADS`, `JB_SANITIZE` or Emscripten).
- **Renderer settings:** internal `scale`, active runtime switches (`precise_vertices`, `perspective_correct`, `dither`, `clut_cache`, `widescreen`, `geometry`) and thread count (`threads`).
- **Minimal reproducer:** the smallest C program, asset or GP0/GP1 command list that triggers the problem.
- **Observed output:** a screenshot, PNG or frame hash alongside what you expected to see.
- **Hardware evidence (for conformance bugs):** a real-console VRAM dump or log (for example from `ps1-tests` or [docs/probe.md](docs/probe.md)), or the relevant section of [psx-spx](https://psx-spx.consoledev.net/graphicsprocessingunitgpu/).

## Building

Development uses a pinned Nix shell so every contributor builds with the same toolchains and libraries. Run build commands inside `nix develop`:

```sh
nix develop -c cmake -S . -B build -G Ninja -DJB_TOOLS=ON -DJB_EXAMPLES=ON
nix develop -c cmake --build build
nix develop -c ctest --test-dir build --output-on-failure
```

You can also configure and build directly with CMake 3.24+ and a C11 compiler (GCC or Clang) outside Nix. Plain builds test the ten checked-in conformance programs; to include the three fetched-only programs as well, pass `-DJB_CONFORMANCE_FETCHED` pointing at the output of `nix build .#conformance-lists` (see [docs/testing.md](docs/testing.md)).

### CMake options

| Option | Default | Effect |
|---|---|---|
| `JB_TESTS` | `ON` (top-level) | Build and register the CTest suite |
| `JB_TOOLS` | `OFF` | Build `jb-bench`, `jb-tex` and `jb-mesh` (`jb-tex` and `jb-mesh` are also built when `JB_TESTS=ON`) |
| `JB_EXAMPLES` | `OFF` | Build `examples/first-light` and `examples/demo` (SDL3 windows when SDL3 is found) |
| `JB_SANITIZE` | `OFF` | Enable AddressSanitizer, UndefinedBehaviorSanitizer and `float-cast-overflow` (with install rules enabled — `JB_INSTALL=ON` at the root, and `JB_INSTALL=ON` with `JB_TOOLS=ON` for tools — refuses `cmake --install` before copying any files, including when installing a tool directory alone) |
| `JB_WERROR` | `OFF` | Treat compiler warnings as errors |
| `JB_THREADS` | `ON` | Enable worker threads for rasterisation (forced `OFF` under Emscripten) |
| `JB_INSTALL` | `ON` (top-level) | Install the library, public header, CMake package, `jitterbug.pc` and licences |
| `JB_VULKAN` | `AUTO` | `AUTO`, `ON` or `OFF`; currently controls only the lavapipe smoke test |
| `JB_CONFORMANCE_FETCHED` | empty | Path to `nix build .#conformance-lists`, adding the three fetched-only conformance tests |
| `JB_LIBFUZZER` | `OFF` | Build and run the file loaders' libFuzzer target (Clang only) |
| `JB_REQUIRE_PTY` | `OFF` | Fail rather than skip the pseudo-terminal probe tests when no PTY is available |

For a WebAssembly build with Emscripten:

```sh
nix develop -c emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DJB_TESTS=ON -DJB_EXAMPLES=ON -DJB_TOOLS=ON -DJB_VULKAN=OFF
nix develop -c cmake --build build-web
nix develop -c ctest --test-dir build-web --output-on-failure
```

## Checks and CI

Before committing or opening a pull request, run the full local check:

```sh
nix flake check -L
```

This builds and tests the library across Clang, GCC, ASan+UBSan (with the libFuzzer run and install refusal), a shared library (checking exported symbols), `-O0`, WebAssembly under Node, the `first-light` and `demo` web pages in headless Chromium (`browser-first-light`, `browser-demo`), `x86-64-v3` and `aarch64` (GCC and Clang) under QEMU (on x86-64 hosts), re-records and verifies the conformance lists and GTE cases, runs differential fuzzing against `psxstack`, builds and verifies the hardware probe's MIPS I executable, and tests the source release tarball and installed packages (`find_package`, `pkg-config` and `add_subdirectory`). See [docs/testing.md](docs/testing.md) and [docs/determinism.md](docs/determinism.md) for details.

Hosted CI runs a GitHub Actions workflow on pushes to `main`, on pull requests and by manual dispatch that builds and tests a small subset of the flake's checks (the Clang build and the WebAssembly build), and publishes the demo page to [https://jardarton.github.io/jitterbug/](https://jardarton.github.io/jitterbug/) from `main`. The full `nix flake check -L` stays the local check.

## Project rules

Every change must keep these rules:

1. **Generic only.** No code, assets, names or data from any specific commercial game. Demo scenes and test assets are original.
2. **Clean-room hardware implementation.** Code is written from public hardware documentation ([psx-spx](https://psx-spx.consoledev.net/graphicsprocessingunitgpu/)). Never copy code from emulators under copyleft or non-commercial licences (the Mednafen/Beetle core, PCSX-Redux, DuckStation, SwanStation). Their output may be used as a test reference.
3. **Vendored code.** Third-party code lives under `third_party/<name>/` with its own `LICENSE` file, and that directory's `README.md` records its origin, version, licence and any local changes.
4. **Public API.** The public API is [include/jitterbug.h](include/jitterbug.h): strict C99 (and valid C++11+), prefix `jb_`, opaque context, no global writable state.
5. **Software backend as reference.** The software backend is the reference renderer. Changes to it must keep the conformance and golden-image tests identical, unless the change is a deliberate fix — in which case the tests and goldens are updated in the same commit with their evidence, and an entry is added to [tests/seed/CHANGES.md](tests/seed/CHANGES.md) whenever the seed traces change.
6. **Integer-only exact path and order of authority.** The exact 1x path uses integer arithmetic only. Never enable `-ffast-math` (the build rejects it). Real-hardware dumps outrank psx-spx, and psx-spx outranks any emulator (see [docs/conformance.md](docs/conformance.md)).
7. **Pinned shell and full check.** Build inside `nix develop -c ...` and run `nix flake check -L` before committing or opening a pull request.

Documentation uses British English (colour, rasterise, licence as a noun). The library is licensed under the Zlib licence ([LICENSE](LICENSE)); code under `third_party/` keeps its own notices.

## Repository layout

```
include/jitterbug.h     public C99/C++ API (the API reference)
src/core/               context, GP0 state and command decode, backend interface, VRAM layout and placer
src/soft/               software backend: 1x and scaled VRAMs, rasterisers, transfers, worker pool, scanout
src/geom/               GTE, deterministic float maths, 3D pipeline, screen-space splitter, scene state
src/api/                jb_* entry points, 2D/3D friendly layer, bitmap font, .jbt/.jbm loaders
cmake/                  installed CMake config and pkg-config templates
tools/                  jb-tex, jb-mesh, jb-bench, plus test/conformance/fuzzing/probe tools
examples/               first-light and demo (SDL3, headless, and WebAssembly pages)
tests/                  unit tests, seed traces, conformance fixtures, GTE cases, file/package tests
third_party/            vendored code and console test data (cgltf, stb, ps1-tests)
docs/                   guides, design, conformance, determinism, performance, testing, probe
flake.nix               pinned toolchains, packages and full check suite
CMakeLists.txt          top-level CMake build and install rules
```

## Re-blessing golden files

Never re-bless golden images or stored hashes to silence a compiler, optimisation level, thread count or refactoring failure. Re-blessing is permitted only for a deliberate fix that is meant to change pixels, backed by evidence (a console dump, or psx-spx where none exists).

Before re-blessing the seed goldens in `tests/seed/`:

1. Measure the pixel-level change between the build before the fix and the build after it with `tools/seed-record/measure.sh`, and verify every changed pixel falls within what the fix is allowed to change.
2. Record the evidence and per-scene accounting in [tests/seed/CHANGES.md](tests/seed/CHANGES.md).
3. Run `tools/seed-record/rebless.sh <build-dir>` to rewrite and verify a copy of the traces, PNGs and scaled references, then install it into `tests/seed/` by moving the old directory aside and renaming the staged copy into place (rolling back if the second rename fails, or reporting the backup path if rollback fails too).
4. Update any other affected stored hashes (determinism, example scenes, benchmark checks) and commit the code, updated goldens and `CHANGES.md` entry together.

See [docs/testing.md](docs/testing.md) for the exact commands for every test fixture and stored hash file.

# Todo

Known gaps in 0.1 and planned work after 0.1.

## Known gaps in 0.1

- **Run the hardware probe on a real console.** `tools/probe` ([docs/probe.md](docs/probe.md)) has been verified in host simulation and in PCSX-Redux, not yet on physical PlayStation hardware. Running it on a real console (with the operator recording the physical console model and mainboard revision alongside the GPU revision and BIOS strings the probe reports) will check all 16 VRAM bits (including the mask bit), settle the seven open/confirmation lists in `tests/conformance/open/` (and further targeted lists for testable assumptions in [docs/conformance.md](docs/conformance.md)), and widen the conformance claim to what those runs verify.
- **Automated windowed and browser checks.** The SDL3 windows (`examples/demo`, `examples/first-light`) are only smoke-tested headless (`SDL_VIDEODRIVER=dummy`), and the headless Chromium checks (`examples/demo/web/check-browser.mjs`, `examples/first-light/web/check-browser.mjs`) run by hand outside `nix flake check`.
- **`jb-mesh` OBJ reader limits.** `tools/jb-mesh/obj.c` fans polygons with more than 4 corners from the first vertex (incorrect for concave n-gons), only joins backslash line continuations before LF (`\n`, not CRLF `\r\n`), and does not strip inline `#` comments on `mtllib` or `usemtl` lines.
- **Extra frame records for cut polygons in the precise pass.** When a clipped polygon has a vertex whose sub-pixel position lies more than a pixel from its whole-pixel position, the splitter emits a second set of records for the precise-vertices pass at any scale (because switches can change before `jb_end_frame`), consuming extra capacity from `jb_config.max_primitives` (documented in [include/jitterbug.h](include/jitterbug.h)).
- **Documented geometry and splitter limits.** As recorded in [include/jitterbug.h](include/jitterbug.h) and [src/geom/split.h](src/geom/split.h):
  - Polygons cannot fill column 1023 of a 1024-wide frame (hardware coordinates stop at 1023 and the fill rule omits right edges), nor column 1022 in GTE mode for a face whose own edge lies on the guard band's right edge.
  - Clipped slivers less than about one unit wide can lose pixels or be dropped rather than fold over.
  - The splitter's no-overlap guarantee is a geometric argument backed by fuzzing (`split_fuzz`), not a formal proof.
- **Web build headroom at 3x.** In the single-threaded Emscripten build without SIMD, `examples/demo` takes ~16.7 ms/frame at 3x under Node (before canvas upload), right at the edge of 60 fps on a fast desktop core. Enabling WASM SIMD or speeding up the hot loops would give comfortable headroom at 3x.
- **Relocatability with absolute `CMAKE_INSTALL_LIBDIR`/`INCLUDEDIR`.** When an install is configured with absolute `CMAKE_INSTALL_LIBDIR` or `CMAKE_INSTALL_INCLUDEDIR` paths inside `CMAKE_INSTALL_PREFIX`, CMake's exported `jitterbugTargets.cmake` keeps those absolute paths, so moving the prefix breaks `find_package(jitterbug)` (`jitterbug.pc` relativises them under `${pcfiledir}` and continues to work after a move).
- **Installing a `JB_SANITIZE=ON` static library.** An installed static library built with `-DJB_SANITIZE=ON` does not propagate sanitizer link flags to consumers, so linking through `find_package(jitterbug)` fails with undefined `__asan_*` symbols. The sanitizer configuration is for running the project's own test suite, not for installation.

## Planned after 0.1

Roughly in priority order:

- **Vulkan backend (`jitterbug_vk`).** A GPU backend behind `jbi_backend` (`src/core/backend.h`) built on MIT/ISC-licensed `parallel-psx` in a separate C++ library (keeping the core pure C), loaded dynamically with `volk` and falling back cleanly to software when Vulkan is unavailable. Would add internal scales 1, 2, 4, 8 and 16 (refusing unsupported factors rather than rounding), cross-backend tolerance tests on Mesa lavapipe (and manual verification on radv, anv and Nvidia), and a `VkImage` hand-off for zero-copy presentation.
- **macOS and Windows support.** Build and test on both platforms, add a Win32 thread shim in `src/soft/workers.c`, and replace or guard the GNU extensions used in the core for MSVC: a portable 128-bit integer fallback in `src/soft/raster_hires.c` and `src/geom/split.c` (`__int128`), and portable macros for `__attribute__((...))` (`noinline`, `always_inline`, `format`).
- **Automatic polygon subdivision.** Optional subdivision of large polygons to tame affine texture wobble when perspective-correct texturing is off.
- **True-colour drawing.** Render directly at 24/32-bit colour depth (natural on a Vulkan backend whose scaled VRAM is RGBA8; on software requires a second pixel path).
- **Draw-only frames and transform interpolation helpers.** A draw-only frame mode that restores VRAM afterwards, paired with camera/transform blending helpers (with a cut threshold) for high-refresh-rate presentation.
- **`jb-view`.** A standalone viewer for `.jbt` textures and `.jbm` meshes with interactive controls for every runtime switch.
- **A raylib example.** Demonstrate embedding `jb_frame` inside a raylib loop alongside the existing SDL3 and browser examples.
- **Depth buffer and higher precision on the Vulkan backend.** Optional Z-buffer and higher vertex/colour precision when running on Vulkan.
- **MDEC-style video decoder.** A decoder for full-motion video matching the console's 15-bit/24-bit look.
- **Language bindings.** Idiomatic bindings for Rust, Zig and Odin once the C API stabilises towards 1.0.
- **Real-PS1 backend via PSn00bSDK.** A separate implementation of the low-level calls targeting actual PlayStation hardware directly.

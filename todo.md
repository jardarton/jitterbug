# Todo

Known gaps in 0.1 and planned work after 0.1.

## Known gaps in 0.1

What stands between 0.1.0 and a finished 0.1: bugs, and checks that the release's own claims still lack. Each item closes without adding a feature, and says what closing it means. Behaviour that is a limit by design is not listed here; it is recorded in [README.md](README.md#limitations) and [include/jitterbug.h](include/jitterbug.h).

- **`jb-mesh` OBJ reader bugs.** `tools/jb-mesh/obj.c` fans polygons with more than 4 corners from the first vertex (wrong for concave n-gons), joins backslash line continuations only before LF (not CRLF), and does not strip inline `#` comments on `mtllib` and `usemtl` lines. Closed when each is fixed, or refused with a clear error, and covered by a test.
- **Browser checks inside `nix flake check`.** The headless Chromium checks (`examples/demo/web/check-browser.mjs`, `examples/first-light/web/check-browser.mjs`) are run by hand, so the published web demo is the one shipped front-end no automated check runs. Closed when both run as a flake check.

## Planned after 0.1

Roughly in priority order:

- **Run the hardware probe on a real console.** `tools/probe` ([docs/probe.md](docs/probe.md)) has been verified in host simulation and in PCSX-Redux, not on a PlayStation; 0.1 states this wherever it claims conformance. A run on a console (the operator recording its model and mainboard revision alongside the GPU revision and BIOS strings the probe reports) would check all 16 VRAM bits, the mask bit included, settle the seven open and confirmation lists in `tests/conformance/open/` and the testable assumptions in [docs/conformance.md](docs/conformance.md), and widen the conformance claim to what the runs verify.
- **Vulkan backend (`jitterbug_vk`).** A GPU backend behind `jbi_backend` (`src/core/backend.h`) built on MIT/ISC-licensed `parallel-psx` in a separate C++ library (keeping the core pure C), loaded dynamically with `volk` and falling back cleanly to software when Vulkan is unavailable. Would add internal scales 1, 2, 4, 8 and 16 (refusing unsupported factors rather than rounding), cross-backend tolerance tests on Mesa lavapipe (and manual verification on radv, anv and Nvidia), and a `VkImage` hand-off for zero-copy presentation.
- **macOS and Windows support.** Build and test on both platforms, add a Win32 thread shim in `src/soft/workers.c`, and replace or guard the GNU extensions used in the core for MSVC: a portable 128-bit integer fallback in `src/soft/raster_hires.c` and `src/geom/split.c` (`__int128`), and portable macros for `__attribute__((...))` (`noinline`, `always_inline`, `format`).
- **Automatic polygon subdivision.** Optional subdivision of large polygons to tame affine texture wobble when perspective-correct texturing is off.
- **Faster WebAssembly build.** WASM SIMD or faster hot loops: the single-threaded build without SIMD takes about 16.7 ms a frame for `examples/demo` at 3x under Node, the edge of 60 fps on a fast desktop core ([docs/performance.md](docs/performance.md)).
- **True-colour drawing.** Render directly at 24/32-bit colour depth (natural on a Vulkan backend whose scaled VRAM is RGBA8; on software requires a second pixel path).
- **Draw-only frames and transform interpolation helpers.** A draw-only frame mode that restores VRAM afterwards, paired with camera/transform blending helpers (with a cut threshold) for high-refresh-rate presentation.
- **`jb-view`.** A standalone viewer for `.jbt` textures and `.jbm` meshes with interactive controls for every runtime switch.
- **A raylib example.** Demonstrate embedding `jb_frame` inside a raylib loop alongside the existing SDL3 and browser examples.
- **Depth buffer and higher precision on the Vulkan backend.** Optional Z-buffer and higher vertex/colour precision when running on Vulkan.
- **Compositing outputs (coverage alpha and depth export).** Optional scanout mode mapping VRAM bit 15 to alpha (`0`/`255`) in `jb_frame` for clean cutouts, plus an optional export-only per-pixel depth buffer for compositing jitterbug output into a host renderer without altering PS1 ordering-table rasterisation.
- **MDEC-style video decoder.** A decoder for full-motion video matching the console's 15-bit/24-bit look.
- **Language bindings.** Idiomatic bindings for Rust, Zig and Odin once the C API stabilises towards 1.0.
- **Real-PS1 backend via PSn00bSDK.** A separate implementation of the low-level calls targeting actual PlayStation hardware directly.

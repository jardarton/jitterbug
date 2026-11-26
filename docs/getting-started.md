# Getting started

This guide covers building and installing jitterbug, linking it into your project, and drawing your first frame.

## Requirements

- **To build the library:** a C11 compiler (gcc or clang on Linux x86-64/arm64, or Emscripten for WebAssembly) and CMake 3.24 or newer.
- **To call the library:** a C99 or C++11 compiler. The public header [include/jitterbug.h](../include/jitterbug.h) is strict C99 and compiles cleanly as C++.
- **Optional:** Ninja (faster builds), `pkg-config`, SDL3 (only for the windowed examples), and Nix (provides a pinned development shell with every tool).

## Building and installing

From a source tarball or checkout:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local -DJB_TESTS=OFF -DJB_TOOLS=ON
cmake --build build
cmake --install build
```

To build and run the test suite from a checkout instead, omit `-DJB_TESTS=OFF` and run `ctest --test-dir build --output-on-failure` (see [docs/testing.md](testing.md) and [CONTRIBUTING.md](../CONTRIBUTING.md)).

Useful CMake options (defined in [CMakeLists.txt](../CMakeLists.txt)):

| Option | Default | Effect |
|---|---|---|
| `BUILD_SHARED_LIBS` | `OFF` | Build `libjitterbug.so` instead of static `libjitterbug.a`. |
| `JB_THREADS` | `ON` (`OFF` under Emscripten) | Build the POSIX worker thread pool. `OFF` removes the thread-library dependency. |
| `JB_TOOLS` | `OFF` | Build and install `jb-tex`, `jb-mesh` and `jb-bench`. |
| `JB_EXAMPLES` | `OFF` | Build `first-light` and `demo` (headless, plus SDL3 windows if SDL3 is found). |
| `JB_TESTS` | `ON` (top-level), `OFF` (subproject) | Build and register the CTest suite. |
| `JB_INSTALL` | `ON` (top-level), `OFF` (subproject) | Install the library, header, CMake config, `jitterbug.pc`, licences and (with `JB_TOOLS`) tools. |

For the four install destinations (`CMAKE_INSTALL_LIBDIR`, `CMAKE_INSTALL_INCLUDEDIR`, `CMAKE_INSTALL_BINDIR` and `CMAKE_INSTALL_DATADIR`), relative paths always follow `cmake --install --prefix`, and if every absolute destination passes a lexical inside-prefix check against `CMAKE_INSTALL_PREFIX` (a mix of relative and absolute-inside-prefix destinations is allowed), the absolute ones are rewritten relative to the prefix so the install and its CMake package are relocatable. That rewrite is all-or-nothing across those four destinations: if any absolute destination fails the check, none of the absolute destinations is rewritten. Independently, `jitterbug.pc` derives `prefix` from `${pcfiledir}` and expresses eligible library and header paths relative to `${prefix}` when they pass the same check. The containment check rejects `..` in either path rather than resolving it. (Verified by `tests/package/check.cmake` in the `package` flake check.)

A build configured with `-DJB_SANITIZE=ON` is for running jitterbug's own tests and cannot be installed: with install rules enabled (the root with `JB_INSTALL=ON`, and each tool with `JB_INSTALL=ON` and `JB_TOOLS=ON`), a sanitizer build refuses installation before copying any files, including when installing a tool directory alone, because the sanitizer link flags do not travel with the installed library.

## Using jitterbug in your project

Every integration method below is tested against an installed build by [tests/package/check.cmake](../tests/package/check.cmake) and [tests/package/CMakeLists.txt](../tests/package/CMakeLists.txt).

### CMake `find_package`

```cmake
cmake_minimum_required(VERSION 3.24)
project(my_game LANGUAGES C)

find_package(jitterbug 0.1 REQUIRED)

add_executable(my_game main.c)
target_link_libraries(my_game PRIVATE jitterbug::jitterbug)
set_target_properties(my_game PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
```

If jitterbug was installed to a custom prefix, pass `-DCMAKE_PREFIX_PATH=<prefix>` when configuring your project. There is no ABI stability promise before 1.0: CMake package version matching uses `SameMinorVersion` (so `0.1.x` satisfies `find_package(jitterbug 0.1)` while `0.2` and `1.0` do not), and the shared library uses soversion `0.1`.

### pkg-config

Export `PKG_CONFIG_PATH` pointing at the installed `pkgconfig` directory before running `pkg-config` (`lib/pkgconfig` or `lib64/pkgconfig` depending on `GNUInstallDirs`; an inline assignment on the `cc` command line would not apply to `$(pkg-config ...)`):

```sh
export PKG_CONFIG_PATH="<prefix>/lib/pkgconfig"   # or <prefix>/lib64/pkgconfig
```

For a shared library install (passing `-Wl,-rpath,...` so the binary finds `libjitterbug.so` from a custom prefix outside the dynamic loader's default search path, as `tests/package/check.cmake` does):

```sh
cc -std=c99 main.c $(pkg-config --cflags --libs jitterbug) \
  -Wl,-rpath,"$(pkg-config --variable=libdir jitterbug)" -o my_game
```

For a static library install (adds `-lm` and the thread library from `Libs.private`):

```sh
cc -std=c99 main.c $(pkg-config --cflags --libs --static jitterbug) -o my_game
```

### CMake `FetchContent` or `add_subdirectory`

Fetch jitterbug directly from CMake with `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(jitterbug
  GIT_REPOSITORY https://github.com/jardarton/jitterbug.git
  GIT_TAG v0.1.2
)
FetchContent_MakeAvailable(jitterbug)

add_executable(my_game main.c)
target_link_libraries(my_game PRIVATE jitterbug::jitterbug)
```

Or drop the jitterbug source tree into your repository and add it as a subproject:

```cmake
add_subdirectory(third_party/jitterbug)

add_executable(my_game main.c)
target_link_libraries(my_game PRIVATE jitterbug::jitterbug)
```

When built as a subproject, `JB_TESTS`, `JB_TOOLS`, `JB_EXAMPLES` and `JB_INSTALL` all default to `OFF`, so only the `jitterbug` library target is built and nothing is added to your install rules.

### Nix

jitterbug provides a Nix flake (`flake.nix`) for `x86_64-linux` and `aarch64-linux`:

- `packages.default` builds and installs the static library, header, CMake package, `jitterbug.pc` and tools (`jb-tex`, `jb-mesh`, `jb-bench`). Add it to a derivation's `buildInputs` and consume it through `find_package(jitterbug 0.1)` or `pkg-config` (verified by the `package-nix` flake check).
- `devShells.default` (`nix develop`) provides CMake, Ninja, clang, pkg-config, SDL3, Emscripten and Node.
- `packages.demo-web` and `packages.first-light-web` build the ready-to-serve browser pages; `packages.bench` builds an optimised `jb-bench`; `packages.source` builds the reproducible source tarball.

### WebAssembly (Emscripten)

Configure with `emcmake`:

```sh
emcmake cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DJB_TESTS=OFF -DJB_VULKAN=OFF
cmake --build build-wasm
```

Under Emscripten, `JB_THREADS` is forced `OFF` so the output runs in any browser tab without requiring cross-origin isolation headers. When using `find_package(jitterbug 0.1)` with Emscripten, pass both `-DCMAKE_PREFIX_PATH=<prefix>` and `-DCMAKE_FIND_ROOT_PATH=<prefix>`.

## Verifying an install

This minimal C99 program is [tests/package/consumer.c](../tests/package/consumer.c), which `tests/package/check.cmake` builds and runs against every install (static, shared, Nix and wasm32):

```c
#include <jitterbug.h>
#include <stdio.h>

int main(void)
{
    jb_result r = JB_OK;
    jb_context *jb = jb_create(NULL, &r);
    uint16_t pixel = 0;
    if (!jb) {
        printf("jb_create: %s\n", jb_result_string(r));
        return 1;
    }
    if (jb_version() != JB_VERSION) {
        printf("library %s, header %u.%u.%u\n", jb_version_string(), JB_VERSION_MAJOR,
               JB_VERSION_MINOR, JB_VERSION_PATCH);
        jb_destroy(jb);
        return 1;
    }
    if (jb_vram_fill(jb, 0, 0, 16, 1, JB_RGB(255, 0, 0)) != JB_OK ||
        jb_vram_read(jb, 3, 0, 1, 1, &pixel) != JB_OK || pixel != 0x001f) {
        printf("pixel 0x%04x, expected 0x001f: %s\n", (unsigned)pixel, jb_error_string(jb));
        jb_destroy(jb);
        return 1;
    }
    jb_destroy(jb);
    printf("jitterbug %s: ok\n", jb_version_string());
    return 0;
}
```

## Drawing your first frame

Using the friendly level (`jb_begin_frame` / `jb_end_frame`), jitterbug manages double-buffered display areas in VRAM, clips 2D and 3D primitives, and places textures and its built-in 8x8 bitmap font automatically. The program below draws a dithered triangle, a semi-transparent panel and text into a 320x240 frame, then writes the RGBA8 pixels returned by `jb_frame` as a PPM image:

```c
#include <jitterbug.h>
#include <stdio.h>

int main(void)
{
    jb_result r = JB_OK;
    jb_config cfg = {0};
    cfg.scale = 1;
    cfg.width = 320;
    cfg.height = 240;

    jb_context *jb = jb_create(&cfg, &r);
    if (!jb) {
        fprintf(stderr, "jb_create: %s\n", jb_result_string(r));
        return 1;
    }

    jb_set_dither(jb, 1);
    jb_begin_frame(jb, JB_RGB(16, 24, 48));

    jb_poly tri = {0};
    tri.vertices = 3;
    tri.gouraud = 1;
    tri.v[0] = (jb_vertex){.x = 160, .y = 32, .color = JB_RGB(255, 96, 64)};
    tri.v[1] = (jb_vertex){.x = 264, .y = 200, .color = JB_RGB(64, 224, 128)};
    tri.v[2] = (jb_vertex){.x = 56, .y = 200, .color = JB_RGB(64, 128, 255)};
    jb_draw_poly(jb, &tri);

    jb_rect panel = {0};
    panel.x = 12;
    panel.y = 12;
    panel.w = 136;
    panel.h = 24;
    panel.color = JB_RGB(16, 24, 40);
    panel.blend = JB_BLEND_HALF;
    jb_draw_rect(jb, &panel);

    jb_draw_text(jb, 20, 20, JB_RGB(255, 240, 200), "HELLO JITTERBUG");

    /* The sticky error reports the first failure anywhere in the frame. */
    if (jb_end_frame(jb) != JB_OK) {
        fprintf(stderr, "frame failed: %s\n", jb_error_string(jb));
        jb_destroy(jb);
        return 1;
    }

    int w = 0, h = 0;
    const uint8_t *rgba = jb_frame(jb, &w, &h);
    if (!rgba) {
        fprintf(stderr, "jb_frame: %s\n", jb_error_string(jb));
        jb_destroy(jb);
        return 1;
    }

    FILE *f = fopen("first-frame.ppm", "wb");
    if (!f) {
        perror("first-frame.ppm");
        jb_destroy(jb);
        return 1;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        fputc(rgba[i * 4 + 0], f);
        fputc(rgba[i * 4 + 1], f);
        fputc(rgba[i * 4 + 2], f);
    }
    fclose(f);

    jb_destroy(jb);
    return 0;
}
```

In a real game or tool, pass `rgba` directly to an SDL3 streaming texture (`SDL_PIXELFORMAT_RGBA32` / `SDL_PIXELFORMAT_ABGR8888`, see `examples/first-light/main_sdl.c`), a raylib texture, or a browser canvas `ImageData` (`examples/first-light/web/index.html`).

## Next steps

- [examples/first-light/README.md](../examples/first-light/README.md) — a small, self-contained 3D scene (spinning lit cube, animated palette, checkered floor, billboard star, 3D lines, HUD panel) with SDL3, headless and WebAssembly front-ends.
- [examples/demo/README.md](../examples/demo/README.md) — a larger 3D archipelago scene using `.jbt` textures, `.jbm` meshes, fog, palette-animated water and every runtime switch.
- [docs/api.md](api.md) — guided tour of the API by task.
- [docs/tools.md](tools.md) and [docs/formats.md](formats.md) — converting PNGs and OBJ/glTF models into `.jbt` and `.jbm` assets.

# examples/demo: the demo scene (README.md here).
#   demo_scene       scene.c, terrain.c, assets.c: the scene, with no
#                    platform code, and its files built in (demo_files.c,
#                    made from assets/ by assets/embed.cmake)
#   demo-headless    ticks, hashes and PNGs without a window; the tests and
#                    the checks run it
#   demo             an SDL3 window, when SDL3 is found (not for emscripten)
#   demo_record      rewrites expected.txt with the hashes the runs give now
#                    (cmake --build build --target demo_record)
#   demo-web         the browser build: demo.js and demo.wasm, with
#                    web/index.html beside them (emscripten only)
#   demo-make-sources  assets/make-sources.c: the original art the files
#                    are converted from (not for emscripten)
#   demo_assets_regen  makes assets/sources and the files in assets/ again
#                    (cmake --build build --target demo_assets_regen)
# Everything here is strict C99 against <jitterbug.h> alone.

function(jb_demo_c99 target)
  set_target_properties(${target} PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_link_libraries(${target} PRIVATE jb_build_flags)
  if(NOT MSVC)
    target_compile_options(${target} PRIVATE -pedantic-errors)
  endif()
endfunction()

# The files (assets/conversions.txt), as bytes in the program.
file(STRINGS assets/conversions.txt _demo_conversions REGEX "^[^#]")
set(_demo_files)
foreach(_line IN LISTS _demo_conversions)
  separate_arguments(_parts UNIX_COMMAND "${_line}")
  list(GET _parts 0 _name)
  list(APPEND _demo_files ${CMAKE_CURRENT_SOURCE_DIR}/assets/${_name})
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS assets/conversions.txt)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/demo_files.c
  COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/demo_files.c
    -P ${CMAKE_CURRENT_SOURCE_DIR}/assets/embed.cmake
  DEPENDS ${_demo_files} assets/embed.cmake assets/conversions.txt
  COMMENT "Building the demo's files into demo_files.c" VERBATIM)

add_library(demo_scene STATIC scene.c terrain.c assets.c ${CMAKE_CURRENT_BINARY_DIR}/demo_files.c)
target_include_directories(demo_scene PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(demo_scene PUBLIC jitterbug)
jb_demo_c99(demo_scene)

# stb_image_write (third_party/stb), without the project's warning flags.
add_library(demo_png STATIC png.c)
target_include_directories(demo_png SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/stb)
set_target_properties(demo_png PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

add_executable(demo-headless main_headless.c)
target_link_libraries(demo-headless PRIVATE demo_scene demo_png)
jb_demo_c99(demo-headless)
if(EMSCRIPTEN)
  # Under Node: the real file system (expected.txt, PNGs), the exit status,
  # and memory to grow into for the scaled VRAM.
  target_link_options(demo-headless PRIVATE
    -sNODERAWFS=1 -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=1MB)
endif()

# The frame hashes, stored once (expected.txt) and checked by every build,
# native and wasm: every run at 1 thread, and natively at 4 threads too.
if(JB_TESTS)
  set(_demo_expected ${CMAKE_CURRENT_SOURCE_DIR}/expected.txt)
  add_test(NAME demo COMMAND demo-headless --threads 1 --expect ${_demo_expected})
  if(JB_THREADS)
    add_test(NAME demo_4t COMMAND demo-headless --threads 4 --expect ${_demo_expected})
  endif()
  # The gap check (main_headless.c, --gaps): over the whole loop, at 1x and
  # 2x, 4:3 and widescreen, no pixel that must show the world lets the
  # clear colour through (a crack along a chunk seam, a face culled on a
  # ridge, a chunk left out at the frame's edge, a slit under a beach).
  # Both geometry paths; and with precise vertices at 2x and 4x (fewer
  # ticks: half the loop at 2x, its first 256 frames at 4x, as those cost
  # minutes under QEMU), where the scaled frame draws faces at their
  # sub-pixel positions and must judge their facing there.
  foreach(_scale 1 2)
    foreach(_wide 0 1)
      foreach(_float 0 1)
        set(_name demo_gaps_${_scale}x)
        if(_wide)
          string(APPEND _name _wide)
        endif()
        if(_float)
          string(APPEND _name _float)
        endif()
        add_test(NAME ${_name} COMMAND demo-headless --gaps 1 --frames 44 --step 64 --panel 0
          --scale ${_scale} --widescreen ${_wide} --float ${_float})
      endforeach()
    endforeach()
  endforeach()
  foreach(_float 0 1)
    set(_name demo_gaps_2x_precise)
    set(_name4 demo_gaps_4x_precise)
    if(_float)
      string(APPEND _name _float)
      string(APPEND _name4 _float)
    endif()
    add_test(NAME ${_name} COMMAND demo-headless --gaps 1 --frames 22 --step 64
      --panel 0 --scale 2 --precise 1 --float ${_float})
    add_test(NAME ${_name4} COMMAND demo-headless --gaps 1 --frames 4 --step 64 --panel 0
      --scale 4 --precise 1 --float ${_float})
  endforeach()
  # The fade check (main_headless.c, --fade): a chunk of terrain crossing
  # to its next fade level changes no pixel's channel by more than 17 of
  # 255, two steps of the frame's 5-bit colour, over the loop at 1x and 2x.
  foreach(_scale 1 2)
    add_test(NAME demo_fade_${_scale}x COMMAND demo-headless --fade 17 --frames 44 --step 64
      --panel 0 --scale ${_scale})
  endforeach()
  # The pier check (main_headless.c, --pier): through the pier's slow
  # passage no post is drawn over its deck.
  add_test(NAME demo_pier COMMAND demo-headless --pier 1 --start 603 --frames 40 --step 16
    --panel 0 --scale 2)
  # The runner's own command line (test-headless.cmake), through
  # CMAKE_CROSSCOMPILING_EMULATOR when there is one (qemu, Node).
  # The files are what the converters make from the sources, byte for byte
  # (assets/convert.cmake), wherever the converters are built.
  if(TARGET jb-tex AND TARGET jb-mesh)
    add_test(NAME demo_assets
      COMMAND ${CMAKE_COMMAND} -DJB_TEX=$<TARGET_FILE:jb-tex> -DJB_MESH=$<TARGET_FILE:jb-mesh>
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/assets-test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assets/convert.cmake -- ${CMAKE_CROSSCOMPILING_EMULATOR})
  endif()
  add_test(NAME demo_headless_cli
    COMMAND ${CMAKE_COMMAND} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/headless-cli-test
      -P ${CMAKE_CURRENT_SOURCE_DIR}/test-headless.cmake --
      ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:demo-headless>)
endif()

# The original art, and the files made from it again.
if(NOT EMSCRIPTEN)
  add_executable(demo-make-sources assets/make-sources.c)
  target_link_libraries(demo-make-sources PRIVATE jitterbug demo_png)
  jb_demo_c99(demo-make-sources)
  if(JB_TESTS)
    # The sources are what the generator writes, byte for byte.
    add_test(NAME demo_sources
      COMMAND ${CMAKE_COMMAND} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/sources-test
        -DSOURCES=${CMAKE_CURRENT_SOURCE_DIR}/assets/sources
        -P ${CMAKE_CURRENT_SOURCE_DIR}/assets/sources.cmake --
        ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:demo-make-sources>)
  endif()
  if(TARGET jb-tex AND TARGET jb-mesh)
    add_custom_target(demo_assets_regen
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_SOURCE_DIR}/assets/sources
      COMMAND demo-make-sources ${CMAKE_CURRENT_SOURCE_DIR}/assets/sources
      COMMAND ${CMAKE_COMMAND} -DJB_TEX=$<TARGET_FILE:jb-tex> -DJB_MESH=$<TARGET_FILE:jb-mesh>
        -DREGEN=ON -P ${CMAKE_CURRENT_SOURCE_DIR}/assets/convert.cmake
      DEPENDS demo-make-sources jb-tex jb-mesh
      COMMENT "Making examples/demo/assets again" VERBATIM)
  endif()
endif()

if(NOT EMSCRIPTEN)
  add_custom_target(demo_record
    COMMAND demo-headless --record ${CMAKE_CURRENT_SOURCE_DIR}/expected.txt
    COMMENT "Re-recording examples/demo/expected.txt" VERBATIM)
endif()

if(NOT EMSCRIPTEN)
  find_package(SDL3 CONFIG QUIET)
  if(SDL3_FOUND)
    add_executable(demo main_sdl.c)
    target_link_libraries(demo PRIVATE demo_scene SDL3::SDL3)
    jb_demo_c99(demo)
  else()
    message(STATUS "SDL3 not found: not building the demo's window")
  endif()
endif()

if(EMSCRIPTEN)
  # The page's module: createDemo() resolves to it, in a browser or under
  # Node, and it exports what web/web.c marks EMSCRIPTEN_KEEPALIVE.
  add_executable(demo-web web/web.c)
  set_target_properties(demo-web PROPERTIES OUTPUT_NAME demo SUFFIX .js)
  target_link_libraries(demo-web PRIVATE demo_scene)
  jb_demo_c99(demo-web)
  target_link_options(demo-web PRIVATE
    -sMODULARIZE=1 -sEXPORT_NAME=createDemo -sENVIRONMENT=web,node
    -sALLOW_MEMORY_GROWTH=1 -sEXPORTED_RUNTIME_METHODS=HEAPU8 -sFILESYSTEM=0)
  configure_file(web/index.html ${CMAKE_CURRENT_BINARY_DIR}/index.html COPYONLY)
  if(JB_TESTS)
    # The page's own module under Node: the functions index.html calls
    # exist, and the frames they give hash as expected.txt says.
    add_test(NAME demo_web_module
      COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} ${CMAKE_CURRENT_SOURCE_DIR}/web/test-module.cjs
        $<TARGET_FILE:demo-web> ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html ${_demo_expected})
  endif()
endif()

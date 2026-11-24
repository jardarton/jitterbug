# examples/first-light: the first program on the library (README.md here).
#   first_light_scene      scene.c: the scene, with no platform code
#   first-light-headless   frames, hashes and PNGs without a window; the
#                          tests and the checks run it
#   first-light            an SDL3 window, when SDL3 is found (not for
#                          emscripten)
#   first_light_record     rewrites expected.txt with the hashes the runs give
#                          now (cmake --build build --target first_light_record)
#   first-light-web        the browser build: first-light.js and .wasm,
#                          with web/index.html beside them (emscripten only)
# Everything here is strict C99 against <jitterbug.h> alone.

function(jb_example_c99 target)
  set_target_properties(${target} PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_link_libraries(${target} PRIVATE jb_build_flags)
  if(NOT MSVC)
    target_compile_options(${target} PRIVATE -pedantic-errors)
  endif()
endfunction()

add_library(first_light_scene STATIC scene.c)
target_include_directories(first_light_scene PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(first_light_scene PUBLIC jitterbug)
jb_example_c99(first_light_scene)

# stb_image_write (third_party/stb), without the project's warning flags.
add_library(first_light_png STATIC png.c)
target_include_directories(first_light_png SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/stb)
set_target_properties(first_light_png PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

add_executable(first-light-headless main_headless.c)
target_link_libraries(first-light-headless PRIVATE first_light_scene first_light_png)
jb_example_c99(first-light-headless)
if(EMSCRIPTEN)
  # Under Node: the real file system (expected.txt, PNGs), the exit status,
  # and memory to grow into for the scaled VRAM.
  target_link_options(first-light-headless PRIVATE
    -sNODERAWFS=1 -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=1MB)
endif()

# The frame hashes, stored once (expected.txt) and checked by every build,
# native and wasm: every run at 1 thread, and natively at 4 threads too.
if(JB_TESTS)
  set(_fl_expected ${CMAKE_CURRENT_SOURCE_DIR}/expected.txt)
  add_test(NAME first_light COMMAND first-light-headless --threads 1 --expect ${_fl_expected})
  if(JB_THREADS)
    add_test(NAME first_light_4t COMMAND first-light-headless --threads 4 --expect ${_fl_expected})
  endif()
  # The scene when a frame cannot begin (test_scene.c).
  add_executable(first-light-scene-test test_scene.c)
  target_link_libraries(first-light-scene-test PRIVATE first_light_scene)
  jb_example_c99(first-light-scene-test)
  add_test(NAME first_light_scene_errors COMMAND first-light-scene-test)
  # The runner's own command line: --record on a run file a careless writer
  # would spoil, concurrent recordings, malformed numbers, a run file it
  # cannot read, --pause-at (test-headless.cmake). The program is run
  # through CMAKE_CROSSCOMPILING_EMULATOR when there is one (qemu, Node),
  # which add_test does not add to a command that is not a target.
  add_test(NAME first_light_headless_cli
    COMMAND ${CMAKE_COMMAND} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/headless-cli-test
      -P ${CMAKE_CURRENT_SOURCE_DIR}/test-headless.cmake --
      ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:first-light-headless>)
endif()

# Re-recording expected.txt after a deliberate change, in one command:
#   cmake --build build --target first_light_record
if(NOT EMSCRIPTEN)
  add_custom_target(first_light_record
    COMMAND first-light-headless --record ${CMAKE_CURRENT_SOURCE_DIR}/expected.txt
    COMMENT "Re-recording examples/first-light/expected.txt" VERBATIM)
endif()

if(NOT EMSCRIPTEN)
  find_package(SDL3 CONFIG QUIET)
  if(SDL3_FOUND)
    add_executable(first-light main_sdl.c)
    target_link_libraries(first-light PRIVATE first_light_scene SDL3::SDL3)
    jb_example_c99(first-light)
  else()
    message(STATUS "SDL3 not found: not building the first-light window")
  endif()
endif()

if(EMSCRIPTEN)
  # The page's module: createFirstLight() resolves to it, in a browser or
  # under Node, and it exports what web/web.c marks EMSCRIPTEN_KEEPALIVE.
  add_executable(first-light-web web/web.c)
  set_target_properties(first-light-web PROPERTIES OUTPUT_NAME first-light SUFFIX .js)
  target_link_libraries(first-light-web PRIVATE first_light_scene)
  jb_example_c99(first-light-web)
  target_link_options(first-light-web PRIVATE
    -sMODULARIZE=1 -sEXPORT_NAME=createFirstLight -sENVIRONMENT=web,node
    -sALLOW_MEMORY_GROWTH=1 -sEXPORTED_RUNTIME_METHODS=HEAPU8 -sFILESYSTEM=0)
  configure_file(web/index.html ${CMAKE_CURRENT_BINARY_DIR}/index.html COPYONLY)
  if(JB_TESTS)
    # The page's own module under Node: the functions index.html calls
    # exist, and the frames they give hash as expected.txt says.
    add_test(NAME first_light_web_module
      COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} ${CMAKE_CURRENT_SOURCE_DIR}/web/test-module.cjs
        $<TARGET_FILE:first-light-web> ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html ${_fl_expected})
  endif()
endif()

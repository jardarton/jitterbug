# tools/jb-bench: frame times for fixed workloads at each scale and thread
# count (docs/performance.md). Strict C99 against <jitterbug.h> alone. The
# first-light workload is the console-level first-light scene,
# tests/console_scene/scene.c itself (the example as it was first written,
# kept there with its hashes), compiled here when the tests are not built.

if(NOT TARGET console_scene)
  add_library(console_scene STATIC ${PROJECT_SOURCE_DIR}/tests/console_scene/scene.c)
  target_include_directories(console_scene PUBLIC ${PROJECT_SOURCE_DIR}/tests/console_scene)
  target_link_libraries(console_scene PUBLIC jitterbug PRIVATE jb_build_flags)
  set_target_properties(console_scene PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endif()

add_executable(jb-bench bench.c firstlight.c heavy.c stats.c)
target_link_libraries(jb-bench PRIVATE console_scene jitterbug jb_build_flags)
set_target_properties(jb-bench PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(NOT MSVC)
  target_compile_options(jb-bench PRIVATE -pedantic-errors)
endif()

# What it was built with, printed with its results.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _jb_bench_type)
set(_jb_bench_build
  "${CMAKE_BUILD_TYPE}, ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}, flags: ${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${_jb_bench_type}}")
string(STRIP "${_jb_bench_build}" _jb_bench_build)
target_compile_definitions(jb-bench PRIVATE "JB_BENCH_BUILD=\"${_jb_bench_build}\"")

if(EMSCRIPTEN)
  # Under Node: the real file system (/proc for the machine), the exit
  # status, and memory to grow into for the scaled VRAM.
  target_link_options(jb-bench PRIVATE
    -sNODERAWFS=1 -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=1MB)
endif()

# Installed: it is meant to be run on the machine a user has
# (docs/performance.md), and packages.bench is its install.
jb_install_tool(jb-bench)

# The workloads draw what they drew when their hashes were stored, at
# every thread count. Nothing is timed.
if(JB_TESTS)
  add_test(NAME jb_bench_check COMMAND jb-bench --check)
endif()

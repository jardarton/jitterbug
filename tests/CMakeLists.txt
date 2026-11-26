# Under emscripten every test runs under Node (the toolchain's
# CMAKE_CROSSCOMPILING_EMULATOR): with the real file system for the traces
# and PNGs in the source tree, its exit status passed on, memory that grows
# for the scaled VRAMs (64 MB at 8x), and the 8 MB stack native tests get.
# The build is single-threaded (JB_THREADS off); what that leaves out is
# said where each test is added.
if(EMSCRIPTEN)
  add_link_options(-sNODERAWFS=1 -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB
    -sSTACK_SIZE=8MB)
endif()

# version: the library reports the version its header declares. Built as
# strict C99, so it also proves the public header is C99.
add_executable(test_version version.c)
target_link_libraries(test_version PRIVATE jitterbug jb_build_flags)
set_target_properties(test_version PROPERTIES
  C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(NOT MSVC)
  target_compile_options(test_version PRIVATE -pedantic-errors)
endif()
add_test(NAME version COMMAND test_version)

# header_cxx: the public header from C++.
include(CheckLanguage)
check_language(CXX)
if(CMAKE_CXX_COMPILER)
  enable_language(CXX)
  add_executable(test_header_cxx header_cxx.cpp)
  target_link_libraries(test_header_cxx PRIVATE jitterbug)
  set_target_properties(test_header_cxx PROPERTIES
    CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  if(NOT MSVC)
    target_compile_options(test_header_cxx PRIVATE -Wall -Wextra -pedantic-errors
      $<$<BOOL:${JB_WERROR}>:-Werror>)
    if(JB_SANITIZE)
      target_compile_options(test_header_cxx PRIVATE -fsanitize=address,undefined)
      target_link_options(test_header_cxx PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
  add_test(NAME header_cxx COMMAND test_header_cxx)
else()
  message(STATUS "No C++ compiler: skipping the header_cxx test")
endif()

# assumptions: what src/core/assumptions.c asserts when it is compiled,
# checked again on values the compiler can't fold.
add_executable(test_assumptions assumptions.c)
target_link_libraries(test_assumptions PRIVATE jb_build_flags)
set_target_properties(test_assumptions PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME assumptions COMMAND test_assumptions)

# jb_expect_status(NAME STATUS REGEX [NOT REGEX] COMMAND TARGET ARGS...): a
# test that requires an exact exit status and a diagnostic. The program
# must exit with exactly STATUS and print something REGEX matches (and
# nothing the NOT regex matches); PASS_REGULAR_EXPRESSION and WILL_FAIL
# would ignore the exit status. Run by conformance/expect-status.cmake,
# through CMAKE_CROSSCOMPILING_EMULATOR when there is one. Also used by
# conformance/CMakeLists.txt.
function(jb_expect_status name status regex)
  cmake_parse_arguments(PARSE_ARGV 3 _e "" "NOT" "COMMAND")
  if(NOT _e_COMMAND)
    message(FATAL_ERROR "jb_expect_status(${name}): COMMAND TARGET ARGS... is required")
  endif()
  list(POP_FRONT _e_COMMAND _target)
  set(_not)
  if(DEFINED _e_NOT)
    set(_not "-DNOT_OUTPUT=${_e_NOT}")
  endif()
  add_test(NAME ${name}
    COMMAND ${CMAKE_COMMAND} -DSTATUS=${status} "-DOUTPUT=${regex}" ${_not}
      -P ${PROJECT_SOURCE_DIR}/tests/conformance/expect-status.cmake --
      ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:${_target}> ${_e_COMMAND})
endfunction()

# isa: the CPU has, and executes, every instruction-set extension the
# compiler may use (tests/isa.c lists the instructions). With
# JB_ISA_EXPECT=x86-64-v3 (the x86-64-v3 check in flake.nix) it runs as
# `test_isa --expect x86-64-v3` and fails unless the build was compiled for
# x86-64-v3 and the CPU, or the QEMU model, advertises (CPUID) and executes
# every requirement of it beyond the baseline, as the x86-64 psABI defines
# the levels (from x86-64-v2: CMPXCHG16B, LAHF-SAHF, POPCNT, SSE3, SSE4.1,
# SSE4.2, SSSE3; from x86-64-v3: AVX, AVX2, BMI1, BMI2, F16C, FMA, LZCNT,
# MOVBE, and OSXSAVE, checked with xgetbv). Elsewhere it checks what the
# compiler was told it may use, which on the x86-64 baseline is nothing.
set(JB_ISA_EXPECT "" CACHE STRING "isa: the instruction-set level the build must be for (x86-64-v3)")
add_executable(test_isa isa.c)
target_link_libraries(test_isa PRIVATE jb_build_flags)
if(JB_ISA_EXPECT STREQUAL "")
  add_test(NAME isa COMMAND test_isa)
elseif(JB_ISA_EXPECT STREQUAL "x86-64-v3")
  add_test(NAME isa COMMAND test_isa --expect ${JB_ISA_EXPECT})
else()
  message(FATAL_ERROR "JB_ISA_EXPECT: only x86-64-v3 is known, not ${JB_ISA_EXPECT}")
endif()

# vk_smoke: a Vulkan instance and device on Mesa's software driver, with no
# display. It shows this machine can run the cross-backend tests.
if(JB_HAVE_VULKAN AND JB_LAVAPIPE_ICD)
  add_executable(test_vk_smoke vk_smoke.c)
  target_link_libraries(test_vk_smoke PRIVATE Vulkan::Vulkan jb_build_flags)
  set_target_properties(test_vk_smoke PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME vk_smoke COMMAND test_vk_smoke)
  # tests/lsan.supp: the driver's own leaks are not ours to fix.
  set_tests_properties(vk_smoke PROPERTIES ENVIRONMENT
    "VK_DRIVER_FILES=${JB_LAVAPIPE_ICD};VK_ICD_FILENAMES=${JB_LAVAPIPE_ICD};VK_LOADER_LAYERS_DISABLE=~implicit~;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp:print_suppressions=0")
elseif(JB_HAVE_VULKAN)
  message(STATUS "JB_LAVAPIPE_ICD not set: skipping the vk_smoke test")
endif()

# The seed's own tests, copied with it (tests/gte), and a smoke test of the
# GPU. They link the internals (jb_internal) directly.
function(jb_seed_test name)
  add_executable(test_${name} ${ARGN})
  set_target_properties(test_${name} PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_link_libraries(test_${name} PRIVATE jb_internal jb_build_flags)
  add_test(NAME ${name} COMMAND test_${name})
endfunction()

# gte: the GTE against hand-worked cases.
jb_seed_test(gte gte/gte_test.c)

# gte_precise: the precise-vertex records against the GTE.
jb_seed_test(gte_precise gte/precise_test.c)
if(NOT MSVC)
  target_link_libraries(test_gte_precise PRIVATE m)
endif()

# gpu_smoke: GP0 words drawn at 1x and 2x with 1 and 4 threads. Built with
# the precise-vertex table as the context's precise source, and again with
# a stand-in source that knows nothing (precise_stub.c).
jb_seed_test(gpu_smoke gpu_smoke.c)
jb_seed_test(gpu_smoke_stub gpu_smoke.c precise_stub.c)
target_compile_definitions(test_gpu_smoke_stub PRIVATE JB_SMOKE_STUB)

jb_seed_test(checkpoint checkpoint.c)

# seed_rtt: a drawn 15-bit texture sampled above 1x, which the recorded
# scenes never do (the scaled rasteriser's sub-texel sampling).
jb_seed_test(seed_rtt seed_rtt.c precise_stub.c)

# seed_persp: the perspective helpers in src/soft/raster.h, which a call
# trace can't reach (the arithmetic half of the seed test's perspective
# clamp checks).
jb_seed_test(seed_persp seed_persp.c)

# seed_fixes: the seed's small defects, fixed without changing a pixel.
jb_seed_test(seed_fixes seed_fixes.c)

# context_lifecycle: many contexts created, drawn with and destroyed, alone
# and several at once; their worker threads are joined (counted), and under
# the sanitizers nothing leaks.
# seed_multi: two contexts replaying two different scenes, interleaved on
# one thread and at once on two; every result of each matches the scene
# alone.
# Neither is built single-threaded: context_lifecycle checks that each
# context runs the threads it asked for and counts them in /proc/self/task;
# seed_multi starts two threads of its own (pthreads), and its scenes check
# the thread counts they recorded.
if(JB_THREADS)
  jb_seed_test(context_lifecycle context_lifecycle.c)

  add_executable(test_seed_multi seed_multi.c)
  set_target_properties(test_seed_multi PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_link_libraries(test_seed_multi PRIVATE jb_internal jb_build_flags Threads::Threads)
  add_test(NAME seed_multi COMMAND test_seed_multi ${PROJECT_SOURCE_DIR}/tests/seed)
endif()

# PNG reading and writing for the golden images (third_party/stb), built
# without the project's warning flags.
add_library(jb_stb STATIC stb.c)
target_include_directories(jb_stb SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/stb)
set_target_properties(jb_stb PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(NOT MSVC)
  target_link_libraries(jb_stb PRIVATE m)
endif()

# Reading the seed traces (trace.h), for seed_replay and api_golden.
add_library(jb_trace STATIC trace.c)
set_target_properties(jb_trace PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(jb_trace PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(jb_trace PUBLIC jb_stb PRIVATE jb_build_flags)

# The seed checkpoint (docs/testing.md, "Seed traces and re-blessing"):
# the seed's GPU test scenes, recorded as traces with golden images
# (tests/seed, FORMAT.md), replayed
# against a context with the fake precise table seed_replay.c provides.
#   seed_NN-name    one scene alone, from its prelude
#   seed_sequence   all of them in order in one process, every capture
#   seed_sequence_1t, seed_sequence_8t   the same with 1 and 8 threads
#                   forced at every configuration
# A mismatch writes *.png.out images into tests/seed-out in the build tree.
# Single-threaded (JB_THREADS off), every run forces one thread instead:
# the replayer checks each configuration's thread count, and the recorded
# counts of 2 to 8 cannot be met. So seed_NN runs with --threads 1, and
# seed_sequence and seed_sequence_8t are left out (seed_sequence_1t is the
# same sequence).
add_executable(test_seed_replay seed_replay.c)
set_target_properties(test_seed_replay PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(test_seed_replay PRIVATE jb_internal jb_trace jb_stb jb_build_flags)
# It refuses to --bless or --record-scale into the source tree's tests/seed
# (tools/seed-record/rebless.sh works on a copy and installs it).
target_compile_definitions(test_seed_replay PRIVATE
  JB_SEED_SOURCE_DIR="${PROJECT_SOURCE_DIR}/tests/seed")
# Exactly the scenes of tools/seed-record/scenes.txt (the re-blessing
# tools require the same list), in its order.
file(GLOB _jb_seed_traces CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/seed/*.jbt)
list(SORT _jb_seed_traces)
set(_jb_seed_list ${PROJECT_SOURCE_DIR}/tools/seed-record/scenes.txt)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_jb_seed_list})
file(STRINGS ${_jb_seed_list} _jb_seed_names REGEX "^[^#]")
set(_jb_seed_want)
foreach(_name ${_jb_seed_names})
  list(APPEND _jb_seed_want ${PROJECT_SOURCE_DIR}/tests/seed/${_name}.jbt)
endforeach()
if(NOT _jb_seed_traces STREQUAL _jb_seed_want)
  message(FATAL_ERROR "tests/seed: the traces are not the scenes of ${_jb_seed_list}")
endif()
set(_jb_seed_out ${CMAKE_CURRENT_BINARY_DIR}/seed-out)
file(MAKE_DIRECTORY ${_jb_seed_out})
if(JB_THREADS)
  set(_jb_one_thread)
  set(_jb_forced_threads 1 8)
else()
  set(_jb_one_thread --threads 1)
  set(_jb_forced_threads 1)
endif()
foreach(_trace ${_jb_seed_traces})
  get_filename_component(_scene ${_trace} NAME_WE)
  add_test(NAME seed_${_scene} COMMAND test_seed_replay ${_jb_one_thread} ${_trace}
    WORKING_DIRECTORY ${_jb_seed_out})
endforeach()
if(JB_THREADS)
  add_test(NAME seed_sequence COMMAND test_seed_replay --sequence ${_jb_seed_traces}
    WORKING_DIRECTORY ${_jb_seed_out})
endif()
foreach(_threads ${_jb_forced_threads})
  add_test(NAME seed_sequence_${_threads}t
    COMMAND test_seed_replay --threads ${_threads} --sequence ${_jb_seed_traces}
    WORKING_DIRECTORY ${_jb_seed_out})
endforeach()

# The re-blessing tools refuse to report a comparison they did not make
# (docs/testing.md, "Seed traces and re-blessing"):
#   seed_compare_missing   --compare with a directory of no dumps
#   seed_compare_aliased   --dump and --compare the same directory
#   seed_bless_source      --bless into the source tree's tests/seed
# each exit status 2 with the message matched (jb_expect_status, both
# required); and natively (measure.sh runs the
# replayer it is given directly)
#   seed_measure           measure.sh with replayers that do not replay
#                          fails, and with the replayer against itself
#                          passes having compared every image.
set(_jb_seed_tools ${CMAKE_CURRENT_BINARY_DIR}/seed-tools)
file(MAKE_DIRECTORY ${_jb_seed_tools}/empty ${_jb_seed_tools}/same)
list(GET _jb_seed_traces 0 _jb_seed_first)
jb_expect_status(seed_compare_missing 2 "--compare [^\n]*: references missing"
  COMMAND test_seed_replay --compare ${_jb_seed_tools}/empty ${_jb_one_thread} ${_jb_seed_first})
set_tests_properties(seed_compare_missing PROPERTIES WORKING_DIRECTORY ${_jb_seed_out})
jb_expect_status(seed_compare_aliased 2 "--dump and --compare are the same directory"
  COMMAND test_seed_replay --dump ${_jb_seed_tools}/same --compare ${_jb_seed_tools}/same
    ${_jb_seed_first})
set_tests_properties(seed_compare_aliased PROPERTIES WORKING_DIRECTORY ${_jb_seed_out})
jb_expect_status(seed_bless_source 2 "will not write into [^\n]*tests/seed"
  COMMAND test_seed_replay --bless ${PROJECT_SOURCE_DIR}/tests/seed --sequence ${_jb_seed_traces})
set_tests_properties(seed_bless_source PROPERTIES WORKING_DIRECTORY ${_jb_seed_out})
find_program(_jb_bash bash)
find_program(_jb_false false)
if(NOT CMAKE_CROSSCOMPILING_EMULATOR AND _jb_bash AND _jb_false)
  add_test(NAME seed_measure
    COMMAND ${CMAKE_COMMAND} -DBASH=${_jb_bash}
      -DMEASURE=${PROJECT_SOURCE_DIR}/tools/seed-record/measure.sh
      -DREPLAY=$<TARGET_FILE:test_seed_replay> -DFALSE=${_jb_false}
      -DWORK=${_jb_seed_tools}/measure -P ${CMAKE_CURRENT_SOURCE_DIR}/seed_measure.cmake)
endif()

# seed_audit_compare: the polygon-interpolation audit's comparison
# (tools/seed-record/audit/compare.c) on synthetic captures
# (tools/seed-record/audit/selftest.c): changes the envelope allows pass,
# the rest (among them a same-primitive blend from 0x0001 to 0xffff and a
# change no primitive wrote) fail, wrong input is refused.
if(NOT MSVC)
  add_executable(test_seed_audit_compare ${PROJECT_SOURCE_DIR}/tools/seed-record/audit/selftest.c)
  set_target_properties(test_seed_audit_compare PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_link_libraries(test_seed_audit_compare PRIVATE jb_build_flags)
  file(MAKE_DIRECTORY ${_jb_seed_tools}/audit)
  add_test(NAME seed_audit_compare COMMAND test_seed_audit_compare ${_jb_seed_tools}/audit)
endif()

# The same sequence at other internal scales (FORMAT.md, "Other scales"):
# every scale above 1x the traces set becomes S, and what then differs from
# the recording is checked against tests/seed/scaled/Sx.txt and its images,
# which the seed GPU wrote as committed and tools/seed-record/rebless.sh
# has rewritten after each hardware fix since (tests/seed/CHANGES.md).
#   seed_scale<S>   S = 3, 4, 8, at the recorded thread counts
#   seed_scale<S>_1t, seed_scale<S>_8t   S = 3, 4, with 1 and 8 forced
# Not 8 with forced threads: hashing the 64 MB scaled VRAM at every capture
# makes a scale-8 run take about 10 s under the sanitizers.
# Single-threaded, as above: only the runs forcing one thread, at 3, 4 and
# 8 (seed_scale8_1t standing for seed_scale8).
set(_jb_seed_scaled ${PROJECT_SOURCE_DIR}/tests/seed/scaled)
foreach(_scale 3 4 8)
  if(NOT JB_THREADS)
    add_test(NAME seed_scale${_scale}_1t
      COMMAND test_seed_replay --threads 1 --scale ${_scale} ${_jb_seed_scaled}
        --sequence ${_jb_seed_traces}
      WORKING_DIRECTORY ${_jb_seed_out})
    continue()
  endif()
  add_test(NAME seed_scale${_scale}
    COMMAND test_seed_replay --scale ${_scale} ${_jb_seed_scaled} --sequence ${_jb_seed_traces}
    WORKING_DIRECTORY ${_jb_seed_out})
  if(_scale EQUAL 8)
    continue()
  endif()
  foreach(_threads 1 8)
    add_test(NAME seed_scale${_scale}_${_threads}t
      COMMAND test_seed_replay --threads ${_threads} --scale ${_scale} ${_jb_seed_scaled}
        --sequence ${_jb_seed_traces}
      WORKING_DIRECTORY ${_jb_seed_out})
  endforeach()
endforeach()

# The public API (include/jitterbug.h). Each test is strict C99 against
# <jitterbug.h> only (api_test.h is C99 helpers over it), except those
# that deliberately compare with the internals, which are C11 and also link
# jb_internal:
#   api_errors   bad configurations, NULL arguments, error strings, the
#                allocator callbacks (counted, and failing at every
#                allocation), many contexts created and destroyed
#   api_read     GPUREAD (round trips, odd sizes, wrapping, GP1(10h)) and
#                GPUSTAT after each command that changes it
#   api_frame    jb_frame: sizes, contents (against the VRAM read back at
#                1x, against the internal scanout at 2x), display disabled,
#                display modes, 24-bit mode, no allocation per call
#   api_stream   one word list fed whole, word by word and in random
#                chunks, and against the core decoder; random streams too
#   api_robust   a seeded random GP0/GP1 stream at 1x and 2x (under the
#                sanitizers: no crash, no report)
#   api_golden_* the seed's scenes through the public API (api_golden.c)
function(jb_api_test name)
  cmake_parse_arguments(ARG "INTERNAL" "" "" ${ARGN})
  add_executable(test_${name} ${name}.c ${ARG_UNPARSED_ARGUMENTS})
  if(ARG_INTERNAL)
    set_target_properties(test_${name} PROPERTIES
      C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_link_libraries(test_${name} PRIVATE jitterbug jb_internal jb_build_flags)
  else()
    set_target_properties(test_${name} PROPERTIES
      C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_link_libraries(test_${name} PRIVATE jitterbug jb_build_flags)
    if(NOT MSVC)
      target_compile_options(test_${name} PRIVATE -pedantic-errors)
    endif()
  endif()
endfunction()

jb_api_test(api_stream INTERNAL)
add_test(NAME api_stream COMMAND test_api_stream)
jb_api_test(api_errors)
add_test(NAME api_errors COMMAND test_api_errors)
jb_api_test(api_read)
add_test(NAME api_read COMMAND test_api_read)
jb_api_test(api_frame INTERNAL)
add_test(NAME api_frame COMMAND test_api_frame)
jb_api_test(api_robust)
add_test(NAME api_robust COMMAND test_api_robust)

# The calls over the console's commands, each against what it stands for:
#   api_prim     jb_prim_* and the drawing-state calls against the GP0
#                words the header says they send: a seeded run of every
#                kind, at 1x and 2x, 1 and 4 threads, with the range rule
#   api_vram     jb_vram_* against GP0(A0h), GP0(C0h) with GPUREAD,
#                GP0(02h) and GP0(80h)
#   api_gte      the context's GTE against the internal one (jbi_gte),
#                register by register over seeded inputs and every
#                command, and hand-checked cases
#   api_options  precise vertices and perspective-correct textures against
#                the internal path with the same precise table, toggled at
#                run time; jb_set_scale and jb_set_threads keep VRAM
jb_api_test(api_prim)
add_test(NAME api_prim COMMAND test_api_prim)
jb_api_test(api_vram)
add_test(NAME api_vram COMMAND test_api_vram)
jb_api_test(api_gte INTERNAL)
add_test(NAME api_gte COMMAND test_api_gte)
jb_api_test(api_options INTERNAL)
add_test(NAME api_options COMMAND test_api_options)
# The GTE helpers and the friendly geometry (include/jitterbug.h, GTE
# helpers and Scene geometry):
#   api_gte_helpers  each helper against the jb_gte_write/command/read
#                    sequence the header gives for it, over seeded inputs;
#                    NCLIP and AVSZ as pure functions against the commands;
#                    the sine table, the products against MVMVA, the
#                    rotation from its definition
#   geom_float       jb_sinf, jb_cosf, jb_tanf against long double within
#                    one ulp, over every float range, and the vector and
#                    matrix helpers; hashes of their bits stored in the
#                    source, so every build must give the same bits
#   geom_pipeline    the scene setters, the per-draw quantisation and its
#                    limits, and faces through both paths: exact faces of
#                    the GTE path against hand-written jb_gte_* code with
#                    the same registers, clipping, and stored hashes of
#                    the float path's results
jb_api_test(api_gte_helpers)
add_test(NAME api_gte_helpers COMMAND test_api_gte_helpers)
jb_api_test(geom_float)
add_test(NAME geom_float COMMAND test_geom_float)
jb_api_test(geom_pipeline INTERNAL)
add_test(NAME geom_pipeline COMMAND test_geom_pipeline)
if(UNIX AND NOT APPLE)
  target_link_libraries(test_api_gte_helpers PRIVATE m)
  target_link_libraries(test_geom_float PRIVATE m)
endif()
# rules: the rules fitted to ps1-tests' dumps where those cover them
# thinly (x-flip start for every u0, line ties in all eight directions, line
# and quad colours), at 1x to 3x (rules.c).
jb_api_test(rules)
add_test(NAME rules COMMAND test_rules)

# clut_cache: the CLUT cache with the queue and workers, at 1x to 3x and 1
# to 8 threads, and switched off (clut_cache.c says what each case checks).
jb_api_test(clut_cache INTERNAL)
add_test(NAME clut_cache COMMAND test_clut_cache)

# api_threads: thread requests in this build (JB_THREADS on or off): all
# accepted; single-threaded, more than one is clamped to one and logged;
# the frame is the same at every count.
jb_api_test(api_threads)
add_test(NAME api_threads COMMAND test_api_threads)
if(NOT JB_THREADS)
  target_compile_definitions(test_api_threads PRIVATE JB_TEST_SINGLE_THREADED)
endif()

# Textures placed in VRAM (include/jitterbug.h, Textures):
#   placer       the placer (src/core/placer.h) against a brute-force model
#                of its documented first fit: seeded create and free
#                sequences over several display layouts and reservations,
#                every answer equal to the model's, every rectangle checked
#                for the rules and overlap; hand-made edge cases
#   api_texture  jb_texture_create and the calls beside it through the
#                public API: VRAM contents and packing, drawing with the
#                textures, repeat and the texture window, palettes and the
#                CLUT cache, reservations, display layouts and widescreen,
#                full VRAM, every error, no allocation, a random sequence
#                checked from the jb_texture fields alone
#   font         the library's font (src/api/font.c): drawn from VRAM, every
#                glyph inside its cell and not empty but space, a stored
#                hash of the bitmap; and jbi_texture_window. It calls the
#                public API's internals, which a shared library hides, so it
#                is built from the library's sources (JB_API_SOURCES) and
#                the internals rather than linked with the library.
jb_api_test(placer INTERNAL)
add_test(NAME placer COMMAND test_placer)
jb_api_test(api_texture)
add_test(NAME api_texture COMMAND test_api_texture)
add_executable(test_font font.c ${JB_API_SOURCES})
set_target_properties(test_font PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(test_font PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(test_font PRIVATE jb_internal jb_build_flags)
add_test(NAME font COMMAND test_font)
# api_draw2d: sprites and text (api_draw2d.c says what each part checks):
# sprites against jb_prim_rect and jb_prim_poly at 1x to 3x, their texels
# by hand, clipping, repeat textures through every 2D draw, text against
# the font's bitmap, every error. It reads the font's glyphs, so it is
# built from the library's sources, as font is.
add_executable(test_api_draw2d api_draw2d.c ${JB_API_SOURCES})
set_target_properties(test_api_draw2d PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(test_api_draw2d PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(test_api_draw2d PRIVATE jb_internal jb_build_flags)
add_test(NAME api_draw2d COMMAND test_api_draw2d)

# The friendly level:
#   api_friendly    the frame, the ordering table, the sticky error, the new
#                   options and the helpers, through <jitterbug.h> alone
#                   (api_friendly.c lists the cases)
#   api_records     records, the frame's buckets and the screen-space
#                   splitter from the inside: records and jb_ot equal to
#                   jb_prim_* at 1x to 3x and 1 and 4 threads, sub-pixel data
#                   equal to the precise table at 2x, clipping exact enough
#                   (api_records.c). It needs functions of src/api that a
#                   shared library does not export, so it compiles those
#                   sources itself rather than link the library.
#   friendly_scene  a friendly 2D scene's frames against the hashes in
#                   friendly_scene.txt, the same file for every build
#                   (docs/determinism.md); --print gives the hashes after a
#                   deliberate change.
jb_api_test(api_friendly)
add_test(NAME api_friendly COMMAND test_api_friendly)
add_executable(test_api_records api_records.c ${JB_API_SOURCES})
set_target_properties(test_api_records PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(test_api_records PRIVATE
  ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_api_records PRIVATE jb_internal jb_build_flags)
add_test(NAME api_records COMMAND test_api_records)
# split_fuzz: the splitter's invariants (src/geom/split.h) on its pieces,
# sampled exactly, for seeded runs of the shapes that broke earlier
# designs (split_fuzz.c).
jb_seed_test(split_fuzz split_fuzz.c)
# The friendly 3D draws:
#   api_draw3d    jb_draw_mesh against hand-written jb_gte_* and jb_ot code
#                 (the same frames at 1x to 3x, precise and perspective,
#                 fog, 1 and 4 threads), a walk through a floor and a wall
#                 with no holes and no pixel twice, culling, ordering,
#                 billboards, 3D lines, every error (api_draw3d.c). Built
#                 from the library's sources, like api_records.
#   draw3d_scene  a 3D scene's frames against draw3d_scene.txt, the same
#                 file for every build (docs/determinism.md): both paths,
#                 1x, 2x, precise vertices, perspective-correct.
add_executable(test_api_draw3d api_draw3d.c ${JB_API_SOURCES})
set_target_properties(test_api_draw3d PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(test_api_draw3d PRIVATE
  ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_api_draw3d PRIVATE jb_internal jb_build_flags)
add_test(NAME api_draw3d COMMAND test_api_draw3d)
jb_api_test(draw3d_scene)
add_test(NAME draw3d_scene
  COMMAND test_draw3d_scene ${CMAKE_CURRENT_SOURCE_DIR}/draw3d_scene.txt)
jb_api_test(friendly_scene)
add_test(NAME friendly_scene
  COMMAND test_friendly_scene ${CMAKE_CURRENT_SOURCE_DIR}/friendly_scene.txt)
# sprite_scene: a scene of sprites and text (sprite_scene.c) against the
# hashes in sprite_scene.txt, the same file for every build
# (docs/determinism.md); --print gives the hashes after a deliberate change.
jb_api_test(sprite_scene)
add_test(NAME sprite_scene
  COMMAND test_sprite_scene ${CMAKE_CURRENT_SOURCE_DIR}/sprite_scene.txt)

# The stored-hash runners refuse a file that lists a run twice in place of
# another: for each of friendly_scene, sprite_scene and draw3d_scene, its
# file with the last run left out and the first listed again (written into
# the build tree from the real one) must fail, naming the missing run
# (<scene>_duplicate, exit status 1, jb_expect_status).
foreach(_scene friendly_scene sprite_scene draw3d_scene)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/${_scene}.txt)
  file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/${_scene}.txt _rows REGEX "^[a-z]")
  list(GET _rows 0 _first)
  list(GET _rows -1 _last)
  list(REMOVE_ITEM _rows "${_last}")
  list(APPEND _rows "${_first}")
  list(JOIN _rows "\n" _text)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${_scene}_duplicate.txt "${_text}\n")
  string(REGEX MATCH "^[^ ]+" _missing "${_last}")
  jb_expect_status(${_scene}_duplicate 1 "no stored hash for ${_missing}"
    COMMAND test_${_scene} ${CMAKE_CURRENT_BINARY_DIR}/${_scene}_duplicate.txt)
endforeach()

# The seed's scenes through the public API (api_golden.c says how each op
# is expressed and what is compared): every scene alone at the scale it
# draws at, and again with 3x for every scale above 1x.
jb_api_test(api_golden)
target_link_libraries(test_api_golden PRIVATE jb_trace)
add_test(NAME api_golden COMMAND test_api_golden ${_jb_seed_traces})
add_test(NAME api_golden_scale3 COMMAND test_api_golden --scale 3 ${_jb_seed_traces})

# exports: a shared build exports exactly the JB_API functions of the
# header; a static one defines them all and holds no writable data
# (tests/exports.cmake). Not under emscripten: WebAssembly memory has no
# read-only segments, so nm reports every constant (string literals,
# tables) as data, and "no writable data" cannot be read from the symbols.
# The native builds check it.
if(NOT EMSCRIPTEN)
  get_target_property(_jb_type jitterbug TYPE)
  add_test(NAME exports COMMAND ${CMAKE_COMMAND} -DLIB=$<TARGET_FILE:jitterbug> -DTYPE=${_jb_type}
    -DNM=${CMAKE_NM} -DHEADER=${PROJECT_SOURCE_DIR}/include/jitterbug.h
    -P ${CMAKE_CURRENT_SOURCE_DIR}/exports.cmake)
endif()

# Conformance against the console: ps1-tests' GPU programs
# (tests/conformance/CMakeLists.txt).
add_subdirectory(conformance)

# ---- GTE conformance (begin) ----------------------------------------------
# The console's GTE results from ps1-tests (third_party/ps1-tests/gte, its
# README.md; converted by tools/gte-cases), replayed through the public API
# by gte/conformance.c: every register and FLAG after each case, exactly,
# with the options off and again with precise_vertices and
# perspective_correct on (which must change no register).
#   gte_test_all   gte/test-all's 1150 cases (registers cleared before
#                  each). gte-fuzz's log holds the same cases in another
#                  order (tools/gte-cases checks that), so it is not kept.
# Strict C99 against <jitterbug.h> only. Runs everywhere, wasm32 too.
add_executable(test_gte_conformance gte/conformance.c)
set_target_properties(test_gte_conformance PROPERTIES
  C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(test_gte_conformance PRIVATE jitterbug jb_build_flags)
if(NOT MSVC)
  target_compile_options(test_gte_conformance PRIVATE -pedantic-errors)
endif()
# The test states what the file holds (cases 1..1150, 50 for each of the 22
# commands and 50 without one), and the runner refuses anything else:
#   gte_test_all_partial   the file cut to its first case: refused
#   gte_test_all_no_case   --case 1151, which is not a case: refused
# (each exit status 2 with its message, jb_expect_status)
set(_gte_all ${PROJECT_SOURCE_DIR}/third_party/ps1-tests/gte/test-all.txt)
set(_gte_counts --cases 1150 --per-command 50)
add_test(NAME gte_test_all COMMAND test_gte_conformance ${_gte_all} ${_gte_counts})
file(STRINGS ${_gte_all} _gte_lines)
set(_gte_cut "")
set(_gte_cases 0)
foreach(_line IN LISTS _gte_lines)
  if(_line MATCHES "^cases ")
    set(_line "cases 1")
  elseif(_line MATCHES "^case ")
    math(EXPR _gte_cases "${_gte_cases} + 1")
    if(_gte_cases GREATER 1)
      continue()
    endif()
  endif()
  string(APPEND _gte_cut "${_line}\n")
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gte-test-all-first-case.txt "${_gte_cut}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_gte_all})
jb_expect_status(gte_test_all_partial 2 "1 cases, not the 1150 stated"
  COMMAND test_gte_conformance
  ${CMAKE_CURRENT_BINARY_DIR}/gte-test-all-first-case.txt ${_gte_counts})
jb_expect_status(gte_test_all_no_case 2 "--case 1151: there is no such case"
  COMMAND test_gte_conformance
  ${_gte_all} ${_gte_counts} --case 1151)
# ---- GTE conformance (end) ------------------------------------------------

# ---- Determinism (docs/determinism.md) ------------------------------------
# determinism_threads: random GP0 streams made to need the queue's order, a
# flood of more primitives than the queue holds, the seed scenes in order,
# and jb-bench's workloads (the console-level first-light scene of
# tests/console_scene, heavy, fill), at 1x, 2x and 3x with
# the float options off and on, each at 1, 2, 3, 4, 8 and 16 threads and
# with the count changing as it draws, the GP0 words cut three ways. Every
# run must give the hash stored in determinism_threads.txt, the same file
# for every build. A thread request the library does not meet (it logs
# "started only N of M") fails the case. Single-threaded (JB_THREADS off,
# emscripten) every request is one thread, so it checks the same hashes on
# one and says so.
jb_api_test(determinism_threads
  ${PROJECT_SOURCE_DIR}/tools/jb-bench/firstlight.c
  ${PROJECT_SOURCE_DIR}/tools/jb-bench/heavy.c
  ${PROJECT_SOURCE_DIR}/tools/jb-bench/stats.c
  ${PROJECT_SOURCE_DIR}/tests/console_scene/scene.c)
target_include_directories(test_determinism_threads PRIVATE
  ${PROJECT_SOURCE_DIR}/tools/jb-bench ${PROJECT_SOURCE_DIR}/tests/console_scene)
target_link_libraries(test_determinism_threads PRIVATE jb_trace)
if(NOT JB_THREADS)
  target_compile_definitions(test_determinism_threads PRIVATE JB_TEST_SINGLE_THREADED)
endif()
add_test(NAME determinism_threads
  COMMAND test_determinism_threads ${CMAKE_CURRENT_SOURCE_DIR}/determinism_threads.txt
    ${_jb_seed_traces})

# console_scene: first-light as it was first written, on the console-level
# API alone (jb_vram_load, the GTE by hand, the program's own ordering
# table, jb_prim_*, GP1 words): the one whole program on that level, kept
# with the frame hashes it was recorded with (tests/console_scene/scene.h).
# Headless: tests/console_scene/main.c draws the runs of its expected.txt,
# at 1 thread and natively at 4.
if(NOT TARGET console_scene)
  add_library(console_scene STATIC ${PROJECT_SOURCE_DIR}/tests/console_scene/scene.c)
  target_include_directories(console_scene PUBLIC ${PROJECT_SOURCE_DIR}/tests/console_scene)
  target_link_libraries(console_scene PUBLIC jitterbug PRIVATE jb_build_flags)
  set_target_properties(console_scene PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endif()
add_executable(test_console_scene console_scene/main.c)
target_link_libraries(test_console_scene PRIVATE console_scene jb_build_flags)
set_target_properties(test_console_scene PROPERTIES
  C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(NOT MSVC)
  target_compile_options(test_console_scene PRIVATE -pedantic-errors)
endif()
add_test(NAME console_scene
  COMMAND test_console_scene --threads 1 --expect ${CMAKE_CURRENT_SOURCE_DIR}/console_scene/expected.txt)
if(JB_THREADS)
  add_test(NAME console_scene_4t
    COMMAND test_console_scene --threads 4 --expect ${CMAKE_CURRENT_SOURCE_DIR}/console_scene/expected.txt)
endif()
# ---- end of Determinism -----------------------------------------------------

# ---- Fuzzing (docs/testing.md, "Differential fuzzing") -------------------
# fuzz_robust: seeded crash fuzzing through the public API: any words,
# every command byte cut short (from a reset command buffer), the largest
# transfers cut short, random splits of the stream, mixed with GPUREAD, the
# jb_vram_* calls (in and out of range), jb_set_scale, jb_set_threads, the
# float options, the GTE, GP1 and jb_frame, from 1x and from 3x; and a batch
# of more primitives than the software backend queues, on four threads and
# on one. It needs no reference: under the sanitizer build it is the check
# for crashes and accesses outside VRAM. The differential half (against
# psxstack) is tools/fuzz-diff.
jb_api_test(fuzz_robust)
add_test(NAME fuzz_robust COMMAND test_fuzz_robust)
# line_clip: a rule differential fuzzing cannot referee (psxstack dithers
# no line; docs/testing.md, "What differential fuzzing cannot check"):
# every line, flat or Gouraud, dithered or not, clipped by the drawing area
# alone, against an exact
# evaluation, at 1x and 2x.
jb_api_test(line_clip)
add_test(NAME line_clip COMMAND test_line_clip)
# ---- end of Fuzzing ---------------------------------------------------------

# ---- Files and converters (docs/formats.md; tools/jb-tex, tools/jb-mesh) ----
# On the inputs in files/inputs (made by files/make-inputs.py) and the
# converters' pinned outputs in files/expected (docs/testing.md, "File,
# converter and loader tests"):
#   convert_OUTPUT  each line of files/conversions.txt: the converter run
#                   through CMAKE_CROSSCOMPILING_EMULATOR when there is one,
#                   its output byte for byte files/expected/OUTPUT, so every
#                   build (both compilers, the sanitizers, x86-64-v3, arm64,
#                   wasm32 under Node) converts alike
#   jb_tex_*, jb_mesh_*  what each converter refuses, with its message, and no
#                   sanitizer report (a leak on the way out included)
#   api_files       the loaders through the public API: the same VRAM as
#                   jb_texture_create, meshes, the round trip (the pinned
#                   files drawn, frames against stored hashes) and every
#                   refusal (api_files.c)
#   fuzz_files      the loaders on seeded mutations of the pinned files: no
#                   crash, no read outside them, no leak (fuzz_files.c); with
#                   -DJB_LIBFUZZER=ON and clang, also a libFuzzer target,
#                   fuzz_files_libfuzzer, run briefly over the pinned files
file(STRINGS files/conversions.txt _jb_conversions REGEX "^[^#]")
foreach(_line IN LISTS _jb_conversions)
  separate_arguments(_parts UNIX_COMMAND "${_line}")
  list(POP_FRONT _parts _out _tool)
  add_test(NAME convert_${_out}
    COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/files/${_out}
      -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/files/expected/${_out}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/files/convert.cmake --
      ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:${_tool}> ${_parts}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/files)
endforeach()
set(_in ${CMAKE_CURRENT_SOURCE_DIR}/files/inputs)
set(_ex ${CMAKE_CURRENT_SOURCE_DIR}/files/expected)
set(_to ${CMAKE_CURRENT_BINARY_DIR}/files/refused)
jb_expect_status(jb_tex_too_wide 1 "wide.png: 300 x 2 pixels; a texture is 1..256 each way"
  NOT "Sanitizer|runtime error"
  COMMAND jb-tex ${_in}/wide.png ${_to}.jbt)
jb_expect_status(jb_tex_repeat_size 1 "--repeat needs power-of-two sizes 8..256, not 64 x 24"
  NOT "Sanitizer|runtime error"
  COMMAND jb-tex --repeat ${_in}/sky.png ${_to}.jbt)
jb_expect_status(jb_tex_depth 1 "--depth 7: not 4, 8 or 15"
  NOT "Sanitizer|runtime error"
  COMMAND jb-tex --depth 7 ${_in}/sky.png ${_to}.jbt)
jb_expect_status(jb_tex_frame_15 1 "--frame needs a palette"
  NOT "Sanitizer|runtime error"
  COMMAND jb-tex --depth 15 --frame ${_in}/water1.png ${_in}/water0.png ${_to}.jbt)
jb_expect_status(jb_tex_frame_size 1 "sky.png: 64 x 24; a frame must be the input's size, 16 x 16"
  NOT "Sanitizer|runtime error"
  COMMAND jb-tex --frame ${_in}/sky.png ${_in}/water0.png ${_to}.jbt)
jb_expect_status(jb_tex_not_png 1 "box.obj: cannot read it as a PNG"
  NOT "Sanitizer|runtime error"
  COMMAND jb-tex ${_in}/box.obj ${_to}.jbt)
jb_expect_status(jb_tex_usage 1 "usage: jb-tex" NOT "Sanitizer|runtime error"
  COMMAND jb-tex ${_in}/sky.png)
jb_expect_status(jb_mesh_unknown_material 1 "no material is named \"marble\"; the materials are: \"stone\" \"glass\""
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh --texture marble=${_ex}/rocks8.jbt ${_in}/box.obj ${_to}.jbm)
jb_expect_status(jb_mesh_scale 1 "face 0, corner 0: x = -40 times the scale 1000 is outside the int16 positions"
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh --scale 1000 ${_in}/box.obj ${_to}.jbm)
jb_expect_status(jb_mesh_tiles_need_repeat 1 "outside the 32 x 32 texture .*0..1; a .jbt made with --repeat may tile"
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh --texture default=${_ex}/rocks8.jbt ${_in}/tiled.obj ${_to}.jbm)
jb_expect_status(jb_mesh_no_uv 1 "has no texture coordinates, but its material \"base\" is textured"
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh --texture base=${_ex}/tiles4.jbt ${_in}/pyramid.gltf ${_to}.jbm)
jb_expect_status(jb_mesh_input_kind 1 "the input must be .obj, .gltf or .glb"
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh ${_in}/box.mtl ${_to}.jbm)
jb_expect_status(jb_mesh_bad_index 1 "broken.obj:5: face corner \"9\""
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh ${_in}/broken.obj ${_to}.jbm)
jb_expect_status(jb_mesh_flat_face 1 "flat.obj:7: a face of 5 corners with no area"
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh ${_in}/flat.obj ${_to}.jbm)
jb_expect_status(jb_mesh_doubled_face 1 "doubled.obj:6: a face of 5 corners that cannot be cut into triangles"
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh ${_in}/doubled.obj ${_to}.jbm)
jb_expect_status(jb_mesh_huge_face 1 "huge.obj:5: a face of more than 1024 corners"
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh ${_in}/huge.obj ${_to}.jbm)
# The most corners jb-mesh cuts (a status 0 test, not a pinned output).
jb_expect_status(jb_mesh_most_corners 0 "1022 triangles"
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh --fit 1000 ${_in}/most.obj ${_to}-most.jbm)
jb_expect_status(jb_mesh_nan_face 1 "nan.obj:7: a face of 5 corners with no area"
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh --fit 1000 ${_in}/nan.obj ${_to}.jbm)
jb_expect_status(jb_mesh_unnamed 1 "unnamed.obj:5: usemtl needs a name"
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh ${_in}/unnamed.obj ${_to}.jbm)
jb_expect_status(jb_mesh_unnamed_mtl 1 "unnamed.mtl:2: newmtl needs a name"
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh ${_in}/unnamed-mtl.obj ${_to}.jbm)
jb_expect_status(jb_mesh_missing_texture 1 "nothing.png: cannot read it"
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh --texture stone=${_in}/nothing.png ${_in}/box.obj ${_to}.jbm)
jb_expect_status(jb_mesh_not_a_texture 1 "box.mtl: neither a .jbt nor a PNG"
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh --texture stone=${_in}/box.mtl ${_in}/box.obj ${_to}.jbm)
# Accessors of the wrong type, which cgltf_validate passes: a MAT4 COLOR_0
# (16 floats, more than a colour's room) and a VEC2 NORMAL.
jb_expect_status(jb_mesh_gltf_color_type 1 "primitive 0: its COLOR_0 is not VEC3 or VEC4"
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh ${_in}/badcolor.gltf ${_to}.jbm)
jb_expect_status(jb_mesh_gltf_normal_type 1 "primitive 0: its NORMAL is not VEC3"
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh ${_in}/badnormal.gltf ${_to}.jbm)
# An OBJ named .gltf: not JSON.
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/files)
file(COPY_FILE ${_in}/box.obj ${CMAKE_CURRENT_BINARY_DIR}/files/box-as.gltf)
jb_expect_status(jb_mesh_bad_gltf 1 "box-as.gltf: (invalid JSON|not glTF or GLB)"
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh ${CMAKE_CURRENT_BINARY_DIR}/files/box-as.gltf ${_to}.jbm)
jb_expect_status(jb_mesh_blend 1 "the mode is none, half, add, sub or quarter"
  NOT "Sanitizer|runtime error"
  COMMAND jb-mesh --blend glass=lighten ${_in}/box.obj ${_to}.jbm)

jb_api_test(api_files)
target_include_directories(test_api_files PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME api_files COMMAND test_api_files ${CMAKE_CURRENT_SOURCE_DIR}/files)
jb_api_test(fuzz_files)
target_include_directories(test_fuzz_files PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME fuzz_files COMMAND test_fuzz_files ${CMAKE_CURRENT_SOURCE_DIR}/files)
option(JB_LIBFUZZER "Build the loaders' libFuzzer target (clang only)" OFF)
if(JB_LIBFUZZER)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "JB_LIBFUZZER needs clang")
  endif()
  add_executable(fuzz_files_libfuzzer fuzz_files_libfuzzer.c)
  target_include_directories(fuzz_files_libfuzzer PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(fuzz_files_libfuzzer PRIVATE jitterbug jb_build_flags)
  target_compile_options(fuzz_files_libfuzzer PRIVATE -fsanitize=fuzzer)
  target_link_options(fuzz_files_libfuzzer PRIVATE -fsanitize=fuzzer)
  # libFuzzer adds what it finds to the first directory: one in the build,
  # never the pinned files, which it only reads.
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/files/corpus)
  add_test(NAME fuzz_files_libfuzzer
    COMMAND fuzz_files_libfuzzer -runs=200000 -seed=1 ${CMAKE_CURRENT_BINARY_DIR}/files/corpus
      ${CMAKE_CURRENT_SOURCE_DIR}/files/expected)
endif()
# ---- end of Files and converters --------------------------------------------

# ---- Hardware probe (tools/probe) -------------------------------------------
# The probe's player and protocol (tools/probe/probe/probe.c) and the host's
# side (tools/probe/host) built for the host, and run against each other.
# The lists are the conformance programs' (tests/conformance/fixtures.txt:
# the ten in the repository always, the three fetched ones when
# JB_CONFORMANCE_FETCHED is set, as for the conformance tests, and a status
# message names those left out) and the seven open-question lists
# (tests/conformance/open/fixtures.txt). Each is played as its fixture
# says: to pass PASSES of its main loop, then a window of two passes that
# must not change VRAM (tests/conformance/FORMAT.md, "Playing a list").
#   probe_sim_NAME, probe_sim_open_NAME
#                      every list played by the probe's own code over a HAL
#                      whose GPU is a jitterbug context (GP0, GP1, GPUREAD
#                      and GPUSTAT only) and whose serial line is a loopback
#                      to the host's side; the result (all 16 bits of VRAM,
#                      the reads, checks, waits, the main loop's summary)
#                      must equal jitterbug's own player's (tests/probe/sim.c)
#   probe_sim_faults_* the same with bytes flipped or dropped on the line,
#                      each way: detected, sent again, the result unchanged
#   probe_sim_naks     a damaged frame each way, answered by a NAK
#   probe_sim_refuse_* a list whose CRC-32 does not match, one that is not a
#                      list, a main loop without a pass, a pass without one:
#                      the probe refuses them
#   probe_sim_late_loop, probe_sim_early
#                      the main loop's rule on the console's side: a loop
#                      that draws only in pass 3, compared at pass 1, and a
#                      list compared at a pass after it had settled; the
#                      probe reports each as jitterbug's player does and the
#                      host refuses the result
#   probe_sim_readback_*  a GPU that never becomes ready for the read-back
#                      or never offers VRAM (GPUSTAT bits 26/28, 27), at a
#                      main loop checkpoint and after the list: the probe
#                      fails the session and sends no R_DONE
#   probe_sim_prepare_* a GPU unready during the preparation before the
#                      list only: the probe fails the session before the
#                      list (JBP_E_PREPARE), or waits when it is ready in time
#   probe_sim_dma_stall a DMA that takes longer than the host's ACK
#                      interval: the probe's PROGRESS keeps the host from
#                      sending again; one that never ends is stopped
#   probe_sim_dma_direction, probe_sim_dma_direction_error
#                      a DMA started with direction 1 fails the run, in a
#                      session that completes and in one refused
#   probe_sim_frames   START, LIST, ACK and NAK frames well and badly made,
#                      sent to the probe's own code, idle and in a session,
#                      and a host that never lets the line go idle
#   probe_sim_packets  the list reader (tests/conformance/jbc.c) refusing a
#                      list whose LOOP or END is inside a GPU packet
#   probe_sim_fuzz     sessions made and damaged at random (seeded): the
#                      probe sends R_DONE only for the list START announced,
#                      and a flooding host does not hold it
#   probe_host_frames, probe_host_files, probe_host_fuzz
#                      the host's side against bad control frames, results
#                      of another list, a result frame with the last seq
#                      that is not the last frame, a probe that never lets
#                      the line go idle, and result files damaged by
#                      hand and at random (tests/probe/host.c)
#   probe_runner_NAME, probe_runner_open_NAME
#                      the conformance runner with each simulated result as
#                      its reference, bound to the list's fixture as a
#                      console's result is (the runner reads the fixture
#                      from the table itself, jb_probe_reference below):
#                      all 16 bits, no exclusion. Both sides are jitterbug,
#                      so these test the binding and the comparison, not a
#                      GPU; probe_runner_bad_* show that they can fail
#   probe_runner_bad_* the same binding refusing (exit status 2) a result of
#                      another list, of another pass, from another platform,
#                      damaged or cut short, not clean (a WAIT, DMA or
#                      vertical blank that timed out, a check not the
#                      list's: probe_sim_unclean*), a simulated
#                      result named as a console's (--probe-fixtures), and
#                      finding (1) one wrong mask or colour bit or a check
#                      the console failed
#   probe_cli_*        jbprobe info, to-png and compare, compare refusing an
#                      unclean result that info and to-png still read
# Every test whose outcome is a refusal or a difference, and those judged by
# their output, require their exact exit status and the diagnostic
# (jb_expect_status, tests/conformance/CMakeLists.txt).
#   probe_pty_*        jbprobe send over a pseudo-terminal
#   probe_line_*       jbprobe send against a line that never takes its
#                      output, or a connection never made: it gives up at
#                      its time limit; an ACK of the START that
#                      comes after its limit is not taken
#   conformance_probe_NAME  each console result in tests/conformance/probe,
#                      by its line in tests/conformance/probe/fixtures.txt
#                      (none yet: docs/testing.md, "`tests/conformance/probe/`")
add_library(jb_probe_host STATIC
  ${PROJECT_SOURCE_DIR}/tools/probe/common/jbp_frame.c
  ${PROJECT_SOURCE_DIR}/tools/probe/host/jbp_result.c
  ${PROJECT_SOURCE_DIR}/tools/probe/host/jbh_session.c)
set_target_properties(jb_probe_host PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(jb_probe_host PUBLIC
  ${PROJECT_SOURCE_DIR}/tools/probe/common ${PROJECT_SOURCE_DIR}/tools/probe/host)
target_link_libraries(jb_probe_host PRIVATE jb_build_flags)

add_executable(test_probe_sim probe/sim.c ${PROJECT_SOURCE_DIR}/tools/probe/probe/probe.c)
set_target_properties(test_probe_sim PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(test_probe_sim PRIVATE ${PROJECT_SOURCE_DIR}/tools/probe/probe)
target_link_libraries(test_probe_sim PRIVATE jb_probe_host jb_jbc jitterbug jb_build_flags)
target_link_libraries(test_conformance_conformance PRIVATE jb_probe_host)
add_executable(test_probe_host probe/host.c)
set_target_properties(test_probe_host PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(test_probe_host PRIVATE jb_probe_host jb_build_flags)

set(_probe_out ${CMAKE_CURRENT_BINARY_DIR}/probe-out)
file(MAKE_DIRECTORY ${_probe_out})

# The open-question lists: open/fixtures.txt, one line for each open/*.jbc.
set(_probe_open_dir ${PROJECT_SOURCE_DIR}/tests/conformance/open)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_probe_open_dir}/fixtures.txt)
file(STRINGS ${_probe_open_dir}/fixtures.txt _probe_lines)
set(_probe_open)
foreach(_line ${_probe_lines})
  if(_line MATCHES "^#" OR _line STREQUAL "")
    continue()
  endif()
  string(REGEX REPLACE " +" ";" _f "${_line}")
  list(LENGTH _f _n)
  if(NOT _n EQUAL 7)
    message(FATAL_ERROR "tests/conformance/open/fixtures.txt: bad line: ${_line}")
  endif()
  list(GET _f 0 _p)
  list(GET _f 1 _where)
  list(GET _f 2 _ref)
  list(GET _f 3 _passes)
  list(GET _f 4 _expects)
  list(GET _f 5 _words)
  list(GET _f 6 _sum)
  if(NOT _where STREQUAL "open" OR NOT _ref STREQUAL "probe" OR NOT _passes MATCHES "^[0-9]+$" OR
     NOT _expects MATCHES "^[0-9]+$" OR NOT _words MATCHES "^[0-9]+$" OR NOT _sum MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "tests/conformance/open/fixtures.txt: bad line: ${_line}")
  endif()
  if(NOT EXISTS ${_probe_open_dir}/${_p}.jbc)
    message(FATAL_ERROR "tests/conformance/open/fixtures.txt: ${_p}.jbc is missing")
  endif()
  list(APPEND _probe_open ${_p})
  set(_probe_open_${_p}_id --fixtures ${_probe_open_dir}/fixtures.txt --fixture ${_p})
  set(_probe_open_${_p}_passes ${_passes})
endforeach()
file(GLOB _probe_found RELATIVE ${_probe_open_dir} CONFIGURE_DEPENDS ${_probe_open_dir}/*.jbc)
foreach(_f ${_probe_found})
  string(REGEX REPLACE "\\.jbc$" "" _p "${_f}")
  if(NOT _p IN_LIST _probe_open)
    message(FATAL_ERROR "tests/conformance/open/${_f} is not in tests/conformance/open/fixtures.txt")
  endif()
endforeach()

# A simulated probe result as the runner's reference, bound to its list's
# fixture: the runner is given the table and the name (ID: --fixtures TABLE
# --fixture NAME) and reads the list's identity and pass from the table
# itself, the result and the platform it must come from, and never an
# exclusion. Console results (conformance_probe_*, below) name their own
# result and exclusion through tests/conformance/probe/fixtures.txt.
function(jb_probe_reference test list result platform)
  cmake_parse_arguments(PARSE_ARGV 4 _r "" "" "ID;EXTRA")
  add_test(NAME ${test}
    COMMAND test_conformance_conformance ${_r_ID} --probe ${result} --platform ${platform}
      ${_r_EXTRA} ${list})
endfunction()

# Every list this build has, played on the simulated probe; the result is
# then the reference of probe_runner_*.
set(_probe_sims)
foreach(_p ${JBC_PROGRAMS})
  if(_p IN_LIST JBC_MISSING)
    continue()
  endif()
  list(APPEND _probe_sims ${_p})
  add_test(NAME probe_sim_${_p}
    COMMAND test_probe_sim --passes ${JBC_${_p}_PASSES} --save ${_probe_out}/${_p}.jbpr
      ${JBC_${_p}_LIST})
  set_tests_properties(probe_sim_${_p} PROPERTIES FIXTURES_SETUP probe_${_p})
  jb_probe_reference(probe_runner_${_p} ${JBC_${_p}_LIST} ${_probe_out}/${_p}.jbpr simulation
    ID ${JBC_${_p}_ID})
  set_tests_properties(probe_runner_${_p} PROPERTIES FIXTURES_REQUIRED probe_${_p})
endforeach()
foreach(_p ${_probe_open})
  add_test(NAME probe_sim_open_${_p}
    COMMAND test_probe_sim --passes ${_probe_open_${_p}_passes} --save ${_probe_out}/open-${_p}.jbpr
      ${_probe_open_dir}/${_p}.jbc)
  set_tests_properties(probe_sim_open_${_p} PROPERTIES FIXTURES_SETUP probe_open_${_p})
  jb_probe_reference(probe_runner_open_${_p} ${_probe_open_dir}/${_p}.jbc
    ${_probe_out}/open-${_p}.jbpr simulation ID ${_probe_open_${_p}_id})
  set_tests_properties(probe_runner_open_${_p} PROPERTIES FIXTURES_REQUIRED probe_open_${_p})
endforeach()
if(JBC_MISSING)
  list(JOIN JBC_MISSING ", " _m)
  message(STATUS "probe: not built for ${_m} (probe_sim_*, probe_runner_*): their lists are "
    "fetched (JB_CONFORMANCE_FETCHED, as for the conformance tests)")
endif()
# No test of these may pass for want of lists.
list(LENGTH _probe_sims _n_sims)
list(LENGTH _probe_open _n_open)
if(_n_sims LESS 10 OR _n_open LESS 7)
  message(FATAL_ERROR "probe: ${_n_sims} ps1-tests lists and ${_n_open} open lists found; "
    "at least 10 and 7 are in the repository")
endif()

# Faults: host to probe within the START and the first list frame, and in
# an ACK of the results; probe to host within the summary and VRAM frames
# and in an ACK of the list.
set(_probe_faults
  "h2p:flip:20;h2p:flip:300;h2p:drop:900;p2h:flip:700;p2h:drop:5000;p2h:flip:400000;p2h:drop:900000")
set(_probe_fault_args)
foreach(_f ${_probe_faults})
  list(APPEND _probe_fault_args --fault ${_f})
endforeach()
foreach(_p triangle mask-bit clut-cache)
  add_test(NAME probe_sim_faults_${_p}
    COMMAND test_probe_sim --passes ${JBC_${_p}_PASSES} ${_probe_fault_args} ${JBC_${_p}_LIST})
endforeach()
# probe_sim_naks: one byte flipped in the first LIST frame (h2p:flip:300)
# and one in a VRAM frame (p2h:flip:400000): each side must answer the
# damaged frame with a NAK (--expect-naks), not only recover by its timer.
add_test(NAME probe_sim_naks
  COMMAND test_probe_sim --passes 1 --fault h2p:flip:300 --fault p2h:flip:400000 --expect-naks
    ${JBC_triangle_LIST})
add_test(NAME probe_sim_refuse_crc
  COMMAND test_probe_sim --passes 1 --lie-crc --expect-error 6 ${JBC_triangle_LIST})
add_test(NAME probe_sim_refuse_magic
  COMMAND test_probe_sim --passes 1 --bad-magic --expect-error 1 ${JBC_triangle_LIST})
# JBP_E_PASSES (9): a main loop with no pass to compare at; a pass for a
# list without a main loop.
add_test(NAME probe_sim_refuse_no_pass
  COMMAND test_probe_sim --passes 0 --expect-error 9 ${JBC_triangle_LIST})
add_test(NAME probe_sim_refuse_pass_without_loop
  COMMAND test_probe_sim --passes 1 --expect-error 9 ${_probe_open_dir}/quad-order.jbc)
# The main loop's rule: the late loop is unchanged by pass 2 and changed by
# pass 3, so "until a pass leaves VRAM unchanged" would stop at pass 1 and
# take it; with the window the probe reports pass 3 and the host refuses.
# triangle settles in pass 1: stated at pass 2, pass 2 changes nothing.
# test_probe_sim --expect-refused exits 0 when the host refused the result
# as required: the status and the reason are both required.
jb_expect_status(probe_sim_late_loop 0
  "refused as expected: the main loop is not at a fixed point at pass 1: pass 3 .*\nOK\n"
  COMMAND test_probe_sim --passes 1 --late-loop --expect-refused)
jb_expect_status(probe_sim_early 0 "refused as expected: the main loop had settled before pass 2.*\nOK\n"
  COMMAND test_probe_sim --passes 2 --expect-refused ${JBC_triangle_LIST})

# Read-backs the GPU never allows: the simulated GPU stops
# showing ready for a command (GPUSTAT bits 26 and 28: never-idle) or VRAM
# ready to read (bit 27: never-read, with GPUREAD then stale) once K + 1
# read-back commands (GP0(C0h) and its two parameters) have been written.
# triangle
# (P 1, window 2) reads VRAM back after passes 1, 2 and 3 (256 commands
# each), then for the results: K 300 is at a main loop checkpoint, 768
# after the list, in the results' read-back; 767 from the last command of
# the last checkpoint, so that the ready wait after the list (restore)
# fails, before the summary is sent (--expect-no-results). quad-order has
# no main loop. Each must fail with JBP_E_NOT_IDLE (11) or JBP_E_READBACK
# (12), no R_DONE, and no GP0 word may reach the GPU while it shows not
# ready (test_probe_sim checks that in every run).
foreach(_c "idle_checkpoint;never-idle:300;11;triangle" "idle_end;never-idle:768;11;triangle"
    "idle_restore;never-idle:767;11;triangle;--expect-no-results"
    "read_checkpoint;never-read:300;12;triangle" "read_end;never-read:768;12;triangle")
  list(GET _c 0 _what)
  list(GET _c 1 _fault)
  list(GET _c 2 _code)
  list(GET _c 3 _p)
  set(_more)
  list(LENGTH _c _n)
  if(_n GREATER 4)
    list(GET _c 4 _more)
  endif()
  add_test(NAME probe_sim_readback_${_what}
    COMMAND test_probe_sim --passes ${JBC_${_p}_PASSES} --gpu ${_fault} --expect-error ${_code} ${_more}
      ${JBC_${_p}_LIST})
endforeach()
add_test(NAME probe_sim_readback_no_loop
  COMMAND test_probe_sim --passes 0 --gpu never-read:0 --expect-error 12 ${_probe_open_dir}/quad-order.jbc)
# A GPU unready during the preparation before the list only: GPUSTAT
# bits 26 and 28 clear for MS ms from the probe's GP1(00h) (prep-busy) or
# from the last word of its fills (fill-busy), ready again while the list
# plays. Longer than the probe's 1 s wait, the session fails with
# JBP_E_PREPARE (15) before any word of the list reaches the GPU; shorter,
# the probe waits and the session completes as jitterbug's player.
foreach(_c "prepare_busy;prep-busy:1500;15" "prepare_fill_busy;fill-busy:1500;15"
    "prepare_slow;prep-busy:500;" "prepare_fill_slow;fill-busy:500;")
  list(GET _c 0 _what)
  list(GET _c 1 _fault)
  list(GET _c 2 _code)
  if(_code STREQUAL "")
    set(_expect)
  else()
    set(_expect --expect-error ${_code})
  endif()
  add_test(NAME probe_sim_${_what}
    COMMAND test_probe_sim --passes ${JBC_triangle_PASSES} --gpu ${_fault} ${_expect} ${JBC_triangle_LIST})
endforeach()
# A DMA busy for 10 s with --wait-ms 20000: longer than the host's
# 3 s ACK interval; the probe's PROGRESS while it polls must keep the host
# from sending the list frame again, and it must not be stopped. One that
# never ends is stopped (hal_gp0_dma_stop, exactly once a transfer)
# after the wait limit and counted, and the session still completes.
add_test(NAME probe_sim_dma_stall
  COMMAND test_probe_sim --passes 1 --gpu dma-stall:10000 --wait-ms 20000 --expect-no-resend
    ${JBC_triangle_LIST})
add_test(NAME probe_sim_dma_never_ends
  COMMAND test_probe_sim --passes 1 --gpu dma-stall:0 --wait-ms 5000 --expect-no-resend
    ${JBC_triangle_LIST})
# A DMA started with GPUSTAT's DMA direction 1, not 2
# (--dma-direction-1): the run fails (exit status 1, the reason in the
# output) in a normal run and in one where the probe is to refuse the
# session after that DMA (never-idle:300, error 11) alike.
jb_expect_status(probe_sim_dma_direction 1 "\nFAIL: 1 DMA transfers with GPUSTAT's DMA direction not 2"
  COMMAND test_probe_sim --passes 1 --dma-direction-1 ${JBC_triangle_LIST})
jb_expect_status(probe_sim_dma_direction_error 1
  "\nFAIL: 1 DMA transfers with GPUSTAT's DMA direction not 2.*\nrefused as expected: error 11"
  COMMAND test_probe_sim --passes 1 --dma-direction-1 --gpu never-idle:300 --expect-error 11
    ${JBC_triangle_LIST})
# These print OK last and FAIL for a failed case: exit status 0, OK and no
# FAIL are all required.
foreach(_c "probe_sim_frames;test_probe_sim;--frames" "probe_sim_packets;test_probe_sim;--packets"
    "probe_sim_fuzz;test_probe_sim;--fuzz;1;300" "probe_host_frames;test_probe_host;--frames"
    "probe_host_files;test_probe_host;--files;${_probe_out}/mask-bit.jbpr"
    "probe_host_fuzz;test_probe_host;--fuzz;1;300;${_probe_out}/mask-bit.jbpr")
  list(POP_FRONT _c _t)
  jb_expect_status(${_t} 0 "\nOK\n$" NOT "FAIL" COMMAND ${_c})
endforeach()
set_tests_properties(probe_host_files probe_host_fuzz PROPERTIES FIXTURES_REQUIRED probe_mask-bit)

# The binding refusing what it must (exit status 2, the reason in the
# output) and finding one wrong bit (exit status 1): results made from
# triangle's and mask-bit's by probe_sim_tampered.
add_test(NAME probe_sim_tampered
  COMMAND test_probe_sim --passes 1
    --save-tampered mask-bit ${_probe_out}/bad-mask-bit.jbpr
    --save-tampered colour-bit ${_probe_out}/bad-colour-bit.jbpr
    --save-tampered raw-bit ${_probe_out}/bad-raw-bit.jbpr
    --save-tampered passes ${_probe_out}/bad-passes.jbpr
    --save-tampered truncated ${_probe_out}/bad-truncated.jbpr
    ${JBC_triangle_LIST})
set_tests_properties(probe_sim_tampered PROPERTIES FIXTURES_SETUP probe_tampered)
# Each with the exit status it requires: 2 for a result refused, 1 for one
# that differs (expect-status.cmake).
function(jb_probe_bad what needs result p platform status expect)
  jb_expect_status(probe_runner_bad_${what} ${status} "${expect}"
    COMMAND test_conformance_conformance ${JBC_${p}_ID} --probe ${result} --platform ${platform}
      ${JBC_${p}_LIST})
  set_tests_properties(probe_runner_bad_${what} PROPERTIES FIXTURES_REQUIRED ${needs})
endfunction()
jb_probe_bad(wrong_list probe_triangle ${_probe_out}/triangle.jbpr quad simulation 2
  "ERROR [^\n]*: a result of gpu/triangle [^\n]*not of this list")
jb_probe_bad(wrong_pass probe_tampered ${_probe_out}/bad-passes.jbpr triangle simulation 2
  "ERROR [^\n]*: played to pass 2 with a window of 2; the fixture triangle says pass 1")
jb_probe_bad(wrong_platform probe_triangle ${_probe_out}/triangle.jbpr triangle console 2
  "ERROR [^\n]*: from the host simulation \\(platform 2\\), and the reference must be from a console")
jb_probe_bad(damaged probe_tampered ${_probe_out}/bad-raw-bit.jbpr triangle simulation 2
  "ERROR [^\n]*: VRAM CRC-32 0x")
jb_probe_bad(truncated probe_tampered ${_probe_out}/bad-truncated.jbpr triangle simulation 2
  "ERROR [^\n]*: a record is cut short or malformed")
jb_probe_bad(mask_bit probe_tampered ${_probe_out}/bad-mask-bit.jbpr triangle simulation 1
  "\n  \\(0, 0\\): ours 0x7fff, console 0xffff\nVRAM: 1 pixels differ")
jb_probe_bad(colour_bit probe_tampered ${_probe_out}/bad-colour-bit.jbpr triangle simulation 1
  "\n  \\(0, 0\\): ours 0x7fff, console 0x7ffe\nVRAM: 1 pixels differ")
# A result that is not clean is no reference, whatever its VRAM and
# reads: a WAIT that timed out (a forged result, made against the
# current file format), a DMA transfer or a vertical blank that timed out,
# a check of another list. The console failing a check is not a refusal
# but a failure (1), with the check named.
foreach(_c "wait_timeout;2;ERROR [^\n]*: no reference: not clean: 1 of [0-9]+ waits \\(WAIT, DRAWSYNC\\) timed out, the first at word"
    "dma_timeout;2;ERROR [^\n]*: no reference: not clean: 1 of 1 DMA transfers timed out"
    "vsync_timeout;2;ERROR [^\n]*: no reference: not clean: 1 of 3 vertical blanks timed out"
    "other_check;2;ERROR [^\n]*: no reference: its check 0 [^\n]* is not the list's"
    "console_check;1;THE CONSOLE FAILED check 1 [^\n]*\n.*mask-bit: DIFFERS from the console's result, but the console failed [0-9]+ of the list's checks")
  list(GET _c 0 _what)
  list(GET _c 1 _status)
  list(GET _c 2 _expect)
  if(_what MATCHES "check")
    set(_p mask-bit)
  else()
    set(_p triangle)
  endif()
  jb_probe_bad(${_what} probe_unclean ${_probe_out}/unclean-${_what}.jbpr ${_p} simulation
    ${_status} "${_expect}")
endforeach()
add_test(NAME probe_sim_unclean
  COMMAND test_probe_sim --passes 1
    --save-tampered wait-timeout ${_probe_out}/unclean-wait_timeout.jbpr
    --save-tampered dma-timeout ${_probe_out}/unclean-dma_timeout.jbpr
    --save-tampered vsync-timeout ${_probe_out}/unclean-vsync_timeout.jbpr
    ${JBC_triangle_LIST})
add_test(NAME probe_sim_unclean_checks
  COMMAND test_probe_sim --passes 1
    --save-tampered other-check ${_probe_out}/unclean-other_check.jbpr
    --save-tampered console-check ${_probe_out}/unclean-console_check.jbpr
    ${JBC_mask-bit_LIST})
set_tests_properties(probe_sim_unclean probe_sim_unclean_checks PROPERTIES FIXTURES_SETUP probe_unclean)
# An unclean capture stays readable for diagnosis (jbprobe info, below).
# A dump is not an option beside a probe result (or at all: the table
# names the dump).
jb_expect_status(probe_runner_bad_with_dump 2 "ERROR --dump: the fixture table states"
  COMMAND test_conformance_conformance ${JBC_triangle_ID}
    --dump ${PROJECT_SOURCE_DIR}/third_party/ps1-tests/gpu/triangle/vram.png
    --probe ${_probe_out}/triangle.jbpr --platform simulation ${JBC_triangle_LIST})
set_tests_properties(probe_runner_bad_with_dump PROPERTIES FIXTURES_REQUIRED probe_triangle)

# Console results kept as references (docs/testing.md,
# "`tests/conformance/probe/`"):
# each NAME.jbpr there has a line in tests/conformance/probe/fixtures.txt,
#   NAME TABLE [EXCLUDE PIXELS CHECKSUM]
# TABLE ps1-tests: the program NAME of tests/conformance/fixtures.txt;
# open: the list NAME of tests/conformance/open/fixtures.txt. That line is
# the fixture the result is bound to; the exclusion (a file there, its
# pixel count and checksum, as in fixtures.txt) is the only one it may
# have, and none by default: a dump's exclusion is not a probe result's.
set(_probe_refs_dir ${PROJECT_SOURCE_DIR}/tests/conformance/probe)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_probe_refs_dir}/fixtures.txt)
file(STRINGS ${_probe_refs_dir}/fixtures.txt _probe_lines)
set(_probe_refs_named)
set(_probe_refs_missing)
foreach(_line ${_probe_lines})
  if(_line MATCHES "^#" OR _line STREQUAL "")
    continue()
  endif()
  string(REGEX REPLACE " +" ";" _f "${_line}")
  list(LENGTH _f _n)
  if(NOT (_n EQUAL 2 OR _n EQUAL 5))
    message(FATAL_ERROR "tests/conformance/probe/fixtures.txt: bad line: ${_line}")
  endif()
  list(GET _f 0 _p)
  list(GET _f 1 _table)
  if(_n EQUAL 5)
    list(GET _f 2 _ex)
    list(GET _f 3 _expix)
    list(GET _f 4 _exsum)
    if(NOT EXISTS ${_probe_refs_dir}/${_ex})
      message(FATAL_ERROR "tests/conformance/probe/fixtures.txt: ${_ex} is missing")
    endif()
    list(APPEND _probe_refs_named ${_ex})
  endif()
  if(NOT EXISTS ${_probe_refs_dir}/${_p}.jbpr)
    message(FATAL_ERROR "tests/conformance/probe/fixtures.txt: ${_p}.jbpr is missing")
  endif()
  list(APPEND _probe_refs_named ${_p}.jbpr)
  if(_table STREQUAL "ps1-tests" AND _p IN_LIST JBC_PROGRAMS)
    if(_p IN_LIST JBC_MISSING)
      list(APPEND _probe_refs_missing ${_p})
      continue()
    endif()
    set(_list ${JBC_${_p}_LIST})
  elseif(_table STREQUAL "open" AND _p IN_LIST _probe_open)
    set(_list ${_probe_open_dir}/${_p}.jbc)
  else()
    message(FATAL_ERROR "tests/conformance/probe/fixtures.txt: ${_p} is not a list of ${_table}")
  endif()
  file(MAKE_DIRECTORY ${_probe_out}/console)
  # The runner reads the result (NAME.jbpr beside the table), the fixture
  # its line names, the platform (a console) and the exclusion from the
  # tables itself.
  add_test(NAME conformance_probe_${_p}
    COMMAND test_conformance_conformance --probe-fixtures ${_probe_refs_dir}/fixtures.txt
      --fixture ${_p} --out ${_probe_out}/console ${_list})
endforeach()
# The same mode on a table of its own in the build tree, naming a simulated
# result: refused (2), since a console result must come from a console.
set(_probe_ctable ${_probe_out}/console-table)
file(MAKE_DIRECTORY ${_probe_ctable}/probe)
file(COPY_FILE ${PROJECT_SOURCE_DIR}/tests/conformance/fixtures.txt ${_probe_ctable}/fixtures.txt)
file(WRITE ${_probe_ctable}/probe/fixtures.txt "triangle ps1-tests\n")
add_test(NAME probe_sim_console_table
  COMMAND test_probe_sim --passes 1 --save ${_probe_ctable}/probe/triangle.jbpr ${JBC_triangle_LIST})
set_tests_properties(probe_sim_console_table PROPERTIES FIXTURES_SETUP probe_console_table)
jb_expect_status(probe_runner_bad_console_table 2
  "ERROR [^\n]*triangle.jbpr: from the host simulation \\(platform 2\\), and the reference must be from a console"
  COMMAND test_conformance_conformance --probe-fixtures ${_probe_ctable}/probe/fixtures.txt
    --fixture triangle ${JBC_triangle_LIST})
set_tests_properties(probe_runner_bad_console_table PROPERTIES FIXTURES_REQUIRED probe_console_table)
file(GLOB _probe_found RELATIVE ${_probe_refs_dir} CONFIGURE_DEPENDS
  ${_probe_refs_dir}/*.jbpr ${_probe_refs_dir}/*.exclude)
foreach(_f ${_probe_found})
  if(NOT _f IN_LIST _probe_refs_named)
    message(FATAL_ERROR "tests/conformance/probe/${_f} is not in tests/conformance/probe/fixtures.txt")
  endif()
endforeach()
if(_probe_refs_missing)
  list(JOIN _probe_refs_missing ", " _m)
  message(STATUS "probe: console results not tested for ${_m}: their lists are fetched "
    "(JB_CONFORMANCE_FETCHED)")
endif()

# jbprobe (tools/probe/host/jbprobe.c), the host's tool: POSIX (a tty or
# TCP), so not under emscripten. Its compare is the runner itself, built in
# with main renamed. probe_cli_*: info, to-png and compare on simulated
# results, compare bound to a fixture table as the tests are.
if(UNIX AND NOT EMSCRIPTEN)
  add_library(jb_probe_runner OBJECT conformance/conformance.c)
  set_target_properties(jb_probe_runner PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_compile_definitions(jb_probe_runner PRIVATE main=jbc_conformance_main)
  target_link_libraries(jb_probe_runner PRIVATE jb_jbc jb_probe_host jb_build_flags)
  target_compile_options(jb_probe_runner PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/probe/runner_entry.h)
  add_executable(jbprobe ${PROJECT_SOURCE_DIR}/tools/probe/host/jbprobe.c $<TARGET_OBJECTS:jb_probe_runner>)
  set_target_properties(jbprobe PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_link_libraries(jbprobe PRIVATE jb_probe_host jb_jbc jb_stb jitterbug jb_build_flags)
  file(MAKE_DIRECTORY ${_probe_out}/png)
  add_test(NAME probe_cli_info COMMAND jbprobe info ${_probe_out}/mask-bit.jbpr)
  add_test(NAME probe_cli_to_png COMMAND jbprobe to-png ${_probe_out}/mask-bit.jbpr ${_probe_out}/png
    ${JBC_mask-bit_LIST})
  jb_expect_status(probe_cli_compare 0 "\nclut-cache: MATCHES\n"
    COMMAND jbprobe compare ${_probe_out}/clut-cache.jbpr
      ${JBC_clut-cache_LIST} --fixtures ${PROJECT_SOURCE_DIR}/tests/conformance/fixtures.txt
      --platform simulation --out ${_probe_out})
  jb_expect_status(probe_cli_compare_open 0 "\nquad-order: MATCHES\n"
    COMMAND jbprobe compare ${_probe_out}/open-quad-order.jbpr
      ${_probe_open_dir}/quad-order.jbc --fixtures ${_probe_open_dir}/fixtures.txt
      --platform simulation --out ${_probe_out})
  set_tests_properties(probe_cli_info probe_cli_to_png PROPERTIES FIXTURES_REQUIRED probe_mask-bit)
  # A file that does not parse, and one that parses and is not sound: each
  # refused, with nothing left allocated (LeakSanitizer in the
  # sanitizer build reports into the output, which fails the test).
  jb_expect_status(probe_cli_info_malformed 2 "a record is cut short or malformed"
    COMMAND jbprobe info ${_probe_out}/bad-truncated.jbpr)
  jb_expect_status(probe_cli_to_png_unsound 2 "VRAM CRC-32 0x"
    COMMAND jbprobe to-png ${_probe_out}/bad-raw-bit.jbpr ${_probe_out}/png)
  set_tests_properties(probe_cli_info_malformed probe_cli_to_png_unsound PROPERTIES
    FIXTURES_REQUIRED probe_tampered FAIL_REGULAR_EXPRESSION "Sanitizer|runtime error")
  set_tests_properties(probe_cli_compare PROPERTIES FIXTURES_REQUIRED probe_clut-cache)
  set_tests_properties(probe_cli_compare_open PROPERTIES FIXTURES_REQUIRED probe_open_quad-order)
  # jbprobe compare applies the runner's rules: an unclean capture
  # is refused there (2), and jbprobe info and to-png still read it.
  jb_expect_status(probe_cli_compare_unclean 2 "no reference: not clean: 1 of [0-9]+ waits"
    COMMAND jbprobe compare ${_probe_out}/unclean-wait_timeout.jbpr ${JBC_triangle_LIST}
      --fixtures ${PROJECT_SOURCE_DIR}/tests/conformance/fixtures.txt --platform simulation)
  jb_expect_status(probe_cli_info_unclean 0
    "waits: [0-9]+, 1 timed out.*complete, but NOT usable as a reference: not clean"
    COMMAND jbprobe info ${_probe_out}/unclean-wait_timeout.jbpr)
  file(MAKE_DIRECTORY ${_probe_out}/png-unclean)
  jb_expect_status(probe_cli_to_png_unclean 0 "wrote [^\n]*/vram.png"
    COMMAND jbprobe to-png ${_probe_out}/unclean-wait_timeout.jbpr ${_probe_out}/png-unclean)
  set_tests_properties(probe_cli_compare_unclean probe_cli_info_unclean probe_cli_to_png_unclean
    PROPERTIES FIXTURES_REQUIRED probe_unclean)

  # probe_pty_NAME: the probe's code and `jbprobe send --tty` over a
  # pseudo-terminal, in real time, jbprobe on a thread of its own
  # (sim.c built with JBP_SIM_PTY). mask-bit and uv-interpolation always,
  # texture-overflow (DMA in blocks) when it is fetched.
  if(JB_THREADS)
    add_library(jb_probe_jbprobe OBJECT ${PROJECT_SOURCE_DIR}/tools/probe/host/jbprobe.c)
    set_target_properties(jb_probe_jbprobe PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_definitions(jb_probe_jbprobe PRIVATE main=jbprobe_main)
    target_compile_options(jb_probe_jbprobe PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/probe/jbprobe_entry.h)
    target_link_libraries(jb_probe_jbprobe PRIVATE jb_probe_host jb_jbc jb_stb jb_build_flags)
    add_executable(test_probe_pty probe/sim.c ${PROJECT_SOURCE_DIR}/tools/probe/probe/probe.c
      $<TARGET_OBJECTS:jb_probe_jbprobe> $<TARGET_OBJECTS:jb_probe_runner>)
    set_target_properties(test_probe_pty PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_definitions(test_probe_pty PRIVATE JBP_SIM_PTY)
    target_include_directories(test_probe_pty PRIVATE ${PROJECT_SOURCE_DIR}/tools/probe/probe)
    target_link_libraries(test_probe_pty PRIVATE jb_probe_host jb_jbc jb_stb jitterbug Threads::Threads
      jb_build_flags)
    set(_probe_pty mask-bit uv-interpolation)
    if(NOT texture-overflow IN_LIST JBC_MISSING)
      list(APPEND _probe_pty texture-overflow)
    endif()
    option(JB_REQUIRE_PTY "probe_pty_* fail, rather than skip, without a pseudo-terminal" OFF)
    foreach(_p ${_probe_pty})
      add_test(NAME probe_pty_${_p}
        COMMAND test_probe_pty --passes ${JBC_${_p}_PASSES} --save ${_probe_out}/${_p}.pty.jbpr
          ${JBC_${_p}_LIST})
      # 77: no pseudo-terminal (a sandbox without /dev/ptmx): skipped,
      # unless JB_REQUIRE_PTY (set by the flake's checks) makes it a failure.
      if(NOT JB_REQUIRE_PTY)
        set_tests_properties(probe_pty_${_p} PROPERTIES SKIP_RETURN_CODE 77)
      endif()
    endforeach()

    # probe_line_*: jbprobe send against a line that never takes its output
    # (tests/probe/line.c): a pseudo-terminal and a TCP connection
    # whose peer floods valid NAKs and never reads, and a TCP listener that
    # never completes the connection. jbprobe must give up at its START
    # limit, shortened here to 3 s with --start-ms (the default stays the
    # protocol's minute), within 2 s more, with the reason.
    add_executable(test_probe_line probe/line.c $<TARGET_OBJECTS:jb_probe_jbprobe>
      $<TARGET_OBJECTS:jb_probe_runner>)
    set_target_properties(test_probe_line PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_link_libraries(test_probe_line PRIVATE jb_probe_host jb_jbc jb_stb jitterbug Threads::Threads
      jb_build_flags)
    # Each requires exit status 0 (77: no pseudo-terminal or loopback,
    # skipped unless JB_REQUIRE_PTY); test_probe_line itself requires
    # jbprobe's exit status, its time and its reason.
    # probe_line_pty_late_ack: a START limit of 1 ms, and the START
    # acknowledged 5 ms after the peer read it: jbprobe must fail the
    # session with that limit's reason and never send the list.
    foreach(_m pty-naks tcp-naks tcp-connect pty-late-ack)
      string(REPLACE "-" "_" _what ${_m})
      set(_limit 3000)
      if(_m STREQUAL "pty-late-ack")
        set(_limit 1)
      endif()
      add_test(NAME probe_line_${_what} COMMAND test_probe_line ${_m} ${_limit} ${JBC_triangle_LIST})
      set_tests_properties(probe_line_${_what} PROPERTIES TIMEOUT 30)
      if(NOT JB_REQUIRE_PTY)
        set_tests_properties(probe_line_${_what} PROPERTIES SKIP_RETURN_CODE 77)
      endif()
    endforeach()
  endif()
endif()
# ---- end of Hardware probe ---------------------------------------------------

# ---- Draw-mode latch, scaled slivers and lines ----------------------------
# api_draw_mode: jb_set_dither changes only GP0(E1h)'s bit 9 (not the
#   GPUSTAT bit 15 latch, the page, bit 10 or the flips), and the helpers
#   that send GP0(E1h) keep the latch.
# hires_slivers: thin triangles in every direction, precise vertices off
#   and on, at 1x, 2x, 3x, 4x and 8x, every pixel's coverage, colour and
#   texture coordinate against an exact (128-bit) evaluation of the rule.
# hires_lines: lines in all eight octants, many slopes and near-ties, at
#   1x, 2x, 3x, 4x and 8x, drawn from either end, every pixel against an
#   exact rational evaluation of the rule.
# Both use __int128 (GCC and Clang), so not with MSVC.
jb_api_test(api_draw_mode)
add_test(NAME api_draw_mode COMMAND test_api_draw_mode)
if(NOT MSVC)
  jb_api_test(hires_slivers)
  add_test(NAME hires_slivers COMMAND test_hires_slivers)
  jb_api_test(hires_lines)
  add_test(NAME hires_lines COMMAND test_hires_lines)
endif()
# ---- end of draw-mode latch, scaled slivers and lines --------------------

# ---- Scaled triangle set-ups: narrow (64-bit) against wide (128-bit) -------
# hires_wide: compiles src/soft/raster_hires.c again with every triangle
#   set up wide (its test hooks) and compares every pixel of random
#   triangles, Gouraud and textured, at 2x, 3x, 4x and 8x, precise vertices
#   off and on, with the library's narrow set-up; every integer-vertex
#   triangle must be set up narrow. __int128, so not with MSVC.
if(NOT MSVC)
  jb_api_test(hires_wide INTERNAL)
  add_test(NAME hires_wide COMMAND test_hires_wide)
endif()
# ---- end of Scaled triangle set-ups ------------------------------------------

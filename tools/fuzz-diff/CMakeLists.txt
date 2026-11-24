# tools/fuzz-diff: differential fuzzing against psxstack's GPU
# (docs/testing.md, "Differential fuzzing").
# A project of its own, built apart from the library's build:
#   cmake -S tools/fuzz-diff -B build-fuzz -DPSXSTACK_DIR=/path/to/psxstack
# psxstack is fetched for this and never vendored: its psyq/gpu.c is copied
# into the build tree and compiled there with shim/psyq_internal.h.
cmake_minimum_required(VERSION 3.20)
project(fuzz_diff C)

set(PSXSTACK_DIR "" CACHE PATH "A checkout of psxstack at the pinned commit")
if(NOT EXISTS "${PSXSTACK_DIR}/psyq/gpu.c")
  message(FATAL_ERROR "PSXSTACK_DIR must name a psxstack checkout (psyq/gpu.c not found)")
endif()

# The pinned commit (pin.sh), named in every reproducer saved.
file(STRINGS pin.sh _pin REGEX "^PSXSTACK_REV=[0-9a-f]+$")
string(REGEX REPLACE "^PSXSTACK_REV=" "" PSXSTACK_REV "${_pin}")

get_filename_component(_jb_root ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
set(JB_TESTS OFF CACHE BOOL "" FORCE)
set(JB_TOOLS OFF CACHE BOOL "" FORCE)
set(JB_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JB_VULKAN OFF CACHE STRING "" FORCE)
add_subdirectory(${_jb_root} jitterbug)

# The reference: gpu.c beside nothing of psxstack's, so that its
# #include "psyq_internal.h" finds the shim. Built with its own flags (it
# is not ours to keep warning-free), optimised as psxstack builds it.
set(_ref_dir ${CMAKE_CURRENT_BINARY_DIR}/psxstack-gpu)
configure_file(${PSXSTACK_DIR}/psyq/gpu.c ${_ref_dir}/gpu.c COPYONLY)
add_library(fuzz_ref STATIC ${_ref_dir}/gpu.c ref.c)
target_include_directories(fuzz_ref PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shim
  ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(fuzz_ref PRIVATE PC_PORT)
set_source_files_properties(${_ref_dir}/gpu.c PROPERTIES COMPILE_OPTIONS "-O2;-w")
set_target_properties(fuzz_ref PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

# stb for the dumps (conformance mode), and the list reader.
add_library(fuzz_stb STATIC ${_jb_root}/tests/stb.c)
target_include_directories(fuzz_stb SYSTEM PUBLIC ${_jb_root}/third_party/stb)
target_link_libraries(fuzz_stb PRIVATE m)
add_library(fuzz_jbc STATIC ${_jb_root}/tests/conformance/jbc.c)
target_include_directories(fuzz_jbc PUBLIC ${_jb_root}/tests/conformance)
target_link_libraries(fuzz_jbc PUBLIC jitterbug fuzz_stb)
set_target_properties(fuzz_jbc PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

add_executable(fuzz-diff main.c gen.c gen_check.c open_lists.c classify.c divergences.c)
target_link_libraries(fuzz-diff PRIVATE fuzz_ref fuzz_jbc jitterbug jb_build_flags)
target_compile_definitions(fuzz-diff PRIVATE "PSXSTACK_REV=\"${PSXSTACK_REV}\"")
# main.c reads jitterbug's 1x VRAM in place for the comparison after every
# command (src/soft/soft.h, "Tests and tools only"), as tests/seed_multi.c does.
target_include_directories(fuzz-diff PRIVATE ${_jb_root}/src)
set_target_properties(fuzz-diff PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

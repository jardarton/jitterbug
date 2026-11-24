# The conformance recorder (docs/testing.md, "Conformance tests and the recorder"):
# one host program per ps1-tests GPU test, its main compiled unchanged against
# the recorder's
# headers and shim. A project of its own, not part of the library's build;
# regen.sh and the flake's conformance-lists build it.
#
#   cmake -S tools/conformance-record -B build-record -G Ninja \
#     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
#     -DPS1_TESTS_DIR=/path/to/ps1-tests
cmake_minimum_required(VERSION 3.24)
project(jb_conformance_record LANGUAGES C CXX)

set(PS1_TESTS_DIR "" CACHE PATH "A checkout of ps1-tests at the pinned commit")
if(NOT IS_DIRECTORY "${PS1_TESTS_DIR}/gpu")
  message(FATAL_ERROR "PS1_TESTS_DIR (${PS1_TESTS_DIR}) is not a ps1-tests checkout")
endif()
# The port hooks (src/mmio.c) need clang's SanitizerCoverage, and clang
# evaluates call arguments left to right, as the console's GCC did
# (docs/testing.md, "How `tools/conformance-record` works").
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "the recorder must be built with clang")
endif()

set(_jbrec_root ${CMAKE_CURRENT_SOURCE_DIR})

# The shim: the SDK's calls, the hooks under ps1-tests' common library,
# the list.
add_library(jbrec STATIC src/record.c src/mmio.c src/sdk.c src/check.c src/gpustat.c
  src/calls.c)
target_include_directories(jbrec PUBLIC ${_jbrec_root}/include
  PRIVATE ${_jbrec_root}/../../tests/conformance)
set_target_properties(jbrec PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
target_compile_options(jbrec PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
  -Wmissing-prototypes -Werror -fsigned-char)
target_compile_definitions(jbrec PRIVATE _GNU_SOURCE)

# jbc-words: a list's GPU words alone, to compare two recordings (record.sh).
add_executable(jbc-words src/words.c)
target_include_directories(jbc-words PRIVATE ${_jbrec_root}/../../tests/conformance)
set_target_properties(jbc-words PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(jbc-words PRIVATE -Wall -Wextra -Wpedantic -Werror)

# jbc-fnv: a file's size and FNV-1a, for OUT/fixtures-identity.txt (record.sh).
add_executable(jbc-fnv src/fnv.c)
set_target_properties(jbc-fnv PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(jbc-fnv PRIVATE -Wall -Wextra -Wpedantic -Werror)

# The programs, as ps1-tests builds them: every .c and .cpp in the test's
# directory. Their sources are third-party code, compiled without
# warnings, with the console's char signedness, zeroed locals, wrapping
# signed arithmetic, no float contraction, the port hooks, and the SDK's
# C library differences (include/jbrec_prelude.h).
function(jbrec_upstream_flags _target)
  set_target_properties(${_target} PROPERTIES
    C_STANDARD 11 C_EXTENSIONS ON CXX_STANDARD 17 CXX_EXTENSIONS ON)
  target_compile_options(${_target} PRIVATE -w -O1 -fsigned-char -ftrivial-auto-var-init=zero
    -fwrapv -fno-strict-aliasing -ffp-contract=off
    -include ${_jbrec_root}/include/jbrec_prelude.h
    -fsanitize-coverage=inline-8bit-counters,trace-loads,trace-stores)
endfunction()

# record.sh puts each program's sources, from the commit programs.txt
# names, in gpu/NAME, and those of the commit in its third field, if any,
# in gpu/NAME.words (recorded only to compare words). Beside each it
# writes DIR.common: which version of ps1-tests' common library that
# commit has ("none", or the commit that made it), and puts that
# version's gpu.h and gpu.c or gpu.cpp, unmodified, in common/VERSION
# (docs/testing.md, "How `tools/conformance-record` works"). The library is compiled
# as the programs are, and also with -finstrument-functions, whose hooks
# (src/calls.c) name its helpers' calls; its includes of ps1-tests' other
# headers ("stdint.h", "io.h", "dma.hpp") find the recorder's stand-ins
# (include/ps1-tests-common, a quote-only include directory). Its later,
# C++ version casts a pointer to a 32-bit integer, as the console's
# compiler allows; clang allows it on a 64-bit host with -fms-extensions.
function(jbrec_program _target _dir)
  file(GLOB _srcs ${_dir}/*.c ${_dir}/*.cpp)
  list(SORT _srcs)
  file(STRINGS ${_dir}.common _common)
  add_executable(${_target} ${_srcs})
  jbrec_upstream_flags(${_target})
  if(_common STREQUAL "none")
    target_link_libraries(${_target} PRIVATE jbrec)
  elseif(_common MATCHES "^[0-9a-f]+$")
    set(_lib jbrec-common-${_common})
    if(NOT TARGET ${_lib})
      set(_libdir ${PS1_TESTS_DIR}/common/${_common})
      file(GLOB _libsrc ${_libdir}/gpu.c ${_libdir}/gpu.cpp)
      list(LENGTH _libsrc _n)
      if(NOT _n EQUAL 1 OR NOT EXISTS ${_libdir}/gpu.h)
        message(FATAL_ERROR "${_libdir}: not one of ps1-tests' common libraries")
      endif()
      add_library(${_lib} STATIC ${_libsrc})
      jbrec_upstream_flags(${_lib})
      target_compile_options(${_lib} PRIVATE -finstrument-functions
        $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions>)
      target_include_directories(${_lib} PUBLIC ${_libdir} ${_jbrec_root}/include)
      target_compile_options(${_lib} PUBLIC -iquote ${_jbrec_root}/include/ps1-tests-common)
      target_link_libraries(${_lib} PUBLIC jbrec)
    endif()
    target_link_libraries(${_target} PRIVATE ${_lib})
  else()
    message(FATAL_ERROR "${_dir}.common: unknown common library version '${_common}'")
  endif()
endfunction()

file(STRINGS programs.txt _jbrec_lines REGEX "^[a-z0-9-]+ [0-9a-f]+( [0-9a-f]+)?$")
foreach(_line ${_jbrec_lines})
  string(REGEX MATCH "^[a-z0-9-]+" _p "${_line}")
  jbrec_program(record-${_p} ${PS1_TESTS_DIR}/gpu/${_p})
  if(_line MATCHES " [0-9a-f]+ [0-9a-f]+$")
    jbrec_program(record-${_p}-words ${PS1_TESTS_DIR}/gpu/${_p}.words)
  endif()
endforeach()

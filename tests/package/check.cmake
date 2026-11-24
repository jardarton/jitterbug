# What a user of an installed jitterbug gets (cmake -P), checked by
# building and running consumer.c against it:
#
#   cmake -DSOURCE=<jitterbug source> -DWORK=<empty dir> [-DSHARED=ON]
#         [-DTOOLCHAIN=<toolchain file> -DEMULATOR=<runner>] [-DCC=<compiler>]
#         [-DGENERATOR=Ninja] -P tests/package/check.cmake
#     builds jitterbug (tests off, tools on), installs it into WORK/prefix,
#     moves that to WORK/moved (the install must be relocatable), and
#     checks it: what was installed, and that nothing in the CMake package
#     or jitterbug.pc names the build, the source or the first prefix; then
#     consumer.c through find_package, through pkg-config (--static for a
#     static library) and through add_subdirectory of SOURCE.
#     TOOLCHAIN (emscripten's) cross-builds, EMULATOR runs the programs,
#     and pkg-config is left out.
#
#   cmake -DPREFIX=<installed prefix> -DWORK=<empty dir> [-DSHARED=ON]
#         [-DTOOLS=ON] ... -P tests/package/check.cmake
#     the same for an install made elsewhere (the flake's packages.default),
#     through find_package and pkg-config; TOOLS: it has the tools too.
cmake_minimum_required(VERSION 3.24)

if(NOT WORK)
  message(FATAL_ERROR "WORK is not set")
endif()
if(NOT SOURCE AND NOT PREFIX)
  message(FATAL_ERROR "set SOURCE (build and install) or PREFIX (an install)")
endif()
if(NOT DEFINED SHARED)
  set(SHARED OFF)
endif()
if(NOT CC)
  set(CC cc)
endif()
set(_here ${CMAKE_CURRENT_LIST_DIR})
file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK})

set(_gen)
if(GENERATOR)
  set(_gen -G ${GENERATOR})
endif()
set(_cross)
if(TOOLCHAIN)
  set(_cross -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN})
endif()

function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    list(JOIN ARGN " " _cmd)
    message(FATAL_ERROR "failed (${_rc}): ${_cmd}")
  endif()
endfunction()

# Runs a consumer: it must print "jitterbug <version>: ok".
function(run_consumer what program)
  execute_process(COMMAND ${EMULATOR} ${program} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out
    ERROR_VARIABLE _out)
  if(NOT _rc EQUAL 0 OR NOT _out MATCHES "^jitterbug [0-9]+\\.[0-9]+\\.[0-9]+: ok")
    message(FATAL_ERROR "${what}: ${program} failed (${_rc}):\n${_out}")
  endif()
  string(STRIP "${_out}" _out)
  message(STATUS "${what}: ${_out}")
endfunction()

# The consumer project (CMakeLists.txt here), configured with ARGN,
# built, and both its programs run.
function(consume what dir)
  run(${CMAKE_COMMAND} -S ${_here} -B ${dir} ${_gen} ${_cross} -DCMAKE_BUILD_TYPE=Release ${ARGN})
  run(${CMAKE_COMMAND} --build ${dir})
  foreach(_p consumer consumer_cxx)
    file(GLOB _exe ${dir}/${_p} ${dir}/${_p}.js ${dir}/${_p}.exe)
    if(_exe)
      run_consumer("${what} (${_p})" ${_exe})
    elseif(_p STREQUAL "consumer")
      message(FATAL_ERROR "${what}: no ${_p} in ${dir}")
    endif()
  endforeach()
endfunction()

if(SOURCE)
  set(_build ${WORK}/build)
  run(${CMAKE_COMMAND} -S ${SOURCE} -B ${_build} ${_gen} ${_cross} -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=${WORK}/prefix -DBUILD_SHARED_LIBS=${SHARED}
    -DJB_TESTS=OFF -DJB_TOOLS=ON -DJB_EXAMPLES=OFF -DJB_VULKAN=OFF -DJB_WERROR=ON)
  run(${CMAKE_COMMAND} --build ${_build})
  run(${CMAKE_COMMAND} --install ${_build})
  file(RENAME ${WORK}/prefix ${WORK}/moved)
  set(PREFIX ${WORK}/moved)
endif()

# What was installed. The library's directory is where jitterbug.pc is
# (GNUInstallDirs may call it lib or lib64; Nix gives it absolute, so for
# an install made elsewhere the package may be in another output).
file(GLOB_RECURSE _pc LIST_DIRECTORIES false ${PREFIX}/*/jitterbug.pc)
file(GLOB_RECURSE _config LIST_DIRECTORIES false ${PREFIX}/*/jitterbugConfig.cmake)
list(LENGTH _pc _npc)
list(LENGTH _config _nconfig)
if(NOT _npc EQUAL 1 OR NOT _nconfig EQUAL 1)
  message(FATAL_ERROR "${PREFIX}: expected one jitterbug.pc and one jitterbugConfig.cmake, "
    "found ${_pc} ${_config}")
endif()
get_filename_component(_pcdir ${_pc} DIRECTORY)
get_filename_component(_cmakedir ${_config} DIRECTORY)
foreach(_f jitterbugConfigVersion.cmake jitterbugTargets.cmake)
  if(NOT EXISTS ${_cmakedir}/${_f})
    message(FATAL_ERROR "${_cmakedir}: no ${_f}")
  endif()
endforeach()
foreach(_f include/jitterbug.h share/licenses/jitterbug/LICENSE)
  if(NOT EXISTS ${PREFIX}/${_f})
    message(FATAL_ERROR "${PREFIX}: no ${_f}")
  endif()
endforeach()
if(SOURCE OR TOOLS)
  # The tools (JB_TOOLS), and the licences of what they have compiled in.
  set(_ext "")
  if(TOOLCHAIN)
    set(_ext .js)
  endif()
  foreach(_f bin/jb-tex${_ext} bin/jb-mesh${_ext} bin/jb-bench${_ext}
      share/licenses/jitterbug/cgltf/LICENSE share/licenses/jitterbug/stb/LICENSE)
    if(NOT EXISTS ${PREFIX}/${_f})
      message(FATAL_ERROR "${PREFIX}: no ${_f}")
    endif()
  endforeach()
  if(TOOLCHAIN AND NOT EXISTS ${PREFIX}/bin/jb-tex.wasm)
    message(FATAL_ERROR "${PREFIX}: no bin/jb-tex.wasm beside jb-tex.js")
  endif()
  # They start from the moved prefix with no library path set: a shared
  # build finds the library through the tools' relative RPATH.
  if(NOT TOOLCHAIN)
    unset(ENV{LD_LIBRARY_PATH})
    foreach(_t jb-tex jb-mesh)
      run(${PREFIX}/bin/${_t} --help)
    endforeach()
  endif()
endif()

# The package names nothing of the build: no internal target, no build or
# source directory, and (moved) not the prefix it was installed into.
file(GLOB _package ${_cmakedir}/*.cmake)
set(_forbidden jb_build_flags jb_internal "LINK_ONLY:>")
if(SOURCE)
  list(APPEND _forbidden ${SOURCE} ${WORK}/build ${WORK}/prefix)
endif()
foreach(_f ${_package} ${_pc})
  file(READ ${_f} _text)
  foreach(_word ${_forbidden})
    string(FIND "${_text}" "${_word}" _at)
    if(NOT _at EQUAL -1)
      message(FATAL_ERROR "${_f} names ${_word}")
    endif()
  endforeach()
endforeach()
# A static library brings its private dependencies as link-only; a
# shared one none at all.
file(GLOB _targets ${_cmakedir}/jitterbugTargets.cmake)
file(READ ${_targets} _text)
if(SHARED)
  if(_text MATCHES "INTERFACE_LINK_LIBRARIES")
    message(FATAL_ERROR "${_targets}: the shared library passes on link libraries")
  endif()
elseif(NOT TOOLCHAIN AND NOT _text MATCHES "\\$<LINK_ONLY:m>")
  message(FATAL_ERROR "${_targets}: the static library does not bring libm")
endif()

# find_package, from CMAKE_PREFIX_PATH (and the root path a cross
# toolchain searches only under).
set(_find -DCMAKE_PREFIX_PATH=${PREFIX})
if(TOOLCHAIN)
  list(APPEND _find -DCMAKE_FIND_ROOT_PATH=${PREFIX})
endif()
consume(find_package ${WORK}/find-package ${_find})

# pkg-config, as a Makefile would use it (not for a cross build).
if(NOT TOOLCHAIN)
  find_program(_pkgconfig NAMES pkg-config pkgconf REQUIRED)
  set(ENV{PKG_CONFIG_PATH} ${_pcdir})
  function(pkgconfig out)
    execute_process(COMMAND ${_pkgconfig} ${ARGN} jitterbug RESULT_VARIABLE _rc
      OUTPUT_VARIABLE _o OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "pkg-config ${ARGN} jitterbug failed")
    endif()
    set(${out} "${_o}" PARENT_SCOPE)
  endfunction()
  pkgconfig(_version --modversion)
  file(STRINGS ${PREFIX}/include/jitterbug.h _v REGEX "^#define JB_VERSION_(MAJOR|MINOR|PATCH) ")
  string(REGEX REPLACE "[^;]*JB_VERSION_[A-Z]+ ([0-9]+)" "\\1" _v "${_v}")
  list(JOIN _v "." _v)
  if(NOT _version STREQUAL _v)
    message(FATAL_ERROR "pkg-config says ${_version}, the header ${_v}")
  endif()
  pkgconfig(_libdir --variable=libdir)
  # A static library is linked with --static, which adds Libs.private.
  if(SHARED)
    set(_ways --libs)
  else()
    set(_ways --static)
  endif()
  foreach(_way ${_ways})
    if(_way STREQUAL "--static")
      pkgconfig(_flags --cflags --libs --static)
      if(NOT _flags MATCHES "-lm")
        message(FATAL_ERROR "pkg-config --static gives no -lm: ${_flags}")
      endif()
    else()
      pkgconfig(_flags --cflags --libs)
    endif()
    separate_arguments(_flags UNIX_COMMAND "${_flags}")
    set(_exe ${WORK}/pkg-config${_way}/consumer)
    file(MAKE_DIRECTORY ${WORK}/pkg-config${_way})
    set(_rpath)
    if(SHARED)
      set(_rpath -Wl,-rpath,${_libdir})
    endif()
    run(${CC} -std=c99 -pedantic-errors -o ${_exe} ${_here}/consumer.c ${_flags} ${_rpath})
    run_consumer("pkg-config ${_way}" ${_exe})
  endforeach()
endif()

# add_subdirectory of the source tree, as FetchContent does.
if(SOURCE)
  consume(add_subdirectory ${WORK}/add-subdirectory -DJB_SOURCE_DIR=${SOURCE}
    -DBUILD_SHARED_LIBS=${SHARED})
endif()

message(STATUS "package: all consumers ran")

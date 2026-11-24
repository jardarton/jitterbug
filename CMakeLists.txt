cmake_minimum_required(VERSION 3.24)

# The version lives in the public header.
file(STRINGS include/jitterbug.h _jb_version_lines REGEX "^#define JB_VERSION_(MAJOR|MINOR|PATCH) ")
foreach(_part MAJOR MINOR PATCH)
  if(NOT _jb_version_lines MATCHES "JB_VERSION_${_part} ([0-9]+)")
    message(FATAL_ERROR "JB_VERSION_${_part} not found in include/jitterbug.h")
  endif()
  set(_jb_${_part} "${CMAKE_MATCH_1}")
endforeach()

project(jitterbug VERSION "${_jb_MAJOR}.${_jb_MINOR}.${_jb_PATCH}" LANGUAGES C)

set(JB_VULKAN "AUTO" CACHE STRING "Build the Vulkan backend: AUTO, ON or OFF")
set_property(CACHE JB_VULKAN PROPERTY STRINGS AUTO ON OFF)
option(JB_TOOLS "Build the tools (jb-bench, jb-tex, jb-mesh; the last two also with JB_TESTS)" OFF)
option(JB_EXAMPLES "Build the examples" OFF)
option(JB_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
option(JB_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(JB_WERROR "Treat warnings as errors" OFF)
option(JB_THREADS "Rasterise on worker threads; OFF builds without a thread library" ON)
# Off when jitterbug is a subproject (add_subdirectory, FetchContent): the
# parent decides what it installs.
option(JB_INSTALL "Install the library, its header, CMake package and pkg-config file (and the tools with JB_TOOLS)"
  ${PROJECT_IS_TOP_LEVEL})
set(JB_LAVAPIPE_ICD "$ENV{JB_LAVAPIPE_ICD}" CACHE FILEPATH
  "Mesa's software Vulkan driver (lvp_icd.*.json), for Vulkan tests without a GPU")

# The web build is single-threaded: threads in a browser need cross-origin
# isolation headers (docs/design.md, "Design choices for 0.1").
if(EMSCRIPTEN AND JB_THREADS)
  message(STATUS "JB_THREADS forced OFF: the emscripten build is single-threaded")
  set(JB_THREADS OFF CACHE BOOL "Rasterise on worker threads; OFF builds without a thread library" FORCE)
endif()

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(GNUInstallDirs)

# Flags for everything we build ourselves (not for third_party/).
add_library(jb_build_flags INTERFACE)
if(MSVC)
  target_compile_options(jb_build_flags INTERFACE /W4 $<$<BOOL:${JB_WERROR}>:/WX>)
else()
  target_compile_options(jb_build_flags INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes
    $<$<BOOL:${JB_WERROR}>:-Werror>
    # Determinism (docs/determinism.md, "How the code keeps it"): the float
    # options must give the same result on every compiler, so no
    # contraction into fused multiply-adds.
    # -ffast-math is refused in src/core/assumptions.c.
    -ffp-contract=off)
  if(JB_SANITIZE)
    # float-cast-overflow is not part of "undefined": a float or double
    # converted to an integer it does not fit is undefined behaviour too,
    # and the float geometry must never do it (src/geom/pipeline.c).
    target_compile_options(jb_build_flags INTERFACE
      -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all
      -fno-omit-frame-pointer)
    target_link_options(jb_build_flags INTERFACE -fsanitize=address,undefined,float-cast-overflow)
  endif()
endif()

# The internals (docs/design.md, "Architecture"), one object library that the
# library and the tests that need internals link:
#   src/core   the context: GP0 drawing state, command decode, the backend
#              interface (backend.h), allocation (alloc.h)
#   src/soft   the software backend: both VRAMs, the primitive queue and
#              workers, the rasterisers, scanout
#   src/geom   the GTE and the precise-vertex table; screen-space clipping
#              (split.c); the GTE helpers' fixed-point maths (fixed.c), the
#              float maths (fmath.c), the friendly scene state (scene.c) and
#              3D pipeline (pipeline.c)
# Their symbols are hidden: a shared library exports only JB_API.
# JB_THREADS=OFF builds the worker pool (src/soft/workers.c) as the calling
# thread alone, with no thread library: a request for more threads is
# clamped to one and logged (include/jitterbug.h, jb_config.threads).
if(JB_THREADS)
  find_package(Threads REQUIRED)
endif()
add_library(jb_internal OBJECT
  src/core/alloc.c
  src/core/context.c
  src/core/gp0.c
  src/core/layout.c
  src/core/placer.c
  src/soft/raster.c
  src/soft/raster_hires.c
  src/soft/scanout.c
  src/soft/soft.c
  src/soft/transfer.c
  src/soft/workers.c
  src/geom/fixed.c
  src/geom/fmath.c
  src/geom/gte.c
  src/geom/pipeline.c
  src/geom/precise.c
  src/geom/scene.c
  src/geom/split.c)
set_target_properties(jb_internal PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(jb_internal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(jb_internal PRIVATE jb_build_flags)
# sqrtf (src/geom/fmath.h), the one C library maths function used.
if(UNIX AND NOT APPLE)
  target_link_libraries(jb_internal PUBLIC m)
endif()
if(JB_THREADS)
  target_link_libraries(jb_internal PUBLIC Threads::Threads)
else()
  target_compile_definitions(jb_internal PRIVATE JBI_NO_THREADS)
endif()

# The library: the public API (src/api: jb_* in include/jitterbug.h) over
# the internals. BUILD_SHARED_LIBS=ON makes it a shared library that
# exports the JB_API functions and nothing else (tests/exports.cmake).
# JB_API_SOURCES are also built into the tests that need the public API's
# own internals (src/api), which a shared library hides.
set(JB_API_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/create.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/display.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/draw2d.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/draw3d.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/files.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/font.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/frame.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/gte.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/gte_helpers.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/jbm.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/jbt.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/prim.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/record.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/scene.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/stream.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/texture.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/version.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api/vram.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/assumptions.c)
add_library(jitterbug ${JB_API_SOURCES} $<TARGET_OBJECTS:jb_internal>)
add_library(jitterbug::jitterbug ALIAS jitterbug)
if(JB_THREADS)
  target_link_libraries(jitterbug PRIVATE Threads::Threads)
endif()
# libm is the implementation's, not the header's: private, so a shared
# library does not pass it on (a static one does, as link-only).
if(UNIX AND NOT APPLE)
  target_link_libraries(jitterbug PRIVATE m)
endif()
# A Windows DLL's users declare its functions dllimport (JB_API).
if(WIN32 AND BUILD_SHARED_LIBS)
  target_compile_definitions(jitterbug INTERFACE JB_SHARED)
endif()
target_include_directories(jitterbug PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(jitterbug PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Before 1.0 every minor version may break the ABI, so the soname has
# both (libjitterbug.so.0.1).
if(PROJECT_VERSION_MAJOR EQUAL 0)
  set(_jb_soversion ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
else()
  set(_jb_soversion ${PROJECT_VERSION_MAJOR})
endif()
# The header is C99. The implementation is C11, for <stdatomic.h>.
set_target_properties(jitterbug PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  C_VISIBILITY_PRESET hidden
  DEFINE_SYMBOL JB_BUILD_SHARED
  VERSION ${PROJECT_VERSION}
  SOVERSION ${_jb_soversion})
# jb_build_flags' options, as the library's own: linking the target, even
# as $<BUILD_INTERFACE:...>, would leave an empty $<LINK_ONLY:> in the
# installed static library's link interface.
target_compile_options(jitterbug PRIVATE
  $<TARGET_PROPERTY:jb_build_flags,INTERFACE_COMPILE_OPTIONS>)
target_link_options(jitterbug PRIVATE $<TARGET_PROPERTY:jb_build_flags,INTERFACE_LINK_OPTIONS>)

# The Vulkan backend comes after 0.1 (todo.md, "Planned after 0.1"). Until
# then this only decides whether the lavapipe smoke test can be built.
set(JB_HAVE_VULKAN OFF)
if(NOT JB_VULKAN STREQUAL "OFF")
  find_package(Vulkan)
  if(Vulkan_FOUND)
    set(JB_HAVE_VULKAN ON)
  elseif(JB_VULKAN STREQUAL "ON")
    message(FATAL_ERROR "JB_VULKAN=ON, but Vulkan was not found")
  else()
    message(STATUS "Vulkan disabled: not found")
  endif()
endif()

# The install (JB_INSTALL): the library, its header, the CMake package
# (find_package(jitterbug), the target jitterbug::jitterbug), the
# pkg-config file and the licence. tests/package checks what a user gets.
# The tools install their own (jb_install_tool, below).
set(JB_LICENSE_DIR ${CMAKE_INSTALL_DATADIR}/licenses/jitterbug)
if(JB_INSTALL)
  include(CMakePackageConfigHelpers)
  set(_jb_cmake_dir ${CMAKE_INSTALL_LIBDIR}/cmake/jitterbug)
  install(TARGETS jitterbug EXPORT jitterbugTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES include/jitterbug.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT jitterbugTargets NAMESPACE jitterbug:: DESTINATION ${_jb_cmake_dir})
  get_target_property(_jb_type jitterbug TYPE)
  if(_jb_type STREQUAL "SHARED_LIBRARY")
    set(JB_PKG_SHARED ON)
  else()
    set(JB_PKG_SHARED OFF)
  endif()
  configure_package_config_file(cmake/jitterbugConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/jitterbugConfig.cmake
    INSTALL_DESTINATION ${_jb_cmake_dir}
    NO_SET_AND_CHECK_MACRO)
  # No API or ABI promise before 1.0 (include/jitterbug.h): 0.1.x is
  # compatible with a request for 0.1.y, y <= x, and 0.2 is not. From 1.0
  # this becomes SameMajorVersion.
  if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(_jb_compat SameMinorVersion)
  else()
    set(_jb_compat SameMajorVersion)
  endif()
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/jitterbugConfigVersion.cmake
    COMPATIBILITY ${_jb_compat})
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/jitterbugConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/jitterbugConfigVersion.cmake
    DESTINATION ${_jb_cmake_dir})

  # jitterbug.pc, relocatable: prefix is found from the file's own
  # directory (${pcfiledir}), and libdir and includedir from prefix. A
  # directory given absolute (as Nix gives them) stays absolute unless it
  # is under the prefix.
  set(_jb_prefix "${CMAKE_INSTALL_PREFIX}")
  set(_jb_pc_full "${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig")
  cmake_path(IS_PREFIX _jb_prefix "${_jb_pc_full}" NORMALIZE _jb_pc_in_prefix)
  if(_jb_pc_in_prefix)
    file(RELATIVE_PATH _jb_up "${_jb_pc_full}" "${_jb_prefix}")
    string(REGEX REPLACE "/$" "" _jb_up "${_jb_up}")
    if(_jb_up STREQUAL "")
      set(JB_PC_PREFIX "\${pcfiledir}")
    else()
      set(JB_PC_PREFIX "\${pcfiledir}/${_jb_up}")
    endif()
  else()
    set(JB_PC_PREFIX "${_jb_prefix}")
  endif()
  foreach(_jb_dir LIBDIR INCLUDEDIR)
    set(_jb_full "${CMAKE_INSTALL_FULL_${_jb_dir}}")
    cmake_path(IS_PREFIX _jb_prefix "${_jb_full}" NORMALIZE _jb_in_prefix)
    if(_jb_pc_in_prefix AND _jb_in_prefix)
      file(RELATIVE_PATH _jb_rel "${_jb_prefix}" "${_jb_full}")
      set(JB_PC_${_jb_dir} "\${prefix}/${_jb_rel}")
    else()
      set(JB_PC_${_jb_dir} "${_jb_full}")
    endif()
  endforeach()
  # What linking the static library needs besides it (pkg-config --static).
  set(_jb_private "")
  if(UNIX AND NOT APPLE)
    list(APPEND _jb_private -lm)
  endif()
  if(JB_THREADS AND CMAKE_THREAD_LIBS_INIT)
    list(APPEND _jb_private ${CMAKE_THREAD_LIBS_INIT})
  endif()
  list(JOIN _jb_private " " JB_PC_LIBS_PRIVATE)
  set(JB_PC_CFLAGS "")
  if(WIN32 AND JB_PKG_SHARED)
    set(JB_PC_CFLAGS " -DJB_SHARED")
  endif()
  configure_file(cmake/jitterbug.pc.in ${CMAKE_CURRENT_BINARY_DIR}/jitterbug.pc @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/jitterbug.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

  # Licences under share/licenses (kept where documentation is left out
  # of an install): jitterbug's, and with the tools those of the code
  # they have compiled in (jb_install_tool).
  install(FILES LICENSE DESTINATION ${JB_LICENSE_DIR})
endif()

# A tool's install, with JB_TOOLS (not when it is built only for the
# tests): the program (under emscripten its .js and the .wasm beside it),
# and the licence of each third_party/ directory compiled into it.
function(jb_install_tool target)
  if(NOT (JB_TOOLS AND JB_INSTALL))
    return()
  endif()
  install(TARGETS ${target} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  # Against a shared library, the installed tool finds it relative to
  # itself, so a prefix outside the loader's path needs no LD_LIBRARY_PATH.
  get_target_property(_type jitterbug TYPE)
  if(UNIX AND _type STREQUAL "SHARED_LIBRARY")
    if(APPLE)
      set(_origin @loader_path)
    else()
      set(_origin $ORIGIN)
    endif()
    file(RELATIVE_PATH _rel ${CMAKE_INSTALL_FULL_BINDIR} ${CMAKE_INSTALL_FULL_LIBDIR})
    set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "${_origin}/${_rel}")
  endif()
  if(EMSCRIPTEN)
    install(FILES $<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_BASE_NAME:${target}>.wasm
      DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
  foreach(_dir ${ARGN})
    install(FILES ${PROJECT_SOURCE_DIR}/third_party/${_dir}/LICENSE
      DESTINATION ${JB_LICENSE_DIR}/${_dir})
  endforeach()
endfunction()

# The converters, jb-tex (PNG to .jbt) and jb-mesh (OBJ or glTF to .jbm):
# with JB_TOOLS, and with JB_TESTS for their tests (tests/CMakeLists.txt,
# "Files and converters"), so they are added before the tests.
if(JB_TOOLS OR JB_TESTS)
  add_subdirectory(tools/common)
  add_subdirectory(tools/jb-tex)
  add_subdirectory(tools/jb-mesh)
endif()

if(JB_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(JB_EXAMPLES)
  add_subdirectory(examples/first-light)
  add_subdirectory(examples/demo)
  add_subdirectory(examples/header-images)
endif()

# Tools: jb-bench (tools/jb-bench), and jb-tex and jb-mesh above.
# tools/seed-record is a script, not built; tools/conformance-record is a
# project of its own, built only to regenerate tests/conformance.
if(JB_TOOLS)
  add_subdirectory(tools/jb-bench)
endif()

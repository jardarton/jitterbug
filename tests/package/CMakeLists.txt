# A project that uses jitterbug (check.cmake), as a user's would:
#   find_package(jitterbug 0.1)  by default, from CMAKE_PREFIX_PATH
#   JB_SOURCE_DIR=<source tree>  through add_subdirectory instead
# It builds consumer.c as C99, and as C++ when there is a C++ compiler.
cmake_minimum_required(VERSION 3.24)
project(jb_consumer LANGUAGES C)

set(JB_SOURCE_DIR "" CACHE PATH "jitterbug's source tree, for add_subdirectory")
if(JB_SOURCE_DIR)
  add_subdirectory(${JB_SOURCE_DIR} jitterbug)
  # A subproject builds the library alone, and installs nothing.
  foreach(_t test_version jb-tex jb-mesh jb-bench first-light-headless demo-headless)
    if(TARGET ${_t})
      message(FATAL_ERROR "jitterbug as a subproject builds ${_t}")
    endif()
  endforeach()
  if(JB_INSTALL)
    message(FATAL_ERROR "jitterbug as a subproject has JB_INSTALL on")
  endif()
else()
  # The version rule (SameMinorVersion): 0.2 and 1.0 are not 0.1.
  foreach(_v 0.2 1.0)
    find_package(jitterbug ${_v} QUIET)
    if(jitterbug_FOUND)
      message(FATAL_ERROR "find_package(jitterbug ${_v}) accepted ${jitterbug_VERSION}")
    endif()
  endforeach()
  find_package(jitterbug 0.1 REQUIRED)
  message(STATUS "jitterbug ${jitterbug_VERSION} from ${jitterbug_DIR}")
endif()

add_executable(consumer consumer.c)
target_link_libraries(consumer PRIVATE jitterbug::jitterbug)
set_target_properties(consumer PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

include(CheckLanguage)
check_language(CXX)
if(CMAKE_CXX_COMPILER)
  enable_language(CXX)
  add_executable(consumer_cxx consumer.cpp)
  target_link_libraries(consumer_cxx PRIVATE jitterbug::jitterbug)
  set_target_properties(consumer_cxx PROPERTIES
    CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endif()

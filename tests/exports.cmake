# What the built library shows the world (cmake -P):
#   -DLIB=<library file> -DTYPE=SHARED_LIBRARY|STATIC_LIBRARY -DNM=<nm>
#   -DHEADER=<include/jitterbug.h>
#
# The functions the header declares with JB_API are the API. A shared
# library must export exactly those (its dynamic symbol table defines
# nothing else); a static one must define every one of them and hold no
# writable data (no D, d, B or b symbols: the library keeps no global
# state).

file(READ "${HEADER}" _header)
string(REGEX MATCHALL "JB_API[^;(]*[ *]jb_[a-z0-9_]+\\(" _decls "${_header}")
set(_api "")
foreach(_d ${_decls})
  string(REGEX MATCH "jb_[a-z0-9_]+\\($" _name "${_d}")
  string(REGEX REPLACE "\\($" "" _name "${_name}")
  list(APPEND _api ${_name})
endforeach()
list(SORT _api)
list(LENGTH _api _count)
if(_count LESS 10)
  message(FATAL_ERROR "found only ${_count} JB_API declarations in ${HEADER}: ${_api}")
endif()

if(TYPE STREQUAL "SHARED_LIBRARY")
  execute_process(COMMAND "${NM}" -D --defined-only "${LIB}"
    OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "${NM} -D failed on ${LIB}")
  endif()
  string(REPLACE "\n" ";" _lines "${_out}")
  set(_exported "")
  foreach(_l ${_lines})
    if(_l MATCHES "^[0-9a-fA-F]* *[A-Za-z] ([^ ]+)$")
      list(APPEND _exported ${CMAKE_MATCH_1})
    endif()
  endforeach()
  list(SORT _exported)
  if(NOT _exported STREQUAL _api)
    message(FATAL_ERROR "${LIB} exports\n  ${_exported}\nbut the header declares\n  ${_api}")
  endif()
  message(STATUS "exports: the shared library exports exactly the ${_count} jb_ functions "
                 "the header declares")
else()
  execute_process(COMMAND "${NM}" --defined-only "${LIB}"
    OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "${NM} failed on ${LIB}")
  endif()
  string(REPLACE "\n" ";" _lines "${_out}")
  set(_data "")
  set(_text "")
  foreach(_l ${_lines})
    if(_l MATCHES "^[0-9a-fA-F]+ [BbDd] (.+)$")
      set(_sym "${CMAKE_MATCH_1}")
      # AddressSanitizer's own globals, in the sanitizer build, are not ours.
      if(NOT _sym MATCHES "^_+(odr_)?asan")
        list(APPEND _data "${_sym}")
      endif()
    elseif(_l MATCHES "^[0-9a-fA-F]+ T (jb_.+)$")
      list(APPEND _text ${CMAKE_MATCH_1})
    endif()
  endforeach()
  if(_data)
    message(FATAL_ERROR "${LIB} holds writable data: ${_data}")
  endif()
  foreach(_name ${_api})
    if(NOT _name IN_LIST _text)
      message(FATAL_ERROR "${LIB} does not define ${_name}")
    endif()
  endforeach()
  message(STATUS "exports: the static library defines the ${_count} jb_ functions and holds "
                 "no writable data")
endif()

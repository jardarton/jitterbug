# One pinned conversion (conversions.txt; docs/testing.md, "File, converter
# and loader tests"):
#   cmake -DOUT=<file to write> -DEXPECTED=<expected/OUTPUT> -P convert.cmake
#         -- [EMULATOR...] TOOL ARGS...
# runs TOOL ARGS OUT (in the test's working directory, tests/files), and
# requires it to exit 0 and OUT to be byte for byte EXPECTED. The command
# is everything after "--", with CMAKE_CROSSCOMPILING_EMULATOR first when
# there is one (qemu, Node), as every other test of the build runs.
#
#   cmake -DBUILD=<build directory> -DREGEN=ON -P convert.cmake
# instead runs every line of conversions.txt with the tools of that build
# and writes the results into expected/ (after a deliberate change; the
# commit says why the bytes changed).
cmake_minimum_required(VERSION 3.24)

if(REGEN)
  set(_dir "${CMAKE_CURRENT_LIST_DIR}")
  file(STRINGS "${_dir}/conversions.txt" _lines REGEX "^[^#]")
  foreach(_line IN LISTS _lines)
    separate_arguments(_parts UNIX_COMMAND "${_line}")
    list(POP_FRONT _parts _out _tool)
    execute_process(COMMAND "${BUILD}/tools/${_tool}/${_tool}" ${_parts}
                            "${_dir}/expected/${_out}"
                    WORKING_DIRECTORY "${_dir}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "${_tool} for ${_out} returned ${_rc}")
    endif()
  endforeach()
  return()
endif()

set(_cmd)
set(_on 0)
math(EXPR _last "${CMAKE_ARGC} - 1")
foreach(_i RANGE ${_last})
  if(_on)
    list(APPEND _cmd "${CMAKE_ARGV${_i}}")
  elseif("${CMAKE_ARGV${_i}}" STREQUAL "--")
    set(_on 1)
  endif()
endforeach()
if(NOT _cmd OR NOT DEFINED OUT OR NOT DEFINED EXPECTED)
  message(FATAL_ERROR "convert.cmake: -DOUT, -DEXPECTED and a command after -- are required")
endif()
file(REMOVE "${OUT}")
get_filename_component(_outdir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_outdir}")
execute_process(COMMAND ${_cmd} "${OUT}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _o ERROR_VARIABLE _o)
message("${_o}")
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "convert.cmake: the tool returned ${_rc}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}" "${EXPECTED}"
                RESULT_VARIABLE _same)
if(NOT _same EQUAL 0)
  file(SHA256 "${OUT}" _got)
  file(SHA256 "${EXPECTED}" _want)
  message(FATAL_ERROR "convert.cmake: ${OUT} (SHA-256 ${_got}) is not ${EXPECTED} "
                      "(${_want}); after a deliberate change, docs/testing.md, \"File, converter "
                      "and loader tests\", says how to write them again")
endif()
file(SHA256 "${OUT}" _got)
message("convert.cmake: ${OUT} is ${EXPECTED}, SHA-256 ${_got}")

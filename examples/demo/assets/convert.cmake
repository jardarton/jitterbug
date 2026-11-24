# The demo's files from their sources (examples/demo/README.md, "How the
# assets are made").
#
#   cmake -DJB_TEX=<jb-tex> -DJB_MESH=<jb-mesh> -DWORK=<scratch directory>
#         -P convert.cmake -- [EMULATOR...]
# runs every line of conversions.txt (in this directory, writing into WORK)
# and requires each output to be byte for byte the file of that name
# here: the demo_assets test. The tools run through EMULATOR when there is
# one (CMAKE_CROSSCOMPILING_EMULATOR: qemu, Node), as every test runs.
#
#   cmake -DJB_TEX=<jb-tex> -DJB_MESH=<jb-mesh> -DREGEN=ON -P convert.cmake
# writes them here instead (the demo_assets_regen target, after a
# deliberate change to the sources or the tools).
cmake_minimum_required(VERSION 3.24)

set(_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_emu)
set(_on 0)
math(EXPR _last "${CMAKE_ARGC} - 1")
foreach(_i RANGE ${_last})
  if(_on)
    list(APPEND _emu "${CMAKE_ARGV${_i}}")
  elseif("${CMAKE_ARGV${_i}}" STREQUAL "--")
    set(_on 1)
  endif()
endforeach()
if(NOT JB_TEX OR NOT JB_MESH)
  message(FATAL_ERROR "convert.cmake: -DJB_TEX and -DJB_MESH are required")
endif()
if(NOT REGEN)
  if(NOT WORK)
    message(FATAL_ERROR "convert.cmake: -DWORK is required (or -DREGEN=ON)")
  endif()
  file(REMOVE_RECURSE "${WORK}")
  file(MAKE_DIRECTORY "${WORK}")
endif()

set(failures 0)
file(STRINGS "${_dir}/conversions.txt" _lines REGEX "^[^#]")
foreach(_line IN LISTS _lines)
  separate_arguments(_parts UNIX_COMMAND "${_line}")
  list(POP_FRONT _parts _out _tool)
  if(_tool STREQUAL "jb-tex")
    set(_exe "${JB_TEX}")
  elseif(_tool STREQUAL "jb-mesh")
    set(_exe "${JB_MESH}")
  else()
    message(FATAL_ERROR "convert.cmake: ${_out}: unknown tool ${_tool}")
  endif()
  if(REGEN)
    set(_to "${_dir}/${_out}")
  else()
    set(_to "${WORK}/${_out}")
  endif()
  execute_process(COMMAND ${_emu} "${_exe}" ${_parts} "${_to}" WORKING_DIRECTORY "${_dir}"
                  RESULT_VARIABLE _rc OUTPUT_VARIABLE _o ERROR_VARIABLE _o)
  if(NOT _rc EQUAL 0)
    message("FAIL ${_tool} for ${_out} returned ${_rc}: ${_o}")
    math(EXPR failures "${failures} + 1")
    continue()
  endif()
  if(REGEN)
    message("wrote ${_out}")
    continue()
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${_to}" "${_dir}/${_out}"
                  RESULT_VARIABLE _same)
  file(SHA256 "${_to}" _got)
  if(NOT _same EQUAL 0)
    file(SHA256 "${_dir}/${_out}" _want)
    message("FAIL ${_out}: the tool gives SHA-256 ${_got}, the checked-in file is ${_want}")
    math(EXPR failures "${failures} + 1")
  else()
    message("ok   ${_out}  SHA-256 ${_got}")
  endif()
endforeach()
if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failures; after a deliberate change, examples/demo/README.md, "
                      "\"How the assets are made\", says how to make the files again")
endif()

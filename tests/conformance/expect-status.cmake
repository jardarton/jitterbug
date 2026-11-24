# Runs one command and requires both its exact exit status and a diagnostic
# in its output, for the tests whose outcome is a refusal or a difference
# (tests/conformance/CMakeLists.txt; tests/CMakeLists.txt, "Hardware
# probe"). PASS_REGULAR_EXPRESSION and WILL_FAIL ignore the exit status, so
# a tool that printed the diagnostic and exited 0 would pass with them.
#
#   cmake -DSTATUS=N -DOUTPUT=REGEX [-DNOT_OUTPUT=REGEX] -P expect-status.cmake
#         -- [EMULATOR...] PROGRAM ARGS...
#
# STATUS is the exit status required (the runner's: 0 matches, 1 differs,
# 2 refused or an error). OUTPUT must match the output (stdout and stderr
# together), NOT_OUTPUT must not. The command is everything after "--",
# with CMAKE_CROSSCOMPILING_EMULATOR first when there is one (qemu, Node),
# which add_test does not add to a command that does not start with a
# target. The output is printed, so a test's FAIL_REGULAR_EXPRESSION still
# sees it.
if(NOT DEFINED STATUS OR NOT DEFINED OUTPUT)
  message(FATAL_ERROR "expect-status.cmake: STATUS and OUTPUT are required")
endif()
if(NOT STATUS MATCHES "^[0-9]+$")
  message(FATAL_ERROR "expect-status.cmake: STATUS ${STATUS} is not an exit status")
endif()
set(_cmd)
set(_on 0)
math(EXPR _last "${CMAKE_ARGC} - 1")
foreach(_i RANGE ${_last})
  if(_on)
    # A ";" in an argument would split it in the list: kept escaped.
    string(REPLACE ";" "\\;" _arg "${CMAKE_ARGV${_i}}")
    list(APPEND _cmd "${_arg}")
  elseif("${CMAKE_ARGV${_i}}" STREQUAL "--")
    set(_on 1)
  endif()
endforeach()
if(NOT _cmd)
  message(FATAL_ERROR "expect-status.cmake: no command after --")
endif()
execute_process(COMMAND ${_cmd} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
message("${_out}")
if(NOT "${_rc}" STREQUAL "${STATUS}")
  message(FATAL_ERROR "expect-status.cmake: exit status ${_rc}, and ${STATUS} is required")
endif()
if(NOT "${_out}" MATCHES "${OUTPUT}")
  message(FATAL_ERROR "expect-status.cmake: the output does not match: ${OUTPUT}")
endif()
if(DEFINED NOT_OUTPUT AND "${_out}" MATCHES "${NOT_OUTPUT}")
  message(FATAL_ERROR "expect-status.cmake: the output matches what it must not: ${NOT_OUTPUT}")
endif()
message("expect-status.cmake: exit status ${_rc} and the output as required")

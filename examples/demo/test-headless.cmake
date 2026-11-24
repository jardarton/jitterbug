# demo_headless_cli: demo-headless's own command line:
#   cmake -DWORK=<a scratch directory> -P test-headless.cmake --
#         [EMULATOR...] <demo-headless>
# The program is everything after "--", with CMAKE_CROSSCOMPILING_EMULATOR
# first when there is one (qemu for a cross build, Node for wasm), so it
# runs as every other test of the build runs.
#
# 1. --record on a run file with comments, tabs, trailing spaces and a CR:
#    then --expect on it passes, every byte but the hashes is as it was.
# 2. A script's keys act at their tick: P pauses (the frames repeat), N
#    steps one frame while paused, a scale key changes the frame's size,
#    and two events at one tick apply in order.
# 3. What it cannot use is refused with exit status 2 and nothing printed
#    on the standard output: numbers out of range or not whole, a script
#    that is not TICK:KEYS[,TICK:KEYS...] of its keys, an unknown option,
#    two of the checks (--gaps, --pier, --fade) at once, a run file with a
#    run of the wrong shape.
cmake_minimum_required(VERSION 3.24)

set(HEADLESS)
set(_on 0)
math(EXPR _last "${CMAKE_ARGC} - 1")
foreach(_i RANGE ${_last})
  if(_on)
    list(APPEND HEADLESS "${CMAKE_ARGV${_i}}")
  elseif(CMAKE_ARGV${_i} STREQUAL "--")
    set(_on 1)
  endif()
endforeach()
if(NOT HEADLESS)
  message(FATAL_ERROR "test-headless.cmake: the program is required after --")
endif()

set(failures 0)
macro(fail msg)
  message("FAIL ${msg}")
  math(EXPR failures "${failures} + 1")
endmacro()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(runs "${WORK}/runs.txt")

# 1. --record keeps every byte but the hashes.
set(text "# a comment\n")
string(APPEND text "one\t2\t1\t1\t0\t0\t1\t0\t0\t1\t1\t-\t0000000000000000   \r\n")
string(APPEND text "      \n")
string(APPEND text "two 3 4 1 0 0 1 0 0 1 0 1:wp,2:n ffffffffffffffff\n")
string(APPEND text "# end")
file(WRITE "${runs}" "${text}")
execute_process(COMMAND ${HEADLESS} --record "${runs}" RESULT_VARIABLE rc OUTPUT_VARIABLE out
                ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  fail("--record returned ${rc}: ${out}${err}")
endif()
execute_process(COMMAND ${HEADLESS} --expect "${runs}" RESULT_VARIABLE rc OUTPUT_VARIABLE out
                ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  fail("--expect after --record returned ${rc}: ${out}${err}")
endif()
file(WRITE "${WORK}/original.txt" "${text}")
file(READ "${WORK}/original.txt" orig_hex HEX)
file(READ "${runs}" recorded_hex HEX)
string(REPEAT "30" 16 zeros_hex)
string(REPEAT "66" 16 effs_hex)
string(REPEAT "--" 16 masked_hash)
string(REPLACE "${zeros_hex}" "${masked_hash}" orig_masked "${orig_hex}")
string(REPLACE "${effs_hex}" "${masked_hash}" orig_masked "${orig_masked}")
string(LENGTH "${orig_hex}" len)
if(NOT len EQUAL 0)
  string(LENGTH "${recorded_hex}" rlen)
  if(NOT rlen EQUAL len)
    fail("--record changed the file's length")
  else()
    # Mask the recorded hashes where the originals were.
    string(FIND "${orig_hex}" "${zeros_hex}" at1)
    string(FIND "${orig_hex}" "${effs_hex}" at2)
    string(SUBSTRING "${recorded_hex}" 0 ${at1} a)
    math(EXPR after1 "${at1} + 32")
    math(EXPR mid "${at2} - ${after1}")
    string(SUBSTRING "${recorded_hex}" ${after1} ${mid} b)
    math(EXPR after2 "${at2} + 32")
    string(SUBSTRING "${recorded_hex}" ${after2} -1 c)
    set(recorded_masked "${a}${masked_hash}${b}${masked_hash}${c}")
    if(NOT recorded_masked STREQUAL orig_masked)
      fail("--record changed bytes other than the hashes")
    endif()
    string(SUBSTRING "${recorded_hex}" ${at1} 32 h1)
    string(SUBSTRING "${recorded_hex}" ${at2} 32 h2)
    if(h1 STREQUAL zeros_hex OR h2 STREQUAL effs_hex)
      fail("--record did not write the hashes")
    endif()
  endif()
endif()

# 2. Keys at their ticks. Each line of output for --hash is
# "tick T (frame F): HASH".
function(hashes out_var)
  execute_process(COMMAND ${HEADLESS} ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out
                  ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message("FAIL ${ARGN} returned ${rc}: ${out}${err}")
    set(${out_var} "failed" PARENT_SCOPE)
    return()
  endif()
  string(REGEX MATCHALL "tick [0-9]+ \\(frame [0-9]+\\): [0-9a-f]+" lines "${out}")
  set(${out_var} "${lines}" PARENT_SCOPE)
endfunction()
hashes(h --frames 5 --script 1:p,3:n --hash 0 --hash 1 --hash 2 --hash 3 --hash 4)
list(LENGTH h n)
if(NOT n EQUAL 5)
  fail("a paused run: ${h}")
else()
  list(GET h 0 h0)
  list(GET h 1 h1)
  list(GET h 2 h2)
  list(GET h 3 h3)
  list(GET h 4 h4)
  # Ticks 1 and 2 show frame 1, paused at tick 1; tick 3 steps to frame 2.
  if(NOT h1 MATCHES "^tick 1 \\(frame 1\\)" OR NOT h2 MATCHES "^tick 2 \\(frame 1\\)" OR
     NOT h3 MATCHES "^tick 3 \\(frame 2\\)" OR NOT h4 MATCHES "^tick 4 \\(frame 2\\)")
    fail("P and N did not pause and step: ${h}")
  endif()
  string(REGEX REPLACE ".*: " "" x1 "${h1}")
  string(REGEX REPLACE ".*: " "" x2 "${h2}")
  string(REGEX REPLACE ".*: " "" x3 "${h3}")
  string(REGEX REPLACE ".*: " "" x4 "${h4}")
  if(NOT x1 STREQUAL x2 OR NOT x3 STREQUAL x4 OR x2 STREQUAL x3)
    fail("paused frames differ, or the step drew the same frame: ${h}")
  endif()
endif()
# Keys at one tick apply in the order given: 3 then 2 is 2x, which 2 then
# 3 would not be (1281 x 720 with W).
execute_process(COMMAND ${HEADLESS} --frames 2 --script 1:3,1:2,1:w --png 1 "${WORK}/f.png"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "wrote .*f.png \\(854 x 480\\)")
  fail("keys 3, 2, W at one tick did not make an 854 x 480 frame: ${rc} ${out}${err}")
endif()

# 3. Refusals: status 2, nothing on the standard output, and a message that
# names the option and the value given (so each case reached the program
# as that option and value: a case is "OPTION VALUE", split at its space).
set(bad_args
  "--frames -1" "--frames 1.5" "--step 0" "--step 65" "--scale 9" "--fog 2" "--panel x"
  "--threads 65" "--script 5" "--script 5:" "--script 5:x" "--script :v" "--script 1:v,"
  "--script 1:vvvvvvvvv" "--script 1:v,,2:t" "--start -3" "--no-such-option")
foreach(case IN LISTS bad_args)
  separate_arguments(args UNIX_COMMAND "${case}")
  execute_process(COMMAND ${HEADLESS} ${args} --frames 1 RESULT_VARIABLE rc OUTPUT_VARIABLE out
                  ERROR_VARIABLE err)
  string(FIND "${err}" "${case}" named)
  if(NOT rc EQUAL 2 OR NOT out STREQUAL "" OR named LESS 0)
    fail("${case}: status ${rc} (want 2), output \"${out}\", message \"${err}\"")
  endif()
endforeach()
# The checks one at a time: any two together are refused, naming them.
foreach(pair "--gaps 1 --pier 1" "--gaps 1 --fade 17" "--pier 1 --fade 17")
  separate_arguments(args UNIX_COMMAND "${pair}")
  execute_process(COMMAND ${HEADLESS} ${args} --frames 1 --panel 0 RESULT_VARIABLE rc
                  OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(FIND "${err}" "one at a time" named)
  if(NOT rc EQUAL 2 OR NOT out STREQUAL "" OR named LESS 0)
    fail("${pair}: status ${rc} (want 2), output \"${out}\", message \"${err}\"")
  endif()
endforeach()
file(WRITE "${WORK}/bad.txt" "one 2 1 1 0 0 1 0 0 1 1 0000000000000000\n")
execute_process(COMMAND ${HEADLESS} --expect "${WORK}/bad.txt" RESULT_VARIABLE rc
                OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT out STREQUAL "")
  fail("a run without its script column: status ${rc} (want 2), output \"${out}\"")
endif()
file(WRITE "${WORK}/bad.txt" "one 2 1 1 0 0 1 0 0 1 1 3:q 0000000000000000\n")
execute_process(COMMAND ${HEADLESS} --expect "${WORK}/bad.txt" RESULT_VARIABLE rc
                OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 2 OR NOT out STREQUAL "")
  fail("a run with a bad script: status ${rc} (want 2), output \"${out}\"")
endif()

if(failures GREATER 0)
  message(FATAL_ERROR "${failures} failures")
endif()
message("demo_headless_cli: all passed")

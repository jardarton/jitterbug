# first_light_headless_cli: first-light-headless's own command line:
#   cmake -DWORK=<a scratch directory> -P test-headless.cmake --
#         [EMULATOR...] <first-light-headless>
# The program is everything after "--", with CMAKE_CROSSCOMPILING_EMULATOR
# first when there is one (qemu for a cross build, Node for wasm), so it
# runs as every other test of the build runs.
#
# 1. --record on a valid run file that the reader takes but a careless
#    writer would spoil: 300 comment lines before the runs, a run with tabs
#    between its columns, spaces after its hash and a CR before its newline,
#    a line of spaces, a run with spaces, a last comment without a newline.
#    Then --expect on the file passes, every byte but the hashes is as it
#    was, and recording again changes nothing.
# 2. --record on a file with a run it cannot read fails and leaves the file
#    as it was.
# 3. --pause-at F draws scene frame F again for every later frame, as a
#    paused window does.
# 4. Six --record runs at once on one file (with a large comment, so that
#    they overlap) all succeed, and the file is there and whole after them,
#    with no file of theirs left beside it.
# 5. Numbers that are not whole or not in range are refused (exit status
#    2), on the command line and in a run file, as is a hash that is not 1
#    to 16 hex digits or a NUL byte in the file: before anything is drawn,
#    printed or written.
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

# 1. A run file a careless writer would spoil.
set(text "")
foreach(i RANGE 1 300)
  string(APPEND text "# comment ${i}\n")
endforeach()
set(row1 "one\t2\t1\t0\t0\t1\t0\t0\t1\t")
string(APPEND text "${row1}0000000000000000   \r\n")
string(APPEND text "      \n")
string(APPEND text "two 2 2 0 0 1 1 0 0 ffffffffffffffff\n")
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
# Every byte (in hex: file(READ) would drop the CRs) as it was, but the two
# hash fields, which are masked in both, and which must have changed.
file(WRITE "${WORK}/original.txt" "${text}")
file(READ "${WORK}/original.txt" orig_hex HEX)
file(READ "${runs}" recorded_hex HEX)
string(REPEAT "30" 16 zeros_hex)
string(REPEAT "66" 16 effs_hex)
string(REPEAT "--" 16 masked_hash)
string(FIND "${orig_hex}" "${zeros_hex}" p1)
string(FIND "${orig_hex}" "${effs_hex}" p2)
function(mask_hashes hex out)
  math(EXPR mid_at "${p1} + 32")
  math(EXPR mid_len "${p2} - ${p1} - 32")
  math(EXPR end_at "${p2} + 32")
  string(SUBSTRING "${hex}" 0 ${p1} a)
  string(SUBSTRING "${hex}" ${mid_at} ${mid_len} b)
  string(SUBSTRING "${hex}" ${end_at} -1 c)
  set(${out} "${a}${masked_hash}${b}${masked_hash}${c}" PARENT_SCOPE)
endfunction()
string(LENGTH "${orig_hex}" orig_len)
string(LENGTH "${recorded_hex}" rec_len)
if(p1 LESS 0 OR p2 LESS p1 OR NOT orig_len EQUAL rec_len)
  fail("the run file's length changed (${orig_len} to ${rec_len} hex digits)")
else()
  mask_hashes("${orig_hex}" orig_masked)
  mask_hashes("${recorded_hex}" rec_masked)
  string(SUBSTRING "${recorded_hex}" ${p1} 32 new1)
  string(SUBSTRING "${recorded_hex}" ${p2} 32 new2)
  if(NOT rec_masked STREQUAL orig_masked)
    fail("bytes other than the hashes changed")
  endif()
  if(new1 STREQUAL zeros_hex OR new2 STREQUAL effs_hex)
    fail("an old hash stayed")
  endif()
endif()
file(SHA256 "${runs}" first)
execute_process(COMMAND ${HEADLESS} --record "${runs}" RESULT_VARIABLE rc OUTPUT_QUIET)
file(SHA256 "${runs}" again)
if(NOT rc EQUAL 0 OR NOT again STREQUAL first)
  fail("recording again changed the file")
endif()
if(EXISTS "${runs}.tmp")
  fail("the temporary file was left")
endif()

# 2. A run it cannot read: refused, the file untouched.
set(bad "# a run with a column missing\nthree 2 1 0 0 1 0 0 0000000000000000\n")
file(WRITE "${WORK}/bad.txt" "${bad}")
execute_process(COMMAND ${HEADLESS} --record "${WORK}/bad.txt" RESULT_VARIABLE rc
                OUTPUT_QUIET ERROR_QUIET)
file(READ "${WORK}/bad.txt" bad_after)
if(NOT rc EQUAL 2 OR NOT bad_after STREQUAL bad)
  fail("a run file with a short run: --record returned ${rc}, the file now:\n${bad_after}")
endif()

# 3. --pause-at, against a run that does not pause: frames 2 and 3 as that
#    run draws them, and frame 5 the same as its frame 3.
execute_process(COMMAND ${HEADLESS} --frames 6 --hash 2 --hash 3 RESULT_VARIABLE rc_u
                OUTPUT_VARIABLE out_u)
string(REGEX MATCH "frame 2: ([0-9a-f]+)" m "${out_u}")
set(u2 "${CMAKE_MATCH_1}")
string(REGEX MATCH "frame 3: ([0-9a-f]+)" m "${out_u}")
set(u3 "${CMAKE_MATCH_1}")
execute_process(COMMAND ${HEADLESS} --frames 6 --pause-at 3 --hash 2 --hash 3 --hash 5
                RESULT_VARIABLE rc OUTPUT_VARIABLE out)
string(REGEX MATCH "frame 2: ([0-9a-f]+)" m "${out}")
set(h2 "${CMAKE_MATCH_1}")
string(REGEX MATCH "frame 3: ([0-9a-f]+)" m "${out}")
set(h3 "${CMAKE_MATCH_1}")
string(REGEX MATCH "frame 5: ([0-9a-f]+)" m "${out}")
set(h5 "${CMAKE_MATCH_1}")
if(NOT rc EQUAL 0 OR NOT rc_u EQUAL 0 OR u2 STREQUAL "" OR u3 STREQUAL "" OR h2 STREQUAL ""
   OR h3 STREQUAL "" OR h5 STREQUAL "" OR NOT h2 STREQUAL u2 OR NOT h3 STREQUAL u3
   OR NOT h5 STREQUAL u3 OR u2 STREQUAL u3)
  fail("--pause-at 3: frames 2, 3, 5 hash ${h2}, ${h3}, ${h5}; unpaused 2, 3: ${u2}, ${u3} "
       "(rc ${rc}, ${rc_u})")
endif()

# 4. Concurrent recordings.
set(big "")
string(REPEAT "# a large comment to make the writes overlap .......................\n" 160000
       big)
set(concurrent "${big}conc 2 1 0 0 1 0 0 1 0000000000000000\n")
file(WRITE "${WORK}/concurrent.txt" "${concurrent}")
file(SIZE "${WORK}/concurrent.txt" conc_size)
# execute_process runs several COMMANDs at once, as a pipeline, each one's
# output into the next; each goes through a cmake -P wrapper that drops its
# output, so that none of them is stopped by a closed pipe.
file(WRITE "${WORK}/quiet.cmake" [=[
set(cmd)
set(started 0)
math(EXPR last "${CMAKE_ARGC} - 1")
foreach(i RANGE ${last})
  if(started)
    list(APPEND cmd "${CMAKE_ARGV${i}}")
  elseif(CMAKE_ARGV${i} STREQUAL "--")
    set(started 1)
  endif()
endforeach()
execute_process(COMMAND ${cmd} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "${rc}")
endif()
]=])
set(cmds)
foreach(k RANGE 1 6)
  list(APPEND cmds COMMAND "${CMAKE_COMMAND}" -P "${WORK}/quiet.cmake" -- ${HEADLESS} --record
       "${WORK}/concurrent.txt")
endforeach()
execute_process(${cmds} RESULTS_VARIABLE rcs OUTPUT_QUIET ERROR_VARIABLE err)
foreach(rc IN LISTS rcs)
  if(NOT rc EQUAL 0)
    fail("a concurrent --record returned ${rc}: ${err}")
    break()
  endif()
endforeach()
if(NOT EXISTS "${WORK}/concurrent.txt")
  fail("concurrent recordings removed the run file")
else()
  file(SIZE "${WORK}/concurrent.txt" conc_after)
  if(NOT conc_after EQUAL conc_size)
    fail("concurrent recordings left a file of ${conc_after} bytes, not ${conc_size}")
  endif()
  execute_process(COMMAND ${HEADLESS} --expect "${WORK}/concurrent.txt" RESULT_VARIABLE rc
                  OUTPUT_QUIET ERROR_QUIET)
  if(NOT rc EQUAL 0)
    fail("--expect after concurrent recordings returned ${rc}")
  endif()
endif()
file(GLOB left "${WORK}/concurrent.txt.*")
if(left)
  fail("files left beside the run file: ${left}")
endif()

# 5. Malformed numbers.
foreach(args "--frames;3;--pause-at;yesterday" "--scale;9" "--scale;0" "--dither;2"
             "--float;yes" "--frames;-1" "--frames;12x" "--hash;99999999999999999999"
             "--threads;65" "--precise; 1")
  execute_process(COMMAND ${HEADLESS} ${args} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
  if(NOT rc EQUAL 2)
    fail("${args}: exit status ${rc}, not 2")
  endif()
endforeach()
# A run file it cannot use, each refused (status 2) before anything is
# drawn, printed or written: a good run first, then a run with garbage in
# a column; a hash past 64 bits; a hash that is not hex; a frame count of 0.
set(bad_files
  "good 1 1 0 0 1 0 0 1 0000000000000000\nfour 2 1 0 0 1 0 garbage 1 0000000000000000\n"
  "five 1 1 0 0 1 0 0 1 10000000000000000\n"
  "six 1 1 0 0 1 0 0 1 zz\n"
  "seven 0 1 0 0 1 0 0 1 0\n")
set(k 0)
foreach(content IN LISTS bad_files)
  math(EXPR k "${k} + 1")
  file(WRITE "${WORK}/refused${k}.txt" "${content}")
  foreach(mode --expect --record)
    execute_process(COMMAND ${HEADLESS} ${mode} "${WORK}/refused${k}.txt" RESULT_VARIABLE rc
                    OUTPUT_VARIABLE out ERROR_QUIET)
    file(READ "${WORK}/refused${k}.txt" after)
    if(NOT rc EQUAL 2 OR NOT out STREQUAL "" OR NOT after STREQUAL content)
      fail("run file ${k}, ${mode}: status ${rc} (not 2), output \"${out}\"")
    endif()
  endforeach()
endforeach()

# A run file with a NUL byte after a good run (refused-nul.txt, beside this
# script): refused (status 2) in both modes, the file untouched, rather
# than read up to the NUL.
foreach(mode --expect --record)
  configure_file("${CMAKE_CURRENT_LIST_DIR}/refused-nul.txt" "${WORK}/refused-nul.txt" COPYONLY)
  file(READ "${WORK}/refused-nul.txt" before HEX)
  execute_process(COMMAND ${HEADLESS} ${mode} "${WORK}/refused-nul.txt" RESULT_VARIABLE rc
                  OUTPUT_VARIABLE out ERROR_QUIET)
  file(READ "${WORK}/refused-nul.txt" after HEX)
  if(NOT rc EQUAL 2 OR NOT out STREQUAL "" OR NOT after STREQUAL before)
    fail("run file with a NUL, ${mode}: status ${rc} (not 2), output \"${out}\"")
  endif()
endforeach()

if(failures GREATER 0)
  message(FATAL_ERROR "first_light_headless_cli: ${failures} failures")
endif()
message(STATUS "first_light_headless_cli: ok")

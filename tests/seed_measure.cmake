# seed_measure: tools/seed-record/measure.sh fails unless it compared
# every dump (docs/testing.md, "Seed traces and re-blessing").
#   cmake -DBASH=... -DMEASURE=... -DREPLAY=... -DFALSE=... -DWORK=... -P seed_measure.cmake
# Two replayers that do nothing, and a real old one with a new one that
# does nothing, must fail; the replayer against itself must pass, having
# compared every image with none missing.
file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK})

function(measure want old new)
  execute_process(COMMAND ${CMAKE_COMMAND} -E env WORK=${WORK} ${BASH} ${MEASURE} ${old} ${new} 1
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  message(STATUS "measure.sh ${old} ${new}: exit ${rc}\n${out}${err}")
  if(want STREQUAL "pass" AND NOT rc EQUAL 0)
    message(FATAL_ERROR "measure.sh failed comparing a replayer with itself")
  elseif(want STREQUAL "pass" AND NOT out MATCHES "VRAM images, 0 differ, 0 missing")
    message(FATAL_ERROR "measure.sh passed without reporting a complete comparison")
  elseif(want STREQUAL "fail" AND rc EQUAL 0)
    message(FATAL_ERROR "measure.sh passed with a replayer that did not replay")
  endif()
endfunction()

measure(fail ${FALSE} ${FALSE})
measure(fail ${REPLAY} ${FALSE})
measure(fail ${FALSE} ${REPLAY})
measure(pass ${REPLAY} ${REPLAY})

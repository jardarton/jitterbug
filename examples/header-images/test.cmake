# The header images, drawn again and compared with the checked-in ones:
#   cmake -DWORK=<dir> -DIMAGES=<docs/images> -DPAGE=<demo's index.html>
#     -P test.cmake -- [emulator...] <header-images>
# Every file must be the same bytes (the logo's every frame with it), and
# the demo page's favicon must be beetle.png as a data URI.
cmake_minimum_required(VERSION 3.24)

set(_cmd)
set(_after OFF)
foreach(_i RANGE ${CMAKE_ARGC})
  if(_after AND DEFINED CMAKE_ARGV${_i})
    list(APPEND _cmd "${CMAKE_ARGV${_i}}")
  elseif("${CMAKE_ARGV${_i}}" STREQUAL "--")
    set(_after ON)
  endif()
endforeach()
if(NOT _cmd)
  message(FATAL_ERROR "test.cmake: no program after --")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND ${_cmd} "${WORK}" RESULT_VARIABLE _status)
if(NOT _status EQUAL 0)
  message(FATAL_ERROR "header-images failed: ${_status}")
endif()

set(_failed OFF)
foreach(_name logo.png beetle.png banner.png)
  file(SHA256 "${WORK}/${_name}" _got)
  file(SHA256 "${IMAGES}/${_name}" _want)
  if(_got STREQUAL _want)
    message(STATUS "${_name}: the same bytes (${_got})")
  else()
    message(STATUS "${_name}: FAIL, drawn ${_got}, checked in ${_want}")
    set(_failed ON)
  endif()
endforeach()
if(_failed)
  message(FATAL_ERROR "the images in ${IMAGES} are not what header-images draws now: if the change "
    "is meant, draw them again (cmake --build <build> --target header_images_regen)")
endif()

execute_process(COMMAND ${_cmd} --check-favicon "${PAGE}" "${IMAGES}/beetle.png" RESULT_VARIABLE _status)
if(NOT _status EQUAL 0)
  message(FATAL_ERROR "${PAGE}: its favicon is not ${IMAGES}/beetle.png")
endif()

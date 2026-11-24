# Conformance (docs/conformance.md): ps1-tests' GPU programs as
# recorded lists (FORMAT.md, tools/conformance-record), played through the
# public API at 1x and compared with the console's VRAM dumps and logs
# (third_party/ps1-tests).
#   conformance_NAME      one program (conformance.c); a mismatch writes
#                         NAME.ours.png and NAME.diff.png into
#                         conformance-out in the build tree
#   conformance_NAME_2x, _3x   the same program on a context at 2x (four
#                         threads) and 3x (three), comparing the 1x VRAM
#                         kept beside the scaled picture on the same terms
#                         (15 colour bits, the same exclusions); images into
#                         conformance-out/2x and /3x
#   conformance_selftest  the instrument against itself (selftest.c)
# fixtures.txt says, for each program, what its test must see: the list's
# identity, the reference, the pass of the main loop compared, the number
# of checks, and the one documented exclusion set (lines'), which the
# runner checks pixel for pixel. No other exclusion is used. The runner
# reads all of it from the table itself (--fixtures fixtures.txt --fixture
# NAME), and the dump and expected-failures.txt from beside it: nothing of
# a program's identity is passed as an option, so no test can state
# another. This file only checks the table and the files
# it names, and builds a test for each line.
#   conformance_refuse_*  the runner refusing what the table does not say
#                         (exit status 2) and finding a difference (1), each
#                         status and diagnostic required (expect-status.cmake)
# Programs marked "fetched" there have no list or dump in the repository
# (third_party/ps1-tests/README.md); their tests are built only when
# JB_CONFORMANCE_FETCHED names the flake's conformance-lists output, as
# every build in `nix flake check` does.
# A program in expected-failures.txt is judged as an expected mismatch (the
# runner reads the file beside the table): it passes while its VRAM or
# checks differ and fails once it matches, so the list has to shrink with
# each fix. docs/conformance.md, "Rules fitted from the console results",
# says why each differed. Single-threaded builds play the lists on one thread;
# the self-test's
# four-thread runs are then clamped to one, as jb_config says.

set(JB_CONFORMANCE_FETCHED "" CACHE PATH
  "The output of `nix build .#conformance-lists`, for the conformance tests whose lists and dumps are not in the repository")

add_library(jb_jbc STATIC jbc.c)
set_target_properties(jb_jbc PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(jb_jbc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(jb_jbc PUBLIC jitterbug jb_stb PRIVATE jb_build_flags)

foreach(_t conformance selftest)
  add_executable(test_conformance_${_t} ${_t}.c)
  set_target_properties(test_conformance_${_t} PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_link_libraries(test_conformance_${_t} PRIVATE jb_jbc jb_build_flags)
endforeach()

set(_jbc_dir ${CMAKE_CURRENT_SOURCE_DIR})
set(_jbc_hw ${PROJECT_SOURCE_DIR}/third_party/ps1-tests/gpu)
set(_jbc_out ${CMAKE_CURRENT_BINARY_DIR}/conformance-out)
file(MAKE_DIRECTORY ${_jbc_out})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS fixtures.txt expected-failures.txt
  ${PROJECT_SOURCE_DIR}/tools/conformance-record/programs.txt)

# The recorded programs (programs.txt) and their fixtures (fixtures.txt):
# the same names, in the same order.
file(STRINGS ${PROJECT_SOURCE_DIR}/tools/conformance-record/programs.txt _jbc_lines
  REGEX "^[a-z0-9-]+ [0-9a-f]+( [0-9a-f]+)?$")
set(_jbc_recorded)
foreach(_line ${_jbc_lines})
  string(REGEX MATCH "^[a-z0-9-]+" _p "${_line}")
  list(APPEND _jbc_recorded ${_p})
endforeach()

file(STRINGS fixtures.txt _jbc_lines)
set(_jbc_programs)
set(_jbc_excludes)
foreach(_line ${_jbc_lines})
  if(_line MATCHES "^#" OR _line STREQUAL "")
    continue()
  endif()
  string(REGEX REPLACE " +" ";" _f "${_line}")
  list(LENGTH _f _n)
  if(NOT (_n EQUAL 8 OR _n EQUAL 11))
    message(FATAL_ERROR "fixtures.txt: bad line: ${_line}")
  endif()
  list(GET _f 0 _p)
  list(GET _f 1 _where)
  list(GET _f 2 _ref)
  list(GET _f 3 _passes)
  list(GET _f 4 _expects)
  list(GET _f 5 _words)
  list(GET _f 6 _sum)
  list(GET _f 7 _refsum)
  if(NOT _where MATCHES "^(repo|fetched)$" OR NOT _ref MATCHES "^(dump|log)$" OR
     NOT _passes MATCHES "^[0-9]+$" OR NOT _expects MATCHES "^[0-9]+$" OR
     NOT _words MATCHES "^[0-9]+$" OR NOT _sum MATCHES "^[0-9a-f]+$" OR
     NOT _refsum MATCHES "^[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]$")
    message(FATAL_ERROR "fixtures.txt: bad line: ${_line}")
  endif()
  list(APPEND _jbc_programs ${_p})
  set(_jbc_${_p}_where ${_where})
  # What the runner is given: the table and the program's name. It reads
  # the identity, the reference and the exclusion from the line itself.
  set(_jbc_${_p}_id --fixtures ${_jbc_dir}/fixtures.txt --fixture ${_p})
  set(_jbc_${_p}_passes ${_passes})
  set(_jbc_${_p}_ref ${_ref})
  if(_n EQUAL 11)
    list(GET _f 8 _ex)
    list(APPEND _jbc_excludes ${_ex})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_ex})
    if(NOT _ref STREQUAL "dump" OR NOT EXISTS ${_jbc_dir}/${_ex})
      message(FATAL_ERROR "fixtures.txt: ${_p}: an exclusion is for a dump, in a file here: ${_line}")
    endif()
  endif()
endforeach()
if(NOT _jbc_programs STREQUAL _jbc_recorded)
  message(FATAL_ERROR "tests/conformance/fixtures.txt and tools/conformance-record/programs.txt "
    "name different programs:\n  ${_jbc_programs}\n  ${_jbc_recorded}")
endif()
# No exclusion file is picked up by its name: each one is in fixtures.txt.
file(GLOB _jbc_found RELATIVE ${_jbc_dir} ${_jbc_dir}/*.exclude)
foreach(_ex ${_jbc_found})
  if(NOT _ex IN_LIST _jbc_excludes)
    message(FATAL_ERROR "tests/conformance/${_ex} is not in fixtures.txt")
  endif()
endforeach()

# expected-failures.txt: a program name first on a line; # starts a comment.
file(STRINGS expected-failures.txt _jbc_lines)
set(_jbc_expected)
foreach(_line ${_jbc_lines})
  string(REGEX REPLACE "#.*" "" _line "${_line}")
  string(STRIP "${_line}" _line)
  if(_line STREQUAL "")
    continue()
  endif()
  string(REGEX MATCH "^[a-z0-9-]+" _name "${_line}")
  if(NOT _name IN_LIST _jbc_programs)
    message(FATAL_ERROR "expected-failures.txt: ${_name} is not a recorded program")
  endif()
  list(APPEND _jbc_expected ${_name})
endforeach()

if(NOT JB_CONFORMANCE_FETCHED STREQUAL "")
  get_filename_component(_jbc_fetched "${JB_CONFORMANCE_FETCHED}" ABSOLUTE)
  if(NOT EXISTS ${_jbc_fetched}/NOTICE.md)
    message(FATAL_ERROR "JB_CONFORMANCE_FETCHED (${JB_CONFORMANCE_FETCHED}) is not the output of "
      "`nix build .#conformance-lists`")
  endif()
endif()

set(_jbc_lists)
set(_jbc_missing)
foreach(_p ${_jbc_programs})
  if(_jbc_${_p}_where STREQUAL "repo")
    set(_list ${_jbc_dir}/${_p}.jbc)
    set(_ref_dir ${_jbc_hw}/${_p})
  elseif(JB_CONFORMANCE_FETCHED STREQUAL "")
    list(APPEND _jbc_missing ${_p})
    continue()
  else()
    set(_list ${_jbc_fetched}/tests/conformance/${_p}.jbc)
    set(_ref_dir ${_jbc_fetched}/third_party/ps1-tests/gpu/${_p})
  endif()
  if(NOT EXISTS ${_list})
    message(FATAL_ERROR "conformance: ${_list} is missing")
  endif()
  list(APPEND _jbc_lists ${_list})
  set(_jbc_${_p}_list ${_list})
  set(_args ${_jbc_${_p}_id})
  # The runner finds the dump or log itself (third_party/ps1-tests/gpu/NAME
  # beside the table's directory, or beside the fetched list's) and checks
  # its REFSUM: here only its presence is checked, for a clear message at
  # configure time.
  if(_jbc_${_p}_ref STREQUAL "dump" AND NOT EXISTS ${_ref_dir}/vram.png)
    message(FATAL_ERROR "conformance: ${_ref_dir}/vram.png is missing")
  endif()
  if(_jbc_${_p}_ref STREQUAL "log" AND NOT EXISTS ${_ref_dir}/psx.log)
    message(FATAL_ERROR "conformance: ${_ref_dir}/psx.log is missing")
  endif()
  add_test(NAME conformance_${_p}
    COMMAND test_conformance_conformance --out ${_jbc_out} ${_args} ${_list})
  # The same against the 1x VRAM a scaled context keeps (jb_vram_read):
  # 2x on four threads, 3x on three.
  foreach(_run "2;4" "3;3")
    list(GET _run 0 _s)
    list(GET _run 1 _t)
    file(MAKE_DIRECTORY ${_jbc_out}/${_s}x)
    add_test(NAME conformance_${_p}_${_s}x
      COMMAND test_conformance_conformance --scale ${_s} --threads ${_t} --out ${_jbc_out}/${_s}x
        ${_args} ${_list})
  endforeach()
endforeach()
# For the hardware probe's tests (tests/CMakeLists.txt, "Hardware probe"),
# which play the same lists: every program, those whose lists this build
# has and those it has not (the fetched ones without JB_CONFORMANCE_FETCHED),
# and for each one it has, its list and its fixture's identity and pass.
set(JBC_PROGRAMS ${_jbc_programs} PARENT_SCOPE)
set(JBC_MISSING ${_jbc_missing} PARENT_SCOPE)
foreach(_p ${_jbc_programs})
  if(NOT _p IN_LIST _jbc_missing)
    set(JBC_${_p}_LIST ${_jbc_${_p}_list} PARENT_SCOPE)
    set(JBC_${_p}_ID ${_jbc_${_p}_id} PARENT_SCOPE)
    set(JBC_${_p}_PASSES ${_jbc_${_p}_passes} PARENT_SCOPE)
  endif()
endforeach()
set(JBC_OUT ${_jbc_out} PARENT_SCOPE)

if(_jbc_missing)
  list(JOIN _jbc_missing ", " _m)
  message(STATUS "conformance: not built: ${_m}. Their lists and dumps contain images that are "
    "not in this repository (third_party/ps1-tests/README.md). To test them too: "
    "nix build .#conformance-lists, then configure with "
    "-DJB_CONFORMANCE_FETCHED=<the result path> (the flake's checks do).")
endif()

# The self-test: a dump program and its dump, a program with checks, the
# fixture whose truncation must be refused (gp0-e1), the documented
# exclusion (lines', from the table), and every list played in several
# ways.
add_test(NAME conformance_selftest
  COMMAND test_conformance_selftest
    --dump-list ${_jbc_dir}/triangle.jbc ${_jbc_hw}/triangle/vram.png
    --check-list ${_jbc_dir}/mask-bit.jbc
    --fixtures ${_jbc_dir}/fixtures.txt
    --cut gp0-e1 ${_jbc_dir}/gp0-e1.jbc
    --exclusion-of lines
    --scratch ${CMAKE_CURRENT_BINARY_DIR}/selftest-scratch
    --lists ${_jbc_lists})
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/selftest-scratch)

# The tests below that require an exact exit status and a diagnostic use
# jb_expect_status (tests/CMakeLists.txt, run by expect-status.cmake here).

# The runner refuses (2) anything the table does not say, and finds a
# difference (1). A forged run (the triangle fixture with quad's dump and
# an exclusion of all of VRAM) cannot be stated: every option it used is
# refused, and an exclusion comes only from the table.
set(_jbc_run test_conformance_conformance)
foreach(_opt "--exclude;${_jbc_dir}/lines.exclude" "--exclude-pixels;524288"
    "--exclude-checksum;c8b763b6a12ce325" "--dump;${_jbc_hw}/quad/vram.png" "--reference;dump"
    "--passes;1" "--expects;0" "--words;231" "--checksum;7ac8ae2f252017e3" "--expect-mismatch")
  list(GET _opt 0 _o)
  string(REGEX REPLACE "^--" "" _t "${_o}")
  jb_expect_status(conformance_refuse_option_${_t} 2
    "ERROR ${_o}: the fixture table states what is played and compared"
    COMMAND ${_jbc_run} ${_jbc_triangle_id} ${_opt} ${_jbc_dir}/triangle.jbc)
endforeach()
# The old way of stating a fixture: refused as a whole.
jb_expect_status(conformance_refuse_r2b1_attack 2
  "ERROR --reference: the fixture table states"
  COMMAND ${_jbc_run} --fixture triangle --reference dump --passes 1 --expects 0 --words 231
    --checksum 7ac8ae2f252017e3 --dump ${_jbc_hw}/quad/vram.png --exclude ${_jbc_dir}/lines.exclude
    --exclude-pixels 64 --exclude-checksum 8dac3f054012b465 ${_jbc_dir}/triangle.jbc)
# Another program's list under a fixture's name.
jb_expect_status(conformance_refuse_other_list 2
  "ERROR [^\n]*quad.jbc is not the fixture triangle of [^\n]*: the list is gpu/quad, not gpu/triangle"
  COMMAND ${_jbc_run} ${_jbc_triangle_id} ${_jbc_dir}/quad.jbc)
# A name the table does not have.
jb_expect_status(conformance_refuse_unknown_fixture 2 "has no line for no-such-program"
  COMMAND ${_jbc_run} --fixtures ${_jbc_dir}/fixtures.txt --fixture no-such-program
    ${_jbc_dir}/triangle.jbc)
# A table that names a program twice, or is malformed, is no authority.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/refuse/twice.txt
  "triangle repo dump 1 0 231 7ac8ae2f252017e3 c5af46d9b0191449\ntriangle repo dump 1 0 231 7ac8ae2f252017e3 c5af46d9b0191449\n")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/refuse/malformed.txt
  "triangle repo dump 1 0 231 7ac8ae2f252017e3 c5af46d9b0191449 all.exclude 524288\n")
jb_expect_status(conformance_refuse_table_twice 2 "line 2: a second line for triangle"
  COMMAND ${_jbc_run} --fixtures ${CMAKE_CURRENT_BINARY_DIR}/refuse/twice.txt --fixture triangle
    ${_jbc_dir}/triangle.jbc)
jb_expect_status(conformance_refuse_table_malformed 2 "line 1: malformed"
  COMMAND ${_jbc_run} --fixtures ${CMAKE_CURRENT_BINARY_DIR}/refuse/malformed.txt --fixture triangle
    ${_jbc_dir}/triangle.jbc)
# The whole table is checked, not only the line asked for. The real
# table with a second line for quad, or with a malformed line for another
# program (an exclusion with a field missing, a dump without its REFSUM),
# is refused when triangle is asked for.
file(READ ${_jbc_dir}/fixtures.txt _jbc_table)
string(REGEX MATCH "\nquad [^\n]*" _jbc_quad "${_jbc_table}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/refuse/other-twice.txt "${_jbc_table}${_jbc_quad}\n")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/refuse/other-malformed.txt
  "${_jbc_table}other repo dump 1 0 2756 db852b116de72493 efdc1651b890309e x.exclude 64\n")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/refuse/other-no-refsum.txt
  "quad repo dump 1 0 316 5aac3474f615c6dd\n%s\n")
jb_expect_status(conformance_refuse_table_other_twice 2 "a second line for quad"
  COMMAND ${_jbc_run} --fixtures ${CMAKE_CURRENT_BINARY_DIR}/refuse/other-twice.txt
    --fixture triangle ${_jbc_dir}/triangle.jbc)
jb_expect_status(conformance_refuse_table_other_malformed 2 "other-malformed.txt, line [0-9]+: malformed"
  COMMAND ${_jbc_run} --fixtures ${CMAKE_CURRENT_BINARY_DIR}/refuse/other-malformed.txt
    --fixture triangle ${_jbc_dir}/triangle.jbc)
jb_expect_status(conformance_refuse_table_other_no_refsum 2 "other-no-refsum.txt, line 1: malformed"
  COMMAND ${_jbc_run} --fixtures ${CMAKE_CURRENT_BINARY_DIR}/refuse/other-no-refsum.txt
    --fixture triangle ${_jbc_dir}/triangle.jbc)
# The same for the console results' table (probe/fixtures.txt's format):
# another result's second line, or its malformed exclusion, is refused
# before triangle's line is taken.
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/refuse/probe-twice/probe
  ${CMAKE_CURRENT_BINARY_DIR}/refuse/probe-malformed/probe)
foreach(_t probe-twice probe-malformed)
  file(COPY_FILE ${_jbc_dir}/fixtures.txt ${CMAKE_CURRENT_BINARY_DIR}/refuse/${_t}/fixtures.txt)
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/refuse/probe-twice/probe/fixtures.txt
  "quad ps1-tests\nquad ps1-tests\ntriangle ps1-tests\n")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/refuse/probe-malformed/probe/fixtures.txt
  "quad ps1-tests quad.exclude 64 not-hex\ntriangle ps1-tests\n")
jb_expect_status(conformance_refuse_probe_table_other_twice 2 "line 2: a second line for quad"
  COMMAND ${_jbc_run} --probe-fixtures ${CMAKE_CURRENT_BINARY_DIR}/refuse/probe-twice/probe/fixtures.txt
    --fixture triangle ${_jbc_dir}/triangle.jbc)
jb_expect_status(conformance_refuse_probe_table_other_malformed 2 "line 1: malformed"
  COMMAND ${_jbc_run} --probe-fixtures
    ${CMAKE_CURRENT_BINARY_DIR}/refuse/probe-malformed/probe/fixtures.txt
    --fixture triangle ${_jbc_dir}/triangle.jbc)
# A table beside a different dump. The dump is the table's, and
# bound to triangle's line by its REFSUM, so quad's dump in its place is
# refused (2) before anything is compared, as is gp0-e1's log in place of
# mask-bit's.
set(_jbc_fake ${CMAKE_CURRENT_BINARY_DIR}/refuse/fake)
file(MAKE_DIRECTORY ${_jbc_fake}/tests/conformance ${_jbc_fake}/third_party/ps1-tests/gpu/triangle
  ${_jbc_fake}/third_party/ps1-tests/gpu/mask-bit)
file(COPY_FILE ${_jbc_hw}/quad/vram.png ${_jbc_fake}/third_party/ps1-tests/gpu/triangle/vram.png)
file(COPY_FILE ${_jbc_hw}/gp0-e1/psx.log ${_jbc_fake}/third_party/ps1-tests/gpu/mask-bit/psx.log)
file(COPY_FILE ${_jbc_dir}/fixtures.txt ${_jbc_fake}/tests/conformance/fixtures.txt)
jb_expect_status(conformance_refuse_other_dump 2
  "ERROR [^\n]*triangle/vram.png: FNV-1a 472abf9b9950de2f; the fixture triangle of [^\n]* states c5af46d9b0191449 for the console's dump of gpu/triangle: not that file"
  COMMAND ${_jbc_run} --fixtures ${_jbc_fake}/tests/conformance/fixtures.txt --fixture triangle
    ${_jbc_dir}/triangle.jbc)
jb_expect_status(conformance_refuse_other_log 2
  "ERROR [^\n]*mask-bit/psx.log: FNV-1a 2a6f69193620ba67; the fixture mask-bit of [^\n]* states ccfa6ba800604ade for the console's log of gpu/mask-bit: not that file"
  COMMAND ${_jbc_run} --fixtures ${_jbc_fake}/tests/conformance/fixtures.txt --fixture mask-bit
    ${_jbc_dir}/mask-bit.jbc)
# A fetched program's dump is found beside its list, and bound the same way.
if(NOT JB_CONFORMANCE_FETCHED STREQUAL "")
  set(_jbc_fake2 ${CMAKE_CURRENT_BINARY_DIR}/refuse/fake-fetched)
  file(MAKE_DIRECTORY ${_jbc_fake2}/tests/conformance ${_jbc_fake2}/third_party/ps1-tests/gpu/rectangles)
  file(COPY_FILE ${_jbc_fetched}/tests/conformance/rectangles.jbc ${_jbc_fake2}/tests/conformance/rectangles.jbc)
  file(COPY_FILE ${_jbc_hw}/quad/vram.png ${_jbc_fake2}/third_party/ps1-tests/gpu/rectangles/vram.png)
  jb_expect_status(conformance_refuse_other_dump_fetched 2
    "ERROR [^\n]*rectangles/vram.png: FNV-1a 472abf9b9950de2f; the fixture rectangles of [^\n]* states 9cceb584fba1e0ed"
    COMMAND ${_jbc_run} ${_jbc_rectangles_id} ${_jbc_fake2}/tests/conformance/rectangles.jbc)
endif()
# An exclusion file that is not the set its line states.
file(WRITE ${_jbc_fake}/tests/conformance/lines.exclude "rect 0 0 1024 512 not the documented set\n")
file(WRITE ${_jbc_fake}/tests/conformance/fixtures-lines.txt
  "lines repo dump 1 0 4898 0e558ab30b43cb0a bb8d5f99bf4a0d5d lines.exclude 64 8dac3f054012b465\n")
file(MAKE_DIRECTORY ${_jbc_fake}/third_party/ps1-tests/gpu/lines)
file(COPY_FILE ${_jbc_hw}/lines/vram.png ${_jbc_fake}/third_party/ps1-tests/gpu/lines/vram.png)
jb_expect_status(conformance_refuse_other_exclusion 2
  "lines.exclude: 524288 pixels with checksum c8b763b6a12ce325; the exclusion [^\n]* states for lines is 64 pixels"
  COMMAND ${_jbc_run} --fixtures ${_jbc_fake}/tests/conformance/fixtures-lines.txt --fixture lines
    ${_jbc_dir}/lines.jbc)

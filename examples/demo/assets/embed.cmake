# Writes demo_files.c: every file conversions.txt names, as bytes in the
# program, so that the demo (native and in a browser) loads its assets with
# the library's _mem loaders and reads no file:
#   cmake -DOUT=<demo_files.c> -P embed.cmake
# Each file is a union with a uint32_t, so its bytes are aligned as
# jb_texture_parse needs them (2 bytes).
cmake_minimum_required(VERSION 3.24)

set(_dir "${CMAKE_CURRENT_LIST_DIR}")
file(STRINGS "${_dir}/conversions.txt" _lines REGEX "^[^#]")
set(_c "/* Made by examples/demo/assets/embed.cmake from the files conversions.txt\n")
string(APPEND _c "   names; do not edit. */\n#include \"files.h\"\n\n")
set(_table "")
set(_n 0)
foreach(_line IN LISTS _lines)
  separate_arguments(_parts UNIX_COMMAND "${_line}")
  list(GET _parts 0 _name)
  file(READ "${_dir}/${_name}" _hex HEX)
  string(LENGTH "${_hex}" _len)
  math(EXPR _size "${_len} / 2")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
  # Sixteen bytes a line (CMake's expressions have no {16}).
  string(REPEAT "0x..," 16 _sixteen)
  string(REGEX REPLACE "(${_sixteen})" "\\1\n" _bytes "${_bytes}")
  string(APPEND _c "static const union {\n    uint32_t align;\n    unsigned char bytes[${_size}];\n"
                   "} file${_n} = {.bytes = {\n${_bytes}}};\n\n")
  string(APPEND _table "    {\"${_name}\", file${_n}.bytes, ${_size}},\n")
  math(EXPR _n "${_n} + 1")
endforeach()
string(APPEND _c "const dm_file dm_files[] = {\n${_table}};\nconst int dm_file_count = ${_n};\n")
# No line of 512 characters or more: well inside the 4095 a C99 compiler
# must take (they are 80 here).
string(REPEAT "[^\n]" 512 _long)
string(REGEX MATCH "${_long}" _too_long "${_c}")
if(_too_long)
  message(FATAL_ERROR "embed.cmake: a line of ${OUT} would be 512 characters or more")
endif()
file(WRITE "${OUT}.tmp" "${_c}")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")

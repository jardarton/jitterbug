# tools/jb-tex: PNG to .jbt (docs/tools.md, "`jb-tex`"). Strict C99 against
# <jitterbug.h> and the format's layout (src/api/formats.h).
add_executable(jb-tex jb-tex.c quantise.c)
target_link_libraries(jb-tex PRIVATE jb_tools jb_tools_png)
set_target_properties(jb-tex PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(NOT MSVC)
  target_compile_options(jb-tex PRIVATE -pedantic-errors)
endif()
# stb_image (jb_tools_png).
jb_install_tool(jb-tex stb)

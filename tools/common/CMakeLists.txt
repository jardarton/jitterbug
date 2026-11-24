# What jb-tex and jb-mesh share:
#   jb_tools_png  stb_image, PNG only (png.c), without the project's warnings
#   jb_tools      the flags both tools build with: <jitterbug.h>, the
#                 formats' layout (src/api/formats.h), and under emscripten
#                 what running under Node needs (the real file system, the
#                 exit status, memory to grow into)
add_library(jb_tools_png STATIC png.c)
target_include_directories(jb_tools_png SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/stb)
set_target_properties(jb_tools_png PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(NOT MSVC)
  target_compile_options(jb_tools_png PRIVATE -ffp-contract=off)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(jb_tools_png PRIVATE m)
endif()

add_library(jb_tools INTERFACE)
target_include_directories(jb_tools INTERFACE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(jb_tools INTERFACE jitterbug jb_build_flags)
if(EMSCRIPTEN)
  target_link_options(jb_tools INTERFACE
    -sNODERAWFS=1 -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=1MB)
endif()

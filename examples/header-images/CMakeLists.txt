# examples/header-images: the README's header images (README.md here).
#   header-images         draws docs/images/logo.png (an animated PNG),
#                         beetle.png and banner.png, with the demo's scene
#                         (examples/demo) for the banner
#   header_images_regen   writes them into docs/images again
#                         (cmake --build build --target header_images_regen)
# Strict C99 against <jitterbug.h> and the demo's scene; the PNG encoder is
# the vendored stb_image_write (encode.c), built without the warning flags.

add_library(header_images_encode STATIC encode.c)
target_include_directories(header_images_encode SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/stb)
set_target_properties(header_images_encode PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

add_executable(header-images header-images.c)
target_link_libraries(header-images PRIVATE demo_scene header_images_encode)
jb_demo_c99(header-images)
if(EMSCRIPTEN)
  # Under Node: the real file system, the exit status, and memory to grow
  # into for the demo's 4x VRAM.
  target_link_options(header-images PRIVATE
    -sNODERAWFS=1 -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=1MB)
endif()

set(_hi_images ${PROJECT_SOURCE_DIR}/docs/images)
if(JB_TESTS)
  # The images are what the program draws now, byte for byte (every frame
  # of the logo with them), and the demo page's favicon is the beetle.
  add_test(NAME header_images
    COMMAND ${CMAKE_COMMAND} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/header-images-test
      -DIMAGES=${_hi_images} -DPAGE=${PROJECT_SOURCE_DIR}/examples/demo/web/index.html
      -P ${CMAKE_CURRENT_SOURCE_DIR}/test.cmake --
      ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:header-images>)
endif()

if(NOT EMSCRIPTEN)
  add_custom_target(header_images_regen
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_hi_images}
    COMMAND header-images ${_hi_images}
    COMMENT "Drawing docs/images again" VERBATIM)
endif()

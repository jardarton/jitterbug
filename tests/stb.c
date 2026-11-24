/*
 * The implementation of the vendored stb headers (third_party/stb), PNG
 * only, for the tests. Built without the project's warning flags, as
 * third-party code.
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

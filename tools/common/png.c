/*
 * PNG reading for jb-tex and jb-mesh: the implementation of the vendored
 * stb_image (third_party/stb), PNG only, built without the project's
 * warning flags, as third-party code. Its PNG decoding is integer-only (no
 * gamma), so a PNG decodes to the same pixels everywhere.
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

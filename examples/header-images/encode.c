/*
 * PNG encoding for header-images (encode.h): the implementation of the
 * vendored stb_image_write (third_party/stb), built without the project's
 * warning flags, as third-party code. Its filter choice and zlib
 * compressor are integer code, so the same pixels give the same bytes on
 * every platform.
 */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "encode.h"

unsigned char *hi_png(const unsigned char *pixels, int w, int h, int comp, int *len)
{
    return stbi_write_png_to_mem(pixels, w * comp, w, h, comp, len);
}

void hi_png_free(void *png)
{
    STBIW_FREE(png);
}

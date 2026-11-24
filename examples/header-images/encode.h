/*
 * PNG encoding for header-images: stb_image_write's PNG writer (the
 * vendored third_party/stb), compiled in encode.c without the project's
 * warning flags, as third-party code.
 */
#ifndef HEADER_IMAGES_ENCODE_H
#define HEADER_IMAGES_ENCODE_H

/* A PNG of w x h pixels of comp bytes each (3 RGB, 4 RGBA), rows with no
   padding, in memory: *len bytes, to be freed with hi_png_free; or NULL. */
unsigned char *hi_png(const unsigned char *pixels, int w, int h, int comp, int *len);
void hi_png_free(void *png);

#endif

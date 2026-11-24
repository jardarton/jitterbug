/* A program that uses an installed jitterbug (tests/package/check.cmake):
 * a context, a fill, one pixel read back. Strict C99, and C++ through
 * consumer.cpp. */
#include <jitterbug.h>
#include <stdio.h>

int main(void)
{
    jb_result r = JB_OK;
    jb_context *jb = jb_create(NULL, &r);
    uint16_t pixel = 0;
    if (!jb) {
        printf("jb_create: %s\n", jb_result_string(r));
        return 1;
    }
    if (jb_version() != JB_VERSION) {
        printf("library %s, header %u.%u.%u\n", jb_version_string(), JB_VERSION_MAJOR,
               JB_VERSION_MINOR, JB_VERSION_PATCH);
        jb_destroy(jb);
        return 1;
    }
    if (jb_vram_fill(jb, 0, 0, 16, 1, JB_RGB(255, 0, 0)) != JB_OK ||
        jb_vram_read(jb, 3, 0, 1, 1, &pixel) != JB_OK || pixel != 0x001f) {
        printf("pixel 0x%04x, expected 0x001f: %s\n", (unsigned)pixel, jb_error_string(jb));
        jb_destroy(jb);
        return 1;
    }
    jb_destroy(jb);
    printf("jitterbug %s: ok\n", jb_version_string());
    return 0;
}

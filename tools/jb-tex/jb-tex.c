/*
 * jb-tex: a PNG to a .jbt texture (docs/formats.md) at 4, 8 or 15 bits.
 * docs/tools.md, "How a PNG becomes a texture", says what it does to the
 * pixels; quantise.c does it.
 *
 *   jb-tex [--depth 4|8|15] [--dither] [--repeat] [--frame PNG]... IN.png OUT.jbt
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api/formats.h"
#include "quantise.h"
#include "stb_image.h"

static const char usage[] =
    "usage: jb-tex [options] INPUT.png OUTPUT.jbt\n"
    "  --depth 4|8|15  texel depth: 4-bit (16 colours), 8-bit (256) or 15-bit\n"
    "                  colour; default 8\n"
    "  --dither        diffuse the quantisation error (Floyd-Steinberg)\n"
    "  --repeat        a texture that repeats (sizes powers of two, 8..256)\n"
    "  --frame PNG     another palette for the same texels from PNG, an image\n"
    "                  of the same size (palette animation; 4 and 8 bits);\n"
    "                  may be given more than once, in order\n"
    "Alpha 0 is transparent (0x0000, or palette entry 0); alpha 1..127 sets\n"
    "the semi-transparency bit; 128..255 is opaque.\n";

static int fail(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* Prints "jb-tex: " and the message; returns 1, the exit status. */
static int fail(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "jb-tex: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    return 1;
}

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)(v >> 8 & 0xff);
}

static void put32(unsigned char *p, unsigned long v)
{
    put16(p, (unsigned)(v & 0xffff));
    put16(p + 2, (unsigned)(v >> 16 & 0xffff));
}

static int power_of_two(int n)
{
    return n > 0 && (n & (n - 1)) == 0;
}

enum { MAX_FRAMES = 256 };

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL, *frames[MAX_FRAMES];
    int depth = 8, dither = 0, repeat = 0, nframes = 0, w, h, n, i, entries, palettes;
    unsigned char *rgba = NULL, *frame = NULL, *file = NULL;
    uint8_t *idx = NULL;
    uint16_t *texels = NULL, *pal = NULL;
    int rc = 0;
    size_t texel_bytes, pal_off, size, k;
    FILE *f;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--depth") == 0 && i + 1 < argc) {
            const char *d = argv[++i];
            depth = strcmp(d, "4") == 0 ? 4 : strcmp(d, "8") == 0 ? 8 : strcmp(d, "15") == 0 ? 15 : 0;
            if (depth == 0)
                return fail("--depth %s: not 4, 8 or 15", d);
        } else if (strcmp(argv[i], "--dither") == 0) {
            dither = 1;
        } else if (strcmp(argv[i], "--repeat") == 0) {
            repeat = 1;
        } else if (strcmp(argv[i], "--frame") == 0 && i + 1 < argc) {
            if (nframes == MAX_FRAMES)
                return fail("more than %d frames", MAX_FRAMES);
            frames[nframes++] = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fputs(usage, stdout);
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fputs(usage, stderr);
            return fail("unknown option %s", argv[i]);
        } else if (in == NULL) {
            in = argv[i];
        } else if (out == NULL) {
            out = argv[i];
        } else {
            fputs(usage, stderr);
            return 1;
        }
    }
    if (in == NULL || out == NULL) {
        fputs(usage, stderr);
        return 1;
    }
    if (nframes > 0 && depth == 15)
        return fail("--frame needs a palette: depth 4 or 8, not 15");

    /* From here every exit frees what was taken (done). */
#define FAIL(...)                                                                             \
    do {                                                                                      \
        rc = fail(__VA_ARGS__);                                                               \
        goto done;                                                                            \
    } while (0)
    rgba = stbi_load(in, &w, &h, &n, 4);
    if (rgba == NULL)
        FAIL("%s: cannot read it as a PNG (%s)", in, stbi_failure_reason());
    if (w < 1 || w > 256 || h < 1 || h > 256)
        FAIL("%s: %d x %d pixels; a texture is 1..256 each way", in, w, h);
    if (repeat && (!power_of_two(w) || !power_of_two(h) || w < 8 || h < 8))
        FAIL("%s: --repeat needs power-of-two sizes 8..256, not %d x %d", in, w, h);
    entries = depth == 4 ? 16 : depth == 8 ? 256 : 0;
    palettes = entries != 0 ? 1 + nframes : 0;
    texel_bytes = (size_t)w * h * (depth == 15 ? 2 : 1);
    pal_off = (JBI_JBT_HEADER + texel_bytes + 3) / 4 * 4;
    size = entries != 0 ? pal_off + (size_t)palettes * entries * 2 : JBI_JBT_HEADER + texel_bytes;
    file = calloc(size, 1);
    idx = calloc((size_t)w * h, 1);
    texels = calloc((size_t)w * h, sizeof *texels);
    pal = calloc((size_t)(palettes > 0 ? palettes : 1) * 256, sizeof *pal);
    if (file == NULL || idx == NULL || texels == NULL || pal == NULL ||
        jbq_convert(rgba, w, h, depth, dither, idx, pal, texels) != 0)
        FAIL("out of memory");
    for (i = 0; i < nframes; i++) {
        int fw, fh, fn;
        frame = stbi_load(frames[i], &fw, &fh, &fn, 4);
        if (frame == NULL)
            FAIL("%s: cannot read it as a PNG (%s)", frames[i], stbi_failure_reason());
        if (fw != w || fh != h)
            FAIL("%s: %d x %d; a frame must be the input's size, %d x %d", frames[i], fw, fh, w,
                 h);
        jbq_frame_palette(frame, w, h, idx, entries, pal, pal + (size_t)(i + 1) * entries);
        stbi_image_free(frame);
        frame = NULL;
    }

    memcpy(file + JBI_JBT_O_MAGIC, JBI_JBT_MAGIC, 4);
    put32(file + JBI_JBT_O_VERSION, JBI_JBT_VERSION);
    put32(file + JBI_JBT_O_SIZE, (unsigned long)size);
    put16(file + JBI_JBT_O_WIDTH, (unsigned)w);
    put16(file + JBI_JBT_O_HEIGHT, (unsigned)h);
    file[JBI_JBT_O_DEPTH] = (unsigned char)depth;
    file[JBI_JBT_O_FLAGS] = repeat ? JBI_JBT_REPEAT : 0;
    put16(file + JBI_JBT_O_PALETTES, (unsigned)palettes);
    put32(file + JBI_JBT_O_TEXELS, JBI_JBT_HEADER);
    put32(file + JBI_JBT_O_PALETTE, entries != 0 ? (unsigned long)pal_off : 0);
    for (k = 0; k < (size_t)w * h; k++) {
        if (depth == 15)
            put16(file + JBI_JBT_HEADER + 2 * k, texels[k]);
        else
            file[JBI_JBT_HEADER + k] = idx[k];
    }
    for (k = 0; entries != 0 && k < (size_t)palettes * entries; k++)
        put16(file + pal_off + 2 * k, pal[k]);

    f = fopen(out, "wb");
    if (f == NULL)
        FAIL("%s: cannot open it for writing", out);
    if (fwrite(file, 1, size, f) != size) {
        fclose(f);
        FAIL("%s: cannot write it", out);
    }
    if (fclose(f) != 0)
        FAIL("%s: cannot write it", out);
#undef FAIL
done:
    if (rgba != NULL)
        stbi_image_free(rgba);
    if (frame != NULL)
        stbi_image_free(frame);
    free(file);
    free(idx);
    free(texels);
    free(pal);
    return rc;
}

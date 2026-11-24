/*
 * sprite_scene: a small original 2D scene of sprites and text drawn with
 * the friendly level alone (jb_texture_create, jb_palette_write,
 * jb_draw_sprite, jb_draw_text, jb_draw_text_blended, layers), hashed
 * frame by frame against hashes stored in the repository
 * (sprite_scene.txt), so every build the check makes (both compilers, the
 * sanitizers, -O0, x86-64-v3, arm64, wasm32) must draw it identically.
 *
 * The scene moves by frame number in integer arithmetic only: a repeat
 * tile scrolled across the background as one sprite whose source runs on
 * past the texture; a palette cycled every frame (jb_palette_write); 1:1
 * sprites from an atlas, flipped, tinted and blending, sliding off every
 * edge; scaled sprites zooming in and out, one thousands of pixels across;
 * opaque and blended text, several lines, a counter, text running off the
 * frame. Runs: 1x, 2x, 3x, dithering off (the scaled, modulated sprites
 * dither), widescreen switched half way, and a single-buffered 640 x 480
 * display, at 1 and 4 threads.
 *
 *   test_sprite_scene sprite_scene.txt          compare
 *   test_sprite_scene sprite_scene.txt --print  print what the runs give
 *   test_sprite_scene sprite_scene.txt --ppm RUN FRAME PATH
 *                                       write that frame as a PPM, to look at
 */
#include "api_test.h"

typedef struct {
    const char *name;
    int scale, w, h, widescreen, dither_off, frames;
} run_def;

static const run_def runs[] = {
    {"base", 1, 320, 240, 0, 0, 32},    {"scale2", 2, 320, 240, 0, 0, 12},
    {"scale3", 3, 320, 240, 0, 0, 6},   {"nodither", 1, 320, 240, 0, 1, 12},
    {"wide", 1, 320, 240, 1, 0, 12},    {"big", 1, 640, 480, 0, 0, 6},
};

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static jb_texture tile, atlas, photo;

/* --ppm: the run and frame to write, and where. */
static const char *ppm_run, *ppm_path;
static int ppm_frame = -1;

static void write_ppm(const uint8_t *px, int w, int h)
{
    FILE *fp = fopen(ppm_path, "wb");
    int i;

    if (fp == NULL) {
        printf("cannot write %s\n", ppm_path);
        return;
    }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; ++i)
        fwrite(px + 4 * i, 1, 3, fp);
    fclose(fp);
}
static uint16_t tile_pal[16];

static void textures(jb_context *jb)
{
    static uint8_t t4[32 * 32], t8[48 * 40];
    static uint16_t t15[24 * 24], pal8[256];
    jb_texture_desc d;
    int x, y;

    /* A 32 x 32 tile of rings, repeating. */
    for (y = 0; y < 32; ++y)
        for (x = 0; x < 32; ++x) {
            int dx = x - 16, dy = y - 16;
            t4[y * 32 + x] = (uint8_t)(1 + (dx * dx + dy * dy) / 18 % 15);
        }
    for (x = 0; x < 16; ++x)
        tile_pal[x] = JB_RGB15(4 + x, 2 + x / 2, 12 + x);
    memset(&d, 0, sizeof d);
    d.width = d.height = 32;
    d.depth = JB_TEX_4BIT;
    d.texels = t4;
    d.palette = tile_pal;
    d.repeat = 1;
    check("tile", jb_texture_create(jb, &d, &tile), JB_OK);
    /* An atlas of four 24 x 20 shapes, 0 transparent around them, some
       entries semi-transparent. */
    for (y = 0; y < 40; ++y)
        for (x = 0; x < 48; ++x) {
            int cx = x % 24 - 12, cy = y % 20 - 10, cell = (y / 20) * 2 + x / 24, in;
            in = cell == 0   ? cx * cx + cy * cy < 80
                 : cell == 1 ? (cx < 0 ? -cx : cx) + (cy < 0 ? -cy : cy) < 10
                 : cell == 2 ? cy > -8 && cy < 8 && (cx < 0 ? -cx : cx) < 10 - (cy + 8) / 2
                             : (cx * cx) / 3 + cy * cy < 60 && (cx + cy) % 4 != 0;
            t8[y * 48 + x] = (uint8_t)(in ? 1 + (cell * 60 + x * 3 + y * 5) % 255 : 0);
        }
    for (x = 0; x < 256; ++x)
        pal8[x] = (uint16_t)(JB_RGB15(x % 32, (x * 7) % 32, 31 - x % 32) | (x % 9 == 0 ? 0x8000 : 0));
    pal8[0] = 0;
    memset(&d, 0, sizeof d);
    d.width = 48;
    d.height = 40;
    d.depth = JB_TEX_8BIT;
    d.texels = t8;
    d.palette = pal8;
    check("atlas", jb_texture_create(jb, &d, &atlas), JB_OK);
    /* A 15-bit gradient. */
    for (y = 0; y < 24; ++y)
        for (x = 0; x < 24; ++x)
            t15[y * 24 + x] = (uint16_t)(JB_RGB15(x + 4, y + 4, 31 - x) | ((x + y) % 7 == 0 ? 0x8000 : 0));
    memset(&d, 0, sizeof d);
    d.width = d.height = 24;
    d.depth = JB_TEX_15BIT;
    d.texels = t15;
    check("photo", jb_texture_create(jb, &d, &photo), JB_OK);
}

static void scene(jb_context *jb, int f, int w, int h)
{
    jb_sprite s;
    uint16_t pal[16];
    char line[64];
    int i;

    check("begin", jb_begin_frame(jb, JB_RGB(10, 12, 30)), JB_OK);
    /* The palette cycled. */
    for (i = 0; i < 16; ++i)
        pal[i] = tile_pal[(i + f) % 15 + 1];
    pal[0] = tile_pal[0];
    check("palette", jb_palette_write(jb, &tile, 0, 16, pal), JB_OK);

    /* Background: the tile scrolled, as one sprite past the texture's
       edge, twice to cover the frame; and a zooming photo. */
    jb_set_layer(jb, JB_LAYER_BACKGROUND);
    memset(&s, 0, sizeof s);
    s.texture = &tile;
    s.u = (f * 3) % 32;
    s.v = (f * 2) % 32;
    s.src_w = 200;
    s.src_h = 200;
    s.x = 0;
    s.y = 0;
    s.tint = JB_RGB(90, 90, 110);
    check("tile", jb_draw_sprite(jb, &s), JB_OK);
    s.x = 200;
    s.src_w = 224;
    check("tile 2", jb_draw_sprite(jb, &s), JB_OK);
    s.x = 0, s.y = 200, s.src_w = 200, s.src_h = 200;
    check("tile 3", jb_draw_sprite(jb, &s), JB_OK);
    s.x = 200, s.src_w = 224;
    check("tile 4", jb_draw_sprite(jb, &s), JB_OK);
    memset(&s, 0, sizeof s);
    s.texture = &photo;
    s.w = 24 + f * 9;
    s.h = 24 + f * 6;
    s.x = w / 2 - s.w / 2;
    s.y = h / 2 - s.h / 2;
    s.blend = JB_BLEND_HALF;
    s.flip_y = f & 1;
    check("zoom", jb_draw_sprite(jb, &s), JB_OK);

    /* Overlay: atlas sprites off every edge. */
    jb_set_layer(jb, JB_LAYER_OVERLAY);
    for (i = 0; i < 14; ++i) {
        memset(&s, 0, sizeof s);
        s.texture = &atlas;
        s.u = (i % 2) * 24;
        s.v = (i / 2 % 2) * 20;
        s.src_w = 24; /* the cell, 1:1 */
        s.src_h = 20;
        s.x = (i * 47 + f * (2 + i)) % (w + 48) - 24;
        s.y = (i * 31 + f * (3 + i % 4)) % (h + 40) - 20;
        s.flip_x = i & 1;
        s.flip_y = (i >> 2) & 1;
        s.tint = i % 3 == 0 ? JB_RGB(255, 160, 80) : 0;
        s.blend = i % 4 == 1 ? JB_BLEND_ADD : i % 4 == 2 ? JB_BLEND_SUB : JB_BLEND_NONE;
        s.raw = i % 5 == 4;
        if (i % 6 == 5) { /* some scaled, from small to large */
            s.w = 8 + (f * 5 + i * 11) % 90;
            s.h = 6 + (f * 3 + i * 7) % 70;
        }
        check("atlas sprite", jb_draw_sprite(jb, &s), JB_OK);
    }
    /* A huge one sweeping past, dark, through its transparent holes: the
       ellipse cell, a texel some 100 pixels across, split into pieces. */
    memset(&s, 0, sizeof s);
    s.texture = &atlas;
    s.u = 24;
    s.v = 20;
    s.src_w = 24;
    s.src_h = 20;
    s.w = 2600;
    s.h = 1800;
    s.x = -2500 + f * 60;
    s.y = -1000 + f * 20;
    s.tint = JB_RGB(40, 40, 60);
    s.blend = JB_BLEND_QUARTER;
    check("huge", jb_draw_sprite(jb, &s), JB_OK);

    /* Text. */
    check("title", jb_draw_text(jb, 8, 8, JB_RGB(255, 240, 200), "jitterbug sprites\nand text"),
          JB_OK);
    snprintf(line, sizeof line, "frame %03d", f);
    check("counter", jb_draw_text(jb, w - 80, h - 12, JB_RGB(120, 255, 120), line), JB_OK);
    check("glow", jb_draw_text_blended(jb, 20 + f, h / 2, JB_RGB(90, 60, 200), JB_BLEND_ADD,
                                       "glowing\n  text"),
          JB_OK);
    check("off the edge", jb_draw_text(jb, w - f * 9, 40, JB_RGB(255, 80, 80),
                                       "running off the right edge ~"),
          JB_OK);
    jb_set_layer(jb, JB_LAYER_BACKGROUND);
    check("under", jb_draw_text(jb, -4, h - 30, JB_RGB(40, 40, 40), "BEHIND everything"),
          JB_OK);
    check("end", jb_end_frame(jb), JB_OK);
}

static uint64_t play(const run_def *d, int threads)
{
    jb_config c;
    jb_context *jb;
    uint64_t hash = 1469598103934665603ull;
    uint16_t *vram;
    int f, fw, fh;

    memset(&c, 0, sizeof c);
    c.scale = d->scale;
    c.width = d->w;
    c.height = d->h;
    c.widescreen = d->widescreen;
    c.threads = threads;
    jb = must_create(&c);
    jb_set_dither(jb, !d->dither_off);
    textures(jb);
    for (f = 0; f < d->frames; ++f) {
        const uint8_t *px;

        if (d->widescreen && f == d->frames / 2)
            check("widescreen off", jb_set_widescreen(jb, 0), JB_OK);
        /* the size this frame is drawn at, not the one shown: on the
           frame widescreen goes off they differ */
        check("frame size", jb_frame_size(jb, &fw, &fh), JB_OK);
        scene(jb, f, fw, fh);
        px = jb_frame(jb, &fw, &fh);
        hash = fnv(hash, px, (size_t)fw * (size_t)fh * 4);
        if (ppm_run != NULL && strcmp(ppm_run, d->name) == 0 && f == ppm_frame)
            write_ppm(px, fw, fh);
    }
    vram = read_vram(jb);
    hash = fnv(hash, (const uint8_t *)vram, (size_t)VRAM_PIXELS * 2);
    free(vram);
    jb_destroy(jb);
    return hash;
}

int main(int argc, char **argv)
{
    FILE *fp;
    char line[256];
    int print = argc > 2 && strcmp(argv[2], "--print") == 0;
    int seen[sizeof runs / sizeof runs[0]] = {0};
    size_t i;

    if (argc < 2) {
        printf("usage: test_sprite_scene sprite_scene.txt [--print]\n");
        return 2;
    }
    if (argc > 5 && strcmp(argv[2], "--ppm") == 0) {
        ppm_run = argv[3];
        ppm_frame = atoi(argv[4]);
        ppm_path = argv[5];
        for (i = 0; i < sizeof runs / sizeof runs[0]; ++i)
            if (strcmp(runs[i].name, ppm_run) == 0)
                play(&runs[i], 1);
        return 0;
    }
    if (print) {
        for (i = 0; i < sizeof runs / sizeof runs[0]; ++i)
            printf("%s %016llx\n", runs[i].name, (unsigned long long)play(&runs[i], 1));
        return 0;
    }
    fp = fopen(argv[1], "r");
    if (fp == NULL) {
        printf("cannot open %s\n", argv[1]);
        return 2;
    }
    /* Every run listed exactly once: a line that names no run, names one
       again, or cannot be read fails, and so does a run with no line (a
       duplicate cannot stand in for a missing one). */
    while (fgets(line, sizeof line, fp) != NULL) {
        char name[64];
        unsigned long long want;

        if (line[0] == '#' || strspn(line, " \t\r\n") == strlen(line))
            continue;
        if (sscanf(line, "%63s %llx", name, &want) != 2) {
            printf("  cannot read: %s", line);
            check("every line readable", 0, 1);
            continue;
        }
        for (i = 0; i < sizeof runs / sizeof runs[0] && strcmp(runs[i].name, name) != 0; ++i)
            ;
        if (i == sizeof runs / sizeof runs[0]) {
            printf("  no run is called %s\n", name);
            check("every line names a run", 0, 1);
            continue;
        }
        if (seen[i]++) {
            printf("  %s is listed twice\n", name);
            check("no run listed twice", 0, 1);
            continue;
        }
        {
            char what[96];

            snprintf(what, sizeof what, "%s, 1 thread", name);
            check(what, (long long)play(&runs[i], 1), (long long)want);
            snprintf(what, sizeof what, "%s, 4 threads", name);
            check(what, (long long)play(&runs[i], 4), (long long)want);
        }
    }
    fclose(fp);
    for (i = 0; i < sizeof runs / sizeof runs[0]; ++i)
        if (!seen[i]) {
            printf("  no stored hash for %s\n", runs[i].name);
            check("every run has a stored hash", 0, 1);
        }
    return finish("sprite_scene");
}

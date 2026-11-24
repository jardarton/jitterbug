/*
 * friendly_scene: a small original 2D scene drawn with the friendly level
 * alone (jb_begin_frame, jb_draw_*, jb_set_layer, jb_end_frame), hashed
 * frame by frame against hashes stored in the repository
 * (friendly_scene.txt), so every build the check makes (both compilers,
 * the sanitizers, -O0, x86-64-v3, arm64, wasm32) must draw it identically.
 *
 * The scene moves by frame number in integer arithmetic only. It has
 * what the 2D path clips: a Gouraud sky in the background layer, a huge
 * Gouraud triangle turning around the frame with corners thousands of
 * pixels outside it, a textured quad reaching past -1024, sprites (4-bit, CLUT, some flipped, some blending)
 * sliding off every edge and a wide one beyond the console's -1024, lines
 * and a polyline running far past the frame, a half-blended quad, and the
 * layers in both orders. Runs: 1x, 2x, 3x, 2x with precise vertices and
 * perspective-correct textures, dithering off, widescreen switched half
 * way, and a single-buffered 640 x 480 display, at 1 and 4 threads.
 *
 *   test_friendly_scene friendly_scene.txt          compare
 *   test_friendly_scene friendly_scene.txt --print  print what the runs give
 *   test_friendly_scene friendly_scene.txt --ppm RUN FRAME PATH
 *                                         write that frame as a PPM, to look at
 */
#include "api_test.h"

typedef struct {
    const char *name;
    int scale, w, h, widescreen, precise, dither_off, frames;
} run_def;

static const run_def runs[] = {
    {"base", 1, 320, 240, 0, 0, 0, 48},
    {"scale2", 2, 320, 240, 0, 0, 0, 16},
    {"scale3", 3, 320, 240, 0, 0, 0, 8},
    {"precise2", 2, 320, 240, 0, 1, 0, 16},
    {"nodither", 1, 320, 240, 0, 0, 1, 16},
    {"wide", 1, 320, 240, 1, 0, 0, 16},
    {"big", 1, 640, 480, 0, 0, 0, 8},
};

/* sin(k / 16 of a turn) x 1024. */
static const int sine16[16] = {0, 392, 724, 946, 1024, 946, 724, 392,
                               0, -392, -724, -946, -1024, -946, -724, -392};

static int isin(int k)
{
    return sine16[((k % 16) + 16) % 16];
}

static int icos(int k)
{
    return isin(k + 4);
}

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static jb_texture tex;

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

static void upload(jb_context *jb)
{
    uint16_t texels[4 * 16], pal[16];
    int x, y;

    /* A 16 x 16 4-bit tile: a ring of indexes, 0 (transparent) outside. */
    for (y = 0; y < 16; ++y)
        for (x = 0; x < 4; ++x) {
            uint16_t w = 0;
            int k;

            for (k = 0; k < 4; ++k) {
                int px = x * 4 + k, dx = px - 8, dy = y - 8, d = dx * dx + dy * dy;
                int idx = d < 64 ? 1 + (d / 5) % 15 : 0;

                w |= (uint16_t)(idx << (4 * k));
            }
            texels[y * 4 + x] = w;
        }
    for (x = 0; x < 16; ++x)
        pal[x] = (uint16_t)(JB_RGB15(31 - x * 2, x * 2, 10 + x) | (x > 10 ? JB_TEXEL_SEMI : 0));
    pal[0] = JB_TEXEL_TRANSPARENT;
    jb_vram_load(jb, 640, 0, 4, 16, texels);
    jb_vram_load(jb, 0, 490, 16, 1, pal);
    memset(&tex, 0, sizeof tex);
    tex.page_x = 640;
    tex.depth = JB_TEX_4BIT;
    tex.clut_x = 0;
    tex.clut_y = 490;
}

static void vertex(jb_vertex *v, int x, int y, uint32_t c, int u, int vv)
{
    v->x = x;
    v->y = y;
    v->color = c;
    v->u = u;
    v->v = vv;
}

static void scene(jb_context *jb, int f, int w, int h)
{
    jb_poly p;
    jb_rect r;
    jb_line l;
    jb_vertex pv[6];
    jb_polyline pl;
    int i, cx = w / 2, cy = h / 2;

    check("begin", jb_begin_frame(jb, JB_RGB(8, 8, 24)), JB_OK);
    /* Background: the sky, and the huge turning triangle. */
    jb_set_layer(jb, JB_LAYER_BACKGROUND);
    memset(&p, 0, sizeof p);
    p.vertices = 4;
    p.gouraud = 1;
    vertex(&p.v[0], 0, 0, JB_RGB(20, 30, 90), 0, 0);
    vertex(&p.v[1], w, 0, JB_RGB(40, 20, 100), 0, 0);
    vertex(&p.v[2], 0, h, JB_RGB(120, 60, 40), 0, 0);
    vertex(&p.v[3], w, h, JB_RGB(160, 90, 30), 0, 0);
    check("sky", jb_draw_poly(jb, &p), JB_OK);
    memset(&p, 0, sizeof p);
    p.vertices = 3;
    p.gouraud = 1;
    p.blend = JB_BLEND_HALF;
    for (i = 0; i < 3; ++i)
        vertex(&p.v[i], cx + icos(f + i * 5) * 3, cy + isin(f + i * 5) * 3,
               JB_RGB(i == 0 ? 255 : 0, i == 1 ? 255 : 0, i == 2 ? 255 : 0), 0, 0);
    check("huge triangle", jb_draw_poly(jb, &p), JB_OK);
    /* A textured quad past the left edge, beyond -1024. */
    memset(&p, 0, sizeof p);
    p.vertices = 4;
    p.texture = &tex;
    vertex(&p.v[0], -1200 + f * 20, 20, JB_NEUTRAL, 0, 0);
    vertex(&p.v[1], 60 + f, 30, JB_NEUTRAL, 15, 0);
    vertex(&p.v[2], -1200 + f * 20, 90, JB_NEUTRAL, 0, 15);
    vertex(&p.v[3], 70 + f, 80, JB_NEUTRAL, 15, 15);
    check("long textured quad", jb_draw_poly(jb, &p), JB_OK);
    /* Overlay: sprites off every edge. */
    jb_set_layer(jb, JB_LAYER_OVERLAY);
    for (i = 0; i < 12; ++i) {
        memset(&r, 0, sizeof r);
        r.x = (i * 53 + f * (3 + i)) % (w + 64) - 32;
        r.y = (i * 37 + f * (5 - i % 3) * 2) % (h + 64) - 32;
        if (i % 4 == 3)
            r.y = h - 8 - (f % 20);
        r.w = 16;
        r.h = 16;
        r.texture = &tex;
        r.color = i % 3 ? JB_NEUTRAL : JB_RGB(200, 100, 255);
        r.blend = i % 5 == 0 ? JB_BLEND_ADD : JB_BLEND_NONE;
        r.flip_x = i & 1;
        r.flip_y = (i >> 1) & 1;
        /* Mirrored, the first pixel takes texel u + 1 in x, v in y. */
        r.u = r.flip_x ? 14 : 0;
        r.v = r.flip_y ? 15 : 0;
        check("sprite", jb_draw_rect(jb, &r), JB_OK);
    }
    /* A wide strip beyond -1024. */
    memset(&r, 0, sizeof r);
    r.x = -1500 + f * 37;
    r.y = h - 20;
    r.w = 1600;
    r.h = 6;
    r.texture = &tex;
    r.color = JB_NEUTRAL;
    r.u = (f * 3) & 255;
    r.v = 5; /* rows through the ring; texels past u 15 are transparent */
    check("strip", jb_draw_rect(jb, &r), JB_OK);
    /* Lines far past the frame, and a polyline. */
    memset(&l, 0, sizeof l);
    l.gouraud = 1;
    l.blend = JB_BLEND_ADD;
    vertex(&l.v[0], -4000 + f * 10, -300, JB_RGB(255, 0, 0), 0, 0);
    vertex(&l.v[1], 5000, h + 2000 - f * 30, JB_RGB(0, 255, 0), 0, 0);
    check("line", jb_draw_line(jb, &l), JB_OK);
    for (i = 0; i < 6; ++i)
        vertex(&pv[i], i * w / 5 - 40 + (i == 5 ? 3000 : 0), cy + isin(f + i * 3) / 12,
               JB_RGB(255, 255 - 40 * i, 40 * i), 0, 0);
    pl.gouraud = 1;
    pl.blend = JB_BLEND_NONE;
    pl.count = 6;
    pl.v = pv;
    check("polyline", jb_draw_polyline(jb, &pl), JB_OK);
    /* A half-blended quad in the background, drawn after the overlay's
       calls, so under them. */
    jb_set_layer(jb, JB_LAYER_BACKGROUND);
    memset(&p, 0, sizeof p);
    p.vertices = 4;
    p.blend = JB_BLEND_HALF;
    vertex(&p.v[0], cx - 60 + f % 7, cy - 40, JB_RGB(255, 255, 255), 0, 0);
    vertex(&p.v[1], cx + 60, cy - 50 + f % 5, 0, 0, 0);
    vertex(&p.v[2], cx - 70, cy + 40, 0, 0, 0);
    vertex(&p.v[3], cx + 50 + f % 3, cy + 45, 0, 0, 0);
    check("quad", jb_draw_poly(jb, &p), JB_OK);
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
    c.precise_vertices = c.perspective_correct = d->precise;
    c.threads = threads;
    jb = must_create(&c);
    if (d->dither_off)
        jb_set_dither(jb, 0);
    else
        jb_set_dither(jb, 1);
    upload(jb);
    for (f = 0; f < d->frames; ++f) {
        const uint8_t *px;

        if (d->widescreen && f == d->frames / 2)
            check("widescreen off", jb_set_widescreen(jb, 0), JB_OK);
        jb_frame(jb, &fw, &fh);
        scene(jb, f, fw / d->scale, fh / d->scale);
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
        printf("usage: test_friendly_scene friendly_scene.txt [--print]\n");
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
    return finish("friendly_scene");
}

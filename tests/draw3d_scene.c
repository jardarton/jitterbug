/*
 * draw3d_scene: a small original 3D scene drawn with the friendly level
 * alone (jb_set_camera, the lights and fog, jb_draw_mesh,
 * jb_draw_billboard, jb_draw_line3d), hashed frame by frame against hashes
 * stored in the repository (draw3d_scene.txt), so every build the check
 * makes (both compilers, the sanitizers, -O0, x86-64-v3, arm64, wasm32)
 * must draw it identically, the float path included.
 *
 * The camera walks forward over a floor of repeat-textured quads that
 * reaches behind it (the near plane cuts it every frame) towards a lit,
 * textured cube that turns and that the camera passes through at the end;
 * a ring of 3D lines, billboards one texel a pixel and scaled, fog. The
 * scene moves by frame number; the float maths it uses is the library's
 * own (jb_sinf, jb_mat4_*), so it is the same everywhere.
 *
 *   test_draw3d_scene draw3d_scene.txt          compare
 *   test_draw3d_scene draw3d_scene.txt --print  print what the runs give
 *   test_draw3d_scene draw3d_scene.txt --ppm RUN FRAME PATH
 *                                         write that frame as a PPM, to look at
 */
#include "api_test.h"

typedef struct {
    const char *name;
    int scale, precise, perspective, geometry, frames;
} run_def;

static const run_def runs[] = {
    {"gte1", 1, 0, 0, JB_GEOMETRY_GTE, 20},   {"gte2", 2, 0, 0, JB_GEOMETRY_GTE, 10},
    {"gte2precise", 2, 1, 0, JB_GEOMETRY_GTE, 10},
    {"gte1perspective", 1, 0, 1, JB_GEOMETRY_GTE, 10},
    {"gte2both", 2, 1, 1, JB_GEOMETRY_GTE, 10}, {"float1", 1, 0, 0, JB_GEOMETRY_FLOAT, 20},
    {"float2both", 2, 1, 1, JB_GEOMETRY_FLOAT, 10},
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

enum { GRID = 8, CELL = 128 };

typedef struct scene {
    jb_texture checker, crate, star;
    jb_gte_vec fpos[(GRID + 1) * (GRID + 1)];
    uint8_t fuv[2 * (GRID + 1) * (GRID + 1)];
    jb_mesh_face ffaces[GRID * GRID];
    jb_material fmat;
    jb_mesh floor;
    jb_gte_vec cpos[24], cnrm[24];
    uint8_t cuv[48];
    jb_mesh_face cfaces[6];
    jb_material cmat[2];
    jb_mesh cube;
} scene;

static scene sc;

static void textures(jb_context *jb)
{
    uint8_t checker[32 * 32], star[16 * 16];
    uint16_t crate[32 * 32], pal[16], spal[16];
    jb_texture_desc d;
    int x, y;

    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++) {
            checker[y * 32 + x] = (uint8_t)(((x / 8 + y / 8) & 1) ? 1 + (x + y) % 3 : 4 + x % 2);
            crate[y * 32 + x] =
                (uint16_t)(x < 2 || y < 2 || x > 29 || y > 29 || x == y || x == 31 - y
                               ? JB_RGB15(25, 18, 6)
                               : JB_RGB15(14 + (x * 3 + y) % 5, 9, 3));
        }
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int dx = x - 8, dy = y - 8, dd = dx * dx + dy * dy;

            star[y * 16 + x] = (uint8_t)(dd < 50 ? 1 + dd / 5 : 0);
        }
    for (x = 0; x < 16; x++) {
        pal[x] = (uint16_t)(JB_RGB15(6 + x * 3, 10 + x * 2, 20 - x) | 0x8000u);
        spal[x] = (uint16_t)(JB_RGB15(31, 31 - x * 2, 10 + x) | (x > 6 ? JB_TEXEL_SEMI : 0));
    }
    spal[0] = JB_TEXEL_TRANSPARENT;
    memset(&d, 0, sizeof d);
    d.width = d.height = 32;
    d.depth = JB_TEX_4BIT;
    d.texels = checker;
    d.palette = pal;
    d.repeat = 1;
    check("checker", jb_texture_create(jb, &d, &sc.checker), JB_OK);
    d.depth = JB_TEX_15BIT;
    d.texels = crate;
    d.palette = NULL;
    d.repeat = 0;
    check("crate", jb_texture_create(jb, &d, &sc.crate), JB_OK);
    d.width = d.height = 16;
    d.depth = JB_TEX_4BIT;
    d.texels = star;
    d.palette = spal;
    check("star", jb_texture_create(jb, &d, &sc.star), JB_OK);
}

static void meshes(void)
{
    static const int corner[6][4][3] = {
        {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
        {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},
        {{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}},
        {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},
        {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},
        {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},
    };
    static const int normal[6][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0},
                                     {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    static const int uv[4][2] = {{0, 31}, {31, 31}, {31, 0}, {0, 0}};
    int i, j, f, k, n = 0;

    for (j = 0; j <= GRID; j++)
        for (i = 0; i <= GRID; i++) {
            sc.fpos[n].x = (int16_t)((i - GRID / 2) * CELL);
            sc.fpos[n].y = 0;
            sc.fpos[n].z = (int16_t)((j - GRID / 2) * CELL);
            sc.fuv[2 * n] = (uint8_t)(i * 32);
            sc.fuv[2 * n + 1] = (uint8_t)(j * 32);
            n++;
        }
    for (j = 0; j < GRID; j++)
        for (i = 0; i < GRID; i++) {
            jb_mesh_face *q = &sc.ffaces[j * GRID + i];
            int a = j * (GRID + 1) + i;

            q->vertices = 4;
            q->v[0] = (uint16_t)(a + GRID + 1);
            q->v[1] = (uint16_t)(a + GRID + 2);
            q->v[2] = (uint16_t)(a + 1);
            q->v[3] = (uint16_t)a;
        }
    sc.fmat.texture = &sc.checker;
    sc.floor.vertex_count = n;
    sc.floor.positions = sc.fpos;
    sc.floor.uv = sc.fuv;
    sc.floor.face_count = GRID * GRID;
    sc.floor.faces = sc.ffaces;
    sc.floor.material_count = 1;
    sc.floor.materials = &sc.fmat;

    for (f = 0; f < 6; f++) {
        for (k = 0; k < 4; k++) {
            int v = f * 4 + k;

            sc.cpos[v].x = (int16_t)(corner[f][k][0] * 64);
            sc.cpos[v].y = (int16_t)(corner[f][k][1] * 64);
            sc.cpos[v].z = (int16_t)(corner[f][k][2] * 64);
            sc.cnrm[v].x = (int16_t)(normal[f][0] * 4096);
            sc.cnrm[v].y = (int16_t)(normal[f][1] * 4096);
            sc.cnrm[v].z = (int16_t)(normal[f][2] * 4096);
            sc.cuv[2 * v] = (uint8_t)uv[k][0];
            sc.cuv[2 * v + 1] = (uint8_t)uv[k][1];
            sc.cfaces[f].v[k] = (uint16_t)v;
        }
        sc.cfaces[f].vertices = 4;
        sc.cfaces[f].material = (uint16_t)(f == 4);
    }
    sc.cmat[0].texture = &sc.crate;
    sc.cmat[1].color = JB_RGB(120, 140, 200);
    sc.cmat[1].blend = JB_BLEND_HALF;
    sc.cube.vertex_count = 24;
    sc.cube.positions = sc.cpos;
    sc.cube.normals = sc.cnrm;
    sc.cube.uv = sc.cuv;
    sc.cube.face_count = 6;
    sc.cube.faces = sc.cfaces;
    sc.cube.material_count = 2;
    sc.cube.materials = sc.cmat;
}

static void draw(jb_context *jb, int f)
{
    jb_camera cam;
    jb_light l;
    jb_mat4 model;
    jb_vec3 t, r, s;
    jb_billboard b;
    int i;

    memset(&cam, 0, sizeof cam);
    cam.eye.x = 20.0f * jb_sinf(0.3f * (float)f);
    cam.eye.y = 70.0f;
    cam.eye.z = 420.0f - 24.0f * (float)f;
    cam.target.x = 0.0f;
    cam.target.y = 40.0f;
    cam.target.z = cam.eye.z - 300.0f;
    cam.fov_y = 70.0f;
    cam.z_near = 24.0f;
    cam.z_far = 2400.0f;
    check("camera", jb_set_camera(jb, &cam), JB_OK);
    memset(&l, 0, sizeof l);
    l.direction.x = -0.5f;
    l.direction.y = -1.0f;
    l.direction.z = -0.3f;
    l.color = JB_RGB(255, 240, 210);
    jb_set_light(jb, 0, &l);
    jb_set_ambient(jb, JB_RGB(70, 70, 90));
    jb_set_fog(jb, JB_RGB(40, 50, 80), 400.0f, 1800.0f);

    check("begin", jb_begin_frame(jb, JB_RGB(40, 50, 80)), JB_OK);
    check("floor", jb_draw_mesh(jb, &sc.floor, NULL), JB_OK);
    t.x = 0.0f;
    t.y = 64.0f;
    t.z = 0.0f;
    r.x = 0.2f * (float)f;
    r.y = 0.31f * (float)f;
    r.z = 0.0f;
    s.x = s.y = s.z = 1.0f;
    model = jb_mat4_trs(t, r, s);
    check("cube", jb_draw_mesh(jb, &sc.cube, &model), JB_OK);
    for (i = 0; i < 12; i++) {
        float a = 0.5236f * (float)i + 0.1f * (float)f, a2 = a + 0.5236f;
        jb_line3d ln;

        memset(&ln, 0, sizeof ln);
        ln.a.x = 180.0f * jb_cosf(a);
        ln.a.y = 120.0f;
        ln.a.z = 180.0f * jb_sinf(a);
        ln.b.x = 180.0f * jb_cosf(a2);
        ln.b.y = 120.0f;
        ln.b.z = 180.0f * jb_sinf(a2);
        ln.color_a = JB_RGB(255, 200, 40);
        ln.color_b = JB_RGB(40, 200, 255);
        ln.blend = JB_BLEND_ADD;
        check("line", jb_draw_line3d(jb, &ln), JB_OK);
    }
    memset(&b, 0, sizeof b);
    b.texture = &sc.star;
    b.blend = JB_BLEND_ADD;
    for (i = 0; i < 4; i++) {
        b.position.x = -200.0f + 130.0f * (float)i;
        b.position.y = 150.0f + 20.0f * jb_sinf(0.5f * (float)(f + i));
        b.position.z = -300.0f;
        b.width = 24.0f + 16.0f * (float)i;
        check("billboard", jb_draw_billboard(jb, &b), JB_OK);
    }
    check("end", jb_end_frame(jb), JB_OK);
}

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

static uint64_t play(const run_def *d, int threads)
{
    jb_config c;
    jb_context *jb;
    uint64_t hash = 1469598103934665603ull;
    uint16_t *vram;
    int f, fw, fh;

    memset(&c, 0, sizeof c);
    c.scale = d->scale;
    c.precise_vertices = d->precise;
    c.perspective_correct = d->perspective;
    c.threads = threads;
    jb = must_create(&c);
    check("geometry", jb_set_geometry(jb, d->geometry), JB_OK);
    textures(jb);
    for (f = 0; f < d->frames; ++f) {
        const uint8_t *px;

        draw(jb, f * (20 / d->frames));
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
        printf("usage: test_draw3d_scene draw3d_scene.txt [--print]\n");
        return 2;
    }
    meshes();
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
    return finish("draw3d_scene");
}

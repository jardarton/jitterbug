/*
 * api_files: the texture and mesh files (include/jitterbug.h, Textures and
 * 3D drawing; docs/formats.md), through the public API, on the converters'
 * pinned outputs in tests/files/expected:
 *
 *   1. Textures: jb_texture_load_mem (from an odd address too) and
 *      jb_texture_load place the same texels and palette, with the same
 *      jb_texture, as jb_texture_create with the desc jb_texture_parse
 *      fills; parse needs 2-byte alignment; a file's extra palettes.
 *   2. Meshes: what jb_mesh_load_mem makes (counts, arrays, materials and
 *      their texture copies), the file form, jb_mesh_destroy, and
 *      jb_destroy freeing what was left (a counting allocator).
 *   3. The round trip: textures and meshes from the files drawn into
 *      frames (1x GTE, 2x float, 2x precise and perspective-correct), the
 *      frames' hashes against those stored below, so every build must
 *      draw them alike.
 *   4. Every refusal of both loaders, each on a valid file with one thing
 *      wrong: JB_ERROR_INVALID_ARGUMENT (or OUT_OF_MEMORY where that is
 *      the error) with its detail, *out unchanged, nothing placed (the
 *      next texture gets the place and id a fresh context gives) and no
 *      memory kept.
 *
 *   test_api_files DIR            DIR is tests/files
 *   test_api_files DIR --print    print the round trip's hashes
 */
#include "api_test.h"

#include "api/formats.h"

/* ------------------------------------------------------------------------ */
/* Files and memory */

static const char *dir;

static unsigned char *read_file(const char *name, size_t *size)
{
    char path[1024];
    unsigned char *buf;
    long n;
    FILE *f;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        printf("cannot read %s\n", path);
        exit(2);
    }
    buf = must_alloc((size_t)n + 8);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        printf("cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static void path_of(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", dir, name);
}

/* A counting allocator, failing from call fail_at on (0: never), and for
   any size above max (0: none). */
typedef struct counter {
    long calls, live, fail_at;
    size_t max;
} counter;

static void *count_alloc(void *user, size_t size)
{
    counter *c = user;
    void *p;

    if ((c->fail_at > 0 && c->calls + 1 >= c->fail_at) || (c->max > 0 && size > c->max))
        return NULL;
    p = malloc(size);
    if (p != NULL) {
        c->calls++;
        c->live++;
    }
    return p;
}

static void count_free(void *user, void *p)
{
    counter *c = user;

    c->live--;
    free(p);
}

static jb_context *create_counted(counter *c, int scale)
{
    jb_config cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.scale = scale;
    cfg.alloc = count_alloc;
    cfg.free = count_free;
    cfg.alloc_user = c;
    return must_create(&cfg);
}

static uint16_t *vram(jb_context *jb)
{
    uint16_t *v = must_alloc((size_t)VRAM_PIXELS * 2);

    check("jb_vram_read", jb_vram_read(jb, 0, 0, VRAM_W, VRAM_H, v), JB_OK);
    return v;
}

static int contains(const char *s, const char *part)
{
    return s != NULL && strstr(s, part) != NULL;
}

static int same_texture(const jb_texture *a, const jb_texture *b)
{
    return a->page_x == b->page_x && a->page_y == b->page_y && a->depth == b->depth &&
           a->clut_x == b->clut_x && a->clut_y == b->clut_y && a->u == b->u && a->v == b->v &&
           a->width == b->width && a->height == b->height && a->repeat == b->repeat &&
           a->id == b->id;
}

static const char *const textures[] = {
    "expected/rocks8.jbt", "expected/rocks8d.jbt", "expected/rocks4.jbt",
    "expected/rocks4d.jbt", "expected/tiles4.jbt", "expected/sky15.jbt",
    "expected/sky15d.jbt", "expected/water4.jbt", "expected/bricks8r.jbt"};

/* ------------------------------------------------------------------------ */
/* 1. Textures */

static void test_textures(void)
{
    size_t t;

    for (t = 0; t < sizeof textures / sizeof *textures; t++) {
        char what[160], path[1024];
        size_t size;
        unsigned char *data = read_file(textures[t], &size), *odd;
        jb_context *a = must_create(NULL), *b = must_create(NULL), *c = must_create(NULL);
        jb_texture_desc d;
        jb_texture ta, tb, tc;
        uint16_t *va, *vb, *vc;
        int palettes = -1;

        snprintf(what, sizeof what, "%s: parse", textures[t]);
        check(what, jb_texture_parse(a, data, size, &d, &palettes), JB_OK);
        check(what, d.depth == JB_TEX_15BIT ? palettes == 0 : palettes >= 1, 1);
        check(what, jb_texture_create(a, &d, &ta), JB_OK);
        /* The same bytes at an odd address. */
        odd = must_alloc(size + 1);
        memcpy(odd + 1, data, size);
        snprintf(what, sizeof what, "%s: load_mem", textures[t]);
        check(what, jb_texture_load_mem(b, odd + 1, size, &tb), JB_OK);
        snprintf(what, sizeof what, "%s: load", textures[t]);
        path_of(path, sizeof path, textures[t]);
        check(what, jb_texture_load(c, path, &tc), JB_OK);
        check("parse refuses an odd address", jb_texture_parse(a, odd + 1, size, &d, NULL),
              JB_ERROR_INVALID_ARGUMENT);
        check("its detail", contains(jb_error_string(a), "aligned to 2 bytes"), 1);
        snprintf(what, sizeof what, "%s: the same jb_texture", textures[t]);
        check(what, same_texture(&ta, &tb) && same_texture(&ta, &tc), 1);
        va = vram(a);
        vb = vram(b);
        vc = vram(c);
        snprintf(what, sizeof what, "%s: the same VRAM", textures[t]);
        check(what, memcmp(va, vb, (size_t)VRAM_PIXELS * 2) == 0 &&
                        memcmp(va, vc, (size_t)VRAM_PIXELS * 2) == 0, 1);
        free(va);
        free(vb);
        free(vc);
        free(odd);
        free(data);
        jb_destroy(a);
        jb_destroy(b);
        jb_destroy(c);
    }

    /* water4.jbt has three palettes (two --frame images), one after
       another; the first is the one placed. Its texels shift by one band
       per frame, so palette k's entry i is the first palette's entry for
       the band k further on: frames 1 and 2 are the first's entries
       rotated (the water's four colours, each its own entry). */
    {
        size_t size;
        unsigned char *data = read_file("expected/water4.jbt", &size);
        jb_context *jb = must_create(NULL);
        jb_texture_desc d;
        int palettes = 0, i, distinct = 0;

        check("water palettes", jb_texture_parse(jb, data, size, &d, &palettes), JB_OK);
        check("water palette count", palettes, 3);
        check("water depth", d.depth, JB_TEX_4BIT);
        for (i = 0; i < 16; i++)
            distinct += d.palette[i] != 0;
        check("water colours", distinct, 4);
        /* Each frame is a permutation of the same four colours. */
        for (i = 1; i < 3; i++) {
            int k, n, found = 0;
            for (k = 0; k < 16; k++)
                for (n = 0; n < 16; n++)
                    if (d.palette[k] != 0 && d.palette[i * 16 + n] == d.palette[k]) {
                        found++;
                        break;
                    }
            check("water frame has the same colours", found, 4);
            check("water frame differs", memcmp(d.palette, d.palette + i * 16, 32) != 0, 1);
        }
        free(data);
        jb_destroy(jb);
    }
}

/* ------------------------------------------------------------------------ */
/* 2. Meshes */

static jb_texture load_texture(jb_context *jb, const char *name)
{
    char path[1024];
    jb_texture t;

    memset(&t, 0, sizeof t);
    path_of(path, sizeof path, name);
    if (jb_texture_load(jb, path, &t) != JB_OK) {
        printf("FAIL loading %s: %s\n", name, jb_error_string(jb));
        exit(2);
    }
    return t;
}

static void test_meshes(void)
{
    counter cnt = {0};
    jb_context *jb = create_counted(&cnt, 1), *other = must_create(NULL);
    jb_texture rocks = load_texture(jb, "expected/rocks8.jbt");
    jb_texture bricks = load_texture(jb, "expected/bricks8r.jbt");
    jb_mesh box, box2, hex, pyr, copy;
    char path[1024];
    long live;
    size_t size;
    unsigned char *data = read_file("expected/box.jbm", &size);
    int i, textured = 0;

    live = cnt.live;
    check("box", jb_mesh_load_mem(jb, data, size, &rocks, 1, &box), JB_OK);
    check("box allocations", cnt.live, live + 1);
    check("box vertices", box.vertex_count, 29);
    check("box faces", box.face_count, 9);
    check("box materials", box.material_count, 2);
    check("box normals", box.normals != NULL, 1);
    check("box colours", box.colors == NULL, 1);
    check("box uv", box.uv != NULL, 1);
    for (i = 0; i < box.material_count; i++)
        if (box.materials[i].texture != NULL) {
            textured++;
            check("box texture copy", same_texture(box.materials[i].texture, &rocks), 1);
            check("box texture is a copy", box.materials[i].texture != &rocks, 1);
        } else {
            check("glass blends", box.materials[i].blend, JB_BLEND_HALF);
        }
    check("box textured materials", textured, 1);
    for (i = 0; i < box.face_count; i++)
        check("box face", box.faces[i].vertices == 3 || box.faces[i].vertices == 4, 1);
    /* The file is not needed once loaded. */
    memset(data, 0xAB, size);
    free(data);
    check("box drawn after its file is gone", box.positions[0].x != (int16_t)0xABAB, 1);

    path_of(path, sizeof path, "expected/box.jbm");
    check("box from the file", jb_mesh_load(jb, path, &rocks, 1, &box2), JB_OK);
    check("the same", box2.vertex_count == box.vertex_count &&
                          memcmp(box2.positions, box.positions,
                                 (size_t)box.vertex_count * sizeof *box.positions) == 0 &&
                          memcmp(box2.faces, box.faces,
                                 (size_t)box.face_count * sizeof *box.faces) == 0,
          1);
    path_of(path, sizeof path, "expected/hexagon.jbm");
    check("hexagon: no textures", jb_mesh_load(jb, path, NULL, 0, &hex), JB_OK);
    check("hexagon colours", hex.colors != NULL && hex.uv == NULL && hex.normals == NULL, 1);
    check("hexagon colour 0 red", hex.colors[0], JB_RGB(255, 0, 0));
    path_of(path, sizeof path, "expected/pyramid.jbm");
    check("pyramid", jb_mesh_load(jb, path, &bricks, 1, &pyr), JB_OK);
    check("pyramid faces", pyr.face_count, 12);

    /* Destroying: a copy names it; twice, by hand, from another context,
       NULL are refused. */
    copy = box;
    check("destroy a copy", jb_mesh_destroy(jb, &copy), JB_OK);
    check("destroy again", jb_mesh_destroy(jb, &box), JB_ERROR_INVALID_ARGUMENT);
    check("its detail", contains(jb_error_string(jb), "jb_mesh_destroy: mesh is not one"), 1);
    check("another context's", jb_mesh_destroy(other, &box2), JB_ERROR_INVALID_ARGUMENT);
    {
        jb_mesh hand;
        jb_gte_vec p = {0, 0, 0};
        memset(&hand, 0, sizeof hand);
        hand.vertex_count = 1;
        hand.positions = &p;
        check("one made by hand", jb_mesh_destroy(jb, &hand), JB_ERROR_INVALID_ARGUMENT);
        hand.positions = NULL;
        check("positions NULL", jb_mesh_destroy(jb, &hand), JB_ERROR_INVALID_ARGUMENT);
    }
    check("mesh NULL", jb_mesh_destroy(jb, NULL), JB_ERROR_INVALID_ARGUMENT);
    check("jb NULL", jb_mesh_destroy(NULL, &box2), JB_ERROR_INVALID_ARGUMENT);
    check("the middle of the list", jb_mesh_destroy(jb, &hex), JB_OK);
    check("allocations: box2 and pyramid", cnt.live, live + 2);
    /* box2 and pyramid are left for jb_destroy. */
    jb_destroy(jb);
    check("jb_destroy frees every loaded mesh", cnt.live, 0);
    jb_destroy(other);
}

/* ------------------------------------------------------------------------ */
/* 3. The round trip */

typedef struct run {
    const char *name;
    int scale, geometry, precise, perspective;
} run;

static const run runs[] = {
    {"gte_1x", 1, JB_GEOMETRY_GTE, 0, 0},
    {"float_2x", 2, JB_GEOMETRY_FLOAT, 0, 0},
    {"precise_2x", 2, JB_GEOMETRY_GTE, 1, 1},
};

/* The frames' hashes, as --print prints them. */
static const uint64_t expected_hashes[3][4] = {
    {0xb88b3480d1f53c7full, 0x9021783c66b1dc85ull, 0x5db26a6bd88ab085ull,
     0xcbf11b2648a61ea9ull},
    {0x0851b6e3854be85bull, 0x2647bc04da959426ull, 0x6e960a1d99440b4eull,
     0x817842504c9d1628ull},
    {0xaa65c8c032165c30ull, 0xa6a7d3ebdacb2de0ull, 0x612a4c8797f60473ull,
     0xab5fe019c460e409ull},
};

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static const char *ppm_path; /* --ppm: write every frame here, as PATH-RUN-FRAME.ppm */

static void test_round_trip(int print)
{
    size_t r;

    for (r = 0; r < sizeof runs / sizeof *runs; r++) {
        jb_config cfg;
        jb_context *jb;
        jb_texture rocks, rocks4, bricks, sky, water;
        jb_mesh box, flat, hex, pyr;
        char path[1024];
        int f;

        memset(&cfg, 0, sizeof cfg);
        cfg.scale = runs[r].scale;
        cfg.precise_vertices = runs[r].precise;
        cfg.perspective_correct = runs[r].perspective;
        jb = must_create(&cfg);
        check("geometry", jb_set_geometry(jb, runs[r].geometry), JB_OK);
        rocks = load_texture(jb, "expected/rocks8.jbt");
        rocks4 = load_texture(jb, "expected/rocks4.jbt");
        bricks = load_texture(jb, "expected/bricks8r.jbt");
        sky = load_texture(jb, "expected/sky15d.jbt");
        water = load_texture(jb, "expected/water4.jbt");
        path_of(path, sizeof path, "expected/box.jbm");
        check("load box", jb_mesh_load(jb, path, &rocks, 1, &box), JB_OK);
        path_of(path, sizeof path, "expected/box-flat.jbm");
        check("load box-flat", jb_mesh_load(jb, path, &rocks4, 1, &flat), JB_OK);
        path_of(path, sizeof path, "expected/hexagon.jbm");
        check("load hexagon", jb_mesh_load(jb, path, NULL, 0, &hex), JB_OK);
        path_of(path, sizeof path, "expected/pyramid.jbm");
        check("load pyramid", jb_mesh_load(jb, path, &bricks, 1, &pyr), JB_OK);
        {
            jb_camera cam;
            jb_light light;
            memset(&cam, 0, sizeof cam);
            cam.eye.x = 0, cam.eye.y = 120, cam.eye.z = 420;
            cam.target.y = 10;
            check("camera", jb_set_camera(jb, &cam), JB_OK);
            memset(&light, 0, sizeof light);
            light.direction.x = -0.5f, light.direction.y = -1.0f, light.direction.z = -0.3f;
            light.color = JB_RGB(255, 240, 220);
            check("light", jb_set_light(jb, 0, &light), JB_OK);
            check("ambient", jb_set_ambient(jb, JB_RGB(60, 60, 80)), JB_OK);
        }
        for (f = 0; f < 4; f++) {
            float a = (float)f * 0.4f;
            jb_mat4 m;
            jb_vec3 t = {0, 0, 0}, rot = {0, 0, 0}, s = {1, 1, 1};
            jb_sprite sp;
            uint16_t pal[16];
            const uint8_t *frame;
            int w, h, i;
            uint64_t hash;

            /* Palette animation: the water's entries turned by f. */
            jb_texture_desc d;
            size_t size;
            unsigned char *wdata = read_file("expected/water4.jbt", &size);
            check("water parse", jb_texture_parse(jb, wdata, size, &d, NULL), JB_OK);
            for (i = 0; i < 16; i++)
                pal[i] = d.palette[(f % 3) * 16 + i];
            check("palette write", jb_palette_write(jb, &water, 0, 16, pal), JB_OK);
            free(wdata);

            check("begin", jb_begin_frame(jb, JB_RGB(20, 30, 50)), JB_OK);
            memset(&sp, 0, sizeof sp);
            sp.texture = &sky;
            sp.w = 320;
            sp.h = 90;
            check("sky", jb_draw_sprite(jb, &sp), JB_OK);
            sp.texture = &water;
            sp.x = 8 + f;
            sp.y = 200;
            sp.w = 64;
            sp.h = 32;
            check("water", jb_draw_sprite(jb, &sp), JB_OK);
            t.x = -120;
            rot.y = a;
            m = jb_mat4_trs(t, rot, s);
            check("draw box", jb_draw_mesh(jb, &box, &m), JB_OK);
            t.x = 120;
            rot.y = -a;
            rot.x = 0.3f;
            m = jb_mat4_trs(t, rot, s);
            check("draw box-flat", jb_draw_mesh(jb, &flat, &m), JB_OK);
            t.x = 0, t.y = -60, t.z = 80;
            rot.x = 0, rot.y = a * 2.0f;
            m = jb_mat4_trs(t, rot, s);
            check("draw hexagon", jb_draw_mesh(jb, &hex, &m), JB_OK);
            t.x = 0, t.y = 0, t.z = -60;
            rot.y = a;
            m = jb_mat4_trs(t, rot, s);
            check("draw pyramid", jb_draw_mesh(jb, &pyr, &m), JB_OK);
            check("end", jb_end_frame(jb), JB_OK);
            frame = jb_frame(jb, &w, &h);
            hash = fnv(frame, (size_t)w * h * 4);
            if (ppm_path != NULL) {
                char name[1100];
                FILE *out;
                snprintf(name, sizeof name, "%s-%s-%d.ppm", ppm_path, runs[r].name, f);
                out = fopen(name, "wb");
                if (out != NULL) {
                    fprintf(out, "P6\n%d %d\n255\n", w, h);
                    for (i = 0; i < w * h; i++)
                        fwrite(frame + 4 * i, 1, 3, out);
                    fclose(out);
                }
            }
            if (print)
                printf("    0x%016llxull, /* %s frame %d */\n", (unsigned long long)hash,
                       runs[r].name, f);
            else {
                char what[64];
                snprintf(what, sizeof what, "%s frame %d", runs[r].name, f);
                check(what, (long long)hash, (long long)expected_hashes[r][f]);
            }
        }
        check("destroy box", jb_mesh_destroy(jb, &box), JB_OK);
        jb_destroy(jb);
    }
}

/* ------------------------------------------------------------------------ */
/* 4. Refusals */

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)(v >> 8 & 0xff);
}

static void put32(unsigned char *p, uint32_t v)
{
    put16(p, (unsigned)(v & 0xffff));
    put16(p + 2, (unsigned)(v >> 16 & 0xffff));
}

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A fresh context's first texture, for "nothing placed". */
static jb_texture probe_texture(jb_context *jb)
{
    static const uint8_t texels[64] = {1};
    static const uint16_t palette[16] = {0x7fff};
    jb_texture_desc d;
    jb_texture t;

    memset(&d, 0, sizeof d);
    d.width = 8;
    d.height = 8;
    d.depth = JB_TEX_4BIT;
    d.texels = texels;
    d.palette = palette;
    memset(&t, 0, sizeof t);
    check("probe texture", jb_texture_create(jb, &d, &t), JB_OK);
    return t;
}

/* One texture refusal: data (size bytes) must be refused with result and a
   detail containing part, with *out unchanged, nothing placed and no
   memory kept. */
static void refuse_texture(const char *what, const unsigned char *data, size_t size,
                           jb_result result, const char *part)
{
    counter cnt = {0};
    jb_context *jb = create_counted(&cnt, 1), *fresh = must_create(NULL);
    jb_texture out, sentinel, a, b;
    jb_texture_desc d, dsent;
    long calls = cnt.calls, live = cnt.live;
    char label[200];

    memset(&sentinel, 0x5A, sizeof sentinel);
    out = sentinel;
    snprintf(label, sizeof label, "%s: load_mem", what);
    check(label, jb_texture_load_mem(jb, data, size, &out), result);
    if (!check(label, contains(jb_error_string(jb), part), 1))
        printf("     detail: %s\n", jb_error_string(jb));
    check(label, contains(jb_error_string(jb), "jb_texture_load_mem"), 1);
    check(label, memcmp(&out, &sentinel, sizeof out) == 0, 1);
    check(label, cnt.calls == calls && cnt.live == live, 1);
    if (result == JB_ERROR_INVALID_ARGUMENT && data != NULL) {
        memset(&dsent, 0x5A, sizeof dsent);
        d = dsent;
        snprintf(label, sizeof label, "%s: parse", what);
        check(label, jb_texture_parse(jb, data, size, &d, NULL), result);
        check(label, contains(jb_error_string(jb), part), 1);
        check(label, memcmp(&d, &dsent, sizeof d) == 0, 1);
    }
    /* Nothing placed: the next texture lands where a fresh context's
       first does, with id 1. */
    a = probe_texture(jb);
    b = probe_texture(fresh);
    snprintf(label, sizeof label, "%s: nothing placed", what);
    check(label, same_texture(&a, &b) && a.id == 1, 1);
    jb_destroy(jb);
    jb_destroy(fresh);
    check(label, cnt.live, 0);
}

static unsigned char *copy_of(const unsigned char *data, size_t size, size_t extra)
{
    unsigned char *c = must_alloc(size + extra);

    memcpy(c, data, size);
    return c;
}

static void test_texture_refusals(void)
{
    size_t size4, size15, size8;
    unsigned char *t4 = read_file("expected/tiles4.jbt", &size4);
    unsigned char *t15 = read_file("expected/sky15.jbt", &size15);
    unsigned char *t8 = read_file("expected/rocks8.jbt", &size8);
    unsigned char *c;
    uint32_t pal4 = get32(t4 + JBI_JBT_O_PALETTE);

#define MUT(file, sz, what, edit, part)                                                       \
    do {                                                                                      \
        c = copy_of(file, sz, 0);                                                             \
        edit;                                                                                 \
        refuse_texture(what, c, sz, JB_ERROR_INVALID_ARGUMENT, part);                         \
        free(c);                                                                              \
    } while (0)

    refuse_texture("NULL data", NULL, 64, JB_ERROR_INVALID_ARGUMENT, "data is NULL");
    refuse_texture("31 bytes", t4, 31, JB_ERROR_INVALID_ARGUMENT, "shorter than a .jbt header");
    refuse_texture("0 bytes", t4, 0, JB_ERROR_INVALID_ARGUMENT, "shorter than a .jbt header");
    refuse_texture("truncated", t4, size4 - 1, JB_ERROR_INVALID_ARGUMENT, "the header says");
    c = copy_of(t4, size4, 4);
    refuse_texture("a byte too many", c, size4 + 1, JB_ERROR_INVALID_ARGUMENT,
                   "the header says");
    free(c);
    MUT(t4, size4, "magic", c[0] = 'X', "not a .jbt");
    MUT(t4, size4, "a mesh's magic", memcpy(c, JBI_JBM_MAGIC, 4), "not a .jbt");
    MUT(t4, size4, "version 2", put32(c + JBI_JBT_O_VERSION, 2), "version 2");
    MUT(t4, size4, "version 0", put32(c + JBI_JBT_O_VERSION, 0), "version 0");
    MUT(t4, size4, "size field larger", put32(c + JBI_JBT_O_SIZE, (uint32_t)size4 + 4),
        "the header says");
    MUT(t4, size4, "size field huge", put32(c + JBI_JBT_O_SIZE, 0xffffffffu), "the header says");
    MUT(t4, size4, "width 0", put16(c + JBI_JBT_O_WIDTH, 0), "outside 1..256");
    MUT(t4, size4, "width 257", put16(c + JBI_JBT_O_WIDTH, 257), "outside 1..256");
    MUT(t4, size4, "height 0", put16(c + JBI_JBT_O_HEIGHT, 0), "outside 1..256");
    MUT(t4, size4, "height 65535", put16(c + JBI_JBT_O_HEIGHT, 65535), "outside 1..256");
    MUT(t4, size4, "depth 5", c[JBI_JBT_O_DEPTH] = 5, "depth 5 is not 4, 8 or 15");
    MUT(t4, size4, "depth 16", c[JBI_JBT_O_DEPTH] = 16, "depth 16 is not");
    MUT(t4, size4, "flag bit 1", c[JBI_JBT_O_FLAGS] = 2, "flags 0x02");
    MUT(t4, size4, "flag bit 7", c[JBI_JBT_O_FLAGS] = 0x81, "flags 0x81");
    MUT(t4, size4, "reserved", c[JBI_JBT_O_RESERVED + 3] = 1, "reserved header field");
    MUT(t15, size15, "repeat 64 x 24", c[JBI_JBT_O_FLAGS] = 1, "power-of-two sizes 8..256");
    MUT(t4, size4, "4-bit, no palette", put16(c + JBI_JBT_O_PALETTES, 0),
        "needs at least one palette");
    MUT(t15, size15, "15-bit with a palette", put16(c + JBI_JBT_O_PALETTES, 1),
        "has no palette");
    MUT(t15, size15, "15-bit with a palette offset", put32(c + JBI_JBT_O_PALETTE, 32),
        "has no palette");
    MUT(t4, size4, "15-bit with a 4-bit file's palette", c[JBI_JBT_O_DEPTH] = 15,
        "has no palette");
    MUT(t4, size4, "texels not aligned", put32(c + JBI_JBT_O_TEXELS, 34), "not a multiple of 4");
    MUT(t4, size4, "texels in the header", put32(c + JBI_JBT_O_TEXELS, 28), "inside the 32-byte");
    MUT(t4, size4, "texels at 0", put32(c + JBI_JBT_O_TEXELS, 0), "inside the 32-byte");
    MUT(t4, size4, "texels past the end", put32(c + JBI_JBT_O_TEXELS, (uint32_t)size4),
        "run past the end");
    MUT(t4, size4, "texels at 2^32 - 4", put32(c + JBI_JBT_O_TEXELS, 0xfffffffcu),
        "run past the end");
    MUT(t4, size4, "palette at 2^32 - 4", put32(c + JBI_JBT_O_PALETTE, 0xfffffffcu),
        "run past the end");
    MUT(t4, size4, "palettes past the end", put16(c + JBI_JBT_O_PALETTES, 2), "run past the end");
    MUT(t4, size4, "palettes 65535", put16(c + JBI_JBT_O_PALETTES, 65535), "run past the end");
    MUT(t4, size4, "palette over the texels", put32(c + JBI_JBT_O_PALETTE, 32), "overlap");
    MUT(t4, size4, "palette over the texels' end", put32(c + JBI_JBT_O_PALETTE, pal4 - 4), "overlap");
    MUT(t8, size8, "8-bit as 15-bit (the texels past the end)", c[JBI_JBT_O_DEPTH] = 15,
        "has no palette");
    MUT(t4, size4, "4-bit texel 16", c[JBI_JBT_HEADER + 17] = 16,
        "4-bit texel (1, 1) is 16, above 15");
    MUT(t4, size4, "4-bit texel 255", c[JBI_JBT_HEADER + 255] = 255,
        "4-bit texel (15, 15) is 255");
    MUT(t8, size8, "8-bit as 4-bit", c[JBI_JBT_O_DEPTH] = 4, "above 15");
#undef MUT

    /* Arguments. */
    {
        jb_context *jb = must_create(NULL);
        jb_texture t;
        jb_texture_desc d;
        char path[1024];
        check("load_mem jb NULL", jb_texture_load_mem(NULL, t4, size4, &t),
              JB_ERROR_INVALID_ARGUMENT);
        check("load_mem out NULL", jb_texture_load_mem(jb, t4, size4, NULL),
              JB_ERROR_INVALID_ARGUMENT);
        check("its detail", contains(jb_error_string(jb), "out is NULL"), 1);
        check("parse jb NULL", jb_texture_parse(NULL, t4, size4, &d, NULL),
              JB_ERROR_INVALID_ARGUMENT);
        check("parse desc NULL", jb_texture_parse(jb, t4, size4, NULL, NULL),
              JB_ERROR_INVALID_ARGUMENT);
        check("load jb NULL", jb_texture_load(NULL, "x", &t), JB_ERROR_INVALID_ARGUMENT);
        check("load path NULL", jb_texture_load(jb, NULL, &t), JB_ERROR_INVALID_ARGUMENT);
        check("its detail", contains(jb_error_string(jb), "path is NULL"), 1);
        check("load out NULL", jb_texture_load(jb, "x", NULL), JB_ERROR_INVALID_ARGUMENT);
        path_of(path, sizeof path, "expected/no-such-file.jbt");
        check("load a missing file", jb_texture_load(jb, path, &t), JB_ERROR_INVALID_ARGUMENT);
        check("its detail", contains(jb_error_string(jb), "jb_texture_load: cannot open"), 1);
        path_of(path, sizeof path, "expected");
        check("load a directory", jb_texture_load(jb, path, &t), JB_ERROR_INVALID_ARGUMENT);
        check("its detail", contains(jb_error_string(jb), "jb_texture_load: cannot"), 1);
        path_of(path, sizeof path, "expected/box.jbm");
        check("load a mesh as a texture", jb_texture_load(jb, path, &t),
              JB_ERROR_INVALID_ARGUMENT);
        check("its detail", contains(jb_error_string(jb), "jb_texture_load: not a .jbt"), 1);
        /* jb_texture_create's own errors, named for the loader: no room. */
        check("reserve", jb_vram_reserve(jb, 0, 0, 1024, 512), JB_OK);
        check("no room in VRAM", jb_texture_load_mem(jb, t4, size4, &t), JB_ERROR_OUT_OF_MEMORY);
        check("its detail", contains(jb_error_string(jb), "jb_texture_load_mem: no room in VRAM"),
              1);
        jb_destroy(jb);
    }
    /* The file form's memory: an allocator that fails. */
    {
        counter cnt = {0};
        jb_context *jb = create_counted(&cnt, 1);
        jb_texture t;
        char path[1024];
        path_of(path, sizeof path, "expected/tiles4.jbt");
        cnt.fail_at = cnt.calls + 1;
        check("load without memory", jb_texture_load(jb, path, &t), JB_ERROR_OUT_OF_MEMORY);
        check("its detail", contains(jb_error_string(jb), "jb_texture_load: no memory"), 1);
        cnt.fail_at = 0;
        check("then with", jb_texture_load(jb, path, &t), JB_OK);
        jb_destroy(jb);
        check("no memory kept", cnt.live, 0);
    }
    /* A directory, and a file past the 64 MB limit, are refused as such
       before any memory is taken for them: with an allocator that gives
       at most 1 MB, they are JB_ERROR_INVALID_ARGUMENT, not
       JB_ERROR_OUT_OF_MEMORY. The large file is sparse where the file
       system allows. */
    {
        counter cnt = {0};
        jb_context *jb = create_counted(&cnt, 1);
        jb_texture t;
        jb_mesh m;
        char path[1024];
        const char *big = "api_files_big.jbt";
        FILE *f = fopen(big, "wb");
        long calls;
        cnt.max = 1 << 20;
        calls = cnt.calls;
        path_of(path, sizeof path, "expected");
        check("a directory, 1 MB allocator", jb_texture_load(jb, path, &t),
              JB_ERROR_INVALID_ARGUMENT);
        check("its detail", contains(jb_error_string(jb), "jb_texture_load: cannot read"), 1);
        check("a directory as a mesh", jb_mesh_load(jb, path, NULL, 0, &m),
              JB_ERROR_INVALID_ARGUMENT);
        check("its detail", contains(jb_error_string(jb), "jb_mesh_load: cannot read"), 1);
        if (f == NULL || fseek(f, 64L << 20, SEEK_SET) != 0 || fputc(0, f) == EOF ||
            fclose(f) != 0) {
            printf("FAIL cannot write %s\n", big);
            api_t.failures++;
        } else {
            check("a file of 64 MB + 1", jb_texture_load(jb, big, &t), JB_ERROR_INVALID_ARGUMENT);
            check("its detail",
                  contains(jb_error_string(jb), "is larger than 67108864 bytes"), 1);
        }
        remove(big);
        check("no allocation for either", cnt.calls, calls);
        jb_destroy(jb);
    }
    free(t4);
    free(t15);
    free(t8);
}

/* One mesh refusal, as refuse_texture. */
static void refuse_mesh(const char *what, const unsigned char *data, size_t size,
                        const jb_texture *tex, int count, jb_result result, const char *part)
{
    counter cnt = {0};
    jb_context *jb = create_counted(&cnt, 1);
    jb_mesh out, sentinel;
    long live;
    char label[200];
    jb_texture own[2];
    int i;

    /* The textures, made on this context at the sizes given. */
    for (i = 0; i < count && i < 2; i++) {
        jb_texture_desc d;
        memset(&d, 0, sizeof d);
        d.width = tex[i].width;
        d.height = tex[i].height;
        d.depth = JB_TEX_15BIT;
        d.repeat = tex[i].repeat;
        if (jb_texture_create(jb, &d, &own[i]) != JB_OK) {
            printf("FAIL %s: texture: %s\n", what, jb_error_string(jb));
            exit(2);
        }
    }
    live = cnt.live;
    memset(&sentinel, 0x5A, sizeof sentinel);
    out = sentinel;
    snprintf(label, sizeof label, "%s", what);
    check(label, jb_mesh_load_mem(jb, data, size, count > 0 && tex != NULL ? own : NULL, count,
                                  &out),
          result);
    if (!check(label, contains(jb_error_string(jb), part), 1))
        printf("     detail: %s\n", jb_error_string(jb));
    check(label, contains(jb_error_string(jb), "jb_mesh_load_mem"), 1);
    check(label, memcmp(&out, &sentinel, sizeof out) == 0, 1);
    check(label, cnt.live, live);
    jb_destroy(jb);
    check(label, cnt.live, 0);
}

static void test_mesh_refusals(void)
{
    size_t sbox, shex, spyr;
    unsigned char *box = read_file("expected/box.jbm", &sbox);
    unsigned char *hex = read_file("expected/hexagon.jbm", &shex);
    unsigned char *pyr = read_file("expected/pyramid.jbm", &spyr);
    unsigned char *c;
    jb_texture t32, t16, b64, two[2];
    uint32_t off[JBI_JBM_SECTIONS];
    int i;

    memset(&t32, 0, sizeof t32);
    t32.width = t32.height = 32;
    t16 = t32;
    t16.width = t16.height = 16;
    b64 = t32;
    b64.width = 64;
    b64.repeat = 1;
    two[0] = two[1] = t32;
    for (i = 0; i < JBI_JBM_SECTIONS; i++)
        off[i] = get32(box + JBI_JBM_O_SECTIONS + 4 * i);

#define MUT(file, sz, what, edit, part)                                                       \
    do {                                                                                      \
        c = copy_of(file, sz, 0);                                                             \
        edit;                                                                                 \
        refuse_mesh(what, c, sz, &t32, 1, JB_ERROR_INVALID_ARGUMENT, part);                   \
        free(c);                                                                              \
    } while (0)

    refuse_mesh("NULL data", NULL, 64, &t32, 1, JB_ERROR_INVALID_ARGUMENT, "data is NULL");
    refuse_mesh("63 bytes", box, 63, &t32, 1, JB_ERROR_INVALID_ARGUMENT,
                "shorter than a .jbm header");
    refuse_mesh("truncated", box, sbox - 1, &t32, 1, JB_ERROR_INVALID_ARGUMENT,
                "the header says");
    c = copy_of(box, sbox, 4);
    refuse_mesh("a byte too many", c, sbox + 1, &t32, 1, JB_ERROR_INVALID_ARGUMENT,
                "the header says");
    free(c);
    MUT(box, sbox, "magic", c[3] = 'X', "not a .jbm");
    MUT(box, sbox, "a texture's magic", memcpy(c, JBI_JBT_MAGIC, 4), "not a .jbm");
    MUT(box, sbox, "version 2", put32(c + JBI_JBM_O_VERSION, 2), "version 2");
    MUT(box, sbox, "size field", put32(c + JBI_JBM_O_SIZE, 0), "the header says 0 bytes");
    MUT(box, sbox, "flag bit 3", c[JBI_JBM_O_FLAGS] |= 8, "flags 0x0000000d has bits other");
    MUT(box, sbox, "flag bit 31", c[JBI_JBM_O_FLAGS + 3] = 0x80, "flags 0x80000005");
    MUT(box, sbox, "reserved", c[JBI_JBM_O_RESERVED] = 1, "reserved header field");
    MUT(box, sbox, "no vertices", put32(c + JBI_JBM_O_VERTICES, 0), "vertex count 0");
    MUT(box, sbox, "65537 vertices", put32(c + JBI_JBM_O_VERTICES, 65537), "vertex count 65537");
    MUT(box, sbox, "2^32 - 1 vertices", put32(c + JBI_JBM_O_VERTICES, 0xffffffffu),
        "vertex count 4294967295");
    MUT(box, sbox, "too many faces", put32(c + JBI_JBM_O_FACES, 1048577), "face count 1048577");
    MUT(box, sbox, "more faces than the file", put32(c + JBI_JBM_O_FACES, 1048576),
        "run past the end");
    MUT(box, sbox, "too many materials", put32(c + JBI_JBM_O_MATERIALS, 65537),
        "material count 65537");
    MUT(box, sbox, "faces, no materials",
        (put32(c + JBI_JBM_O_MATERIALS, 0), put32(c + JBI_JBM_O_SECTIONS + 4 * 5, 0)),
        "faces but no material");
    MUT(box, sbox, "too many slots", put32(c + JBI_JBM_O_SLOTS, 65536),
        "texture slot count 65536");
    MUT(box, sbox, "a section's offset without it",
        c[JBI_JBM_O_FLAGS] &= (unsigned char)~JBI_JBM_NORMALS,
        "there are no normals, but their offset");
    MUT(box, sbox, "colours flagged, no offset", c[JBI_JBM_O_FLAGS] |= JBI_JBM_COLORS,
        "the colours are at offset 0, inside the 64-byte header");
    MUT(box, sbox, "positions not aligned",
        put32(c + JBI_JBM_O_SECTIONS, off[JBI_JBM_S_POSITIONS] + 2), "not a multiple of 4");
    MUT(box, sbox, "positions in the header", put32(c + JBI_JBM_O_SECTIONS, 60),
        "inside the 64-byte header");
    MUT(box, sbox, "positions past the end", put32(c + JBI_JBM_O_SECTIONS, 0xfffffffcu),
        "run past the end");
    MUT(box, sbox, "faces past the end",
        put32(c + JBI_JBM_O_SECTIONS + 4 * JBI_JBM_S_FACES, (uint32_t)sbox - 4),
        "the faces (108 bytes");
    MUT(box, sbox, "uv over the positions",
        put32(c + JBI_JBM_O_SECTIONS + 4 * JBI_JBM_S_UV, off[JBI_JBM_S_POSITIONS] + 4),
        "the positions and the texel coordinates overlap");
    MUT(box, sbox, "slots over the materials",
        put32(c + JBI_JBM_O_SECTIONS + 4 * JBI_JBM_S_SLOTS, off[JBI_JBM_S_MATERIALS]),
        "the materials and the texture slots overlap");
    MUT(box, sbox, "slot size 0", put16(c + off[JBI_JBM_S_SLOTS], 0),
        "texture slot 0's size 0 x 32");
    MUT(box, sbox, "slot size 257", put16(c + off[JBI_JBM_S_SLOTS] + 2, 257),
        "texture slot 0's size 32 x 257");
    MUT(box, sbox, "a normal of 4097", put16(c + off[JBI_JBM_S_NORMALS] + 2, 4097),
        "normal 0 has a component 4097");
    MUT(box, sbox, "a normal of -4097", put16(c + off[JBI_JBM_S_NORMALS] + 6 + 4, 0xefff),
        "normal 1 has a component -4097");
    MUT(box, sbox, "5 vertices", put16(c + off[JBI_JBM_S_FACES] + 8, 5),
        "faces[0].vertices is 5");
    MUT(box, sbox, "2 vertices", put16(c + off[JBI_JBM_S_FACES] + 8, 2),
        "faces[0].vertices is 2");
    MUT(box, sbox, "an index past the vertices", put16(c + off[JBI_JBM_S_FACES] + 2, 29),
        "faces[0].v[1] is 29, but there are 29 vertices");
    MUT(box, sbox, "an index 65535", put16(c + off[JBI_JBM_S_FACES] + 12 * 8, 65535),
        "faces[8].v[0] is 65535");
    MUT(box, sbox, "a triangle's fourth index",
        put16(c + off[JBI_JBM_S_FACES] + 12 * 6 + 6, 1),
        "face 6 is a triangle, but its fourth index is 1");
    MUT(box, sbox, "a material past the materials", put16(c + off[JBI_JBM_S_FACES] + 10, 2),
        "faces[0].material is 2, but there are 2 materials");
    MUT(box, sbox, "blend 5", c[off[JBI_JBM_S_MATERIALS] + 4] = 5,
        "material 0's blend mode 5");
    MUT(box, sbox, "material flag bit 3", c[off[JBI_JBM_S_MATERIALS] + 5] = 8,
        "material 0's flags 0x08");
    MUT(box, sbox, "a slot past the slots", put16(c + off[JBI_JBM_S_MATERIALS] + 6, 1),
        "material 0's texture slot is 1, but there are 1 slots");
    MUT(box, sbox, "a material colour above 0xFFFFFF",
        put32(c + off[JBI_JBM_S_MATERIALS], 0x01000000u), "materials[0].color 0x1000000");
    MUT(box, sbox, "u past the texture", c[off[JBI_JBM_S_UV]] = 32,
        "has u, v (32, ");
    MUT(box, sbox, "v 255", c[off[JBI_JBM_S_UV] + 1] = 255, "outside its 32 x 32 texture");
#undef MUT
    /* Vertex colours, on the hexagon. */
    {
        uint32_t hcol = get32(hex + JBI_JBM_O_SECTIONS + 4 * JBI_JBM_S_COLORS);
        c = copy_of(hex, shex, 0);
        put32(c + hcol + 4, 0x01000000u);
        refuse_mesh("a vertex colour above 0xFFFFFF", c, shex, NULL, 0,
                    JB_ERROR_INVALID_ARGUMENT, "colors[1] 0x1000000 is above 0xFFFFFF");
        free(c);
    }
    /* The bound textures. */
    refuse_mesh("no textures for a slot", box, sbox, NULL, 0, JB_ERROR_INVALID_ARGUMENT,
                "the mesh has 1 texture slots, but 0 textures are given");
    refuse_mesh("textures for a mesh without slots", hex, shex, &t32, 1,
                JB_ERROR_INVALID_ARGUMENT, "the mesh has 0 texture slots, but 1 textures");
    refuse_mesh("two textures for one slot", box, sbox, two, 2, JB_ERROR_INVALID_ARGUMENT,
                "but 2 textures are given");
    refuse_mesh("a texture of the wrong size", box, sbox, &t16, 1, JB_ERROR_INVALID_ARGUMENT,
                "texture slot 0 was converted for a 32 x 32 texture, but textures[0] is 16 x 16");
    {
        /* The pyramid's u, v run past its repeat texture's width (they
           repeat); bound to one that does not repeat, they are refused. */
        jb_texture b = b64;
        b.repeat = 0;
        refuse_mesh("a texture that does not repeat for the pyramid", pyr, spyr, &b, 1,
                    JB_ERROR_INVALID_ARGUMENT, "outside its 64 x 32 texture");
    }
    /* Arguments. */
    {
        jb_context *jb = must_create(NULL);
        jb_mesh m;
        char path[1024];
        check("jb NULL", jb_mesh_load_mem(NULL, box, sbox, &t32, 1, &m),
              JB_ERROR_INVALID_ARGUMENT);
        check("out NULL", jb_mesh_load_mem(jb, box, sbox, &t32, 1, NULL),
              JB_ERROR_INVALID_ARGUMENT);
        check("its detail", contains(jb_error_string(jb), "jb_mesh_load_mem: out is NULL"), 1);
        check("textures NULL", jb_mesh_load_mem(jb, box, sbox, NULL, 1, &m),
              JB_ERROR_INVALID_ARGUMENT);
        check("its detail", contains(jb_error_string(jb), "textures is NULL but texture_count is 1"),
              1);
        check("count negative", jb_mesh_load_mem(jb, box, sbox, &t32, -1, &m),
              JB_ERROR_INVALID_ARGUMENT);
        check("its detail", contains(jb_error_string(jb), "texture_count -1 is negative"), 1);
        check("load jb NULL", jb_mesh_load(NULL, "x", NULL, 0, &m), JB_ERROR_INVALID_ARGUMENT);
        check("load path NULL", jb_mesh_load(jb, NULL, NULL, 0, &m), JB_ERROR_INVALID_ARGUMENT);
        check("load out NULL", jb_mesh_load(jb, "x", NULL, 0, NULL), JB_ERROR_INVALID_ARGUMENT);
        path_of(path, sizeof path, "expected/no-such-file.jbm");
        check("load a missing file", jb_mesh_load(jb, path, NULL, 0, &m),
              JB_ERROR_INVALID_ARGUMENT);
        check("its detail", contains(jb_error_string(jb), "jb_mesh_load: cannot open"), 1);
        path_of(path, sizeof path, "expected/tiles4.jbt");
        check("load a texture as a mesh", jb_mesh_load(jb, path, NULL, 0, &m),
              JB_ERROR_INVALID_ARGUMENT);
        check("its detail", contains(jb_error_string(jb), "jb_mesh_load: not a .jbm"), 1);
        jb_destroy(jb);
    }
    /* No memory: at every allocation the loads make. */
    {
        int k;
        for (k = 1; k <= 3; k++) {
            counter cnt = {0};
            jb_context *jb = create_counted(&cnt, 1);
            jb_texture tex;
            jb_mesh m;
            char path[1024];
            jb_result r;
            long live;
            jb_texture_desc d;
            memset(&d, 0, sizeof d);
            d.width = d.height = 32;
            d.depth = JB_TEX_15BIT;
            check("texture", jb_texture_create(jb, &d, &tex), JB_OK);
            live = cnt.live;
            path_of(path, sizeof path, "expected/box.jbm");
            cnt.fail_at = cnt.calls + k;
            r = jb_mesh_load(jb, path, &tex, 1, &m);
            if (k <= 2) {
                check("no memory", r, JB_ERROR_OUT_OF_MEMORY);
                check("its detail", contains(jb_error_string(jb), "jb_mesh_load: no memory"), 1);
                check("nothing kept", cnt.live, live);
            } else {
                check("memory enough", r, JB_OK);
            }
            jb_destroy(jb);
            check("no memory kept", cnt.live, 0);
        }
    }
    free(box);
    free(hex);
    free(pyr);
}

int main(int argc, char **argv)
{
    int print = argc > 2 && strcmp(argv[2], "--print") == 0;

    if (argc < 2) {
        printf("usage: test_api_files DIR [--print | --ppm PATH]\n");
        return 2;
    }
    dir = argv[1];
    if (argc > 3 && strcmp(argv[2], "--ppm") == 0) {
        ppm_path = argv[3];
        print = 1;
    }
    if (print) {
        test_round_trip(1);
        return 0;
    }
    test_textures();
    test_meshes();
    test_round_trip(0);
    test_texture_refusals();
    test_mesh_refusals();
    return finish("api_files");
}

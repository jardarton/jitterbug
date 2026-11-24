/*
 * api_draw3d: the friendly 3D draws (include/jitterbug.h, 3D drawing).
 *
 *   - Equivalence: a lit, textured mesh with every face exact (checked
 *     through the pipeline first) drawn by jb_draw_mesh, and the same faces
 *     drawn by hand-written console code into the next frame of the same
 *     context: the quantised registers (jbi_geom_begin) written with
 *     jb_gte_write, each face through RTPT/RTPS, NCLIP, AVSZ and NCCS/NCDS
 *     with jb_gte_command, the results into jb_poly and a jb_ot by
 *     bucket, drawn with jb_ot_draw. The frames must be identical: at 1x,
 *     2x and 3x, 2x with precise vertices (the hand-written code's
 *     vertices then come from the GTE's own precise table), 2x and 1x with
 *     perspective-correct textures, with fog and without, on 1 and 4
 *     threads.
 *   - A walk through a floor of quads and a wall, the camera crossing
 *     both: drawn additively in one flat colour, every pixel whose ray
 *     meets the geometry (computed in double, the pixels within two of an
 *     outline left out) must be drawn exactly once: no holes, no pixel
 *     drawn twice. Both paths.
 *   - Culling: a closed cube from outside covers its outline once; from
 *     inside nothing is drawn. Ordering: a near face drawn before a far
 *     one still covers it. Double-sided materials.
 *   - Billboards (one texel a pixel: the texture exactly, as a rectangle;
 *     scaled: a quad; behind the camera: nothing), 3D lines (through the
 *     near plane, behind the camera), fog on a billboard.
 *   - Every error, latched as the sticky error; the frame full.
 *   - Three fixed cases of the seam fix (seam_fix_cases): it leaves 1x
 *     and non-precise frames as they were; the precise pass culls on the
 *     positions it draws.
 */
#include "api_test.h"

#include <math.h>

#include "api/frame.h"
#include "geom/pipeline.h"

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

static jb_context *make(int scale, int threads, int precise, int persp)
{
    jb_config c;

    memset(&c, 0, sizeof c);
    c.scale = scale;
    c.threads = threads;
    c.precise_vertices = precise;
    c.perspective_correct = persp;
    return must_create(&c);
}

static uint8_t *copy_frame(jb_context *jb, int *w, int *h)
{
    const uint8_t *px = jb_frame(jb, w, h);
    uint8_t *out = must_alloc((size_t)*w * (size_t)*h * 4);

    memcpy(out, px, (size_t)*w * (size_t)*h * 4);
    return out;
}

/* ------------------------------------------------------------------------ */
/* A test mesh: a box of 6 quads and a fan of triangles. */

enum { BOX_V = 24, BOX_F = 6 };

typedef struct mesh_data {
    jb_gte_vec pos[64], nrm[64];
    uint32_t col[64];
    uint8_t uv[128];
    jb_mesh_face faces[64];
    jb_material mats[4];
    jb_mesh mesh;
} mesh_data;

/* A box from -s to s, its faces anticlockwise from outside, normals out,
   u, v over each face. */
static void box(mesh_data *m, int s, const jb_texture *tex)
{
    static const int corner[6][4][3] = {
        {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},     /* +z */
        {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}}, /* -z */
        {{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}},     /* +x */
        {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}}, /* -x */
        {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},     /* +y */
        {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}}, /* -y */
    };
    static const int normal[6][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0},
                                     {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    static const int uv[4][2] = {{0, 15}, {15, 15}, {15, 0}, {0, 0}};
    int f, k;

    memset(m, 0, sizeof *m);
    for (f = 0; f < 6; f++) {
        for (k = 0; k < 4; k++) {
            int i = f * 4 + k;

            m->pos[i].x = (int16_t)(corner[f][k][0] * s);
            m->pos[i].y = (int16_t)(corner[f][k][1] * s);
            m->pos[i].z = (int16_t)(corner[f][k][2] * s);
            m->nrm[i].x = (int16_t)(normal[f][0] * 4096);
            m->nrm[i].y = (int16_t)(normal[f][1] * 4096);
            m->nrm[i].z = (int16_t)(normal[f][2] * 4096);
            m->uv[2 * i] = (uint8_t)uv[k][0];
            m->uv[2 * i + 1] = (uint8_t)uv[k][1];
            m->col[i] = JB_RGB(100 + 20 * f, 128, 200 - 10 * k);
            m->faces[f].v[k] = (uint16_t)i;
        }
        m->faces[f].vertices = 4;
        m->faces[f].material = (uint16_t)(f % 2);
    }
    m->mats[0].texture = tex;
    m->mats[0].color = 0;
    m->mats[1].color = JB_RGB(90, 160, 110);
    m->mats[1].blend = JB_BLEND_NONE;
    m->mesh.vertex_count = BOX_V;
    m->mesh.positions = m->pos;
    m->mesh.normals = m->nrm;
    m->mesh.colors = NULL;
    m->mesh.uv = m->uv;
    m->mesh.face_count = BOX_F;
    m->mesh.faces = m->faces;
    m->mesh.material_count = 2;
    m->mesh.materials = m->mats;
}

static void make_texture(jb_context *jb, jb_texture *t)
{
    uint16_t texels[16 * 16];
    jb_texture_desc d;
    int i;

    for (i = 0; i < 256; i++)
        texels[i] = (uint16_t)(JB_RGB15((i * 7) & 31, (i >> 4) * 2, (i & 15) * 2) | 0x0421);
    memset(&d, 0, sizeof d);
    d.width = d.height = 16;
    d.depth = JB_TEX_15BIT;
    d.texels = texels;
    check("texture", jb_texture_create(jb, &d, t), JB_OK);
}

/* ------------------------------------------------------------------------ */
/* Hand-written console code */

static void hand_matrix(jb_context *jb, int first, const jb_gte_matrix *a)
{
    jb_gte_write(jb, first + 0, JB_GTE_PAIR(a->m[0][0], a->m[0][1]));
    jb_gte_write(jb, first + 1, JB_GTE_PAIR(a->m[0][2], a->m[1][0]));
    jb_gte_write(jb, first + 2, JB_GTE_PAIR(a->m[1][1], a->m[1][2]));
    jb_gte_write(jb, first + 3, JB_GTE_PAIR(a->m[2][0], a->m[2][1]));
    jb_gte_write(jb, first + 4, (uint32_t)(int32_t)a->m[2][2]);
}

static void hand_load(jb_context *jb, const jbi_geom_draw *d)
{
    int i;

    hand_matrix(jb, JB_GTE_RT11RT12, &d->r.rt);
    hand_matrix(jb, JB_GTE_L11L12, &d->r.llm);
    hand_matrix(jb, JB_GTE_LR1LR2, &d->r.lcm);
    for (i = 0; i < 3; i++) {
        jb_gte_write(jb, JB_GTE_TRX + i, (uint32_t)d->r.tr[i]);
        jb_gte_write(jb, JB_GTE_RBK + i, (uint32_t)d->r.bk[i]);
        jb_gte_write(jb, JB_GTE_RFC + i, (uint32_t)d->r.fc[i]);
    }
    jb_gte_write(jb, JB_GTE_OFX, (uint32_t)d->r.ofx);
    jb_gte_write(jb, JB_GTE_OFY, (uint32_t)d->r.ofy);
    jb_gte_write(jb, JB_GTE_H, (uint32_t)d->r.h);
    jb_gte_write(jb, JB_GTE_DQA, (uint32_t)(int32_t)d->r.dqa);
    jb_gte_write(jb, JB_GTE_DQB, (uint32_t)d->r.dqb);
    jb_gte_write(jb, JB_GTE_ZSF3, (uint32_t)(int32_t)d->r.zsf3);
    jb_gte_write(jb, JB_GTE_ZSF4, (uint32_t)(int32_t)d->r.zsf4);
}

static void hand_vertex(jb_context *jb, int n, int x, int y, int z)
{
    jb_gte_write(jb, JB_GTE_VXY0 + 2 * n, JB_GTE_PAIR(x, y));
    jb_gte_write(jb, JB_GTE_VZ0 + 2 * n, (uint32_t)z);
}

/* One face as console code builds it, added to ot; 0 when culled. */
static int hand_face(jb_context *jb, jb_ot *ot, const jbi_geom_draw *d, const jb_mesh *m, int fi)
{
    static const int zorder[4] = {0, 1, 3, 2};
    const jb_mesh_face *f = &m->faces[fi];
    const jb_material *mat = &m->materials[f->material];
    int n = f->vertices, k, sh = d->shift, idx[4], bucket, gouraud = 0;
    uint32_t sxy[4], otz;
    int32_t ir0[4], mac0;
    jb_poly poly;
    int64_t b;

    for (k = 0; k < n; k++)
        idx[k] = f->v[n == 4 ? zorder[k] : k];
#define P(k) m->positions[idx[k]].x *(1 << sh), m->positions[idx[k]].y *(1 << sh), \
             m->positions[idx[k]].z *(1 << sh)
    if (d->fog) {
        for (k = 0; k < 3; k++) {
            hand_vertex(jb, 0, P(k));
            jb_gte_command(jb, JB_GTE_RTPS);
            sxy[k] = jb_gte_read(jb, JB_GTE_SXY2);
            ir0[k] = (int32_t)jb_gte_read(jb, JB_GTE_IR0);
        }
    } else {
        for (k = 0; k < 3; k++)
            hand_vertex(jb, k, P(k));
        jb_gte_command(jb, JB_GTE_RTPT);
        for (k = 0; k < 3; k++) {
            sxy[k] = jb_gte_read(jb, JB_GTE_SXY0 + k);
            ir0[k] = 0;
        }
    }
    jb_gte_command(jb, JB_GTE_NCLIP);
    mac0 = (int32_t)jb_gte_read(jb, JB_GTE_MAC0);
    if (n == 4) {
        hand_vertex(jb, 0, P(3));
        jb_gte_command(jb, JB_GTE_RTPS);
        sxy[3] = jb_gte_read(jb, JB_GTE_SXY2);
        ir0[3] = (int32_t)jb_gte_read(jb, JB_GTE_IR0);
    }
#undef P
    jb_gte_command(jb, n == 4 ? JB_GTE_AVSZ4 : JB_GTE_AVSZ3);
    otz = jb_gte_read(jb, JB_GTE_OTZ);
    if (mac0 == 0 && n == 4) /* the first triangle has no area: the second's */
        mac0 = jb_gte_nclip(sxy[1], sxy[3], sxy[2], NULL);
    if (mac0 >= 0 && !mat->double_sided)
        return 0;
    b = ((int64_t)otz - d->otz_near) * d->ot_size;
    b = b <= 0 ? 0 : b / ((int64_t)d->otz_far - d->otz_near + 1);
    bucket = (int)(b > d->ot_size - 1 ? d->ot_size - 1 : b);
    memset(&poly, 0, sizeof poly);
    poly.vertices = n;
    poly.blend = mat->blend;
    poly.texture = mat->texture;
    poly.raw = mat->texture != NULL && mat->raw;
    for (k = 0; k < n; k++) {
        uint32_t c = m->colors ? m->colors[idx[k]] : mat->color ? mat->color : JB_NEUTRAL;
        if (!mat->unlit) {
            const jb_gte_vec *nv = &m->normals[idx[k]];

            jb_gte_write(jb, JB_GTE_RGBC, c);
            hand_vertex(jb, 0, nv->x, nv->y, nv->z);
            if (d->fog) {
                jb_gte_write(jb, JB_GTE_IR0, (uint32_t)ir0[k]);
                jb_gte_command(jb, JB_GTE_NCDS);
            } else {
                jb_gte_command(jb, JB_GTE_NCCS);
            }
            c = jb_gte_read(jb, JB_GTE_RGB2) & 0xffffff;
        } else if (d->fog) {
            jb_gte_write(jb, JB_GTE_RGBC, c);
            jb_gte_write(jb, JB_GTE_IR0, (uint32_t)ir0[k]);
            jb_gte_command(jb, JB_GTE_DPCS);
            c = jb_gte_read(jb, JB_GTE_RGB2) & 0xffffff;
        }
        poly.v[k].x = JB_GTE_SX(sxy[k]);
        poly.v[k].y = JB_GTE_SY(sxy[k]);
        poly.v[k].color = c;
        if (mat->texture != NULL) {
            poly.v[k].u = mat->texture->u + m->uv[2 * idx[k]];
            poly.v[k].v = mat->texture->v + m->uv[2 * idx[k] + 1];
        }
        gouraud |= c != poly.v[0].color;
    }
    poly.gouraud = gouraud;
    check("hand-written face added", jb_ot_add_poly(ot, bucket, &poly), JB_OK);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Equivalence */

static void scene_lights(jb_context *jb, int fog)
{
    jb_light l;

    memset(&l, 0, sizeof l);
    l.direction.x = -0.4f;
    l.direction.y = -1.0f;
    l.direction.z = -0.6f;
    l.color = JB_RGB(255, 230, 200);
    jb_set_light(jb, 0, &l);
    l.direction.x = 1.0f;
    l.direction.y = 0.2f;
    l.direction.z = 0.0f;
    l.color = JB_RGB(40, 60, 160);
    jb_set_light(jb, 1, &l);
    jb_set_ambient(jb, JB_RGB(60, 60, 70));
    if (fog)
        jb_set_fog(jb, JB_RGB(90, 100, 120), 300.0f, 1600.0f);
}

static void equivalence(int scale, int threads, int precise, int persp, int fog)
{
    jb_context *jb = make(scale, threads, precise, persp);
    jb_texture tex;
    static mesh_data md;
    jbi_geom_draw d;
    jb_camera cam;
    jb_ot *ot;
    jb_result r;
    uint8_t *a, *b;
    int w, h, step, faces = 0, exact = 0, differ = 0, i;
    char what[120], detail[200];

    snprintf(what, sizeof what, "equivalence %dx, %d threads%s%s%s", scale, threads,
             precise ? ", precise" : "", persp ? ", perspective" : "", fog ? ", fog" : "");
    make_texture(jb, &tex);
    box(&md, 60, &tex);
    memset(&cam, 0, sizeof cam);
    cam.eye.x = 40.0f;
    cam.eye.y = 90.0f;
    cam.eye.z = 260.0f;
    cam.fov_y = 55.0f;
    check(what, jb_set_camera(jb, &cam), JB_OK);
    scene_lights(jb, fog);
    ot = jb_ot_create(jb, jbi_frame_ot_size(jb), 64, &r);
    for (step = 0; step < 24; step++) {
        jb_vec3 t = {(float)(step * 7 - 80), (float)(step * 3 - 30), (float)(-step * 11)};
        jb_vec3 rot = {0.13f * (float)step, 0.29f * (float)step, 0.07f * (float)step};
        jb_vec3 sc = {1.0f, 0.8f + 0.02f * (float)step, 1.1f};
        jb_mat4 model = jb_mat4_trs(t, rot, sc);
        int extent;
        jbi_geom_poly p[2];

        /* Only faces that need no clipping are equal to console code: check
           the pipeline says so, so the comparison means something. */
        jbi_geom_check_mesh(&md.mesh, "t", &extent, detail, sizeof detail);
        jbi_frame_size(jb, &w, &h);
        check(what, jbi_geom_begin(&d, &jb->api->scene, &model, w, h, jbi_frame_ot_size(jb),
                                   extent, "t", detail, sizeof detail),
              JB_OK);
        jbi_geom_load(&jb->api->scene.gte, &d);
        for (i = 0; i < BOX_F; i++)
            if (jbi_geom_face(&jb->api->scene.gte, &d, &md.mesh, i, p) == 1) {
                faces++;
                exact += p[0].exact;
            }

        check(what, jb_begin_frame(jb, JB_RGB(10, 20, 30)), JB_OK);
        check(what, jb_draw_mesh(jb, &md.mesh, &model), JB_OK);
        check(what, jb_end_frame(jb), JB_OK);
        a = copy_frame(jb, &w, &h);

        check(what, jb_begin_frame(jb, JB_RGB(10, 20, 30)), JB_OK);
        jb_ot_clear(ot);
        hand_load(jb, &d);
        for (i = 0; i < BOX_F; i++)
            hand_face(jb, ot, &d, &md.mesh, i);
        check(what, jb_ot_draw(jb, ot), JB_OK);
        check(what, jb_end_frame(jb), JB_OK);
        b = copy_frame(jb, &w, &h);
        if (memcmp(a, b, (size_t)w * (size_t)h * 4) != 0) {
            int k, n = 0;

            int x0 = w, y0 = h, x1 = -1, y1 = -1;

            for (k = 0; k < w * h; k++)
                if (memcmp(a + 4 * k, b + 4 * k, 4) != 0) {
                    n++;
                    x0 = k % w < x0 ? k % w : x0;
                    x1 = k % w > x1 ? k % w : x1;
                    y0 = k / w < y0 ? k / w : y0;
                    y1 = k / w > y1 ? k / w : y1;
                }
            if (getenv("JB_DEBUG"))
                printf("  box %d,%d..%d,%d\n", x0, y0, x1, y1);
            if (differ++ < 3)
                printf("  %s step %d: %d pixels differ\n", what, step, n);
        }
        free(a);
        free(b);
    }
    check(what, differ, 0);
    check(what, faces > 50 && exact == faces, 1);
    printf("%s: 24 frames equal, %d faces\n", what, faces);
    jb_ot_destroy(ot);
    jb_destroy(jb);
}

/* ------------------------------------------------------------------------ */
/* Coverage: no holes and no pixel drawn twice */

enum { GRID = 10, CELL = 160 };

/* A floor of GRID x GRID quads at y = 0 around the origin, and a wall of
   4 x 2 quads at z = -400 facing +z, x -320..320, y 0..320. */
typedef struct big_mesh {
    jb_gte_vec pos[(GRID + 1) * (GRID + 1) + 15];
    jb_mesh_face faces[GRID * GRID + 8];
    jb_material mat;
    jb_mesh mesh;
} big_mesh;

static void make_floor(big_mesh *m)
{
    int i, j, nv = 0, nf = 0, base;

    memset(m, 0, sizeof *m);
    for (j = 0; j <= GRID; j++)
        for (i = 0; i <= GRID; i++) {
            m->pos[nv].x = (int16_t)((i - GRID / 2) * CELL);
            m->pos[nv].y = 0;
            m->pos[nv].z = (int16_t)((j - GRID / 2) * CELL);
            nv++;
        }
    for (j = 0; j < GRID; j++)
        for (i = 0; i < GRID; i++) {
            jb_mesh_face *f = &m->faces[nf++];
            int a = j * (GRID + 1) + i;

            /* Anticlockwise seen from above (+y): -x,+z  +x,+z  +x,-z  -x,-z. */
            f->vertices = 4;
            f->v[0] = (uint16_t)(a + GRID + 1);
            f->v[1] = (uint16_t)(a + GRID + 2);
            f->v[2] = (uint16_t)(a + 1);
            f->v[3] = (uint16_t)a;
        }
    base = nv;
    for (j = 0; j <= 2; j++)
        for (i = 0; i <= 4; i++) {
            m->pos[nv].x = (int16_t)(-320 + i * 160);
            m->pos[nv].y = (int16_t)(j * 160);
            m->pos[nv].z = -400;
            nv++;
        }
    for (j = 0; j < 2; j++)
        for (i = 0; i < 4; i++) {
            jb_mesh_face *f = &m->faces[nf++];
            int a = base + j * 5 + i;

            f->vertices = 4;
            f->v[0] = (uint16_t)a;
            f->v[1] = (uint16_t)(a + 1);
            f->v[2] = (uint16_t)(a + 6);
            f->v[3] = (uint16_t)(a + 5);
        }
    m->mat.unlit = 1;
    m->mat.color = JB_RGB(64, 64, 64);
    m->mat.blend = JB_BLEND_ADD;
    m->mesh.vertex_count = nv;
    m->mesh.positions = m->pos;
    m->mesh.face_count = nf;
    m->mesh.faces = m->faces;
    m->mesh.material_count = 1;
    m->mesh.materials = &m->mat;
}

/* How many of the floor and the wall the ray through screen point (sx, sy)
   meets, in double, the camera at (0, ey, ez) looking down -z, H the
   projection distance, between the near and far planes. */
static int ray_at(double sx, double sy, int w, int hgt, double hh, double ey, double ez,
                  double zn, double zf)
{
    double dx = (sx - w * 0.5) / hh, dy = -(sy - hgt * 0.5) / hh;
    double half = GRID * CELL / 2.0;
    int hits = 0;

    if (dy < 0.0) { /* the floor, y = 0, at depth t */
        double t = -ey / dy, x = dx * t, z = ez - t;

        hits += x > -half && x < half && z > -half && z < half && t > zn && t < zf;
    }
    {
        double t = ez + 400.0, x = dx * t, y = ey + dy * t;

        hits += t > zn && t < zf && x > -320 && x < 320 && y > 0 && y < 320;
    }
    return hits;
}

/* The same for a pixel, or -1 when an outline passes within two pixels
   of its centre (rounding decides there). */
static int ray(int px, int py, int w, int hgt, double hh, double ey, double ez, double zn,
               double zf)
{
    double cx = px + 0.5, cy = py + 0.5;
    int want = ray_at(cx, cy, w, hgt, hh, ey, ez, zn, zf), i, j;

    for (j = -1; j <= 1; j++)
        for (i = -1; i <= 1; i++)
            if (ray_at(cx + 2.0 * i, cy + 2.0 * j, w, hgt, hh, ey, ez, zn, zf) != want)
                return -1;
    return want;
}

static void coverage(int mode)
{
    jb_context *jb = make(1, 1, 0, 0);
    static big_mesh m;
    jb_camera cam;
    int step, w, h, holes = 0, doubles = 0, checked = 0, x, y;
    const char *name = mode == JB_GEOMETRY_GTE ? "GTE" : "float";

    make_floor(&m);
    jb_set_geometry(jb, mode);
    jb_set_dither(jb, 0);
    for (step = 0; step < 40; step++) {
        double ez = 700.0 - step * 30.0, ey = 60.0;
        const uint8_t *px;
        double hh, zn, zf;
        jbi_geom_draw d;
        char detail[200];

        memset(&cam, 0, sizeof cam);
        cam.eye.x = 0.0f;
        cam.eye.y = (float)ey;
        cam.eye.z = (float)ez;
        cam.target.x = 0.0f;
        cam.target.y = (float)ey;
        cam.target.z = (float)ez - 100.0f;
        cam.z_near = 40.0f;
        cam.z_far = 3000.0f;
        check("walk camera", jb_set_camera(jb, &cam), JB_OK);
        check("walk frame", jb_begin_frame(jb, 0), JB_OK);
        check("walk mesh", jb_draw_mesh(jb, &m.mesh, NULL), JB_OK);
        check("walk end", jb_end_frame(jb), JB_OK);
        px = jb_frame(jb, &w, &h);
        jbi_geom_begin(&d, &jb->api->scene, NULL, w, h, 16, 0, "t", detail, sizeof detail);
        if (mode == JB_GEOMETRY_GTE) {
            hh = (double)d.r.h;
            zn = (double)d.near_z / d.scale;
        } else {
            hh = (double)d.fh;
            zn = 40.0;
        }
        zf = 3000.0;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                int want = ray(x, y, w, h, hh, ey, ez, zn, zf);
                int layers = (px[4 * (y * w + x)] + 33) / 66; /* 64 a layer: 66 in the frame */

                if (want < 0)
                    continue;
                checked++;
                if (getenv("JB_DEBUG") && layers != want && holes + doubles < 60)
                    printf("cov %s step %d (%d, %d) want %d got %d\n", name, step, x, y, want,
                           layers);
                holes += layers < want;
                doubles += layers > want;
            }
    }
    printf("walk (%s): %d pixels checked over 40 frames, %d holes, %d drawn twice\n", name,
           checked, holes, doubles);
    check(name, holes, 0);
    check(name, doubles, 0);
    jb_destroy(jb);
}

/* ------------------------------------------------------------------------ */
/* Culling, ordering */

static int count_pixels(jb_context *jb, int *twice)
{
    int w, h, i, n = 0;
    const uint8_t *px = jb_frame(jb, &w, &h);

    *twice = 0;
    for (i = 0; i < w * h; i++) {
        n += px[4 * i] != 0;
        *twice += px[4 * i] > 100;
    }
    return n;
}

static void culling(int mode)
{
    jb_context *jb = make(1, 1, 0, 0);
    static mesh_data md;
    jb_camera cam;
    jb_mat4 model = jb_mat4_trs((jb_vec3){0, 0, -400}, (jb_vec3){0.5f, 0.7f, 0.1f},
                                (jb_vec3){1, 1, 1});
    int n, twice, i;

    jb_set_geometry(jb, mode);
    jb_set_dither(jb, 0);
    box(&md, 80, NULL);
    for (i = 0; i < 2; i++) {
        md.mats[i].unlit = 1;
        md.mats[i].color = JB_RGB(64, 64, 64);
        md.mats[i].blend = JB_BLEND_ADD;
    }
    jb_begin_frame(jb, 0);
    jb_draw_mesh(jb, &md.mesh, &model);
    check("cube end", jb_end_frame(jb), JB_OK);
    n = count_pixels(jb, &twice);
    check("cube from outside: drawn", n > 2000, 1);
    check("cube from outside: no pixel twice", twice, 0);
    /* Double-sided: the back faces too, so most pixels twice. */
    md.mats[0].double_sided = md.mats[1].double_sided = 1;
    jb_begin_frame(jb, 0);
    jb_draw_mesh(jb, &md.mesh, &model);
    jb_end_frame(jb);
    count_pixels(jb, &twice);
    check("double-sided: pixels twice", twice > n / 2, 1);
    md.mats[0].double_sided = md.mats[1].double_sided = 0;
    /* From inside the cube (scaled up around the camera): nothing. */
    model = jb_mat4_scale(10, 10, 10);
    memset(&cam, 0, sizeof cam);
    cam.target.z = -1.0f;
    cam.z_near = 10.0f;
    jb_set_camera(jb, &cam);
    jb_begin_frame(jb, 0);
    check("inside", jb_draw_mesh(jb, &md.mesh, &model), JB_OK);
    jb_end_frame(jb);
    check("cube from inside: nothing", count_pixels(jb, &twice), 0);
    jb_destroy(jb);
}

static void ordering(int mode)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_gte_vec pos[8] = {{-50, -50, 0}, {50, -50, 0}, {50, 50, 0}, {-50, 50, 0},
                         {-50, -50, 0}, {50, -50, 0}, {50, 50, 0}, {-50, 50, 0}};
    jb_mesh_face f = {{0, 1, 2, 3}, 4, 0};
    jb_material mat;
    jb_mesh m;
    jb_mat4 near = jb_mat4_translate(0, 0, -300), far = jb_mat4_translate(20, 10, -900);
    int w, h;
    const uint8_t *px;

    jb_set_geometry(jb, mode);
    memset(&mat, 0, sizeof mat);
    memset(&m, 0, sizeof m);
    mat.unlit = 1;
    m.vertex_count = 4;
    m.positions = pos;
    m.face_count = 1;
    m.faces = &f;
    m.material_count = 1;
    m.materials = &mat;
    jb_begin_frame(jb, 0);
    mat.color = JB_RGB(255, 0, 0);
    jb_draw_mesh(jb, &m, &near); /* the near one first */
    mat.color = JB_RGB(0, 0, 255);
    jb_draw_mesh(jb, &m, &far);
    check("ordering end", jb_end_frame(jb), JB_OK);
    px = jb_frame(jb, &w, &h);
    check("near face over the far one", px[4 * (h / 2 * w + w / 2)] > 200 &&
                                            px[4 * (h / 2 * w + w / 2) + 2] == 0,
          1);
    jb_destroy(jb);
}

/* Through the frame: a quad at depth 2000 drawn first and
   a triangle behind it at 2001; the triangle must not sort nearer (it used
   to, its AVSZ3 scale being steeper than the quad's AVSZ4). */
static void tri_behind_quad(int mode)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_gte_vec quad[4] = {{-400, -400, 0}, {400, -400, 0}, {400, 400, 0}, {-400, 400, 0}};
    jb_gte_vec tri[3] = {{-400, -400, 0}, {400, -400, 0}, {0, 400, 0}};
    jb_mesh_face fq = {{0, 1, 2, 3}, 4, 0}, ft = {{0, 1, 2, 0}, 3, 0};
    jb_material mat;
    jb_mesh mq, mt;
    jb_mat4 at_q = jb_mat4_translate(0, 0, -2000), at_t = jb_mat4_translate(0, 0, -2001);
    const uint8_t *px;
    int w, h;
    char what[128];

    jb_set_geometry(jb, mode);
    memset(&mat, 0, sizeof mat);
    mat.unlit = 1;
    memset(&mq, 0, sizeof mq);
    mq.vertex_count = 4;
    mq.positions = quad;
    mq.face_count = 1;
    mq.faces = &fq;
    mq.material_count = 1;
    mq.materials = &mat;
    mt = mq;
    mt.vertex_count = 3;
    mt.positions = tri;
    mt.faces = &ft;
    jb_begin_frame(jb, 0);
    mat.color = JB_RGB(255, 0, 0);
    jb_draw_mesh(jb, &mq, &at_q);
    mat.color = JB_RGB(0, 0, 255);
    jb_draw_mesh(jb, &mt, &at_t);
    jb_end_frame(jb);
    px = jb_frame(jb, &w, &h);
    snprintf(what, sizeof what, "tri behind quad: (%s): the nearer quad over the farther triangle",
             mode == JB_GEOMETRY_GTE ? "GTE" : "float");
    check(what, px[4 * (h / 2 * w + w / 2)] > 200 && px[4 * (h / 2 * w + w / 2) + 2] == 0, 1);
    jb_destroy(jb);
}

/* ------------------------------------------------------------------------ */
/* Billboards and lines */

static void billboards(int mode)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_texture tex;
    jb_billboard b;
    uint16_t texels[256];
    int w, h, x, y, found = 0, i, j;
    const uint8_t *px;

    jb_set_geometry(jb, mode);
    make_texture(jb, &tex);
    jb_vram_read(jb, tex.page_x, tex.page_y, 16, 16, texels);
    memset(&b, 0, sizeof b);
    b.texture = &tex;
    /* At 208 world units from the default camera a world unit is a pixel
       (H 208): 16 units wide shows the 16 texels one to a pixel. */
    b.position.z = -208.0f;
    b.width = 16.0f;
    jb_begin_frame(jb, 0);
    check("billboard 1:1", jb_draw_billboard(jb, &b), JB_OK);
    jb_end_frame(jb);
    px = jb_frame(jb, &w, &h);
    for (y = 0; y + 16 <= h && !found; y++)
        for (x = 0; x + 16 <= w && !found; x++) {
            int same = 1;

            for (j = 0; j < 16 && same; j++)
                for (i = 0; i < 16 && same; i++) {
                    const uint8_t *p = px + 4 * ((y + j) * w + x + i);
                    unsigned t = texels[j * 16 + i];

                    same = p[0] == widen5(t & 31) && p[1] == widen5((t >> 5) & 31) &&
                           p[2] == widen5((t >> 10) & 31);
                }
            if (same) {
                found = 1;
                check("billboard 1:1 centred", iabs(x + 8 - w / 2) <= 1 && iabs(y + 8 - h / 2) <= 1,
                      1);
            }
        }
    check("billboard 1:1: the texture exactly", found, 1);
    /* Twice as wide: a quad, drawn over about 32 x 32 pixels. */
    b.width = 32.0f;
    jb_begin_frame(jb, 0);
    check("billboard scaled", jb_draw_billboard(jb, &b), JB_OK);
    jb_end_frame(jb);
    {
        int n, twice;

        n = count_pixels(jb, &twice);
        check("billboard scaled: about 32 x 32", n >= 30 * 30 && n <= 34 * 34, 1);
    }
    b.position.z = 100.0f;
    jb_begin_frame(jb, 0);
    check("billboard behind", jb_draw_billboard(jb, &b), JB_OK);
    jb_end_frame(jb);
    {
        int twice;

        check("billboard behind: nothing", count_pixels(jb, &twice), 0);
    }
    /* Untextured and fogged: the tint moves to the fog colour. */
    /* So wide its size on screen overflows: drawn or not, with no
       conversion of an infinity to an integer (which the sanitizer build
       reports). */
    b.position.z = -300.0f;
    b.width = 3e38f;
    jb_begin_frame(jb, 0);
    check("billboard wider than the float range", jb_draw_billboard(jb, &b), JB_OK);
    jb_end_frame(jb);
    b.texture = NULL;
    b.position.z = -2000.0f;
    b.width = 200.0f;
    b.tint = JB_RGB(255, 0, 0);
    jb_set_fog(jb, JB_RGB(0, 0, 255), 100.0f, 1000.0f);
    jb_begin_frame(jb, 0);
    check("billboard fogged", jb_draw_billboard(jb, &b), JB_OK);
    jb_end_frame(jb);
    px = jb_frame(jb, &w, &h);
    check("billboard fogged: the fog colour",
          px[4 * (h / 2 * w + w / 2)] < 16 && px[4 * (h / 2 * w + w / 2) + 2] > 200, 1);
    jb_destroy(jb);
}

static void lines(int mode)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_line3d l;
    int twice;

    jb_set_geometry(jb, mode);
    memset(&l, 0, sizeof l);
    l.a.x = -100.0f;
    l.a.y = -20.0f;
    l.a.z = -500.0f;
    l.b.x = 30.0f;
    l.b.y = -20.0f;
    l.b.z = 200.0f; /* behind the camera */
    l.color_a = JB_RGB(255, 255, 255);
    l.color_b = JB_RGB(255, 0, 0);
    jb_begin_frame(jb, 0);
    check("line3d through the near plane", jb_draw_line3d(jb, &l), JB_OK);
    jb_end_frame(jb);
    check("line3d: drawn", count_pixels(jb, &twice) > 20, 1);
    l.a.z = 50.0f;
    jb_begin_frame(jb, 0);
    check("line3d behind", jb_draw_line3d(jb, &l), JB_OK);
    jb_end_frame(jb);
    check("line3d behind: nothing", count_pixels(jb, &twice), 0);
    jb_destroy(jb);
}

/* ------------------------------------------------------------------------ */
/* Errors */

static void errors(void)
{
    jb_context *jb = make(1, 1, 0, 0);
    static mesh_data md;
    jb_billboard b;
    jb_line3d l;
    jb_texture bad;
    jb_mat4 huge = jb_mat4_scale(1e6f, 1e6f, 1e6f), inf;
    const char *detail;
    jb_config c;
    jb_context *small;

    box(&md, 60, NULL);
    memset(&b, 0, sizeof b);
    b.width = 10.0f;
    b.position.z = -300.0f;
    memset(&l, 0, sizeof l);
    l.a.z = -100.0f;
    l.b.z = -200.0f;

    check("mesh NULL jb", jb_draw_mesh(NULL, &md.mesh, NULL), JB_ERROR_INVALID_ARGUMENT);
    check("billboard NULL jb", jb_draw_billboard(NULL, &b), JB_ERROR_INVALID_ARGUMENT);
    check("line3d NULL jb", jb_draw_line3d(NULL, &l), JB_ERROR_INVALID_ARGUMENT);
    check("mesh outside a frame", jb_draw_mesh(jb, &md.mesh, NULL), JB_ERROR_INVALID_ARGUMENT);
    check_str("mesh outside a frame: detail", jb_error_string(jb),
              "jb_draw_mesh: no frame begun (call jb_begin_frame first)");
    check("outside a frame: latched", jb_check(jb, &detail), JB_ERROR_INVALID_ARGUMENT);
    check_str("latched detail", detail, "jb_draw_mesh: no frame begun (call jb_begin_frame first)");
    check("billboard outside a frame", jb_draw_billboard(jb, &b), JB_ERROR_INVALID_ARGUMENT);
    check("line3d outside a frame", jb_draw_line3d(jb, &l), JB_ERROR_INVALID_ARGUMENT);
    jb_check(jb, NULL);

    jb_begin_frame(jb, 0);
    check("mesh NULL", jb_draw_mesh(jb, NULL, NULL), JB_ERROR_INVALID_ARGUMENT);
    check_str("mesh NULL: detail", jb_error_string(jb), "jb_draw_mesh: mesh is NULL");
    md.faces[2].v[1] = 99;
    check("mesh bad index", jb_draw_mesh(jb, &md.mesh, NULL), JB_ERROR_INVALID_ARGUMENT);
    check_str("mesh bad index: detail", jb_error_string(jb),
              "jb_draw_mesh: faces[2].v[1] is 99, but there are 24 vertices");
    md.faces[2].v[1] = 9;
    memset(&bad, 0, sizeof bad);
    bad.page_x = 30;
    bad.depth = JB_TEX_15BIT;
    md.mats[0].texture = &bad;
    check("mesh bad texture", jb_draw_mesh(jb, &md.mesh, NULL), JB_ERROR_INVALID_ARGUMENT);
    md.mats[0].texture = NULL;
    md.mats[1].blend = 7;
    check("mesh bad blend", jb_draw_mesh(jb, &md.mesh, NULL), JB_ERROR_INVALID_ARGUMENT);
    md.mats[1].blend = 0;
    check("mesh too large for the GTE", jb_draw_mesh(jb, &md.mesh, &huge),
          JB_ERROR_INVALID_ARGUMENT);
    inf = jb_mat4_identity();
    inf.m[12] = 1e30f * 1e30f;
    check("mesh not finite", jb_draw_mesh(jb, &md.mesh, &inf), JB_ERROR_INVALID_ARGUMENT);
    check_str("mesh not finite: detail", jb_error_string(jb),
              "jb_draw_mesh: model x view is not finite (or too large)");
    check("billboard NULL", jb_draw_billboard(jb, NULL), JB_ERROR_INVALID_ARGUMENT);
    b.width = 0.0f;
    check("billboard width 0", jb_draw_billboard(jb, &b), JB_ERROR_INVALID_ARGUMENT);
    b.width = 10.0f;
    b.height = -1.0f;
    check("billboard height negative", jb_draw_billboard(jb, &b), JB_ERROR_INVALID_ARGUMENT);
    b.height = 0.0f;
    b.tint = 0x1000000u;
    check("billboard tint", jb_draw_billboard(jb, &b), JB_ERROR_INVALID_ARGUMENT);
    b.tint = 0;
    b.blend = -1;
    check("billboard blend", jb_draw_billboard(jb, &b), JB_ERROR_INVALID_ARGUMENT);
    b.blend = 0;
    {
        jb_texture t;

        make_texture(jb, &t);
        b.texture = &t;
        b.u = 10;
        b.tw = 7;
        check("billboard texels outside", jb_draw_billboard(jb, &b), JB_ERROR_INVALID_ARGUMENT);
        check_str("billboard texels: detail", jb_error_string(jb),
                  "jb_draw_billboard: texels (10, 0) 7 x 0 are not within the texture's 16 x 16");
        b.texture = NULL;
        b.u = b.tw = 0;
    }
    check("line3d NULL", jb_draw_line3d(jb, NULL), JB_ERROR_INVALID_ARGUMENT);
    l.color_b = 0x1000000u;
    check("line3d colour", jb_draw_line3d(jb, &l), JB_ERROR_INVALID_ARGUMENT);
    l.color_b = 0;
    l.blend = 5;
    check("line3d blend", jb_draw_line3d(jb, &l), JB_ERROR_INVALID_ARGUMENT);
    l.blend = 0;
    l.a.x = 1e30f * 1e30f;
    check("line3d not finite", jb_draw_line3d(jb, &l), JB_ERROR_INVALID_ARGUMENT);
    l.a.x = 0.0f;
    check("good draws still fine", jb_draw_mesh(jb, &md.mesh, NULL) | jb_draw_billboard(jb, &b) |
                                       jb_draw_line3d(jb, &l),
          JB_OK);
    check("end returns the first failure", jb_end_frame(jb), JB_ERROR_INVALID_ARGUMENT);
    jb_destroy(jb);

    /* A full frame. */
    memset(&c, 0, sizeof c);
    c.max_primitives = 1;
    small = must_create(&c);
    {
        jb_mat4 model = jb_mat4_trs((jb_vec3){0, 0, -400}, (jb_vec3){0.5f, 0.7f, 0.1f},
                                    (jb_vec3){1, 1, 1});

        jb_begin_frame(small, 0);
        check("frame full", jb_draw_mesh(small, &md.mesh, &model), JB_ERROR_OUT_OF_MEMORY);
        check("frame full: end", jb_end_frame(small), JB_ERROR_OUT_OF_MEMORY);
    }
    jb_destroy(small);
}

/* ------------------------------------------------------------------------ */
/* Edge cases through the public API, each of which failed before its
   fix, and the per-draw depth bias. */

static jb_mesh one_tri(jb_gte_vec *pos, jb_mesh_face *f, jb_material *mat, uint32_t color)
{
    jb_mesh m;

    memset(&m, 0, sizeof m);
    memset(mat, 0, sizeof *mat);
    mat->unlit = 1;
    mat->double_sided = 1;
    mat->color = color;
    f->v[0] = 0;
    f->v[1] = 1;
    f->v[2] = 2;
    f->v[3] = 0;
    f->vertices = 3;
    f->material = 0;
    m.vertex_count = 3;
    m.positions = pos;
    m.face_count = 1;
    m.faces = f;
    m.material_count = 1;
    m.materials = mat;
    return m;
}

/* A pixel of a frame read back once (jb_frame converts the whole frame
   at each call, so loops over pixels read it once and use this). */
static const uint8_t *frame_px(const uint8_t *px, int w, int x, int y)
{
    return px + 4 * (y * w + x);
}

static const uint8_t *pixel(jb_context *jb, int x, int y)
{
    int w, h;
    const uint8_t *px = jb_frame(jb, &w, &h);

    return px + 4 * (y * w + x);
}

static int is_red(const uint8_t *p)
{
    return p[0] > 200 && p[2] < 40;
}

static int is_blue(const uint8_t *p)
{
    return p[2] > 200 && p[0] < 40;
}

static void billboard_field_limits(void)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_texture tex, hand;
    jb_billboard b;
    static mesh_data md;

    make_texture(jb, &tex);
    memset(&b, 0, sizeof b);
    b.texture = &tex;
    b.position.z = -300.0f;
    b.width = 10.0f;
    jb_begin_frame(jb, 0);
    b.u = 1;
    b.tw = 2147483647;
    check("billboard field limits: tw INT_MAX refused", jb_draw_billboard(jb, &b),
          JB_ERROR_INVALID_ARGUMENT);
    b.tw = 0;
    b.v = 1;
    b.th = 2147483647;
    check("billboard field limits: th INT_MAX refused", jb_draw_billboard(jb, &b),
          JB_ERROR_INVALID_ARGUMENT);
    b.v = b.th = b.u = 0;
    /* Friendly texture fields a hand-made texture could get wrong. */
    memset(&hand, 0, sizeof hand);
    hand.page_x = 64;
    hand.depth = JB_TEX_15BIT;
    hand.u = -2147483647 - 1;
    b.texture = &hand;
    check("billboard field limits: u INT_MIN refused", jb_draw_billboard(jb, &b),
          JB_ERROR_INVALID_ARGUMENT);
    hand.u = -1;
    check("billboard field limits: u -1 refused", jb_draw_billboard(jb, &b),
          JB_ERROR_INVALID_ARGUMENT);
    hand.u = 200;
    hand.width = 100;
    check("billboard field limits: past the page's edge refused", jb_draw_billboard(jb, &b),
          JB_ERROR_INVALID_ARGUMENT);
    hand.u = 0;
    hand.width = 24;
    hand.repeat = 1;
    check("billboard field limits: repeat not a power of two refused", jb_draw_billboard(jb, &b),
          JB_ERROR_INVALID_ARGUMENT);
    hand.width = 0;
    hand.repeat = 0;
    check("billboard field limits: hand-made zeros accepted", jb_draw_billboard(jb, &b), JB_OK);
    box(&md, 60, &hand);
    hand.u = 2147483647;
    check("billboard field limits: mesh texture u INT_MAX refused",
          jb_draw_mesh(jb, &md.mesh, NULL),
          JB_ERROR_INVALID_ARGUMENT);
    jb_end_frame(jb);
    jb_destroy(jb);
}

static void billboard_depth_order(void)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_billboard b;
    jb_line3d l;
    jb_gte_vec pos[3] = {{-10, -10, 0}, {10, -10, 0}, {0, 10, 0}};
    jb_mesh_face f;
    jb_material mat;
    jb_mesh m = one_tri(pos, &f, &mat, JB_RGB(0, 0, 255));
    jb_mat4 at = jb_mat4_translate(0.0f, 0.0f, -35.95f);

    /* Float path: a blue triangle at depth 35.95, then a red billboard
       just in front at 35.9. Both have the same integer depth (143 GTE
       units), so call order decides and the billboard, drawn later, shows;
       it must not sort farther than the triangle (its depth used to be
       rounded up, to 144, past the triangle's 143). */
    jb_set_geometry(jb, JB_GEOMETRY_FLOAT);
    memset(&b, 0, sizeof b);
    b.position.z = -35.9f;
    b.width = 4.0f;
    b.tint = JB_RGB(255, 0, 0);
    jb_begin_frame(jb, 0);
    jb_draw_mesh(jb, &m, &at);
    jb_draw_billboard(jb, &b);
    jb_end_frame(jb);
    check("billboard depth order: the nearer billboard over the farther triangle",
          is_red(pixel(jb, 160, 120)),
          1);
    /* The same for a line with both ends at 35.9. */
    memset(&l, 0, sizeof l);
    l.a.x = -2.0f;
    l.a.z = -35.9f;
    l.b.x = 2.0f;
    l.b.z = -35.9f;
    l.color_a = l.color_b = JB_RGB(255, 0, 0);
    jb_begin_frame(jb, 0);
    jb_draw_mesh(jb, &m, &at);
    jb_draw_line3d(jb, &l);
    jb_end_frame(jb);
    check("billboard depth order: the nearer line over the farther triangle",
          is_red(pixel(jb, 160, 120)), 1);
    jb_destroy(jb);
}

static void wide_line_both_sides(int mode)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_line3d l;
    char what[128];

    jb_set_geometry(jb, mode);
    memset(&l, 0, sizeof l);
    l.a.x = -1e20f;
    l.a.z = -100.0f;
    l.b.x = 1e20f;
    l.b.z = -100.0f;
    l.color_a = l.color_b = JB_RGB(255, 0, 0);
    jb_begin_frame(jb, 0);
    jb_draw_line3d(jb, &l);
    jb_end_frame(jb);
    snprintf(what, sizeof what,
             "wide line both sides: (%s): a wide line drawn left and right of the centre",
             mode == JB_GEOMETRY_GTE ? "GTE" : "float");
    check(what, is_red(pixel(jb, 80, 120)) && is_red(pixel(jb, 240, 120)), 1);
    jb_destroy(jb);
}

static void huge_billboard_and_crop(int mode)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_billboard b;
    jb_texture tex;
    jb_texture_desc d;
    uint16_t texels[64];
    int i, red = 1;
    char what[128];
    static const int at[4][2] = {{157, 117}, {160, 117}, {157, 120}, {160, 120}};

    jb_set_geometry(jb, mode);
    /* A billboard far larger than the screen still shows its middle. */
    memset(&b, 0, sizeof b);
    b.position.z = -100.0f;
    b.width = 1e6f;
    b.tint = JB_RGB(255, 0, 0);
    jb_begin_frame(jb, 0);
    check("huge billboard and crop: huge billboard", jb_draw_billboard(jb, &b), JB_OK);
    jb_end_frame(jb);
    snprintf(what, sizeof what, "huge billboard and crop: (%s): a huge billboard covers the screen",
             mode == JB_GEOMETRY_GTE ? "GTE" : "float");
    check(what, is_red(pixel(jb, 160, 120)) && is_red(pixel(jb, 5, 5)), 1);
    /* An 8 x 8 green texture with a red texel at (3, 3), cropped to that
       texel and magnified: red throughout, never the green beside it. */
    for (i = 0; i < 64; i++)
        texels[i] = i == 3 * 8 + 3 ? JB_RGB15(31, 0, 0) : JB_RGB15(0, 31, 0);
    memset(&d, 0, sizeof d);
    d.width = d.height = 8;
    d.depth = JB_TEX_15BIT;
    d.texels = texels;
    jb_texture_create(jb, &d, &tex);
    memset(&b, 0, sizeof b);
    b.texture = &tex;
    b.raw = 1;
    b.position.z = -100.0f;
    b.width = b.height = 8.0f / 2.08f;
    b.u = b.v = 3;
    b.tw = b.th = 1;
    jb_begin_frame(jb, 0);
    jb_draw_billboard(jb, &b);
    jb_end_frame(jb);
    for (i = 0; i < 4; i++)
        red &= is_red(pixel(jb, at[i][0], at[i][1]));
    snprintf(what, sizeof what,
             "huge billboard and crop: (%s): a one-texel crop samples only that texel",
             mode == JB_GEOMETRY_GTE ? "GTE" : "float");
    check(what, red, 1);
    jb_destroy(jb);
}

static void near_plane_one(void)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_camera c;
    jb_gte_vec pos[3] = {{-10, -10, 0}, {10, -10, 0}, {0, 10, 0}};
    jb_mesh_face f1, f2;
    jb_material blue, red;
    jb_mesh mb = one_tri(pos, &f1, &blue, JB_RGB(0, 0, 255));
    jb_mesh mr = one_tri(pos, &f2, &red, JB_RGB(255, 0, 0));
    jb_mat4 far = jb_mat4_translate(0, 0, -40.0f), near = jb_mat4_translate(0, 0, -35.0f);

    /* z_near 1: the GTE path clips nearer than H / 2 + 1 anyway; both
       paths order over the same depths, so switching between them within
       a frame keeps the nearer (red, float path) over the farther (blue,
       GTE path). */
    memset(&c, 0, sizeof c);
    c.target.z = -1.0f;
    c.z_near = 1.0f;
    jb_set_camera(jb, &c);
    jb_begin_frame(jb, 0);
    jb_set_geometry(jb, JB_GEOMETRY_GTE);
    jb_draw_mesh(jb, &mb, &far);
    jb_set_geometry(jb, JB_GEOMETRY_FLOAT);
    jb_draw_mesh(jb, &mr, &near);
    jb_end_frame(jb);
    check("near plane one: paths switched in a frame keep the order", is_red(pixel(jb, 160, 120)),
          1);
    jb_destroy(jb);
}

static void tiny_field_of_view(void)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_camera c;
    jb_gte_vec pos[3] = {{-10, -10, -100}, {10, -10, -100}, {0, 10, -100}};
    jb_mesh_face f;
    jb_material mat;
    jb_mesh m = one_tri(pos, &f, &mat, JB_RGB(255, 0, 0));
    jb_mat4 s = jb_mat4_scale(1e-36f / 1024.0f, 1e-36f / 1024.0f, 1.0f);

    jb_set_geometry(jb, JB_GEOMETRY_FLOAT);
    memset(&c, 0, sizeof c);
    c.target.z = -1.0f;
    c.fov_y = 1e-36f;
    check("tiny field of view: narrow camera", jb_set_camera(jb, &c), JB_OK);
    jb_begin_frame(jb, 0);
    check("tiny field of view: draw", jb_draw_mesh(jb, &m, &s), JB_OK);
    jb_end_frame(jb);
    check("tiny field of view: a 1e-36 degree camera draws", is_red(pixel(jb, 160, 120)), 1);
    jb_destroy(jb);
}

static void depth_bias(int mode)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_gte_vec pos[3] = {{-40, -40, 0}, {40, -40, 0}, {0, 40, 0}};
    jb_mesh_face f1, f2;
    jb_material floor_mat, decal_mat;
    jb_mesh floor_m = one_tri(pos, &f1, &floor_mat, JB_RGB(255, 0, 0));
    jb_mesh decal = one_tri(pos, &f2, &decal_mat, JB_RGB(0, 0, 255));
    jb_mat4 at = jb_mat4_translate(0, 0, -500.0f);
    jb_mesh_draw d;
    char what[128];

    /* A decal at the same depth as the surface it lies on, drawn first:
       without a bias it shares the surface's bucket and the surface, drawn
       after it, covers it; biased nearer it sorts after and shows. */
    jb_set_geometry(jb, mode);
    memset(&d, 0, sizeof d);
    d.mesh = &decal;
    d.model = &at;
    jb_begin_frame(jb, 0);
    check("depth bias 0", jb_draw_mesh_ex(jb, &d), JB_OK);
    jb_draw_mesh(jb, &floor_m, &at);
    jb_end_frame(jb);
    snprintf(what, sizeof what, "no bias: the surface over the decal (%s)",
             mode == JB_GEOMETRY_GTE ? "GTE" : "float");
    check(what, is_red(pixel(jb, 160, 120)), 1);
    d.depth_bias = -10.0f;
    jb_begin_frame(jb, 0);
    check("depth bias -10", jb_draw_mesh_ex(jb, &d), JB_OK);
    jb_draw_mesh(jb, &floor_m, &at);
    jb_end_frame(jb);
    snprintf(what, sizeof what, "biased nearer: the decal over the surface (%s)",
             mode == JB_GEOMETRY_GTE ? "GTE" : "float");
    check(what, is_blue(pixel(jb, 160, 120)), 1);
    /* Errors. */
    jb_begin_frame(jb, 0);
    check("mesh_ex NULL", jb_draw_mesh_ex(jb, NULL), JB_ERROR_INVALID_ARGUMENT);
    check_str("mesh_ex NULL: detail", jb_error_string(jb), "jb_draw_mesh_ex: draw is NULL");
    d.depth_bias = 1e30f * 1e30f;
    check("mesh_ex bias not finite", jb_draw_mesh_ex(jb, &d), JB_ERROR_INVALID_ARGUMENT);
    d.depth_bias = 0.0f;
    d.tint = 0x1000000u;
    check("mesh_ex tint", jb_draw_mesh_ex(jb, &d), JB_ERROR_INVALID_ARGUMENT);
    d.tint = 0;
    d.mesh = NULL;
    check("mesh_ex mesh NULL", jb_draw_mesh_ex(jb, &d), JB_ERROR_INVALID_ARGUMENT);
    check_str("mesh_ex mesh NULL: detail", jb_error_string(jb), "jb_draw_mesh_ex: mesh is NULL");
    jb_end_frame(jb);
    jb_destroy(jb);
}

/* Through the frame: a triangle the guard band cuts,
   (-2000, 0, -100) u 0 to (0, 0, -1000) u 255, textured from a 256 x 1
   strip that is red below u 210 and blue from it. At the frame's left
   edge (0, 122) the affine u is about 245 (blue) and the
   perspective-correct one about 182 (red): each option must show its
   own, so both attribute sets reach the splitter. */
static void guard_cut_options(int mode)
{
    uint16_t strip[256];
    jb_texture_desc td;
    jb_gte_vec pos[3] = {{-2000, 0, -100}, {0, 0, -1000}, {0, -200, -1000}};
    uint8_t uv[6] = {0, 0, 255, 0, 255, 0};
    jb_mesh_face f;
    jb_material mat;
    jb_mesh m;
    int k, i;

    for (i = 0; i < 256; i++)
        strip[i] = i < 210 ? JB_RGB15(31, 0, 0) : JB_RGB15(0, 0, 31);
    for (k = 0; k < 2; k++) {
        jb_context *jb = make(1, 1, 0, k);
        jb_texture tex;
        char what[96];

        jb_set_geometry(jb, mode);
        memset(&td, 0, sizeof td);
        td.width = 256;
        td.height = 1;
        td.depth = JB_TEX_15BIT;
        td.texels = strip;
        check("guard cut options: strip", jb_texture_create(jb, &td, &tex), JB_OK);
        m = one_tri(pos, &f, &mat, 0);
        mat.texture = &tex;
        mat.raw = 1;
        m.uv = uv;
        jb_begin_frame(jb, 0);
        check("guard cut options: draw", jb_draw_mesh(jb, &m, NULL), JB_OK);
        jb_end_frame(jb);
        snprintf(what, sizeof what,
                 "guard cut options: (%s, %s): the option's own u at the guard cut",
                 mode == JB_GEOMETRY_GTE ? "GTE" : "float", k ? "perspective" : "affine");
        if (!check(what, k ? is_red(pixel(jb, 0, 122)) : is_blue(pixel(jb, 0, 122)), 1)) {
            const uint8_t *p = pixel(jb, 0, 122);

            printf("  %d %d %d\n", p[0], p[1], p[2]);
        }
        jb_destroy(jb);
    }
}

/* JB_TINT_BLACK: black where 0 is JB_NEUTRAL, in a material's colour, a
   mesh draw's tint and a billboard's tint. */
static int is_black(const uint8_t *p)
{
    return p[0] == 0 && p[1] == 0 && p[2] == 0;
}

static void tint_black(int mode)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_gte_vec pos[3] = {{-40, -40, 0}, {40, -40, 0}, {0, 40, 0}};
    jb_mat4 at = jb_mat4_translate(0, 0, -500.0f);
    const uint32_t grey = JB_RGB(200, 200, 200);
    jb_mesh_face f;
    jb_material mat;
    jb_mesh m = one_tri(pos, &f, &mat, JB_TINT_BLACK);
    jb_mesh_draw d;
    jb_billboard b;
    jb_texture tex;
    const char *name = mode == JB_GEOMETRY_GTE ? "GTE" : "float";
    char what[80];

    jb_set_geometry(jb, mode);
    make_texture(jb, &tex);
    jb_begin_frame(jb, grey);
    check("tint black: material", jb_draw_mesh(jb, &m, &at), JB_OK);
    jb_end_frame(jb);
    snprintf(what, sizeof what, "tint black: a material of JB_TINT_BLACK is black (%s)", name);
    check(what, is_black(pixel(jb, 160, 120)), 1);
    mat.color = JB_RGB(255, 0, 0);
    memset(&d, 0, sizeof d);
    d.mesh = &m;
    d.model = &at;
    d.tint = JB_TINT_BLACK;
    jb_begin_frame(jb, grey);
    check("tint black: mesh tint", jb_draw_mesh_ex(jb, &d), JB_OK);
    jb_end_frame(jb);
    snprintf(what, sizeof what, "tint black: a mesh tinted JB_TINT_BLACK is black (%s)", name);
    check(what, is_black(pixel(jb, 160, 120)), 1);
    d.tint = 0;
    jb_begin_frame(jb, grey);
    jb_draw_mesh_ex(jb, &d);
    jb_end_frame(jb);
    snprintf(what, sizeof what, "tint black: tint 0 leaves it red (%s)", name);
    check(what, is_red(pixel(jb, 160, 120)), 1);
    memset(&b, 0, sizeof b);
    b.texture = &tex;
    b.position.z = -208.0f;
    b.width = 16.0f;
    b.tint = JB_TINT_BLACK;
    jb_begin_frame(jb, grey);
    check("tint black: billboard", jb_draw_billboard(jb, &b), JB_OK);
    jb_end_frame(jb);
    snprintf(what, sizeof what, "tint black: a billboard tinted JB_TINT_BLACK is black (%s)",
             name);
    check(what, is_black(pixel(jb, 160, 120)), 1);
    check("tint black: the clear around it", is_black(pixel(jb, 100, 120)), 0);
    check("tint black: no error", jb_check(jb, NULL), JB_OK);
    jb_destroy(jb);
}

/* A mesh's u, v outside a (non-repeat) texture would sample whatever lies
   beside it in VRAM: refused, naming the face. */
static void mesh_uv_range(void)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_gte_vec pos[3] = {{-40, -40, 0}, {40, -40, 0}, {0, 40, 0}};
    uint8_t uv[6] = {0, 0, 15, 0, 0, 15};
    jb_mat4 at = jb_mat4_translate(0, 0, -500.0f);
    jb_mesh_face f;
    jb_material mat;
    jb_mesh m = one_tri(pos, &f, &mat, 0);
    jb_texture tex;

    make_texture(jb, &tex);
    mat.texture = &tex;
    m.uv = uv;
    jb_begin_frame(jb, 0);
    check("mesh uv: the texture's last texel", jb_draw_mesh(jb, &m, &at), JB_OK);
    uv[2] = 16;
    check("mesh uv: past the texture", jb_draw_mesh(jb, &m, &at), JB_ERROR_INVALID_ARGUMENT);
    check_str("mesh uv: detail", jb_error_string(jb),
              "jb_draw_mesh: faces[0].v[1] (vertex 1) has u, v (16, 0), outside its 16 x 16 "
              "texture");
    uv[2] = 15;
    uv[5] = 16;
    check("mesh uv: v past the texture", jb_draw_mesh(jb, &m, &at), JB_ERROR_INVALID_ARGUMENT);
    jb_end_frame(jb);
    jb_destroy(jb);
}

static void api_edge_cases(void)
{
    mesh_uv_range();
    tint_black(JB_GEOMETRY_GTE);
    tint_black(JB_GEOMETRY_FLOAT);
    billboard_field_limits();
    billboard_depth_order();
    wide_line_both_sides(JB_GEOMETRY_GTE);
    wide_line_both_sides(JB_GEOMETRY_FLOAT);
    huge_billboard_and_crop(JB_GEOMETRY_GTE);
    huge_billboard_and_crop(JB_GEOMETRY_FLOAT);
    near_plane_one();
    tiny_field_of_view();
    guard_cut_options(JB_GEOMETRY_GTE);
    guard_cut_options(JB_GEOMETRY_FLOAT);
    depth_bias(JB_GEOMETRY_GTE);
    depth_bias(JB_GEOMETRY_FLOAT);
}

/* ------------------------------------------------------------------------ */
/* Distant lines, wide billboards, shared buckets and covered frame edges. */

/* Red within a pixel of (x, y). */
static int red_near(jb_context *jb, int x, int y)
{
    int dx, dy, w, h;

    jb_frame_size(jb, &w, &h);
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++)
            if (x + dx >= 0 && x + dx < w && y + dy >= 0 && y + dy < h &&
                is_red(pixel(jb, x + dx, y + dy)))
                return 1;
    return 0;
}

static void distant_line(int mode)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_line3d l;
    char what[96];
    const char *name = mode == JB_GEOMETRY_GTE ? "GTE" : "float";

    jb_set_geometry(jb, mode);
    /* A diagonal through the view from ends 1e20 away: the same pixels as
       the same line with ends at +-100, (80, 199) and (240, 39) among
       them. */
    memset(&l, 0, sizeof l);
    l.a.x = l.a.y = -1e20f;
    l.a.z = -100.0f;
    l.b.x = l.b.y = 1e20f;
    l.b.z = -100.0f;
    l.color_a = l.color_b = JB_RGB(255, 0, 0);
    jb_begin_frame(jb, 0);
    check("distant line: diagonal", jb_draw_line3d(jb, &l), JB_OK);
    jb_end_frame(jb);
    snprintf(what, sizeof what, "distant line: (%s): a diagonal from ends 1e20 away", name);
    check(what, red_near(jb, 80, 199) && red_near(jb, 240, 39), 1);
    /* Ends at the largest floats, beyond the float range once scaled. */
    memset(&l, 0, sizeof l);
    l.a.x = -3.4028234e38f;
    l.a.z = -100.0f;
    l.b.x = 3.4028234e38f;
    l.b.z = -100.0f;
    l.color_a = l.color_b = JB_RGB(255, 0, 0);
    jb_begin_frame(jb, 0);
    check("distant line: FLT_MAX line", jb_draw_line3d(jb, &l), JB_OK);
    jb_end_frame(jb);
    snprintf(what, sizeof what, "distant line: (%s): a line from ends at FLT_MAX", name);
    check(what, red_near(jb, 80, 120) && red_near(jb, 240, 120), 1);
    jb_destroy(jb);
}

static void wide_billboard_texels(int mode)
{
    jb_context *jb = make(1, 1, 0, 0);
    uint16_t two[2] = {JB_RGB15(31, 0, 0), JB_RGB15(0, 0, 31)};
    jb_texture_desc td;
    jb_texture tex;
    jb_billboard b;
    int x, all = 1;
    char what[96];

    /* Two texels, red then blue, across a billboard 10000 wide centred at
       x 80: its middle, where blue starts, lies off screen at about x 326,
       so every visible pixel is red, also after clipping. */
    jb_set_geometry(jb, mode);
    memset(&td, 0, sizeof td);
    td.width = 2;
    td.height = 1;
    td.depth = JB_TEX_15BIT;
    td.texels = two;
    jb_texture_create(jb, &td, &tex);
    memset(&b, 0, sizeof b);
    b.texture = &tex;
    b.raw = 1;
    b.position.x = 80.0f;
    b.position.z = -100.0f;
    b.width = 10000.0f;
    b.height = 100.0f;
    jb_begin_frame(jb, 0);
    check("wide billboard texels: billboard", jb_draw_billboard(jb, &b), JB_OK);
    jb_end_frame(jb);
    {
        int fw, fh;
        const uint8_t *fp = jb_frame(jb, &fw, &fh);

        for (x = 0; x < 320; x++)
            all &= is_red(frame_px(fp, fw, x, 120));
    }
    snprintf(what, sizeof what,
             "wide billboard texels: (%s): clipping keeps the texel boundary off screen",
             mode == JB_GEOMETRY_GTE ? "GTE" : "float");
    check(what, all, 1);
    jb_destroy(jb);
}

static void bucket_across_paths(void)
{
    jb_config c;
    jb_context *jb;
    jb_billboard b;

    /* 65536 buckets: a blue billboard at depth 100.24 in the GTE path,
       then a red one at 100.2 in the float path. Both paths order by the
       same integer depth (400 GTE units here), so the farther one is never
       sorted after the nearer: red shows. */
    memset(&c, 0, sizeof c);
    c.ot_size = 65536;
    jb = must_create(&c);
    memset(&b, 0, sizeof b);
    b.width = 10.0f;
    jb_begin_frame(jb, 0);
    jb_set_geometry(jb, JB_GEOMETRY_GTE);
    b.position.z = -100.24f;
    b.tint = JB_RGB(0, 0, 255);
    jb_draw_billboard(jb, &b);
    jb_set_geometry(jb, JB_GEOMETRY_FLOAT);
    b.position.z = -100.2f;
    b.tint = JB_RGB(255, 0, 0);
    jb_draw_billboard(jb, &b);
    jb_end_frame(jb);
    check("bucket across paths: mixed paths order by one depth", is_red(pixel(jb, 160, 120)), 1);
    jb_destroy(jb);
}

/* Every pixel at every edge of the frame is drawn by geometry that
   covers it, in frames of several sizes, both paths: a wall in front of
   the camera reaching far past the screen on every side, and a floor
   from behind the camera to far ahead (cut by the near plane). The one
   exception is the console's: column 1023 of a frame 1024 wide, which no
   polygon reaches (its vertices end at x 1023 and its right edge is not
   drawn). */
static void frame_edges_covered(void)
{
    static const int sizes[][2] = {{320, 240}, {427, 240}, {640, 480}, {256, 512},
                                   {1024, 240}, {1024, 512}, {1, 1},     {17, 300}};
    jb_gte_vec wall[4] = {{-1000, -1000, 0}, {1000, -1000, 0}, {1000, 1000, 0}, {-1000, 1000, 0}};
    jb_gte_vec floor_pos[4] = {{-1000, 0, 100}, {1000, 0, 100}, {1000, 0, -1000},
                               {-1000, 0, -1000}};
    jb_mesh_face quad = {{0, 1, 2, 3}, 4, 0};
    jb_material mat;
    jb_mesh m;
    size_t si;
    int k, x, y;

    memset(&mat, 0, sizeof mat);
    mat.unlit = 1;
    mat.double_sided = 1;
    mat.color = JB_RGB(255, 0, 0);
    memset(&m, 0, sizeof m);
    m.vertex_count = 4;
    m.face_count = 1;
    m.faces = &quad;
    m.material_count = 1;
    m.materials = &mat;
    for (si = 0; si < sizeof sizes / sizeof sizes[0]; si++)
        for (k = 0; k < 4; k++) {
            int w = sizes[si][0], h = sizes[si][1], mode = k & 1, which = k >> 1, missing = 0;
            jb_config c;
            jb_context *jb;
            jb_mat4 at;
            char what[128];

            memset(&c, 0, sizeof c);
            c.width = w;
            c.height = h;
            jb = must_create(&c);
            jb_set_geometry(jb, mode);
            jb_set_dither(jb, 0);
            if (which == 0) {
                jb_mat4 s = jb_mat4_scale(10.0f, 10.0f, 1.0f), t = jb_mat4_translate(0, 0, -100);

                at = jb_mat4_mul(&t, &s);
                m.positions = wall;
            } else {
                jb_mat4 s = jb_mat4_scale(10.0f, 1.0f, 3.0f), t = jb_mat4_translate(0, -50, 0);

                at = jb_mat4_mul(&t, &s);
                m.positions = floor_pos;
            }
            jb_begin_frame(jb, 0);
            check("frame edges covered: draw", jb_draw_mesh(jb, &m, &at), JB_OK);
            jb_end_frame(jb);
            /* The wall: all four edges. The floor: the bottom row, and the
               left and right columns below the horizon (with a margin for
               where it fades into the distance at the horizon). */
            {
                int fw, fh;
                const uint8_t *fp = jb_frame(jb, &fw, &fh);

                for (y = 0; y < h; y++)
                    for (x = 0; x < w; x++) {
                        int edge = x == 0 || y == 0 || x == w - 1 || y == h - 1;

                        if (!edge || (w == 1024 && x == 1023))
                            continue;
                        if (which == 1 && y < h / 2 + 16)
                            continue;
                        missing += !is_red(frame_px(fp, fw, x, y));
                    }
            }
            snprintf(what, sizeof what,
                     "frame edges covered: %s, %s, %d x %d: every edge pixel drawn",
                     which ? "floor through the near plane" : "wall past the screen",
                     mode == JB_GEOMETRY_GTE ? "GTE" : "float", w, h);
            if (!check(what, missing, 0))
                printf("  %d missing\n", missing);
            if (w == 1024 && which == 0)
                check("frame edges covered: column 1022 of a 1024-wide frame drawn",
                      is_red(pixel(jb, 1022, h / 2)), 1);
            jb_destroy(jb);
        }
}

/* What the header promises of this case: a quad
   two of whose vertices land on one integer position (160, 120) with
   depths within 1/16 (SZ 1600 and 1696), so the console's precise-vertex
   table keeps one record for both. With affine texturing at 1x the
   friendly draw and console code (jb_gte_* and an ordering table) are
   equal; perspective-correct, the table's depth is the other vertex's and
   they may differ (documented: not checked here). Returns the pixels that
   differ. */
static int shared_position_quad(int persp)
{
    jb_gte_vec pos[4] = {{-10, -100, -160}, {0, 0, -400}, {1, 0, -424}, {50, -100, -1600}};
    uint8_t uv[8] = {255, 0, 0, 0, 0, 0, 255, 0};
    static uint16_t strip[256];
    jb_mesh_face f = {{0, 1, 2, 3}, 4, 0};
    jb_material mat;
    jb_mesh m;
    jb_texture_desc td;
    jb_texture tex;
    jbi_geom_draw d;
    jbi_geom_poly p[2];
    jb_context *jb = make(1, 1, 0, persp);
    jb_ot *ot;
    jb_result r;
    uint8_t *a, *b;
    int w, h, i, extent, n = 0;
    char detail[200];

    for (i = 0; i < 256; i++)
        strip[i] = (uint16_t)JB_RGB15(i & 31, (i >> 3) & 31, 31 - (i & 31));
    memset(&td, 0, sizeof td);
    td.width = 256;
    td.height = 1;
    td.depth = JB_TEX_15BIT;
    td.texels = strip;
    check("shared position quad: texture", jb_texture_create(jb, &td, &tex), JB_OK);
    memset(&mat, 0, sizeof mat);
    mat.unlit = 1;
    mat.double_sided = 1;
    mat.raw = 1;
    mat.texture = &tex;
    memset(&m, 0, sizeof m);
    m.vertex_count = 4;
    m.positions = pos;
    m.uv = uv;
    m.face_count = 1;
    m.faces = &f;
    m.material_count = 1;
    m.materials = &mat;
    jbi_geom_check_mesh(&m, "t", &extent, detail, sizeof detail);
    jbi_frame_size(jb, &w, &h);
    check("shared position quad: begin",
          jbi_geom_begin(&d, &jb->api->scene, NULL, w, h, jbi_frame_ot_size(jb),
                                         extent, "t", detail, sizeof detail),
          JB_OK);
    jbi_geom_load(&jb->api->scene.gte, &d);
    check("shared position quad: the quad is exact",
          jbi_geom_face(&jb->api->scene.gte, &d, &m, 0, p) == 1 && p[0].exact, 1);
    jb_begin_frame(jb, 0);
    check("shared position quad: draw", jb_draw_mesh(jb, &m, NULL), JB_OK);
    jb_end_frame(jb);
    a = copy_frame(jb, &w, &h);
    ot = jb_ot_create(jb, jbi_frame_ot_size(jb), 4, &r);
    jb_begin_frame(jb, 0);
    hand_load(jb, &d);
    check("shared position quad: console face", hand_face(jb, ot, &d, &m, 0), 1);
    jb_ot_draw(jb, ot);
    jb_end_frame(jb);
    b = copy_frame(jb, &w, &h);
    for (i = 0; i < w * h; i++)
        n += memcmp(a + 4 * i, b + 4 * i, 4) != 0;
    free(a);
    free(b);
    jb_ot_destroy(ot);
    jb_destroy(jb);
    return n;
}

/* A triangle reaching past the right edge of a
   frame 1024 wide, cut by the guard band's right plane. Its row 120 is red
   from x 1000 through 1022 (its whole 1x pixel at 4x) in both paths, at 1x
   and at 4x with precise vertices. (The GTE path stopped at 1021: the cut
   vertex, projected, truncated to 1022, and a polygon's right edge is not
   drawn.) Column 1023 is the console's limit (frame_edges_covered). */
static void right_plane_triangle(int mode, int scale)
{
    jb_gte_vec pos[3] = {{230, -50, -100}, {280, 0, -100}, {230, 50, -100}};
    jb_mesh_face f;
    jb_material mat;
    jb_mesh m = one_tri(pos, &f, &mat, JB_RGB(255, 0, 0));
    jb_config c;
    jb_context *jb;
    char what[128];
    int x, first = -1;

    memset(&c, 0, sizeof c);
    c.width = 1024;
    c.height = 240;
    c.scale = scale;
    c.precise_vertices = scale > 1;
    jb = must_create(&c);
    jb_set_geometry(jb, mode);
    jb_set_dither(jb, 0);
    jb_begin_frame(jb, 0);
    check("right plane triangle: triangle", jb_draw_mesh(jb, &m, NULL), JB_OK);
    jb_end_frame(jb);
    {
        int fw, fh;
        const uint8_t *fp = jb_frame(jb, &fw, &fh);

        for (x = 1023 * scale - 1; x >= 1000 * scale; x--)
            if (!is_red(frame_px(fp, fw, x, 120 * scale)))
                first = x;
    }
    snprintf(what, sizeof what,
             "right plane triangle: (%s, %dx): row 120 red from x 1000 through 1022",
             mode == JB_GEOMETRY_GTE ? "GTE" : "float", scale);
    if (!check(what, first, -1))
        printf("  first gap at x %d\n", first);
    jb_destroy(jb);
}

static void clip_and_cover_cases(void)
{
    distant_line(JB_GEOMETRY_GTE);
    distant_line(JB_GEOMETRY_FLOAT);
    wide_billboard_texels(JB_GEOMETRY_GTE);
    wide_billboard_texels(JB_GEOMETRY_FLOAT);
    bucket_across_paths();
    check("clip and cover cases: one position, two depths: equal to console code at 1x, affine",
          shared_position_quad(0), 0);
    printf("perspective-correct, %d pixels differ (documented)\n", shared_position_quad(1));
    frame_edges_covered();
    right_plane_triangle(JB_GEOMETRY_GTE, 1);
    right_plane_triangle(JB_GEOMETRY_FLOAT, 1);
    right_plane_triangle(JB_GEOMETRY_GTE, 4);
    right_plane_triangle(JB_GEOMETRY_FLOAT, 4);
}

/* ------------------------------------------------------------------------ */
/* Small depth biases and planes within one depth. */

/* In the float path a depth bias is added to the depth before it
   is rounded, so a bias smaller than a step of OTZ still moves a draw
   across a bucket boundary its biased depth crosses. Red first, biased
   by bias, then blue at the same place unbiased, at depth 36.005 (GTE
   depth 144.02 with the default camera's scale of 4; 65536 buckets, one
   for each OTZ): biased by -0.01 (GTE -0.04) red's depth is 143.98, a
   whole depth nearer than blue's, and red shows; unbiased, blue (drawn
   later in the same bucket). The rounded bias alone was 0. kind: 0 a
   mesh, 1 a billboard, 2 a line. */
static int bias_red(int kind, float bias)
{
    jb_config c;
    jb_context *jb;
    int k, red;

    memset(&c, 0, sizeof c);
    c.ot_size = 65536;
    jb = must_create(&c);
    jb_set_geometry(jb, JB_GEOMETRY_FLOAT);
    jb_set_dither(jb, 0);
    jb_begin_frame(jb, 0);
    for (k = 0; k < 2; k++) {
        uint32_t color = k == 0 ? JB_RGB(255, 0, 0) : JB_RGB(0, 0, 255);
        float b = k == 0 ? bias : 0.0f;

        if (kind == 0) {
            jb_gte_vec pos[3] = {{-5, -5, 0}, {5, -5, 0}, {0, 5, 0}};
            jb_mesh_face f;
            jb_material mat;
            jb_mesh m = one_tri(pos, &f, &mat, color);
            jb_mat4 at = jb_mat4_translate(0.0f, 0.0f, -36.005f);
            jb_mesh_draw md;

            memset(&md, 0, sizeof md);
            md.mesh = &m;
            md.model = &at;
            md.depth_bias = b;
            check("bias red: mesh", jb_draw_mesh_ex(jb, &md), JB_OK);
        } else if (kind == 1) {
            jb_billboard bb;

            memset(&bb, 0, sizeof bb);
            bb.position.z = -36.005f;
            bb.width = 4.0f;
            bb.tint = color;
            bb.depth_bias = b;
            check("bias red: billboard", jb_draw_billboard(jb, &bb), JB_OK);
        } else {
            jb_line3d l;

            memset(&l, 0, sizeof l);
            l.a.x = -5.0f;
            l.a.z = l.b.z = -36.005f;
            l.b.x = 5.0f;
            l.color_a = l.color_b = color;
            l.depth_bias = b;
            check("bias red: line", jb_draw_line3d(jb, &l), JB_OK);
        }
    }
    jb_end_frame(jb);
    red = is_red(pixel(jb, 160, 120));
    jb_destroy(jb);
    return red;
}

static void small_bias(void)
{
    static const char *const names[3] = {"mesh", "billboard", "line"};
    char what[96];
    int kind;

    for (kind = 0; kind < 3; kind++) {
        snprintf(what, sizeof what, "small bias: (%s): unbiased, the later draw shows",
                 names[kind]);
        check(what, bias_red(kind, 0.0f), 0);
        snprintf(what, sizeof what, "small bias: (%s): a bias of -0.01 across a boundary shows",
                 names[kind]);
        check(what, bias_red(kind, -0.01f), 1);
    }
}

/* GTE planes within one integer depth draw. With the default
   camera (H 208, scale 256 for z_far 100.003) z_near 100.001 and z_far
   100.003 are GTE depths 25600.256 and 25600.768: ceil and floor cross,
   but a billboard at 100.002 (25600.512) is between the planes, at SZ
   25600. It was refused as an invalid depth range. */
static void close_planes(int mode)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_camera cam;
    jb_billboard bb;
    char what[96];
    const char *name = mode == JB_GEOMETRY_GTE ? "GTE" : "float";

    jb_set_geometry(jb, mode);
    memset(&cam, 0, sizeof cam);
    cam.target.z = -1.0f;
    cam.z_near = 100.001f;
    cam.z_far = 100.003f;
    check("close planes: camera", jb_set_camera(jb, &cam), JB_OK);
    memset(&bb, 0, sizeof bb);
    bb.position.z = -100.002f;
    bb.width = 10.0f;
    bb.tint = JB_RGB(255, 0, 0);
    jb_begin_frame(jb, 0);
    snprintf(what, sizeof what, "close planes: (%s): planes within one depth accepted", name);
    check(what, jb_draw_billboard(jb, &bb), JB_OK);
    jb_end_frame(jb);
    snprintf(what, sizeof what, "close planes: (%s): the billboard between them drawn", name);
    check(what, is_red(pixel(jb, 160, 120)), 1);
    jb_destroy(jb);
}

static void bias_and_plane_cases(void)
{
    small_bias();
    close_planes(JB_GEOMETRY_GTE);
    close_planes(JB_GEOMETRY_FLOAT);
}

/* ------------------------------------------------------------------------ */
/* Fixed cases through the public API, and properties over the classes
   they belong to. */

static uint32_t prop_rng = 5555;

static int rand_below(int n)
{
    prop_rng = prop_rng * 1103515245u + 12345u;
    return (int)((prop_rng >> 8) % (uint32_t)n);
}

static double rand_unit(void)
{
    return (double)rand_below(1 << 20) / (double)(1 << 20);
}

/* An asymmetric distant diagonal: the same visible line as ends at
   -+100 (red at (80, 199) and (240, 39)). */
static void asymmetric_diagonal(int mode)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_line3d l;
    char what[96];

    jb_set_geometry(jb, mode);
    memset(&l, 0, sizeof l);
    l.a.x = l.a.y = -1e20f;
    l.a.z = -100.0f;
    l.b.x = l.b.y = 2e20f;
    l.b.z = -100.0f;
    l.color_a = l.color_b = JB_RGB(255, 0, 0);
    jb_begin_frame(jb, 0);
    check("asymmetric diagonal: line", jb_draw_line3d(jb, &l), JB_OK);
    jb_end_frame(jb);
    snprintf(what, sizeof what,
             "asymmetric diagonal: (%s): an asymmetric diagonal from ends 1e20 and 2e20 away",
             mode == JB_GEOMETRY_GTE ? "GTE" : "float");
    check(what, red_near(jb, 80, 199) && red_near(jb, 240, 39) && !red_near(jb, 80, 120), 1);
    jb_destroy(jb);
}

/*
 * A clipped billboard keeps every texel where the whole billboard
 * puts it, whatever its size. Random widths from a few pixels to 1e30
 * world units, centres across the screen, crops of 2..8 texels of a strip
 * of distinct colours, both paths: along the centre row, each pixel
 * shows the texel the unclipped billboard maps it to (its coordinate at
 * the pixel's centre, u0 at the left edge to u0 + tw - 1 at the right,
 * rounded to the nearest), but within two pixels of a texel boundary
 * (at 1x the GPU draws from whole-pixel vertices and samples at a pixel's
 * corner).
 */
static void billboard_texels_property(int mode)
{
    static uint16_t strip[8];
    jb_context *jb = make(1, 1, 0, 0);
    jb_texture_desc td;
    jb_texture tex;
    char what[96];
    int n, i, bad = 0, compared = 0;

    for (i = 0; i < 8; i++)
        strip[i] = (uint16_t)JB_RGB15(2 + 4 * i, 31 - 4 * i, 3 * i);
    memset(&td, 0, sizeof td);
    td.width = 8;
    td.height = 1;
    td.depth = JB_TEX_15BIT;
    td.texels = strip;
    check("billboard texels property: texture", jb_texture_create(jb, &td, &tex), JB_OK);
    jb_set_geometry(jb, mode);
    jb_set_dither(jb, 0);
    for (n = 0; n < 300; n++) {
        jb_billboard b;
        jbi_geom_draw d;
        jbi_geom_vertex c;
        double k, pw, cx, step, tm;
        int u0 = rand_below(7), tw = 2 + rand_below(7 - u0), x, w, h, fw, fh;
        const uint8_t *fp;
        char detail[200];

        if (tw > 8 - u0)
            tw = 8 - u0;
        memset(&b, 0, sizeof b);
        b.texture = &tex;
        b.raw = 1;
        b.u = u0;
        b.tw = tw;
        b.position.x = (float)(rand_unit() * 120.0 - 60.0);
        b.position.z = -100.0f;
        b.width = (float)pow(10.0, rand_unit() * 31.0 - 1.0);
        b.height = 30.0f;
        /* The unclipped billboard's numbers, as the draw takes them. */
        jbi_frame_size(jb, &w, &h);
        jbi_geom_begin(&d, &jb->api->scene, NULL, w, h, jbi_frame_ot_size(jb), JBI_GEOM_NO_MESH,
                       "t", detail, sizeof detail);
        if (!jbi_geom_point(&d, b.position, &c))
            continue;
        k = (mode == JB_GEOMETRY_GTE ? (double)d.r.h : d.fh) * (double)d.scale /
            (mode == JB_GEOMETRY_GTE ? (double)c.z : c.zf);
        pw = (double)b.width * k;
        if (pw < 1e6 && floor(pw + 0.5) == tw)
            continue; /* drawn as a rectangle, one texel a pixel */
        cx = (double)c.sx / 256.0;
        step = pw / (double)(tw - 1);
        tm = u0 + (tw - 1) * 0.5;
        jb_begin_frame(jb, 0);
        check("billboard texels property: billboard", jb_draw_billboard(jb, &b), JB_OK);
        jb_end_frame(jb);
        fp = jb_frame(jb, &fw, &fh);
        for (x = 0; x < w; x++) {
            /* The texel whose span of offsets from the centre holds the
               pixel's, (t - 0.5 - tm) step .. (t + 0.5 - tm) step (offsets,
               not coordinates: a huge step rounds a coordinate near the
               centre onto a boundary). */
            double off = (double)x + 0.5 - cx, edge = 1e300;
            int want = -1, got = -1, t;
            const uint8_t *p = frame_px(fp, fw, x, 120);

            if (fabs(off) > pw * 0.5 - 2.0)
                continue; /* outside, or at its edge */
            for (t = u0; t < u0 + tw; t++) {
                double lo = t == u0 ? -1e300 : ((double)t - 0.5 - tm) * step;
                double hi = t == u0 + tw - 1 ? 1e300 : ((double)t + 0.5 - tm) * step;

                if (off >= lo && off < hi)
                    want = t;
                if (t > u0)
                    edge = fabs(off - lo) < edge ? fabs(off - lo) : edge;
            }
            if (edge < 2.0)
                continue; /* at a texel boundary (vertices in whole pixels at 1x) */
            for (t = 0; t < 8; t++)
                if (p[0] >> 3 == 2 + 4 * t && p[1] >> 3 == 31 - 4 * t)
                    got = t;
            compared++;
            if (got != want) {
                if (bad++ < 3)
                    printf("  (%s): width %g, x %d shows texel %d, not %d\n",
                           mode == JB_GEOMETRY_GTE ? "GTE" : "float", (double)b.width, x, got,
                           want);
            }
        }
    }
    snprintf(what, sizeof what,
             "billboard texels property: (%s): every texel where the whole billboard puts it",
             mode == JB_GEOMETRY_GTE ? "GTE" : "float");
    check(what, bad, 0);
    check("billboard texels property: many pixels compared", compared > 20000, 1);
    jb_destroy(jb);
}

/* Two texels red and blue across a billboard
   1e20 wide centred at x 80: every visible pixel red. */
static void huge_billboard_texel(int mode)
{
    jb_context *jb = make(1, 1, 0, 0);
    uint16_t two[2] = {JB_RGB15(31, 0, 0), JB_RGB15(0, 0, 31)};
    jb_texture_desc td;
    jb_texture tex;
    jb_billboard b;
    int x, all = 1;
    char what[96];

    jb_set_geometry(jb, mode);
    memset(&td, 0, sizeof td);
    td.width = 2;
    td.height = 1;
    td.depth = JB_TEX_15BIT;
    td.texels = two;
    jb_texture_create(jb, &td, &tex);
    memset(&b, 0, sizeof b);
    b.texture = &tex;
    b.raw = 1;
    b.position.x = 80.0f;
    b.position.z = -100.0f;
    b.width = 1e20f;
    b.height = 100.0f;
    jb_begin_frame(jb, 0);
    check("huge billboard texel: billboard", jb_draw_billboard(jb, &b), JB_OK);
    jb_end_frame(jb);
    {
        int fw, fh;
        const uint8_t *fp = jb_frame(jb, &fw, &fh);

        for (x = 0; x < 320; x++)
            all &= is_red(frame_px(fp, fw, x, 120));
    }
    snprintf(what, sizeof what,
             "huge billboard texel: (%s): a billboard 1e20 wide keeps its texel boundary",
             mode == JB_GEOMETRY_GTE ? "GTE" : "float");
    check(what, all, 1);
    jb_destroy(jb);
}

/* The GTE path's near plane raised to H / 2 + 1 (32766, H 65530)
   within one integer depth of the far plane (32766.75): blue at 32766.5
   in the GTE path, then red at 32766.25 in the float path. Both have key
   32766, so they share a bucket and the later draw, red, shows. */
static void raised_near_plane(void)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_camera cam;
    jb_billboard b;

    memset(&cam, 0, sizeof cam);
    cam.target.z = -1.0f;
    cam.projection = 65530.0f;
    cam.z_near = 1.0f;
    cam.z_far = 32766.75f;
    check("raised near plane: camera", jb_set_camera(jb, &cam), JB_OK);
    memset(&b, 0, sizeof b);
    b.width = 10.0f;
    jb_begin_frame(jb, 0);
    jb_set_geometry(jb, JB_GEOMETRY_GTE);
    b.position.z = -32766.5f;
    b.tint = JB_RGB(0, 0, 255);
    check("raised near plane: blue", jb_draw_billboard(jb, &b), JB_OK);
    jb_set_geometry(jb, JB_GEOMETRY_FLOAT);
    b.position.z = -32766.25f;
    b.tint = JB_RGB(255, 0, 0);
    check("raised near plane: red", jb_draw_billboard(jb, &b), JB_OK);
    jb_end_frame(jb);
    check("raised near plane: one key, one bucket in both paths (thin GTE range)",
          is_red(pixel(jb, 160, 120)), 1);
    jb_destroy(jb);
}

/*
 * A clipped face whose own vertices lie on the guard band's right
 * plane draws up to it. In a frame 1024 wide with H 511 (x = 1023 where
 * the view's x equals its depth), random triangles with two vertices on
 * that plane (x = |z|) and a third far to the left (past the left plane,
 * so the face is clipped), the float path at 1x and 4x and the GTE path at
 * 4x with precise vertices: every row between the two plane vertices (a
 * row's margin either way) has column 1022 drawn (its whole 1x pixel at
 * 4x).
 */
static void right_plane_property(int mode, int scale)
{
    jb_config cfg;
    jb_context *jb;
    jb_camera cam;
    char what[128];
    int n, bad = 0, rows = 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.width = 1024;
    cfg.height = 240;
    cfg.scale = scale;
    cfg.precise_vertices = scale > 1;
    jb = must_create(&cfg);
    jb_set_geometry(jb, mode);
    jb_set_dither(jb, 0);
    memset(&cam, 0, sizeof cam);
    cam.target.z = -1.0f;
    cam.projection = 511.0f;
    jb_set_camera(jb, &cam);
    for (n = 0; n < 60; n++) {
        /* Depths beyond the GTE path's near plane, H / 2 + 1 = 256 GTE
           units (64 world units at the scale of 4). */
        int z = 70 + rand_below(930), y1 = -(z / 50 + rand_below(z / 5)), y2 = z / 50 + rand_below(z / 5);
        int z3 = z + rand_below(200) - 100, y, x, fw, fh;
        const uint8_t *fp;
        jb_gte_vec pos[3];
        jb_mesh_face f;
        jb_material mat;
        jb_mesh m;
        /* World y is up, the screen's down: y2 is the higher row. */
        double sy1 = 120.0 - 511.0 * y2 / z, sy2 = 120.0 - 511.0 * y1 / z;

        pos[0].x = (int16_t)z, pos[0].y = (int16_t)y1, pos[0].z = (int16_t)-z;
        pos[1].x = (int16_t)z, pos[1].y = (int16_t)y2, pos[1].z = (int16_t)-z;
        z3 = z3 < 70 ? 70 : z3;
        pos[2].x = (int16_t)-(4 * z3 + rand_below(4 * z3)), pos[2].y = (int16_t)(rand_below(z3 / 3));
        pos[2].z = (int16_t)-z3;
        m = one_tri(pos, &f, &mat, JB_RGB(255, 0, 0));
        jb_begin_frame(jb, 0);
        check("right plane property: draw", jb_draw_mesh(jb, &m, NULL), JB_OK);
        jb_end_frame(jb);
        fp = jb_frame(jb, &fw, &fh);
        for (y = (int)ceil(sy1) + 1; y <= (int)floor(sy2) - 2; y++) {
            if (y < 0 || y >= 240)
                continue;
            rows++;
            for (x = 1022 * scale; x < 1023 * scale; x++)
                if (!is_red(frame_px(fp, fw, x, y * scale + scale / 2))) {
                    if (bad++ < 3)
                        printf("  (%s, %dx): (%d %d %d) (%d %d %d) (%d %d %d), row %d, x "
                               "%d not drawn\n",
                               mode == JB_GEOMETRY_GTE ? "GTE" : "float", scale, pos[0].x,
                               pos[0].y, pos[0].z, pos[1].x, pos[1].y, pos[1].z, pos[2].x,
                               pos[2].y, pos[2].z, y, x);
                    break;
                }
        }
    }
    snprintf(what, sizeof what,
             "(%s, %dx): faces with vertices on the right plane reach column 1022",
             mode == JB_GEOMETRY_GTE ? "GTE" : "float", scale);
    check(what, bad, 0);
    check("right plane property: rows compared", rows > 300, 1);
    jb_destroy(jb);
}

/* A lit face without normals, so thin that float products of its
   edges round its direction away: lit as given the exact normal. */
static void thin_face_light(void)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_gte_vec pos[3] = {{-30000, -29999, -100}, {0, 0, -100}, {1, 1, -100}};
    jb_mesh_face f;
    jb_material mat;
    jb_mesh m = one_tri(pos, &f, &mat, JB_RGB(255, 0, 0));
    jb_light l;
    jb_mat4 model = jb_mat4_identity();

    jb_set_geometry(jb, JB_GEOMETRY_FLOAT);
    jb_set_dither(jb, 0);
    mat.unlit = 0;
    jb_set_ambient(jb, 0);
    memset(&l, 0, sizeof l);
    l.direction.z = -1.0f;
    l.color = JB_RGB(255, 255, 255);
    jb_set_light(jb, 0, &l);
    /* x' = 1000000 (x - y), y' = 20 y, z' = z (column-major). */
    model.m[0] = 1000000.0f;
    model.m[4] = -1000000.0f;
    model.m[5] = 20.0f;
    jb_begin_frame(jb, 0);
    check("thin face light: draw", jb_draw_mesh(jb, &m, &model), JB_OK);
    jb_end_frame(jb);
    check("thin face light: a thin face without normals lit by its direction",
          is_red(pixel(jb, 130, 100)), 1);
    jb_destroy(jb);
}

/* A face biased nearer than the eye for one vertex (-1100) keeps
   that in its average (about 233 world units), so it sorts nearer than a
   billboard at 400 drawn after it. */
static void biased_face_order(void)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_gte_vec pos[3] = {{0, 60, -100}, {-500, -500, -2100}, {500, -500, -2100}};
    jb_mesh_face f;
    jb_material mat;
    jb_mesh m = one_tri(pos, &f, &mat, JB_RGB(255, 0, 0));
    jb_mesh_draw md;
    jb_billboard b;

    jb_set_geometry(jb, JB_GEOMETRY_FLOAT);
    memset(&md, 0, sizeof md);
    md.mesh = &m;
    md.depth_bias = -1200.0f;
    memset(&b, 0, sizeof b);
    b.position.z = -400.0f;
    b.width = 60.0f;
    b.tint = JB_RGB(0, 0, 255);
    jb_begin_frame(jb, 0);
    check("biased face order: mesh", jb_draw_mesh_ex(jb, &md), JB_OK);
    check("biased face order: billboard", jb_draw_billboard(jb, &b), JB_OK);
    jb_end_frame(jb);
    check("biased face order: a negative biased depth kept in the average",
          is_red(pixel(jb, 160, 120)), 1);
    jb_destroy(jb);
}

/* Draws m with model through jb_draw_mesh and as console code does
   (jb_gte_* and an ordering table); returns the pixels that differ, or
   -1 when the face is not exact or two of its vertices share an integer
   position (the documented exception). */
static int console_diff(jb_context *jb, const jb_mesh *m, const jb_mat4 *model)
{
    jbi_geom_draw d;
    jbi_geom_poly p[2];
    jb_ot *ot;
    jb_result r;
    uint8_t *a, *b;
    int w, h, i, extent, n = 0;
    char detail[200];

    jbi_geom_check_mesh(m, "t", &extent, detail, sizeof detail);
    jbi_frame_size(jb, &w, &h);
    if (jbi_geom_begin(&d, &jb->api->scene, model, w, h, jbi_frame_ot_size(jb), extent, "t",
                       detail, sizeof detail) != JB_OK)
        return -1;
    jbi_geom_load(&jb->api->scene.gte, &d);
    if (jbi_geom_face(&jb->api->scene.gte, &d, m, 0, p) != 1 || !p[0].exact)
        return -1;
    for (i = 0; i < p[0].n; i++) {
        int j;

        for (j = 0; j < i; j++)
            if (p[0].v[i].x == p[0].v[j].x && p[0].v[i].y == p[0].v[j].y)
                return -1;
    }
    jb_begin_frame(jb, 0);
    check("console diff: friendly draw", jb_draw_mesh(jb, m, model), JB_OK);
    jb_end_frame(jb);
    a = copy_frame(jb, &w, &h);
    ot = jb_ot_create(jb, jbi_frame_ot_size(jb), 4, &r);
    jb_begin_frame(jb, 0);
    hand_load(jb, &d);
    check("console diff: console face", hand_face(jb, ot, &d, m, 0), 1);
    jb_ot_draw(jb, ot);
    jb_end_frame(jb);
    b = copy_frame(jb, &w, &h);
    for (i = 0; i < w * h; i++)
        n += memcmp(a + 4 * i, b + 4 * i, 4) != 0;
    free(a);
    free(b);
    jb_ot_destroy(ot);
    return n;
}

static jb_context *wide_context(int scale)
{
    jb_config cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.width = 1024;
    cfg.height = 240;
    cfg.scale = scale;
    cfg.precise_vertices = scale > 1;
    return must_create(&cfg);
}

/* An exact face whose first two vertices
   project just inside the right plane, the GTE's record past 1023: at 4x
   with precise vertices it draws (4092, 480) and (4093, 480) as console
   code does. */
static void right_plane_exact_face(void)
{
    jb_context *jb = wide_context(4);
    jb_gte_vec pos[3] = {{293, 401, -803}, {293, -131, -803}, {270, 0, -803}};
    jb_mesh_face f;
    jb_material mat;
    jb_mesh m = one_tri(pos, &f, &mat, JB_RGB(255, 0, 0));
    jb_mat4 model = jb_mat4_scale(0.25f, 0.0625f, 0.125f);
    jb_camera cam;

    memset(&cam, 0, sizeof cam);
    cam.target.z = -1.0f;
    cam.projection = 700.0f;
    jb_set_camera(jb, &cam);
    jb_set_dither(jb, 0);
    check("right plane exact face: equal to console code at the right edge",
          console_diff(jb, &m, &model), 0);
    jb_begin_frame(jb, 0);
    jb_draw_mesh(jb, &m, &model);
    jb_end_frame(jb);
    check("right plane exact face: the GTE's sub-pixel coverage past 1023",
          is_red(pixel(jb, 4092, 480)) && is_red(pixel(jb, 4093, 480)), 1);
    jb_destroy(jb);
}

/*
 * The class: exact faces at the right edge of a frame 1024 wide
 * draw what console code draws. Random projection distances, depths and
 * model scales (so the view's coordinates have fractions, which the
 * GTE's integer IR and SZ and its divide turn into a record past 1023 for
 * a vertex whose exact projection is just inside, the more so the nearer
 * it is: depths just past the near plane), triangles with two
 * vertices aimed within the last two columns and one further in, 1x and
 * 4x with precise vertices: every exact one (no two vertices on one
 * integer position) gives the same pixels as console code.
 */
static void right_plane_exact_property(int scale)
{
    jb_context *jb = wide_context(scale);
    char what[96];
    int n, compared = 0, bad = 0;

    jb_set_dither(jb, 0);
    for (n = 0; n < 120; n++) {
        jb_camera cam;
        jb_gte_vec pos[3];
        jb_mesh_face f;
        jb_material mat;
        jb_mesh m;
        /* View depths just past the near plane (H / 2 + 1), where the
           GTE's whole SZ moves a vertex at the edge furthest (about 511 /
           SZ pixels a unit): z is world units, a quarter of the view's. */
        double hh = 300.0 + rand_below(600), z = (hh / 2.0 + 2.0 + rand_below(300)) / 4.0;
        float ms[3];
        jb_mat4 model;
        int i, diff;

        for (i = 0; i < 3; i++)
            ms[i] = (float)(1 + rand_below(16)) / 16.0f;
        model = jb_mat4_scale(ms[0], ms[1], ms[2]);

        memset(&cam, 0, sizeof cam);
        cam.target.z = -1.0f;
        cam.projection = (float)hh;
        jb_set_camera(jb, &cam);
        for (i = 0; i < 3; i++) {
            double sx = i < 2 ? 1022.2 + rand_unit() * 0.79 : 900.0 + rand_unit() * 90.0;
            double sy = i == 0 ? rand_unit() * 100.0 : i == 1 ? 140.0 + rand_unit() * 99.0
                                                              : 60.0 + rand_unit() * 120.0;
            double wz = z / ms[2];

            /* The default far plane's scale is 4 (view = 4 x world), the
               model's scale on top. */
            pos[i].x = (int16_t)floor((sx - 512.0) * z / hh / ms[0] + 0.5);
            pos[i].y = (int16_t)floor(-(sy - 120.0) * z / hh / ms[1] + 0.5);
            pos[i].z = (int16_t)-(wz > 32767.0 ? 32767.0 : wz);
        }
        m = one_tri(pos, &f, &mat, JB_RGB(255, 0, 0));
        diff = console_diff(jb, &m, &model);
        if (diff < 0)
            continue;
        compared++;
        if (diff != 0 && bad++ < 3)
            printf("  (%dx): H %g z %g: %d pixels differ from console code\n", scale, hh, z,
                   diff);
    }
    snprintf(what, sizeof what,
             "right plane exact property: (%dx): exact faces at the right edge equal console code",
             scale);
    check(what, bad, 0);
    check("right plane exact property: faces compared", compared > 30, 1);
    jb_destroy(jb);
}

/*
 * A billboard or line draws in the GTE path wherever it does in the
 * float path, the mesh registers' limits notwithstanding (points and
 * segments never use them). Random cameras far from the origin (x up to
 * 1e9) and depth ranges from wide to a thousandth of their distance, a
 * billboard between the planes and a line across it: both paths accept
 * both draws, and both draw the centre pixel.
 */
static void far_points_property(void)
{
    jb_context *jb = make(1, 1, 0, 0);
    int n, bad = 0, compared = 0;

    jb_set_dither(jb, 0);
    for (n = 0; n < 200; n++) {
        jb_camera cam;
        jb_billboard b;
        jb_line3d l;
        double near = pow(10.0, rand_unit() * 5.0 - 3.0);
        double far = near * (1.0 + pow(10.0, rand_unit() * 6.0 - 3.0));
        int mode, ok[2], drawn[2];

        memset(&cam, 0, sizeof cam);
        cam.eye.x = (float)floor(pow(10.0, rand_unit() * 9.0));
        cam.eye.y = (float)(rand_below(2001) - 1000);
        cam.target = cam.eye;
        cam.target.z = -1.0f;
        cam.z_near = (float)near;
        cam.z_far = (float)far;
        if (jb_set_camera(jb, &cam) != JB_OK || !(cam.z_near < cam.z_far))
            continue;
        memset(&b, 0, sizeof b);
        b.position = cam.eye;
        b.position.z = (float)-((near + far) * 0.5);
        if (!((double)-b.position.z > near && (double)-b.position.z < far))
            continue;
        b.width = (float)((near + far) * 0.1);
        b.tint = JB_RGB(255, 0, 0);
        memset(&l, 0, sizeof l);
        l.a = l.b = b.position;
        l.a.y -= b.width;
        l.b.y += b.width;
        l.color_a = l.color_b = JB_RGB(255, 0, 0);
        for (mode = 0; mode < 2; mode++) {
            jb_set_geometry(jb, mode);
            jb_begin_frame(jb, 0);
            ok[mode] = jb_draw_billboard(jb, &b) == JB_OK;
            jb_end_frame(jb);
            drawn[mode] = is_red(pixel(jb, 160, 120));
            jb_begin_frame(jb, 0);
            ok[mode] += 2 * (jb_draw_line3d(jb, &l) == JB_OK);
            jb_end_frame(jb);
            drawn[mode] += 2 * is_red(pixel(jb, 160, 120));
        }
        jb_check(jb, NULL);
        compared++;
        if (ok[0] != 3 || ok[1] != 3 || drawn[0] != drawn[1] || drawn[0] != 3) {
            if (bad++ < 3)
                printf("  eye x %g, near %g, far %g: accepted %d %d, drawn %d %d\n",
                       (double)cam.eye.x, near, far, ok[0], ok[1], drawn[0], drawn[1]);
        }
    }
    check("far points property: billboards and lines draw in the GTE path where they do in float",
          bad, 0);
    check("far points property: cameras compared", compared > 100, 1);
    jb_destroy(jb);
}

/* Billboards and lines far from the origin or in a tiny depth range. */
static void far_points(void)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_camera cam;
    jb_billboard b;
    jb_line3d l;

    jb_set_geometry(jb, JB_GEOMETRY_GTE);
    memset(&cam, 0, sizeof cam);
    cam.eye.x = 1e9f;
    cam.target.x = 1e9f;
    cam.target.z = -1.0f;
    jb_set_camera(jb, &cam);
    memset(&b, 0, sizeof b);
    b.position.x = 1e9f;
    b.position.z = -100.0f;
    b.width = 20.0f;
    b.tint = JB_RGB(255, 0, 0);
    memset(&l, 0, sizeof l);
    l.a = l.b = b.position;
    l.a.y = -50.0f;
    l.b.y = 50.0f;
    l.color_a = l.color_b = JB_RGB(255, 0, 0);
    jb_begin_frame(jb, 0);
    check("far points: a billboard far from the origin (GTE)", jb_draw_billboard(jb, &b), JB_OK);
    check("far points: a line far from the origin (GTE)", jb_draw_line3d(jb, &l), JB_OK);
    jb_end_frame(jb);
    check("far points: drawn", is_red(pixel(jb, 160, 120)), 1);
    memset(&cam, 0, sizeof cam);
    cam.target.z = -1.0f;
    cam.z_near = 0.001f;
    cam.z_far = 0.05f;
    jb_set_camera(jb, &cam);
    b.position.x = 0.0f;
    b.position.z = -0.02f;
    b.width = 0.004f;
    jb_begin_frame(jb, 0);
    check("far points: a billboard with a tiny depth range (GTE)", jb_draw_billboard(jb, &b),
          JB_OK);
    jb_end_frame(jb);
    check("far points: drawn (tiny range)", is_red(pixel(jb, 160, 120)), 1);
    jb_destroy(jb);
}

/* A line lying on the near plane (z_near 100: both ends at view
   depth 400 exactly) draws, as it does just beyond it. */
static void line_on_near_plane(int mode)
{
    jb_context *jb = make(1, 1, 0, 0);
    jb_camera cam;
    jb_line3d l;
    char what[96];
    int fw, fh, x, y, red = 0;
    const uint8_t *fp;

    jb_set_geometry(jb, mode);
    memset(&cam, 0, sizeof cam);
    cam.target.z = -1.0f;
    cam.z_near = 100.0f;
    jb_set_camera(jb, &cam);
    memset(&l, 0, sizeof l);
    l.a.x = -40.0f;
    l.a.y = -1.0f;
    l.a.z = -100.0f;
    l.b.x = 8.0f;
    l.b.y = -0.01f;
    l.b.z = -100.0f;
    l.color_a = l.color_b = JB_RGB(255, 0, 0);
    jb_begin_frame(jb, 0);
    check("line on near plane: line", jb_draw_line3d(jb, &l), JB_OK);
    jb_end_frame(jb);
    fp = jb_frame(jb, &fw, &fh);
    for (y = 0; y < fh; y++)
        for (x = 0; x < fw; x++)
            red += is_red(frame_px(fp, fw, x, y));
    snprintf(what, sizeof what, "line on near plane: (%s): a line on the near plane drawn",
             mode == JB_GEOMETRY_GTE ? "GTE" : "float");
    if (!check(what, red > 50, 1))
        printf("  %d red pixels\n", red);
    jb_destroy(jb);
}

/* Two additive triangles sharing edge AB, one clipped (ABC, past
   the left plane) and one exact (ADB), with A on the right plane of a
   frame 1024 wide (H 511): the shared edge is drawn once, so no pixel
   gets the red twice (64 each: about 66 once, 132 twice). */
static void shared_clipped_edge(int mode)
{
    jb_context *jb = wide_context(1);
    jb_gte_vec pos[4] = {{100, 15, -100}, {80, -15, -100}, {-400, 0, -100}, {100, -15, -100}};
    jb_mesh_face f[2] = {{{0, 1, 2, 0}, 3, 0}, {{0, 3, 1, 0}, 3, 0}};
    jb_material mat;
    jb_mesh m;
    jb_camera cam;
    char what[96];
    int fw, fh, x, y, twice = 0, drawn = 0;
    const uint8_t *fp;

    jb_set_geometry(jb, mode);
    jb_set_dither(jb, 0);
    memset(&cam, 0, sizeof cam);
    cam.target.z = -1.0f;
    cam.projection = 511.0f;
    jb_set_camera(jb, &cam);
    memset(&mat, 0, sizeof mat);
    mat.unlit = 1;
    mat.double_sided = 1;
    mat.blend = JB_BLEND_ADD;
    mat.color = JB_RGB(64, 0, 0);
    memset(&m, 0, sizeof m);
    m.vertex_count = 4;
    m.positions = pos;
    m.face_count = 2;
    m.faces = f;
    m.material_count = 1;
    m.materials = &mat;
    jb_begin_frame(jb, 0);
    check("shared clipped edge: draw", jb_draw_mesh(jb, &m, NULL), JB_OK);
    jb_end_frame(jb);
    fp = jb_frame(jb, &fw, &fh);
    for (y = 0; y < fh; y++)
        for (x = 0; x < fw; x++) {
            int r = frame_px(fp, fw, x, y)[0];

            drawn += r > 0;
            twice += r > 100;
        }
    snprintf(what, sizeof what,
             "shared clipped edge: (%s): a clipped and an exact face share an edge once",
             mode == JB_GEOMETRY_GTE ? "GTE" : "float");
    if (!check(what, twice == 0 && drawn > 1000, 1))
        printf("  %d pixels drawn, %d of them twice\n", drawn, twice);
    jb_destroy(jb);
}

static void property_cases(void)
{
    asymmetric_diagonal(JB_GEOMETRY_GTE);
    asymmetric_diagonal(JB_GEOMETRY_FLOAT);
    huge_billboard_texel(JB_GEOMETRY_GTE);
    huge_billboard_texel(JB_GEOMETRY_FLOAT);
    billboard_texels_property(JB_GEOMETRY_GTE);
    billboard_texels_property(JB_GEOMETRY_FLOAT);
    raised_near_plane();
    /* Not the GTE path at 1x: there the face's own vertex keeps the GTE's
       1022 so as to meet an exact neighbour (the header's known
       limit), and its edge stops a column short. */
    right_plane_property(JB_GEOMETRY_FLOAT, 1);
    right_plane_property(JB_GEOMETRY_GTE, 4);
    right_plane_property(JB_GEOMETRY_FLOAT, 4);
    thin_face_light();
    biased_face_order();
    right_plane_exact_face();
    right_plane_exact_property(1);
    right_plane_exact_property(4);
    far_points();
    far_points_property();
    line_on_near_plane(JB_GEOMETRY_GTE);
    line_on_near_plane(JB_GEOMETRY_FLOAT);
    shared_clipped_edge(JB_GEOMETRY_GTE);
    shared_clipped_edge(JB_GEOMETRY_FLOAT);
}

/*
 * The seam fix (a cut polygon's precise vertex drawn where a passed-through
 * one draws it) and the precise pass's culling: three fixed cases, each drawn on the GTE path with
 * the default camera, a model scale of 0.731 and a small translation, the
 * frame's RGBA hashed (FNV-1a 64).
 *   - 1: an unlit, double-sided, textured Gouraud triangle the splitter
 *     cuts: its 1x and non-precise 2x frames must be those from before
 *     the seam fix (hashes from the build just before it): the fix is the
 *     precise pass's alone.
 *   - 2: a 3D line clipped at the view: likewise.
 *   - 0: an exact triangle whose raw sub-pixel corners face away but whose
 *     drawn ones face the eye (a corner's sub-pixel position is beyond the
 *     decoder's slack, so the precise pass draws it at its whole pixel): at
 *     4x with precise vertices it must draw single-sided what it draws
 *     double-sided (it drew nothing).
 */
static uint64_t seam_frame(int fixture, int scale, int precise, int sided, int *count)
{
    static const jb_gte_vec a[3] = {{-79, 192, -331}, {-193, -161, -355}, {-123, 58, -340}};
    static const jb_gte_vec b[3] = {{360, -916, -1392}, {-172, 528, -487}, {29, 520, -217}};
    static const uint32_t colors[3] = {JB_RGB(0, 255, 0), JB_RGB(120, 135, 92), JB_RGB(240, 15, 184)};
    static const uint8_t uv[6] = {0, 0, 114, 0, 228, 0};
    static uint16_t words[256];
    jb_config c;
    jb_context *jb;
    jb_mat4 m;
    jb_texture t;
    jb_texture_desc d;
    jb_mesh_face f;
    jb_material mat;
    jb_mesh mesh;
    const uint8_t *px;
    uint64_t hash = 1469598103934665603ull; /* FNV-1a 64's offset basis */
    int w, h, i, k;

    memset(&c, 0, sizeof c);
    memset(&d, 0, sizeof d);
    memset(&f, 0, sizeof f);
    memset(&mat, 0, sizeof mat);
    f.vertices = 3, f.v[0] = 0, f.v[1] = 1, f.v[2] = 2;
    mesh.vertex_count = 3, mesh.positions = fixture ? b : a, mesh.normals = NULL, mesh.colors = colors;
    mesh.uv = fixture ? uv : NULL, mesh.face_count = 1, mesh.faces = &f, mesh.material_count = 1;
    mesh.materials = &mat;
    c.scale = scale, c.precise_vertices = precise, c.threads = 1;
    jb = must_create(&c);
    mat.unlit = 1, mat.double_sided = sided;
    m = jb_mat4_translate(fixture ? 0.4375f : 0.125f, 0.1875f, 0);
    m.m[0] = m.m[5] = m.m[10] = 0.731f;
    if (fixture == 1) {
        for (i = 0; i < 256; i++)
            words[i] = i & 1 ? JB_RGB15(31, 3, 29) : JB_RGB15(5, 30, 4);
        d.width = 256, d.height = 1, d.depth = JB_TEX_15BIT, d.texels = words;
        check("seam frame: texture", jb_texture_create(jb, &d, &t), JB_OK);
        mat.texture = &t;
    }
    check("seam frame: begin", jb_begin_frame(jb, 0), JB_OK);
    if (fixture == 2) {
        jb_line3d l;
        memset(&l, 0, sizeof l);
        l.a.x = -1116.875f, l.a.y = 523, l.a.z = -191.25f;
        l.b.x = -107.625f, l.b.y = 351.625f, l.b.z = -865;
        l.color_a = JB_RGB(255, 16, 16), l.color_b = JB_RGB(0, 255, 180);
        check("seam frame: line", jb_draw_line3d(jb, &l), JB_OK);
    } else {
        check("seam frame: draw", jb_draw_mesh(jb, &mesh, &m), JB_OK);
    }
    check("seam frame: end", jb_end_frame(jb), JB_OK);
    px = jb_frame(jb, &w, &h);
    *count = 0;
    for (i = 0; i < w * h; i++) {
        *count += px[4 * i] || px[4 * i + 1] || px[4 * i + 2];
        for (k = 0; k < 4; k++)
            hash = (hash ^ px[4 * i + k]) * 1099511628211ull;
    }
    jb_destroy(jb);
    return hash;
}

static void seam_fix_cases(void)
{
    int n, n2;
    uint64_t one, two;

    check("seam fix cases: a cut textured triangle at 1x as before the seam fix",
          (long long)seam_frame(1, 1, 0, 1, &n), (long long)0xd8c0da89dc81d592ull);
    check("seam fix cases: and at 2x without precise vertices",
          (long long)seam_frame(1, 2, 0, 1, &n),
          (long long)0x10815916653d8796ull);
    check("seam fix cases: a clipped 3D line at 1x as before the seam fix",
          (long long)seam_frame(2, 1, 0, 1, &n),
          (long long)0x2304d1c52e967847ull);
    check("seam fix cases: and at 2x without precise vertices",
          (long long)seam_frame(2, 2, 0, 1, &n),
          (long long)0x830e37ab52f8469full);
    one = seam_frame(0, 4, 1, 0, &n);
    two = seam_frame(0, 4, 1, 1, &n2);
    check("seam fix cases: the face drawn single-sided at 4x precise", n > 0, 1);
    check("seam fix cases: as it is drawn double-sided", (long long)one, (long long)two);
}

int main(void)
{
    equivalence(1, 1, 0, 0, 0);
    equivalence(1, 1, 0, 0, 1);
    equivalence(2, 1, 1, 0, 0);
    equivalence(2, 4, 1, 1, 1);
    equivalence(1, 1, 0, 1, 0);
    equivalence(3, 1, 1, 0, 0);
    coverage(JB_GEOMETRY_GTE);
    coverage(JB_GEOMETRY_FLOAT);
    culling(JB_GEOMETRY_GTE);
    culling(JB_GEOMETRY_FLOAT);
    ordering(JB_GEOMETRY_GTE);
    ordering(JB_GEOMETRY_FLOAT);
    tri_behind_quad(JB_GEOMETRY_GTE);
    tri_behind_quad(JB_GEOMETRY_FLOAT);
    billboards(JB_GEOMETRY_GTE);
    billboards(JB_GEOMETRY_FLOAT);
    lines(JB_GEOMETRY_GTE);
    lines(JB_GEOMETRY_FLOAT);
    errors();
    api_edge_cases();
    clip_and_cover_cases();
    bias_and_plane_cases();
    property_cases();
    seam_fix_cases();
    return finish("api_draw3d");
}

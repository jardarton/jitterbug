/*
 * geom_pipeline: the friendly scene state and the 3D pipeline
 * (src/geom/scene.h, src/geom/pipeline.h), before any drawing.
 *
 *   - The public setters: defaults, every refusal with its detail, and
 *     nothing changed by a refusal.
 *   - jbi_geom_begin: the quantised registers for the default camera
 *     worked by hand, and each limit refused with its detail.
 *   - Equivalence: seeded random cameras, lights, fog, model matrices and
 *     meshes; every face through the GTE path, and through hand-written
 *     console code on a public context (jb_gte_write, jb_gte_command,
 *     jb_gte_read) loaded with the same quantised registers: vertices in
 *     Z order times 2^shift, RTPT (RTPS each with fog) and RTPS, NCLIP,
 *     AVSZ3/4, and NCCS, NCDS or DPCS per vertex. Where the hand-written
 *     code finds nothing saturated and every SZ within near_z..far_z, the
 *     face is exact: culled the same way, and the same positions, depths,
 *     colours, u, v and bucket. Elsewhere the pipeline must have clipped.
 *   - Every polygon of either path, exact or clipped: 3..10 vertices,
 *     positions in the guard band, sub-pixel positions within a pixel,
 *     depths within near..far, buckets in range.
 *   - Clipping by hand-built faces: across the near plane, wholly behind
 *     the camera, beyond the far plane, far off to the side.
 *   - The float path culling as the GTE path does, by NCLIP of its
 *     corners as rounded to the screen, on faces almost edge on; and the
 *     precise scaled pass by the same rule on the sub-pixel positions it
 *     draws (jbi_geom_poly.passes).
 *   - The float path against the GTE path on exact faces (within a couple
 *     of pixels and a few colour steps), and stored hashes of both paths'
 *     results over the seeded scenes, so every build gives the same.
 *   - jbi_geom_poly_prims, jbi_geom_point, jbi_geom_segment,
 *     jbi_geom_check_mesh, and the private GTE leaving the public one
 *     alone.
 */
#include "api_test.h"

#include "api/api.h"
#include "geom/pipeline.h"
#include "geom/scene.h"

#include <math.h>

#define GTE_PATH_HASH 0x0d6506a54952498dull
#define FLOAT_PATH_HASH 0xa53109717bfb1731ull

static char detail[256];

/* ------------------------------------------------------------------------ */
/* Setters */

static void setters(void)
{
    jb_context *jb = must_create(NULL);
    jbi_scene *s = &jb->api->scene;
    jb_camera c;
    jb_light l;

    check("default geometry", s->geometry, JB_GEOMETRY_GTE);
    check("default ambient", s->ambient, 0xffffff);
    check("default fog off", s->fog, 0);
    check("default fov", s->camera.fov_y == 60.0f, 1);
    check("default near", s->camera.z_near == 32.0f, 1);
    check("default far", s->camera.z_far == 4096.0f, 1);
    check("default looks down -z", s->camera.target.z == -1.0f, 1);

    memset(&c, 0, sizeof c);
    c.target.z = -10.0f;
    check("camera zeros", jb_set_camera(jb, &c), JB_OK);
    check("zeros: fov", s->camera.fov_y == 60.0f, 1);
    check("zeros: up", s->camera.up.y == 1.0f, 1);
    c.fov_y = 180.0f;
    check("fov 180", jb_set_camera(jb, &c), JB_ERROR_INVALID_ARGUMENT);
    check_str("fov 180: detail", jb_error_string(jb),
              "jb_set_camera: fov_y 180 is outside 0..180 degrees");
    c.fov_y = -1.0f;
    check("fov -1", jb_set_camera(jb, &c), JB_ERROR_INVALID_ARGUMENT);
    c.fov_y = 45.0f;
    c.z_near = 100.0f;
    c.z_far = 50.0f;
    check("far before near", jb_set_camera(jb, &c), JB_ERROR_INVALID_ARGUMENT);
    check_str("far before near: detail", jb_error_string(jb),
              "jb_set_camera: z_far 50 is not beyond z_near 100");
    c.z_far = 0.0f;
    c.z_near = 5000.0f;
    check("near beyond the default far", jb_set_camera(jb, &c), JB_ERROR_INVALID_ARGUMENT);
    c.z_near = -1.0f;
    check("near negative", jb_set_camera(jb, &c), JB_ERROR_INVALID_ARGUMENT);
    c.z_near = 0.0f;
    c.eye.y = 1e30f * 1e30f;
    check("not finite", jb_set_camera(jb, &c), JB_ERROR_INVALID_ARGUMENT);
    check_str("not finite: detail", jb_error_string(jb), "jb_set_camera: a value is not finite");
    c.eye.y = 0.0f;
    c.eye.z = c.target.z = 5.0f;
    check("eye at target", jb_set_camera(jb, &c), JB_ERROR_INVALID_ARGUMENT);
    c.target.y = 5.0f;
    check("up parallel", jb_set_camera(jb, &c), JB_ERROR_INVALID_ARGUMENT);
    check_str("up parallel: detail", jb_error_string(jb),
              "jb_set_camera: up is parallel to the view direction");
    check("refusals kept the camera", s->camera.target.z == -10.0f, 1);
    memset(&c, 0, sizeof c);
    c.fov_y = 45.0f;
    check("eye and target both zero", jb_set_camera(jb, &c), JB_OK);
    check("both zero: the default view down -z, the rest as given",
          s->camera.target.z == -1.0f && s->camera.eye.z == 0.0f && s->camera.fov_y == 45.0f, 1);
    check("camera NULL", jb_set_camera(jb, NULL), JB_OK);
    check("camera NULL: default", s->camera.target.z == -1.0f && s->camera.fov_y == 60.0f, 1);
    check("camera NULL jb", jb_set_camera(NULL, NULL), JB_ERROR_INVALID_ARGUMENT);

    check("ambient", jb_set_ambient(jb, 0x102030), JB_OK);
    check("ambient set", s->ambient, 0x102030);
    check("ambient too big", jb_set_ambient(jb, 0x1000000), JB_ERROR_INVALID_ARGUMENT);
    check_str("ambient: detail", jb_error_string(jb),
              "jb_set_ambient: color 0x1000000 is above 0xFFFFFF");
    check("ambient kept", s->ambient, 0x102030);
    check("ambient NULL jb", jb_set_ambient(NULL, 0), JB_ERROR_INVALID_ARGUMENT);

    memset(&l, 0, sizeof l);
    l.direction.y = -2.0f;
    l.color = 0xffffff;
    check("light", jb_set_light(jb, 1, &l), JB_OK);
    check("light points up towards the light",
          s->light_to[1].y == 1.0f && s->light_to[1].x == 0.0f, 1);
    check("light colour", s->light_color[1], 0xffffff);
    check("light index 3", jb_set_light(jb, 3, &l), JB_ERROR_INVALID_ARGUMENT);
    check_str("light index: detail", jb_error_string(jb),
              "jb_set_light: index 3 is outside 0..2");
    check("light index -1", jb_set_light(jb, -1, &l), JB_ERROR_INVALID_ARGUMENT);
    l.direction.y = 0.0f;
    check("light no direction", jb_set_light(jb, 1, &l), JB_ERROR_INVALID_ARGUMENT);
    check_str("light no direction: detail", jb_error_string(jb),
              "jb_set_light: direction (0, 0, 0) has no length");
    l.color = 0x1000000;
    l.direction.x = 1.0f;
    check("light colour too big", jb_set_light(jb, 1, &l), JB_ERROR_INVALID_ARGUMENT);
    check("light kept", s->light_color[1], 0xffffff);
    l.color = 0;
    l.direction.x = 0.0f;
    check("light colour 0 is off, any direction", jb_set_light(jb, 1, &l), JB_OK);
    check("light off", s->light_color[1], 0);
    check("light NULL is off", jb_set_light(jb, 0, NULL), JB_OK);
    check("light NULL jb", jb_set_light(NULL, 0, NULL), JB_ERROR_INVALID_ARGUMENT);

    check("fog", jb_set_fog(jb, 0x808080, 100.0f, 1000.0f), JB_OK);
    check("fog on", s->fog, 1);
    check("fog start 0", jb_set_fog(jb, 0, 0.0f, 10.0f), JB_ERROR_INVALID_ARGUMENT);
    check_str("fog: detail", jb_error_string(jb),
              "jb_set_fog: start 0 and end 10 are not 0 < start < end");
    check("fog end before start", jb_set_fog(jb, 0, 20.0f, 10.0f), JB_ERROR_INVALID_ARGUMENT);
    check("fog colour", jb_set_fog(jb, 0x1000000, 1.0f, 10.0f), JB_ERROR_INVALID_ARGUMENT);
    check("fog kept", s->fog == 1 && s->fog_start == 100.0f, 1);
    check("fog off", jb_set_fog(jb, 0, 5.0f, 0.0f), JB_OK);
    check("fog is off", s->fog, 0);
    check("fog NULL jb", jb_set_fog(NULL, 0, 0, 0), JB_ERROR_INVALID_ARGUMENT);

    check("geometry float", jb_set_geometry(jb, JB_GEOMETRY_FLOAT), JB_OK);
    check("geometry is float", s->geometry, JB_GEOMETRY_FLOAT);
    check("geometry 2", jb_set_geometry(jb, 2), JB_ERROR_INVALID_ARGUMENT);
    check_str("geometry: detail", jb_error_string(jb),
              "jb_set_geometry: mode 2 is not JB_GEOMETRY_GTE or JB_GEOMETRY_FLOAT");
    check("geometry kept", s->geometry, JB_GEOMETRY_FLOAT);
    check("geometry NULL jb", jb_set_geometry(NULL, 0), JB_ERROR_INVALID_ARGUMENT);
    jb_destroy(jb);
}

/* ------------------------------------------------------------------------ */
/* Set-up */

static void setup(void)
{
    jbi_scene s;
    jbi_geom_draw d;
    jb_mat4 m;
    jb_result r;

    jbi_scene_init(&s);
    r = jbi_geom_begin(&d, &s, NULL, 320, 240, 1024, 256, "t", detail, sizeof detail);
    check("default set-up", r, JB_OK);
    check("scale 4 (4096 x 8 > 32767)", d.scale == 4.0f, 1);
    check("H = round(120 / tan 30)", d.r.h, 208);
    check("near: 32 x 4", d.near_z, 128);
    check("far: 4096 x 4", d.far_z, 16384);
    check("OFX", d.r.ofx, 160 << 16);
    check("OFY", d.r.ofy, 120 << 16);
    /* ZSF3 : ZSF4 = 4 : 3, k = 65535 x 4096 / (12 x 16384) = 1365. */
    check("ZSF3", d.r.zsf3, 4 * 1365);
    check("ZSF4", d.r.zsf4, 3 * 1365);
    check("OTZ of the near plane", d.otz_near, (12 * 1365 * 128) >> 12);
    check("OTZ of the far plane", d.otz_far, (12 * 1365 * 16384) >> 12);
    check("shift 0", d.shift, 0);
    check("rotation: x kept", d.r.rt.m[0][0], 4 * 4096);
    check("rotation: y flipped", d.r.rt.m[1][1], -4 * 4096);
    check("rotation: z flipped", d.r.rt.m[2][2], -4 * 4096);
    check("rotation: off the diagonal", d.r.rt.m[0][1] | d.r.rt.m[1][2] | d.r.rt.m[2][0], 0);
    check("translation 0", d.r.tr[0] | d.r.tr[1] | d.r.tr[2], 0);
    check("ambient white", d.r.bk[0] == 4096 && d.r.bk[2] == 4096, 1);
    check("no lights", d.r.lcm.m[0][0] | d.r.llm.m[0][0], 0);

    /* A model scaled by 3.9 needs the vertices doubled: 4 x 3.9 x 4096 is
       beyond 16 bits, half of it is not. */
    m = jb_mat4_scale(3.9f, 3.9f, 3.9f);
    r = jbi_geom_begin(&d, &s, &m, 320, 240, 1024, 1000, "t", detail, sizeof detail);
    check("scale 3.9: ok", r, JB_OK);
    check("scale 3.9: shift 1", d.shift, 1);
    check("scale 3.9: element", d.r.rt.m[0][0], 31949);
    r = jbi_geom_begin(&d, &s, &m, 320, 240, 1024, 20000, "t", detail, sizeof detail);
    check("scale 3.9, extent 20000: refused", r, JB_ERROR_INVALID_ARGUMENT);
    check_str("extent: detail", detail,
              "t: the mesh (coordinates up to 20000) is too large for the GTE with this model "
              "matrix and camera: model x view needs its vertices x 2, beyond 16 bits");
    m = jb_mat4_translate(0.0f, 0.0f, -1e9f);
    r = jbi_geom_begin(&d, &s, &m, 320, 240, 1024, 10, "t", detail, sizeof detail);
    check("far translation: refused", r, JB_ERROR_INVALID_ARGUMENT);
    s.fog = 1;
    s.fog_color = 0x808080;
    s.fog_start = 3000.0f;
    s.fog_end = 3001.0f;
    r = jbi_geom_begin(&d, &s, NULL, 320, 240, 1024, 10, "t", detail, sizeof detail);
    check("sharp fog: refused", r, JB_ERROR_INVALID_ARGUMENT);
    s.fog_start = 100.0f;
    s.fog_end = 1000.0f;
    r = jbi_geom_begin(&d, &s, NULL, 320, 240, 1024, 10, "t", detail, sizeof detail);
    check("fog: ok", r, JB_OK);
    /* IR0 = 4096 zf (z - zn) / (z (zf - zn)), zn 400, zf 4000 GTE units:
       0 at 400, 4096 at 4000. */
    {
        int ov;
        int64_t q = jbi_gte_divide(208, 400, &ov), q2 = jbi_gte_divide(208, 4000, &ov);
        int64_t i0 = ((int64_t)d.r.dqa * q + d.r.dqb) >> 12;
        int64_t i1 = ((int64_t)d.r.dqa * q2 + d.r.dqb) >> 12;

        check("fog: none at its start", i0 >= -4 && i0 <= 4, 1);
        check("fog: full at its end", i1 >= 4092 && i1 <= 4100, 1);
        check("fog colour", d.r.fc[0], 128 * 16);
    }
    s.fog = 0;
    s.camera.fov_y = 0.001f;
    r = jbi_geom_begin(&d, &s, NULL, 320, 240, 1024, 10, "t", detail, sizeof detail);
    check("narrow fov: refused", r, JB_ERROR_INVALID_ARGUMENT);
    s.geometry = JB_GEOMETRY_FLOAT;
    r = jbi_geom_begin(&d, &s, NULL, 320, 240, 1024, 10, "t", detail, sizeof detail);
    check("narrow fov: fine for the float path", r, JB_OK);
    s.geometry = JB_GEOMETRY_GTE;
    s.camera.fov_y = 60.0f;
    s.camera.z_far = 40.0f;
    s.camera.z_near = 0.1f;
    r = jbi_geom_begin(&d, &s, NULL, 320, 240, 1024, 10, "t", detail, sizeof detail);
    check("small far: ok", r, JB_OK);
    check("small far: scale 512", d.scale == 512.0f, 1);
    check("small far: near is H / 2 + 1", d.near_z, 105);
    s.camera.z_far = 1e-3f;
    s.camera.z_near = 1e-4f;
    r = jbi_geom_begin(&d, &s, NULL, 320, 240, 1024, 10, "t", detail, sizeof detail);
    check("tiny far: matrix beyond range", r, JB_ERROR_INVALID_ARGUMENT);
}

/* Whether the pipeline's got polygons p are drawn by the 1x pass (and the
   scaled one without precise vertices): some, and not for the precise
   scaled pass alone (jbi_geom_poly.passes). */
static int drawn_1x(const jbi_geom_poly *p, int got)
{
    return got > 0 && (p[0].passes == 0 || (p[0].passes & JBI_GEOM_PASS_1X) != 0);
}

static int drawn_precise(const jbi_geom_poly *p, int got)
{
    return got > 0 && (p[0].passes == 0 || (p[0].passes & JBI_GEOM_PASS_PRECISE) != 0);
}

/* ------------------------------------------------------------------------ */
/* Random scenes */

enum { VERTS = 24, FACES = 40, MATS = 6 };

typedef struct scene_case {
    jbi_scene s;
    jb_mat4 model;
    jb_gte_vec pos[VERTS], nrm[VERTS];
    uint32_t col[VERTS];
    uint8_t uv[VERTS * 2];
    jb_mesh_face faces[FACES];
    jb_material mats[MATS];
    jb_mesh mesh;
    int w, h, ot;
} scene_case;

static float frand(gen *g, float lo, float hi)
{
    return lo + (hi - lo) * (float)rnd_below(g, 100001) / 100000.0f;
}

static void random_case(gen *g, scene_case *c, int mode)
{
    int i, k;
    jb_camera cam;
    jb_vec3 t, r, sc;
    float size;

    memset(c, 0, sizeof *c);
    jbi_scene_init(&c->s);
    c->s.geometry = mode;
    c->w = rnd_below(g, 4) == 0 ? 640 : 320;
    c->h = rnd_below(g, 4) == 0 ? 480 : 240;
    c->ot = 1 + rnd_below(g, 2048);
    memset(&cam, 0, sizeof cam);
    cam.eye.x = frand(g, -500, 500);
    cam.eye.y = frand(g, -200, 500);
    cam.eye.z = frand(g, -500, 500);
    cam.target.x = frand(g, -100, 100);
    cam.target.y = frand(g, -100, 100);
    cam.target.z = frand(g, -100, 100);
    cam.fov_y = frand(g, 30, 100);
    cam.z_near = frand(g, 1, 100);
    cam.z_far = frand(g, 1500, 9000);
    jbi_scene_set_camera(&c->s, &cam, detail, sizeof detail);
    jbi_scene_set_ambient(&c->s, rnd(g) & 0x7f7f7f, detail, sizeof detail);
    for (i = 0; i < 3; i++)
        if (rnd_below(g, 2)) {
            jb_light l;

            memset(&l, 0, sizeof l); /* every field, intensity too (1.0) */
            l.direction.x = frand(g, -1, 1);
            l.direction.y = frand(g, -1, 1);
            l.direction.z = frand(g, -1, 1) + 0.01f;
            l.color = rnd(g) & 0xffffff;
            jbi_scene_set_light(&c->s, i, &l, detail, sizeof detail);
        }
    if (rnd_below(g, 3) == 0) {
        uint32_t fc = rnd(g) & 0xffffff;
        float start = frand(g, 50, 400);
        float end = frand(g, 900, 3000);

        jbi_scene_set_fog(&c->s, fc, start, end, detail, sizeof detail);
    }
    t.x = frand(g, -150, 150);
    t.y = frand(g, -150, 150);
    t.z = frand(g, -150, 150);
    r.x = frand(g, -3, 3);
    r.y = frand(g, -3, 3);
    r.z = frand(g, -3, 3);
    sc.x = frand(g, 0.05f, 1.5f);
    sc.y = rnd_below(g, 2) ? sc.x : frand(g, 0.05f, 1.5f);
    sc.z = rnd_below(g, 2) ? sc.x : frand(g, 0.05f, 1.5f);
    c->model = jb_mat4_trs(t, r, sc);
    size = frand(g, 20, 3000);
    for (i = 0; i < VERTS; i++) {
        c->pos[i].x = (int16_t)frand(g, -size, size);
        c->pos[i].y = (int16_t)frand(g, -size, size);
        c->pos[i].z = (int16_t)frand(g, -size, size);
        {
            jb_vec3 n;

            /* One call a statement: the order of the calls in an
               initializer list is unspecified. */
            n.x = frand(g, -1, 1);
            n.y = frand(g, -1, 1);
            n.z = frand(g, -1, 1);
            n = jb_vec3_normalize(n);
            c->nrm[i].x = (int16_t)(n.x * 4096.0f);
            c->nrm[i].y = (int16_t)(n.y * 4096.0f);
            c->nrm[i].z = (int16_t)(n.z * 4096.0f);
        }
        c->col[i] = rnd(g) & 0xffffff;
        c->uv[2 * i] = (uint8_t)rnd(g);
        c->uv[2 * i + 1] = (uint8_t)rnd(g);
    }
    for (i = 0; i < MATS; i++) {
        c->mats[i].color = rnd_below(g, 3) == 0 ? 0 : rnd(g) & 0xffffff;
        c->mats[i].unlit = rnd_below(g, 3) == 0;
        c->mats[i].double_sided = rnd_below(g, 4) == 0;
        c->mats[i].blend = rnd_below(g, 5);
    }
    for (i = 0; i < FACES; i++) {
        c->faces[i].vertices = (uint16_t)(3 + rnd_below(g, 2));
        for (k = 0; k < 4; k++)
            c->faces[i].v[k] = (uint16_t)rnd_below(g, VERTS);
        c->faces[i].material = (uint16_t)rnd_below(g, MATS);
    }
    c->mesh.vertex_count = VERTS;
    c->mesh.positions = c->pos;
    c->mesh.normals = rnd_below(g, 3) ? c->nrm : NULL;
    c->mesh.colors = rnd_below(g, 3) == 0 ? c->col : NULL;
    c->mesh.uv = rnd_below(g, 2) ? c->uv : NULL;
    c->mesh.face_count = FACES;
    c->mesh.faces = c->faces;
    c->mesh.material_count = MATS;
    c->mesh.materials = c->mats;
}

/* The hand-written console code's registers. */
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

/* The face's flat normal as the header defines it: the cross product of
   the edges from vertex 0 (a triangle) or of the diagonals (a quad),
   normalised, in 4.12. */
static jb_gte_vec flat_normal(const jb_mesh *m, const jb_mesh_face *f)
{
    const jb_gte_vec *p = m->positions;
    jb_vec3 a, b, n;
    jb_gte_vec o;
    int quad = f->vertices == 4;
    int i0 = f->v[0], i1 = f->v[1], i2 = f->v[2], i3 = quad ? f->v[3] : 0;

    if (quad) {
        a.x = (float)(p[i2].x - p[i0].x);
        a.y = (float)(p[i2].y - p[i0].y);
        a.z = (float)(p[i2].z - p[i0].z);
        b.x = (float)(p[i3].x - p[i1].x);
        b.y = (float)(p[i3].y - p[i1].y);
        b.z = (float)(p[i3].z - p[i1].z);
    } else {
        a.x = (float)(p[i1].x - p[i0].x);
        a.y = (float)(p[i1].y - p[i0].y);
        a.z = (float)(p[i1].z - p[i0].z);
        b.x = (float)(p[i2].x - p[i0].x);
        b.y = (float)(p[i2].y - p[i0].y);
        b.z = (float)(p[i2].z - p[i0].z);
    }
    n = jb_vec3_normalize(jb_vec3_cross(a, b));
    o.x = (int16_t)((double)n.x * 4096.0 + (n.x >= 0 ? 0.5 : -0.5));
    o.y = (int16_t)((double)n.y * 4096.0 + (n.y >= 0 ? 0.5 : -0.5));
    o.z = (int16_t)((double)n.z * 4096.0 + (n.z >= 0 ? 0.5 : -0.5));
    return o;
}

#define SAT_FLAGS                                                                                \
    (0x7e000000u | JB_GTE_FLAG_IR1 | JB_GTE_FLAG_IR2 | JB_GTE_FLAG_SZ_OTZ | JB_GTE_FLAG_DIVIDE | \
     JB_GTE_FLAG_MAC0_POS | JB_GTE_FLAG_MAC0_NEG | JB_GTE_FLAG_SX2 | JB_GTE_FLAG_SY2)

static struct {
    int faces, exact, exact_culled, clipped, clip_culled, refused, float_compared;
    int max_dpos, max_dcolor;
} st;

/* The bucket console code puts an OTZ in: linear between the near and far
   planes' OTZ (the header's ordering). */
static int ot_bucket(const jbi_geom_draw *d, uint32_t otz)
{
    int64_t range = (int64_t)d->otz_far - d->otz_near + 1;
    int64_t b = ((int64_t)otz - d->otz_near) * d->ot_size;

    if (b <= 0)
        return 0;
    b /= range;
    return (int)(b > d->ot_size - 1 ? d->ot_size - 1 : b);
}

/* What console code makes of the face (cyclic vertex i at out[i]); 1 when
   it is exact, with *culled. */
static int hand_face(jb_context *jb, const jbi_geom_draw *d, const jb_mesh *m, int fi,
                     int *culled, jbi_geom_vertex out[4], int *bucket)
{
    static const int zorder[4] = {0, 1, 3, 2};
    const jb_mesh_face *f = &m->faces[fi];
    const jb_material *mat = &m->materials[f->material];
    int n = f->vertices, k, sh = d->shift, exact = 1;
    int idx[4];
    uint32_t sxy[4], flags = 0, otz;
    int32_t sz[4], ir0[4], mac0;

    for (k = 0; k < n; k++)
        idx[k] = f->v[n == 4 ? zorder[k] : k];
#define P(k) m->positions[idx[k]].x *(1 << sh), m->positions[idx[k]].y *(1 << sh), \
             m->positions[idx[k]].z *(1 << sh)
    if (d->fog) {
        for (k = 0; k < 3; k++) {
            hand_vertex(jb, 0, P(k));
            jb_gte_command(jb, JB_GTE_RTPS);
            flags |= jb_gte_read(jb, JB_GTE_FLAG);
            sxy[k] = jb_gte_read(jb, JB_GTE_SXY2);
            sz[k] = (int32_t)jb_gte_read(jb, JB_GTE_SZ3);
            ir0[k] = (int32_t)jb_gte_read(jb, JB_GTE_IR0);
        }
    } else {
        for (k = 0; k < 3; k++)
            hand_vertex(jb, k, P(k));
        jb_gte_command(jb, JB_GTE_RTPT);
        flags |= jb_gte_read(jb, JB_GTE_FLAG);
        for (k = 0; k < 3; k++) {
            sxy[k] = jb_gte_read(jb, JB_GTE_SXY0 + k);
            sz[k] = (int32_t)jb_gte_read(jb, JB_GTE_SZ1 + k);
            ir0[k] = 0;
        }
    }
    jb_gte_command(jb, JB_GTE_NCLIP);
    mac0 = (int32_t)jb_gte_read(jb, JB_GTE_MAC0);
    if (n == 4) {
        hand_vertex(jb, 0, P(3));
        jb_gte_command(jb, JB_GTE_RTPS);
        flags |= jb_gte_read(jb, JB_GTE_FLAG);
        sxy[3] = jb_gte_read(jb, JB_GTE_SXY2);
        sz[3] = (int32_t)jb_gte_read(jb, JB_GTE_SZ3);
        ir0[3] = (int32_t)jb_gte_read(jb, JB_GTE_IR0);
    }
#undef P
    jb_gte_command(jb, n == 4 ? JB_GTE_AVSZ4 : JB_GTE_AVSZ3);
    otz = jb_gte_read(jb, JB_GTE_OTZ);
    if (flags & SAT_FLAGS)
        exact = 0;
    (void)sz;
    /* And inside the guard band, as the header's exact faces are: the
       screen position of the GTE's exact sums (TR x 4096 + RT x V) within
       JBI_GEOM_GUARD_MIN..MAX, tested as the clipping planes are. */
    for (k = 0; k < n && exact; k++) {
        double sum[3], h = (double)d->r.h, ox = d->width * 0.5, oy = d->height * 0.5;
        double lo = JBI_GEOM_GUARD_MIN, hi = JBI_GEOM_GUARD_MAX, t;
        int r;

        for (r = 0; r < 3; r++)
            sum[r] = (double)((int64_t)d->r.tr[r] * 4096 +
                              (int64_t)d->r.rt.m[r][0] * (m->positions[idx[k]].x * (1 << sh)) +
                              (int64_t)d->r.rt.m[r][1] * (m->positions[idx[k]].y * (1 << sh)) +
                              (int64_t)d->r.rt.m[r][2] * (m->positions[idx[k]].z * (1 << sh)));
        t = h * sum[0];
        t = t + 0.0 * sum[1];
        t = t + (ox - lo) * sum[2];
        exact &= t + 0.0 >= 0.0;
        t = -h * sum[0];
        t = t + 0.0 * sum[1];
        t = t + (hi - ox) * sum[2];
        exact &= t + 0.0 >= 0.0;
        t = 0.0 * sum[0];
        t = t + h * sum[1];
        t = t + (oy - lo) * sum[2];
        exact &= t + 0.0 >= 0.0;
        t = 0.0 * sum[0];
        t = t + -h * sum[1];
        t = t + (hi - oy) * sum[2];
        exact &= t + 0.0 >= 0.0;
        /* The near and far planes at their unrounded depths (the header's
           clipping), as the planes are tested: 0 x + 0 y + z - near. */
        t = 0.0 * sum[0];
        t = t + 0.0 * sum[1];
        t = t + 1.0 * sum[2];
        exact &= t + -(d->near_f * 4096.0) >= 0.0;
        t = 0.0 * sum[0];
        t = t + 0.0 * sum[1];
        t = t + -1.0 * sum[2];
        exact &= t + d->far_f * 4096.0 >= 0.0;
    }
    if (!exact)
        return 0;
    /* A quad whose first triangle has no area is judged by its second. */
    if (mac0 == 0 && n == 4)
        mac0 = jb_gte_nclip(sxy[1], sxy[3], sxy[2], NULL);
    *culled = mac0 >= 0 && !mat->double_sided;
    *bucket = ot_bucket(d, otz);
    for (k = 0; k < n; k++) {
        jbi_geom_vertex *v = &out[n == 4 ? zorder[k] : k];
        uint32_t c = m->colors ? m->colors[idx[k]] : mat->color ? mat->color : 0x808080u;
        jb_gte_vec nrm = m->normals ? m->normals[idx[k]] : flat_normal(m, f);

        if (!mat->unlit) {
            jb_gte_write(jb, JB_GTE_RGBC, c);
            hand_vertex(jb, 0, nrm.x, nrm.y, nrm.z);
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
        v->x = JB_GTE_SX(sxy[k]);
        v->y = JB_GTE_SY(sxy[k]);
        v->z = sz[k];
        v->color = c;
        v->u = m->uv ? m->uv[2 * idx[k]] : 0;
        v->v = m->uv ? m->uv[2 * idx[k] + 1] : 0;
    }
    return 1;
}

/* What every polygon must be. */
static void invariants(const jbi_geom_draw *d, const jbi_geom_poly *p, const char *what)
{
    int i, ok = p->n >= 3 && p->n <= JBI_GEOM_POLY_MAX && p->bucket >= 0 &&
                p->bucket < d->ot_size;
    int32_t zlo = d->mode == JB_GEOMETRY_GTE ? (int32_t)d->near_f : (int32_t)d->fnear - 1;
    int32_t zhi = d->mode == JB_GEOMETRY_GTE ? d->far_z : (int32_t)d->ffar + 1;

    for (i = 0; i < p->n && ok; i++) {
        const jbi_geom_vertex *v = &p->v[i];
        int dx = v->sx - v->x * 256, dy = v->sy - v->y * 256;

        ok &= v->x >= -1024 && v->x <= 1023 && v->y >= -1024 && v->y <= 1023;
        ok &= dx >= -256 && dx < 512 && dy >= -256 && dy < 512;
        ok &= v->z >= zlo && v->z <= zhi && v->z >= 1;
        ok &= v->color <= 0xffffff && v->u >= 0 && v->u <= 255 && v->v >= 0 && v->v <= 255;
        if (!p->exact && d->mode == JB_GEOMETRY_FLOAT)
            ok &= v->x >= JBI_GEOM_GUARD_MIN - 2 && v->x <= JBI_GEOM_GUARD_MAX + 2 &&
                  v->y >= JBI_GEOM_GUARD_MIN - 2 && v->y <= JBI_GEOM_GUARD_MAX + 2;
    }
    if (!check(what, ok, 1)) {
        printf("  mode %d n %d bucket %d exact %d, z %d..%d:", d->mode, p->n, p->bucket,
               p->exact, zlo, zhi);
        for (i = 0; i < p->n; i++)
            printf(" (%d %d %d/%d %d/%d c%06x uv %d %d)", p->v[i].x, p->v[i].y, p->v[i].sx,
                   p->v[i].sy, p->v[i].z, 0, (unsigned)p->v[i].color, p->v[i].u, p->v[i].v);
        printf("\n");
    }
}

static uint64_t fnv(uint64_t h, uint32_t v)
{
    int i;

    for (i = 0; i < 4; i++) {
        h ^= (v >> (8 * i)) & 0xffu;
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint64_t hash_poly(uint64_t h, int got, const jbi_geom_poly *p)
{
    int i;

    h = fnv(h, (uint32_t)got);
    if (!got)
        return h;
    h = fnv(h, (uint32_t)p->n);
    h = fnv(h, (uint32_t)p->bucket);
    h = fnv(h, (uint32_t)p->exact);
    for (i = 0; i < p->n; i++) {
        const jbi_geom_vertex *v = &p->v[i];

        h = fnv(h, (uint32_t)v->x);
        h = fnv(h, (uint32_t)v->y);
        h = fnv(h, (uint32_t)v->sx);
        h = fnv(h, (uint32_t)v->sy);
        h = fnv(h, (uint32_t)v->z);
        h = fnv(h, v->color);
        h = fnv(h, (uint32_t)(v->u | v->v << 8));
    }
    return h;
}

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

static int color_diff(uint32_t a, uint32_t b)
{
    int i, m = 0;

    for (i = 0; i < 3; i++) {
        int d = iabs((int)((a >> (8 * i)) & 255) - (int)((b >> (8 * i)) & 255));

        m = d > m ? d : m;
    }
    return m;
}

static void equivalence(uint64_t seed, int cases)
{
    jb_context *jb = must_create(NULL);
    gen g = {0, 0, 0};
    uint64_t hg = 0xcbf29ce484222325ull, hf = 0xcbf29ce484222325ull;
    int c, fi, i;
    static scene_case sc;

    g.s = seed;
    for (c = 0; c < cases; c++) {
        jbi_geom_draw d, fd;
        jbi_scene fs;
        int extent;
        jb_result r;

        random_case(&g, &sc, JB_GEOMETRY_GTE);
        check("random mesh is valid",
              jbi_geom_check_mesh(&sc.mesh, "t", &extent, detail, sizeof detail), JB_OK);
        r = jbi_geom_begin(&d, &sc.s, &sc.model, sc.w, sc.h, sc.ot, extent, "t", detail,
                           sizeof detail);
        fs = sc.s;
        fs.geometry = JB_GEOMETRY_FLOAT;
        check("float set-up never fails",
              jbi_geom_begin(&fd, &fs, &sc.model, sc.w, sc.h, sc.ot, extent, "t", detail,
                             sizeof detail),
              JB_OK);
        if (r != JB_OK) {
            st.refused++;
            continue;
        }
        jbi_geom_load(&sc.s.gte, &d);
        hand_load(jb, &d);
        for (fi = 0; fi < FACES; fi++) {
            jbi_geom_poly pp[2], qq[2], p, q;
            jbi_geom_vertex hv[4];
            int culled = 0, bucket = 0, got, fgot, k, g1, f1;
            char what[96];

            st.faces++;
            got = jbi_geom_face(&sc.s.gte, &d, &sc.mesh, fi, pp);
            fgot = jbi_geom_face(NULL, &fd, &sc.mesh, fi, qq);
            snprintf(what, sizeof what, "case %d face %d", c, fi);
            check(what, got >= 0 && got <= (sc.faces[fi].vertices == 4 ? 2 : 1), 1);
            check(what, fgot >= 0 && fgot <= (sc.faces[fi].vertices == 4 ? 2 : 1), 1);
            /* Hashed: what the 1x pass draws (a face only the precise
               scaled pass draws counts as none). */
            g1 = drawn_1x(pp, got) ? got : 0;
            f1 = drawn_1x(qq, fgot) ? fgot : 0;
            hg = fnv(hg, (uint32_t)g1);
            hf = fnv(hf, (uint32_t)f1);
            for (k = 0; k < got; k++) {
                if (k < g1)
                    hg = hash_poly(hg, 1, &pp[k]);
                invariants(&d, &pp[k], what);
            }
            for (k = 0; k < fgot; k++) {
                if (k < f1)
                    hf = hash_poly(hf, 1, &qq[k]);
                invariants(&fd, &qq[k], what);
            }
            p = pp[0];
            q = qq[0];
            if (hand_face(jb, &d, &sc.mesh, fi, &culled, hv, &bucket)) {
                st.exact++;
                if (culled) {
                    st.exact_culled++;
                    check(what, g1, 0);
                    continue;
                }
                if (!check(what, g1, 1))
                    continue;
                check(what, p.exact, 1);
                check(what, p.n, sc.faces[fi].vertices);
                check(what, p.bucket, bucket);
                for (i = 0; i < p.n; i++) {
                    check(what, p.v[i].x, hv[i].x);
                    check(what, p.v[i].y, hv[i].y);
                    check(what, p.v[i].z, hv[i].z);
                    check(what, p.v[i].color, hv[i].color);
                    check(what, p.v[i].u, hv[i].u);
                    check(what, p.v[i].v, hv[i].v);
                }
                /* The float path, for faces that are clearly front faces
                   and not clipped there either. */
                if (fgot == 1 && q.exact && q.n == p.n) {
                    int32_t area = jb_gte_nclip(JB_GTE_PAIR(hv[0].x, hv[0].y),
                                                JB_GTE_PAIR(hv[1].x, hv[1].y),
                                                JB_GTE_PAIR(hv[2].x, hv[2].y), NULL);

                    if (area < -2000 || area > 2000) {
                        st.float_compared++;
                        for (i = 0; i < p.n; i++) {
                            /* The GTE's whole-unit IR and truncated SZ move a vertex by
                               up to about (its distance from the centre + H) / SZ. */
                            int off = iabs(p.v[i].x - sc.w / 2) + iabs(p.v[i].y - sc.h / 2);
                            int tol = 4 + 2 * (off + d.r.h) / p.v[i].z;
                            int dp = iabs(q.v[i].x - p.v[i].x) + iabs(q.v[i].y - p.v[i].y) -
                                     tol;
                            int dc = color_diff(q.v[i].color, p.v[i].color);

                            if (getenv("JB_DEBUG") && (dp > 4 || dc > 8))
                                printf("dbg case %d face %d v%d: gte (%d %d z%d c%06x) float "
                                       "(%d %d z%d c%06x) fog %d lit %d h %d fh %g sc %g shift %d\n",
                                       c, fi, i, p.v[i].x, p.v[i].y, p.v[i].z,
                                       (unsigned)p.v[i].color, q.v[i].x, q.v[i].y, q.v[i].z,
                                       (unsigned)q.v[i].color, d.fog,
                                       !sc.mats[sc.faces[fi].material].unlit, d.r.h,
                                       (double)fd.fh, (double)d.scale, d.shift);
                            st.max_dpos = dp > st.max_dpos ? dp : st.max_dpos;
                            st.max_dcolor = dc > st.max_dcolor ? dc : st.max_dcolor;
                        }
                    }
                }
            } else {
                if (g1) {
                    st.clipped++;
                    for (k = 0; k < got; k++)
                        check(what, pp[k].exact, 0);
                } else {
                    st.clip_culled++;
                }
            }
        }
    }
    printf("equivalence: %d faces; %d exact (%d culled), %d clipped, %d culled or gone; "
           "%d set-ups refused\n",
           st.faces, st.exact, st.exact_culled, st.clipped, st.clip_culled, st.refused);
    printf("float against GTE on %d exact faces: %d pixels (|dx| + |dy|) beyond the GTE's "
           "precision, %d colour steps\n",
           st.float_compared, st.max_dpos, st.max_dcolor);
    check("enough exact faces", st.exact - st.exact_culled > 1000, 1);
    check("enough clipped faces", st.clipped > 300, 1);
    check("float compared with the GTE on enough faces", st.float_compared > 1000, 1);
    check("float near the GTE: positions", st.max_dpos <= 0, 1);
    check("float near the GTE: colours", st.max_dcolor <= 8, 1);
    if (!check("stored GTE path hash", (long long)hg, (long long)GTE_PATH_HASH))
        printf("GTE path hash: 0x%016llxull\n", (unsigned long long)hg);
    if (!check("stored float path hash", (long long)hf, (long long)FLOAT_PATH_HASH))
        printf("float path hash: 0x%016llxull\n", (unsigned long long)hf);
    /* The private GTE is not the context's own. */
    {
        int reg, zero = 1;
        jb_context *other = must_create(NULL);
        jbi_scene *s = &other->api->scene;
        jbi_geom_poly p[2];
        jbi_geom_draw d2;
        int extent;

        jbi_geom_check_mesh(&sc.mesh, "t", &extent, detail, sizeof detail);
        jbi_scene_set_camera(s, NULL, detail, sizeof detail);
        if (jbi_geom_begin(&d2, s, NULL, 320, 240, 16, extent, "t", detail, sizeof detail) ==
            JB_OK) {
            jbi_geom_load(&s->gte, &d2);
            for (i = 0; i < FACES; i++)
                jbi_geom_face(&s->gte, &d2, &sc.mesh, i, p);
        }
        for (reg = 0; reg < 64; reg++)
            zero &= jb_gte_read(other, reg) == 0;
        check("the public GTE untouched", zero, 1);
        jb_destroy(other);
    }
    jb_destroy(jb);
}

/* ------------------------------------------------------------------------ */
/* Hand-built clipping cases, camera at the origin looking down -z. */

static void clipping(int mode)
{
    jbi_scene s;
    jbi_geom_draw d;
    jbi_geom_poly p[2];
    jb_gte_vec pos[4];
    jb_mesh_face f = {{0, 1, 2, 3}, 4, 0};
    jb_material mat;
    jb_mesh m;
    int got, i, k, minz = 1 << 30;
    const char *name = mode == JB_GEOMETRY_GTE ? "GTE" : "float";
    char what[96];

    jbi_scene_init(&s);
    s.geometry = mode;
    memset(&mat, 0, sizeof mat);
    memset(&m, 0, sizeof m);
    m.vertex_count = 4;
    m.positions = pos;
    m.face_count = 1;
    m.faces = &f;
    m.material_count = 1;
    m.materials = &mat;
    /* A floor from behind the camera to far ahead, anticlockwise seen from
       above: (-500, -50, 100), (500, -50, 100), (500, -50, -3000),
       (-500, -50, -3000). */
    pos[0].x = -500, pos[0].y = -50, pos[0].z = 100;
    pos[1].x = 500, pos[1].y = -50, pos[1].z = 100;
    pos[2].x = 500, pos[2].y = -50, pos[2].z = -3000;
    pos[3].x = -500, pos[3].y = -50, pos[3].z = -3000;
    check("clip set-up", jbi_geom_begin(&d, &s, NULL, 320, 240, 256, 3000, "t", detail,
                                        sizeof detail),
          JB_OK);
    jbi_geom_load(&s.gte, &d);
    got = jbi_geom_face(&s.gte, &d, &m, 0, p);
    snprintf(what, sizeof what, "%s: floor through the near plane", name);
    check(what, got, 2); /* the quad's two triangles, each cut */
    for (k = 0; k < got; k++) {
        invariants(&d, &p[k], what);
        check(what, p[k].exact, 0);
        for (i = 0; i < p[k].n; i++) {
            minz = p[k].v[i].z < minz ? p[k].v[i].z : minz;
            check(what, p[k].v[i].y > 120, 1); /* below the horizon */
        }
    }
    check(what, minz, mode == JB_GEOMETRY_GTE ? d.near_z : (int)(d.fnear + 0.5f));
    /* Seen from below it is a back face. */
    for (i = 0; i < 4; i++)
        pos[i].y = 50;
    snprintf(what, sizeof what, "%s: floor seen from below", name);
    check(what, jbi_geom_face(&s.gte, &d, &m, 0, p), 0);
    mat.double_sided = 1;
    snprintf(what, sizeof what, "%s: double-sided, from below", name);
    check(what, jbi_geom_face(&s.gte, &d, &m, 0, p), 2);
    mat.double_sided = 0;
    /* Wholly behind the camera. */
    for (i = 0; i < 4; i++) {
        pos[i].y = -50;
        pos[i].z = (int16_t)(i < 2 ? 300 : 100);
    }
    snprintf(what, sizeof what, "%s: behind the camera", name);
    check(what, jbi_geom_face(&s.gte, &d, &m, 0, p), 0);
    /* Beyond the far plane (4096). */
    for (i = 0; i < 4; i++)
        pos[i].z = (int16_t)(i < 2 ? -5000 : -9000);
    check("beyond the far plane: set-up",
          jbi_geom_begin(&d, &s, NULL, 320, 240, 256, 9000, "t", detail, sizeof detail), JB_OK);
    jbi_geom_load(&s.gte, &d);
    snprintf(what, sizeof what, "%s: beyond the far plane", name);
    check(what, jbi_geom_face(&s.gte, &d, &m, 0, p), 0);
    /* Far off to the side, in front: clipped by the guard band. */
    pos[0].x = -50, pos[0].y = 0, pos[0].z = -100;
    pos[1].x = 30000, pos[1].y = 0, pos[1].z = -100;
    pos[2].x = 30000, pos[2].y = 100, pos[2].z = -100;
    pos[3].x = -50, pos[3].y = 100, pos[3].z = -100;
    check("to the side: set-up",
          jbi_geom_begin(&d, &s, NULL, 320, 240, 256, 30000, "t", detail, sizeof detail),
          JB_OK);
    {
        jb_mat4 sm = jb_mat4_scale(0.25f, 0.25f, 0.25f);
        jb_mat4 tr = jb_mat4_translate(0.0f, 0.0f, -75.0f);

        sm = jb_mat4_mul(&tr, &sm);
        check("to the side, scaled: set-up",
              jbi_geom_begin(&d, &s, &sm, 320, 240, 256, 30000, "t", detail, sizeof detail),
              JB_OK);
        jbi_geom_load(&s.gte, &d);
        got = jbi_geom_face(&s.gte, &d, &m, 0, p);
        snprintf(what, sizeof what, "%s: to the side", name);
        check(what, got >= 1, 1);
        for (k = 0; k < got; k++) {
            invariants(&d, &p[k], what);
            check(what, p[k].exact, 0);
            for (i = 0; i < p[k].n; i++)
                check(what, p[k].v[i].x <= JBI_GEOM_GUARD_MAX + 1, 1);
        }
    }
}

static void prims(void)
{
    jbi_geom_poly p;
    jbi_geom_prim out[JBI_GEOM_PRIMS_MAX];
    int n, i, count, tris;

    memset(&p, 0, sizeof p);
    for (i = 0; i < JBI_GEOM_POLY_MAX; i++)
        p.v[i].x = i;
    for (n = 3; n <= JBI_GEOM_POLY_MAX; n++) {
        p.n = n;
        count = jbi_geom_poly_prims(&p, out);
        tris = 0;
        for (i = 0; i < count; i++) {
            tris += out[i].vertices - 2;
            check("fan from vertex 0", out[i].v[0].x, 0);
        }
        check("prims cover the polygon", tris, n - 2);
        check("prims at most JBI_GEOM_PRIMS_MAX", count <= JBI_GEOM_PRIMS_MAX, 1);
    }
    p.n = 4;
    jbi_geom_poly_prims(&p, out);
    check("quad in Z order", out[0].v[0].x == 0 && out[0].v[1].x == 1 && out[0].v[2].x == 3 &&
                                 out[0].v[3].x == 2,
          1);
}

static void points(int mode)
{
    jbi_scene s;
    jbi_geom_draw d;
    jbi_geom_vertex v, seg[2];
    jb_vec3 a = {0, 0, -100}, b = {0, 0, 100}, side = {100000, 0, -100};
    int bucket;

    jbi_scene_init(&s);
    s.geometry = mode;
    jbi_geom_begin(&d, &s, NULL, 320, 240, 256, 0, "t", detail, sizeof detail);
    check("point ahead", jbi_geom_point(&d, a, &v), 1);
    check("point ahead: centre", v.x == 160 && v.y == 120, 1);
    check("point ahead: depth", v.z, 400);
    check("point behind", jbi_geom_point(&d, b, &v), 0);
    check("point far to the side", jbi_geom_point(&d, side, &v), 0);
    check("segment through the near plane",
          jbi_geom_segment(&s.gte, &d, a, b, 0x0000ff, 0xff0000, seg, &bucket), 1);
    check("segment: clipped end at the near plane",
          seg[1].z == (mode == JB_GEOMETRY_GTE ? d.near_z : 128), 1);
    check("segment: colour between", seg[1].color != 0xff0000 && seg[0].color == 0x0000ff, 1);
    check("segment behind", jbi_geom_segment(&s.gte, &d, b, b, 0, 0, seg, &bucket), 0);
}

static void meshes(void)
{
    jb_gte_vec pos[3] = {{1, 2, 3}, {-7, 0, 0}, {0, 300, -9}};
    jb_mesh_face f = {{0, 1, 2, 0}, 3, 0};
    jb_material mat;
    jb_mesh m;
    uint32_t col[3] = {0, 0, 0x1000000};
    int extent = -1;

    memset(&mat, 0, sizeof mat);
    memset(&m, 0, sizeof m);
    m.vertex_count = 3;
    m.positions = pos;
    m.face_count = 1;
    m.faces = &f;
    m.material_count = 1;
    m.materials = &mat;
    check("mesh ok", jbi_geom_check_mesh(&m, "f", &extent, detail, sizeof detail), JB_OK);
    check("extent", extent, 300);
    check("NULL mesh", jbi_geom_check_mesh(NULL, "f", NULL, detail, sizeof detail),
          JB_ERROR_INVALID_ARGUMENT);
    check_str("NULL mesh: detail", detail, "f: mesh is NULL");
    f.v[2] = 3;
    check("index out of range", jbi_geom_check_mesh(&m, "f", NULL, detail, sizeof detail),
          JB_ERROR_INVALID_ARGUMENT);
    check_str("index: detail", detail, "f: faces[0].v[2] is 3, but there are 3 vertices");
    f.v[2] = 2;
    f.vertices = 5;
    check("5 vertices", jbi_geom_check_mesh(&m, "f", NULL, detail, sizeof detail),
          JB_ERROR_INVALID_ARGUMENT);
    f.vertices = 3;
    f.material = 1;
    check("material out of range", jbi_geom_check_mesh(&m, "f", NULL, detail, sizeof detail),
          JB_ERROR_INVALID_ARGUMENT);
    f.material = 0;
    mat.blend = 5;
    check("bad blend", jbi_geom_check_mesh(&m, "f", NULL, detail, sizeof detail),
          JB_ERROR_INVALID_ARGUMENT);
    mat.blend = 0;
    mat.color = 0x1000000;
    check("bad material colour", jbi_geom_check_mesh(&m, "f", NULL, detail, sizeof detail),
          JB_ERROR_INVALID_ARGUMENT);
    mat.color = 0;
    m.colors = col;
    check("bad vertex colour", jbi_geom_check_mesh(&m, "f", NULL, detail, sizeof detail),
          JB_ERROR_INVALID_ARGUMENT);
    check_str("vertex colour: detail", detail, "f: colors[2] 0x1000000 is above 0xFFFFFF");
    m.colors = NULL;
    m.vertex_count = 0;
    check("no vertices", jbi_geom_check_mesh(&m, "f", NULL, detail, sizeof detail),
          JB_ERROR_INVALID_ARGUMENT);
    m.vertex_count = 3;
    m.faces = NULL;
    check("faces NULL", jbi_geom_check_mesh(&m, "f", NULL, detail, sizeof detail),
          JB_ERROR_INVALID_ARGUMENT);
    m.face_count = 0;
    m.materials = NULL;
    m.material_count = 0;
    check("no faces is fine", jbi_geom_check_mesh(&m, "f", NULL, detail, sizeof detail), JB_OK);
}

/* The neutral case of the header: material JB_RGB(128, 128, 128), one
   white light straight on, no ambient: the colour is unchanged in both
   paths. */
static void neutral(int mode)
{
    jbi_scene s;
    jbi_geom_draw d;
    jbi_geom_poly p[2];
    jb_gte_vec pos[3] = {{-100, -100, 0}, {100, -100, 0}, {0, 100, 0}};
    jb_mesh_face f = {{0, 1, 2, 0}, 3, 0};
    jb_material mat;
    jb_mesh m;
    jb_light l = {{0.0f, 0.0f, -1.0f}, 0xffffff, 0};
    jb_mat4 tr = jb_mat4_translate(0, 0, -500);
    int i, ok = 1;

    jbi_scene_init(&s);
    s.geometry = mode;
    jbi_scene_set_ambient(&s, 0, detail, sizeof detail);
    jbi_scene_set_light(&s, 0, &l, detail, sizeof detail);
    memset(&mat, 0, sizeof mat);
    memset(&m, 0, sizeof m);
    m.vertex_count = 3;
    m.positions = pos;
    m.face_count = 1;
    m.faces = &f;
    m.material_count = 1;
    m.materials = &mat;
    jbi_geom_begin(&d, &s, &tr, 320, 240, 256, 100, "t", detail, sizeof detail);
    jbi_geom_load(&s.gte, &d);
    check("neutral: drawn", jbi_geom_face(&s.gte, &d, &m, 0, p), 1);
    for (i = 0; i < 3; i++)
        ok &= p[0].v[i].color == 0x808080;
    check(mode == JB_GEOMETRY_GTE ? "neutral GTE: unchanged" : "neutral float: unchanged", ok,
          1);
    check("neutral: exact", p[0].exact, 1);
}

/* ------------------------------------------------------------------------ */
/* Geometry edge cases, each of which failed before its fix. */

/* A mesh of one face (3 or 4 vertices) and one material, in a scene with
   the default camera. */
typedef struct one_face {
    jbi_scene s;
    jbi_geom_draw d;
    jbi_geom_poly p[2];
    jb_gte_vec pos[4], nrm[4];
    uint32_t col[4];
    uint8_t uv[8];
    jb_mesh_face f;
    jb_material mat;
    jb_mesh m;
} one_face;

static void one_face_init(one_face *o, int mode, int n)
{
    memset(o, 0, sizeof *o);
    jbi_scene_init(&o->s);
    o->s.geometry = mode;
    o->f.vertices = (uint16_t)n;
    o->f.v[0] = 0;
    o->f.v[1] = 1;
    o->f.v[2] = 2;
    o->f.v[3] = 3;
    o->m.vertex_count = n;
    o->m.positions = o->pos;
    o->m.face_count = 1;
    o->m.faces = &o->f;
    o->m.material_count = 1;
    o->m.materials = &o->mat;
}

static void put(jb_gte_vec *v, int x, int y, int z)
{
    v->x = (int16_t)x;
    v->y = (int16_t)y;
    v->z = (int16_t)z;
}

/* Begin, load and run the face; returns the polygons made, or -1 when the
   set-up refused. */
static int run_face(one_face *o, const jb_mat4 *model)
{
    int extent;

    if (jbi_geom_check_mesh(&o->m, "t", &extent, detail, sizeof detail) != JB_OK)
        return -2;
    if (jbi_geom_begin(&o->d, &o->s, model, 320, 240, 1024, extent, "t", detail,
                       sizeof detail) != JB_OK)
        return -1;
    jbi_geom_load(&o->s.gte, &o->d);
    return jbi_geom_face(&o->s.gte, &o->d, &o->m, 0, o->p);
}

static const char *path_name(int mode)
{
    return mode == JB_GEOMETRY_GTE ? "GTE" : "float";
}

static void float_quantises_nothing(void)
{
    static one_face o;
    jb_mat4 m = jb_mat4_scale(1e30f, 1.0f, 1.0f);
    int r, c, zero = 1;

    /* Float geometry quantises nothing: no GTE register from a model
       matrix far beyond the GTE's range (it used to be converted to an
       integer from about 5e29, undefined), and the face is still drawn
       or culled without a NaN reaching an integer. */
    one_face_init(&o, JB_GEOMETRY_FLOAT, 3);
    put(&o.pos[0], -1, -1, 0);
    put(&o.pos[1], 1, -1, 0);
    put(&o.pos[2], 0, 1, 0);
    check("float quantises nothing: float set-up with a huge model", run_face(&o, &m) >= 0, 1);
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            zero &= o.d.r.rt.m[r][c] == 0;
    check("float quantises nothing: no rotation register in float mode", zero, 1);
    check("float quantises nothing: no translation register in float mode",
          o.d.r.tr[0] == 0 && o.d.r.tr[1] == 0 && o.d.r.tr[2] == 0, 1);
}

static void huge_light_direction(void)
{
    static one_face o;
    jb_light l;
    jb_mat4 tr = jb_mat4_translate(0.0f, 0.0f, -500.0f);
    int k, ch;

    /* A finite, huge light direction is normalised, so a diagonal normal
       gets cos 45 degrees of the light, not all of it (the float path used
       the unnormalised direction and saturated). */
    for (k = 0; k < 2; k++) {
        int mode = k == 0 ? JB_GEOMETRY_GTE : JB_GEOMETRY_FLOAT;
        char what[64];

        one_face_init(&o, mode, 3);
        memset(&l, 0, sizeof l);
        l.direction.x = -1e30f; /* light travelling in -x: it comes from +x */
        l.color = 0xffffff;
        check("huge light direction: huge direction accepted",
              jbi_scene_set_light(&o.s, 0, &l, detail,
                                                                 sizeof detail),
              JB_OK);
        check("huge light direction: direction normalised",
              o.s.light_to[0].x == 1.0f && o.s.light_to[0].y == 0.0f,
              1);
        jbi_scene_set_ambient(&o.s, 0, detail, sizeof detail);
        put(&o.pos[0], -100, -100, 0);
        put(&o.pos[1], 100, -100, 0);
        put(&o.pos[2], 0, 100, 0);
        for (ch = 0; ch < 3; ch++)
            put(&o.nrm[ch], 2896, 0, 2896);
        o.m.normals = o.nrm;
        snprintf(what, sizeof what, "huge light direction: %s: drawn", path_name(mode));
        check(what, run_face(&o, &tr), 1);
        snprintf(what, sizeof what, "huge light direction: %s: diagonal normal lit by cos 45",
                 path_name(mode));
        /* 128 x 0.707 = 90.5 */
        check(what, (int)(o.p[0].v[0].color & 0xff) >= 88 && (int)(o.p[0].v[0].color & 0xff) <= 92,
              1);
    }
    /* Eye and target whose difference overflows: refused, not a NaN view. */
    {
        jbi_scene s;
        jb_camera c;

        jbi_scene_init(&s);
        memset(&c, 0, sizeof c);
        c.eye.x = -3e38f;
        c.target.x = 3e38f;
        check("huge light direction: eye and target too far apart",
              jbi_scene_set_camera(&s, &c, detail, sizeof detail), JB_ERROR_INVALID_ARGUMENT);
        check_str("detail", detail,
                  "jb_set_camera: eye and target are too far apart for float arithmetic");
    }
}

/* A quad and a triangle, both facing the camera, at world depths zq and zt,
   in the GTE path: their buckets. */
static void tri_and_quad(int zq, int zt, int *bq, int *bt)
{
    static one_face o;
    jb_mat4 m;

    one_face_init(&o, JB_GEOMETRY_GTE, 4);
    put(&o.pos[0], -50, -50, 0);
    put(&o.pos[1], 50, -50, 0);
    put(&o.pos[2], 50, 50, 0);
    put(&o.pos[3], -50, 50, 0);
    m = jb_mat4_translate(0.0f, 0.0f, (float)-zq);
    *bq = run_face(&o, &m) == 1 ? o.p[0].bucket : -1;
    one_face_init(&o, JB_GEOMETRY_GTE, 3);
    put(&o.pos[0], -50, -50, 0);
    put(&o.pos[1], 50, -50, 0);
    put(&o.pos[2], 0, 50, 0);
    m = jb_mat4_translate(0.0f, 0.0f, (float)-zt);
    *bt = run_face(&o, &m) == 1 ? o.p[0].bucket : -1;
}

static void quad_triangle_order(void)
{
    int z, bq, bt, wrong = 0, unequal = 0;

    /* The case: the quad at 2000, the triangle at 2001, 1024
       buckets; the triangle must not sort nearer. */
    tri_and_quad(2000, 2001, &bq, &bt);
    check("quad triangle order: both drawn", bq >= 0 && bt >= 0, 1);
    check("quad triangle order: farther triangle not nearer than the quad", bt >= bq, 1);
    for (z = 40; z < 4090; z += 13) {
        tri_and_quad(z, z + 1, &bq, &bt);
        wrong += bt < bq;
        tri_and_quad(z, z, &bq, &bt);
        unequal += bt != bq;
    }
    check("quad triangle order: a triangle 1 farther never sorts nearer than a quad", wrong, 0);
    check("quad triangle order: a triangle and a quad at one depth share a bucket", unequal, 0);
}

static void fog_uses_rounded_h(void)
{
    static one_face o;
    jb_camera c;
    int i, dark = 1;

    /* H rounds from 0.63 to 1: DQA must use the H the GTE is given, so a
       white face at the fog's end is fully fogged (black). */
    one_face_init(&o, JB_GEOMETRY_GTE, 3);
    memset(&c, 0, sizeof c);
    c.target.z = -1.0f;
    c.fov_y = 179.4f;
    c.z_far = 1000000.0f;
    check("fog uses rounded h: camera", jbi_scene_set_camera(&o.s, &c, detail, sizeof detail),
          JB_OK);
    check("fog uses rounded h: fog",
          jbi_scene_set_fog(&o.s, 0, 64.0f, 128.0f, detail, sizeof detail), JB_OK);
    o.mat.unlit = 1;
    o.mat.color = 0xffffff;
    put(&o.pos[0], -8000, -8000, -128);
    put(&o.pos[1], 8000, -8000, -128);
    put(&o.pos[2], 0, 8000, -128);
    check("fog uses rounded h: drawn", run_face(&o, NULL), 1);
    check("fog uses rounded h: H is 1", o.d.r.h, 1);
    for (i = 0; i < o.p[0].n; i++)
        dark &= (o.p[0].v[i].color & 0xff) <= 2;
    check("fog uses rounded h: fully fogged at the fog's end", dark, 1);
    /* The DQB limit: refused, and the detail says DQB. */
    jbi_scene_init(&o.s);
    jbi_scene_set_fog(&o.s, 0, 1.0f, 1.001f, detail, sizeof detail);
    check("fog uses rounded h: fog from 1 to 1.001 refused", run_face(&o, NULL), -1);
    check("fog uses rounded h: the detail names DQB", strstr(detail, "DQB") != NULL, 1);
}

static void guard_band_cut_u(void)
{
    static one_face o;
    int k, i, found;

    /* An edge from (-2000, 0, -100), u 0, to (0, 0, -1000), u 255: on the
       screen it runs from x -4000 to 160 along y 120. Where the guard band
       cuts it, at x -1016, u must be the screen-linear 183, not the 52 the
       view-space fraction gave. */
    for (k = 0; k < 2; k++) {
        int mode = k == 0 ? JB_GEOMETRY_GTE : JB_GEOMETRY_FLOAT;
        char what[64];

        one_face_init(&o, mode, 3);
        o.mat.double_sided = 1;
        o.mat.unlit = 1;
        put(&o.pos[0], -2000, 0, -100);
        put(&o.pos[1], 0, 0, -1000);
        put(&o.pos[2], 0, 50, -1000);
        o.uv[0] = 0;
        o.uv[2] = 255;
        o.uv[4] = 255;
        o.m.uv = o.uv;
        snprintf(what, sizeof what, "guard band cut u: %s: drawn", path_name(mode));
        check(what, run_face(&o, NULL), 1);
        found = 0;
        for (i = 0; i < o.p[0].n; i++) {
            const jbi_geom_vertex *v = &o.p[0].v[i];

            if (iabs(v->x - JBI_GEOM_GUARD_MIN) <= 2 && iabs(v->y - 120) <= 1) {
                found = 1;
                snprintf(what, sizeof what, "guard band cut u: %s: screen-linear u at the cut",
                         path_name(mode));
                if (!check(what, iabs(v->u - 183) <= 2, 1))
                    printf("  u %d\n", v->u);
            }
        }
        snprintf(what, sizeof what, "guard band cut u: %s: a vertex at the guard band",
                 path_name(mode));
        check(what, found, 1);
    }
}

static void far_plane_fraction(void)
{
    static one_face o;
    jb_mat4 t = jb_mat4_translate(0.0f, 0.0f, -4096.0f), sc = jb_mat4_scale(1.0f, 1.0f, 0.125f);

    /* World depth 4096.125, beyond the far plane at 4096: its SZ truncate
       to 16384 = far_z, but the face is beyond and must not be drawn. */
    one_face_init(&o, JB_GEOMETRY_GTE, 3);
    o.mat.double_sided = 1;
    put(&o.pos[0], -100, -100, -1);
    put(&o.pos[1], 100, -100, -1);
    put(&o.pos[2], 0, 100, -1);
    t = jb_mat4_mul(&t, &sc);
    check("far plane fraction: beyond the far plane by a fraction: not drawn", run_face(&o, &t), 0);
}

static void near_plane_vertex(void)
{
    static one_face o;
    int k, i, j, dup;

    /* (-8, -8, -32) lies on the near plane, (5, -8, -10) behind it: the
       vertex on the plane is kept once, not again as a cut point. */
    for (k = 0; k < 2; k++) {
        int mode = k == 0 ? JB_GEOMETRY_GTE : JB_GEOMETRY_FLOAT;
        char what[64];

        one_face_init(&o, mode, 3);
        o.mat.double_sided = 1;
        put(&o.pos[0], -8, -8, -32);
        put(&o.pos[1], 5, -8, -10);
        put(&o.pos[2], 0, 300, -1000);
        snprintf(what, sizeof what, "near plane vertex: %s: drawn", path_name(mode));
        check(what, run_face(&o, NULL), 1);
        snprintf(what, sizeof what, "near plane vertex: %s: three vertices", path_name(mode));
        check(what, o.p[0].n, 3);
        dup = 0;
        for (i = 0; i < o.p[0].n; i++)
            for (j = i + 1; j < o.p[0].n; j++)
                dup += o.p[0].v[i].sx == o.p[0].v[j].sx && o.p[0].v[i].sy == o.p[0].v[j].sy &&
                       o.p[0].v[i].z == o.p[0].v[j].z;
        snprintf(what, sizeof what, "near plane vertex: %s: no vertex twice", path_name(mode));
        check(what, dup, 0);
    }
}

static void near_quad_diagonal(void)
{
    static one_face o;
    int k, i, t, found;

    /* A quad crossing the near plane, only vertex 2 white: its diagonal
       1-3 meets the near plane at the screen's centre, black there. The
       clipped quad must keep that diagonal (two triangles), so a vertex at
       the centre is black. */
    for (k = 0; k < 2; k++) {
        int mode = k == 0 ? JB_GEOMETRY_GTE : JB_GEOMETRY_FLOAT;
        char what[64];
        int got;

        one_face_init(&o, mode, 4);
        o.mat.double_sided = 1;
        o.mat.unlit = 1;
        put(&o.pos[0], -10, -8, -16);
        put(&o.pos[1], 10, -8, -48);
        put(&o.pos[2], 10, 8, -48);
        put(&o.pos[3], -10, 8, -16);
        o.col[2] = 0xffffff;
        o.m.colors = o.col;
        got = run_face(&o, NULL);
        snprintf(what, sizeof what, "near quad diagonal: %s: two triangles", path_name(mode));
        check(what, got, 2);
        found = 0;
        for (t = 0; t < got; t++)
            for (i = 0; i < o.p[t].n; i++)
                if (iabs(o.p[t].v[i].x - 160) <= 1 && iabs(o.p[t].v[i].y - 120) <= 1) {
                    found = 1;
                    snprintf(what, sizeof what, "near quad diagonal: %s: black on the diagonal",
                             path_name(mode));
                    check(what, o.p[t].v[i].color, 0);
                }
        snprintf(what, sizeof what, "near quad diagonal: %s: the diagonal's cut point",
                 path_name(mode));
        check(what, found, 1);
    }
}

static void saturated_ir_fallback(void)
{
    static one_face o;
    jb_camera c;
    int got, i, all = 1;

    /* H 1: IR1 saturates at 32767 while SX stays in range. The fallback
       must saturate IR as the GTE does: every vertex at x 176. */
    one_face_init(&o, JB_GEOMETRY_GTE, 3);
    memset(&c, 0, sizeof c);
    c.target.z = -1.0f;
    c.fov_y = 179.4f;
    jbi_scene_set_camera(&o.s, &c, detail, sizeof detail);
    o.mat.double_sided = 1;
    put(&o.pos[0], 10000, -2000, -500);
    put(&o.pos[1], 11000, -2000, -500);
    put(&o.pos[2], 10000, 2000, -500);
    got = run_face(&o, NULL);
    /* The triangle must come back (a fallback that dropped it
       would pass the per-vertex check below vacuously). */
    check("saturated ir fallback: the triangle drawn", got, 1);
    for (i = 0; got > 0 && i < o.p[0].n; i++)
        all &= o.p[0].v[i].x == 176;
    check("saturated ir fallback: IR saturated as the GTE saturates it", all && got > 0, 1);
}

static void rotation_rounding_limits(void)
{
    static one_face o;
    jb_mat4 m;

    /* 4 x 4096 x 32767.49609375 / 16384 = 32767.496 rounds to 32767: no
       shift needed, so a mesh of extent 20000 is accepted. */
    one_face_init(&o, JB_GEOMETRY_GTE, 3);
    put(&o.pos[0], -20000, -100, 0);
    put(&o.pos[1], 100, -100, 0);
    put(&o.pos[2], 0, 100, 0);
    m = jb_mat4_scale(32767.49609375f / 16384.0f, 1.0f, 1.0f);
    check("rotation rounding limits: just below 32767.5: accepted", run_face(&o, &m) >= 0, 1);
    check("rotation rounding limits: no shift", o.d.shift, 0);
    check("rotation rounding limits: rounds to 32767", o.d.r.rt.m[0][0], 32767);
    m = jb_mat4_scale(-32768.49609375f / 16384.0f, 1.0f, 1.0f);
    check("rotation rounding limits: just above -32768.5: accepted", run_face(&o, &m) >= 0, 1);
    check("rotation rounding limits: rounds to -32768", o.d.r.rt.m[0][0], -32768);
    m = jb_mat4_scale(32767.5f / 16384.0f, 1.0f, 1.0f);
    check("rotation rounding limits: 32767.5 needs a shift: refused at extent 20000",
          run_face(&o, &m), -1);
}

static void fog_to_flt_max(void)
{
    static one_face o;
    jbi_geom_vertex e[2];
    jb_vec3 a = {-10.0f, 0.0f, -128.0f}, b = {10.0f, 0.0f, -128.0f};
    int bucket, ch;

    /* Fog to the largest float: its end times the scale passes the float
       range; the amount at depth 128 (GTE 512, start 256) is 0.5. */
    one_face_init(&o, JB_GEOMETRY_FLOAT, 3);
    check("fog to flt max: fog to FLT_MAX", jbi_scene_set_fog(&o.s, 0, 64.0f, 3.4028234e38f, detail,
                                                   sizeof detail),
          JB_OK);
    jbi_geom_begin(&o.d, &o.s, NULL, 320, 240, 1024, 0, "t", detail, sizeof detail);
    check("fog to flt max: segment",
          jbi_geom_segment(NULL, &o.d, a, b, 0xffffff, 0xffffff, e, &bucket), 1);
    ch = (int)(e[0].color & 0xff);
    check("fog to flt max: half fogged, not black", ch >= 127 && ch <= 128, 1);
}

static void light_and_fog_rounded_once(void)
{
    static one_face o;
    jb_mat4 tr = jb_mat4_translate(0.0f, 0.0f, -80.0f);

    /* 2 x 100 / 255 x 0.7 = 0.549: rounded once at the end, 1. */
    one_face_init(&o, JB_GEOMETRY_FLOAT, 3);
    jbi_scene_set_ambient(&o.s, JB_RGB(100, 100, 100), detail, sizeof detail);
    jbi_scene_set_fog(&o.s, 0, 64.0f, 192.0f, detail, sizeof detail);
    o.mat.color = JB_RGB(2, 2, 2);
    put(&o.pos[0], -10, -10, 0);
    put(&o.pos[1], 10, -10, 0);
    put(&o.pos[2], 0, 10, 0);
    check("light and fog rounded once: drawn", run_face(&o, &tr), 1);
    check("light and fog rounded once: lit and fogged, rounded once", o.p[0].v[0].color,
          JB_RGB(1, 1, 1));
}

static void flat_first_triangle(void)
{
    static one_face o;
    int k;

    /* A flat convex quad whose first triangle projects to no area: judged
       by its second, which faces the camera. */
    for (k = 0; k < 2; k++) {
        int mode = k == 0 ? JB_GEOMETRY_GTE : JB_GEOMETRY_FLOAT;
        char what[64];

        one_face_init(&o, mode, 4);
        put(&o.pos[0], 0, 1, -1000);
        put(&o.pos[1], 5, 1, -1000);
        put(&o.pos[2], 100, 101, -1000);
        put(&o.pos[3], -8, 2, -1000);
        snprintf(what, sizeof what, "flat first triangle: %s: front-facing quad kept",
                 path_name(mode));
        check(what, run_face(&o, NULL), 1);
    }
}

static void parallel_up_refused(void)
{
    jbi_scene s;
    jb_camera c;

    /* Parallel, not along an axis: the unit forward vector's rounding must
       not make it pass. */
    jbi_scene_init(&s);
    memset(&c, 0, sizeof c);
    c.target.x = 2.0f;
    c.target.y = 3.0f;
    c.target.z = 4.0f;
    c.up = c.target;
    check("parallel up refused: parallel up refused",
          jbi_scene_set_camera(&s, &c, detail, sizeof detail),
          JB_ERROR_INVALID_ARGUMENT);
    c.up.x = 6.0f;
    c.up.y = 9.0f;
    c.up.z = 12.0f;
    check("parallel up refused: parallel up, longer, refused",
          jbi_scene_set_camera(&s, &c, detail, sizeof detail),
          JB_ERROR_INVALID_ARGUMENT);
}

/* Interior front-facing faces must be drawn, exact, by both paths. */
static void interior_faces_exact(void)
{
    static one_face o;
    gen g = {99, 0, 0};
    int i, k, ok[2] = {0, 0};

    for (i = 0; i < 40; i++) {
        int s = 20 + rnd_below(&g, 60), z = 300 + rnd_below(&g, 1500);
        int x = rnd_below(&g, 200) - 100, y = rnd_below(&g, 150) - 75;

        for (k = 0; k < 2; k++) {
            one_face_init(&o, k == 0 ? JB_GEOMETRY_GTE : JB_GEOMETRY_FLOAT, 3);
            put(&o.pos[0], x - s, y - s, -z);
            put(&o.pos[1], x + s, y - s, -z);
            put(&o.pos[2], x, y + s, -z);
            ok[k] += run_face(&o, NULL) == 1 && o.p[0].exact;
        }
    }
    check("interior faces exact: every interior front face drawn exactly (GTE)", ok[0], 40);
    check("interior faces exact: every interior front face drawn exactly (float)", ok[1], 40);
}

static void geometry_cases(void)
{
    float_quantises_nothing();
    huge_light_direction();
    quad_triangle_order();
    fog_uses_rounded_h();
    guard_band_cut_u();
    far_plane_fraction();
    near_plane_vertex();
    near_quad_diagonal();
    saturated_ir_fallback();
    rotation_rounding_limits();
    fog_to_flt_max();
    light_and_fog_rounded_once();
    interior_faces_exact();
    flat_first_triangle();
    parallel_up_refused();
}

/* ------------------------------------------------------------------------ */
/* More pipeline edge cases, and the API for light intensity, projection
   distance, depth bias and tint. */

static int run_face_size(one_face *o, const jb_mat4 *model, int w, int h)
{
    int extent;

    if (jbi_geom_check_mesh(&o->m, "t", &extent, detail, sizeof detail) != JB_OK)
        return -2;
    if (jbi_geom_begin(&o->d, &o->s, model, w, h, 1024, extent, "t", detail, sizeof detail) !=
        JB_OK)
        return -1;
    jbi_geom_load(&o->s.gte, &o->d);
    return jbi_geom_face(&o->s.gte, &o->d, &o->m, 0, o->p);
}

static void camera_limits(void)
{
    jbi_scene s;
    jb_camera c;

    /* A tiny up is no less an up: perpendicular, so accepted. */
    jbi_scene_init(&s);
    memset(&c, 0, sizeof c);
    c.target.x = -1.0f;
    c.up.y = 1e-30f;
    check("camera limits: up of 1e-30 accepted",
          jbi_scene_set_camera(&s, &c, detail, sizeof detail),
          JB_OK);
    /* An eye whose view translation overflows: refused, the camera kept. */
    memset(&c, 0, sizeof c);
    c.eye.x = 3.4028234e38f;
    c.eye.y = 3.4028234e38f;
    check("camera limits: view not representable: refused",
          jbi_scene_set_camera(&s, &c, detail, sizeof detail), JB_ERROR_INVALID_ARGUMENT);
    check_str("detail", detail,
              "jb_set_camera: the view is not representable in float (the eye is too far from "
              "the origin)");
    check("camera limits: the camera kept", s.camera.target.x == -1.0f, 1);
}

static void shared_integer_position(void)
{
    static one_face o;

    /* Two vertices on one integer SXY with different projections: the
       friendly record keeps each its own sub-pixel position (the console's
       table would give both the later record's). */
    one_face_init(&o, JB_GEOMETRY_GTE, 4);
    o.mat.unlit = 1;
    put(&o.pos[0], 0, 0, -400);
    put(&o.pos[1], 1, 0, -400);
    put(&o.pos[2], 100, -100, -400);
    put(&o.pos[3], -100, -100, -400);
    o.mat.double_sided = 1;
    check("shared integer position: drawn", run_face(&o, NULL), 1);
    check("shared integer position: one integer position", o.p[0].v[0].x == o.p[0].v[1].x &&
                                             o.p[0].v[0].y == o.p[0].v[1].y,
          1);
    check("shared integer position: two sub-pixel positions", o.p[0].v[0].sx != o.p[0].v[1].sx, 1);
}

static void guard_band_cut_persp(void)
{
    static one_face o;
    int k, i, found;

    /* (-2000, 0, -100) u 0 to (0, 0, -1000) u 255: at the guard band's cut
       (x -1024) the affine u is 182 (screen-linear), the perspective-correct
       one 51 (view-linear). Both are handed on. */
    for (k = 0; k < 2; k++) {
        int mode = k == 0 ? JB_GEOMETRY_GTE : JB_GEOMETRY_FLOAT;
        char what[64];

        one_face_init(&o, mode, 3);
        o.mat.double_sided = 1;
        o.mat.unlit = 1;
        put(&o.pos[0], -2000, 0, -100);
        put(&o.pos[1], 0, 0, -1000);
        put(&o.pos[2], 0, -200, -1000);
        o.uv[2] = 255;
        o.uv[4] = 255;
        o.m.uv = o.uv;
        check("guard band cut persp: drawn", run_face(&o, NULL), 1);
        found = 0;
        for (i = 0; i < o.p[0].n; i++) {
            const jbi_geom_vertex *v = &o.p[0].v[i];

            if (iabs(v->x - JBI_GEOM_GUARD_MIN) <= 2 && iabs(v->y - 120) <= 1) {
                found = 1;
                snprintf(what, sizeof what, "guard band cut persp: %s: affine u at the cut",
                         path_name(mode));
                if (!check(what, v->u >= 180 && v->u <= 185, 1))
                    printf("  u %d\n", v->u);
                snprintf(what, sizeof what, "guard band cut persp: %s: perspective u at the cut",
                         path_name(mode));
                if (!check(what, v->pu >= 49 && v->pu <= 54, 1))
                    printf("  pu %d\n", v->pu);
            }
        }
        snprintf(what, sizeof what, "guard band cut persp: %s: a vertex at the guard band",
                 path_name(mode));
        check(what, found, 1);
    }
}

static void huge_line_halves(void)
{
    static one_face o;
    jbi_geom_vertex e[2];
    jb_vec3 a = {-1e20f, 0.0f, -100.0f}, b = {1e20f, 0.0f, -100.0f};
    int k, bucket;

    /* A line from x -1e20 to 1e20 keeps both halves: one end at each side
       of the guard band, not at the screen's centre. */
    for (k = 0; k < 2; k++) {
        char what[64];

        one_face_init(&o, k == 0 ? JB_GEOMETRY_GTE : JB_GEOMETRY_FLOAT, 3);
        jbi_geom_begin(&o.d, &o.s, NULL, 320, 240, 1024, 0, "t", detail, sizeof detail);
        snprintf(what, sizeof what, "huge line halves: %s: drawn", path_name(k == 0 ? 0 : 1));
        check(what, jbi_geom_segment(&o.s.gte, &o.d, a, b, 0xff, 0xff, e, &bucket), 1);
        snprintf(what, sizeof what, "huge line halves: %s: both sides of the centre",
                 path_name(k == 0 ? 0 : 1));
        if (!check(what, e[0].x <= JBI_GEOM_GUARD_MIN + 2 && e[1].x >= JBI_GEOM_GUARD_MAX - 2, 1))
            printf("  %d %d\n", e[0].x, e[1].x);
    }
}

static void reciprocal_limit(void)
{
    jb_context *jb = must_create(NULL);
    static one_face o;
    jb_camera c;
    jb_mat4 m;
    int got, i, same = 1;

    /* H 42767: at SZ 21384 the reciprocal reaches 0x1FFFF with no divide
       flag; the GTE's own record keeps its coarse position then, and so
       must the friendly one. Compared with the public GTE's table. */
    jb_set_precise_vertices(jb, 1);
    one_face_init(&o, JB_GEOMETRY_GTE, 4);
    memset(&c, 0, sizeof c);
    c.target.z = -1.0f;
    c.projection = 42767.0f;
    c.z_far = 8000.0f;
    check("reciprocal limit: camera", jbi_scene_set_camera(&o.s, &c, detail, sizeof detail), JB_OK);
    o.mat.unlit = 1;
    put(&o.pos[0], -3, -3, 0);
    put(&o.pos[1], 3, -3, 0);
    put(&o.pos[2], 3, 3, 0);
    put(&o.pos[3], -3, 3, 0);
    {
        jb_vec3 t = {0.0f, 0.0f, -5346.0f}, r = {0.0f, 0.0f, 0.0f}, sc = {1.0625f, 1.0f, 1.0f};

        m = jb_mat4_trs(t, r, sc);
    }
    got = run_face(&o, &m);
    check("reciprocal limit: drawn exactly", got == 1 && o.p[0].exact, 1);
    hand_load(jb, &o.d);
    jb_gte_begin_frame(jb);
    for (i = 0; got == 1 && i < 4; i++) {
        jbi_precise_vertex pv;
        int sh = o.d.shift;

        hand_vertex(jb, 0, o.pos[i].x * (1 << sh), o.pos[i].y * (1 << sh),
                    o.pos[i].z * (1 << sh));
        jb_gte_command(jb, JB_GTE_RTPS);
        if (jbi_precise_lookup(jb->api->precise, jb_gte_read(jb, JB_GTE_SXY2), &pv)) {
            const jbi_geom_vertex *v = &o.p[0].v[i];

            same &= pv.x == v->sx && pv.y == v->sy;
        } else {
            same = 0;
        }
    }
    check("reciprocal limit: the GTE's own sub-pixel positions with a capped quotient", same, 1);
    jb_destroy(jb);
}

static void wide_frame_edge(void)
{
    static one_face o;
    jbi_geom_vertex e[2];
    jb_vec3 a = {244.0f, -10.0f, -100.0f}, b = {244.0f, 10.0f, -100.0f};
    int bucket, k;

    /* A 1024-wide frame: x 1019 is on screen and must be reachable. */
    for (k = 0; k < 2; k++) {
        char what[64];

        one_face_init(&o, k == 0 ? JB_GEOMETRY_GTE : JB_GEOMETRY_FLOAT, 3);
        jbi_geom_begin(&o.d, &o.s, NULL, 1024, 240, 1024, 0, "t", detail, sizeof detail);
        snprintf(what, sizeof what, "wide frame edge: %s: line at x 1019 drawn",
                 path_name(k ? 1 : 0));
        check(what, jbi_geom_segment(&o.s.gte, &o.d, a, b, 0xff, 0xff, e, &bucket), 1);
        snprintf(what, sizeof what, "wide frame edge: %s: at x 1019", path_name(k ? 1 : 0));
        check(what, iabs(e[0].x - 1019) <= 1, 1);
        put(&o.pos[0], 243, -10, -100);
        put(&o.pos[1], 245, -10, -100);
        put(&o.pos[2], 244, 10, -100);
        o.mat.double_sided = 1;
        snprintf(what, sizeof what, "wide frame edge: %s: triangle there drawn",
                 path_name(k ? 1 : 0));
        check(what, run_face_size(&o, NULL, 1024, 240) >= 1, 1);
    }
}

static void tiny_field_of_view(void)
{
    static one_face o;
    jb_camera c;
    jb_mat4 m = jb_mat4_scale(1e-36f / 1024.0f, 1e-36f / 1024.0f, 1.0f);

    /* A field of view of 1e-36 degrees: its focal length passes the float
       range but not double's; a triangle scaled to match still draws. */
    one_face_init(&o, JB_GEOMETRY_FLOAT, 3);
    memset(&c, 0, sizeof c);
    c.target.z = -1.0f;
    c.fov_y = 1e-36f;
    check("tiny field of view: camera", jbi_scene_set_camera(&o.s, &c, detail, sizeof detail),
          JB_OK);
    o.mat.unlit = 1;
    put(&o.pos[0], -10, -10, -100);
    put(&o.pos[1], 10, -10, -100);
    put(&o.pos[2], 0, 10, -100);
    check("tiny field of view: drawn", run_face(&o, &m), 1);
}

static void near_cut_colour(void)
{
    static one_face o;
    int i, found = 0, ok = 1;

    /* The near plane cuts the edge from colour 0 to 29 a third of the way:
       29 / 3 lit by 192 / 255 is 7.28, so 7 (not 10 x 0.75 = 8). */
    one_face_init(&o, JB_GEOMETRY_FLOAT, 3);
    jbi_scene_set_ambient(&o.s, JB_RGB(192, 192, 192), detail, sizeof detail);
    o.mat.double_sided = 1;
    put(&o.pos[0], 0, 0, -29);
    put(&o.pos[1], -15, -10, -38);
    put(&o.pos[2], 15, -10, -38);
    o.col[0] = 0;
    o.col[1] = JB_RGB(29, 29, 29);
    o.col[2] = JB_RGB(29, 29, 29);
    o.m.colors = o.col;
    check("near cut colour: drawn", run_face(&o, NULL), 1);
    for (i = 0; i < o.p[0].n; i++)
        if (o.p[0].v[i].z == 128) {
            found++;
            ok &= (o.p[0].v[i].color & 0xff) == 7;
        }
    check("near cut colour: two vertices on the near plane", found, 2);
    check("near cut colour: lit from the unrounded colour", ok, 1);
}

static void fractional_far_plane(void)
{
    static one_face o;
    jb_camera c;
    jb_mat4 t = jb_mat4_translate(0.0f, 0.0f, -4096.0f), sc = jb_mat4_scale(1.0f, 1.0f, 0.125f);

    /* World depth 4096.125, inside a far plane at 4096.1875: drawn. */
    t = jb_mat4_mul(&t, &sc);
    one_face_init(&o, JB_GEOMETRY_GTE, 3);
    memset(&c, 0, sizeof c);
    c.target.z = -1.0f;
    c.z_far = 4096.1875f;
    jbi_scene_set_camera(&o.s, &c, detail, sizeof detail);
    o.mat.double_sided = 1;
    put(&o.pos[0], -200, -200, -1);
    put(&o.pos[1], 200, -200, -1);
    put(&o.pos[2], 0, 200, -1);
    check("fractional far plane: just inside a fractional far plane: drawn", run_face(&o, &t), 1);
    c.z_far = 4096.0f;
    jbi_scene_set_camera(&o.s, &c, detail, sizeof detail);
    check("fractional far plane: just beyond: not drawn", run_face(&o, &t), 0);
}

static void api_lights(void)
{
    static one_face o;
    jb_light l;
    jb_mat4 tr = jb_mat4_translate(0.0f, 0.0f, -500.0f);
    static const float level[3] = {1.0f, 1.5f, 2.0f};
    static const int want[3] = {128, 192, 255};
    int k, i;

    /* Intensity: a neutral material straight under a white light, no
       ambient, shows 128 at 1, 192 at 1.5, saturates at 2. */
    for (k = 0; k < 2; k++)
        for (i = 0; i < 3; i++) {
            char what[64];
            int ch;

            one_face_init(&o, k == 0 ? JB_GEOMETRY_GTE : JB_GEOMETRY_FLOAT, 3);
            memset(&l, 0, sizeof l);
            l.direction.z = -1.0f;
            l.color = 0xffffff;
            l.intensity = level[i];
            check("intensity set", jbi_scene_set_light(&o.s, 0, &l, detail, sizeof detail), JB_OK);
            jbi_scene_set_ambient(&o.s, 0, detail, sizeof detail);
            put(&o.pos[0], -100, -100, 0);
            put(&o.pos[1], 100, -100, 0);
            put(&o.pos[2], 0, 100, 0);
            run_face(&o, &tr);
            ch = (int)(o.p[0].v[0].color & 0xff);
            snprintf(what, sizeof what, "intensity %g (%s)", (double)level[i],
                     path_name(k ? 1 : 0));
            if (!check(what, iabs(ch - want[i]) <= 1, 1))
                printf("  %d\n", ch);
        }
    jbi_scene_init(&o.s);
    l.intensity = -1.0f;
    check("intensity negative", jbi_scene_set_light(&o.s, 0, &l, detail, sizeof detail),
          JB_ERROR_INVALID_ARGUMENT);
    check_str("intensity: detail", detail, "jb_set_light: intensity -1 is outside 0..7.99");
    l.intensity = 9.0f;
    check("intensity 9", jbi_scene_set_light(&o.s, 0, &l, detail, sizeof detail),
          JB_ERROR_INVALID_ARGUMENT);
}

static void api_projection(void)
{
    static one_face o;
    jb_camera c;

    one_face_init(&o, JB_GEOMETRY_GTE, 3);
    memset(&c, 0, sizeof c);
    c.target.z = -1.0f;
    c.projection = 256.0f;
    check("projection 256", jbi_scene_set_camera(&o.s, &c, detail, sizeof detail), JB_OK);
    put(&o.pos[0], -100, -100, -500);
    put(&o.pos[1], 100, -100, -500);
    put(&o.pos[2], 0, 100, -500);
    run_face(&o, NULL);
    check("projection: H", o.d.r.h, 256);
    check("projection: float focal length", o.d.fh == 256.0, 1);
    c.projection = -1.0f;
    check("projection negative", jbi_scene_set_camera(&o.s, &c, detail, sizeof detail),
          JB_ERROR_INVALID_ARGUMENT);
    check_str("projection: detail", detail, "jb_set_camera: projection -1 is negative");
}

static void api_bias_tint(void)
{
    static one_face o;
    jb_mat4 tr = jb_mat4_translate(0.0f, 0.0f, -1000.0f);
    int k, plain, biased;

    /* depth_bias moves the bucket, the same in both paths; tint scales
       the colour before lighting. */
    for (k = 0; k < 2; k++) {
        char what[64];

        one_face_init(&o, k == 0 ? JB_GEOMETRY_GTE : JB_GEOMETRY_FLOAT, 3);
        o.mat.unlit = 1;
        o.mat.color = JB_RGB(200, 100, 128);
        put(&o.pos[0], -100, -100, 0);
        put(&o.pos[1], 100, -100, 0);
        put(&o.pos[2], 0, 100, 0);
        run_face(&o, &tr);
        plain = o.p[0].bucket;
        o.d.bias = -200.0 * o.d.scale;
        o.d.tint = JB_RGB(64, 128, 255);
        jbi_geom_load(&o.s.gte, &o.d);
        check("biased face", jbi_geom_face(&o.s.gte, &o.d, &o.m, 0, o.p), 1);
        biased = o.p[0].bucket;
        snprintf(what, sizeof what, "depth bias nearer (%s)", path_name(k ? 1 : 0));
        /* 1024 buckets over GTE depths 128..16384: 200 world units, 800 GTE
           units, are about 50 buckets. */
        if (!check(what, plain - biased >= 45 && plain - biased <= 55, 1))
            printf("  %d -> %d\n", plain, biased);
        snprintf(what, sizeof what, "tint (%s)", path_name(k ? 1 : 0));
        check(what, o.p[0].v[0].color, JB_RGB(100, 100, 255));
    }
}

static void pipeline_cases(void)
{
    camera_limits();
    shared_integer_position();
    guard_band_cut_persp();
    huge_line_halves();
    reciprocal_limit();
    wide_frame_edge();
    tiny_field_of_view();
    near_cut_colour();
    fractional_far_plane();
    api_lights();
    api_projection();
    api_bias_tint();
}

/* ------------------------------------------------------------------------ */
/* The ordering property, randomized over both paths and depth
   bias. Every draw's bucket must be one monotonic function of one key,
   whichever path made it: the key is the OTZ of its vertices' integer
   depths (GTE units, rounded down: SZ in the GTE path, the unrounded depth
   plus the bias rounded down in the float path), on the AVSZ scale, plus
   in the GTE path its bias on that scale (rounded; the float
   path's bias is in its depths, not rounded on its own). So sorted by
   key, buckets never decrease, and
   equal keys share a bucket: a draw with a deeper key is never sorted
   nearer, also across an in-frame switch of path. */

typedef struct ord {
    int64_t key;
    int bucket;
} ord;

static int ord_cmp(const void *a, const void *b)
{
    const ord *x = a, *y = b;

    return x->key < y->key ? -1 : x->key > y->key ? 1 : x->bucket - y->bucket;
}

static int64_t ord_depth(const jbi_geom_draw *d, const jbi_geom_vertex *v)
{
    double z;

    if (d->mode == JB_GEOMETRY_GTE)
        return v->z;
    z = v->zf + d->bias;
    return (int64_t)floor(z); /* Negative ones kept in the average */
}

static int64_t ord_key(const jbi_geom_draw *d, int64_t sum, int n)
{
    int64_t k = (int64_t)floor((double)(sum * 3 * d->r.zsf3) / (double)n / 4096.0);
    double b = d->bias * (double)(3 * d->r.zsf3) / 4096.0;

    if (d->mode != JB_GEOMETRY_GTE)
        return k; /* the bias is in the depths (ord_depth) */
    return k + (int64_t)(b >= 0 ? b + 0.5 : b - 0.5);
}

static void ordering_property(void)
{
    enum { DRAWS = 6000 };
    static ord o[DRAWS];
    static one_face f;
    gen g = {2024, 0, 0};
    int n = 0, i, bad_order = 0, bad_equal = 0;

    while (n < DRAWS) {
        int mode = rnd_below(&g, 2), kind = rnd_below(&g, 3), ot = 1024;
        /* Biases to -4000 world units too, nearer than the eye for
           a vertex or the whole face. */
        float bias = rnd_below(&g, 3) == 0   ? 0.0f
                     : rnd_below(&g, 3) == 0 ? -(float)rnd_below(&g, 4000)
                                             : (float)(rnd_below(&g, 4001) - 2000) / 7.0f;
        float z = -(30.0f + (float)rnd_below(&g, 400000) / 100.0f);
        int extent;

        one_face_init(&f, mode, 3);
        f.mat.double_sided = 1;
        {
            /* Some faces deep (vertices up to 2000 apart in depth),
               so a bias can bring one vertex nearer than the eye while
               their average stays in front. */
            int spread = rnd_below(&g, 3) == 0 ? 2000 : 20;

            put(&f.pos[0], -20, -20, 0);
            put(&f.pos[1], 20, -20, (int)rnd_below(&g, 2 * spread + 1) - spread);
            put(&f.pos[2], 0, 20, (int)rnd_below(&g, 2 * spread + 1) - spread);
        }
        jbi_geom_check_mesh(&f.m, "t", &extent, detail, sizeof detail);
        {
            jb_mat4 at = jb_mat4_translate((float)(rnd_below(&g, 201) - 100) * 0.1f,
                                           (float)(rnd_below(&g, 201) - 100) * 0.1f, z);

            if (jbi_geom_begin(&f.d, &f.s, kind == 0 ? &at : NULL, 320, 240, ot, extent, "t",
                               detail, sizeof detail) != JB_OK)
                continue;
            f.d.bias = (double)bias * f.d.scale;
            jbi_geom_load(&f.s.gte, &f.d);
            if (kind == 0) { /* a face */
                int got = jbi_geom_face(&f.s.gte, &f.d, &f.m, 0, f.p), k;
                int64_t sum = 0;

                if (got != 1)
                    continue;
                for (k = 0; k < f.p[0].n; k++)
                    sum += ord_depth(&f.d, &f.p[0].v[k]);
                o[n].key = ord_key(&f.d, sum, f.p[0].n);
                o[n].bucket = f.p[0].bucket;
            } else if (kind == 1) { /* a billboard's centre */
                jbi_geom_vertex v;
                jb_vec3 p = {0.0f, 0.0f, z};

                if (!jbi_geom_point(&f.d, p, &v))
                    continue;
                o[n].key = ord_key(&f.d, ord_depth(&f.d, &v), 1);
                o[n].bucket = jbi_geom_bucket(&f.d, &v);
            } else { /* a segment */
                jbi_geom_vertex e[2];
                jb_vec3 a = {-5.0f, 0.0f, z}, b = {5.0f, 0.0f, z - (float)rnd_below(&g, 50)};
                int bucket;

                if (!jbi_geom_segment(&f.s.gte, &f.d, a, b, 0xff, 0xff, e, &bucket))
                    continue;
                o[n].key = ord_key(&f.d, ord_depth(&f.d, &e[0]) + ord_depth(&f.d, &e[1]), 2);
                o[n].bucket = bucket;
            }
            n++;
        }
    }
    qsort(o, DRAWS, sizeof o[0], ord_cmp);
    for (i = 1; i < DRAWS; i++) {
        bad_order += o[i].bucket < o[i - 1].bucket;
        bad_equal += o[i].key == o[i - 1].key && o[i].bucket != o[i - 1].bucket;
    }
    check("ordering property: a deeper key never sorts nearer (both paths, bias)", bad_order, 0);
    check("ordering property: equal keys share a bucket (both paths, bias)", bad_equal, 0);
}

/* ------------------------------------------------------------------------ */
/* Properties over whole input classes: clipped lines, ordering bounds
   and lighting without normals. */

/* The planes a draw clips to, as values >= 0 inside, for a view-space
   point (GTE units: x right, y down, z forward), each by margin pixels
   (guard band) or margin x 1e-6 of the depth (near, far) wider: the
   documented planes (jbi_geom_begin, include/jitterbug.h), worked out
   here from the draw's numbers. */
static double clip_plane_value(const jbi_geom_draw *d, int k, const double p[3], double margin)
{
    int gte = d->mode == JB_GEOMETRY_GTE;
    double h = gte ? (double)d->r.h : d->fh;
    double ox = d->width * 0.5, oy = d->height * 0.5;
    double zn = gte ? d->near_f : (double)d->fnear, zf = gte ? d->far_f : (double)d->ffar;

    switch (k) {
    case 0: return p[2] - zn * (1.0 - margin * 1e-6);
    case 1: return zf * (1.0 + margin * 1e-6) - p[2];
    case 2: return (ox + 1024.0 + margin) * p[2] + h * p[0];
    case 3: return (1023.0 + margin - ox) * p[2] - h * p[0];
    case 4: return (oy + 1024.0 + margin) * p[2] + h * p[1];
    default: return (1023.0 + margin - oy) * p[2] - h * p[1];
    }
}

/* A model-space point in view space, in double (the draw's g). */
static void to_view(const jbi_geom_draw *d, const double w[3], double out[3])
{
    int r;

    for (r = 0; r < 3; r++)
        out[r] = (double)d->g[r][0] * w[0] + (double)d->g[r][1] * w[1] +
                 (double)d->g[r][2] * w[2] + (double)d->g[r][3];
}

/* The part lam0..lam1 of the line v1 + lam (v2 - v1) (lam within lo..hi)
   inside the planes widened by margin; 0 when none. v1 and v2 are near
   the view, so this is precise whatever lo and hi are. */
static int clip_segment(const jbi_geom_draw *d, const double v1[3], const double v2[3], double lo,
                   double hi, double margin, double *lam0, double *lam1)
{
    int k;

    for (k = 0; k < 6; k++) {
        double f1 = clip_plane_value(d, k, v1, margin), f2 = clip_plane_value(d, k, v2, margin), g = f2 - f1;

        if (g == 0.0) {
            if (f1 < 0.0)
                return 0;
            continue;
        }
        if (g > 0.0) {
            double t = -f1 / g;

            lo = t > lo ? t : lo;
        } else {
            double t = -f1 / g;

            hi = t < hi ? t : hi;
        }
        if (!(lo <= hi))
            return 0;
    }
    *lam0 = lo;
    *lam1 = hi;
    return 1;
}

static void to_screen(const jbi_geom_draw *d, const double p[3], double *x, double *y)
{
    double h = d->mode == JB_GEOMETRY_GTE ? (double)d->r.h : d->fh;

    *x = d->width * 0.5 + h * p[0] / p[2];
    *y = d->height * 0.5 + h * p[1] / p[2];
}

/*
 * A segment draws its line's visible part wherever its ends are.
 * Random cameras near the origin, both paths, and segments along lines
 * of small integer direction whose ends are 2^0..2^100 times it from a
 * point on them, independently each side (asymmetric), exactly
 * representable as floats (a component the direction leaves 0 has any
 * offset). The reference clips the same line from two points of it near
 * the view, where nothing cancels. The drawn segment must be visible
 * where the reference is (by a margin of 2 pixels), invisible where it is
 * not (by the same margin), and its ends within 2 pixels of the
 * reference's on the screen (in the GTE path, more by the GTE's integer
 * IR and SZ: a unit of each, at the end's depth; and not compared where
 * IR saturates, beyond 32767 GTE units off the axis, where the GTE path
 * projects as the GTE does).
 */
static void clipped_lines_property(void)
{
    enum { CASES = 40000 };
    static one_face f;
    gen g = {5501, 0, 0};
    int n, bad_vis = 0, bad_invis = 0, bad_pos = 0, drawn = 0, worst_case = -1;
    double worst = 0.0;

    for (n = 0; n < CASES; n++) {
        int mode = n & 1, dir[3], c, k1, k2, got, vis, vis_wide;
        double off[3], w1[3], w2[3], v1[3], v2[3], lo, hi, l0, l1, m0, m1;
        jb_camera cam;
        jb_vec3 a, b;
        jbi_geom_vertex e[2];
        int bucket;

        one_face_init(&f, mode, 3);
        memset(&cam, 0, sizeof cam);
        cam.eye.x = (float)(rnd_below(&g, 601) - 300);
        cam.eye.y = (float)(rnd_below(&g, 601) - 300);
        cam.eye.z = (float)(rnd_below(&g, 601) - 300);
        cam.target.x = cam.eye.x + (float)(rnd_below(&g, 201) - 100);
        cam.target.y = cam.eye.y + (float)(rnd_below(&g, 201) - 100);
        cam.target.z = cam.eye.z + (float)(rnd_below(&g, 201) - 100);
        cam.z_near = (float)(1 + rnd_below(&g, 50));
        cam.z_far = (float)(500 + rnd_below(&g, 4000));
        if (jbi_scene_set_camera(&f.s, &cam, detail, sizeof detail) != JB_OK)
            continue;
        if (jbi_geom_begin(&f.d, &f.s, NULL, 320, 240, 1024, JBI_GEOM_NO_MESH, "t", detail,
                           sizeof detail) != JB_OK)
            continue;
        do {
            for (c = 0; c < 3; c++)
                dir[c] = rnd_below(&g, 7) - 3;
        } while (dir[0] == 0 && dir[1] == 0 && dir[2] == 0);
        for (c = 0; c < 3; c++)
            off[c] = dir[c] == 0 ? (double)(rnd_below(&g, 601) - 300) : 0.0;
        k1 = rnd_below(&g, 4) == 0 ? rnd_below(&g, 12) : rnd_below(&g, 101);
        k2 = rnd_below(&g, 4) == 0 ? rnd_below(&g, 12) : rnd_below(&g, 101);
        lo = -ldexp(1.0, k1);
        hi = ldexp(1.0, k2);
        a.x = (float)(off[0] + lo * dir[0]);
        a.y = (float)(off[1] + lo * dir[1]);
        a.z = (float)(off[2] + lo * dir[2]);
        b.x = (float)(off[0] + hi * dir[0]);
        b.y = (float)(off[1] + hi * dir[1]);
        b.z = (float)(off[2] + hi * dir[2]);
        /* Two points of the line near the view: the reference's base. */
        for (c = 0; c < 3; c++) {
            w1[c] = off[c];
            w2[c] = off[c] + dir[c];
        }
        to_view(&f.d, w1, v1);
        to_view(&f.d, w2, v2);
        {
            /* A line through the eye (or nearly) has no image to compare. */
            double dv[3] = {v2[0] - v1[0], v2[1] - v1[1], v2[2] - v1[2]}, m[3], dist;

            m[0] = v1[1] * dv[2] - v1[2] * dv[1];
            m[1] = v1[2] * dv[0] - v1[0] * dv[2];
            m[2] = v1[0] * dv[1] - v1[1] * dv[0];
            dist = sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]) /
                   sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
            if (dist < 4.0)
                continue;
        }
        got = jbi_geom_segment(&f.s.gte, &f.d, a, b, 0xff, 0xff00, e, &bucket);
        vis = clip_segment(&f.d, v1, v2, lo, hi, -2.0, &l0, &l1);
        vis_wide = clip_segment(&f.d, v1, v2, lo, hi, 2.0, &m0, &m1);
        if (vis) {
            /* The visible part a few pixels long at least. */
            double p0[3], p1[3], x0, y0, x1, y1;

            for (c = 0; c < 3; c++) {
                p0[c] = v1[c] + l0 * (v2[c] - v1[c]);
                p1[c] = v1[c] + l1 * (v2[c] - v1[c]);
            }
            to_screen(&f.d, p0, &x0, &y0);
            to_screen(&f.d, p1, &x1, &y1);
            if (fabs(x1 - x0) + fabs(y1 - y0) < 6.0)
                vis = 0;
        }
        if (vis && !got) {
            if (bad_vis++ < 3)
                printf("  case %d (%s): visible, not drawn: k %d %d dir %d %d %d\n", n,
                       mode ? "float" : "GTE", k1, k2, dir[0], dir[1], dir[2]);
        }
        if (!vis_wide && got)
            bad_invis++;
        if (got && vis) {
            /* Each end against the reference's: the planes as drawn, the
               ends of the part inside them. */
            double r0, r1, err = 0.0;
            int i;

            drawn++;
            if (clip_segment(&f.d, v1, v2, lo, hi, 0.0, &r0, &r1)) {
                for (i = 0; i < 2; i++) {
                    double p[3], x, y, dx, dy;

                    for (c = 0; c < 3; c++)
                        p[c] = v1[c] + (i ? r1 : r0) * (v2[c] - v1[c]);
                    to_screen(&f.d, p, &x, &y);
                    double tol = 2.0;

                    if (mode == JB_GEOMETRY_GTE && (fabs(p[0]) >= 32767.0 || fabs(p[1]) >= 32767.0))
                        continue; /* the GTE saturates IR there, as documented */
                    dx = (double)e[i].sx / 256.0 - x;
                    dy = (double)e[i].sy / 256.0 - y;
                    dx = dx < 0 ? -dx : dx;
                    dy = dy < 0 ? -dy : dy;
                    if (mode == JB_GEOMETRY_GTE)
                        /* The GTE's projection: integer IR (a unit, h / z
                           pixels) and integer SZ (the offset from the
                           centre over z). */
                        tol += (fabs(x - 160.0) + fabs(y - 120.0) + (double)f.d.r.h) / p[2];
                    err = dx / tol > err ? dx / tol : err;
                    err = dy / tol > err ? dy / tol : err;
                }
                if (err > 1.0) {
                    if (bad_pos++ < 3)
                        printf("  case %d (%s): an end %.2f x tolerance off: k %d %d "
                               "dir %d %d %d\n",
                               n, mode ? "float" : "GTE", err, k1, k2, dir[0], dir[1], dir[2]);
                }
                if (err > worst) {
                    worst = err;
                    worst_case = n;
                }
            }
        }
    }
    check("clipped lines property: every visible segment drawn (any ends)", bad_vis, 0);
    check("clipped lines property: no invisible segment drawn (any ends)", bad_invis, 0);
    check("clipped lines property: a drawn segment's ends where its line leaves the view", bad_pos,
          0);
    check("clipped lines property: many segments compared", drawn > 2000, 1);
    printf("%d segments drawn and compared, worst end %.2f x tolerance (case %d)\n",
           drawn, worst,
           worst_case);
}

/*
 * Both paths order over the same bounds whenever the GTE path can
 * draw: for random cameras (projection distances to 65535, near and far
 * planes from far apart to within one integer depth of each other, the
 * GTE path's near plane often raised to H / 2 + 1 right by the far one),
 * where the GTE path's set-up succeeds the float path's does too, with
 * the same ordering scale and bounds, so the same key goes in the same
 * bucket in either.
 */
static void ordering_bounds_property(void)
{
    enum { CASES = 20000 };
    static jbi_scene s;
    static jbi_geom_draw dg, df;
    gen g = {5503, 0, 0};
    int n, compared = 0, bad = 0, thin = 0;

    for (n = 0; n < CASES; n++) {
        jb_camera cam;
        float sc;

        jbi_scene_init(&s);
        memset(&cam, 0, sizeof cam);
        cam.target.z = -1.0f;
        cam.z_far = (float)(1 + rnd_below(&g, 32767)) + (float)rnd_below(&g, 4) * 0.25f;
        for (sc = 1.0f; cam.z_far * sc * 2.0f <= 32767.0f; sc *= 2.0f)
            ;
        if (rnd_below(&g, 2)) {
            /* H / 2 + 1 just at or below the far plane's GTE depth. */
            int fz = (int)(cam.z_far * sc), hh = 2 * (fz - 1 - rnd_below(&g, 3));

            cam.projection = (float)(hh < 2 ? 2 : hh > 65535 ? 65535 : hh);
        } else {
            cam.projection = rnd_below(&g, 2) ? 0.0f : (float)(1 + rnd_below(&g, 65535));
        }
        cam.z_near = cam.z_far * (float)(1 + rnd_below(&g, 1000)) / 1001.0f;
        if (rnd_below(&g, 2))
            cam.z_near = cam.z_far - (float)(1 + rnd_below(&g, 4)) / (4.0f * sc);
        if (!(cam.z_near > 0.0f) || jbi_scene_set_camera(&s, &cam, detail, sizeof detail) != JB_OK)
            continue;
        s.geometry = JB_GEOMETRY_GTE;
        if (jbi_geom_begin(&dg, &s, NULL, 320, 240, 1024, JBI_GEOM_NO_MESH, "t", detail,
                           sizeof detail) != JB_OK)
            continue;
        s.geometry = JB_GEOMETRY_FLOAT;
        if (jbi_geom_begin(&df, &s, NULL, 320, 240, 1024, JBI_GEOM_NO_MESH, "t", detail,
                           sizeof detail) != JB_OK) {
            bad++;
            continue;
        }
        compared++;
        thin += dg.near_z == dg.far_z;
        bad += dg.r.zsf3 != df.r.zsf3 || dg.near_z != df.near_z || dg.far_z != df.far_z ||
               dg.otz_near != df.otz_near || dg.otz_far != df.otz_far;
    }
    check("ordering bounds property: both paths order over one scale and bounds", bad, 0);
    check("ordering bounds property: cases compared, thin ones among them",
          compared > 5000 && thin > 100, 1);
    printf("%d cameras compared, %d within one integer depth\n", compared, thin);
}

/*
 * A face with no normals is lit by its own direction, exactly: for
 * random faces of int16 positions, thin ones among them (a long edge
 * and a third vertex a few units from its end), lit
 * by one light along -z with no ambient, both paths, the colour must be
 * that of the same face given the normal of the exact cross product of
 * its edges (in 64-bit integers, normalised, x 4096).
 */
static void normal_free_lighting_property(void)
{
    enum { CASES = 20000 };
    static one_face a, b;
    gen g = {5505, 0, 0};
    int n, bad = 0, compared = 0;

    for (n = 0; n < CASES; n++) {
        int mode = n & 1, i, k, p[3][3];
        int64_t e1[3], e2[3], cr[3];
        double len;
        jb_light l;
        jb_mat4 at = jb_mat4_translate(0.0f, 0.0f, -2000.0f);
        int ga, gb;

        if (rnd_below(&g, 4) != 0) {
            /* Thin: a long edge (products of its components near 1e9,
               past a float's 24 bits) and a third vertex a few units from
               its end, so the exact cross product is small and float
               products of the edges cancel to nothing or to noise. */
            for (k = 0; k < 3; k++) {
                p[0][k] = rnd_below(&g, 32001) - 16000;
                p[1][k] = p[0][k] + rnd_below(&g, 32001) - 16000;
                p[2][k] = p[1][k] + rnd_below(&g, 5) - 2;
            }
        } else {
            for (k = 0; k < 3; k++) {
                p[0][k] = rnd_below(&g, 65536) - 32768;
                p[1][k] = rnd_below(&g, 65536) - 32768;
                p[2][k] = rnd_below(&g, 65536) - 32768;
            }
        }
        for (k = 0; k < 3; k++) {
            e1[k] = (int64_t)p[1][k] - p[0][k];
            e2[k] = (int64_t)p[2][k] - p[0][k];
        }
        cr[0] = e1[1] * e2[2] - e1[2] * e2[1];
        cr[1] = e1[2] * e2[0] - e1[0] * e2[2];
        cr[2] = e1[0] * e2[1] - e1[1] * e2[0];
        len = sqrt((double)cr[0] * (double)cr[0] + (double)cr[1] * (double)cr[1] +
                   (double)cr[2] * (double)cr[2]);
        if (len == 0.0)
            continue;
        for (i = 0; i < 2; i++) {
            one_face *o = i ? &b : &a;

            one_face_init(o, mode, 3);
            o->mat.double_sided = 1;
            o->mat.color = JB_RGB(255, 255, 255);
            for (k = 0; k < 3; k++)
                put(&o->pos[k], p[k][0], p[k][1], p[k][2]);
            jbi_scene_set_ambient(&o->s, 0, detail, sizeof detail);
            memset(&l, 0, sizeof l);
            l.direction.z = -1.0f;
            l.color = JB_RGB(255, 255, 255);
            jbi_scene_set_light(&o->s, 0, &l, detail, sizeof detail);
            if (i) { /* the exact normal, given */
                for (k = 0; k < 3; k++)
                    put(&o->nrm[k], (int)floor((double)cr[0] / len * 4096.0 + 0.5),
                        (int)floor((double)cr[1] / len * 4096.0 + 0.5),
                        (int)floor((double)cr[2] / len * 4096.0 + 0.5));
                o->m.normals = o->nrm;
            }
        }
        /* A model scale small enough for the GTE to take any int16 mesh. */
        {
            jb_mat4 s = jb_mat4_scale(1.0f / 64.0f, 1.0f / 64.0f, 1.0f / 64.0f);

            at = jb_mat4_mul(&at, &s);
        }
        ga = run_face(&a, &at);
        gb = run_face(&b, &at);
        if (ga != gb) {
            bad++;
            continue;
        }
        if (ga < 1)
            continue;
        compared++;
        for (k = 0; k < a.p[0].n && k < b.p[0].n; k++)
            if (a.p[0].v[k].color != b.p[0].v[k].color) {
                if (bad++ < 3)
                    printf("  case %d (%s): %06x without normals, %06x with\n", n,
                           mode ? "float" : "GTE", (unsigned)a.p[0].v[k].color,
                           (unsigned)b.p[0].v[k].color);
                break;
            }
    }
    check("normal free lighting property: a face without normals lit by its exact direction", bad,
          0);
    check("normal free lighting property: many faces compared", compared > 2000, 1);
}

/* The packed SXY of a polygon's vertex, as the GTE keeps it. */
static uint32_t sxy_of(const jbi_geom_vertex *v)
{
    return (uint32_t)(uint16_t)v->x | (uint32_t)(uint16_t)v->y << 16;
}

/* The orientation of three points about the eye (the origin): the sign
   says which side the eye is on. */
static double orient(int p[3][3])
{
    double a[3], b[3], c[3];
    int k;

    for (k = 0; k < 3; k++)
        a[k] = p[0][k], b[k] = p[1][k], c[k] = p[2][k];
    return a[0] * (b[1] * c[2] - b[2] * c[1]) + a[1] * (b[2] * c[0] - b[0] * c[2]) +
           a[2] * (b[0] * c[1] - b[1] * c[0]);
}

/*
 * The float path culls a face as the header says facing is judged, and as
 * the GTE path judges it: by NCLIP's sign for its corners as rounded to the
 * screen (the GPU's first triangle), not by its unrounded orientation in
 * view space. Faces almost edge on, where the two can disagree: each is
 * drawn single-sided exactly when the corners its double-sided twin is
 * drawn with give NCLIP below 0. Before the fix, a face turned away by a
 * hair whose rounded corners faced the eye was dropped (a pixel's gap on a
 * silhouette), and one facing the eye whose rounded corners did not was
 * drawn the wrong way round.
 */
static void float_cull_on_screen(void)
{
    static one_face o;
    jb_mat4 id = jb_mat4_identity();
    gen g = {12345, 0, 0};
    int n, k, bad = 0, bad_precise = 0, tried = 0, away_drawn = 0, facing_culled = 0;
    int only_1x = 0, only_precise = 0;
    double front;

    /* Which sign of orient faces the eye: a large face, plainly drawn. */
    {
        int p[3][3] = {{-100, -100, -500}, {100, -100, -500}, {0, 100, -500}};
        int r[2];
        for (k = 0; k < 2; k++) {
            one_face_init(&o, JB_GEOMETRY_FLOAT, 3);
            put(&o.pos[0], p[0][0], p[0][1], p[0][2]);
            put(&o.pos[1 + k], p[1][0], p[1][1], p[1][2]);
            put(&o.pos[2 - k], p[2][0], p[2][1], p[2][2]);
            r[k] = run_face(&o, &id);
        }
        check("float cull: one winding of a plain face drawn, the other culled", r[0] + r[1], 1);
        front = r[0] == 1 ? orient(p) : -orient(p);
    }
    for (n = 0; n < 40000; n++) {
        int p[3][3], drawn, mac, single, precise_front;
        int64_t w;
        double a = 0.2 + 0.6 * rnd_below(&g, 1001) / 1000.0, b = 0.2 + 0.6 * rnd_below(&g, 1001) / 1000.0;
        double o3;

        for (k = 0; k < 2; k++) {
            int z = -(200 + rnd_below(&g, 1800));
            p[k][2] = z;
            p[k][0] = (rnd_below(&g, 1001) - 500) * -z / 1000;
            p[k][1] = (rnd_below(&g, 801) - 400) * -z / 1000;
        }
        /* The third corner on the plane through the eye and the first two,
           a unit or so off it: the face is seen almost edge on. */
        for (k = 0; k < 3; k++)
            p[2][k] = (int)floor(a * p[0][k] + b * p[1][k] + 0.5) + rnd_below(&g, 3) - 1;
        if (p[2][2] > -100)
            continue;
        one_face_init(&o, JB_GEOMETRY_FLOAT, 3);
        for (k = 0; k < 3; k++)
            put(&o.pos[k], p[k][0], p[k][1], p[k][2]);
        o.mat.double_sided = 1;
        if (run_face(&o, &id) != 1 || !o.p[0].exact)
            continue;
        mac = jb_gte_nclip(sxy_of(&o.p[0].v[0]), sxy_of(&o.p[0].v[1]), sxy_of(&o.p[0].v[2]), NULL);
        /* The same on the sub-pixel positions the precise scaled pass draws. */
        w = ((int64_t)o.p[0].v[1].sx - o.p[0].v[0].sx) * ((int64_t)o.p[0].v[2].sy - o.p[0].v[0].sy) -
            ((int64_t)o.p[0].v[2].sx - o.p[0].v[0].sx) * ((int64_t)o.p[0].v[1].sy - o.p[0].v[0].sy);
        precise_front = w < 0;
        o.mat.double_sided = 0;
        single = run_face(&o, &id);
        drawn = drawn_1x(o.p, single);
        tried++;
        if (drawn != (mac < 0) && bad++ < 3)
            printf("  float cull case %d: NCLIP %d, %s\n", n, (int)mac, drawn ? "drawn" : "culled");
        if (drawn_precise(o.p, single) != precise_front && bad_precise++ < 3)
            printf("  float cull case %d: sub-pixel winding %lld, precise pass %s\n", n,
                   (long long)w, precise_front ? "culled" : "drawn");
        only_1x += single == 1 && o.p[0].passes == JBI_GEOM_PASS_1X;
        only_precise += single == 1 && o.p[0].passes == JBI_GEOM_PASS_PRECISE;
        o3 = orient(p) * front;
        away_drawn += drawn && !(o3 > 0);
        facing_culled += !drawn && o3 > 0;
    }
    printf("float cull: %d faces almost edge on, %d turned away drawn, %d facing culled; "
           "%d for the 1x pass alone, %d for the precise pass alone\n",
           tried, away_drawn, facing_culled, only_1x, only_precise);
    check("float cull: faces culled by NCLIP of their rounded corners", bad, 0);
    check("float cull: the precise pass culls by the sub-pixel winding", bad_precise, 0);
    check("float cull: some faces for the 1x pass alone", only_1x > 0, 1);
    check("float cull: some faces for the precise pass alone", only_precise > 0, 1);
    check("float cull: enough faces", tried > 10000, 1);
    check("float cull: some turned away by a hair, drawn", away_drawn > 0, 1);
    check("float cull: some facing by a hair, culled", facing_culled > 0, 1);
}

static void clip_and_bound_properties(void)
{
    clipped_lines_property();
    ordering_bounds_property();
    normal_free_lighting_property();
}

int main(void)
{
    setters();
    setup();
    meshes();
    prims();
    clipping(JB_GEOMETRY_GTE);
    clipping(JB_GEOMETRY_FLOAT);
    points(JB_GEOMETRY_GTE);
    points(JB_GEOMETRY_FLOAT);
    neutral(JB_GEOMETRY_GTE);
    neutral(JB_GEOMETRY_FLOAT);
    geometry_cases();
    pipeline_cases();
    ordering_property();
    clip_and_bound_properties();
    float_cull_on_screen();
    equivalence(42, 400);
    return finish("geom_pipeline");
}

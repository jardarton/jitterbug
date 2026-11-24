/*
 * The friendly 3D pipeline (pipeline.h): per-draw set-up, quantisation
 * into the GTE's registers, and faces through the GTE path or the float
 * path, with near, far and guard-band clipping.
 *
 * Arithmetic: the GTE path's results for exact faces come from the GTE
 * itself (src/geom/gte.c). Everything else is integer, or IEEE double and
 * float written one operation at a time (fmath.c says why that is the same
 * on every platform). Clipping works in double on the GTE's exact 44-bit
 * sums (GTE units x 4096, which double holds exactly), or on the float
 * path's view positions.
 */
#include "geom/pipeline.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "geom/fmath.h"

typedef int64_t s64;

static jb_result fail(char *detail, size_t n, const char *fn, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 4, 5)))
#endif
    ;

static jb_result fail(char *detail, size_t n, const char *fn, const char *fmt, ...)
{
    char msg[200];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (detail != NULL && n > 0)
        snprintf(detail, n, "%s: %s", fn, msg);
    return JB_ERROR_INVALID_ARGUMENT;
}

static int finite_d(double x)
{
    return x - x == 0.0;
}

/* x within +-2^62, so that converting it to an integer is defined: NaN
   is 0. Every conversion below goes through this, whatever its input. */
static double tame(double x)
{
    const double lim = 4611686018427387904.0; /* 2^62 */

    if (!(x == x))
        return 0.0;
    return x > lim ? lim : x < -lim ? -lim : x;
}

/* Nearest integer, halves away from zero (saturated at +-2^62). */
static s64 round_d(double x)
{
    x = tame(x);
    return x >= 0.0 ? (s64)(x + 0.5) : -(s64)(-x + 0.5);
}

/* Largest integer not above x (saturated at +-2^62). */
static s64 floor_d(double x)
{
    s64 i;

    x = tame(x);
    i = (s64)x;
    return (double)i > x ? i - 1 : i;
}

/* Nearest integer, halves up (as record_precise rounds); |x| below 2^62. */
static s64 round_up(double x)
{
    return floor_d(x + 0.5);
}

static s64 ceil_d(double x)
{
    s64 i;

    x = tame(x);
    i = (s64)x;

    return (double)i < x ? i + 1 : i;
}

static s64 clamp64(s64 v, s64 lo, s64 hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int finite3(jb_vec3 v)
{
    return finite_d((double)v.x) && finite_d((double)v.y) && finite_d((double)v.z);
}

/* A colour channel 0..255 as a 4.12 light level, as jb_gte_set_lights. */
static int32_t level(uint32_t c)
{
    return (int32_t)((c * 4096u + 127u) / 255u);
}

#define M4(a, r, c) ((a).m[(c) * 4 + (r)])

/* ------------------------------------------------------------------------ */
/* Meshes */

jb_result jbi_geom_check_mesh(const jb_mesh *m, const char *fn, int *extent, char *detail,
                              size_t n)
{
    int i, k, e = 0;

    if (m == NULL)
        return fail(detail, n, fn, "mesh is NULL");
    if (m->vertex_count < 1 || m->vertex_count > 65536)
        return fail(detail, n, fn, "vertex_count %d is outside 1..65536", m->vertex_count);
    if (m->positions == NULL)
        return fail(detail, n, fn, "positions is NULL");
    if (m->face_count < 0)
        return fail(detail, n, fn, "face_count %d is negative", m->face_count);
    if (m->face_count > 0) {
        if (m->faces == NULL)
            return fail(detail, n, fn, "faces is NULL but face_count is %d", m->face_count);
        if (m->material_count < 1 || m->material_count > 65536)
            return fail(detail, n, fn, "material_count %d is outside 1..65536",
                        m->material_count);
        if (m->materials == NULL)
            return fail(detail, n, fn, "materials is NULL");
    }
    for (i = 0; m->face_count > 0 && i < m->material_count; i++) {
        const jb_material *t = &m->materials[i];

        if (t->color > 0xffffffu && t->color != JB_TINT_BLACK)
            return fail(detail, n, fn, "materials[%d].color 0x%lx is above 0xFFFFFF", i,
                        (unsigned long)t->color);
        if (t->blend < JB_BLEND_NONE || t->blend > JB_BLEND_QUARTER)
            return fail(detail, n, fn, "materials[%d].blend %d is not a JB_BLEND_* mode", i,
                        t->blend);
    }
    for (i = 0; i < m->face_count; i++) {
        const jb_mesh_face *f = &m->faces[i];

        if (f->vertices != 3 && f->vertices != 4)
            return fail(detail, n, fn, "faces[%d].vertices is %u, not 3 or 4", i,
                        (unsigned)f->vertices);
        for (k = 0; k < f->vertices; k++)
            if (f->v[k] >= m->vertex_count)
                return fail(detail, n, fn, "faces[%d].v[%d] is %u, but there are %d vertices",
                            i, k, (unsigned)f->v[k], m->vertex_count);
        if (f->material >= m->material_count)
            return fail(detail, n, fn, "faces[%d].material is %u, but there are %d materials", i,
                        (unsigned)f->material, m->material_count);
        {
            /* Texel coordinates within the texture (not past it into its
               neighbours in VRAM), but a repeat texture's, which the window
               wraps, and a hand-made one's without a size. */
            const jb_texture *t = m->materials[f->material].texture;

            for (k = 0; m->uv != NULL && t != NULL && !t->repeat && t->width > 0 &&
                        t->height > 0 && k < f->vertices;
                 k++) {
                int u = m->uv[2 * f->v[k]], v = m->uv[2 * f->v[k] + 1];

                if (u >= t->width || v >= t->height)
                    return fail(detail, n, fn,
                                "faces[%d].v[%d] (vertex %u) has u, v (%d, %d), outside its "
                                "%d x %d texture",
                                i, k, (unsigned)f->v[k], u, v, t->width, t->height);
            }
        }
    }
    for (i = 0; m->colors != NULL && i < m->vertex_count; i++)
        if (m->colors[i] > 0xffffffu)
            return fail(detail, n, fn, "colors[%d] 0x%lx is above 0xFFFFFF", i,
                        (unsigned long)m->colors[i]);
    for (i = 0; i < m->vertex_count; i++) {
        const jb_gte_vec *p = &m->positions[i];
        int c[3] = {p->x, p->y, p->z};

        for (k = 0; k < 3; k++) {
            int a = c[k] < 0 ? -c[k] : c[k];

            e = a > e ? a : e;
        }
    }
    if (extent != NULL)
        *extent = e;
    return JB_OK;
}

/* ------------------------------------------------------------------------ */
/* Set-up */

/* The GTE path's rotation and translation registers from d->g (GTE path
   only). */
static jb_result quantise(jbi_geom_draw *d, int extent, float sc, const char *fn, char *detail,
                          size_t n)
{
    double a[3][3];
    int r, c, j;

    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            a[r][c] = (double)d->g[r][c] * 4096.0;
    for (j = 0; j <= 15; j++) {
        double scale = 1.0 / (double)(1 << j);
        int fits = 1;

        for (r = 0; r < 3; r++)
            for (c = 0; c < 3; c++)
                fits &= a[r][c] * scale < 32767.5 && a[r][c] * scale > -32768.5;
        if (fits)
            break;
    }
    if (j > 15)
        return fail(detail, n, fn,
                    "model x view x %g (GTE units) has an element beyond the GTE's range even "
                    "with vertices x 32768",
                    (double)sc);
    if (((s64)extent << j) > 32767)
        return fail(detail, n, fn,
                    "the mesh (coordinates up to %d) is too large for the GTE with this model "
                    "matrix and camera: model x view needs its vertices x %d, beyond 16 bits",
                    extent, 1 << j);
    d->shift = j;
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            d->r.rt.m[r][c] = (int16_t)clamp64(round_d(a[r][c] / (double)(1 << j)), -32768, 32767);
    for (r = 0; r < 3; r++) {
        double tv = (double)d->g[r][3];
        const double lim = 2147483648.0 - 1048576.0;

        if (!(tv <= lim && tv >= -lim))
            return fail(detail, n, fn,
                        "the model's origin is %g GTE units from the camera, beyond the GTE's "
                        "range",
                        tv);
        d->r.tr[r] = (int32_t)round_d(tv);
    }
    return JB_OK;
}

jb_result jbi_geom_begin(jbi_geom_draw *d, const jbi_scene *s, const jb_mat4 *model, int w,
                         int h, int ot_size, int extent, const char *fn, char *detail, size_t n)
{
    const jb_camera *cam = &s->camera;
    int gte = s->geometry == JB_GEOMETRY_GTE;
    jb_mat4 mv = jb_mat4_mul(&s->view, model), mm;
    float sc = 1.0f, t, fov;
    double hd;
    s64 hh, nz, fz, zk;
    int r, c, k;

    memset(d, 0, sizeof *d);
    d->mode = s->geometry;
    d->width = w;
    d->height = h;
    d->ot_size = ot_size;

    /* The scale: the largest power of two with z_far x scale <= 32767. */
    for (k = 0; k < 160 && cam->z_far * sc > 32767.0f; k++)
        sc = sc * 0.5f;
    for (k = 0; k < 160 && cam->z_far * (sc * 2.0f) <= 32767.0f && sc < 0x1p100f; k++)
        sc = sc * 2.0f;
    d->scale = sc;

    /* Model to GTE units, in the GTE's axes: x right, y down, z forward. */
    for (r = 0; r < 3; r++)
        for (c = 0; c < 4; c++) {
            float v = sc * M4(mv, r, c);

            d->g[r][c] = r == 0 ? v : -v;
            if (!finite_d((double)d->g[r][c]))
                return fail(detail, n, fn, "model x view is not finite (or too large)");
        }

    /* The projection: the camera's projection distance, or from its field
       of view (in float, as before, and in double where that overflows: a
       field of view of 1e-36 degrees is accepted and must still draw). */
    if (cam->projection > 0.0f) {
        d->fh = (double)cam->projection;
    } else {
        float fh;

        fov = cam->fov_y * 0.017453292f;
        t = jb_tanf(fov * 0.5f);
        fh = (float)h * 0.5f / t;
        d->fh = finite_d((double)fh) ? (double)fh : (double)h * 0.5 / (double)t;
    }
    if (!(d->fh > 0.0 && d->fh < 1e300))
        return fail(detail, n, fn,
                    "the field of view of %g degrees is too narrow to project (its tangent is "
                    "0 in float)",
                    (double)cam->fov_y);
    d->fofx = (float)w * 0.5f;
    d->fofy = (float)h * 0.5f;
    d->fnear = cam->z_near * sc;
    d->ffar = cam->z_far * sc;
    hd = d->fh;
    hh = hd < 70000.0 ? round_d(hd) : 70000;
    if (gte && (hh < 1 || hh > 65535))
        return fail(detail, n, fn,
                    "the field of view of %g degrees (or the projection distance %g) makes H "
                    "%lld for a display %d high, outside the GTE's 1..65535",
                    (double)cam->fov_y, (double)cam->projection, (long long)hh, h);
    d->r.h = (int32_t)hh;
    d->r.ofx = (int32_t)w * 32768;
    d->r.ofy = (int32_t)h * 32768;

    /* The depth range. The GTE path clips at the planes themselves, not
       rounded (near_f, far_f: a face just inside a fractional plane is
       drawn), and quantises depths only for its registers and records.
       Both paths order over the same depths, near_z..far_z, so switching
       paths within a frame keeps the order. The range is valid when the
       planes have room between them, however little: a range within one
       integer depth draws at that SZ. Its ordering bounds are quantised
       each on its own, the near one no farther than the far one. */
    d->near_f = (double)cam->z_near * (double)sc;
    if (d->near_f < (double)(hh / 2 + 1))
        d->near_f = (double)(hh / 2 + 1);
    d->far_f = (double)cam->z_far * (double)sc;
    nz = ceil_d(d->near_f);
    fz = floor_d(d->far_f);
    if (gte && !(d->near_f < d->far_f))
        return fail(detail, n, fn,
                    "the near plane is at GTE depth %g (at least H / 2 + 1, H %lld), not "
                    "nearer than the far plane at %g",
                    d->near_f, (long long)hh, d->far_f);
    if (d->near_f < d->far_f) {
        /* The GTE path's planes have room (however little): both paths
           order over its bounds, so the same key gets the same bucket in
           either. Planes within one integer depth order at that depth. */
        if (nz > fz)
            nz = fz;
    } else {
        /* A float camera with no room for the GTE path (its near plane,
           at H / 2 + 1 at least, is not nearer than the far one): the
           float path orders from its own near plane; the GTE path cannot
           draw with this camera at all. */
        nz = ceil_d((double)d->fnear);
        if (nz > fz)
            nz = fz;
    }
    d->near_z = (int32_t)nz;
    d->far_z = (int32_t)fz;
    /* The ordering key. ZSF3 = 4k and ZSF4 = 3k, so AVSZ3 and AVSZ4 both
       give OTZ = floor(12 k x average depth / 4096), one scale for
       triangles and quads (and for clipped polygons, points and segments,
       computed the same way); k as large as keeps OTZ for the far plane
       within 16 bits and ZSF3 within 15. OTZ is then mapped linearly onto
       the buckets between the near and far planes' OTZ (bucket()). */
    zk = fz > 0 ? (65535 * (s64)4096) / (12 * fz) : 8191;
    zk = zk > 8191 ? 8191 : zk < 1 ? 1 : zk;
    d->r.zsf3 = (int16_t)(4 * zk);
    d->r.zsf4 = (int16_t)(3 * zk);
    d->otz_near = (int32_t)((12 * zk * (nz > 0 ? nz : 0)) >> 12);
    d->otz_far = (int32_t)((12 * zk * (fz > 0 ? fz : 0)) >> 12);
    if (d->otz_far < d->otz_near)
        d->otz_far = d->otz_near;

    /* The rotation, 4.12, with the smallest vertex shift that fits it: an
       element fits when it rounds (halves away from zero) into
       -32768..32767, so when it lies in the open interval (-32768.5,
       32767.5) once divided by 2^shift (exact: a power of two). The float
       path has no registers: none of this is done for it. */
    if (gte && extent >= 0) {
        jb_result q = quantise(d, extent, sc, fn, detail, n);

        if (q != JB_OK)
            return q;
    }

    /* Lights, turned into model space by the model's rotation. */
    mm = model != NULL ? *model : jb_mat4_identity();
    {
        jb_vec3 col[3];

        for (c = 0; c < 3; c++) {
            col[c].x = M4(mm, 0, c);
            col[c].y = M4(mm, 1, c);
            col[c].z = M4(mm, 2, c);
            col[c] = jb_vec3_normalize(col[c]);
        }
        for (k = 0; k < JBI_LIGHTS; k++) {
            jb_vec3 l = s->light_to[k], o;
            uint32_t lc = s->light_color[k];

            if (lc == 0)
                continue;
            o.x = jb_vec3_dot(col[0], l);
            o.y = jb_vec3_dot(col[1], l);
            o.z = jb_vec3_dot(col[2], l);
            o = jb_vec3_normalize(o);
            if (!finite3(o))
                o = l; /* a model matrix that is not a rotation and scale */
            d->lto[k][0] = o.x;
            d->lto[k][1] = o.y;
            d->lto[k][2] = o.z;
            d->r.llm.m[k][0] = (int16_t)clamp64(round_d((double)o.x * 4096.0), -4096, 4096);
            d->r.llm.m[k][1] = (int16_t)clamp64(round_d((double)o.y * 4096.0), -4096, 4096);
            d->r.llm.m[k][2] = (int16_t)clamp64(round_d((double)o.z * 4096.0), -4096, 4096);
            for (c = 0; c < 3; c++) {
                uint32_t ch = (lc >> (8 * c)) & 0xffu;

                double lv = (double)level(ch) * (double)s->light_intensity[k];

                d->r.lcm.m[c][k] = (int16_t)clamp64(round_d(lv), 0, 32767);
                d->lcol[k][c] = (float)ch / 255.0f * s->light_intensity[k];
            }
        }
        for (c = 0; c < 3; c++) {
            uint32_t ch = (s->ambient >> (8 * c)) & 0xffu;

            d->r.bk[c] = level(ch);
            d->amb[c] = (float)ch / 255.0f;
        }
    }

    /* Fog: the depth cue. */
    d->fog = s->fog;
    if (s->fog) {
        /* In double from the start: the far end times the scale can pass
           the largest float. DQA divides by the H the GTE is given (its
           quotient is that H / SZ), not the ideal focal length. */
        double zn = (double)s->fog_start * (double)sc, zf = (double)s->fog_end * (double)sc;
        double qa = -256.0 * zn * zf / ((zf - zn) * (double)hh);
        double qb = 16777216.0 * zf / (zf - zn);

        if (gte && !(qa > -32768.5 && qa < 32767.5 && qb > -2147483648.5 && qb < 2147483647.5))
            return fail(detail, n, fn,
                        "fog from %g to %g is too sharp for the GTE's depth cue: DQA %g (16 "
                        "bits) or DQB %g (32 bits) is out of range",
                        (double)s->fog_start, (double)s->fog_end, qa, qb);
        d->r.dqa = (int16_t)clamp64(round_d(qa), -32768, 32767);
        d->r.dqb = (int32_t)clamp64(round_d(qb), -2147483647 - 1, 2147483647);
        d->fog_near = zn;
        d->fog_far = zf;
        for (c = 0; c < 3; c++) {
            uint32_t ch = (s->fog_color >> (8 * c)) & 0xffu;

            d->r.fc[c] = (int32_t)ch * 16;
            d->fog_color[c] = (float)ch;
        }
    }
    return JB_OK;
}

static void put_matrix(jbi_gte *g, int first, const jb_gte_matrix *a)
{
    const int16_t(*m)[3] = a->m;

    jbi_gte_write_ctrl(g, first + 0, JB_GTE_PAIR(m[0][0], m[0][1]));
    jbi_gte_write_ctrl(g, first + 1, JB_GTE_PAIR(m[0][2], m[1][0]));
    jbi_gte_write_ctrl(g, first + 2, JB_GTE_PAIR(m[1][1], m[1][2]));
    jbi_gte_write_ctrl(g, first + 3, JB_GTE_PAIR(m[2][0], m[2][1]));
    jbi_gte_write_ctrl(g, first + 4, (uint32_t)(int32_t)m[2][2]);
}

void jbi_geom_load(jbi_gte *g, const jbi_geom_draw *d)
{
    const jbi_gte_regs *r = &d->r;
    int i;

    if (d->mode != JB_GEOMETRY_GTE)
        return;
    put_matrix(g, JBI_GTE_R11R12, &r->rt);
    put_matrix(g, JBI_GTE_L11L12, &r->llm);
    put_matrix(g, JBI_GTE_LR1LR2, &r->lcm);
    for (i = 0; i < 3; i++) {
        jbi_gte_write_ctrl(g, JBI_GTE_TRX + i, (uint32_t)r->tr[i]);
        jbi_gte_write_ctrl(g, JBI_GTE_RBK + i, (uint32_t)r->bk[i]);
        jbi_gte_write_ctrl(g, JBI_GTE_RFC + i, (uint32_t)r->fc[i]);
    }
    jbi_gte_write_ctrl(g, JBI_GTE_OFX, (uint32_t)r->ofx);
    jbi_gte_write_ctrl(g, JBI_GTE_OFY, (uint32_t)r->ofy);
    jbi_gte_write_ctrl(g, JBI_GTE_H, (uint32_t)r->h);
    jbi_gte_write_ctrl(g, JBI_GTE_DQA, (uint32_t)(int32_t)r->dqa);
    jbi_gte_write_ctrl(g, JBI_GTE_DQB, (uint32_t)r->dqb);
    jbi_gte_write_ctrl(g, JBI_GTE_ZSF3, (uint32_t)(int32_t)r->zsf3);
    jbi_gte_write_ctrl(g, JBI_GTE_ZSF4, (uint32_t)(int32_t)r->zsf4);
}

/* ------------------------------------------------------------------------ */
/* Clipping, in GTE units (x 4096 in the GTE path) */

typedef struct cvert {
    double p[3];  /* x right, y down, z forward */
    double c[3];  /* colour channels, before lighting */
    double n[3];  /* the normal, 4.12 */
    double uv[2];
    /* The same attributes interpolated in view space at every cut (the
       perspective-correct set); c, n, uv are interpolated along the screen
       at the guard band's planes (the affine set). Equal at a face's own
       vertices and at near and far cuts. */
    double pc[3], pn[3], puv[2];
    /* The guard band's plane each screen axis lies on: 2..5 (planes()),
       or -1 for none. */
    int on[2];
} cvert;

enum { PLANES = 6 };

/* The guard band's right and bottom planes: the GTE's and the GPU's last
   coordinate, 1023, exactly (a vertex cut there has its sub-pixel position
   on 1023 too). Not one past it: a vertex at 1024 has no GPU position, and
   rounded back to 1023 while its sub-pixel position stays beyond, it is
   inconsistent with the range the splitter clips to. */
static double guard_hi(void)
{
    return (double)JBI_GEOM_GUARD_MAX;
}

/* The projection distance the planes and the screen positions use. */
static double focal(const jbi_geom_draw *d)
{
    return d->mode == JB_GEOMETRY_GTE ? (double)d->r.h : (double)d->fh;
}

/* The planes, a x + b y + c z + e >= 0 inside: near, far, then the guard
   band's left, right, top and bottom (screen x = ofx + h x / z within
   JBI_GEOM_GUARD_MIN..MAX). unit is 4096 for the GTE path's sums, 1 for
   the float path. */
static void planes(const jbi_geom_draw *d, double unit, double pl[PLANES][4])
{
    double h = focal(d), ox = (double)d->width * 0.5, oy = (double)d->height * 0.5;
    double lo = (double)JBI_GEOM_GUARD_MIN, hi = guard_hi();
    double nz, fz;

    if (d->mode == JB_GEOMETRY_GTE) {
        nz = d->near_f * unit;
        fz = d->far_f * unit;
    } else {
        nz = (double)d->fnear;
        fz = (double)d->ffar;
    }
    memset(pl, 0, sizeof(double) * PLANES * 4);
    pl[0][2] = 1.0;
    pl[0][3] = -nz;
    pl[1][2] = -1.0;
    pl[1][3] = fz;
    pl[2][0] = h;
    pl[2][2] = ox - lo;
    pl[3][0] = -h;
    pl[3][2] = hi - ox;
    pl[4][1] = h;
    pl[4][2] = oy - lo;
    pl[5][1] = -h;
    pl[5][2] = hi - oy;
}

static double side(const double pl[4], const double p[3])
{
    double s = pl[0] * p[0];

    s = s + pl[1] * p[1];
    s = s + pl[2] * p[2];
    return s + pl[3];
}

/* Whether a comes before b in a fixed order of the vertices (position,
   then the attributes). */
static int before(const cvert *a, const cvert *b)
{
    double ka[11], kb[11];
    int i;

    memcpy(ka, a->p, sizeof a->p);
    memcpy(ka + 3, a->c, sizeof a->c);
    memcpy(ka + 6, a->n, sizeof a->n);
    memcpy(ka + 9, a->uv, sizeof a->uv);
    memcpy(kb, b->p, sizeof b->p);
    memcpy(kb + 3, b->c, sizeof b->c);
    memcpy(kb + 6, b->n, sizeof b->n);
    memcpy(kb + 9, b->uv, sizeof b->uv);
    for (i = 0; i < 11; i++)
        if (ka[i] != kb[i])
            return ka[i] < kb[i];
    return 1;
}

/*
 * The point where the edge a b (fa and fb its sides of plane k, of
 * opposite signs, neither 0) meets plane k, into o. From the edge's ends in
 * one order whichever way the face goes round, so that two faces sharing
 * the edge get the same point, bit for bit.
 *
 * The position is the plane's intersection in view space. The attributes
 * (colour, normal, u, v) at the near and far planes are interpolated in
 * view space too (the vertex there is a real point of the surface, as one
 * of the model's own would be); at the guard band's planes, which stand
 * for cutting in screen space, they are interpolated by the fraction of
 * the way along the edge on the screen, as the console's affine
 * interpolation and the frame's splitter would have them, so the guard
 * band does not change what the face shows. (The near plane is first:
 * both ends are in front of the camera by then.)
 */
static void cut_point(const jbi_geom_draw *d, double pl[PLANES][4], int k, const cvert *a,
                      double fa, const cvert *b, double fb, cvert *o)
{
    const cvert *e0 = before(a, b) ? a : b, *e1 = e0 == a ? b : a;
    double f0 = e0 == a ? fa : fb, f1 = e0 == a ? fb : fa;
    double t = f0 / (f0 - f1), s = t;
    int c;

    for (c = 0; c < 3; c++)
        o->p[c] = e0->p[c] + (e1->p[c] - e0->p[c]) * t;
    /* A point between two on one plane is on it. */
    for (c = 0; c < 2; c++)
        o->on[c] = e0->on[c] == e1->on[c] ? e0->on[c] : -1;
    if (k < 2) {
        o->p[2] = -pl[k][3] / pl[k][2]; /* on the plane exactly */
    } else {
        int axis = k < 4 ? 0 : 1;
        double h = focal(d), half = axis == 0 ? d->width * 0.5 : d->height * 0.5;
        double target = k == 2 || k == 4 ? (double)JBI_GEOM_GUARD_MIN : guard_hi();
        double s0 = half + h * e0->p[axis] / e0->p[2], s1 = half + h * e1->p[axis] / e1->p[2];

        /* The constrained coordinate from the plane itself, not from the
           interpolation, which cancels catastrophically between ends far
           apart (a line from x -1e20 to 1e20 would land at 0). */
        o->p[axis] = (target - half) * o->p[2] / h;
        o->on[axis] = k;
        s = (target - s0) / (s1 - s0);
        s = s < 0.0 ? 0.0 : s > 1.0 ? 1.0 : s == s ? s : t;
    }
    for (c = 0; c < 3; c++) {
        o->c[c] = e0->c[c] + (e1->c[c] - e0->c[c]) * s;
        o->n[c] = e0->n[c] + (e1->n[c] - e0->n[c]) * s;
        o->pc[c] = e0->pc[c] + (e1->pc[c] - e0->pc[c]) * t;
        o->pn[c] = e0->pn[c] + (e1->pn[c] - e0->pn[c]) * t;
    }
    for (c = 0; c < 2; c++) {
        o->uv[c] = e0->uv[c] + (e1->uv[c] - e0->uv[c]) * s;
        o->puv[c] = e0->puv[c] + (e1->puv[c] - e0->puv[c]) * t;
    }
}

/* Sutherland-Hodgman against each plane; returns the vertices left (in
   v, which holds JBI_GEOM_POLY_MAX), and sets *cut if any plane cut. A
   vertex on a plane is kept once: an edge from it gets no second point. */
static int clip(const jbi_geom_draw *d, cvert *v, int n, double pl[PLANES][4], int *cut)
{
    cvert tmp[JBI_GEOM_POLY_MAX];
    int k, i, m;

    for (k = 0; k < PLANES && n > 0; k++) {
        m = 0;
        for (i = 0; i < n; i++) {
            const cvert *a = &v[i], *b = &v[(i + 1) % n];
            double fa = side(pl[k], a->p), fb = side(pl[k], b->p);

            /* A triangle gains at most one vertex a plane, so 3 + 6 fit;
               the bound only guards against what cannot happen. */
            /* A face's own vertex keeps its projection even on a guard
               plane (on is set only at a cut): an exact neighbour sharing
               it puts it where the GTE does, and the shared edge must
               be the same on both sides. */
            if (fa >= 0.0 && m < JBI_GEOM_POLY_MAX)
                tmp[m++] = *a;
            else if (fa < 0.0)
                *cut = 1;
            if (((fa > 0.0 && fb < 0.0) || (fa < 0.0 && fb > 0.0)) && m < JBI_GEOM_POLY_MAX) {
                cut_point(d, pl, k, a, fa, b, fb, &tmp[m++]);
                *cut = 1;
            }
        }
        memcpy(v, tmp, sizeof(cvert) * (size_t)m);
        n = m;
    }
    return n;
}

static int inside(double pl[PLANES][4], const double p[3])
{
    int k;

    for (k = 0; k < PLANES; k++)
        if (!(side(pl[k], p) >= 0.0))
            return 0;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Projection */

/*
 * A vertex at x, y, z (GTE units x 4096) the way RTPS projects it: IR1
 * and IR2 the sums >> 12 saturated to -32768..32767 as the GTE saturates
 * them (lm off), SZ the depth sum >> 12, the GTE's divide, SX = (IR1 q +
 * OFX) >> 16 (the guard band keeps it within range, but for the rounding
 * of a vertex very near the camera, which is saturated as SX is).
 */
static void project_gte(const jbi_geom_draw *d, double x, double y, double z,
                        jbi_geom_vertex *v)
{
    s64 sz = clamp64(floor_d(z / 4096.0), d->r.h / 2 + 1, d->far_z);
    s64 ix = floor_d(x / 4096.0), iy = floor_d(y / 4096.0);
    int overflow, saturated = ix < -32768 || ix > 32767 || iy < -32768 || iy > 32767;
    s64 q = jbi_gte_divide((uint32_t)d->r.h, (uint32_t)sz, &overflow);
    s64 X, Y, ownx, owny;
    /* The same expressions, in the same order, as the GTE's own record of
       a projection for precise vertices (src/geom/gte.c, record_precise),
       so a face drawn here and the same vertices projected by console code
       get the same sub-pixel positions. */
    double k = (double)d->r.h * 4096.0 * 256.0 / (z * 4096.0);
    double bx = (double)d->r.ofx / 256.0 + x * k, by = (double)d->r.ofy / 256.0 + y * k;

    ix = clamp64(ix, -32768, 32767);
    iy = clamp64(iy, -32768, 32767);
    X = ix * q + d->r.ofx;
    Y = iy * q + d->r.ofy;
    if (X >> 16 < -1024 || X >> 16 > 1023 || Y >> 16 < -1024 || Y >> 16 > 1023) {
        X = clamp64(X, (s64)-1024 * 65536, (s64)1024 * 65536 - 1);
        Y = clamp64(Y, (s64)-1024 * 65536, (s64)1024 * 65536 - 1);
        bx = (double)clamp64(round_up(bx), -1024 * 256, 1024 * 256 - 1);
        by = (double)clamp64(round_up(by), -1024 * 256, 1024 * 256 - 1);
    }
    ownx = X >> 8;
    owny = Y >> 8;
    v->x = (int)(X >> 16);
    v->y = (int)(Y >> 16);
    v->z = (int32_t)sz;
    /* The exact sums' projection, unless it strays a pixel from the GTE's
       or the GTE saturated IR (as its record does then: the hardware's
       position). */
    v->sx = (int32_t)ownx;
    v->sy = (int32_t)owny;
    v->zf = (double)sz;
    if (!saturated && q < 0x1ffff && bx >= (double)(ownx - 256) && bx <= (double)(ownx + 256) &&
        by >= (double)(owny - 256) && by <= (double)(owny + 256)) {
        v->sx = (int32_t)round_up(bx);
        v->sy = (int32_t)round_up(by);
    }
    /* The sub-pixel position is the GTE's record's, also its fraction
       past 1023 where the whole-pixel position is 1023 (the splitter takes
       a position up to its slack past the range: split.h), so an exact
       face draws the sub-pixel coverage console code draws there. */
}

/* The float path's projection of x, y, z (GTE units, inside the planes). */
static void project_float(const jbi_geom_draw *d, double x, double y, double z,
                          jbi_geom_vertex *v)
{
    double k = (double)d->fh / z;
    s64 sx = round_d(((double)d->fofx + x * k) * 256.0);
    s64 sy = round_d(((double)d->fofy + y * k) * 256.0);
    s64 iz = round_d(z);

    sx = clamp64(sx, -1024 * 256, 1023 * 256); /* as project_gte's */
    sy = clamp64(sy, -1024 * 256, 1023 * 256);
    v->sx = (int32_t)sx;
    v->sy = (int32_t)sy;
    v->x = (int)(sx >> 8);
    v->y = (int)(sy >> 8);
    v->z = (int32_t)clamp64(iz, 1, 0x7fffffff);
    v->zf = z;
}

/* The depth cue IR0 for a depth sz, as RTPS makes it. */
static int32_t gte_ir0(const jbi_geom_draw *d, s64 sz)
{
    int overflow;
    s64 q = jbi_gte_divide((uint32_t)d->r.h, (uint32_t)sz, &overflow);

    return (int32_t)clamp64(((s64)d->r.dqa * q + d->r.dqb) >> 12, 0, 4096);
}

/* The float path's fog amount, 0..1, at depth z (GTE units, > 0). */
static double fog_amount(const jbi_geom_draw *d, double z)
{
    double zn = d->fog_near, zf = d->fog_far;
    double f = zf * (z - zn) / (z * (zf - zn));

    return f < 0.0 ? 0.0 : f > 1.0 ? 1.0 : f == f ? f : 1.0;
}

static uint32_t pack(const double c[3])
{
    uint32_t out = 0;
    int i;

    for (i = 0; i < 3; i++)
        out |= (uint32_t)clamp64(round_d(c[i]), 0, 255) << (8 * i);
    return out;
}

static void unpack(uint32_t c, double out[3])
{
    out[0] = (double)(c & 0xffu);
    out[1] = (double)((c >> 8) & 0xffu);
    out[2] = (double)((c >> 16) & 0xffu);
}

/* OTZ (12 k x average depth / 4096: jbi_geom_begin) mapped linearly onto
   the buckets between the near and far planes' OTZ. Monotonic, so a
   deeper average never sorts nearer, whatever the vertex count. */
static int bucket(const jbi_geom_draw *d, s64 otz)
{
    s64 range = (s64)d->otz_far - d->otz_near + 1, b;

    /* The GTE path's depth bias: added to OTZ on its scale, rounded, as
       console code would add an offset to OTZ. (The float path's is in
       its depths already: order_depth.) */
    if (d->bias != 0.0 && d->mode == JB_GEOMETRY_GTE)
        otz = otz + round_d(d->bias * (double)(3 * d->r.zsf3) / 4096.0);
    otz = clamp64(otz, -0x40000000, 0x40000000);
    b = (otz - d->otz_near) * d->ot_size;

    if (b <= 0)
        return 0;
    return (int)clamp64(b / range, 0, d->ot_size - 1);
}

/* The OTZ of n depths summing to sum, as AVSZ3 and AVSZ4 make it:
   floor(sum x ZSF3 x 3 / n / 4096), also for a negative sum (the float
   path's biased depths). */
static s64 otz_of(const jbi_geom_draw *d, s64 sum, int n)
{
    s64 p = sum * 3 * d->r.zsf3, q = (s64)n * 4096, r = p / q;

    return r * q > p ? r - 1 : r;
}

/* The depth a vertex is ordered by, in both paths: its integer depth in
   GTE units, rounded down (the GTE path's SZ; the float path's unrounded
   depth rounded down the same way), so that the two paths' keys mean the
   same and an in-frame switch keeps the order. The float path adds the
   draw's depth bias (GTE units) to the depth before rounding it down, so
   that a bias of any size orders the draw as the same draw that much
   nearer or farther would be (a bias rounded on its own would be lost
   below a step of OTZ). */
static s64 order_depth(const jbi_geom_draw *d, const jbi_geom_vertex *v)
{
    if (d->mode == JB_GEOMETRY_GTE)
        return v->z;
    /* Negative where a bias brings it nearer than the eye: kept, so the
       average is the biased depths' (otz_of rounds it down, bucket()
       limits the result). */
    return clamp64(floor_d(v->zf + d->bias), -0x40000000, 0x40000000);
}

/* ------------------------------------------------------------------------ */
/* The GTE's commands on the private GTE */

static void put_v(jbi_gte *g, int n, int x, int y, int z)
{
    jbi_gte_write_data(g, JBI_GTE_VXY0 + 2 * n, JB_GTE_PAIR(x, y));
    jbi_gte_write_data(g, JBI_GTE_VZ0 + 2 * n, (uint32_t)z);
}

static uint32_t gte_lit(jbi_gte *g, const jb_gte_vec *nrm, uint32_t color, int fog, int32_t ir0)
{
    jbi_gte_write_data(g, JBI_GTE_RGBC, color);
    put_v(g, 0, nrm->x, nrm->y, nrm->z);
    if (fog) {
        jbi_gte_write_data(g, JBI_GTE_IR0, (uint32_t)ir0);
        jbi_gte_command(g, JBI_GTE_CMD_NCDS);
    } else {
        jbi_gte_command(g, JBI_GTE_CMD_NCCS);
    }
    return jbi_gte_read_data(g, JBI_GTE_RGB2) & 0xffffffu;
}

static uint32_t gte_fogged(jbi_gte *g, uint32_t color, int32_t ir0)
{
    jbi_gte_write_data(g, JBI_GTE_RGBC, color);
    jbi_gte_write_data(g, JBI_GTE_IR0, (uint32_t)ir0);
    jbi_gte_command(g, JBI_GTE_CMD_DPCS);
    return jbi_gte_read_data(g, JBI_GTE_RGB2) & 0xffffffu;
}

/* FLAG bits that make a projection unusable as it is. */
#define SATURATED                                                                                \
    (JBI_GTE_FLAG_MAC_POS(1) | JBI_GTE_FLAG_MAC_POS(2) | JBI_GTE_FLAG_MAC_POS(3) |               \
     JBI_GTE_FLAG_MAC_NEG(1) | JBI_GTE_FLAG_MAC_NEG(2) | JBI_GTE_FLAG_MAC_NEG(3) |               \
     JBI_GTE_FLAG_IR(1) | JBI_GTE_FLAG_IR(2) | JBI_GTE_FLAG_SZ_OTZ | JBI_GTE_FLAG_DIVIDE |       \
     JBI_GTE_FLAG_MAC0_POS | JBI_GTE_FLAG_MAC0_NEG | JBI_GTE_FLAG_SX | JBI_GTE_FLAG_SY)

/* ------------------------------------------------------------------------ */
/* Faces */

/* What a face's vertex brings, in the Z order. */
typedef struct fvert {
    int idx;
    int p[3];          /* model position */
    jb_gte_vec normal; /* 4.12 */
    uint32_t color;    /* before lighting */
    int u, v;
} fvert;

/* The face's flat normal, 4.12, from its model positions (cyclic). */
static jb_gte_vec flat_normal(const jb_mesh *m, const jb_mesh_face *f)
{
    const jb_gte_vec *p = m->positions;
    s64 a[3], b[3], c[3];
    double len;
    jb_gte_vec out = {0, 0, 0};
    int i0 = f->v[0], i1 = f->v[1], i2 = f->v[2], i3 = f->vertices == 4 ? f->v[3] : f->v[0];
    int ea = f->vertices == 4 ? i2 : i1, sa = i0, eb = f->vertices == 4 ? i3 : i2;
    int sb = f->vertices == 4 ? i1 : i0;

    /* The edges (a quad's diagonals) and their cross product in integers,
       exactly (differences of int16 within 17 bits, products within 34,
       their differences within 35): a valid face's direction however
       thin, which a float product would round away. */
    a[0] = (s64)p[ea].x - p[sa].x;
    a[1] = (s64)p[ea].y - p[sa].y;
    a[2] = (s64)p[ea].z - p[sa].z;
    b[0] = (s64)p[eb].x - p[sb].x;
    b[1] = (s64)p[eb].y - p[sb].y;
    b[2] = (s64)p[eb].z - p[sb].z;
    c[0] = a[1] * b[2] - a[2] * b[1];
    c[1] = a[2] * b[0] - a[0] * b[2];
    c[2] = a[0] * b[1] - a[1] * b[0];
    len = (double)c[0] * (double)c[0] + (double)c[1] * (double)c[1];
    len = jbi_sqrt(len + (double)c[2] * (double)c[2]);
    if (len > 0.0) {
        out.x = (int16_t)round_d((double)c[0] / len * 4096.0);
        out.y = (int16_t)round_d((double)c[1] / len * 4096.0);
        out.z = (int16_t)round_d((double)c[2] / len * 4096.0);
    }
    return out;
}

/* colour x tint / 128 a channel, saturated (tint 0: colour; JB_TINT_BLACK,
   whose channels are 0: black). */
static uint32_t tinted(uint32_t color, uint32_t tint)
{
    uint32_t out = 0;
    int c;

    if (tint == 0)
        return color;
    for (c = 0; c < 3; c++) {
        uint32_t v = ((color >> (8 * c)) & 0xffu) * ((tint >> (8 * c)) & 0xffu) / 128u;

        out |= (v > 255u ? 255u : v) << (8 * c);
    }
    return out;
}

/* Gathers the face's vertices in the Z order; returns their count. */
static int gather(const jbi_geom_draw *d, const jb_mesh *m, const jb_mesh_face *f,
                  const jb_material *mat, fvert *v)
{
    static const int zorder[4] = {0, 1, 3, 2};
    int n = f->vertices == 4 ? 4 : 3, k, normal_done = 0; /* checked: 3 or 4 */
    uint32_t base = mat->color == 0 ? 0x808080u : mat->color & 0xffffffu; /* JB_TINT_BLACK: 0 */
    jb_gte_vec flat = {0, 0, 0};

    for (k = 0; k < n; k++) {
        int i = f->v[n == 4 ? zorder[k] : k];
        fvert *o = &v[k];

        o->idx = i;
        o->p[0] = m->positions[i].x;
        o->p[1] = m->positions[i].y;
        o->p[2] = m->positions[i].z;
        if (m->normals != NULL) {
            o->normal = m->normals[i];
        } else {
            if (!normal_done) {
                flat = flat_normal(m, f);
                normal_done = 1;
            }
            o->normal = flat;
        }
        o->color = tinted(m->colors != NULL ? m->colors[i] : base, d->tint);
        o->u = m->uv != NULL ? m->uv[2 * i] : 0;
        o->v = m->uv != NULL ? m->uv[2 * i + 1] : 0;
    }
    return n;
}

/* The Z-order index of each cyclic position. */
static int zpos(int n, int i)
{
    static const int cyc[4] = {0, 1, 3, 2};

    return n == 4 ? cyc[i & 3] : i;
}

/* The float path's lighting, saturating as NCCS does, into out: each
   channel the colour times the light, unrounded and not yet saturated,
   so that fog moves it as NCDS does and the byte is rounded once, at the
   end (pack). */
static void float_lit(const jbi_geom_draw *d, const double nrm[3], const double color[3],
                      double out[3])
{
    float n[3], dots[JBI_LIGHTS];
    int c, k;

    n[0] = (float)nrm[0] / 4096.0f;
    n[1] = (float)nrm[1] / 4096.0f;
    n[2] = (float)nrm[2] / 4096.0f;
    for (k = 0; k < JBI_LIGHTS; k++) {
        float t = d->lto[k][0] * n[0];

        t = t + d->lto[k][1] * n[1];
        t = t + d->lto[k][2] * n[2];
        dots[k] = t < 0.0f ? 0.0f : t > 7.9997f ? 7.9997f : t;
    }
    for (c = 0; c < 3; c++) {
        float l = d->amb[c];

        for (k = 0; k < JBI_LIGHTS; k++)
            l = l + d->lcol[k][c] * dots[k];
        l = l < 0.0f ? 0.0f : l > 7.9997f ? 7.9997f : l;
        out[c] = (double)((float)color[c] * l);
    }
}

/* One attribute set's colour at vertex v (projected already): lit and
   fogged on the GTE (rounded to bytes first, as RGBC takes them) or in
   float (fractional until the end, rounded once). */
static uint32_t shade(jbi_gte *g, const jbi_geom_draw *d, const double col[3],
                      const double nrm[3], double depth, const jbi_geom_vertex *v, int lit)
{
    uint32_t color = pack(col);
    double c[3];
    int k;

    if (d->mode == JB_GEOMETRY_GTE) {
        jb_gte_vec n;

        n.x = (int16_t)clamp64(round_d(nrm[0]), -32768, 32767);
        n.y = (int16_t)clamp64(round_d(nrm[1]), -32768, 32767);
        n.z = (int16_t)clamp64(round_d(nrm[2]), -32768, 32767);
        if (lit)
            return gte_lit(g, &n, color, d->fog, d->fog ? gte_ir0(d, v->z) : 0);
        if (d->fog)
            return gte_fogged(g, color, gte_ir0(d, v->z));
        return color;
    }
    if (lit)
        float_lit(d, nrm, col, c);
    else
        memcpy(c, col, sizeof c);
    if (d->fog) {
        double f = fog_amount(d, depth);

        for (k = 0; k < 3; k++)
            c[k] = c[k] + ((double)d->fog_color[k] - c[k]) * f;
    }
    return pack(c);
}

/*
 * A projected vertex that lies on guard plane k (2..5) put on it on the
 * screen exactly: whole pixel and sub-pixel position alike at the plane's
 * coordinate, -1024 or 1023. Projected, it can land a fraction short (the
 * GTE path truncates 1022.99 to 1022), and a pixel short there the cut
 * face would stop a pixel short of the frame's right or bottom edge in a
 * frame 1024 wide or high. Clipped geometry only: the plane is the
 * pipeline's, not the console's.
 */
static void on_plane(jbi_geom_vertex *v, int k)
{
    int c = k == 2 || k == 4 ? JBI_GEOM_GUARD_MIN : JBI_GEOM_GUARD_MAX;

    if (k == 2 || k == 3) {
        v->x = c;
        v->sx = c * 256;
    } else if (k == 4 || k == 5) {
        v->y = c;
        v->sy = c * 256;
    }
}

/* A polygon's vertices, lit, projected and fogged, into out: both
   attribute sets (cvert), the perspective-correct one shaded again only
   where it differs. */
static void finish(jbi_gte *g, const jbi_geom_draw *d, cvert *cv, int n, int lit,
                   jbi_geom_poly *out)
{
    s64 sum = 0;
    int i;

    out->n = n;
    for (i = 0; i < n; i++) {
        jbi_geom_vertex *v = &out->v[i];
        const cvert *q = &cv[i];

        if (d->mode == JB_GEOMETRY_GTE)
            project_gte(d, q->p[0], q->p[1], q->p[2], v);
        else
            project_float(d, q->p[0], q->p[1], q->p[2], v);
        on_plane(v, q->on[0]);
        on_plane(v, q->on[1]);
        v->color = shade(g, d, q->c, q->n, q->p[2], v, lit);
        if (memcmp(q->c, q->pc, sizeof q->c) == 0 && memcmp(q->n, q->pn, sizeof q->n) == 0)
            v->pcolor = v->color;
        else
            v->pcolor = shade(g, d, q->pc, q->pn, q->p[2], v, lit);
        v->u = (int)clamp64(round_d(q->uv[0]), 0, 255);
        v->v = (int)clamp64(round_d(q->uv[1]), 0, 255);
        v->pu = (int)clamp64(round_d(q->puv[0]), 0, 255);
        v->pv = (int)clamp64(round_d(q->puv[1]), 0, 255);
        sum += order_depth(d, v);
    }
    out->bucket = bucket(d, otz_of(d, sum, n));
}

/* The triple product of three points: the sign NCLIP gives in front of
   the camera (front faces negative), and right behind it too. */
static double triple(const double a[3], const double b[3], const double c[3])
{
    double x = b[1] * c[2] - b[2] * c[1];
    double y = b[2] * c[0] - b[0] * c[2];
    double z = b[0] * c[1] - b[1] * c[0];

    return a[0] * x + a[1] * y + a[2] * z;
}

static void to_cvert(cvert *o, const double p[3], const fvert *f)
{
    memcpy(o->p, p, sizeof o->p);
    unpack(f->color, o->c);
    o->n[0] = (double)f->normal.x;
    o->n[1] = (double)f->normal.y;
    o->n[2] = (double)f->normal.z;
    o->uv[0] = (double)f->u;
    o->uv[1] = (double)f->v;
    memcpy(o->pc, o->c, sizeof o->pc);
    memcpy(o->pn, o->n, sizeof o->pn);
    memcpy(o->puv, o->uv, sizeof o->puv);
    o->on[0] = o->on[1] = -1;
}

/*
 * The clipped path for both: the face as its console triangles (a quad's
 * two, cyclic 0 1 3 and 1 2 3, so its diagonal stays where the GPU puts
 * it), each culled by its own triple product, clipped and finished into
 * out. p are the vertices' view positions in the Z order. Returns the
 * polygons made (0..2).
 */
static int clipped(jbi_gte *g, const jbi_geom_draw *d, const jb_material *mat, const fvert *fv,
                   double p[4][3], int n, double unit, jbi_geom_poly out[2])
{
    static const int tris[2][3] = {{0, 1, 2}, {1, 3, 2}}; /* Z order */
    double pl[PLANES][4];
    int t, count = 0;

    planes(d, unit, pl);
    for (t = 0; t < (n == 4 ? 2 : 1); t++) {
        cvert cv[JBI_GEOM_POLY_MAX];
        int i, m, cut = 0;

        if (!mat->double_sided &&
            !(triple(p[tris[t][0]], p[tris[t][1]], p[tris[t][2]]) < 0.0))
            continue;
        for (i = 0; i < 3; i++)
            to_cvert(&cv[i], p[tris[t][i]], &fv[tris[t][i]]);
        m = clip(d, cv, 3, pl, &cut);
        if (m < 3)
            continue;
        out[count].material = mat;
        out[count].exact = 0;
        out[count].passes = 0;
        finish(g, d, cv, m, !mat->unlit, &out[count]);
        count++;
    }
    return count;
}

/* A vertex where the precise scaled pass draws it, in 1/JBI_GEOM_SUBPIXEL
   pixels: its sub-pixel position, unless that is more than 1 1/8 pixels
   from its whole-pixel one in either coordinate, where the decoder ignores
   it and draws the whole-pixel one (src/core/gp0.c, PRECISE_SLACK). */
static void precise_at(const jbi_geom_vertex *v, s64 *x, s64 *y)
{
    const s64 one = JBI_GEOM_SUBPIXEL, slack = JBI_GEOM_SUBPIXEL + JBI_GEOM_SUBPIXEL / 8;
    s64 dx = (s64)v->sx - (s64)v->x * one, dy = (s64)v->sy - (s64)v->y * one;

    if (dx > slack || dx < -slack || dy > slack || dy < -slack)
        *x = (s64)v->x * one, *y = (s64)v->y * one;
    else
        *x = v->sx, *y = v->sy;
}

/* The same rule on the positions the precise scaled pass draws a polygon
   at (precise_at): whether it faces away there. */
static int faces_away_precise(const jbi_geom_vertex *v, int n)
{
    s64 x[4], y[4], w;
    int k;

    for (k = 0; k < n; k++)
        precise_at(&v[k], &x[k], &y[k]);
    w = (x[1] - x[0]) * (y[n == 4 ? 3 : 2] - y[0]) - (x[n == 4 ? 3 : 2] - x[0]) * (y[1] - y[0]);
    if (w == 0 && n == 4)
        w = (x[2] - x[1]) * (y[3] - y[1]) - (x[3] - x[1]) * (y[2] - y[1]);
    return w >= 0;
}

/*
 * The outputs a whole face (its vertices round it, positioned) is drawn
 * by, as jbi_geom_poly.passes, or -1 for none: both (0) for a double-sided
 * face; else the 1x pass (and the scaled one without precise vertices)
 * unless away_1x (its whole-pixel corners face away), and the precise
 * scaled pass unless its sub-pixel corners face away. The two can differ
 * for a face seen almost edge on, and each output must judge by what it
 * rasterises, or it drops a face whose front it shows (a gap) or draws one
 * whose back it shows.
 */
static int facing(const jbi_geom_vertex *v, int n, int away_1x, int double_sided)
{
    int p;

    if (double_sided)
        return 0;
    p = (away_1x ? 0 : JBI_GEOM_PASS_1X) | (faces_away_precise(v, n) ? 0 : JBI_GEOM_PASS_PRECISE);
    return p == 0 ? -1 : p == (JBI_GEOM_PASS_1X | JBI_GEOM_PASS_PRECISE) ? 0 : p;
}

static int face_gte(jbi_gte *g, const jbi_geom_draw *d, const jb_mesh *m, int fi,
                    jbi_geom_poly out[2])
{
    const jb_mesh_face *f = &m->faces[fi];
    const jb_material *mat = &m->materials[f->material];
    fvert fv[4];
    int n = gather(d, m, f, mat, fv), k, sh = d->shift, exact = 1, lit = !mat->unlit;
    uint32_t sxy[4], flags = 0;
    int32_t sz[4], ir0[4];
    s64 sums[4][3];
    double p[4][3];
    int32_t mac0;

    /* Project, as console code would. */
    if (d->fog) {
        for (k = 0; k < 3; k++) {
            put_v(g, 0, fv[k].p[0] * (1 << sh), fv[k].p[1] * (1 << sh), fv[k].p[2] * (1 << sh));
            jbi_gte_command(g, JBI_GTE_CMD_RTPS);
            flags |= jbi_gte_read_ctrl(g, JBI_GTE_FLAG);
            sxy[k] = jbi_gte_read_data(g, JBI_GTE_SXY2);
            sz[k] = (int32_t)jbi_gte_read_data(g, JBI_GTE_SZ3);
            ir0[k] = (int32_t)jbi_gte_read_data(g, JBI_GTE_IR0);
        }
    } else {
        for (k = 0; k < 3; k++)
            put_v(g, k, fv[k].p[0] * (1 << sh), fv[k].p[1] * (1 << sh), fv[k].p[2] * (1 << sh));
        jbi_gte_command(g, JBI_GTE_CMD_RTPT);
        flags |= jbi_gte_read_ctrl(g, JBI_GTE_FLAG);
        for (k = 0; k < 3; k++) {
            sxy[k] = jbi_gte_read_data(g, JBI_GTE_SXY0 + k);
            sz[k] = (int32_t)jbi_gte_read_data(g, JBI_GTE_SZ1 + k);
            ir0[k] = 0;
        }
    }
    jbi_gte_command(g, JBI_GTE_CMD_NCLIP);
    mac0 = (int32_t)jbi_gte_read_data(g, JBI_GTE_MAC0);
    if (n == 4) {
        put_v(g, 0, fv[3].p[0] * (1 << sh), fv[3].p[1] * (1 << sh), fv[3].p[2] * (1 << sh));
        jbi_gte_command(g, JBI_GTE_CMD_RTPS);
        flags |= jbi_gte_read_ctrl(g, JBI_GTE_FLAG);
        sxy[3] = jbi_gte_read_data(g, JBI_GTE_SXY2);
        sz[3] = (int32_t)jbi_gte_read_data(g, JBI_GTE_SZ3);
        ir0[3] = (int32_t)jbi_gte_read_data(g, JBI_GTE_IR0);
    }
    jbi_gte_command(g, n == 4 ? JBI_GTE_CMD_AVSZ4 : JBI_GTE_CMD_AVSZ3);

    /* The GTE's sums, exactly (TR x 4096 + RT x V, which the set-up keeps
       from overflowing 44 bits). */
    for (k = 0; k < n; k++) {
        int r;

        for (r = 0; r < 3; r++) {
            sums[k][r] = (s64)d->r.tr[r] * 4096 +
                         (s64)d->r.rt.m[r][0] * (fv[k].p[0] * (1 << sh)) +
                         (s64)d->r.rt.m[r][1] * (fv[k].p[1] * (1 << sh)) +
                         (s64)d->r.rt.m[r][2] * (fv[k].p[2] * (1 << sh));
            p[k][r] = (double)sums[k][r];
        }
    }
    if (flags & SATURATED)
        exact = 0;
    /* Exact only inside every clipping plane, tested on the full-precision
       sums as the clipped path tests them (not on the truncated SZ), so a
       face just beyond the far plane is not drawn, and two faces sharing an
       edge that crosses a plane are both cut there (never one cut and its
       neighbour exact: a T-junction). */
    if (exact) {
        double pl[PLANES][4];

        planes(d, 4096.0, pl);
        for (k = 0; k < n && exact; k++)
            exact = inside(pl, p[k]);
    }
    if (!exact)
        return clipped(g, d, mat, fv, p, n, 4096.0, out);

    /* Culled when NCLIP of the first three says it faces away; a quad whose
       first triangle has no area is judged by its second (cyclic 1 2 3).
       That is the 1x pass's decision; the precise scaled pass's is the same
       rule on the sub-pixel positions it draws (facing). */
    if (mac0 == 0 && n == 4)
        mac0 = jb_gte_nclip(sxy[1], sxy[3], sxy[2], NULL);
    for (k = 0; k < n; k++) {
        jbi_geom_vertex *v = &out[0].v[n == 4 ? (k == 2 ? 3 : k == 3 ? 2 : k) : k];

        project_gte(d, p[k][0], p[k][1], p[k][2], v);
        v->x = JB_GTE_SX(sxy[k]);
        v->y = JB_GTE_SY(sxy[k]);
        v->z = sz[k];
        v->zf = (double)sz[k];
    }
    if ((out[0].passes = facing(out[0].v, n, mac0 >= 0, mat->double_sided)) < 0)
        return 0;
    out[0].material = mat;
    out[0].n = n;
    out[0].exact = 1;
    out[0].bucket = bucket(d, jbi_gte_read_data(g, JBI_GTE_OTZ));
    for (k = 0; k < n; k++) {
        jbi_geom_vertex *v = &out[0].v[n == 4 ? (k == 2 ? 3 : k == 3 ? 2 : k) : k];
        uint32_t c = fv[k].color;

        if (lit)
            c = gte_lit(g, &fv[k].normal, c, d->fog, ir0[k]);
        else if (d->fog)
            c = gte_fogged(g, c, ir0[k]);
        v->color = v->pcolor = c;
        v->u = v->pu = fv[k].u;
        v->v = v->pv = fv[k].v;
    }
    return 1;
}

/* Model to GTE units in float, for the float path and for points; 0 when
   a coordinate is not finite (a model matrix too large for the point). */
static int float_view(const jbi_geom_draw *d, float x, float y, float z, double out[3])
{
    int r;

    for (r = 0; r < 3; r++) {
        float s = d->g[r][0] * x;

        s = s + d->g[r][1] * y;
        s = s + d->g[r][2] * z;
        s = s + d->g[r][3];
        out[r] = (double)s;
        if (!finite_d(out[r]))
            return 0;
    }
    return 1;
}

/* A polygon's vertex as the GTE's packed SXY. */
static uint32_t packed_sxy(const jbi_geom_vertex *v)
{
    return (uint32_t)(uint16_t)v->x | (uint32_t)(uint16_t)v->y << 16;
}

/* Whether a polygon (its vertices round the face, at whole pixels) faces
   away as the GTE path judges it: NCLIP of the GPU's first triangle
   (v[0] v[1] v[3] of a quad) not below 0, or for a quad whose first
   triangle has no area, of its second (v[1] v[2] v[3]). */
static int faces_away(const jbi_geom_vertex *v, int n)
{
    int32_t mac0 = jb_gte_nclip(packed_sxy(&v[0]), packed_sxy(&v[1]), packed_sxy(&v[n == 4 ? 3 : 2]), NULL);

    if (mac0 == 0 && n == 4)
        mac0 = jb_gte_nclip(packed_sxy(&v[1]), packed_sxy(&v[2]), packed_sxy(&v[3]), NULL);
    return mac0 >= 0;
}

static int face_float(const jbi_geom_draw *d, const jb_mesh *m, int fi, jbi_geom_poly out[2])
{
    const jb_mesh_face *f = &m->faces[fi];
    const jb_material *mat = &m->materials[f->material];
    fvert fv[4];
    int n = gather(d, m, f, mat, fv), i, all = 1;
    double p[4][3], pl[PLANES][4];
    cvert cv[4];

    for (i = 0; i < n; i++)
        if (!float_view(d, (float)fv[i].p[0], (float)fv[i].p[1], (float)fv[i].p[2], p[i]))
            return 0;
    planes(d, 1.0, pl);
    for (i = 0; i < n; i++)
        all &= inside(pl, p[i]);
    if (!all)
        return clipped(NULL, d, mat, fv, p, n, 1.0, out);
    /* Wholly inside: the face itself, culled as the GTE path culls, by
       NCLIP's sign for its corners as rounded to the screen (as the GPU
       draws them), not by its unrounded orientation: a face seen almost
       edge on can be turned away by a hair while its rounded corners face
       the eye, and dropping it left a pixel's gap on a silhouette. */
    for (i = 0; i < n; i++)
        to_cvert(&cv[i], p[zpos(n, i)], &fv[zpos(n, i)]);
    out[0].material = mat;
    out[0].exact = 1;
    finish(NULL, d, cv, n, !mat->unlit, &out[0]);
    if ((out[0].passes = facing(out[0].v, n, faces_away(out[0].v, n), mat->double_sided)) < 0)
        return 0;
    return 1;
}

int jbi_geom_face(jbi_gte *g, const jbi_geom_draw *d, const jb_mesh *mesh, int face,
                  jbi_geom_poly out[2])
{
    if (d->mode == JB_GEOMETRY_GTE)
        return face_gte(g, d, mesh, face, out);
    return face_float(d, mesh, face, out);
}

int jbi_geom_poly_prims(const jbi_geom_poly *p, jbi_geom_prim *out)
{
    int count = 0, i = 1;

    if (p->n == 3) {
        out[0].vertices = 3;
        memcpy(out[0].v, p->v, sizeof(jbi_geom_vertex) * 3);
        return 1;
    }
    /* Quads (0, i, i+1, i+2) in Z order 0, i, i+2, i+1, then a triangle. */
    while (i + 2 < p->n) {
        jbi_geom_prim *q = &out[count++];

        q->vertices = 4;
        q->v[0] = p->v[0];
        q->v[1] = p->v[i];
        q->v[2] = p->v[i + 2];
        q->v[3] = p->v[i + 1];
        i += 2;
    }
    if (i + 1 < p->n) {
        jbi_geom_prim *t = &out[count++];

        t->vertices = 3;
        t->v[0] = p->v[0];
        t->v[1] = p->v[i];
        t->v[2] = p->v[i + 1];
    }
    return count;
}

/* ------------------------------------------------------------------------ */
/* Points and segments */

static void project_any(const jbi_geom_draw *d, const double p[3], jbi_geom_vertex *v)
{
    if (d->mode == JB_GEOMETRY_GTE)
        project_gte(d, p[0] * 4096.0, p[1] * 4096.0, p[2] * 4096.0, v);
    else
        project_float(d, p[0], p[1], p[2], v);
}

/* Model to GTE units in double, for points and segments, whose
   coordinates (floats, any size) can pass the float range once scaled;
   0 when a coordinate is not finite even so. */
static int view_d(const jbi_geom_draw *d, jb_vec3 p, double out[3])
{
    int r;

    for (r = 0; r < 3; r++) {
        double s = (double)d->g[r][0] * (double)p.x;

        s = s + (double)d->g[r][1] * (double)p.y;
        s = s + (double)d->g[r][2] * (double)p.z;
        s = s + (double)d->g[r][3];
        out[r] = s;
        if (!finite_d(s))
            return 0;
    }
    return 1;
}

int jbi_geom_point(const jbi_geom_draw *d, jb_vec3 p, jbi_geom_vertex *out)
{
    double v[3], pl[PLANES][4];

    if (!view_d(d, p, v))
        return 0;
    planes(d, 1.0, pl);
    if (!inside(pl, v))
        return 0;
    memset(out, 0, sizeof *out);
    project_any(d, v, out);
    return 1;
}

/*
 * A segment's line in view space, kept so that its visible part is as
 * precise as the view itself, wherever its ends are: a line from ends
 * 1e20 away that crosses the view is found to within rounding of the
 * view's own size, not of 1e20. (A parameter along the segment cannot do
 * that: where the ends are far apart and unequally so, every plane's
 * crossing rounds to one parameter, and a point evaluated from an end,
 * or from their middle, cancels huge against huge.)
 *
 * The line is its direction D and moment M = a x b (Pluecker coordinates),
 * made from the model-space ends, which are floats, so a x b is exact
 * products less one rounding, and taken to view space as lines transform:
 * D' = G D and M' = cof(G) M + t x D' for the view x' = G x + t (cof(G)
 * the cofactor matrix: (G a) x (G b) = cof(G) (a x b)). Each has a
 * rounding error relative to its own size, which moves the line by about
 * that error times its distance from the eye, so ends of any size and a
 * camera far from the origin keep it in place. Then p0 = D' x M' / |D'|^2
 * is its point nearest the eye, and the line is p0 + t u, u = D' / |D'|:
 * where it crosses the view t is small, and so precise.
 */
typedef struct line3 {
    double p0[3], u[3];
    double ea[3], eb[3]; /* the ends in view space, exact enough when in view */
    double ta, tb;       /* their t, ta < tb */
} line3;

static void cross_d(const double a[3], const double b[3], double out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static double dot_d(const double a[3], const double b[3])
{
    double s = a[0] * b[0];

    s = s + a[1] * b[1];
    return s + a[2] * b[2];
}

/* The line through model-space a and b (and their view-space ea, eb);
   0 when it has no direction there or is not finite. */
static int line_init(const jbi_geom_draw *d, jb_vec3 a, jb_vec3 b, const double ea[3],
                     const double eb[3], line3 *l)
{
    double A[3] = {(double)a.x, (double)a.y, (double)a.z};
    double B[3] = {(double)b.x, (double)b.y, (double)b.z};
    double dm[3], mm[3], gr[3][3], cof[3][3], dv[3], mv[3], tx[3], tr[3], q[3], dd, len;
    int r, c;

    for (c = 0; c < 3; c++)
        dm[c] = B[c] - A[c];
    cross_d(A, B, mm); /* float x float is exact in double: one rounding */
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++)
            gr[r][c] = (double)d->g[r][c];
        tr[r] = (double)d->g[r][3];
    }
    for (r = 0; r < 3; r++)
        cross_d(gr[(r + 1) % 3], gr[(r + 2) % 3], cof[r]);
    for (r = 0; r < 3; r++) {
        dv[r] = dot_d(gr[r], dm);
        mv[r] = dot_d(cof[r], mm);
    }
    cross_d(tr, dv, tx);
    for (r = 0; r < 3; r++)
        mv[r] = mv[r] + tx[r];
    dd = dot_d(dv, dv);
    if (!(dd > 0.0) || !finite_d(dd))
        return 0;
    cross_d(dv, mv, q);
    len = jbi_sqrt(dd);
    for (c = 0; c < 3; c++) {
        l->p0[c] = q[c] / dd;
        l->u[c] = dv[c] / len;
        l->ea[c] = ea[c];
        l->eb[c] = eb[c];
        if (!finite_d(l->p0[c]) || !finite_d(l->u[c]))
            return 0;
    }
    {
        /* A coordinate constant along a segment of ordinary size (a line
           at one depth, as on the near plane): the ends' own value, not
           the reconstruction's, which can round it off the plane it lies
           on. Only where the ends are not much farther than the line's
           nearest point: farther, their rounding is the larger, and two
           ends can be equal in a coordinate only by rounding. */
        double far = 0.0, near = 0.0;

        for (c = 0; c < 3; c++) {
            double fa = ea[c] < 0 ? -ea[c] : ea[c], fb = eb[c] < 0 ? -eb[c] : eb[c];
            double fp = l->p0[c] < 0 ? -l->p0[c] : l->p0[c];

            far = fa > far ? fa : far;
            far = fb > far ? fb : far;
            near = fp > near ? fp : near;
        }
        for (c = 0; c < 3; c++)
            if (ea[c] == eb[c] && far <= 1024.0 * (near + 1.0)) {
                l->p0[c] = ea[c];
                l->u[c] = 0.0;
            }
    }
    for (c = 0; c < 3; c++) {
        dm[c] = ea[c] - l->p0[c];
        q[c] = eb[c] - l->p0[c];
    }
    l->ta = dot_d(dm, l->u);
    l->tb = dot_d(q, l->u);
    return l->ta < l->tb;
}

/* Clips [*t0, *t1] to planes k0..k1-1; returns 0 when nothing is left.
   *which0 and *which1 get the plane that set each end (-1 for none). The
   plane's value along the line is side(p0) + t (n . u), both small where
   the line is in view. */
static int line_clip(const line3 *l, double pl[PLANES][4], int k0, int k1, double *t0, double *t1,
                     int *which0, int *which1)
{
    int k;

    for (k = k0; k < k1; k++) {
        double g = pl[k][0] * l->u[0], f = side(pl[k], l->p0), t;

        g = g + pl[k][1] * l->u[1];
        g = g + pl[k][2] * l->u[2];
        if (g == 0.0) {
            if (!(f >= 0.0))
                return 0; /* parallel and outside */
            continue;
        }
        t = -f / g;
        if (g > 0.0 && t > *t0) {
            *t0 = t;
            *which0 = k;
        } else if (g < 0.0 && t < *t1) {
            *t1 = t;
            *which1 = k;
        }
        if (!(*t0 <= *t1))
            return 0;
    }
    return 1;
}

/* The fraction of the way from end a to end b at t, 0..1. */
static double line_frac(const line3 *l, double t)
{
    double f = (t - l->ta) / (l->tb - l->ta);

    return f < 0.0 ? 0.0 : f > 1.0 ? 1.0 : f == f ? f : 0.0;
}

/* The point at t, an end itself where t is an end's and no plane cut
   there, else put exactly on plane k (if k >= 0) across it. */
static void line_point(const jbi_geom_draw *d, const line3 *l, double pl[PLANES][4], double t,
                       int k, double out[3])
{
    int c;

    if (k < 0 && t == l->ta) {
        memcpy(out, l->ea, sizeof l->ea);
        return;
    }
    if (k < 0 && t == l->tb) {
        memcpy(out, l->eb, sizeof l->eb);
        return;
    }
    for (c = 0; c < 3; c++)
        out[c] = l->p0[c] + l->u[c] * t;
    if (k == 0 || k == 1) {
        out[2] = -pl[k][3] / pl[k][2];
    } else if (k >= 2) {
        int axis = k < 4 ? 0 : 1;
        double half = axis == 0 ? d->width * 0.5 : d->height * 0.5;
        double target = k == 2 || k == 4 ? (double)JBI_GEOM_GUARD_MIN : guard_hi();

        out[axis] = (target - half) * out[2] / focal(d);
    }
}

int jbi_geom_segment(jbi_gte *g, const jbi_geom_draw *d, jb_vec3 a, jb_vec3 b, uint32_t ca,
                     uint32_t cb, jbi_geom_vertex out[2], int *bucket_out)
{
    double pa[3], pb[3], pl[PLANES][4], c0[3], c1[3];
    double n0, n1, s[2], pn[2][3], cn[2][3];
    int wn0 = -1, wn1 = -1, w[2] = {-1, -1}, i, c;
    line3 l;

    if (!view_d(d, a, pa) || !view_d(d, b, pb))
        return 0;
    unpack(ca, c0);
    unpack(cb, c1);
    planes(d, 1.0, pl);
    if (!line_init(d, a, b, pa, pb, &l)) {
        /* No direction (ends equal, or too close to tell apart): the point
           its ends are, if in view. */
        if (!inside(pl, pa))
            return 0;
        for (i = 0; i < 2; i++) {
            double zero[3] = {0.0, 0.0, 0.0};

            memset(&out[i], 0, sizeof out[i]);
            project_any(d, pa, &out[i]);
            out[i].color = out[i].pcolor = shade(g, d, i ? c1 : c0, zero, pa[2], &out[i], 0);
        }
        if (bucket_out != NULL)
            *bucket_out =
                bucket(d, otz_of(d, (s64)order_depth(d, &out[0]) + order_depth(d, &out[1]), 2));
        return 1;
    }
    n0 = l.ta;
    n1 = l.tb;
    /* The near and far planes first, in view space (colours there by the
       view-space fraction); then the guard band (colours by the fraction
       along the screen between the ends the near and far planes left). */
    if (!line_clip(&l, pl, 0, 2, &n0, &n1, &wn0, &wn1))
        return 0;
    s[0] = n0;
    s[1] = n1;
    w[0] = wn0;
    w[1] = wn1;
    if (!line_clip(&l, pl, 2, PLANES, &s[0], &s[1], &w[0], &w[1]))
        return 0;
    line_point(d, &l, pl, n0, wn0, pn[0]);
    line_point(d, &l, pl, n1, wn1, pn[1]);
    for (i = 0; i < 2; i++)
        for (c = 0; c < 3; c++)
            cn[i][c] = c0[c] + (c1[c] - c0[c]) * line_frac(&l, i ? n1 : n0);
    for (i = 0; i < 2; i++) {
        double p[3], col[3], pcol[3], zero[3] = {0.0, 0.0, 0.0}, t = line_frac(&l, s[i]);

        line_point(d, &l, pl, s[i], w[i], p);
        for (c = 0; c < 3; c++)
            pcol[c] = c0[c] + (c1[c] - c0[c]) * t;
        if (w[i] >= 2) {
            /* Along the screen, on the axis the ends are farthest apart. */
            double h = focal(d), f;
            double ax0 = h * pn[0][0] / pn[0][2], ax1 = h * pn[1][0] / pn[1][2];
            double ay0 = h * pn[0][1] / pn[0][2], ay1 = h * pn[1][1] / pn[1][2];
            int x = (ax1 - ax0 < 0 ? ax0 - ax1 : ax1 - ax0) >= (ay1 - ay0 < 0 ? ay0 - ay1 : ay1 - ay0);
            double sp = h * (x ? p[0] : p[1]) / p[2], s0v = x ? ax0 : ay0, s1v = x ? ax1 : ay1;

            f = s1v != s0v ? (sp - s0v) / (s1v - s0v) : 0.0;
            f = f < 0.0 ? 0.0 : f > 1.0 ? 1.0 : f == f ? f : 0.0;
            for (c = 0; c < 3; c++)
                col[c] = cn[0][c] + (cn[1][c] - cn[0][c]) * f;
        } else {
            memcpy(col, pcol, sizeof col);
        }
        memset(&out[i], 0, sizeof out[i]);
        project_any(d, p, &out[i]);
        on_plane(&out[i], w[i]);
        out[i].color = shade(g, d, col, zero, p[2], &out[i], 0);
        out[i].pcolor = memcmp(col, pcol, sizeof col) == 0
                            ? out[i].color
                            : shade(g, d, pcol, zero, p[2], &out[i], 0);
    }
    if (bucket_out != NULL)
        *bucket_out = bucket(d, otz_of(d, (s64)order_depth(d, &out[0]) + order_depth(d, &out[1]),
                                       2));
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Single depths, for billboards */

int jbi_geom_bucket(const jbi_geom_draw *d, const jbi_geom_vertex *v)
{
    return bucket(d, otz_of(d, order_depth(d, v), 1));
}

uint32_t jbi_geom_fog(jbi_gte *g, const jbi_geom_draw *d, uint32_t color,
                      const jbi_geom_vertex *v)
{
    double c[3], n[3] = {0.0, 0.0, 0.0};

    if (!d->fog)
        return color;
    unpack(color, c);
    return shade(g, d, c, n, v->zf, v, 0);
}

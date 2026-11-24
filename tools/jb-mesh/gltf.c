/*
 * jb-mesh's glTF 2.0 reader (mesh.h), through cgltf: .gltf (with buffers
 * in data URIs or files beside it) and .glb.
 *
 * What it takes: the default scene (or the first, or, with no scenes, every
 * node without a parent; with no nodes, every mesh as it is), each node's
 * meshes placed by its world transform (its matrix, or translation,
 * rotation and scale, in double precision); each primitive's POSITION,
 * NORMAL (turned by the transform's inverse transpose), TEXCOORD_0 and
 * COLOR_0 (red, green and blue; alpha is ignored), its triangles (mode
 * TRIANGLES, TRIANGLE_STRIP or TRIANGLE_FAN; points and lines are
 * refused), indexed or not. A transform that mirrors (negative
 * determinant) turns each triangle round, as glTF says. Materials: the
 * base colour factor, doubleSided, alphaMode BLEND (JB_BLEND_HALF),
 * KHR_materials_unlit, and whether a base colour texture is given. A
 * material is named by its name when the file gives it one no other
 * material has, and otherwise "#k", k its index; a primitive without one
 * has "default". Morph targets, skins, texture transforms and other
 * extensions are ignored.
 */
#include "mesh.h"

#include <stdio.h>
#include <string.h>

#include "cgltf.h"

static const char *result_name(cgltf_result r)
{
    switch (r) {
    case cgltf_result_data_too_short: return "data too short";
    case cgltf_result_unknown_format: return "not glTF or GLB";
    case cgltf_result_invalid_json: return "invalid JSON";
    case cgltf_result_invalid_gltf: return "invalid glTF";
    case cgltf_result_invalid_options: return "invalid options";
    case cgltf_result_file_not_found: return "file not found";
    case cgltf_result_io_error: return "cannot read it";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "glTF 1.0, not 2.0";
    default: return "error";
    }
}

/* Column-major 4 x 4, as glTF's. */
typedef struct mat4d {
    double m[16];
} mat4d;

static mat4d identity(void)
{
    mat4d r;
    int i;

    for (i = 0; i < 16; i++)
        r.m[i] = (i % 5 == 0) ? 1.0 : 0.0;
    return r;
}

static mat4d mul(const mat4d *a, const mat4d *b)
{
    mat4d r;
    int i, j, k;

    for (j = 0; j < 4; j++)
        for (i = 0; i < 4; i++) {
            double s = 0.0;
            for (k = 0; k < 4; k++)
                s += a->m[k * 4 + i] * b->m[j * 4 + k];
            r.m[j * 4 + i] = s;
        }
    return r;
}

static mat4d local(const cgltf_node *n)
{
    mat4d r = identity();
    int i;

    if (n->has_matrix) {
        for (i = 0; i < 16; i++)
            r.m[i] = n->matrix[i];
        return r;
    }
    {
        double t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
        double xx, yy, zz, xy, xz, yz, wx, wy, wz;
        if (n->has_translation)
            for (i = 0; i < 3; i++)
                t[i] = n->translation[i];
        if (n->has_rotation)
            for (i = 0; i < 4; i++)
                q[i] = n->rotation[i];
        if (n->has_scale)
            for (i = 0; i < 3; i++)
                s[i] = n->scale[i];
        xx = q[0] * q[0], yy = q[1] * q[1], zz = q[2] * q[2];
        xy = q[0] * q[1], xz = q[0] * q[2], yz = q[1] * q[2];
        wx = q[3] * q[0], wy = q[3] * q[1], wz = q[3] * q[2];
        r.m[0] = (1.0 - 2.0 * (yy + zz)) * s[0];
        r.m[1] = 2.0 * (xy + wz) * s[0];
        r.m[2] = 2.0 * (xz - wy) * s[0];
        r.m[4] = 2.0 * (xy - wz) * s[1];
        r.m[5] = (1.0 - 2.0 * (xx + zz)) * s[1];
        r.m[6] = 2.0 * (yz + wx) * s[1];
        r.m[8] = 2.0 * (xz + wy) * s[2];
        r.m[9] = 2.0 * (yz - wx) * s[2];
        r.m[10] = (1.0 - 2.0 * (xx + yy)) * s[2];
        r.m[12] = t[0], r.m[13] = t[1], r.m[14] = t[2];
    }
    return r;
}

/* The upper 3 x 3's cofactors (its determinant times its inverse
   transpose, so they turn normals as the transform turns surfaces), as
   n[row * 3 + col], and the determinant. */
static double normal_matrix(const mat4d *a, double *n)
{
#define A(r, c) a->m[(c) * 4 + (r)]
    double det;

    n[0] = A(1, 1) * A(2, 2) - A(1, 2) * A(2, 1);
    n[1] = A(1, 2) * A(2, 0) - A(1, 0) * A(2, 2);
    n[2] = A(1, 0) * A(2, 1) - A(1, 1) * A(2, 0);
    n[3] = A(0, 2) * A(2, 1) - A(0, 1) * A(2, 2);
    n[4] = A(0, 0) * A(2, 2) - A(0, 2) * A(2, 0);
    n[5] = A(0, 1) * A(2, 0) - A(0, 0) * A(2, 1);
    n[6] = A(0, 1) * A(1, 2) - A(0, 2) * A(1, 1);
    n[7] = A(0, 2) * A(1, 0) - A(0, 0) * A(1, 2);
    n[8] = A(0, 0) * A(1, 1) - A(0, 1) * A(1, 0);
    det = A(0, 0) * n[0] + A(0, 1) * n[1] + A(0, 2) * n[2];
#undef A
    return det;
}

typedef struct reader {
    const char *path;
    const cgltf_data *d;
    jbm_scene *s;
} reader;

/* The scene material for a glTF material (NULL: "default"). */
static int material_of(reader *r, const cgltf_material *m)
{
    char name[128];
    size_t k, i;
    int unique = 1, idx;

    if (m == NULL)
        return jbm_add_material(r->s, "default");
    k = (size_t)(m - r->d->materials);
    if (m->name != NULL && m->name[0] != '\0' && strlen(m->name) < sizeof name) {
        for (i = 0; i < r->d->materials_count; i++)
            if (i != k && r->d->materials[i].name != NULL &&
                strcmp(r->d->materials[i].name, m->name) == 0)
                unique = 0;
        if (strcmp(m->name, "default") == 0 || m->name[0] == '#')
            unique = 0;
    } else {
        unique = 0;
    }
    if (unique)
        snprintf(name, sizeof name, "%s", m->name);
    else
        snprintf(name, sizeof name, "#%lu", (unsigned long)k);
    i = (size_t)r->s->material_count;
    idx = jbm_add_material(r->s, name);
    if (idx >= 0 && (size_t)idx == i) {
        jbm_material *o = &r->s->materials[idx];
        const cgltf_float *f = m->pbr_metallic_roughness.base_color_factor;
        if (m->has_pbr_metallic_roughness) {
            o->color[0] = f[0], o->color[1] = f[1], o->color[2] = f[2];
            o->had_texture = m->pbr_metallic_roughness.base_color_texture.texture != NULL;
        }
        o->double_sided = m->double_sided != 0;
        o->blend = m->alpha_mode == cgltf_alpha_mode_blend ? 1 : 0;
        o->unlit = m->unlit != 0;
    }
    return idx;
}

static int error(const reader *r, const char *what)
{
    fprintf(stderr, "jb-mesh: %s: %s\n", r->path, what);
    return -1;
}

static int add_primitive(reader *r, const cgltf_primitive *p, const mat4d *w, size_t mesh,
                         size_t prim)
{
    const cgltf_accessor *pos = NULL, *nrm = NULL, *uv = NULL, *col = NULL;
    double nm[9], det;
    size_t i, count, tris;
    int material;
    char msg[160];

    if (p->type != cgltf_primitive_type_triangles &&
        p->type != cgltf_primitive_type_triangle_strip &&
        p->type != cgltf_primitive_type_triangle_fan) {
        snprintf(msg, sizeof msg,
                 "mesh %lu, primitive %lu: points and lines are not supported, only triangles",
                 (unsigned long)mesh, (unsigned long)prim);
        return error(r, msg);
    }
    for (i = 0; i < p->attributes_count; i++) {
        const cgltf_attribute *a = &p->attributes[i];
        if (a->type == cgltf_attribute_type_position && a->index == 0)
            pos = a->data;
        else if (a->type == cgltf_attribute_type_normal && a->index == 0)
            nrm = a->data;
        else if (a->type == cgltf_attribute_type_texcoord && a->index == 0)
            uv = a->data;
        else if (a->type == cgltf_attribute_type_color && a->index == 0)
            col = a->data;
    }
    if (pos == NULL) {
        snprintf(msg, sizeof msg, "mesh %lu, primitive %lu has no POSITION",
                 (unsigned long)mesh, (unsigned long)prim);
        return error(r, msg);
    }
    /* The accessors' types, which cgltf_validate does not check, and on
       which the reads below rely (x has room for 4 floats). */
    {
        const char *bad = pos->type != cgltf_type_vec3                  ? "POSITION is not VEC3"
                          : nrm != NULL && nrm->type != cgltf_type_vec3 ? "NORMAL is not VEC3"
                          : uv != NULL && uv->type != cgltf_type_vec2   ? "TEXCOORD_0 is not VEC2"
                          : col != NULL && col->type != cgltf_type_vec3 &&
                                  col->type != cgltf_type_vec4
                              ? "COLOR_0 is not VEC3 or VEC4"
                              : NULL;
        if (bad != NULL) {
            snprintf(msg, sizeof msg, "mesh %lu, primitive %lu: its %s", (unsigned long)mesh,
                     (unsigned long)prim, bad);
            return error(r, msg);
        }
    }
    count = p->indices != NULL ? p->indices->count : pos->count;
    tris = p->type == cgltf_primitive_type_triangles ? count / 3 : count >= 3 ? count - 2 : 0;
    material = material_of(r, p->material);
    if (material < 0)
        return error(r, "out of memory");
    det = normal_matrix(w, nm);
    for (i = 0; i < tris; i++) {
        size_t k[3], j;
        jbm_face f;
        if (p->type == cgltf_primitive_type_triangles) {
            k[0] = 3 * i, k[1] = 3 * i + 1, k[2] = 3 * i + 2;
        } else if (p->type == cgltf_primitive_type_triangle_strip) {
            k[0] = i;
            k[1] = i + 1 + i % 2;
            k[2] = i + 2 - i % 2;
        } else {
            k[0] = i + 1, k[1] = i + 2, k[2] = 0;
        }
        if (det < 0.0) {
            size_t t = k[1];
            k[1] = k[2];
            k[2] = t;
        }
        memset(&f, 0, sizeof f);
        f.vertices = 3;
        f.material = material;
        for (j = 0; j < 3; j++) {
            jbm_corner *c = &f.c[j];
            size_t v = p->indices != NULL ? cgltf_accessor_read_index(p->indices, k[j]) : k[j];
            cgltf_float x[4] = {0, 0, 0, 0};
            double q[3];
            int a;
            if (v >= pos->count) {
                snprintf(msg, sizeof msg, "mesh %lu, primitive %lu: index %lu past its %lu vertices",
                         (unsigned long)mesh, (unsigned long)prim, (unsigned long)v,
                         (unsigned long)pos->count);
                return error(r, msg);
            }
            if (!cgltf_accessor_read_float(pos, v, x, 3))
                return error(r, "a POSITION accessor cannot be read as floats");
            for (a = 0; a < 3; a++)
                q[a] = x[a];
            for (a = 0; a < 3; a++)
                c->p[a] = w->m[a] * q[0] + w->m[4 + a] * q[1] + w->m[8 + a] * q[2] + w->m[12 + a];
            memset(x, 0, sizeof x);
            if (nrm != NULL && v < nrm->count && cgltf_accessor_read_float(nrm, v, x, 3)) {
                double sign = det < 0.0 ? -1.0 : 1.0;
                for (a = 0; a < 3; a++)
                    q[a] = x[a];
                for (a = 0; a < 3; a++)
                    c->n[a] = sign * (nm[a * 3] * q[0] + nm[a * 3 + 1] * q[1] + nm[a * 3 + 2] * q[2]);
                c->has_n = 1;
            }
            memset(x, 0, sizeof x);
            if (uv != NULL && v < uv->count && cgltf_accessor_read_float(uv, v, x, 2)) {
                c->uv[0] = x[0];
                c->uv[1] = x[1];
                c->has_uv = 1;
            }
            memset(x, 0, sizeof x);
            if (col != NULL && v < col->count && cgltf_accessor_read_float(col, v, x, 4)) {
                for (a = 0; a < 3; a++)
                    c->c[a] = x[a];
                c->has_c = 1;
            }
        }
        if (jbm_add_face(r->s, &f) != 0)
            return error(r, "out of memory");
    }
    return 0;
}

static int add_mesh(reader *r, const cgltf_mesh *m, const mat4d *w)
{
    size_t i;

    for (i = 0; i < m->primitives_count; i++)
        if (add_primitive(r, &m->primitives[i], w, (size_t)(m - r->d->meshes), i) != 0)
            return -1;
    return 0;
}

static int walk(reader *r, const cgltf_node *n, const mat4d *parent, int depth)
{
    mat4d l, w;
    size_t i;

    if (depth > 1000)
        return error(r, "nodes nested more than 1000 deep");
    l = local(n);
    w = mul(parent, &l);
    if (n->mesh != NULL && add_mesh(r, n->mesh, &w) != 0)
        return -1;
    for (i = 0; i < n->children_count; i++)
        if (walk(r, n->children[i], &w, depth + 1) != 0)
            return -1;
    return 0;
}

int jbm_read_gltf(const char *path, jbm_scene *s)
{
    cgltf_options o;
    cgltf_data *d = NULL;
    cgltf_result res;
    reader r;
    mat4d id = identity();
    const cgltf_scene *scene;
    size_t i;
    int rc = 0;

    memset(&o, 0, sizeof o);
    r.path = path;
    r.s = s;
    res = cgltf_parse_file(&o, path, &d);
    if (res == cgltf_result_success)
        res = cgltf_load_buffers(&o, d, path);
    if (res == cgltf_result_success)
        res = cgltf_validate(d);
    if (res != cgltf_result_success) {
        fprintf(stderr, "jb-mesh: %s: %s\n", path, result_name(res));
        cgltf_free(d);
        return -1;
    }
    r.d = d;
    scene = d->scene != NULL ? d->scene : d->scenes_count > 0 ? &d->scenes[0] : NULL;
    if (scene != NULL) {
        for (i = 0; rc == 0 && i < scene->nodes_count; i++)
            rc = walk(&r, scene->nodes[i], &id, 0);
    } else if (d->nodes_count > 0) {
        for (i = 0; rc == 0 && i < d->nodes_count; i++)
            if (d->nodes[i].parent == NULL)
                rc = walk(&r, &d->nodes[i], &id, 0);
    } else {
        for (i = 0; rc == 0 && i < d->meshes_count; i++)
            rc = add_mesh(&r, &d->meshes[i], &id);
    }
    cgltf_free(d);
    return rc;
}

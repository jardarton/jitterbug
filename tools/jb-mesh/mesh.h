/*
 * jb-mesh's model of what it reads (obj.c, gltf.c) before it is quantised
 * into a .jbm (jb-mesh.c): faces of corners in double precision, each
 * corner with what the file gave it, and the materials by name.
 *
 * Conventions, the same for both readers: positions in the file's units
 * after any node transform (right-handed, y up, as jb_mesh takes them);
 * faces going anticlockwise round their front; texture coordinates with
 * (0, 0) the image's top-left corner and (1, 1) its bottom-right (OBJ's v
 * is turned over to this); colours as factors, 1.0 full.
 */
#ifndef JB_MESH_MESH_H
#define JB_MESH_MESH_H

#include <stddef.h>

typedef struct jbm_corner {
    double p[3];
    double n[3];
    double c[3];
    double uv[2];
    int has_n, has_c, has_uv;
} jbm_corner;

typedef struct jbm_face {
    int vertices; /* 3 or 4 */
    int material; /* index into the scene's materials */
    jbm_corner c[4];
} jbm_face;

typedef struct jbm_material {
    char name[128];
    double color[3]; /* the diffuse or base colour factor */
    int blend;       /* JB_BLEND_* from the file (glTF BLEND, MTL d < 1) */
    int unlit;       /* glTF KHR_materials_unlit */
    int double_sided;
    int had_texture; /* the file gives it a texture (map_Kd, baseColorTexture) */
} jbm_material;

typedef struct jbm_scene {
    jbm_face *faces;
    size_t face_count, face_cap;
    jbm_material *materials;
    int material_count, material_cap;
} jbm_scene;

/* Appends a face (copied); 0, or -1 for no memory. */
int jbm_add_face(jbm_scene *s, const jbm_face *f);

/* The index of the material named name, added (colour 1, 1, 1) if it is
   new; -1 for no memory or a name of 128 bytes or more. */
int jbm_add_material(jbm_scene *s, const char *name);

void jbm_scene_free(jbm_scene *s);

/*
 * The readers. Each fills s from the file at path and returns 0, or prints
 * "jb-mesh: path:line: why" (or without a line) to stderr and returns -1.
 * Warnings (something ignored) go to stderr too.
 */
int jbm_read_obj(const char *path, jbm_scene *s);
int jbm_read_gltf(const char *path, jbm_scene *s);

/*
 * A decimal number, as OBJ and JSON write them: [+-]digits[.digits][(e|E)
 * [+-]digits], at most 19 significant digits kept (the rest only move the
 * exponent). Its value is made with IEEE double operations alone (the
 * digits as an integer, times or divided by exact powers of ten), so the
 * same text gives the same double on every platform, whatever the C
 * library's strtod does. *end gets the first character after it; returns
 * 0, or -1 for text that is not such a number or one too large for a
 * double (a tiny one becomes a subnormal or 0, the same everywhere).
 */
int jbm_parse_number(const char *text, const char **end, double *out);

#endif

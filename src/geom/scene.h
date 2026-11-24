/*
 * The friendly level's scene state (include/jitterbug.h, Scene geometry):
 * camera, lights, fog, the geometry path, and the private GTE the
 * friendly 3D draws use. One per context, inside its public API state
 * (src/api/api.h); the public setters (src/api/scene.c) check their
 * arguments with the functions here.
 *
 * The private GTE is separate from the one jb_gte_* drive, so friendly
 * draws never change a register a program set, and it has no precise
 * table: the friendly records carry their sub-pixel positions themselves
 * (src/geom/pipeline.h).
 */
#ifndef JBI_GEOM_SCENE_H
#define JBI_GEOM_SCENE_H

#include <stddef.h>
#include <stdint.h>

#include <jitterbug.h>

#include "geom/gte.h"

enum { JBI_LIGHTS = 3 };

/* The camera's defaults (jb_camera). */
#define JBI_CAMERA_FOV 60.0f
#define JBI_CAMERA_NEAR 32.0f
#define JBI_CAMERA_FAR 4096.0f
/* The largest light intensity: a light colour channel of 255 at this is
   the GTE's largest level, 32767 / 4096. */
#define JBI_LIGHT_MAX 7.99f

typedef struct jbi_scene {
    jb_camera camera; /* with every default applied */
    jb_mat4 view;     /* jb_mat4_look_at of the camera: world to view */
    uint32_t ambient;
    /* Per light: the unit direction from a surface towards it (minus
       jb_light.direction, normalised), world space; colour 0 is off. */
    jb_vec3 light_to[JBI_LIGHTS];
    uint32_t light_color[JBI_LIGHTS];
    float light_intensity[JBI_LIGHTS]; /* jb_light.intensity, 0 made 1 */
    int fog; /* on */
    uint32_t fog_color;
    float fog_start, fog_end;
    int geometry; /* JB_GEOMETRY_* */
    jbi_gte gte;  /* the friendly draws' own GTE */
} jbi_scene;

/* A new context's: the default camera, ambient white and no lights, no
   fog, the GTE path, every register of the private GTE 0. */
void jbi_scene_init(jbi_scene *s);

/*
 * The setters, for the public calls of the same names. Each returns JB_OK
 * and changes the state, or an error with its detail (naming the public
 * call) in detail (n bytes, always terminated) and changes nothing. The
 * arguments are the public call's, without jb.
 */
jb_result jbi_scene_set_camera(jbi_scene *s, const jb_camera *c, char *detail, size_t n);
jb_result jbi_scene_set_ambient(jbi_scene *s, uint32_t color, char *detail, size_t n);
jb_result jbi_scene_set_light(jbi_scene *s, int index, const jb_light *l, char *detail, size_t n);
jb_result jbi_scene_set_fog(jbi_scene *s, uint32_t color, float start, float end, char *detail,
                            size_t n);
jb_result jbi_scene_set_geometry(jbi_scene *s, int mode, char *detail, size_t n);

#endif

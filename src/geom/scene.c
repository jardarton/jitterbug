/* The friendly scene state (scene.h). */
#include "geom/scene.h"

#include "geom/fmath.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static jb_result fail(char *detail, size_t n, jb_result r, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 4, 5)))
#endif
    ;

static jb_result fail(char *detail, size_t n, jb_result r, const char *fmt, ...)
{
    va_list ap;

    if (detail != NULL && n > 0) {
        va_start(ap, fmt);
        vsnprintf(detail, n, fmt, ap);
        va_end(ap);
    }
    return r;
}

static int finite(float x)
{
    return x - x == 0.0f;
}

static int finite3(jb_vec3 v)
{
    return finite(v.x) && finite(v.y) && finite(v.z);
}

void jbi_scene_init(jbi_scene *s)
{
    char unused[8];
    int i;

    memset(s, 0, sizeof *s);
    jbi_scene_set_camera(s, NULL, unused, sizeof unused);
    s->ambient = 0xffffffu;
    for (i = 0; i < JBI_LIGHTS; i++)
        s->light_intensity[i] = 1.0f;
    s->geometry = JB_GEOMETRY_GTE;
    jbi_gte_init(&s->gte);
}

jb_result jbi_scene_set_camera(jbi_scene *s, const jb_camera *c, char *detail, size_t n)
{
    static const char fn[] = "jb_set_camera";
    jb_camera k;
    jb_vec3 d, side;
    jb_mat4 view;
    int i;

    if (c != NULL)
        k = *c;
    else
        memset(&k, 0, sizeof k);
    if (!finite3(k.eye) || !finite3(k.target) || !finite3(k.up) || !finite(k.fov_y) ||
        !finite(k.z_near) || !finite(k.z_far) || !finite(k.projection))
        return fail(detail, n, JB_ERROR_INVALID_ARGUMENT, "%s: a value is not finite", fn);
    if (k.eye.x == 0.0f && k.eye.y == 0.0f && k.eye.z == 0.0f && k.target.x == 0.0f &&
        k.target.y == 0.0f && k.target.z == 0.0f)
        k.target.z = -1.0f; /* both zero (or NULL): the default view, down -z */
    if (k.up.x == 0.0f && k.up.y == 0.0f && k.up.z == 0.0f)
        k.up.y = 1.0f;
    if (k.fov_y == 0.0f)
        k.fov_y = JBI_CAMERA_FOV;
    if (!(k.fov_y > 0.0f && k.fov_y < 180.0f))
        return fail(detail, n, JB_ERROR_INVALID_ARGUMENT,
                    "%s: fov_y %g is outside 0..180 degrees", fn, (double)k.fov_y);
    if (k.projection < 0.0f)
        return fail(detail, n, JB_ERROR_INVALID_ARGUMENT, "%s: projection %g is negative", fn,
                    (double)k.projection);
    if (k.z_near < 0.0f || k.z_far < 0.0f)
        return fail(detail, n, JB_ERROR_INVALID_ARGUMENT, "%s: z_near %g or z_far %g is negative",
                    fn, (double)k.z_near, (double)k.z_far);
    if (k.z_near == 0.0f)
        k.z_near = JBI_CAMERA_NEAR;
    if (k.z_far == 0.0f)
        k.z_far = JBI_CAMERA_FAR;
    if (!(k.z_far > k.z_near))
        return fail(detail, n, JB_ERROR_INVALID_ARGUMENT,
                    "%s: z_far %g is not beyond z_near %g", fn, (double)k.z_far,
                    (double)k.z_near);
    d.x = k.target.x - k.eye.x;
    d.y = k.target.y - k.eye.y;
    d.z = k.target.z - k.eye.z;
    if (d.x == 0.0f && d.y == 0.0f && d.z == 0.0f)
        return fail(detail, n, JB_ERROR_INVALID_ARGUMENT,
                    "%s: eye and target are the same point", fn);
    if (!finite3(d))
        return fail(detail, n, JB_ERROR_INVALID_ARGUMENT,
                    "%s: eye and target are too far apart for float arithmetic", fn);
    /* Crossed with up made unit first (so an up of 1e-30 is no different
       from one of 1). */
    side = jb_vec3_cross(jb_vec3_normalize(d), jb_vec3_normalize(k.up));
    if (jbi_vec3_parallel(k.eye, k.target, k.up) || jb_vec3_dot(side, side) == 0.0f)
        return fail(detail, n, JB_ERROR_INVALID_ARGUMENT,
                    "%s: up is parallel to the view direction", fn);
    view = jb_mat4_look_at(k.eye, k.target, k.up);
    for (i = 0; i < 16; i++)
        if (!finite(view.m[i]))
            return fail(detail, n, JB_ERROR_INVALID_ARGUMENT,
                        "%s: the view is not representable in float (the eye is too far "
                        "from the origin)",
                        fn);
    s->camera = k;
    s->view = view;
    return JB_OK;
}

jb_result jbi_scene_set_ambient(jbi_scene *s, uint32_t color, char *detail, size_t n)
{
    if (color > 0xffffffu)
        return fail(detail, n, JB_ERROR_INVALID_ARGUMENT,
                    "jb_set_ambient: color 0x%lx is above 0xFFFFFF", (unsigned long)color);
    s->ambient = color;
    return JB_OK;
}

jb_result jbi_scene_set_light(jbi_scene *s, int index, const jb_light *l, char *detail, size_t n)
{
    static const char fn[] = "jb_set_light";
    jb_vec3 to;

    if (index < 0 || index >= JBI_LIGHTS)
        return fail(detail, n, JB_ERROR_INVALID_ARGUMENT, "%s: index %d is outside 0..%d", fn,
                    index, JBI_LIGHTS - 1);
    if (l == NULL || l->color == 0) {
        s->light_color[index] = 0;
        s->light_intensity[index] = 1.0f;
        memset(&s->light_to[index], 0, sizeof s->light_to[index]);
        return JB_OK;
    }
    if (!finite(l->intensity) || l->intensity < 0.0f || l->intensity > JBI_LIGHT_MAX)
        return fail(detail, n, JB_ERROR_INVALID_ARGUMENT,
                    "%s: intensity %g is outside 0..%g", fn, (double)l->intensity,
                    (double)JBI_LIGHT_MAX);
    if (l->color > 0xffffffu)
        return fail(detail, n, JB_ERROR_INVALID_ARGUMENT,
                    "%s: color 0x%lx is above 0xFFFFFF", fn, (unsigned long)l->color);
    if (!finite3(l->direction))
        return fail(detail, n, JB_ERROR_INVALID_ARGUMENT, "%s: direction is not finite", fn);
    to.x = -l->direction.x;
    to.y = -l->direction.y;
    to.z = -l->direction.z;
    to = jb_vec3_normalize(to);
    if (jb_vec3_dot(to, to) == 0.0f || !finite3(to))
        return fail(detail, n, JB_ERROR_INVALID_ARGUMENT,
                    "%s: direction (%g, %g, %g) has no length", fn, (double)l->direction.x,
                    (double)l->direction.y, (double)l->direction.z);
    s->light_to[index] = to;
    s->light_color[index] = l->color;
    s->light_intensity[index] = l->intensity != 0.0f ? l->intensity : 1.0f;
    return JB_OK;
}

jb_result jbi_scene_set_fog(jbi_scene *s, uint32_t color, float start, float end, char *detail,
                            size_t n)
{
    static const char fn[] = "jb_set_fog";

    if (color > 0xffffffu)
        return fail(detail, n, JB_ERROR_INVALID_ARGUMENT, "%s: color 0x%lx is above 0xFFFFFF",
                    fn, (unsigned long)color);
    if (end == 0.0f) {
        s->fog = 0;
        return JB_OK;
    }
    if (!finite(start) || !finite(end) || !(start > 0.0f) || !(end > start))
        return fail(detail, n, JB_ERROR_INVALID_ARGUMENT,
                    "%s: start %g and end %g are not 0 < start < end", fn, (double)start,
                    (double)end);
    s->fog = 1;
    s->fog_color = color;
    s->fog_start = start;
    s->fog_end = end;
    return JB_OK;
}

jb_result jbi_scene_set_geometry(jbi_scene *s, int mode, char *detail, size_t n)
{
    if (mode != JB_GEOMETRY_GTE && mode != JB_GEOMETRY_FLOAT)
        return fail(detail, n, JB_ERROR_INVALID_ARGUMENT,
                    "jb_set_geometry: mode %d is not JB_GEOMETRY_GTE or JB_GEOMETRY_FLOAT", mode);
    s->geometry = mode;
    return JB_OK;
}

/*
 * The friendly scene state's public setters (include/jitterbug.h, Scene
 * geometry): jb_set_camera, jb_set_ambient, jb_set_light, jb_set_fog and
 * jb_set_geometry, over the scene in the context's API state
 * (src/geom/scene.h).
 */
#include "api/api.h"

static jb_result done(jb_context *jb, jb_result r, const char *detail)
{
    if (r == JB_OK)
        return JB_OK;
    return jbi_api_fail(jb, r, "%s", detail);
}

jb_result jb_set_camera(jb_context *jb, const jb_camera *camera)
{
    char detail[160];

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    return done(jb, jbi_scene_set_camera(&jb->api->scene, camera, detail, sizeof detail), detail);
}

jb_result jb_set_ambient(jb_context *jb, uint32_t color)
{
    char detail[160];

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    return done(jb, jbi_scene_set_ambient(&jb->api->scene, color, detail, sizeof detail), detail);
}

jb_result jb_set_light(jb_context *jb, int index, const jb_light *light)
{
    char detail[160];

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    return done(jb, jbi_scene_set_light(&jb->api->scene, index, light, detail, sizeof detail),
                detail);
}

jb_result jb_set_fog(jb_context *jb, uint32_t color, float start, float end)
{
    char detail[160];

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    return done(jb,
                jbi_scene_set_fog(&jb->api->scene, color, start, end, detail, sizeof detail),
                detail);
}

jb_result jb_set_geometry(jb_context *jb, int mode)
{
    char detail[160];

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    return done(jb, jbi_scene_set_geometry(&jb->api->scene, mode, detail, sizeof detail),
                detail);
}

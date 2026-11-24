/*
 * The demo's assets (assets.h): the .jbt and .jbm files built into the
 * program (files.h), loaded with the library's _mem loaders.
 */
#include "assets.h"

#include <string.h>

#include "files.h"

/* The file of that name, or NULL with *size 0. */
static const unsigned char *file(const char *name, size_t *size)
{
    int i;

    for (i = 0; i < dm_file_count; ++i)
        if (strcmp(dm_files[i].name, name) == 0) {
            *size = dm_files[i].size;
            return dm_files[i].data;
        }
    *size = 0;
    return NULL;
}

/* A texture, placed; *palettes (when not NULL) its palettes, *frames how
   many. */
static jb_result texture(jb_context *jb, const char *name, jb_texture *out,
                         const uint16_t **palettes, int *frames)
{
    size_t size;
    const unsigned char *data = file(name, &size);
    jb_texture_desc desc;
    jb_result r;
    int n;

    if (palettes != NULL) {
        if ((r = jb_texture_parse(jb, data, size, &desc, &n)) != JB_OK)
            return r;
        *palettes = desc.palette;
        if (frames != NULL)
            *frames = n;
    }
    return jb_texture_load_mem(jb, data, size, out);
}

/* A mesh, with textures bound to its slots. */
static jb_result mesh(jb_context *jb, const char *name, const jb_texture *textures, int count,
                      jb_mesh *out)
{
    size_t size;
    const unsigned char *data = file(name, &size);

    return jb_mesh_load_mem(jb, data, size, textures, count, out);
}

jb_result dm_assets_load(jb_context *jb, dm_assets *a)
{
    jb_result r;

    memset(a, 0, sizeof *a);
    if ((r = texture(jb, "sand.jbt", &a->tex[DM_TEX_SAND], NULL, NULL)) != JB_OK ||
        (r = texture(jb, "grass.jbt", &a->tex[DM_TEX_GRASS], NULL, NULL)) != JB_OK ||
        (r = texture(jb, "rock.jbt", &a->tex[DM_TEX_ROCK], NULL, NULL)) != JB_OK ||
        (r = texture(jb, "water.jbt", &a->tex[DM_TEX_WATER], &a->water, &a->water_frames)) !=
            JB_OK ||
        (r = texture(jb, "cloud.jbt", &a->tex[DM_TEX_CLOUD], &a->cloud, NULL)) != JB_OK ||
        (r = texture(jb, "glow.jbt", &a->tex[DM_TEX_GLOW], NULL, NULL)) != JB_OK ||
        (r = texture(jb, "tower.jbt", &a->tex[DM_TEX_TOWER], NULL, NULL)) != JB_OK ||
        (r = texture(jb, "planks.jbt", &a->tex[DM_TEX_PLANKS], NULL, NULL)) != JB_OK)
        return r;
    if ((r = mesh(jb, "tree.jbm", NULL, 0, &a->tree)) != JB_OK ||
        (r = mesh(jb, "stone.jbm", &a->tex[DM_TEX_ROCK], 1, &a->stone)) != JB_OK ||
        (r = mesh(jb, "lighthouse.jbm", &a->tex[DM_TEX_TOWER], 1, &a->tower)) != JB_OK ||
        (r = mesh(jb, "beam.jbm", NULL, 0, &a->beam)) != JB_OK ||
        (r = mesh(jb, "pier.jbm", &a->tex[DM_TEX_PLANKS], 1, &a->pier)) != JB_OK)
        return r;
    return JB_OK;
}

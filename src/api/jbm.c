/*
 * Mesh files, .jbm (docs/formats.md; include/jitterbug.h, 3D drawing):
 * jb_mesh_load_mem, jb_mesh_load, jb_mesh_destroy.
 *
 * A loaded mesh is one allocation from the context's allocator, a block:
 * a header that links it into the context's list of loaded meshes (so
 * jb_mesh_destroy can tell a loaded mesh from any other, and jb_destroy
 * frees those left), then the arrays a jb_mesh points to, decoded from
 * the file, and a copy of each bound texture, which the materials point
 * to. The header and the sections are checked before the block is
 * allocated; the records are checked as they are decoded and then as
 * jb_draw_mesh checks a mesh (jbi_geom_check_mesh: indices, colours, u, v
 * within the bound textures), so a loaded mesh is one jb_draw_mesh takes.
 * A failure frees the block.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "api/api.h"
#include "api/files.h"
#include "api/formats.h"
#include "core/alloc.h"
#include "geom/pipeline.h"

struct jbi_mesh_block {
    struct jbi_mesh_block *next, *prev;
    jb_mesh mesh;
};

/* The arrays' offsets in a block, each aligned for any object. */
static size_t align_up(size_t n)
{
    const size_t a = _Alignof(max_align_t);

    return (n + a - 1) / a * a;
}

/* A signed 16-bit value. */
static int s16(const unsigned char *q)
{
    unsigned u = jbi_le16(q);

    return u >= 0x8000u ? (int)u - 0x10000 : (int)u;
}

typedef struct jbm_header {
    uint32_t flags, vertices, faces, materials, slots;
    uint32_t off[JBI_JBM_SECTIONS];
} jbm_header;

static jb_result check_header(jb_context *jb, const char *fn, const unsigned char *p, size_t size,
                              jbm_header *h)
{
    /* Not pointers: a table of them would be writable data (relocated). */
    static const char names[JBI_JBM_SECTIONS][20] = {
        "positions", "normals", "colours", "texel coordinates", "faces", "materials",
        "texture slots"};
    static const unsigned rec[JBI_JBM_SECTIONS] = {
        JBI_JBM_POSITION, JBI_JBM_NORMAL, JBI_JBM_COLOR, JBI_JBM_UVREC,
        JBI_JBM_FACE,     JBI_JBM_MATERIAL, JBI_JBM_SLOT};
    jbi_section s[JBI_JBM_SECTIONS];
    uint32_t version, file_size, reserved, count[JBI_JBM_SECTIONS];
    int present[JBI_JBM_SECTIONS], i;

    memset(h, 0, sizeof *h);
    if (p == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: data is NULL", fn);
    if (size < JBI_JBM_HEADER)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: %lu bytes is shorter than a .jbm header (%d bytes)", fn,
                            (unsigned long)size, JBI_JBM_HEADER);
    if (memcmp(p + JBI_JBM_O_MAGIC, JBI_JBM_MAGIC, 4) != 0)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: not a .jbm file (no \"%s\")", fn,
                            JBI_JBM_MAGIC);
    version = jbi_le32(p + JBI_JBM_O_VERSION);
    if (version != JBI_JBM_VERSION)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: .jbm version %lu; this library reads version %d", fn,
                            (unsigned long)version, JBI_JBM_VERSION);
    file_size = jbi_le32(p + JBI_JBM_O_SIZE);
    if (file_size != size)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: the header says %lu bytes, but there are %lu", fn,
                            (unsigned long)file_size, (unsigned long)size);
    h->flags = jbi_le32(p + JBI_JBM_O_FLAGS);
    h->vertices = jbi_le32(p + JBI_JBM_O_VERTICES);
    h->faces = jbi_le32(p + JBI_JBM_O_FACES);
    h->materials = jbi_le32(p + JBI_JBM_O_MATERIALS);
    h->slots = jbi_le32(p + JBI_JBM_O_SLOTS);
    for (i = 0; i < JBI_JBM_SECTIONS; i++)
        h->off[i] = jbi_le32(p + JBI_JBM_O_SECTIONS + 4 * i);
    reserved = jbi_le32(p + JBI_JBM_O_RESERVED);
    if (h->flags & ~(uint32_t)(JBI_JBM_NORMALS | JBI_JBM_COLORS | JBI_JBM_UV))
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: flags 0x%08lx has bits other than normals, colours and texel "
                            "coordinates (0x7)",
                            fn, (unsigned long)h->flags);
    if (reserved != 0)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: the reserved header field is 0x%08lx, not 0", fn,
                            (unsigned long)reserved);
    if (h->vertices < 1 || h->vertices > JBI_JBM_MAX_VERTICES)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: vertex count %lu is outside 1..%d", fn,
                            (unsigned long)h->vertices, JBI_JBM_MAX_VERTICES);
    if (h->faces > JBI_JBM_MAX_FACES)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: face count %lu is above %d", fn,
                            (unsigned long)h->faces, JBI_JBM_MAX_FACES);
    if (h->materials > JBI_JBM_MAX_MATERIALS)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: material count %lu is above %d", fn,
                            (unsigned long)h->materials, JBI_JBM_MAX_MATERIALS);
    if (h->faces > 0 && h->materials == 0)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: %lu faces but no material", fn, (unsigned long)h->faces);
    if (h->slots > JBI_JBM_MAX_SLOTS)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: texture slot count %lu is above %d", fn,
                            (unsigned long)h->slots, JBI_JBM_MAX_SLOTS);
    count[JBI_JBM_S_POSITIONS] = h->vertices;
    count[JBI_JBM_S_NORMALS] = h->vertices;
    count[JBI_JBM_S_COLORS] = h->vertices;
    count[JBI_JBM_S_UV] = h->vertices;
    count[JBI_JBM_S_FACES] = h->faces;
    count[JBI_JBM_S_MATERIALS] = h->materials;
    count[JBI_JBM_S_SLOTS] = h->slots;
    present[JBI_JBM_S_POSITIONS] = 1;
    present[JBI_JBM_S_NORMALS] = (h->flags & JBI_JBM_NORMALS) != 0;
    present[JBI_JBM_S_COLORS] = (h->flags & JBI_JBM_COLORS) != 0;
    present[JBI_JBM_S_UV] = (h->flags & JBI_JBM_UV) != 0;
    present[JBI_JBM_S_FACES] = h->faces > 0;
    present[JBI_JBM_S_MATERIALS] = h->materials > 0;
    present[JBI_JBM_S_SLOTS] = h->slots > 0;
    for (i = 0; i < JBI_JBM_SECTIONS; i++) {
        if (!present[i] && h->off[i] != 0)
            return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                                "%s: there are no %s, but their offset is %lu, not 0", fn,
                                names[i], (unsigned long)h->off[i]);
        s[i].name = names[i];
        s[i].offset = h->off[i];
        s[i].bytes = present[i] ? (uint64_t)count[i] * rec[i] : 0;
    }
    return jbi_check_sections(jb, fn, s, JBI_JBM_SECTIONS, size, JBI_JBM_HEADER);
}

static jb_result load(jb_context *jb, const char *fn, const void *data, size_t size,
                      const jb_texture *textures, int texture_count, jb_mesh *out)
{
    const unsigned char *p = data;
    struct jbi_mesh_block *b;
    jbm_header h;
    jb_gte_vec *pos, *nrm = NULL;
    uint32_t *col = NULL;
    uint8_t *uv = NULL;
    jb_mesh_face *faces = NULL;
    jb_material *mats = NULL;
    jb_texture *tex = NULL;
    size_t o_tex, o_mat, o_pos, o_nrm, o_col, o_face, o_uv, total;
    char detail[200];
    jb_result r;
    uint32_t i;
    int k;

    if (out == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: out is NULL", fn);
    if (texture_count < 0)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: texture_count %d is negative", fn,
                            texture_count);
    if (texture_count > 0 && textures == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: textures is NULL but texture_count is %d", fn, texture_count);
    r = check_header(jb, fn, p, size, &h);
    if (r != JB_OK)
        return r;
    if ((uint32_t)texture_count != h.slots)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "%s: the mesh has %lu texture slots, but %d textures are given", fn,
                            (unsigned long)h.slots, texture_count);
    /* The bound textures: each the size its slot was converted for. */
    for (i = 0; i < h.slots; i++) {
        const unsigned char *s = p + h.off[JBI_JBM_S_SLOTS] + (size_t)i * JBI_JBM_SLOT;
        unsigned w = jbi_le16(s), ht = jbi_le16(s + 2);

        if (w < 1 || w > 256 || ht < 1 || ht > 256)
            return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                                "%s: texture slot %lu's size %u x %u is outside 1..256 x 1..256",
                                fn, (unsigned long)i, w, ht);
        if (textures[i].width != (int)w || textures[i].height != (int)ht)
            return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                                "%s: texture slot %lu was converted for a %u x %u texture, but "
                                "textures[%lu] is %d x %d",
                                fn, (unsigned long)i, w, ht, (unsigned long)i,
                                textures[i].width, textures[i].height);
    }

    /* The block: header, then the arrays, each aligned. */
    o_tex = align_up(sizeof *b);
    o_mat = align_up(o_tex + (size_t)h.slots * sizeof(jb_texture));
    o_pos = align_up(o_mat + (size_t)h.materials * sizeof(jb_material));
    o_nrm = align_up(o_pos + (size_t)h.vertices * sizeof(jb_gte_vec));
    o_col = align_up(o_nrm + ((h.flags & JBI_JBM_NORMALS) ? h.vertices * sizeof(jb_gte_vec) : 0));
    o_face = align_up(o_col + ((h.flags & JBI_JBM_COLORS) ? h.vertices * sizeof(uint32_t) : 0));
    o_uv = align_up(o_face + (size_t)h.faces * sizeof(jb_mesh_face));
    total = o_uv + ((h.flags & JBI_JBM_UV) ? h.vertices * 2u : 0);
    b = jbi_calloc(&jb->alloc, 1, total);
    if (b == NULL)
        return jbi_api_fail(jb, JB_ERROR_OUT_OF_MEMORY,
                            "%s: no memory for the mesh (%lu bytes)", fn, (unsigned long)total);
    pos = (jb_gte_vec *)(void *)((char *)b + o_pos);
    if (h.slots > 0)
        tex = (jb_texture *)(void *)((char *)b + o_tex);
    if (h.materials > 0)
        mats = (jb_material *)(void *)((char *)b + o_mat);
    if (h.flags & JBI_JBM_NORMALS)
        nrm = (jb_gte_vec *)(void *)((char *)b + o_nrm);
    if (h.flags & JBI_JBM_COLORS)
        col = (uint32_t *)(void *)((char *)b + o_col);
    if (h.faces > 0)
        faces = (jb_mesh_face *)(void *)((char *)b + o_face);
    if (h.flags & JBI_JBM_UV)
        uv = (uint8_t *)b + o_uv;

#define FAIL(...)                                                                             \
    do {                                                                                      \
        r = jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, __VA_ARGS__);                         \
        goto fail;                                                                            \
    } while (0)

    for (i = 0; i < h.slots; i++)
        tex[i] = textures[i];
    for (i = 0; i < h.vertices; i++) {
        const unsigned char *q = p + h.off[JBI_JBM_S_POSITIONS] + (size_t)i * JBI_JBM_POSITION;
        pos[i].x = (int16_t)s16(q);
        pos[i].y = (int16_t)s16(q + 2);
        pos[i].z = (int16_t)s16(q + 4);
    }
    for (i = 0; nrm != NULL && i < h.vertices; i++) {
        const unsigned char *q = p + h.off[JBI_JBM_S_NORMALS] + (size_t)i * JBI_JBM_NORMAL;
        int c[3];
        for (k = 0; k < 3; k++) {
            c[k] = s16(q + 2 * k);
            if (c[k] < -4096 || c[k] > 4096)
                FAIL("%s: normal %lu has a component %d, outside -4096..4096", fn,
                     (unsigned long)i, c[k]);
        }
        nrm[i].x = (int16_t)c[0];
        nrm[i].y = (int16_t)c[1];
        nrm[i].z = (int16_t)c[2];
    }
    for (i = 0; col != NULL && i < h.vertices; i++)
        col[i] = jbi_le32(p + h.off[JBI_JBM_S_COLORS] + (size_t)i * JBI_JBM_COLOR);
    if (uv != NULL)
        memcpy(uv, p + h.off[JBI_JBM_S_UV], (size_t)h.vertices * 2u);
    for (i = 0; i < h.faces; i++) {
        const unsigned char *q = p + h.off[JBI_JBM_S_FACES] + (size_t)i * JBI_JBM_FACE;
        jb_mesh_face *f = &faces[i];
        for (k = 0; k < 4; k++)
            f->v[k] = (uint16_t)jbi_le16(q + 2 * k);
        f->vertices = (uint16_t)jbi_le16(q + 8);
        f->material = (uint16_t)jbi_le16(q + 10);
        if (f->vertices == 3 && f->v[3] != 0)
            FAIL("%s: face %lu is a triangle, but its fourth index is %u, not 0", fn,
                 (unsigned long)i, (unsigned)f->v[3]);
    }
    for (i = 0; i < h.materials; i++) {
        const unsigned char *q = p + h.off[JBI_JBM_S_MATERIALS] + (size_t)i * JBI_JBM_MATERIAL;
        jb_material *m = &mats[i];
        unsigned blend = q[4], flags = q[5], slot = jbi_le16(q + 6);

        m->color = jbi_le32(q);
        if (blend > JB_BLEND_QUARTER)
            FAIL("%s: material %lu's blend mode %u is not a JB_BLEND_* mode (0..4)", fn,
                 (unsigned long)i, blend);
        if (flags & ~(unsigned)(JBI_JBM_UNLIT | JBI_JBM_RAW | JBI_JBM_DOUBLE_SIDED))
            FAIL("%s: material %lu's flags 0x%02x have bits other than unlit, raw and "
                 "double-sided (0x7)",
                 fn, (unsigned long)i, flags);
        if (slot != JBI_JBM_NO_SLOT && slot >= h.slots)
            FAIL("%s: material %lu's texture slot is %u, but there are %lu slots", fn,
                 (unsigned long)i, slot, (unsigned long)h.slots);
        m->blend = (int)blend;
        m->unlit = (flags & JBI_JBM_UNLIT) != 0;
        m->raw = (flags & JBI_JBM_RAW) != 0;
        m->double_sided = (flags & JBI_JBM_DOUBLE_SIDED) != 0;
        m->texture = slot != JBI_JBM_NO_SLOT ? &tex[slot] : NULL;
    }
#undef FAIL

    memset(&b->mesh, 0, sizeof b->mesh);
    b->mesh.vertex_count = (int)h.vertices;
    b->mesh.positions = pos;
    b->mesh.normals = nrm;
    b->mesh.colors = col;
    b->mesh.uv = uv;
    b->mesh.face_count = (int)h.faces;
    b->mesh.faces = faces;
    b->mesh.material_count = (int)h.materials;
    b->mesh.materials = mats;
    /* What jb_draw_mesh checks, here once: colours, indices in range, and
       u, v within the bound textures. */
    r = jbi_geom_check_mesh(&b->mesh, fn, NULL, detail, sizeof detail);
    if (r != JB_OK) {
        r = jbi_api_fail(jb, r, "%s", detail);
        goto fail;
    }
    /* Materials past the faces' needs are still checked as the faces'
       are (jbi_geom_check_mesh checks them only with faces). */
    for (i = 0; h.faces == 0 && i < h.materials; i++)
        if (mats[i].color > 0xffffffu && mats[i].color != JB_TINT_BLACK) {
            r = jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                             "%s: materials[%lu].color 0x%lx is above 0xFFFFFF", fn,
                             (unsigned long)i, (unsigned long)mats[i].color);
            goto fail;
        }
    b->prev = NULL;
    b->next = jb->api->meshes;
    if (b->next != NULL)
        b->next->prev = b;
    jb->api->meshes = b;
    *out = b->mesh;
    return JB_OK;

fail:
    jbi_free(&jb->alloc, b);
    return r;
}

jb_result jb_mesh_load_mem(jb_context *jb, const void *data, size_t size,
                           const jb_texture *textures, int texture_count, jb_mesh *out)
{
    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    return load(jb, "jb_mesh_load_mem", data, size, textures, texture_count, out);
}

jb_result jb_mesh_load(jb_context *jb, const char *path, const jb_texture *textures,
                       int texture_count, jb_mesh *out)
{
    static const char fn[] = "jb_mesh_load";
    unsigned char *data;
    size_t size;
    jb_result r;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (path == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: path is NULL", fn);
    if (out == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: out is NULL", fn);
    r = jbi_read_file(jb, fn, path, &data, &size);
    if (r != JB_OK)
        return r;
    r = load(jb, fn, data, size, textures, texture_count, out);
    jbi_free(&jb->alloc, data);
    return r;
}

jb_result jb_mesh_destroy(jb_context *jb, const jb_mesh *mesh)
{
    struct jbi_mesh_block *b;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (mesh == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_mesh_destroy: mesh is NULL");
    for (b = jb->api->meshes; b != NULL; b = b->next)
        if (mesh->positions != NULL && b->mesh.positions == mesh->positions)
            break;
    if (b == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_mesh_destroy: mesh is not one jb_mesh_load or jb_mesh_load_mem "
                            "made on this context, or it was destroyed already");
    if (b->prev != NULL)
        b->prev->next = b->next;
    else
        jb->api->meshes = b->next;
    if (b->next != NULL)
        b->next->prev = b->prev;
    jbi_free(&jb->alloc, b);
    return JB_OK;
}

void jbi_meshes_destroy(jb_context *jb)
{
    struct jbi_mesh_block *b = jb->api->meshes, *next;

    for (; b != NULL; b = next) {
        next = b->next;
        jbi_free(&jb->alloc, b);
    }
    jb->api->meshes = NULL;
}

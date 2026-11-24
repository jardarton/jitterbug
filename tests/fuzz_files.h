/*
 * One input through both file loaders, with every check fuzz_files makes
 * (fuzz_files.c says which), for the seeded test and the libFuzzer target
 * (fuzz_files_libfuzzer.c). C99, <jitterbug.h> and the format's layout
 * (src/api/formats.h) only.
 */
#ifndef JB_TEST_FUZZ_FILES_H
#define JB_TEST_FUZZ_FILES_H

#include <jitterbug.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api/formats.h"

typedef struct jbf_state {
    long calls, live; /* the counting allocator's */
    jb_context *jb;
    unsigned char *copy; /* an aligned copy of the input, for jb_texture_parse */
    size_t copy_cap;
    long loads, accepted, refused, drawn;
} jbf_state;

static inline void *jbf_alloc(void *user, size_t size)
{
    jbf_state *s = user;
    void *p = malloc(size);

    if (p != NULL) {
        s->calls++;
        s->live++;
    }
    return p;
}

static inline void jbf_release(void *user, void *p)
{
    jbf_state *s = user;

    s->live--;
    free(p);
}

static inline int jbf_init(jbf_state *s)
{
    jb_config cfg;
    jb_result r;

    memset(s, 0, sizeof *s);
    memset(&cfg, 0, sizeof cfg);
    cfg.alloc = jbf_alloc;
    cfg.free = jbf_release;
    cfg.alloc_user = s;
    s->jb = jb_create(&cfg, &r);
    if (s->jb == NULL || r != JB_OK)
        return -1;
    /* The first frame's memory (its ordering table), kept from then on:
       warm-up, before the loads are counted. */
    if (jb_begin_frame(s->jb, 0) != JB_OK || jb_end_frame(s->jb) != JB_OK)
        return -1;
    return 0;
}

static inline void jbf_free(jbf_state *s)
{
    jb_destroy(s->jb);
    free(s->copy);
    s->jb = NULL;
    s->copy = NULL;
}

static inline unsigned jbf_le16(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static inline uint32_t jbf_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A refusal must say what it refused, named by the call. */
static inline int jbf_refusal(jbf_state *s, jb_result r, const char *fn)
{
    const char *why = jb_error_string(s->jb);

    if (r != JB_ERROR_INVALID_ARGUMENT) {
        printf("FAIL %s returned %d (%s): %s\n", fn, (int)r, jb_result_string(r), why);
        return -1;
    }
    if (why == NULL || strstr(why, fn) == NULL) {
        printf("FAIL %s refused without its detail: \"%s\"\n", fn, why ? why : "(null)");
        return -1;
    }
    jb_check(s->jb, NULL);
    s->refused++;
    return 0;
}

static inline int jbf_texture(jbf_state *s, const unsigned char *data, size_t size, int must_load)
{
    jb_texture t;
    jb_texture_desc d;
    jb_result r, rp;

    r = jb_texture_load_mem(s->jb, data, size, &t);
    if (size > s->copy_cap) {
        free(s->copy);
        s->copy = malloc(size);
        s->copy_cap = s->copy != NULL ? size : 0;
        if (s->copy == NULL)
            return -1;
    }
    if (size > 0)
        memcpy(s->copy, data, size);
    if (r != JB_OK) {
        if (must_load) {
            printf("FAIL a valid texture: %s\n", jb_error_string(s->jb));
            return -1;
        }
        if (jbf_refusal(s, r, "jb_texture_load_mem") != 0)
            return -1;
        rp = jb_texture_parse(s->jb, size > 0 ? s->copy : data, size, &d, NULL);
        if (rp != r) {
            printf("FAIL jb_texture_parse gave %d where jb_texture_load_mem refused\n", (int)rp);
            return -1;
        }
        jb_check(s->jb, NULL);
        return 0;
    }
    s->accepted++;
    rp = jb_texture_parse(s->jb, s->copy, size, &d, NULL);
    if (rp != JB_OK || d.width != t.width || d.height != t.height || d.depth != t.depth) {
        printf("FAIL jb_texture_parse disagrees with jb_texture_load_mem: %s\n",
               jb_error_string(s->jb));
        return -1;
    }
    if (jb_texture_destroy(s->jb, &t) != JB_OK) {
        printf("FAIL destroying a loaded texture: %s\n", jb_error_string(s->jb));
        return -1;
    }
    return 0;
}

static inline int jbf_mesh(jbf_state *s, const unsigned char *data, size_t size, int must_load)
{
    jb_texture tex[4];
    int slots = 0, made = 0, i, rc = 0;
    jb_mesh m;
    jb_result r;

    /* Textures of the sizes the file's slots ask for, when it gives a few
       slots that can be read: repeat ones for a valid file (the pyramid's
       u, v repeat) and for about half the others (by their length and last
       byte), so u, v are checked both ways. */
    if (size >= JBI_JBM_HEADER) {
        uint32_t n = jbf_le32(data + JBI_JBM_O_SLOTS);
        uint32_t off = jbf_le32(data + JBI_JBM_O_SECTIONS + 4 * JBI_JBM_S_SLOTS);
        if (n <= 4 && off <= size && size - off >= n * 4u)
            slots = (int)n;
        for (i = 0; i < slots; i++) {
            jb_texture_desc d;
            memset(&d, 0, sizeof d);
            d.width = (int)jbf_le16(data + off + 4u * (uint32_t)i);
            d.height = (int)jbf_le16(data + off + 4u * (uint32_t)i + 2);
            d.depth = JB_TEX_15BIT;
            d.repeat = must_load || ((size + data[size - 1]) & 1) != 0;
            if (d.width < 1 || d.width > 256 || d.height < 1 || d.height > 256)
                d.width = d.height = 8;
            if (jb_texture_create(s->jb, &d, &tex[i]) != JB_OK) {
                d.repeat = 0;
                if (jb_texture_create(s->jb, &d, &tex[i]) != JB_OK) {
                    printf("FAIL making a %d x %d texture: %s\n", d.width, d.height,
                           jb_error_string(s->jb));
                    return -1;
                }
            }
            made++;
        }
    }
    r = jb_mesh_load_mem(s->jb, data, size, slots > 0 ? tex : NULL, slots, &m);
    if (r != JB_OK) {
        if (must_load) {
            printf("FAIL a valid mesh: %s\n", jb_error_string(s->jb));
            rc = -1;
        } else {
            rc = jbf_refusal(s, r, "jb_mesh_load_mem");
        }
    } else {
        jb_vec3 at = {0.0f, 0.0f, -600.0f}, rot = {0.3f, 0.5f, 0.0f}, one = {1.0f, 1.0f, 1.0f};
        jb_mat4 model = jb_mat4_trs(at, rot, one);
        s->accepted++;
        if (jb_begin_frame(s->jb, 0) != JB_OK) {
            printf("FAIL jb_begin_frame: %s\n", jb_error_string(s->jb));
            rc = -1;
        } else {
            r = jb_draw_mesh(s->jb, &m, &model);
            /* Refused for the GTE's limits, or the frame full of a valid
               mesh's faces (more than jb_config.max_primitives). */
            if (r != JB_OK && r != JB_ERROR_INVALID_ARGUMENT &&
                !(r == JB_ERROR_OUT_OF_MEMORY &&
                  strstr(jb_error_string(s->jb), "max_primitives") != NULL)) {
                printf("FAIL drawing a loaded mesh: %s\n", jb_error_string(s->jb));
                rc = -1;
            }
            if (r == JB_OK)
                s->drawn++;
            jb_end_frame(s->jb);
        }
        if (jb_mesh_destroy(s->jb, &m) != JB_OK) {
            printf("FAIL destroying a loaded mesh: %s\n", jb_error_string(s->jb));
            rc = -1;
        }
    }
    for (i = 0; i < made; i++)
        jb_texture_destroy(s->jb, &tex[i]);
    jb_check(s->jb, NULL);
    return rc;
}

/*
 * One input: as a texture and as a mesh (each refuses the other's magic,
 * and an input with neither is refused by both). must_load: it is a valid
 * file, which must load. Returns 0, or -1 with the failure printed.
 */
static inline int jbf_one(jbf_state *s, const unsigned char *data, size_t size, int must_load)
{
    long live = s->live;
    int is_mesh = size >= 4 && memcmp(data, JBI_JBM_MAGIC, 4) == 0;
    int is_tex = size >= 4 && memcmp(data, JBI_JBT_MAGIC, 4) == 0;

    s->loads += 2;
    if (jbf_texture(s, data, size, must_load && is_tex) != 0)
        return -1;
    if (jbf_mesh(s, data, size, must_load && is_mesh) != 0)
        return -1;
    if (s->live != live) {
        printf("FAIL the context holds %ld allocations more after the load\n", s->live - live);
        return -1;
    }
    return 0;
}

#endif

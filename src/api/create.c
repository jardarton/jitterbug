/* jb_create, jb_destroy, results and error strings (include/jitterbug.h). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "api/api.h"
#include "api/texture.h"
#include "core/alloc.h"
#include "core/config.h"

enum { MAX_THREADS = 64 };

/* One constant string and offsets into it, rather than a table of
   pointers, which a position-independent library would have to relocate
   (writable data). */
static const char result_text[] = "ok\0invalid argument\0out of memory\0unsupported\0"
                                  "unknown result";
static const unsigned char result_at[] = {0, 3, 20, 34, 46};

const char *jb_result_string(jb_result result)
{
    unsigned k = (unsigned)result;

    return result_text + result_at[k < 4 ? k : 4];
}

const char *jb_error_string(const jb_context *jb)
{
    if (jb == NULL)
        return "no context";
    return jb->api->error;
}

jb_result jbi_api_fail(jb_context *jb, jb_result r, const char *fmt, ...)
{
    struct jbi_api *a = jb->api;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(a->error, sizeof a->error, fmt, ap);
    va_end(ap);
    if (a->sticky == JB_OK && r != JB_OK) {
        a->sticky = r;
        memcpy(a->sticky_detail, a->error, sizeof a->sticky_detail);
    }
    if (a->log != NULL)
        a->log(a->log_user, a->error);
    return r;
}

jb_result jbi_api_take_error(jb_context *jb, const char **detail)
{
    struct jbi_api *a = jb->api;
    jb_result r = a->sticky;

    if (detail != NULL)
        *detail = r != JB_OK ? a->sticky_detail : "";
    a->sticky = JB_OK;
    return r;
}

jb_result jb_check(jb_context *jb, const char **detail)
{
    if (jb == NULL) {
        if (detail != NULL)
            *detail = "no context";
        return JB_ERROR_INVALID_ARGUMENT;
    }
    return jbi_api_take_error(jb, detail);
}

/* A failure before there is a context: to the log only. */
static jb_context *create_failed(const jb_config *c, jb_result *result, jb_result r,
                                 const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 4, 5)))
#endif
    ;

static jb_context *create_failed(const jb_config *c, jb_result *result, jb_result r,
                                 const char *fmt, ...)
{
    char msg[200];
    va_list ap;

    if (result != NULL)
        *result = r;
    if (c->log != NULL) {
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        c->log(c->log_user, msg);
    }
    return NULL;
}

jb_context *jb_create(const jb_config *config, jb_result *result)
{
    jb_config c;
    jbi_config ic;
    jb_context *jb;
    struct jbi_api *a;
    int scale;

    if (config != NULL)
        c = *config;
    else
        memset(&c, 0, sizeof c);
    if (c.scale < 0)
        return create_failed(&c, result, JB_ERROR_INVALID_ARGUMENT,
                             "jb_create: scale %d is negative", c.scale);
    if (c.scale > JBI_MAX_SCALE)
        return create_failed(&c, result, JB_ERROR_UNSUPPORTED,
                             "jb_create: scale %d: the software backend draws at 1x to %dx",
                             c.scale, JBI_MAX_SCALE);
    if (c.width < 0 || c.width > JBI_VRAM_W || c.height < 0 || c.height > JBI_VRAM_H)
        return create_failed(&c, result, JB_ERROR_INVALID_ARGUMENT,
                             "jb_create: display size %d x %d is outside 0..%d x 0..%d", c.width,
                             c.height, JBI_VRAM_W, JBI_VRAM_H);
    if (c.threads < 0 || c.threads > MAX_THREADS)
        return create_failed(&c, result, JB_ERROR_INVALID_ARGUMENT,
                             "jb_create: threads %d is outside 0..%d", c.threads, MAX_THREADS);
    if ((c.alloc == NULL) != (c.free == NULL))
        return create_failed(&c, result, JB_ERROR_INVALID_ARGUMENT,
                             "jb_create: alloc and free must both be set or both be NULL");
    if (c.max_primitives < 0 || c.max_primitives > JB_LIMIT_MAX_PRIMITIVES)
        return create_failed(&c, result, JB_ERROR_INVALID_ARGUMENT,
                             "jb_create: max_primitives %d is outside 0..%d", c.max_primitives,
                             JB_LIMIT_MAX_PRIMITIVES);
    if (c.ot_size < 0 || c.ot_size > JB_LIMIT_OT_SIZE)
        return create_failed(&c, result, JB_ERROR_INVALID_ARGUMENT,
                             "jb_create: ot_size %d is outside 0..%d", c.ot_size,
                             JB_LIMIT_OT_SIZE);
    if (c.geometry != JB_GEOMETRY_GTE && c.geometry != JB_GEOMETRY_FLOAT)
        return create_failed(&c, result, JB_ERROR_INVALID_ARGUMENT,
                             "jb_create: geometry %d is neither JB_GEOMETRY_GTE (0) nor "
                             "JB_GEOMETRY_FLOAT (1)",
                             c.geometry);
    scale = c.scale > 1 ? c.scale : 1;

    jbi_config_default(&ic);
    ic.scale = scale;
    ic.threads = c.threads;
    ic.exact_1x = 1; /* 1x stays the console's at every scale */
    ic.log = c.log;
    ic.log_user = c.log_user;
    ic.alloc.alloc = c.alloc;
    ic.alloc.free = c.free;
    ic.alloc.user = c.alloc_user;
    jb = jbi_context_create(&ic);
    if (jb == NULL)
        return create_failed(&c, result, JB_ERROR_OUT_OF_MEMORY,
                             "jb_create: no memory for the context and its VRAM");
    if (jbi_scale(jb) != scale) {
        /* The backend fell back to 1x rather than fail. */
        jbi_context_destroy(jb);
        return create_failed(&c, result, JB_ERROR_OUT_OF_MEMORY,
                             "jb_create: no memory for the %dx scaled VRAM", scale);
    }
    a = jbi_calloc(&jb->alloc, 1, sizeof *a);
    if (a != NULL)
        a->rd_pix = jbi_calloc(&jb->alloc, (size_t)JBI_VRAM_W * JBI_VRAM_H, sizeof(uint16_t));
    if (a == NULL || a->rd_pix == NULL) {
        jbi_free(&jb->alloc, a);
        jbi_context_destroy(jb);
        return create_failed(&c, result, JB_ERROR_OUT_OF_MEMORY,
                             "jb_create: no memory for the read buffer");
    }
    a->precise = jbi_calloc(&jb->alloc, 1, jbi_precise_size());
    if (a->precise == NULL) {
        jbi_free(&jb->alloc, a->rd_pix);
        jbi_free(&jb->alloc, a);
        jbi_context_destroy(jb);
        return create_failed(&c, result, JB_ERROR_OUT_OF_MEMORY,
                             "jb_create: no memory for the GTE's vertex table");
    }
    jb->api = a;
    a->cfg_w = c.width > 0 ? c.width : 320;
    a->cfg_h = c.height > 0 ? c.height : 240;
    a->textures = jbi_textures_create(jb);
    if (a->textures == NULL) {
        jbi_free(&jb->alloc, a->precise);
        jbi_free(&jb->alloc, a->rd_pix);
        jbi_free(&jb->alloc, a);
        jb->api = NULL;
        jbi_context_destroy(jb);
        return create_failed(&c, result, JB_ERROR_OUT_OF_MEMORY,
                             "jb_create: no memory for the table of textures");
    }
    jbi_precise_init(a->precise);
    jbi_gte_init(&a->gte);
    jbi_scene_init(&a->scene);
    jbi_gte_set_precise(&a->gte, a->precise);
    a->precise_vertices = c.precise_vertices != 0;
    a->perspective_correct = c.perspective_correct != 0;
    a->threads = c.threads;
    a->log = c.log;
    a->log_user = c.log_user;
    a->cfg_w = c.width > 0 ? c.width : 320;
    a->cfg_h = c.height > 0 ? c.height : 240;
    jb->api = a;
    jbi_frame_init(jb, &c); /* may make cfg_w the 16:9 width */
    jbi_textures_mark_buffers(jb, a->buffer, a->nbuffers); /* the frame's, exactly */
    jbi_api_reset(jb);
    jbi_api_apply_options(jb);
    if (result != NULL)
        *result = JB_OK;
    return jb;
}

void jb_destroy(jb_context *jb)
{
    if (jb == NULL)
        return;
    jbi_meshes_destroy(jb);
    jbi_textures_destroy(jb);
    jbi_frame_free(jb);
    jbi_free(&jb->alloc, jb->api->frame);
    jbi_free(&jb->alloc, jb->api->rd_pix);
    jbi_free(&jb->alloc, jb->api->precise);
    jbi_free(&jb->alloc, jb->api);
    jb->api = NULL;
    jbi_context_destroy(jb);
}

/* ------------------------------------------------------------------------ */
/* The scale at run time: jbi_configure, which keeps the 1x VRAM and makes
   the scaled one again from it, or changes nothing if it can't. The dither
   setting is jb_create's, the default (-1). The threads: jbi_set_threads,
   which touches no pixel. */

enum { DITHER_DEFAULT = -1 };

jb_result jb_set_scale(jb_context *jb, int scale)
{
    int old, want;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (scale < 0)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_set_scale: scale %d is negative",
                            scale);
    if (scale > JBI_MAX_SCALE)
        return jbi_api_fail(jb, JB_ERROR_UNSUPPORTED,
                            "jb_set_scale: scale %d: the software backend draws at 1x to %dx",
                            scale, JBI_MAX_SCALE);
    want = scale > 1 ? scale : 1;
    old = jbi_scale(jb);
    if (want == old)
        return JB_OK;
    /* With the threads there are (jbi_threads), not those once asked for:
       a change of scale starts none. */
    if (jbi_configure(jb, want, DITHER_DEFAULT, jbi_threads(jb)) == want)
        return JB_OK;
    return jbi_api_fail(jb, JB_ERROR_OUT_OF_MEMORY,
                        "jb_set_scale: no memory for the %dx scaled VRAM; still drawing at %dx",
                        want, old);
}

jb_result jb_set_threads(jb_context *jb, int threads)
{
    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (threads < 0 || threads > MAX_THREADS)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_set_threads: threads %d is outside 0..%d", threads, MAX_THREADS);
    if (threads == jb->api->threads)
        return JB_OK;
    /* Fewer threads than asked for is logged by the backend, not an error;
       only a primitive queue it cannot get is (src/soft/soft.c). */
    if (jbi_set_threads(jb, threads) != 0) {
        jb->api->threads = threads;
        return JB_OK;
    }
    return jbi_api_fail(jb, JB_ERROR_OUT_OF_MEMORY,
                        "jb_set_threads: no memory for the primitive queue for %d threads",
                        threads);
}

/*
 * The context's GTE (jb_gte_*, include/jitterbug.h, Geometry) and the two
 * options that use its projections: precise vertices and
 * perspective-correct textures.
 *
 * The GTE is a jbi_gte (src/geom/gte.h) inside the API's state, and the
 * calls pass straight through to it: registers 0-31 are its data
 * registers, 32-63 its control registers. Commands it does not implement
 * are refused here rather than passed on (it would only clear FLAG).
 *
 * Its precise table (src/geom/precise.h) is always attached and is enabled,
 * so that RTPS and RTPT record into it, while either option is on. The
 * context's precise source (src/core/precise_source.h) is then this table,
 * with `enabled` reporting precise_vertices alone: the decoder takes
 * positions from it only then (and only above 1x), and depths for
 * perspective-correct texturing whenever the context's texture mapping is
 * perspective (jbi_set_texture_mapping). With both options off the context
 * has no precise source and affine textures, as it had before these
 * options existed, so it draws exactly as then.
 */
#include "api/api.h"

static int source_enabled(void *user)
{
    return ((const struct jbi_api *)user)->precise_vertices;
}

static int source_lookup(void *user, uint32_t sxy, jbi_precise_vertex *v)
{
    return jbi_precise_lookup(((const struct jbi_api *)user)->precise, sxy, v);
}

static unsigned source_lookup_poly(void *user, const uint32_t *sxy, int n, jbi_precise_vertex *v)
{
    return jbi_precise_lookup_poly(((const struct jbi_api *)user)->precise, sxy, n, v);
}

void jbi_api_apply_options(jb_context *jb)
{
    struct jbi_api *a = jb->api;
    int on = a->precise_vertices || a->perspective_correct;

    jbi_precise_enable(a->precise, on);
    if (on) {
        jbi_precise_source s;

        s.enabled = source_enabled;
        s.lookup = source_lookup;
        s.lookup_poly = source_lookup_poly;
        s.user = a;
        jbi_set_precise_source(jb, &s);
    } else {
        jbi_set_precise_source(jb, NULL);
    }
    if (jbi_texture_mapping(jb) != a->perspective_correct)
        jbi_set_texture_mapping(jb, a->perspective_correct);
}

jb_result jb_set_precise_vertices(jb_context *jb, int on)
{
    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    jb->api->precise_vertices = on != 0;
    jbi_api_apply_options(jb);
    return JB_OK;
}

jb_result jb_set_perspective_correct(jb_context *jb, int on)
{
    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    jb->api->perspective_correct = on != 0;
    jbi_api_apply_options(jb);
    return JB_OK;
}

/* ------------------------------------------------------------------------ */

static int bad_register(jb_context *jb, const char *fn, int reg)
{
    if (reg >= 0 && reg <= 63)
        return 0;
    jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: register %d is outside 0..63", fn, reg);
    return 1;
}

jb_result jb_gte_write(jb_context *jb, int reg, uint32_t value)
{
    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (bad_register(jb, "jb_gte_write", reg))
        return JB_ERROR_INVALID_ARGUMENT;
    if (reg < 32)
        jbi_gte_write_data(&jb->api->gte, reg, value);
    else
        jbi_gte_write_ctrl(&jb->api->gte, reg - 32, value);
    return JB_OK;
}

uint32_t jb_gte_read(jb_context *jb, int reg)
{
    if (jb == NULL || bad_register(jb, "jb_gte_read", reg))
        return 0;
    if (reg < 32)
        return jbi_gte_read_data(&jb->api->gte, reg);
    return jbi_gte_read_ctrl(&jb->api->gte, reg - 32);
}

/* The command numbers (bits 0-5) psx-spx documents. */
static int is_command(uint32_t op)
{
    switch (op) {
    case 0x01: case 0x06: case 0x0c: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
    case 0x16: case 0x1b: case 0x1c: case 0x1e: case 0x20: case 0x28: case 0x29: case 0x2a:
    case 0x2d: case 0x2e: case 0x30: case 0x3d: case 0x3e: case 0x3f:
        return 1;
    default:
        return 0;
    }
}

jb_result jb_gte_command(jb_context *jb, uint32_t command)
{
    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (command > 0x1ffffffu)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_gte_command: 0x%lx is more than the 25 bits of a command",
                            (unsigned long)command);
    if (!is_command(command & 0x3f))
        return jbi_api_fail(jb, JB_ERROR_UNSUPPORTED,
                            "jb_gte_command: 0x%lx: command number 0x%02x is not a GTE command",
                            (unsigned long)command, (unsigned)(command & 0x3f));
    jbi_gte_command(&jb->api->gte, command);
    return JB_OK;
}

jb_result jb_gte_begin_frame(jb_context *jb)
{
    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    /* Also with both options off, so that turning one on later in the
       frame does not find older records. */
    jbi_precise_begin_frame(jb->api->precise);
    return JB_OK;
}

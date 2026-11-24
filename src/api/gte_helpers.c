/*
 * The GTE helpers on a context (include/jitterbug.h, GTE helpers): each is
 * the sequence of register writes, commands and reads its comment in the
 * header gives, on the context's GTE, done with the same internal calls
 * jb_gte_write, jb_gte_command and jb_gte_read make (src/api/gte.c), so it
 * leaves the registers, and the precise-vertex records, exactly as that
 * sequence would. Arguments are checked first; a failing call writes
 * nothing.
 */
#include "api/api.h"

enum { MATRICES = JB_GTE_ROTATION | JB_GTE_LIGHT | JB_GTE_LIGHT_COLOR };
enum { VECTORS = JB_GTE_TRANSLATION | JB_GTE_BACKGROUND | JB_GTE_FAR_COLOR };

/* Register numbers as jb_gte_write takes them (0-31 data, 32-63 control). */
static void put(jbi_gte *g, int reg, uint32_t v)
{
    if (reg < 32)
        jbi_gte_write_data(g, reg, v);
    else
        jbi_gte_write_ctrl(g, reg - 32, v);
}

static uint32_t get(const jbi_gte *g, int reg)
{
    return reg < 32 ? jbi_gte_read_data(g, reg) : jbi_gte_read_ctrl(g, reg - 32);
}

static void put_matrix(jbi_gte *g, int first, const jb_gte_matrix *a)
{
    const int16_t(*m)[3] = a->m;

    put(g, first + 0, JB_GTE_PAIR(m[0][0], m[0][1]));
    put(g, first + 1, JB_GTE_PAIR(m[0][2], m[1][0]));
    put(g, first + 2, JB_GTE_PAIR(m[1][1], m[1][2]));
    put(g, first + 3, JB_GTE_PAIR(m[2][0], m[2][1]));
    put(g, first + 4, (uint32_t)(int32_t)m[2][2]);
}

static void put_vertex(jbi_gte *g, int n, const jb_gte_vec *v)
{
    put(g, JB_GTE_VXY0 + 2 * n, JB_GTE_PAIR(v->x, v->y));
    put(g, JB_GTE_VZ0 + 2 * n, (uint32_t)(int32_t)v->z);
}

/* Runs a command and ORs FLAG into *flags. */
static void run(jbi_gte *g, uint32_t cmd, uint32_t *flags)
{
    jbi_gte_command(g, cmd);
    *flags |= get(g, JB_GTE_FLAG);
}

jb_result jb_gte_set_matrix(jb_context *jb, int which, const jb_gte_matrix *m)
{
    jbi_gte *g;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (m == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "jb_gte_set_matrix: m is NULL");
    if (which == 0 || (which & ~MATRICES) != 0)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_gte_set_matrix: which is 0x%x, not JB_GTE_ROTATION, "
                            "JB_GTE_LIGHT and JB_GTE_LIGHT_COLOR ORed",
                            (unsigned)which);
    g = &jb->api->gte;
    if (which & JB_GTE_ROTATION)
        put_matrix(g, JB_GTE_RT11RT12, m);
    if (which & JB_GTE_LIGHT)
        put_matrix(g, JB_GTE_L11L12, m);
    if (which & JB_GTE_LIGHT_COLOR)
        put_matrix(g, JB_GTE_LR1LR2, m);
    return JB_OK;
}

jb_result jb_gte_set_vector(jb_context *jb, int which, int32_t x, int32_t y, int32_t z)
{
    static const int first[3] = {JB_GTE_TRX, JB_GTE_RBK, JB_GTE_RFC};
    jbi_gte *g;
    int i;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (which == 0 || (which & ~VECTORS) != 0)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_gte_set_vector: which is 0x%x, not JB_GTE_TRANSLATION, "
                            "JB_GTE_BACKGROUND and JB_GTE_FAR_COLOR ORed",
                            (unsigned)which);
    g = &jb->api->gte;
    for (i = 0; i < 3; i++) {
        if (!(which & (JB_GTE_TRANSLATION << i)))
            continue;
        put(g, first[i], (uint32_t)x);
        put(g, first[i] + 1, (uint32_t)y);
        put(g, first[i] + 2, (uint32_t)z);
    }
    return JB_OK;
}

jb_result jb_gte_project(jb_context *jb, const jb_gte_vec *v, size_t n, uint32_t *sxy,
                         uint16_t *sz, uint32_t *flag)
{
    uint32_t flags = 0;
    jbi_gte *g;
    size_t i = 0;
    int k;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (v == NULL && n > 0)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_gte_project: v is NULL but n is %lu", (unsigned long)n);
    g = &jb->api->gte;
    for (; n - i >= 3; i += 3) {
        for (k = 0; k < 3; k++)
            put_vertex(g, k, &v[i + k]);
        run(g, JB_GTE_RTPT, &flags);
        for (k = 0; k < 3; k++) {
            if (sxy != NULL)
                sxy[i + k] = get(g, JB_GTE_SXY0 + k);
            if (sz != NULL)
                sz[i + k] = (uint16_t)get(g, JB_GTE_SZ1 + k);
        }
    }
    for (; i < n; i++) {
        put_vertex(g, 0, &v[i]);
        run(g, JB_GTE_RTPS, &flags);
        if (sxy != NULL)
            sxy[i] = get(g, JB_GTE_SXY2);
        if (sz != NULL)
            sz[i] = (uint16_t)get(g, JB_GTE_SZ3);
    }
    if (flag != NULL)
        *flag = flags;
    return JB_OK;
}

jb_result jb_gte_light(jb_context *jb, int kind, const jb_gte_vec *normals, size_t n,
                       uint32_t color, uint32_t *rgb, uint32_t *flag)
{
    static const uint32_t triple[4] = {0, JB_GTE_NCT, JB_GTE_NCCT, JB_GTE_NCDT};
    static const uint32_t single[4] = {0, JB_GTE_NCS, JB_GTE_NCCS, JB_GTE_NCDS};
    uint32_t flags = 0;
    jbi_gte *g;
    size_t i = 0;
    int k;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    if (kind < JB_GTE_LIGHT_NC || kind > JB_GTE_LIGHT_NCD)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_gte_light: kind %d is not JB_GTE_LIGHT_NC, _NCC or _NCD", kind);
    if (color > 0xffffffu)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_gte_light: color 0x%lx is above 0xFFFFFF",
                            (unsigned long)color);
    if (normals == NULL && n > 0)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_gte_light: normals is NULL but n is %lu", (unsigned long)n);
    g = &jb->api->gte;
    put(g, JB_GTE_RGBC, color);
    for (; n - i >= 3; i += 3) {
        for (k = 0; k < 3; k++)
            put_vertex(g, k, &normals[i + k]);
        run(g, triple[kind], &flags);
        if (rgb != NULL)
            for (k = 0; k < 3; k++)
                rgb[i + k] = get(g, JB_GTE_RGB0 + k) & 0xffffffu;
    }
    for (; i < n; i++) {
        put_vertex(g, 0, &normals[i]);
        run(g, single[kind], &flags);
        if (rgb != NULL)
            rgb[i] = get(g, JB_GTE_RGB2) & 0xffffffu;
    }
    if (flag != NULL)
        *flag = flags;
    return JB_OK;
}

/* A colour channel (0..255) as a 4.12 light level: 255 is 1.0. */
static int16_t level(uint32_t c)
{
    return (int16_t)((c * 4096u + 127u) / 255u);
}

static jb_result check_lamps(jb_context *jb, const char *fn, const jb_gte_lamp *lamps, int n,
                             int colors)
{
    int i;

    if (n < 0 || n > 3)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: n %d is outside 0..3", fn, n);
    if (lamps == NULL && n > 0)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: lamps is NULL but n is %d", fn,
                            n);
    for (i = 0; colors && i < n; i++)
        if (lamps[i].color > 0xffffffu)
            return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                                "%s: lamps[%d].color 0x%lx is above 0xFFFFFF", fn, i,
                                (unsigned long)lamps[i].color);
    return JB_OK;
}

/* -c, saturated: -(-32768) is 32767. */
static int16_t towards(int16_t c)
{
    return (int16_t)(c == -32768 ? 32767 : -c);
}

/* The light matrix's rows: towards each lamp, the opposite of the way its
   light travels (jb_gte_lamp), 0 for absent lamps. */
static void directions(const jb_gte_lamp *lamps, int n, jb_gte_matrix *d)
{
    int i;

    for (i = 0; i < 3; i++) {
        d->m[i][0] = i < n ? towards(lamps[i].x) : 0;
        d->m[i][1] = i < n ? towards(lamps[i].y) : 0;
        d->m[i][2] = i < n ? towards(lamps[i].z) : 0;
    }
}

jb_result jb_gte_set_lights(jb_context *jb, const jb_gte_lamp *lamps, int n, uint32_t ambient)
{
    jb_gte_matrix d, c;
    jbi_gte *g;
    jb_result r;
    int i, ch;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    r = check_lamps(jb, "jb_gte_set_lights", lamps, n, 1);
    if (r != JB_OK)
        return r;
    if (ambient > 0xffffffu)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                            "jb_gte_set_lights: ambient 0x%lx is above 0xFFFFFF",
                            (unsigned long)ambient);
    directions(lamps, n, &d);
    for (ch = 0; ch < 3; ch++)
        for (i = 0; i < 3; i++)
            c.m[ch][i] = i < n ? level((lamps[i].color >> (8 * ch)) & 0xffu) : 0;
    g = &jb->api->gte;
    put_matrix(g, JB_GTE_L11L12, &d);
    put_matrix(g, JB_GTE_LR1LR2, &c);
    put(g, JB_GTE_RBK, (uint32_t)level(ambient & 0xffu));
    put(g, JB_GTE_GBK, (uint32_t)level((ambient >> 8) & 0xffu));
    put(g, JB_GTE_BBK, (uint32_t)level((ambient >> 16) & 0xffu));
    return JB_OK;
}

jb_result jb_gte_set_light_directions(jb_context *jb, const jb_gte_lamp *lamps, int n,
                                      const jb_gte_matrix *rotation)
{
    jb_gte_matrix d;
    jb_result r;

    if (jb == NULL)
        return JB_ERROR_INVALID_ARGUMENT;
    r = check_lamps(jb, "jb_gte_set_light_directions", lamps, n, 0);
    if (r != JB_OK)
        return r;
    directions(lamps, n, &d);
    if (rotation != NULL)
        jb_gte_mul_matrix(&d, rotation, &d);
    put_matrix(&jb->api->gte, JB_GTE_L11L12, &d);
    return JB_OK;
}

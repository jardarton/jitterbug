/*
 * api_gte_helpers: the GTE helpers and the fixed-point maths
 * (include/jitterbug.h, GTE helpers) against the register sequences the
 * header says each one is, written out by hand with jb_gte_write,
 * jb_gte_command and jb_gte_read on a second context. Public API only.
 *
 *   - Seeded runs: both contexts' 64 registers written with the same
 *     random values (small ones, now and then any 32 bits, so that
 *     saturation and overflow happen), then a random helper on one and its
 *     hand-written sequence on the other; the outputs and then all 64
 *     registers must agree. Every helper: jb_gte_set_matrix (every
 *     combination of matrices), jb_gte_set_vector, jb_gte_project (n 0..11,
 *     sxy and sz NULL or not), jb_gte_light (each kind), jb_gte_set_lights,
 *     jb_gte_set_light_directions (with and without a rotation).
 *   - jb_gte_nclip, jb_gte_avsz3, jb_gte_avsz4 against NCLIP, AVSZ3 and
 *     AVSZ4 on registers holding the same values (MAC0 and OTZ, FLAG),
 *     random and extreme.
 *   - jb_isin and jb_icos against the long double sine, rounded, at all
 *     4096 angles, and their symmetries for any int.
 *   - jb_gte_mul_matrix and jb_gte_mul_vector against MVMVA's IR1-3;
 *     jb_gte_rotation_matrix against its definition and known angles.
 *   - Errors: each refusal, with its detail, changing no register.
 */
#include "api_test.h"

#include <math.h>

static uint32_t gen_value(gen *g, int reg)
{
    int k = rnd_below(g, 8);

    if (k == 0)
        return rnd(g);
    if (reg == JB_GTE_H)
        return 100 + (uint32_t)rnd_below(g, 600);
    if (reg == JB_GTE_OFX || reg == JB_GTE_OFY)
        return (uint32_t)(rnd_below(g, 400) << 16);
    if (reg == JB_GTE_TRZ)
        return 200 + (uint32_t)rnd_below(g, 4000);
    if (reg == JB_GTE_RGBC)
        return rnd(g);
    if (reg == JB_GTE_IR0)
        return (uint32_t)rnd_below(g, 4097);
    return JB_GTE_PAIR(rnd_below(g, 8192) - 4096, rnd_below(g, 8192) - 4096);
}

static int16_t gen16(gen *g)
{
    int k = rnd_below(g, 8);

    if (k == 0)
        return (int16_t)rnd(g);
    if (k == 1)
        return (int16_t)(rnd_below(g, 2) ? 32767 : -32768);
    return (int16_t)(rnd_below(g, 8192) - 4096);
}

static void gen_matrix(gen *g, jb_gte_matrix *m)
{
    int r, c;

    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            m->m[r][c] = gen16(g);
}

static void gen_vec(gen *g, jb_gte_vec *v)
{
    v->x = gen16(g);
    v->y = gen16(g);
    v->z = gen16(g);
}

static uint32_t gen_color(gen *g)
{
    return rnd(g) & 0xffffffu;
}

/* A light matrix row from a lamp's direction, the way its light travels
   (the header's convention): the opposite, -(-32768) saturated. */
static int16_t towards(int16_t c)
{
    return (int16_t)(c == -32768 ? 32767 : -c);
}

static int same_registers(jb_context *a, jb_context *b, const char *what)
{
    int reg;
    char msg[160];

    for (reg = 0; reg < 64; reg++) {
        uint32_t x = jb_gte_read(a, reg), y = jb_gte_read(b, reg);

        if (x != y) {
            snprintf(msg, sizeof msg, "%s: register %d", what, reg);
            check(msg, x, y);
            return 0;
        }
    }
    ++api_t.checks;
    return 1;
}

static void hand_matrix(jb_context *jb, int first, const jb_gte_matrix *a)
{
    jb_gte_write(jb, first + 0, JB_GTE_PAIR(a->m[0][0], a->m[0][1]));
    jb_gte_write(jb, first + 1, JB_GTE_PAIR(a->m[0][2], a->m[1][0]));
    jb_gte_write(jb, first + 2, JB_GTE_PAIR(a->m[1][1], a->m[1][2]));
    jb_gte_write(jb, first + 3, JB_GTE_PAIR(a->m[2][0], a->m[2][1]));
    jb_gte_write(jb, first + 4, (uint32_t)(int32_t)a->m[2][2]);
}

static void hand_vertex(jb_context *jb, int n, const jb_gte_vec *v)
{
    jb_gte_write(jb, JB_GTE_VXY0 + 2 * n, JB_GTE_PAIR(v->x, v->y));
    jb_gte_write(jb, JB_GTE_VZ0 + 2 * n, (uint32_t)(int32_t)v->z);
}

static uint32_t hand_command(jb_context *jb, uint32_t cmd)
{
    check("hand command", jb_gte_command(jb, cmd), JB_OK);
    return jb_gte_read(jb, JB_GTE_FLAG);
}

enum { MAXN = 12 };

static void one_case(jb_context *a, jb_context *b, gen *g, int c)
{
    int kind = rnd_below(g, 6), n, i, k;
    char what[96];
    jb_gte_matrix m;
    jb_gte_vec v[MAXN];
    uint32_t sxy_a[MAXN], sxy_b[MAXN], rgb_a[MAXN], rgb_b[MAXN], fa = 0, fb = 0;
    uint16_t sz_a[MAXN], sz_b[MAXN];
    jb_gte_lamp lamps[3];

    memset(sxy_a, 0, sizeof sxy_a);
    memset(sxy_b, 0, sizeof sxy_b);
    memset(sz_a, 0, sizeof sz_a);
    memset(sz_b, 0, sizeof sz_b);
    memset(rgb_a, 0, sizeof rgb_a);
    memset(rgb_b, 0, sizeof rgb_b);
    n = rnd_below(g, MAXN);
    for (i = 0; i < n; i++)
        gen_vec(g, &v[i]);
    for (i = 0; i < 3; i++) {
        lamps[i].x = gen16(g);
        lamps[i].y = gen16(g);
        lamps[i].z = gen16(g);
        lamps[i].color = gen_color(g);
    }
    snprintf(what, sizeof what, "case %d kind %d n %d", c, kind, n);
    switch (kind) {
    case 0: { /* jb_gte_set_matrix */
        int which = 1 + rnd_below(g, 7);

        gen_matrix(g, &m);
        check(what, jb_gte_set_matrix(a, which, &m), JB_OK);
        if (which & JB_GTE_ROTATION)
            hand_matrix(b, JB_GTE_RT11RT12, &m);
        if (which & JB_GTE_LIGHT)
            hand_matrix(b, JB_GTE_L11L12, &m);
        if (which & JB_GTE_LIGHT_COLOR)
            hand_matrix(b, JB_GTE_LR1LR2, &m);
        break;
    }
    case 1: { /* jb_gte_set_vector */
        int which = (1 + rnd_below(g, 7)) << 3;
        int32_t x = (int32_t)rnd(g), y = (int32_t)rnd(g), z = (int32_t)rnd(g);
        static const int first[3] = {JB_GTE_TRX, JB_GTE_RBK, JB_GTE_RFC};

        check(what, jb_gte_set_vector(a, which, x, y, z), JB_OK);
        for (i = 0; i < 3; i++)
            if (which & (JB_GTE_TRANSLATION << i)) {
                jb_gte_write(b, first[i], (uint32_t)x);
                jb_gte_write(b, first[i] + 1, (uint32_t)y);
                jb_gte_write(b, first[i] + 2, (uint32_t)z);
            }
        break;
    }
    case 2: { /* jb_gte_project */
        int nulls = rnd_below(g, 4);

        check(what, jb_gte_project(a, v, (size_t)n, (nulls & 1) ? NULL : sxy_a,
                                   (nulls & 2) ? NULL : sz_a, &fa),
              JB_OK);
        for (i = 0; n - i >= 3; i += 3) {
            for (k = 0; k < 3; k++)
                hand_vertex(b, k, &v[i + k]);
            fb |= hand_command(b, JB_GTE_RTPT);
            for (k = 0; k < 3; k++) {
                sxy_b[i + k] = (nulls & 1) ? 0 : jb_gte_read(b, JB_GTE_SXY0 + k);
                sz_b[i + k] = (nulls & 2) ? 0 : (uint16_t)jb_gte_read(b, JB_GTE_SZ1 + k);
            }
        }
        for (; i < n; i++) {
            hand_vertex(b, 0, &v[i]);
            fb |= hand_command(b, JB_GTE_RTPS);
            sxy_b[i] = (nulls & 1) ? 0 : jb_gte_read(b, JB_GTE_SXY2);
            sz_b[i] = (nulls & 2) ? 0 : (uint16_t)jb_gte_read(b, JB_GTE_SZ3);
        }
        break;
    }
    case 3: { /* jb_gte_light */
        static const uint32_t triple[4] = {0, JB_GTE_NCT, JB_GTE_NCCT, JB_GTE_NCDT};
        static const uint32_t single[4] = {0, JB_GTE_NCS, JB_GTE_NCCS, JB_GTE_NCDS};
        int how = 1 + rnd_below(g, 3);
        uint32_t color = gen_color(g);

        check(what, jb_gte_light(a, how, v, (size_t)n, color, rgb_a, &fa), JB_OK);
        jb_gte_write(b, JB_GTE_RGBC, color);
        for (i = 0; n - i >= 3; i += 3) {
            for (k = 0; k < 3; k++)
                hand_vertex(b, k, &v[i + k]);
            fb |= hand_command(b, triple[how]);
            for (k = 0; k < 3; k++)
                rgb_b[i + k] = jb_gte_read(b, JB_GTE_RGB0 + k) & 0xffffffu;
        }
        for (; i < n; i++) {
            hand_vertex(b, 0, &v[i]);
            fb |= hand_command(b, single[how]);
            rgb_b[i] = jb_gte_read(b, JB_GTE_RGB2) & 0xffffffu;
        }
        break;
    }
    case 4: { /* jb_gte_set_lights */
        int count = rnd_below(g, 4), ch;
        uint32_t ambient = gen_color(g);
        jb_gte_matrix d, col;

        check(what, jb_gte_set_lights(a, lamps, count, ambient), JB_OK);
        for (i = 0; i < 3; i++) {
            d.m[i][0] = i < count ? towards(lamps[i].x) : 0;
            d.m[i][1] = i < count ? towards(lamps[i].y) : 0;
            d.m[i][2] = i < count ? towards(lamps[i].z) : 0;
            for (ch = 0; ch < 3; ch++)
                col.m[ch][i] = (int16_t)(i < count ? (((lamps[i].color >> (8 * ch)) & 255) *
                                                          4096 + 127) / 255
                                                   : 0);
        }
        hand_matrix(b, JB_GTE_L11L12, &d);
        hand_matrix(b, JB_GTE_LR1LR2, &col);
        jb_gte_write(b, JB_GTE_RBK, ((ambient & 255) * 4096 + 127) / 255);
        jb_gte_write(b, JB_GTE_GBK, (((ambient >> 8) & 255) * 4096 + 127) / 255);
        jb_gte_write(b, JB_GTE_BBK, (((ambient >> 16) & 255) * 4096 + 127) / 255);
        break;
    }
    default: { /* jb_gte_set_light_directions */
        int count = rnd_below(g, 4), rot = rnd_below(g, 2);
        jb_gte_matrix d;

        gen_matrix(g, &m);
        check(what, jb_gte_set_light_directions(a, lamps, count, rot ? &m : NULL), JB_OK);
        for (i = 0; i < 3; i++) {
            d.m[i][0] = i < count ? towards(lamps[i].x) : 0;
            d.m[i][1] = i < count ? towards(lamps[i].y) : 0;
            d.m[i][2] = i < count ? towards(lamps[i].z) : 0;
        }
        if (rot) /* products() checks jb_gte_mul_matrix against MVMVA */
            jb_gte_mul_matrix(&d, &m, &d);
        hand_matrix(b, JB_GTE_L11L12, &d);
        break;
    }
    }
    for (i = 0; i < n; i++) {
        check(what, sxy_a[i], sxy_b[i]);
        check(what, sz_a[i], sz_b[i]);
        check(what, rgb_a[i], rgb_b[i]);
    }
    check(what, fa, fb);
    same_registers(a, b, what);
}

static void seeded(uint64_t seed, int cases)
{
    jb_context *a = must_create(NULL), *b = must_create(NULL);
    gen g = {0, 0, 0};
    int c, reg;

    g.s = seed;
    for (c = 0; c < cases; c++) {
        if (c % 8 == 0)
            for (reg = 63; reg >= 0; reg--) {
                uint32_t v = gen_value(&g, reg);

                jb_gte_write(a, reg, v);
                jb_gte_write(b, reg, v);
            }
        one_case(a, b, &g, c);
    }
    printf("seeded: %d helper calls compared\n", cases);
    jb_destroy(a);
    jb_destroy(b);
}

/* ------------------------------------------------------------------------ */

static uint32_t gen_sxy(gen *g)
{
    int k = rnd_below(g, 4);

    if (k == 0)
        return rnd(g);
    if (k == 1)
        return JB_GTE_PAIR(rnd_below(g, 2) ? 32767 : -32768, rnd_below(g, 2) ? 32767 : -32768);
    return JB_GTE_PAIR(rnd_below(g, 2048) - 1024, rnd_below(g, 2048) - 1024);
}

static uint16_t gen_sz(gen *g)
{
    return (uint16_t)(rnd_below(g, 4) == 0 ? rnd(g) : (uint32_t)rnd_below(g, 4000));
}

static void pure(uint64_t seed, int cases)
{
    jb_context *jb = must_create(NULL);
    gen g = {0, 0, 0};
    int c;

    g.s = seed;
    for (c = 0; c < cases; c++) {
        uint32_t s0 = gen_sxy(&g), s1 = gen_sxy(&g), s2 = gen_sxy(&g), flag;
        uint16_t z0 = gen_sz(&g), z1 = gen_sz(&g), z2 = gen_sz(&g), z3 = gen_sz(&g);
        int16_t zsf = gen16(&g);
        int32_t mac0;
        uint32_t otz;

        jb_gte_write(jb, JB_GTE_SXY0, s0);
        jb_gte_write(jb, JB_GTE_SXY1, s1);
        jb_gte_write(jb, JB_GTE_SXY2, s2);
        mac0 = jb_gte_nclip(s0, s1, s2, &flag);
        jb_gte_command(jb, JB_GTE_NCLIP);
        check("nclip MAC0", mac0, (int32_t)jb_gte_read(jb, JB_GTE_MAC0));
        check("nclip FLAG", flag, jb_gte_read(jb, JB_GTE_FLAG));
        check("nclip without flag", jb_gte_nclip(s0, s1, s2, NULL), mac0);

        jb_gte_write(jb, JB_GTE_SZ0, z0);
        jb_gte_write(jb, JB_GTE_SZ1, z1);
        jb_gte_write(jb, JB_GTE_SZ2, z2);
        jb_gte_write(jb, JB_GTE_SZ3, z3);
        jb_gte_write(jb, JB_GTE_ZSF3, (uint32_t)(int32_t)zsf);
        jb_gte_write(jb, JB_GTE_ZSF4, (uint32_t)(int32_t)zsf);
        otz = jb_gte_avsz3(zsf, z1, z2, z3, &flag);
        jb_gte_command(jb, JB_GTE_AVSZ3);
        check("avsz3 OTZ", otz, jb_gte_read(jb, JB_GTE_OTZ));
        check("avsz3 FLAG", flag, jb_gte_read(jb, JB_GTE_FLAG));
        otz = jb_gte_avsz4(zsf, z0, z1, z2, z3, &flag);
        jb_gte_command(jb, JB_GTE_AVSZ4);
        check("avsz4 OTZ", otz, jb_gte_read(jb, JB_GTE_OTZ));
        check("avsz4 FLAG", flag, jb_gte_read(jb, JB_GTE_FLAG));
        check("avsz4 without flag", jb_gte_avsz4(zsf, z0, z1, z2, z3, NULL), otz);
    }
    /* Extremes: the largest NCLIP and AVSZ sums, both signs. */
    {
        uint32_t f;

        jb_gte_nclip(JB_GTE_PAIR(-32768, 32767), JB_GTE_PAIR(32767, -32768),
                     JB_GTE_PAIR(-32768, -32768), &f);
        check("nclip overflows", (f & JB_GTE_FLAG_ERROR) != 0, 1);
        check("avsz4 saturates high",
              jb_gte_avsz4(32767, 65535, 65535, 65535, 65535, &f), 0xffff);
        check("avsz4 flag", f, JB_GTE_FLAG_SZ_OTZ | JB_GTE_FLAG_ERROR | JB_GTE_FLAG_MAC0_POS);
        check("avsz3 saturates low", jb_gte_avsz3(-1, 1, 0, 0, &f), 0);
        check("avsz3 low flag", f, JB_GTE_FLAG_SZ_OTZ | JB_GTE_FLAG_ERROR);
        check("avsz3 plain", jb_gte_avsz3(1365, 1000, 1000, 1000, &f), 999);
        check("avsz3 plain flag", f, 0);
        check("nclip known", jb_gte_nclip(JB_GTE_PAIR(260, 170), JB_GTE_PAIR(60, 120),
                                          JB_GTE_PAIR(160, 70), &f),
              15000);
    }
    printf("pure: %d cases of NCLIP, AVSZ3, AVSZ4\n", cases);
    jb_destroy(jb);
}

/* ------------------------------------------------------------------------ */

static void trig(void)
{
    int a, bad = 0;

    for (a = 0; a < 4096; a++) {
        long double t = 2.0L * 3.14159265358979323846264338327950288L * (long double)a / 4096.0L;
        long double s = 4096.0L * sinl(t), c = 4096.0L * cosl(t);
        long rs = lroundl(s), rc = lroundl(c);

        if (jb_isin(a) != (int)rs || jb_icos(a) != (int)rc)
            bad++;
        if (bad == 1 && (jb_isin(a) != (int)rs || jb_icos(a) != (int)rc)) {
            check("jb_isin at", a, -1);
            bad++;
        }
    }
    check("jb_isin and jb_icos are the rounded sine and cosine", bad, 0);
    check("isin 0", jb_isin(0), 0);
    check("isin 1024", jb_isin(1024), 4096);
    check("isin 3072", jb_isin(3072), -4096);
    check("icos 2048", jb_icos(2048), -4096);
    check("isin 512", jb_isin(512), 2896);
    for (a = -100000; a < 100000; a += 37) {
        if (jb_isin(a) != jb_isin(a & 4095) || jb_icos(a) != jb_icos(a + 4096 * 3) ||
            jb_isin(-a) != -jb_isin(a) || jb_icos(-a) != jb_icos(a))
            bad++;
    }
    check("modulo and symmetry", bad, 0);
    check("isin INT_MIN", jb_isin(-2147483647 - 1), 0);
    check("icos INT_MAX", jb_icos(2147483647), jb_icos(4095));
}

/* MVMVA on a context: matrix m (as RT) times vector v, IR1-3. */
static void mvmva(jb_context *jb, const jb_gte_matrix *m, const jb_gte_vec *v, jb_gte_vec *out)
{
    jb_gte_set_matrix(jb, JB_GTE_ROTATION, m);
    hand_vertex(jb, 0, v);
    jb_gte_command(jb, JB_GTE_MVMVA | JB_GTE_SF | JB_GTE_MX_RT | JB_GTE_V_V0 | JB_GTE_CV_NONE);
    out->x = (int16_t)jb_gte_read(jb, JB_GTE_IR1);
    out->y = (int16_t)jb_gte_read(jb, JB_GTE_IR2);
    out->z = (int16_t)jb_gte_read(jb, JB_GTE_IR3);
}

static void products(uint64_t seed, int cases)
{
    jb_context *jb = must_create(NULL);
    gen g = {0, 0, 0};
    int c, r, k;

    g.s = seed;
    for (c = 0; c < cases; c++) {
        jb_gte_matrix a, b, p, p2;
        jb_gte_vec v, w, w2;

        gen_matrix(&g, &a);
        gen_matrix(&g, &b);
        gen_vec(&g, &v);
        jb_gte_mul_vector(&a, &v, &w);
        mvmva(jb, &a, &v, &w2);
        check("mul_vector x", w.x, w2.x);
        check("mul_vector y", w.y, w2.y);
        check("mul_vector z", w.z, w2.z);
        jb_gte_mul_matrix(&a, &b, &p);
        for (k = 0; k < 3; k++) {
            jb_gte_vec col, out;

            col.x = b.m[0][k];
            col.y = b.m[1][k];
            col.z = b.m[2][k];
            mvmva(jb, &a, &col, &out);
            check("mul_matrix column", p.m[0][k], out.x);
            check("mul_matrix column", p.m[1][k], out.y);
            check("mul_matrix column", p.m[2][k], out.z);
        }
        /* out may alias an input. */
        p2 = a;
        jb_gte_mul_matrix(&p2, &b, &p2);
        check("mul_matrix aliasing a", memcmp(&p2, &p, sizeof p), 0);
        p2 = b;
        jb_gte_mul_matrix(&a, &p2, &p2);
        check("mul_matrix aliasing b", memcmp(&p2, &p, sizeof p), 0);
        w2 = v;
        jb_gte_mul_vector(&a, &w2, &w2);
        check("mul_vector aliasing", memcmp(&w2, &w, sizeof w), 0);

        /* The rotation from its definition. */
        {
            int ax = (int)rnd(&g), ay = (int)rnd(&g), az = (int)rnd(&g);
            jb_gte_matrix rx = {{{4096, 0, 0}, {0, 0, 0}, {0, 0, 0}}};
            jb_gte_matrix ry = {{{0, 0, 0}, {0, 4096, 0}, {0, 0, 0}}};
            jb_gte_matrix rz = {{{0, 0, 0}, {0, 0, 0}, {0, 0, 4096}}};
            jb_gte_matrix want, got;

            rx.m[1][1] = (int16_t)jb_icos(ax);
            rx.m[1][2] = (int16_t)-jb_isin(ax);
            rx.m[2][1] = (int16_t)jb_isin(ax);
            rx.m[2][2] = (int16_t)jb_icos(ax);
            ry.m[0][0] = (int16_t)jb_icos(ay);
            ry.m[0][2] = (int16_t)jb_isin(ay);
            ry.m[2][0] = (int16_t)-jb_isin(ay);
            ry.m[2][2] = (int16_t)jb_icos(ay);
            rz.m[0][0] = (int16_t)jb_icos(az);
            rz.m[0][1] = (int16_t)-jb_isin(az);
            rz.m[1][0] = (int16_t)jb_isin(az);
            rz.m[1][1] = (int16_t)jb_icos(az);
            jb_gte_mul_matrix(&ry, &rx, &want);
            jb_gte_mul_matrix(&rz, &want, &want);
            jb_gte_rotation_matrix(ax, ay, az, &got);
            check("rotation matrix", memcmp(&want, &got, sizeof got), 0);
        }
    }
    /* Known angles: a quarter turn about z takes x to y. */
    {
        jb_gte_matrix m;
        jb_gte_vec x = {4096, 0, 0}, out;

        jb_gte_rotation_matrix(0, 0, 1024, &m);
        jb_gte_mul_vector(&m, &x, &out);
        check("z quarter: x to y", out.x == 0 && out.y == 4096 && out.z == 0, 1);
        jb_gte_rotation_matrix(1024, 0, 0, &m);
        x.x = 0;
        x.y = 4096;
        jb_gte_mul_vector(&m, &x, &out);
        check("x quarter: y to z", out.x == 0 && out.y == 0 && out.z == 4096, 1);
        jb_gte_rotation_matrix(0, 1024, 0, &m);
        x.y = 0;
        x.z = 4096;
        jb_gte_mul_vector(&m, &x, &out);
        check("y quarter: z to x", out.x == 4096 && out.y == 0 && out.z == 0, 1);
        jb_gte_rotation_matrix(0, 0, 0, &m);
        for (r = 0; r < 3; r++)
            for (k = 0; k < 3; k++)
                check("zero angles: identity", m.m[r][k], r == k ? 4096 : 0);
        /* NULL writes nothing. */
        jb_gte_rotation_matrix(1, 2, 3, NULL);
        jb_gte_mul_matrix(NULL, &m, &m);
        jb_gte_mul_vector(&m, NULL, &out);
        check("NULL writes nothing", m.m[0][0], 4096);
    }
    printf("products: %d cases\n", cases);
    jb_destroy(jb);
}

static void errors(void)
{
    jb_context *jb = must_create(NULL), *ref = must_create(NULL);
    jb_gte_matrix m;
    jb_gte_vec v = {1, 2, 3};
    jb_gte_lamp lamp = {4096, 0, 0, 0x1000000u};
    uint32_t out[4];
    int reg;

    memset(&m, 0, sizeof m);
    for (reg = 0; reg < 64; reg++) {
        jb_gte_write(jb, reg, 0x1234u * (uint32_t)reg);
        jb_gte_write(ref, reg, 0x1234u * (uint32_t)reg);
    }
    check("set_matrix NULL jb", jb_gte_set_matrix(NULL, JB_GTE_ROTATION, &m),
          JB_ERROR_INVALID_ARGUMENT);
    check("set_matrix NULL m", jb_gte_set_matrix(jb, JB_GTE_ROTATION, NULL),
          JB_ERROR_INVALID_ARGUMENT);
    check_str("set_matrix NULL m: detail", jb_error_string(jb), "jb_gte_set_matrix: m is NULL");
    check("set_matrix which 0", jb_gte_set_matrix(jb, 0, &m), JB_ERROR_INVALID_ARGUMENT);
    check("set_matrix a vector", jb_gte_set_matrix(jb, JB_GTE_TRANSLATION, &m),
          JB_ERROR_INVALID_ARGUMENT);
    check_str("set_matrix which: detail", jb_error_string(jb),
              "jb_gte_set_matrix: which is 0x8, not JB_GTE_ROTATION, JB_GTE_LIGHT and "
              "JB_GTE_LIGHT_COLOR ORed");
    check("set_vector a matrix", jb_gte_set_vector(jb, JB_GTE_ROTATION, 1, 2, 3),
          JB_ERROR_INVALID_ARGUMENT);
    check("set_vector 0", jb_gte_set_vector(jb, 0, 1, 2, 3), JB_ERROR_INVALID_ARGUMENT);
    check("set_vector NULL", jb_gte_set_vector(NULL, JB_GTE_TRANSLATION, 1, 2, 3),
          JB_ERROR_INVALID_ARGUMENT);
    check("project NULL v", jb_gte_project(jb, NULL, 3, out, NULL, NULL),
          JB_ERROR_INVALID_ARGUMENT);
    check_str("project NULL v: detail", jb_error_string(jb),
              "jb_gte_project: v is NULL but n is 3");
    check("project NULL jb", jb_gte_project(NULL, &v, 1, out, NULL, NULL),
          JB_ERROR_INVALID_ARGUMENT);
    check("light kind 0", jb_gte_light(jb, 0, &v, 1, 0, out, NULL), JB_ERROR_INVALID_ARGUMENT);
    check("light kind 4", jb_gte_light(jb, 4, &v, 1, 0, out, NULL), JB_ERROR_INVALID_ARGUMENT);
    check("light colour", jb_gte_light(jb, JB_GTE_LIGHT_NCC, &v, 1, 0x1000000u, out, NULL),
          JB_ERROR_INVALID_ARGUMENT);
    check_str("light colour: detail", jb_error_string(jb),
              "jb_gte_light: color 0x1000000 is above 0xFFFFFF");
    check("light NULL normals", jb_gte_light(jb, JB_GTE_LIGHT_NC, NULL, 2, 0, out, NULL),
          JB_ERROR_INVALID_ARGUMENT);
    check("lights n 4", jb_gte_set_lights(jb, &lamp, 4, 0), JB_ERROR_INVALID_ARGUMENT);
    check("lights n -1", jb_gte_set_lights(jb, &lamp, -1, 0), JB_ERROR_INVALID_ARGUMENT);
    check("lights NULL", jb_gte_set_lights(jb, NULL, 1, 0), JB_ERROR_INVALID_ARGUMENT);
    check("lights colour", jb_gte_set_lights(jb, &lamp, 1, 0), JB_ERROR_INVALID_ARGUMENT);
    check_str("lights colour: detail", jb_error_string(jb),
              "jb_gte_set_lights: lamps[0].color 0x1000000 is above 0xFFFFFF");
    check("lights ambient", jb_gte_set_lights(jb, NULL, 0, 0x1000000u),
          JB_ERROR_INVALID_ARGUMENT);
    check("directions n 4", jb_gte_set_light_directions(jb, &lamp, 4, NULL),
          JB_ERROR_INVALID_ARGUMENT);
    check("directions NULL jb", jb_gte_set_light_directions(NULL, &lamp, 1, NULL),
          JB_ERROR_INVALID_ARGUMENT);
    same_registers(jb, ref, "refused calls change nothing");
    check("project n 0", jb_gte_project(jb, NULL, 0, NULL, NULL, NULL), JB_OK);
    check("light n 0", jb_gte_light(jb, JB_GTE_LIGHT_NC, NULL, 0, 0, NULL, NULL), JB_OK);
    check("lights n 0", jb_gte_set_lights(jb, NULL, 0, 0), JB_OK);
    /* A colour above 0xFFFFFF is no error for the directions alone. */
    check("directions ignore colours", jb_gte_set_light_directions(jb, &lamp, 1, NULL), JB_OK);
    jb_destroy(jb);
    jb_destroy(ref);
}

/* The header's scale: a normal straight at one white lamp, no ambient,
   gives NCC the colour unchanged; NC 255. */
static void light_scale(void)
{
    jb_context *jb = must_create(NULL);
    /* Light travelling down -z, so it falls on a normal facing +z. */
    jb_gte_lamp lamp = {0, 0, -4096, JB_RGB(255, 255, 255)};
    jb_gte_vec n = {0, 0, 4096};
    uint32_t rgb;

    jb_gte_set_lights(jb, &lamp, 1, 0);
    jb_gte_light(jb, JB_GTE_LIGHT_NCC, &n, 1, JB_RGB(128, 128, 128), &rgb, NULL);
    check("NCC with one white lamp: unchanged", rgb, JB_RGB(128, 128, 128));
    jb_gte_light(jb, JB_GTE_LIGHT_NCC, &n, 1, JB_RGB(10, 200, 77), &rgb, NULL);
    check("NCC: any colour unchanged", rgb, JB_RGB(10, 200, 77));
    jb_gte_light(jb, JB_GTE_LIGHT_NC, &n, 1, 0, &rgb, NULL);
    check("NC: 255", rgb, JB_RGB(255, 255, 255));
    n.z = -4096;
    jb_gte_light(jb, JB_GTE_LIGHT_NCC, &n, 1, JB_RGB(128, 128, 128), &rgb, NULL);
    check("NCC from behind: dark", rgb, 0);
    jb_gte_set_lights(jb, &lamp, 1, JB_RGB(255, 255, 255));
    jb_gte_light(jb, JB_GTE_LIGHT_NCC, &n, 1, JB_RGB(128, 128, 128), &rgb, NULL);
    check("NCC ambient alone: unchanged", rgb, JB_RGB(128, 128, 128));
    n.z = 4096;
    jb_gte_light(jb, JB_GTE_LIGHT_NCC, &n, 1, JB_RGB(128, 128, 128), &rgb, NULL);
    check("NCC ambient and lamp: doubled", rgb, JB_RGB(255, 255, 255));
    jb_destroy(jb);
}

int main(void)
{
    seeded(1, 6000);
    seeded(2, 3000);
    pure(3, 20000);
    trig();
    products(4, 5000);
    errors();
    light_scale();
    return finish("api_gte_helpers");
}

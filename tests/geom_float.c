/*
 * geom_float: the library's float maths (include/jitterbug.h, Scene
 * geometry). Public API only; the C library's long double functions are
 * the reference here, never part of the library.
 *
 *   - jb_sinf, jb_cosf, jb_tanf within one ulp of sinl, cosl and tanl
 *     rounded to float: a dense sweep of -20..20, the floats nearest
 *     multiples of pi/2 (and their neighbours) up to 2^24, random bit
 *     patterns over every finite float (the large ones reduced from the
 *     bits of 2/pi), subnormals; NaN for infinities and NaN; odd and even
 *     symmetry. The count off the correctly rounded float is printed.
 *   - Stored bit patterns: an FNV-1a hash of the three functions' bits
 *     over fixed inputs, and of the matrix helpers' results, compared with
 *     the values below, so every build (compilers, -O levels, x86-64-v3,
 *     aarch64, wasm32) must give the same bits.
 *   - The vector and matrix helpers' semantics: layout, products, the
 *     rotations' directions, look_at, TRS, NULL as the identity.
 */
#include "api_test.h"

#include <float.h>
#include <math.h>

/* The stored hashes (re-record: the test prints what it got). */
#define TRIG_HASH 0xc7365f5b08e90c9cull
#define MATRIX_HASH 0xcbd4acdab21656a1ull

static uint32_t bits_of(float f)
{
    uint32_t b;

    memcpy(&b, &f, sizeof b);
    return b;
}

static float from_bits(uint32_t b)
{
    float f;

    memcpy(&f, &b, sizeof f);
    return f;
}

/* Distance in ulps between two finite floats. */
static long long ulps(float a, float b)
{
    long long x = (long long)bits_of(a), y = (long long)bits_of(b);

    if (x & 0x80000000ll)
        x = 0x80000000ll - x;
    if (y & 0x80000000ll)
        y = 0x80000000ll - y;
    return x > y ? x - y : y - x;
}

static struct {
    long long count, off, bad;
    float worst_x;
    const char *worst_fn;
} acc;

static void compare(const char *fn, float x, float got, long double ref)
{
    float want = (float)ref;
    long long d;

    acc.count++;
    if (got != got || want != want) {
        if (!(got != got && want != want)) {
            acc.bad++;
            if (acc.bad < 10)
                printf("FAIL %s(%a): got %a, want %a\n", fn, (double)x, (double)got,
                       (double)want);
        }
        return;
    }
    d = ulps(got, want);
    if (d == 0)
        return;
    acc.off++;
    if (d > 1) {
        acc.bad++;
        if (acc.bad < 10)
            printf("FAIL %s(%a): got %a, want %a (%lld ulps)\n", fn, (double)x, (double)got,
                   (double)want, d);
    }
}

static void one(float x)
{
    long double lx = (long double)x;

    compare("jb_sinf", x, jb_sinf(x), sinl(lx));
    compare("jb_cosf", x, jb_cosf(x), cosl(lx));
    compare("jb_tanf", x, jb_tanf(x), tanl(lx));
}

static uint64_t fnv(uint64_t h, uint32_t v)
{
    int i;

    for (i = 0; i < 4; i++) {
        h ^= (v >> (8 * i)) & 0xffu;
        h *= 0x100000001b3ull;
    }
    return h;
}

static void accuracy(void)
{
    gen g = {12345, 0, 0};
    int i, k;
    const long double pio2 = 1.57079632679489661923132169163975144L;

    for (i = -50000; i <= 50000; i++)
        one((float)i * 0.0004f);
    for (k = 1; k < (1 << 24); k += 1 + k / 64) {
        float x = (float)((long double)k * pio2);
        uint32_t b = bits_of(x);
        int d;

        for (d = -2; d <= 2; d++) {
            one(from_bits(b + (uint32_t)d));
            one(-from_bits(b + (uint32_t)d));
        }
    }
    for (i = 0; i < 100000; i++) {
        uint32_t b = rnd(&g);

        if (((b >> 23) & 0xff) == 0xff)
            continue;
        one(from_bits(b));
    }
    for (i = 0; i < 1000; i++)
        one(from_bits((uint32_t)i * 8191u));
    printf("trig: %lld values, %lld off the correctly rounded float by one ulp, %lld more\n",
           acc.count, acc.off, acc.bad);
    check("trig within one ulp", acc.bad, 0);
    check("sin(inf) is NaN", jb_sinf(INFINITY) != jb_sinf(INFINITY), 1);
    check("cos(-inf) is NaN", jb_cosf(-INFINITY) != jb_cosf(-INFINITY), 1);
    check("tan(NaN) is NaN", jb_tanf(NAN) != jb_tanf(NAN), 1);
    check("sin(0)", bits_of(jb_sinf(0.0f)), 0);
    check("sin(-0) keeps the sign", bits_of(jb_sinf(-0.0f)), 0x80000000u);
    check("cos(0)", jb_cosf(0.0f) == 1.0f, 1);
    check("tan(-0) keeps the sign", bits_of(jb_tanf(-0.0f)), 0x80000000u);
    for (i = 0; i < 20000; i++) {
        float x = from_bits(rnd(&g) & 0x7f7fffffu);

        if (jb_sinf(-x) != -jb_sinf(x) || jb_cosf(-x) != jb_cosf(x) ||
            jb_tanf(-x) != -jb_tanf(x)) {
            check("odd and even", 0, 1);
            break;
        }
    }
}

static void stored_bits(void)
{
    uint64_t h = 0xcbf29ce484222325ull;
    gen g = {777, 0, 0};
    int i;

    for (i = 0; i < 20000; i++) {
        float x = i < 10000 ? (float)(i - 5000) * 0.00731f : from_bits(rnd(&g) & 0xff7fffffu);

        h = fnv(h, bits_of(jb_sinf(x)));
        h = fnv(h, bits_of(jb_cosf(x)));
        h = fnv(h, bits_of(jb_tanf(x)));
    }
    if (!check("stored trig bits", (long long)h, (long long)TRIG_HASH))
        printf("trig hash: 0x%016llxull\n", (unsigned long long)h);

    h = 0xcbf29ce484222325ull;
    for (i = 0; i < 500; i++) {
        jb_vec3 t, r, s, e, tg, up = {0.0f, 1.0f, 0.0f}, p;
        jb_mat4 a, b, c;
        int k;

        t.x = (float)rnd_below(&g, 2000) - 1000.0f;
        t.y = (float)rnd_below(&g, 2000) * 0.37f;
        t.z = (float)rnd_below(&g, 2000) * -0.11f;
        r.x = (float)rnd_below(&g, 6283) * 0.001f;
        r.y = (float)rnd_below(&g, 6283) * -0.002f;
        r.z = (float)rnd_below(&g, 6283) * 0.003f;
        s.x = 0.5f + (float)rnd_below(&g, 100) * 0.01f;
        s.y = s.x * 1.5f;
        s.z = 1.0f / s.x;
        e.x = t.z;
        e.y = 10.0f + t.y;
        e.z = t.x;
        tg = r;
        a = jb_mat4_trs(t, r, s);
        b = jb_mat4_look_at(e, tg, up);
        c = jb_mat4_mul(&b, &a);
        p = jb_mat4_transform_point(&c, s);
        p = jb_vec3_normalize(jb_vec3_cross(p, t));
        for (k = 0; k < 16; k++)
            h = fnv(h, bits_of(c.m[k]));
        h = fnv(h, bits_of(p.x));
        h = fnv(h, bits_of(p.y));
        h = fnv(h, bits_of(p.z));
        h = fnv(h, bits_of(jb_vec3_dot(p, e)));
    }
    if (!check("stored matrix bits", (long long)h, (long long)MATRIX_HASH))
        printf("matrix hash: 0x%016llxull\n", (unsigned long long)h);
}

static int near(float a, float b)
{
    float d = a - b;

    return d < 1e-5f && d > -1e-5f;
}

static int near_point(jb_vec3 p, float x, float y, float z)
{
    return near(p.x, x) && near(p.y, y) && near(p.z, z);
}

static void matrices(void)
{
    jb_mat4 i = jb_mat4_identity(), m, n;
    jb_vec3 x = {1, 0, 0}, y = {0, 1, 0}, z = {0, 0, 1}, p = {1, 2, 3}, zero = {0, 0, 0}, q;
    const float q4 = 1.5707964f;
    int k;

    for (k = 0; k < 16; k++)
        check("identity", i.m[k] == (k % 5 == 0 ? 1.0f : 0.0f), 1);
    m = jb_mat4_translate(4, 5, 6);
    check("translation in m[12..14]", m.m[12] == 4 && m.m[13] == 5 && m.m[14] == 6, 1);
    check("translate a point", near_point(jb_mat4_transform_point(&m, p), 5, 7, 9), 1);
    check("NULL transforms nothing", near_point(jb_mat4_transform_point(NULL, p), 1, 2, 3), 1);
    m = jb_mat4_rotate_x(q4);
    check("rotate_x: y to z", near_point(jb_mat4_transform_point(&m, y), 0, 0, 1), 1);
    m = jb_mat4_rotate_y(q4);
    check("rotate_y: z to x", near_point(jb_mat4_transform_point(&m, z), 1, 0, 0), 1);
    m = jb_mat4_rotate_z(q4);
    check("rotate_z: x to y", near_point(jb_mat4_transform_point(&m, x), 0, 1, 0), 1);
    m = jb_mat4_scale(2, 3, 4);
    check("scale", near_point(jb_mat4_transform_point(&m, p), 2, 6, 12), 1);
    /* b applies first. */
    {
        jb_mat4 t = jb_mat4_translate(1, 0, 0), s = jb_mat4_scale(2, 2, 2);

        m = jb_mat4_mul(&t, &s);
        check("mul: b first", near_point(jb_mat4_transform_point(&m, x), 3, 0, 0), 1);
        n = jb_mat4_mul(NULL, &s);
        check("mul: NULL a", memcmp(&n, &s, sizeof n), 0);
        n = jb_mat4_mul(&t, NULL);
        check("mul: NULL b", memcmp(&n, &t, sizeof n), 0);
        n = jb_mat4_mul(NULL, NULL);
        check("mul: NULL both", memcmp(&n, &i, sizeof n), 0);
    }
    /* TRS: scaled, then x, y, z, then moved. */
    {
        jb_vec3 t = {10, 20, 30}, r = {0.3f, -1.1f, 2.0f}, s = {2, 3, 0.5f};
        jb_mat4 a = jb_mat4_trs(t, r, s), b, c;

        b = jb_mat4_scale(s.x, s.y, s.z);
        c = jb_mat4_rotate_x(r.x);
        b = jb_mat4_mul(&c, &b);
        c = jb_mat4_rotate_y(r.y);
        b = jb_mat4_mul(&c, &b);
        c = jb_mat4_rotate_z(r.z);
        b = jb_mat4_mul(&c, &b);
        c = jb_mat4_translate(t.x, t.y, t.z);
        b = jb_mat4_mul(&c, &b);
        q = jb_mat4_transform_point(&a, p);
        check("trs", near_point(q, jb_mat4_transform_point(&b, p).x,
                                jb_mat4_transform_point(&b, p).y,
                                jb_mat4_transform_point(&b, p).z),
              1);
    }
    /* look_at: the camera at the origin looking down -z. */
    {
        jb_vec3 eye = {0, 0, 5}, target = {0, 0, 0}, up = {0, 1, 0}, right = {3, 0, 0};

        m = jb_mat4_look_at(eye, target, up);
        check("look_at: target ahead", near_point(jb_mat4_transform_point(&m, target), 0, 0, -5),
              1);
        check("look_at: x right", near_point(jb_mat4_transform_point(&m, right), 3, 0, -5), 1);
        eye.x = 5;
        eye.z = 0;
        m = jb_mat4_look_at(eye, target, up);
        check("look_at from +x: target ahead",
              near_point(jb_mat4_transform_point(&m, target), 0, 0, -5), 1);
        check("look_at from +x: -z is right",
              near_point(jb_mat4_transform_point(&m, jb_vec3_cross(up, x)), 1, 0, -5), 1);
        n = jb_mat4_look_at(eye, eye, up);
        check("look_at: eye at target", memcmp(&n, &i, sizeof n), 0);
        n = jb_mat4_look_at(zero, y, y);
        check("look_at: up parallel", memcmp(&n, &i, sizeof n), 0);
    }
    q = jb_vec3_cross(x, y);
    check("x cross y is z", near_point(q, 0, 0, 1), 1);
    check("dot", jb_vec3_dot(p, p) == 14.0f, 1);
    q = jb_vec3_normalize(zero);
    check("normalize zero", near_point(q, 0, 0, 0), 1);
    q.x = 3;
    q.y = 0;
    q.z = 4;
    q = jb_vec3_normalize(q);
    check("normalize", near_point(q, 0.6f, 0, 0.8f), 1);
    q.x = 3e38f;
    q.y = 3e38f;
    q.z = 0;
    q = jb_vec3_normalize(q);
    check("normalize where the squared length overflows", near_point(q, 0.70710677f, 0.70710677f, 0), 1);
}

/* Edge cases: normalising vectors whose squared length leaves the float
   range, the floating-point environment the bit guarantee needs and the
   one NaN the trigonometry returns, and look_at with an up exactly
   parallel to the view, off the axes. */
static void edge_cases(void)
{
    volatile float one = 1.0f, tiny = 0x1p-126f, half_ulp = 0x1p-24f;
    jb_vec3 big = {1e30f, 0.0f, 0.0f}, small = {1e-30f, 1e-30f, 0.0f}, r;
    jb_vec3 eye = {0, 0, 0}, dir = {2, 3, 4};
    jb_mat4 m = jb_mat4_look_at(eye, dir, dir), id = jb_mat4_identity();

    r = jb_vec3_normalize(big);
    check("edge cases: normalize (1e30, 0, 0)", r.x == 1.0f && r.y == 0.0f && r.z == 0.0f, 1);
    r = jb_vec3_normalize(small);
    check("edge cases: normalize (1e-30, 1e-30, 0)", r.x > 0.7071f && r.x < 0.70711f && r.x == r.y,
          1);
    check("edge cases: look_at with up parallel, off the axes: identity",
          memcmp(&m, &id, sizeof m), 0);
    check("edge cases: jb_sinf(inf) is the canonical NaN", bits_of(jb_sinf(INFINITY)), 0x7fc00000u);
    check("edge cases: jb_cosf(-inf) is the canonical NaN", bits_of(jb_cosf(-INFINITY)),
          0x7fc00000u);
    check("edge cases: jb_tanf(NaN) is the canonical NaN", bits_of(jb_tanf(-NAN)), 0x7fc00000u);
    /* The environment the bit guarantee assumes (docs/determinism.md):
       round to nearest, ties to even, and subnormals kept. */
    check("edge cases: rounding to nearest, ties to even", one + half_ulp == 1.0f &&
                                                       one + half_ulp * 3.0f == 1.0f + 0x1p-22f,
          1);
    check("edge cases: subnormals not flushed to zero", tiny * 0.5f != 0.0f, 1);
    {
        /* A subnormal input must not be read as 0 (denormals-are-
           zero), and double rounds to nearest too. */
        volatile float sub = 0x1p-140f;
        volatile double done = 1.0, dhalf = 0x1p-53;
        jb_vec3 nan_y = {1.0f, NAN, 2.0f}, back;

        check("edge cases: subnormal inputs kept", sub * 2.0f == 0x1p-139f, 1);
        check("edge cases: double rounds to nearest, ties to even",
              done + dhalf == 1.0 && done + dhalf * 3.0 == 1.0 + 0x1p-51, 1);
        /* A NaN in y or z leaves the vector as it was. */
        back = jb_vec3_normalize(nan_y);
        check("edge cases: (1, NaN, 2) unchanged",
              back.x == 1.0f && back.y != back.y && back.z == 2.0f,
              1);
    }
}

int main(void)
{
    check("FLT_EVAL_METHOD", FLT_EVAL_METHOD, 0);
    edge_cases();
    accuracy();
    stored_bits();
    matrices();
    return finish("geom_float");
}

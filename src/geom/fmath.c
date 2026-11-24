/*
 * The friendly level's float maths (include/jitterbug.h, Scene geometry):
 * the library's own sine, cosine and tangent, and the vector and matrix
 * helpers.
 *
 * Determinism. Everything here is + - x / and sqrtf, in float or double,
 * each correctly rounded by IEEE 754, so the same inputs give the same bits
 * on every platform the library builds for, provided that the compiler
 * neither fuses multiply-adds (the build passes -ffp-contract=off) nor
 * evaluates at a wider precision than the type (src/core/assumptions.c
 * requires FLT_EVAL_METHOD 0). Expressions are written with the order of
 * evaluation the results depend on, one operation at a time where it
 * matters, and nothing calls the C library's sin, cos or tan.
 *
 * The trigonometric functions, from the definitions (no code was copied):
 *   - Argument reduction: x = k pi/2 + r with |r| <= about pi/4, and the
 *     quadrant k mod 4.
 *       |x| < 2^23: k = nearest(x 2/pi); r = ((x - k C1) - k C2) - k C3,
 *       with pi/2 = C1 + C2 + C3, C1 and C2 of 30 significant bits each, so
 *       k C1 and k C2 are exact for k < 2^23 and x - k C1 is exact (both
 *       are multiples of 2^-29 and the difference is below 1); the error is
 *       about 2^-90, far below any r a float can come to.
 *       |x| >= 2^23: x = m 2^E with m the 24-bit significand and E >= 0, and
 *       x 2/pi mod 4 = (m W mod 2^96) 2^-94 where W is the 96 bits of 2/pi
 *       from 2^-(E-1) on (the bits above contribute multiples of 4). The
 *       integer part is the quadrant, the 94-bit fraction r 2/pi.
 *   - sin r and cos r by their Taylor series to r^17 and r^18 in double
 *     (the next terms are below 1e-19), tan r as their quotient, then
 *     rounded once to float.
 */
#include <jitterbug.h>

#include <stdint.h>
#include <string.h>

#include "geom/fmath.h"

/* pi/2 in three parts (see above), and 2/pi, as exact hex constants. */
static const double pio2_1 = 0x1.921fb548p+0;
static const double pio2_2 = -0x1.de973dc8p-31;
static const double pio2_3 = -0x1.9d9cceba3f91fp-62;
static const double pio2 = 0x1.921fb54442d18p+0;
static const double two_over_pi = 0x1.45f306dc9c883p-1;

/* The bits of 2/pi after the binary point, most significant first, after a
   word of zeros so that windows may start before the point. */
static const uint32_t bits_2_over_pi[11] = {
    0x00000000u, 0xA2F9836Eu, 0x4E441529u, 0xFC2757D1u, 0xF534DDC0u, 0xDB629599u,
    0x3C439041u, 0xFE5163ABu, 0xDEBBC561u, 0xB7246E3Au, 0x424DD2E0u,
};

/* The 32 bits b_i ... b_(i+31) of 2/pi (2/pi = sum of b_i 2^-i, i >= 1;
   b_i = 0 for i < 1), for i >= -31. */
static uint32_t window32(int i)
{
    int q = i + 31, a = q / 32, o = q % 32;

    if (o == 0)
        return bits_2_over_pi[a];
    return bits_2_over_pi[a] << o | bits_2_over_pi[a + 1] >> (32 - o);
}

/* r (radians, |r| <= about pi/4) and the quadrant for |x| >= 2^23, x
   finite. */
static int reduce_large(uint32_t bits, double *r)
{
    int E = (int)((bits >> 23) & 0xff) - 127 - 23;
    uint64_t m = (bits & 0x7fffffu) | 0x800000u;
    uint64_t w0 = window32(E - 1), w1 = window32(E + 31), w2 = window32(E + 63);
    uint64_t p, l0, l1, l2, hi;
    int q;

    p = m * w2;
    l0 = p & 0xffffffffu;
    p = m * w1 + (p >> 32);
    l1 = p & 0xffffffffu;
    p = m * w0 + (p >> 32);
    l2 = p & 0xffffffffu;
    q = (int)(l2 >> 30);
    hi = (l2 & 0x3fffffffu) << 32 | l1; /* the fraction's bits 93..32 */
    if (hi >> 61) {
        /* Fraction >= 1/2: take 1 - fraction, exactly, for the next
           quadrant. */
        uint64_t nlo = (0x100000000u - l0) & 0xffffffffu;
        uint64_t nhi = ((uint64_t)1 << 62) - hi - (l0 != 0);
        double f = (double)nhi * 0x1p-62 + (double)nlo * 0x1p-94;

        *r = -(f * pio2);
        return (q + 1) & 3;
    }
    *r = ((double)hi * 0x1p-62 + (double)l0 * 0x1p-94) * pio2;
    return q;
}

/* x = quadrant pi/2 + r. Returns the quadrant (0..3); NaN r for x not
   finite. */
static int reduce(float x, double *r)
{
    uint32_t bits;
    double ax, k, y;
    int q;

    memcpy(&bits, &x, sizeof bits);
    if (((bits >> 23) & 0xff) == 0xff) {
        *r = (double)(x - x); /* NaN for infinities and NaNs */
        return 0;
    }
    ax = (double)x;
    if (ax < 0)
        ax = -ax;
    if (ax < 0x1p23) {
        double d = (double)x;

        if (ax <= 0x1.921fb5p-1) { /* below pi/4: no reduction */
            *r = d;
            return 0;
        }
        y = d * two_over_pi;
        /* The nearest integer (ties away from zero do not matter here):
           adding 1.5 x 2^52 rounds y to an integer in double. */
        k = (y + 0x1.8p52) - 0x1.8p52;
        y = d - k * pio2_1;
        y = y - k * pio2_2;
        *r = y - k * pio2_3;
        return (int)(int64_t)k & 3;
    }
    q = reduce_large(bits & 0x7fffffffu, r);
    if (bits >> 31) {
        *r = -*r;
        q = (4 - q) & 3;
    }
    return q;
}

/* Taylor series by Horner's rule, the coefficients 1/n! rounded to double
   (constant expressions, so rounded once by the compiler, exactly). */
static double sin_poly(double r)
{
    double z = r * r, p;

    p = 1.0 / 355687428096000.0;      /* 1/17! */
    p = p * z - 1.0 / 1307674368000.0; /* 1/15! */
    p = p * z + 1.0 / 6227020800.0;    /* 1/13! */
    p = p * z - 1.0 / 39916800.0;      /* 1/11! */
    p = p * z + 1.0 / 362880.0;        /* 1/9! */
    p = p * z - 1.0 / 5040.0;          /* 1/7! */
    p = p * z + 1.0 / 120.0;           /* 1/5! */
    p = p * z - 1.0 / 6.0;             /* 1/3! */
    p = p * z;
    return r + r * p;
}

static double cos_poly(double r)
{
    double z = r * r, p;

    p = 1.0 / 6402373705728000.0;     /* 1/18! */
    p = p * z - 1.0 / 20922789888000.0; /* 1/16! */
    p = p * z + 1.0 / 87178291200.0;    /* 1/14! */
    p = p * z - 1.0 / 479001600.0;      /* 1/12! */
    p = p * z + 1.0 / 3628800.0;        /* 1/10! */
    p = p * z - 1.0 / 40320.0;          /* 1/8! */
    p = p * z + 1.0 / 720.0;            /* 1/6! */
    p = p * z - 1.0 / 24.0;             /* 1/4! */
    p = p * z + 0.5;                    /* 1/2! */
    return 1.0 - z * p;
}

/* The one NaN the trigonometric functions return (quiet, positive,
   payload 0), whatever NaN the hardware would make. */
static float canonical_nan(void)
{
    const uint32_t bits = 0x7fc00000u;
    float f;

    memcpy(&f, &bits, sizeof f);
    return f;
}

static int not_finite(float x)
{
    uint32_t bits;

    memcpy(&bits, &x, sizeof bits);
    return ((bits >> 23) & 0xff) == 0xff;
}

float jb_sinf(float x)
{
    double r;
    int q;

    if (x == 0.0f) /* keeps the sign of zero */
        return x;
    if (not_finite(x))
        return canonical_nan();
    q = reduce(x, &r);

    switch (q) {
    case 0:
        return (float)sin_poly(r);
    case 1:
        return (float)cos_poly(r);
    case 2:
        return (float)-sin_poly(r);
    default:
        return (float)-cos_poly(r);
    }
}

float jb_cosf(float x)
{
    double r;
    int q;

    if (not_finite(x))
        return canonical_nan();
    q = reduce(x, &r);

    switch (q) {
    case 0:
        return (float)cos_poly(r);
    case 1:
        return (float)-sin_poly(r);
    case 2:
        return (float)-cos_poly(r);
    default:
        return (float)sin_poly(r);
    }
}

float jb_tanf(float x)
{
    double r, s, c;
    int q;

    if (x == 0.0f)
        return x;
    if (not_finite(x))
        return canonical_nan();
    q = reduce(x, &r);
    s = sin_poly(r);
    c = cos_poly(r);

    if (q & 1)
        return (float)(-c / s);
    return (float)(s / c);
}

/* ------------------------------------------------------------------------ */
/* Vectors */

float jb_vec3_dot(jb_vec3 a, jb_vec3 b)
{
    float s = a.x * b.x;

    s = s + a.y * b.y;
    return s + a.z * b.z;
}

jb_vec3 jb_vec3_cross(jb_vec3 a, jb_vec3 b)
{
    jb_vec3 c;

    c.x = a.y * b.z - a.z * b.y;
    c.y = a.z * b.x - a.x * b.z;
    c.z = a.x * b.y - a.y * b.x;
    return c;
}

static float absf(float x)
{
    return x < 0.0f ? -x : x;
}

jb_vec3 jb_vec3_normalize(jb_vec3 v)
{
    float d = jb_vec3_dot(v, v), l;

    /* A squared length that overflows or underflows (a component beyond
       about 1e19 or below 1e-19): scaled by the largest component first,
       which leaves the direction and puts the squared length within 1..3.
       Lengths in between are normalised directly, as before. */
    /* Any component infinite or NaN: as it was, whatever the others. */
    if (v.x - v.x != 0.0f || v.y - v.y != 0.0f || v.z - v.z != 0.0f)
        return v;
    if (!(d >= 0x1p-100f && d <= 0x1p100f)) {
        float m = absf(v.x);

        m = absf(v.y) > m ? absf(v.y) : m;
        m = absf(v.z) > m ? absf(v.z) : m;
        if (!(m > 0.0f) || !(m <= 3.4028234e38f))
            return v; /* zero, infinite or NaN: as it was */
        v.x = v.x / m;
        v.y = v.y / m;
        v.z = v.z / m;
        d = jb_vec3_dot(v, v);
    }
    l = jbi_sqrtf(d);
    v.x = v.x / l;
    v.y = v.y / l;
    v.z = v.z / l;
    return v;
}

/* ------------------------------------------------------------------------ */
/* Matrices */

#define M(a, r, c) ((a).m[(c) * 4 + (r)])

jb_mat4 jb_mat4_identity(void)
{
    jb_mat4 a;

    memset(&a, 0, sizeof a);
    a.m[0] = a.m[5] = a.m[10] = a.m[15] = 1.0f;
    return a;
}

jb_mat4 jb_mat4_translate(float x, float y, float z)
{
    jb_mat4 a = jb_mat4_identity();

    a.m[12] = x;
    a.m[13] = y;
    a.m[14] = z;
    return a;
}

jb_mat4 jb_mat4_scale(float x, float y, float z)
{
    jb_mat4 a = jb_mat4_identity();

    a.m[0] = x;
    a.m[5] = y;
    a.m[10] = z;
    return a;
}

/* A rotation in the plane of axes i and j (i to j for a positive angle). */
static jb_mat4 rotation(int i, int j, float radians)
{
    jb_mat4 a = jb_mat4_identity();
    float c = jb_cosf(radians), s = jb_sinf(radians);

    M(a, i, i) = c;
    M(a, i, j) = -s;
    M(a, j, i) = s;
    M(a, j, j) = c;
    return a;
}

jb_mat4 jb_mat4_rotate_x(float radians)
{
    return rotation(1, 2, radians);
}

jb_mat4 jb_mat4_rotate_y(float radians)
{
    return rotation(2, 0, radians);
}

jb_mat4 jb_mat4_rotate_z(float radians)
{
    return rotation(0, 1, radians);
}

jb_mat4 jb_mat4_mul(const jb_mat4 *a, const jb_mat4 *b)
{
    jb_mat4 out;
    int r, c, k;

    if (a == NULL && b == NULL)
        return jb_mat4_identity();
    if (a == NULL)
        return *b;
    if (b == NULL)
        return *a;
    for (c = 0; c < 4; c++) {
        for (r = 0; r < 4; r++) {
            float s = M(*a, r, 0) * M(*b, 0, c);

            for (k = 1; k < 4; k++)
                s = s + M(*a, r, k) * M(*b, k, c);
            M(out, r, c) = s;
        }
    }
    return out;
}

jb_mat4 jb_mat4_trs(jb_vec3 t, jb_vec3 r, jb_vec3 s)
{
    jb_mat4 a = jb_mat4_translate(t.x, t.y, t.z), b;

    b = jb_mat4_rotate_z(r.z);
    a = jb_mat4_mul(&a, &b);
    b = jb_mat4_rotate_y(r.y);
    a = jb_mat4_mul(&a, &b);
    b = jb_mat4_rotate_x(r.x);
    a = jb_mat4_mul(&a, &b);
    b = jb_mat4_scale(s.x, s.y, s.z);
    return jb_mat4_mul(&a, &b);
}

int jbi_vec3_parallel(jb_vec3 eye, jb_vec3 target, jb_vec3 up)
{
    /* In double, from the given values: the differences of two floats are
       exact or nearly so there, and the products exact, so directions
       that are parallel give 0 rather than a rounding error's worth. */
    double dx = (double)target.x - (double)eye.x, dy = (double)target.y - (double)eye.y;
    double dz = (double)target.z - (double)eye.z;
    double ux = (double)up.x, uy = (double)up.y, uz = (double)up.z;
    double cx = dy * uz - dz * uy, cy = dz * ux - dx * uz, cz = dx * uy - dy * ux;
    double c2 = cx * cx + cy * cy + cz * cz;
    double d2 = dx * dx + dy * dy + dz * dz, u2 = ux * ux + uy * uy + uz * uz;

    return !(c2 > 1e-12 * d2 * u2);
}

jb_mat4 jb_mat4_look_at(jb_vec3 eye, jb_vec3 target, jb_vec3 up)
{
    jb_vec3 f, s, u, d;
    jb_mat4 a;

    d.x = target.x - eye.x;
    d.y = target.y - eye.y;
    d.z = target.z - eye.z;
    /* eye at target, a difference that overflows, or up parallel to the
       view direction (judged from the given values: jbi_vec3_parallel). */
    if ((d.x == 0.0f && d.y == 0.0f && d.z == 0.0f) || d.x - d.x != 0.0f ||
        d.y - d.y != 0.0f || d.z - d.z != 0.0f || jbi_vec3_parallel(eye, target, up))
        return jb_mat4_identity();
    f = jb_vec3_normalize(d);
    s = jb_vec3_cross(f, jb_vec3_normalize(up)); /* up made unit: any length works */
    if (jb_vec3_dot(s, s) == 0.0f)
        return jb_mat4_identity();
    s = jb_vec3_normalize(s);
    u = jb_vec3_cross(s, f);
    a = jb_mat4_identity();
    /* Rows: right, up, back (-f); then the eye moved to the origin. */
    M(a, 0, 0) = s.x;
    M(a, 0, 1) = s.y;
    M(a, 0, 2) = s.z;
    M(a, 1, 0) = u.x;
    M(a, 1, 1) = u.y;
    M(a, 1, 2) = u.z;
    M(a, 2, 0) = -f.x;
    M(a, 2, 1) = -f.y;
    M(a, 2, 2) = -f.z;
    M(a, 0, 3) = -jb_vec3_dot(s, eye);
    M(a, 1, 3) = -jb_vec3_dot(u, eye);
    M(a, 2, 3) = jb_vec3_dot(f, eye);
    return a;
}

jb_vec3 jb_mat4_transform_point(const jb_mat4 *m, jb_vec3 p)
{
    jb_vec3 o;

    if (m == NULL)
        return p;
    o.x = M(*m, 0, 0) * p.x;
    o.x = o.x + M(*m, 0, 1) * p.y;
    o.x = o.x + M(*m, 0, 2) * p.z;
    o.x = o.x + M(*m, 0, 3);
    o.y = M(*m, 1, 0) * p.x;
    o.y = o.y + M(*m, 1, 1) * p.y;
    o.y = o.y + M(*m, 1, 2) * p.z;
    o.y = o.y + M(*m, 1, 3);
    o.z = M(*m, 2, 0) * p.x;
    o.z = o.z + M(*m, 2, 1) * p.y;
    o.z = o.z + M(*m, 2, 2) * p.z;
    o.z = o.z + M(*m, 2, 3);
    return o;
}

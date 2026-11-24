/*
 * The library's float maths (src/geom/fmath.c; the public jb_sinf,
 * jb_mat4_* and friends in include/jitterbug.h), and the C library's
 * square roots, the only functions of it used.
 */
#ifndef JBI_GEOM_FMATH_H
#define JBI_GEOM_FMATH_H

#include <math.h>

/* sqrtf, which IEEE 754 requires to be correctly rounded, so the same on
   every platform (the trigonometric functions are not, and are never
   called). */
static inline float jbi_sqrtf(float x)
{
    return sqrtf(x);
}

/* sqrt, correctly rounded likewise. */
static inline double jbi_sqrt(double x)
{
    return sqrt(x);
}

/*
 * Whether the camera direction target - eye is parallel to up (or either
 * is zero), judged in double from the given floats, so that directions
 * that are exactly parallel are found so whatever rounding a unit vector
 * would bring (jb_mat4_look_at, jb_set_camera).
 */
#include <jitterbug.h>
int jbi_vec3_parallel(jb_vec3 eye, jb_vec3 target, jb_vec3 up);

#endif

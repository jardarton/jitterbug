/*
 * What the exact path assumes about the compiler and the machine
 * (docs/determinism.md, "How the code keeps it"). The integer code depends
 * on each of these, so the build stops here when one doesn't hold.
 * tests/assumptions.c checks
 * the same things at run time.
 */
#include <float.h>
#include <limits.h>
#include <stdint.h>

#ifdef __FAST_MATH__
#error "jitterbug must not be built with -ffast-math: it changes results between compilers"
#endif

/* The float maths (src/geom/fmath.c) and the float options need float and
 * double expressions evaluated at their own precision, as IEEE 754 single
 * and double, not wider (x87's extended precision would change results). */
#if !defined(FLT_EVAL_METHOD) || FLT_EVAL_METHOD != 0
#error "jitterbug needs FLT_EVAL_METHOD 0: float arithmetic at float precision (SSE2, not x87)"
#endif

/* The float maths takes float and double apart by their bits (range
   reduction, the NaN it returns) and its polynomials assume their
   precision: IEEE 754 binary32 and binary64. */
_Static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
               "float is IEEE 754 binary32");
_Static_assert(DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024, "double is IEEE 754 binary64");
_Static_assert(sizeof(float) == 4 && sizeof(double) == 8, "float and double are 4 and 8 bytes");

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "jitterbug needs a little-endian host"
#endif
#endif

_Static_assert(CHAR_BIT == 8, "bytes are 8 bits");
_Static_assert(sizeof(int) == 4, "int is 32 bits");
_Static_assert(sizeof(short) == 2, "short is 16 bits");
/* Two's complement. */
_Static_assert((-1 & 3) == 3, "integers are two's complement");
_Static_assert(INT_MIN == -INT_MAX - 1, "integers are two's complement");
/* C leaves the right shift of a negative value to the implementation. The
 * rasteriser and the GTE need it to keep the sign. */
_Static_assert((-1 >> 1) == -1, "right shift of a negative int is arithmetic");
_Static_assert((-5 >> 1) == -3, "right shift of a negative int rounds down");
_Static_assert(((int64_t)-1 >> 33) == -1, "right shift of a negative int64_t is arithmetic");
/* Narrowing keeps the low bits (implementation-defined before C23). */
_Static_assert((int16_t)0x18000 == -32768, "conversion to int16_t keeps the low bits");
_Static_assert((int32_t)0xffffffffu == -1, "conversion to int32_t keeps the low bits");

/* A translation unit needs a declaration; a constant, so the library keeps
   no writable data. */
extern const int jbi_assumptions_hold;
const int jbi_assumptions_hold = 1;

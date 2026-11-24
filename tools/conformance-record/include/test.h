/*
 * The recorder's test.h, standing in for ps1-tests' common/test.h (C++).
 *
 * An assertion becomes a check in the recorded list: the value the
 * program compared, as a function of the one GPUREAD or GPUSTAT word it
 * was read from, against what the console gave (docs/testing.md, "How
 * `tools/conformance-record` works",
 * "Main loop & checks"). The expression is passed as a closure so that
 * src/check.c can evaluate
 * it again with chosen read values.
 */
#ifndef JBREC_TEST_H
#define JBREC_TEST_H

#ifndef __cplusplus
#error "test.h is C++, as ps1-tests' is"
#endif

#include <stdint.h>

extern "C" {
int jbrec_assert(const char *function, int line, const char *given, const char *expected,
                 uint32_t (*eval)(void *), void *closure, uint32_t want);
void jbrec_unsupported(const char *what);
}

template <class F> static uint32_t jbrec_call(void *f)
{
    return (*static_cast<F *>(f))();
}

template <class F>
static bool jbrec_assert_cxx(const char *function, int line, const char *given,
                             const char *expected, F f, uint32_t want)
{
    return jbrec_assert(function, line, given, expected, &jbrec_call<F>, &f, want) != 0;
}

#define assertEqualsWithComment(given, expected, comment)                                   \
    jbrec_assert_cxx(__FUNCTION__, __LINE__, #given, #expected,                            \
                     [&]() -> uint32_t { return (uint32_t)(given); }, (uint32_t)(expected))

#define assertEquals(given, expected) assertEqualsWithComment(given, expected, "")
#define assertTrue(given) assertEqualsWithComment(given, true, "")
#define assertFalse(given) assertEqualsWithComment(given, false, "")

/* Groups print one line for several checks; not recorded yet. */
#define TEST_MULTIPLE_BEGIN() jbrec_unsupported("TEST_MULTIPLE_BEGIN")
#define TEST_MULTIPLE_END() (jbrec_unsupported("TEST_MULTIPLE_END"), true)

#endif

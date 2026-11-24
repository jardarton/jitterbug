/*
 * Stands in for ps1-tests' common/stdint.h, which its common/gpu.h
 * includes as "stdint.h" (this directory is on the quote-only include
 * path, -iquote, so <stdint.h> is still the host's). ps1-tests' file gives
 * the integer types for the console and, for C, true, false and a bool
 * that is a char; here the integer types are the host's (they have the
 * same sizes) and the rest is as ps1-tests has it.
 */
#ifndef JBREC_PS1_TESTS_STDINT_H
#define JBREC_PS1_TESTS_STDINT_H

#include <stddef.h>
#include <stdint.h>

#ifndef __cplusplus
#ifndef true
#define true 1
#endif
#ifndef false
#define false 0
#endif
typedef char bool;
#endif

#endif

/*
 * Included before every source of a recorded program (CMakeLists.txt,
 * -include). What the SDK's C library headers gave a program that the
 * host's do not, or give differently (docs/testing.md, "How
 * `tools/conformance-record` works",
 * "Determinism flags"):
 *
 *   - true and false: the SDK's stdio.h defines them as 1 and 0 for C as
 *     well, and programs from before ps1-tests' common library use them
 *     with no other header for them;
 *   - rand and srand: the SDK's, not the host's (src/sdk.c), whichever
 *     header the program included them from.
 *
 * The host's stdlib.h and stdio.h are included first, so that the names
 * below do not rename their declarations.
 */
#ifndef JBREC_PRELUDE_H
#define JBREC_PRELUDE_H

#include <stdio.h>
#include <stdlib.h>

#ifndef __cplusplus
#ifndef true
#define true 1
#endif
#ifndef false
#define false 0
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif
int jbrec_rand(void);
void jbrec_srand(unsigned int seed);
#ifdef __cplusplus
}
#endif

#define rand jbrec_rand
#define srand jbrec_srand

#endif

/*
 * The recorder's common.h, standing in for ps1-tests' common library
 * header of the same name, which the programs include: the C library
 * headers it gives them, its "stdint.h" (include/ps1-tests-common, the
 * host's types and, for C, ps1-tests' bool), and its gpu.h, which is
 * ps1-tests' own, unmodified, from the program's commit (CMakeLists.txt).
 * The SDK's rand comes from jbrec_prelude.h.
 */
#ifndef JBREC_COMMON_H
#define JBREC_COMMON_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "stdint.h"

#include "gpu.h"

#endif

/*
 * The implementation of the vendored cgltf (third_party/cgltf), built
 * without the project's warning flags, as third-party code. Its numbers go
 * through jb-mesh's own parser (jbm_parse_number) rather than the C
 * library's atof, so a glTF gives the same floats on every platform.
 */
#include "mesh.h"

static double jbm_atof(const char *text)
{
    double v = 0.0;

    return jbm_parse_number(text, NULL, &v) == 0 ? v : 0.0;
}

#define CGLTF_IMPLEMENTATION
#define CGLTF_ATOF(str) jbm_atof(str)
#include "cgltf.h"

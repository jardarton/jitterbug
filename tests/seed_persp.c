/*
 * The perspective-correct texturing helpers of src/soft/raster.h
 * (jbi_persp_setup, jbi_persp_row, jbi_persp_at) on a local triangle: the
 * part of the seed GPU test's perspective clamp checks that a call trace
 * cannot carry (tests/seed holds the drawn part). Exact values at a vertex,
 * and clamping to the texture coordinates' bounding box far outside the
 * triangle.
 */
#include <stdio.h>
#include <string.h>

#include "soft/raster.h"

static int failures, checks;

static void check(const char *what, long long got, long long want)
{
    ++checks;
    if (got != want) {
        ++failures;
        printf("FAIL %s: got %lld, want %lld\n", what, got, want);
    }
}

int main(void)
{
    jbi_prim p;
    jbi_persp pp;
    double X[3] = {10, 50, 12}, Y[3] = {10, 12, 40}, row[JBI_PERSP_ATTRS + 1];
    long long out[JBI_PERSP_ATTRS];
    int i, ok = 1;

    /* Texture coordinates (10, 5), (20, 5), (10, 15); the second vertex
       60 times as deep as the others. */
    memset(&p, 0, sizeof p);
    p.u.tri.v[0].u = 10, p.u.tri.v[0].v = 5;
    p.u.tri.v[1].u = 20, p.u.tri.v[1].v = 5;
    p.u.tri.v[2].u = 10, p.u.tri.v[2].v = 15;
    p.u.tri.z[0] = 50, p.u.tri.z[1] = 3000, p.u.tri.z[2] = 50;
    /* The perspective pass reads them from the perspective attributes
       (src/core/prim.h), which the decoder copies from the vertices. */
    for (i = 0; i < 3; ++i) {
        p.u.tri.pattr[i][0] = (uint8_t)p.u.tri.v[i].u;
        p.u.tri.pattr[i][1] = (uint8_t)p.u.tri.v[i].v;
    }
    check("setup", jbi_persp_setup(&pp, &p, X, Y, 0), 1);

    jbi_persp_row(&pp, 10, row);
    jbi_persp_at(&pp, row, 10, 16, 0, out);
    check("exact u at a vertex", out[0], 10 << 16);
    check("exact v at a vertex", out[1], 5 << 16);

    for (i = -400; i <= 400; i += 7) {
        jbi_persp_row(&pp, i, row);
        jbi_persp_at(&pp, row, -i * 3, 16, 0, out);
        ok &= out[0] >= 10 << 16 && out[0] <= 20 << 16 && out[1] >= 5 << 16 &&
              out[1] <= 15 << 16;
    }
    check("clamped to the bounding box outside the triangle", ok, 1);

    printf("seed_persp: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}

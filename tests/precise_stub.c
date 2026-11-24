/*
 * A precise source (src/core/precise_source.h) that records nothing:
 * never enabled, every lookup misses. It shows that a context draws with a
 * substitute for src/geom/precise.c's table, as a trace replayer that
 * supplies its own does.
 */
#include "precise_stub.h"

static int stub_enabled(void *user)
{
    (void)user;
    return 0;
}

static int stub_lookup(void *user, uint32_t sxy, jbi_precise_vertex *v)
{
    (void)user;
    (void)sxy;
    (void)v;
    return 0;
}

static unsigned stub_lookup_poly(void *user, const uint32_t *sxy, int n, jbi_precise_vertex *v)
{
    (void)user;
    (void)sxy;
    (void)n;
    (void)v;
    return 0;
}

jbi_precise_source precise_stub_source(void)
{
    jbi_precise_source s;

    s.enabled = stub_enabled;
    s.lookup = stub_lookup;
    s.lookup_poly = stub_lookup_poly;
    s.user = 0;
    return s;
}

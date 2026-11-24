/*
 * The reference's glue (ref.h): the symbols gpu.c refers to that live
 * elsewhere in psxstack, as stubs, and the calls the tool makes. Compiled
 * with shim/psyq_internal.h beside a build-time copy of gpu.c.
 */
#include "psyq_internal.h"
#include "ref.h"

int psyq_gte_shadow_on = 0;

int psyq_gte_shadow_find(const void *src, u32 word, int *fx, int *fy, int *z)
{
    (void)src;
    (void)word;
    *fx = *fy = -1;
    *z = 0;
    return 0;
}

void port_state_bytes(PortState *s, const char *name, void *p, size_t n)
{
    (void)s;
    (void)name;
    (void)p;
    (void)n;
}

int port_state_loading(PortState *s)
{
    (void)s;
    return 0;
}

void ref_reset(void)
{
    gpu_power_on();
}

void ref_reset_state(void)
{
    gpu_reset_state();
}

void ref_gp0(const uint32_t *w, size_t n)
{
    gpu_gp0_words(w, (u32)n);
}

const uint16_t *ref_vram(void)
{
    return gpu_vram_pixels();
}

/*
 * The reference GPU: psxstack's psyq/gpu.c, built from its pinned commit
 * with shim/psyq_internal.h (docs/testing.md, "Differential fuzzing").
 * One per process: gpu.c keeps its
 * state in globals.
 */
#ifndef FUZZ_DIFF_REF_H
#define FUZZ_DIFF_REF_H

#include <stddef.h>
#include <stdint.h>

/* VRAM all zero and the drawing state reset (gpu.c's power-on). */
void ref_reset(void);
/* The drawing state reset, VRAM kept (gpu.c's gpu_reset_state: E1h-E6h's
   settings 0, the command or transfer in progress dropped). psxstack
   models no GP1: play and conformance give it this for GP1(00h), which
   does the same to jitterbug's drawing state. */
void ref_reset_state(void);
/* GP0 words, in order. */
void ref_gp0(const uint32_t *w, size_t n);
/* Its 1024 x 512 VRAM, row-major. */
const uint16_t *ref_vram(void);

#endif

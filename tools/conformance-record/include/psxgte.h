/*
 * The recorder's psxgte.h: only what the recorded programs use (lines'
 * circle). The GTE sends nothing to the GPU, so InitGeom records nothing;
 * isin and icos give the SDK's values (src/sdk.c, isin, says how).
 */
#ifndef JBREC_PSXGTE_H
#define JBREC_PSXGTE_H

#ifdef __cplusplus
extern "C" {
#endif

void InitGeom(void);
/* Sine and cosine of a (4096 a full turn), 4096 for 1. */
int isin(int a);
int icos(int a);

#ifdef __cplusplus
}
#endif

#endif

/*
 * Stands in for ps1-tests' common/io.h, which its later common/gpu.cpp
 * includes for the register accessors of its DMA helpers. No recorded
 * program calls those helpers, and the recorder does not model the DMA
 * registers, so every accessor stops the recording.
 */
#ifndef JBREC_PS1_TESTS_IO_H
#define JBREC_PS1_TESTS_IO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void jbrec_unsupported(const char *what); /* src/check.c; does not return */

static inline uint32_t read32(uint32_t addr)
{
    (void)addr;
    jbrec_unsupported("a register read of ps1-tests' io.h");
    return 0;
}

static inline void write32(uint32_t addr, uint32_t val)
{
    (void)addr, (void)val;
    jbrec_unsupported("a register write of ps1-tests' io.h");
}

#ifdef __cplusplus
}
#endif

#endif

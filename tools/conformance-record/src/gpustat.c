/*
 * ReadGPUstat: one read of GPUSTAT. The SDK had it in its older versions
 * (mask-bit's dump-era source calls it so), ps1-tests' common library
 * declares it in later ones; either way the program reads the port once.
 * On its own here so that a program that brings its own helpers links
 * without the common library (CMakeLists.txt).
 *
 * ps1-tests' vramGet calls it in a loop until GPUSTAT bit 27 is set; the
 * recorder cannot record a loop, so a call from a helper that polls
 * (calls.c) is the WAIT for those bits, and returns them.
 */
#include "rec.h"

#include <jbc_format.h>

unsigned int ReadGPUstat(void);

unsigned int ReadGPUstat(void)
{
    uint32_t polls = rec_poll_bits(), v;

    if (polls != 0) {
        mmio_flush();
        rec_wait(polls, polls);
        return polls;
    }
    rec_enter("ReadGPUstat");
    v = rec_read(JBC_PORT_GPUSTAT);
    rec_leave();
    return v;
}

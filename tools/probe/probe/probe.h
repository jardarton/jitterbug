/*
 * The probe: plays conformance lists (tests/conformance/FORMAT.md) received
 * over the serial line and sends back what the GPU did (PROTOCOL.md). Plain
 * C99 over hal.h, freestanding.
 */
#ifndef JBP_PROBE_H
#define JBP_PROBE_H

#include <stdint.h>

/* Once, before anything else: identifies the GPU and the machine (before
   any reset, so GPUSTAT is as the probe found it). */
void probe_init(void);

/* Idles (HELLO every second) until a START comes, then plays that session
   to its end. 0 after a session with results sent, 1 after one that
   failed, -1 when idle_ms (0: for ever) passed with no START. */
int probe_serve(uint32_t idle_ms);

#endif

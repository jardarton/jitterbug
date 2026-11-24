/*
 * What the probe needs from the machine it runs on. probe.c (the player and
 * the protocol) uses nothing else, so the same code runs on the console
 * (psx/hal_psx.c: the GPU's ports, DMA channel 2, SIO1, timer 2, I_STAT)
 * and in the host simulation (tests/probe/sim.c: a jitterbug context and a
 * loopback serial line). Freestanding C99.
 */
#ifndef JBP_HAL_H
#define JBP_HAL_H

#include <stddef.h>
#include <stdint.h>

/* The GPU's ports (psx-spx, GPU I/O Ports): a word written to GP0 or GP1,
   a word read from GPUREAD (GP0) or GPUSTAT (GP1). */
void hal_gp0(uint32_t word);
void hal_gp1(uint32_t word);
uint32_t hal_gpuread(void);
uint32_t hal_gpustat(void);

/* n words from RAM to GP0 by DMA channel 2 (psx-spx, DMA Channels): in
   blocks of 8 words (SyncMode 1, as the SDK's LoadImage) when n is a
   multiple of 8, else as a linked list (SyncMode 2) of nodes of up to 16
   words. hal_gp0_dma_start starts the transfer and returns; the probe
   polls hal_gp0_dma_busy (1 while it runs) with its own time limit, and
   stops a transfer that never ends with hal_gp0_dma_stop. The contract
   (tests/probe/sim.c, hal_contract, holds the probe to it): after each
   start, hal_gp0_dma_busy until it reads 0, or, when the limit passes
   first, exactly one hal_gp0_dma_stop; never a stop for a transfer that
   has ended, never a start while one runs. The GPU's DMA direction
   (GP1(04h)) is the list's business: the probe neither sets nor checks
   it. */
void hal_gp0_dma_start(const uint32_t *words, uint32_t n);
int hal_gp0_dma_busy(void);
void hal_gp0_dma_stop(void);

/* A vertical blank: hal_vblank_clear acknowledges one (I_STAT bit 0), and
   hal_vblank_seen is 1 once the next has begun. The probe polls with its
   own time limit. */
void hal_vblank_clear(void);
int hal_vblank_seen(void);

/* Milliseconds since some start, wrapping. Must be called at least every
   15 ms to keep count on the console (timer 2 wraps): every waiting loop
   in probe.c calls it. */
uint32_t hal_ms(void);

/* The serial line: a byte received, or -1 when none came within
   timeout_ms; a byte sent (0, or -1 when the line could not take it within
   the HAL's own limit, CTS off say). */
int hal_serial_get(uint32_t timeout_ms);
int hal_serial_put(uint8_t byte);
/* Overrun, framing and parity errors seen so far. */
uint32_t hal_serial_errors(void);

/* What only the machine can tell: COP0 PRId, the BIOS date and console
   type words, its maker string (from BFC00108h) and version string (from
   BFC7FF32h), each copied up to the size given; the platform
   (JBP_ID_PLATFORM); the SIO1 baud rate factor and reload value. */
typedef struct hal_ident {
    uint32_t platform, cpu_prid, bios_date, console_type, sio_factor, sio_reload;
    char maker[64], version[48];
} hal_ident;
void hal_identify(hal_ident *id);

/* Shown on the screen between sessions (never during one: it draws into
   VRAM), for a first run without a working serial line (docs/probe.md,
   "First-run checklist"): idle, or the last session failed. */
enum { HAL_SHOW_IDLE, HAL_SHOW_ERROR };
void hal_show(int state);

#endif

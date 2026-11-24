/*
 * The recorder's core (record.c): the list being written, the calls that
 * add ops to it, and the end of a recording. Everything a recorded
 * program does to the GPU goes through these.
 */
#ifndef JBREC_REC_H
#define JBREC_REC_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__)
#define JBREC_NORETURN __attribute__((noreturn))
#define JBREC_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define JBREC_NORETURN
#define JBREC_PRINTF(a, b)
#endif

/* Stops the recording: the message to stderr, exit status 1, no list. */
JBREC_NORETURN void rec_fail(const char *fmt, ...) JBREC_PRINTF(1, 2);

/* A call of the SDK or of ps1-tests' common library begins and ends. The
   outermost one names the ops it sends (a CALL op, when the name differs
   from the last one written). Every entry point calls these first, which
   also delivers a pending port write (mmio.c). */
void rec_enter(const char *name);
void rec_leave(void);

void rec_gp0(const uint32_t *words, size_t n, int dma);
void rec_gp1(uint32_t word);
/* One read of a port: the value the program gets (0 when recording; the
   chosen value while probing). */
uint32_t rec_read(int port);
void rec_wait(uint32_t mask, uint32_t value);
void rec_drawsync(void);
/* A vertical blank. The recording ends at the fourth (docs/testing.md, "How
   `tools/conformance-record` works",
   "Main loop & checks"); this does not return then. */
void rec_vsync(void);
void rec_expect(uint32_t read_index, uint32_t mask, uint32_t value, const char *label);

/* Reads recorded so far; the port of the last. */
uint32_t rec_reads(void);
int rec_last_port(void);

/* Probing (check.c): while on, nothing is recorded and every read returns
   value; rec_probe_end gives the number of reads made. */
void rec_probe_begin(uint32_t value);
unsigned rec_probe_end(void);
int rec_probing(void);

/* calls.c: the GPUSTAT bits that the ps1-tests helper calling
   ReadGPUstat waits for (gpustat.c), or 0 when it does not poll. */
uint32_t rec_poll_bits(void);

/* mmio.c: delivers a write to the GPU ports that has not been yet. */
void mmio_flush(void);

/* check.c: at the end, every line of the console's log must have been
   matched to a check. */
void check_finish(void);

/* The bytes of a file (free them), or a recording failure. */
unsigned char *rec_read_file(const char *path, size_t *size);

#endif

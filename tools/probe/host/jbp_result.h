/*
 * A probe result (PROTOCOL.md, "Result files"): what one session sent
 * back, kept in memory and in a .jbpr file. Host C99.
 */
#ifndef JBP_RESULT_H
#define JBP_RESULT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "jbp_proto.h"

enum { JBP_VRAM_W = 1024, JBP_VRAM_H = 512 };

/* The host's own record in a result file. */
enum { JBP_REC_HOST = 0x80 };

typedef struct jbp_result {
    int have_hello;       /* hello holds an identification (HELLO or R_IDENT) */
    int have_ident;       /* from R_IDENT, the session's own */
    int have_head, have_done, have_error, have_host;
    uint8_t hello[JBP_HELLO_BYTES];
    uint32_t head[JBP_H_WORDS];
    uint32_t *reads;      /* kept reads, in order */
    uint32_t nreads;
    uint32_t *expects;    /* JBP_EXPECT_WORDS each */
    uint32_t nexpects;
    uint32_t *waits;      /* JBP_WAITFAIL_WORDS each */
    uint32_t nwaits;
    uint16_t *vram;       /* 1024 x 512, all 16 bits */
    uint32_t next_row;    /* rows received: they come in order, two a record */
    uint32_t done_crc, done_frames;
    uint32_t done_resent, done_rx_bad, done_sio_errors; /* the line's counts at the end */
    uint32_t error[3];    /* code, word offset, bytes received */
    /* The host's record: the list's name ("gpu/triangle"), its length and
       CRC-32, and free text (tty, baud, date). */
    char list_name[128];
    uint32_t list_bytes, list_crc;
    char note[512];
} jbp_result;

int jbp_result_init(jbp_result *r);
void jbp_result_free(jbp_result *r);

/* A payload received (or read from a file) of a frame or record type:
   0, or -1 when it is malformed: not the type's exact size, a string
   without its terminating zero, a part of the reads, checks, waits or
   VRAM that is not the next one or runs past the limits (PROTOCOL.md,
   "Limits and timeouts"), a second summary, end, error, host record or R_IDENT, or a
   type that is not a result's. */
int jbp_result_apply(jbp_result *r, unsigned type, const uint8_t *p, size_t n);

/* Whether it is complete and sound (PROTOCOL.md, "Session flow", 5): the
   session's identification of this protocol, the host's record, the
   summary, of the list the host's record names (length and CRC-32), every
   row, DONE, the CRC-32 of VRAM as received equal to the probe's, every
   read, check and kept wait the summary counts, each check consistent
   with the read it names, and the main loop's rule. 0, or -1 with the
   reason. */
int jbp_result_check(const jbp_result *r, char *why, size_t size);

/* Whether it is clean, which a conformance reference must be (PROTOCOL.md,
   "Result files"): complete and sound (jbp_result_check: no error record,
   steady through the window), and with no WAIT or DRAWSYNC, DMA transfer
   or vertical blank that timed out, since the probe goes on after a
   timeout and the program on the console would not have. A result that is
   not clean can still be read and printed (jbprobe info, to-png). 0, or -1
   with the reason. */
int jbp_result_clean(const jbp_result *r, char *why, size_t size);

/* The CRC-32 of VRAM as the probe sends it (pixels little-endian, rows in
   order). */
uint32_t jbp_result_vram_crc(const jbp_result *r);

int jbp_result_save(const jbp_result *r, const char *path, char *err, size_t size);
/* 0, or -1 with the reason and r freed (nothing to free after a failure). */
int jbp_result_load(jbp_result *r, const char *path, char *err, size_t size);
/* The same from bytes in memory. */
int jbp_result_parse(jbp_result *r, const uint8_t *bytes, size_t n, char *err, size_t size);

/* A JBP_E_* code in words. */
const char *jbp_error_name(uint32_t code);

/* The identification and summary as text. */
void jbp_result_describe(const jbp_result *r, FILE *f);

/* A .jbc list's name from its header ("gpu/triangle"), or "" */
void jbp_list_name(const uint8_t *list, size_t n, char *buf, size_t size);

#endif

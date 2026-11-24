/*
 * The host's side of a probe session (PROTOCOL.md): sends START and the
 * list, acknowledges the results and collects them into a jbp_result.
 * Event-driven, so that the same code runs over a real serial line
 * (jbprobe send) and over the host simulation's loopback (tests/probe):
 * bytes in through jbh_input, time through jbh_poll, bytes out through the
 * write callback. Host C99.
 */
#ifndef JBH_SESSION_H
#define JBH_SESSION_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "jbp_proto.h"
#include "jbp_result.h"

typedef struct jbh_options {
    uint32_t flags;        /* JBP_FLAG_*; jbh_defaults sets JBP_FLAGS_DEFAULT */
    uint32_t passes;       /* the main loop's pass P compared (fixtures.txt's
                              PASSES); 0 for a list without a main loop */
    uint32_t window;       /* passes after P that must not change VRAM (0: 2) */
    uint32_t wait_ms;      /* 0: the probe's default, 1000 */
    uint32_t session_id;
    uint32_t ack_ms;       /* before a data frame is sent again (3000) */
    uint32_t tries;        /* sends of one data frame (16) */
    uint32_t start_ms;     /* how long to keep offering START (60000) */
    uint32_t quiet_ms;     /* the longest silence from a busy probe (60000) */
    int lie_crc;           /* tests only: send a wrong list CRC-32 */
    FILE *log;             /* progress and errors; may be NULL */
    void (*write)(void *user, const uint8_t *bytes, size_t n);
    void *user;
} jbh_options;

void jbh_defaults(jbh_options *o);

enum { JBH_RUNNING, JBH_DONE, JBH_FAILED };

typedef struct jbh_session {
    jbh_options o;
    const uint8_t *list;
    size_t list_n, next; /* bytes of the list sent and acknowledged */
    int state, phase;
    jbp_parser rx;
    uint8_t frame[JBP_MAX_FRAME];
    size_t frame_n;
    int outstanding;      /* frame waits for an ACK */
    unsigned seq;         /* frame's */
    uint32_t sent_ms, first_ms, heard_ms;
    unsigned tries;
    unsigned rx_expect, rx_last;
    int have_last;
    /* The last result frame applied (seq rx_last): only a frame equal to it
       (type, length, payload) is that frame sent again. */
    unsigned last_type;
    size_t last_len;
    uint32_t last_crc;
    uint32_t progress_shown;
    /* Counts, for the log and the tests. */
    unsigned long resent, naks_sent, naks_received, bad_received, cut_received;
    unsigned long ignored; /* good frames not acted on: wrong type, size or seq */
    int said_protocol;
    char why[256];        /* JBH_FAILED: why */
    jbp_result result;
} jbh_session;

/* 0, or -1 (out of memory, or a list of 4 GB or more). The list must stay
   valid until the end. */
int jbh_begin(jbh_session *s, const uint8_t *list, size_t n, const jbh_options *o, uint32_t now_ms);
void jbh_input(jbh_session *s, const uint8_t *bytes, size_t n, uint32_t now_ms);
void jbh_poll(jbh_session *s, uint32_t now_ms);
void jbh_end(jbh_session *s);

/* While running: the time (ms, on jbh_poll's clock, wrapping) at which
   jbh_poll fails the session if nothing more is heard from the probe: the
   START's minute from its first send; for an outstanding LIST frame, its
   sends left, ack_ms apart; while the results come, quiet_ms after the
   probe was last heard. A caller that blocks (a write the line does not
   take, a connect) must give up by then (jbprobe.c, line_write). */
uint32_t jbh_deadline(const jbh_session *s);
/* Fails a running session for a reason of the caller's (the line): why
   is kept as the session's reason. */
void jbh_abandon(jbh_session *s, const char *why);

#endif

/*
 * The probe's serial protocol (tools/probe/PROTOCOL.md): frames, their
 * types and payload layouts, CRC-32, and a frame parser. Shared by the
 * console program (tools/probe/probe) and the host side (tools/probe/host).
 * C99, freestanding: no library calls, so it builds for the console with
 * -ffreestanding -nostdlib.
 */
#ifndef JBP_PROTO_H
#define JBP_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define JBP_PROTOCOL_VERSION 4u

/* A frame: 'J' 'B' type seq len(2, LE) payload[len] crc32(4, LE); the
   CRC covers type, seq, len and the payload. */
enum { JBP_SYNC0 = 0x4a, JBP_SYNC1 = 0x42 };
enum { JBP_HEADER_BYTES = 6, JBP_TRAILER_BYTES = 4 };

/* The largest payload either side sends or accepts: a VRAM frame (two rows
   of 1024 pixels and an 8-byte header) or a list frame. */
enum { JBP_LIST_CHUNK = 4096, JBP_VRAM_ROWS = 2, JBP_MAX_PAYLOAD = 8 + JBP_VRAM_ROWS * 2048 };
enum { JBP_MAX_FRAME = JBP_HEADER_BYTES + JBP_MAX_PAYLOAD + JBP_TRAILER_BYTES };

/* Frame types. Data frames are numbered by seq and acknowledged; the others
   are not. */
enum {
    /* probe -> host, unacknowledged */
    JBP_HELLO = 0x01,    /* the probe is idle; payload: identification words */
    JBP_PROGRESS = 0x02, /* the probe is busy; payload: phase, count, ms */
    /* host -> probe, data */
    JBP_START = 0x10, /* a session; payload: JBP_START_* words */
    JBP_LIST = 0x11,  /* the next bytes of the .jbc file */
    /* probe -> host, data */
    JBP_R_HEAD = 0x20,    /* the session's summary: JBP_H_* words */
    JBP_R_READS = 0x21,   /* first index, then words read */
    JBP_R_EXPECTS = 0x22, /* first index, then JBP_EXPECT_WORDS per EXPECT */
    JBP_R_WAITS = 0x23,   /* first index, then JBP_WAITFAIL_WORDS per failed wait */
    JBP_R_VRAM = 0x24,    /* first row, rows, then the pixels (2 bytes each, LE) */
    JBP_R_DONE = 0x25,    /* CRC-32 of VRAM as read, VRAM frames, frames sent again,
                             damaged frames received, line errors */
    JBP_R_ERROR = 0x26,   /* the session failed: code, word offset, detail */
    JBP_R_IDENT = 0x27,   /* HELLO's payload again, first of the results */
    /* either way, unacknowledged */
    JBP_ACK = 0x30, /* seq: the data frame received */
    JBP_NAK = 0x31  /* a damaged frame was seen: resend now */
};

/* The defined payload sizes. A frame of a known type with another size is
   ignored (control frames) or ends the session (data frames): it is never
   acted on, and it does not count as hearing from the other side. */
enum {
    JBP_PROGRESS_BYTES = 12,
    JBP_LIST_MIN = 1, /* a LIST frame carries 1 to JBP_LIST_CHUNK bytes */
    JBP_DONE_BYTES = 20,
    JBP_ERROR_BYTES = 12
};

/* What the probe keeps and sends back (PROTOCOL.md, "Limits and timeouts"). A
   READ that would take the reads past JBP_READS_MAX, or an EXPECT past
   JBP_EXPECTS_MAX, ends the session (JBP_E_READS, JBP_E_EXPECTS): nothing
   read or evaluated is ever dropped. Of the waits that timed out, the
   first JBP_WAITFAIL_KEPT are sent back and the rest only counted. */
enum { JBP_READS_MAX = 16384, JBP_EXPECTS_MAX = 512 };

/* The smallest list (tests/conformance/FORMAT.md): the four header words,
   two empty strings, END. */
enum { JBP_LIST_BYTES_MIN = 28 };

/* JBP_HELLO payload: identification words, then the BIOS strings. */
enum {
    JBP_ID_PROTOCOL,     /* JBP_PROTOCOL_VERSION */
    JBP_ID_PLATFORM,     /* 1 console, 2 host simulation */
    JBP_ID_CPU_PRID,     /* COP0 r15 */
    JBP_ID_GPUSTAT_BOOT, /* GPUSTAT as the probe found it */
    JBP_ID_GPU_VERSION,  /* 0, 1 or 2 (psx-spx, GPU Detection) */
    JBP_ID_GP1_10_2,     /* GP1(10h) index 2 to 5, 7 and 8, as read */
    JBP_ID_GP1_10_3,
    JBP_ID_GP1_10_4,
    JBP_ID_GP1_10_5,
    JBP_ID_GP1_10_7,
    JBP_ID_GP1_10_8,
    JBP_ID_GPUSTAT_E1,   /* GPUSTAT after the v0/v1 test's GP0(E1h), or 0 */
    JBP_ID_BIOS_DATE,    /* BFC00100h, BCD YYYYMMDD */
    JBP_ID_CONSOLE_TYPE, /* BFC00104h */
    JBP_ID_SIO_FACTOR,   /* SIO1 MR bits 0-1 */
    JBP_ID_SIO_RELOAD,   /* SIO1 BR */
    JBP_ID_LOOP_CAP,     /* words of a main loop the probe can hold */
    JBP_ID_READ_CAP,     /* reads it keeps: JBP_READS_MAX */
    JBP_ID_WORDS         /* then 64 bytes from BFC00108h and 48 from BFC7FF32h */
};
enum { JBP_ID_MAKER_BYTES = 64, JBP_ID_VERSION_BYTES = 48 };
enum { JBP_HELLO_BYTES = JBP_ID_WORDS * 4 + JBP_ID_MAKER_BYTES + JBP_ID_VERSION_BYTES };

/* JBP_START payload. */
enum {
    JBP_START_PROTOCOL,     /* JBP_PROTOCOL_VERSION */
    JBP_START_SESSION,      /* any value; a START with a new one starts again */
    JBP_START_FLAGS,        /* JBP_FLAG_* */
    JBP_START_LIST_BYTES,   /* the length of the .jbc file */
    JBP_START_LIST_CRC,     /* its CRC-32 */
    JBP_START_PASSES,       /* the main loop's pass P after which VRAM is compared,
                               pass 1 being everything before LOOP (FORMAT.md,
                               "Playing a list"); 0 for a list without a main loop */
    JBP_START_WINDOW,       /* passes played after P, which must leave VRAM as
                               it was after P (0: 2) */
    JBP_START_WAIT_MS,      /* how long a WAIT or DRAWSYNC may poll (0: 1000) */
    JBP_START_WORDS     /* exactly these: a START of another size is refused */
};
enum { JBP_WINDOW_DEFAULT = 2, JBP_PASSES_MAX = 1000000 };
enum {
    JBP_FLAG_RESET = 1u << 0, /* GP1(00h), GP1(01h), GP1(09h)=0 before the list */
    JBP_FLAG_CLEAR = 1u << 1  /* VRAM filled with 0 before the list */
};
#define JBP_FLAGS_DEFAULT (JBP_FLAG_RESET | JBP_FLAG_CLEAR)

/* JBP_R_HEAD payload. */
enum {
    JBP_H_STATUS,        /* 0, or a JBP_E_* code (then R_ERROR came first) */
    JBP_H_LIST_BYTES,    /* bytes received */
    JBP_H_LIST_CRC,      /* their CRC-32 */
    JBP_H_OPS,           /* ops played before the main loop (LOOP and END included) */
    JBP_H_READS,         /* words read (READ ops), at most JBP_READS_MAX */
    JBP_H_READS_KEPT,    /* of which kept and sent: all of them */
    JBP_H_EXPECTS,       /* EXPECT ops, at most JBP_EXPECTS_MAX, all sent */
    JBP_H_EXPECTS_FAILED,
    JBP_H_WAITS,         /* WAIT and DRAWSYNC ops played, passes included */
    JBP_H_WAITS_FAILED,  /* of which timed out (the first JBP_WAITFAIL_KEPT sent) */
    JBP_H_DMA_OPS,       /* DMA transfers started */
    JBP_H_DMA_TIMEOUTS,
    JBP_H_VSYNCS,
    JBP_H_VSYNC_TIMEOUTS,
    JBP_H_LOOP,          /* 1 when the list has a main loop */
    JBP_H_PASSES,        /* START's pass P: VRAM after it is the state compared */
    JBP_H_WINDOW,        /* START's window W (the default filled in) */
    JBP_H_PLAYED,        /* the last pass played: P + W, or the window pass
                            that changed VRAM (pass 1 is up to LOOP) */
    JBP_H_STEADY,        /* 1 when no pass of the window changed VRAM */
    JBP_H_EARLY,         /* 1 when P > 1 and pass P left VRAM as pass P - 1 had
                            (P is not the first pass of the fixed point) */
    JBP_H_CHANGED_AT,    /* the window pass that changed VRAM, or 0 */
    JBP_H_CRC_BEFORE,    /* CRC-32 of a read-back after pass P - 1 (0 if P < 2) */
    JBP_H_CRC_AT,        /* after pass P */
    JBP_H_CRC_LAST,      /* after the last pass played */
    JBP_H_GPUSTAT_END,   /* GPUSTAT when the list had been played */
    JBP_H_GPUSTAT_READ,  /* GPUSTAT just before VRAM was read back */
    JBP_H_RESTORE,       /* JBP_RESTORE_*: what was done before the read-back */
    JBP_H_SIO_ERRORS,    /* overrun, framing or parity errors seen */
    JBP_H_RX_BAD,        /* damaged or cut frames received */
    JBP_H_TX_RESENT,     /* frames sent again */
    JBP_H_LIST_MS,       /* ms from START to the end of the list */
    JBP_H_LOOP_MS,       /* ms in the main loop */
    JBP_H_WORDS
};
enum {
    /* Bit 0 (protocol 2: GP1(01h) sent when the GPU never became ready) is
       no longer used: such a session fails (JBP_E_NOT_IDLE). */
    JBP_RESTORE_DMA_OFF = 1u << 1, /* GP1(04h)=0 sent (always) */
    JBP_RESTORE_VRAM_SIZE = 1u << 2 /* GP1(09h)=0 sent (always) */
};

/* One EXPECT, as evaluated on the probe: the op's word offset in the file,
   the read index, mask, value, the word read. */
enum { JBP_EXPECT_WORDS = 5 };
/* One wait that timed out: the op's word offset, mask, value, GPUSTAT last
   seen, the main loop pass (0 before it). */
enum { JBP_WAITFAIL_WORDS = 5, JBP_WAITFAIL_KEPT = 64 };

/* JBP_R_ERROR codes. */
enum {
    JBP_E_NONE,
    JBP_E_MAGIC,      /* not a .jbc list */
    JBP_E_VERSION,    /* a version the probe does not know */
    JBP_E_OP,         /* an unknown op or a bad payload size */
    JBP_E_LOOP_FULL,  /* the main loop is larger than the probe can hold */
    JBP_E_LENGTH,     /* more or fewer bytes than START said, or data after END */
    JBP_E_CRC,        /* the list's CRC-32 is not START's */
    JBP_E_TIMEOUT,    /* the host stopped sending */
    JBP_E_PROTOCOL,   /* a START for another protocol version */
    JBP_E_PASSES,     /* a main loop and no pass P, or a pass P and no main loop,
                         or P above JBP_PASSES_MAX */
    JBP_E_FRAME,      /* a frame the protocol does not allow: a START not of
                         exactly JBP_START_WORDS words or not at seq 0, a LIST
                         of no bytes or more than JBP_LIST_CHUNK */
    JBP_E_NOT_IDLE,   /* at a read-back (after a pass of the main loop, or
                         after the list), the GPU was not ready for a command
                         (GPUSTAT bits 26 and 28) within JBP_READY_MS: a GPU
                         packet left unfinished at LOOP or END, or a GPU that
                         never finished drawing */
    JBP_E_READBACK,   /* after GP0(C0h), GPUSTAT bit 27 (VRAM ready to be
                         read) never came within JBP_READY_MS */
    JBP_E_READS,      /* a READ would take the reads past JBP_READS_MAX */
    JBP_E_EXPECTS,    /* an EXPECT past JBP_EXPECTS_MAX */
    JBP_E_PREPARE     /* before the list (START's flags), the GPU was not ready
                         for a command (GPUSTAT bits 26 and 28) within
                         JBP_READY_MS: no word of the list was played */
};

/* How long a read-back, or the preparation before the list, waits for
   each GPUSTAT condition, ms. */
enum { JBP_READY_MS = 1000 };

/* JBP_PROGRESS phases. */
enum { JBP_P_LIST = 1, JBP_P_LOOP = 2, JBP_P_READBACK = 3 };

/* CRC-32 (IEEE 802.3, reflected, as zlib's crc32). Start with 0. */
uint32_t jbp_crc32(uint32_t crc, const void *data, size_t n);

/* Builds a frame in out (JBP_MAX_FRAME bytes): its length. */
size_t jbp_frame(uint8_t *out, unsigned type, unsigned seq, const void *payload, size_t n);

/* Little-endian words in a payload. */
void jbp_put32(uint8_t *p, uint32_t v);
uint32_t jbp_get32(const uint8_t *p);

/* A receiver, byte by byte. */
typedef struct jbp_parser {
    unsigned state;
    unsigned type, seq;
    size_t len, got;
    uint32_t last_ms; /* when the last byte came */
    uint8_t head[JBP_HEADER_BYTES];
    uint8_t crc[4];
    uint8_t payload[JBP_MAX_PAYLOAD];
    unsigned long bad;   /* damaged frames (CRC, length) */
    unsigned long cut;   /* frames abandoned half way (no byte for JBP_BYTE_GAP_MS) */
} jbp_parser;

enum { JBP_BYTE_GAP_MS = 200 };
enum { JBP_PARSE_NONE, JBP_PARSE_FRAME, JBP_PARSE_BAD };

void jbp_parser_init(jbp_parser *p);
/* One byte received at now_ms: JBP_PARSE_FRAME when it completes a good
   frame (p->type, p->seq, p->payload, p->len), JBP_PARSE_BAD when it ends a
   damaged one, else JBP_PARSE_NONE. */
int jbp_parse(jbp_parser *p, uint8_t byte, uint32_t now_ms);
/* No byte came at now_ms: abandons a frame half received for too long (1
   when one was). */
int jbp_parser_idle(jbp_parser *p, uint32_t now_ms);

#endif

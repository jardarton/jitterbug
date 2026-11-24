/*
 * The conformance list format (tests/conformance/FORMAT.md): the magic,
 * the version and the op codes, shared by the recorder
 * (tools/conformance-record) and the players (tests/conformance). C99.
 */
#ifndef JB_CONFORMANCE_JBC_FORMAT_H
#define JB_CONFORMANCE_JBC_FORMAT_H

#define JBC_MAGIC "JBCLIST" /* with its terminating zero: 8 bytes */
#define JBC_VERSION 1u

/* An op is one word, the code in bits 24-31 and the number of payload
   words that follow in bits 0-23. */
#define JBC_OP(code, n) ((unsigned long)(code) << 24 | (unsigned long)(n))
#define JBC_OP_CODE(w) ((unsigned)((w) >> 24) & 0xffu)
#define JBC_OP_COUNT(w) ((unsigned long)(w) & 0xffffffu)
#define JBC_MAX_COUNT 0xffffffu

enum {
    JBC_END = 0x00,      /* none: the last op, which ends the file */
    JBC_GP0 = 0x01,      /* words: written to GP0 by the CPU, in order */
    JBC_GP0_DMA = 0x02,  /* words: reached GP0 by DMA, in order */
    JBC_GP1 = 0x03,      /* words: written to GP1, in order */
    JBC_READ = 0x04,     /* port, count: count reads of the port */
    JBC_WAIT = 0x05,     /* mask, value: GPUSTAT polled until (stat & mask) == value */
    JBC_DRAWSYNC = 0x06, /* none: the SDK's DrawSync(0) */
    JBC_VSYNC = 0x07,    /* none: a wait for the vertical blank */
    JBC_LOOP = 0x08,     /* none: from here to END is repeated for ever */
    JBC_EXPECT = 0x09,   /* read index, mask, value, label: what the console read */
    JBC_CALL = 0x0a,     /* label: the function that sends the ops that follow */
    JBC_OP_LIMIT
};

/* READ's ports. */
enum { JBC_PORT_GPUREAD = 0, JBC_PORT_GPUSTAT = 1 };

#endif

/*
 * Reading the seed traces (tests/seed/FORMAT.md), shared by the players:
 * seed_replay.c (the internals) and api_golden.c (the public API). Every
 * read checks the bounds of the file and of the current op's payload, and
 * a malformed trace ends the process with exit status 2.
 */
#ifndef JB_TEST_TRACE_H
#define JB_TEST_TRACE_H

#include <stddef.h>
#include <stdint.h>

enum {
    TRACE_OP_END = 0x00, TRACE_OP_GP0, TRACE_OP_FILL, TRACE_OP_LOAD, TRACE_OP_STORE,
    TRACE_OP_COPY, TRACE_OP_FLUSH, TRACE_OP_SYNC, TRACE_OP_RESET_STATE,
    TRACE_OP_HIRES_REPLICATE, TRACE_OP_LOWRES_MARK, TRACE_OP_LOWRES_RESOLVE,
    TRACE_OP_TRACE_CALL, TRACE_OP_TRACE_TIME,
    TRACE_OP_CONFIGURE = 0x10, TRACE_OP_SET_EXACT_1X, TRACE_OP_SET_TEXTURE_MAPPING,
    TRACE_OP_VRAM_GET,
    TRACE_OP_PRECISE_ENABLE = 0x20, TRACE_OP_PRECISE_BEGIN_FRAME, TRACE_OP_PRECISE_RECORD,
    TRACE_OP_PRECISE_STATS,
    TRACE_OP_PRESENT = 0x30, TRACE_OP_CAPTURE, TRACE_OP_POKE,
    TRACE_OP_LIMIT
};

/* The op's name, or "prelude" for a code that is none. */
const char *trace_op_name(unsigned code);

typedef struct trace_reader {
    const char *path;
    unsigned char *data;
    size_t len, pos, end; /* end: of the current op's payload */
} trace_reader;

/* The prelude (FORMAT.md, "Prelude"). The precise table is left in the file:
   trace_precise_entry reads it. */
typedef struct trace_prelude {
    int scale, dither, threads, exact_1x, texture_mapping, persp_span, persp_span_1x;
    int precise_enabled;
    uint32_t precise_lookups, precise_hits, precise_count;
    size_t precise_table; /* offset of the table's first entry */
    uint32_t gpu_scale;
    uint64_t start_hash_1x, start_hash_scaled;
} trace_prelude;

#if defined(__GNUC__)
__attribute__((noreturn))
#endif
void trace_die(const trace_reader *r, const char *what);

uint32_t trace_u32(trace_reader *r);
int trace_i32(trace_reader *r);
uint64_t trace_u64(trace_reader *r);
/* u16[n], padded to 4 bytes, into a new array (free it). */
uint16_t *trace_u16s(trace_reader *r, uint32_t n);

/*
 * Reads the file at path, its header and prelude: the scene as "NN-name"
 * into name, the prelude into *p. Leaves r at the first op. trace_close
 * frees the data.
 */
void trace_open(trace_reader *r, const char *path, char *name, size_t name_size,
                trace_prelude *p);
void trace_close(trace_reader *r);

/* Entry i of the prelude's precise table. */
void trace_precise_entry(const trace_reader *r, const trace_prelude *p, uint32_t i,
                         uint32_t *key, int *x, int *y, int *z);

/* The next op's code; r->end is set to the end of its payload. For END it
   also checks that END has no payload and ends the file. */
uint32_t trace_next_op(trace_reader *r);
/* After an op's fields were read: they must be all of its payload. */
void trace_op_done(trace_reader *r);

/* FNV-1a, 64 bits (FORMAT.md, "Hash"). */
#define TRACE_FNV_INIT 0xcbf29ce484222325ull
uint64_t trace_fnv_bytes(uint64_t h, const unsigned char *p, size_t n);
/* A VRAM hash of n pixels, each low byte first. */
uint64_t trace_fnv_pixels(const uint16_t *p, size_t n);

/* A golden image (FORMAT.md, "Golden images") of w x h as VRAM pixels
   (free it), or NULL with the reason in *why. */
uint16_t *trace_golden_load(const char *path, int w, int h, const char **why);

#endif

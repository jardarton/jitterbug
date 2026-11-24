/*
 * Reading and playing conformance lists (FORMAT.md) through the public
 * API alone. C99. Used by conformance.c (the runner) and selftest.c.
 */
#ifndef JB_CONFORMANCE_JBC_H
#define JB_CONFORMANCE_JBC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "jbc_format.h"

enum { JBC_VRAM_W = 1024, JBC_VRAM_H = 512, JBC_VRAM_PIXELS = JBC_VRAM_W * JBC_VRAM_H };

typedef struct jbc_list {
    char name[128];   /* "gpu/triangle" */
    char source[512]; /* the provenance line */
    uint32_t *w;      /* the whole file as words */
    size_t n;
    size_t first; /* the first op's word */
    size_t loop;  /* the LOOP op's word, or 0 for none */
    unsigned reads, expects;
} jbc_list;

/* Reads and checks a list: every op known and inside the file, END last,
   at most one LOOP and no READ or EXPECT after it, every EXPECT naming a
   read before it, and LOOP and END between GPU packets (FORMAT.md, "Packet
   boundaries at LOOP and END"). 0, or -1 with the reason in err. */
int jbc_load(const char *path, jbc_list *l, char *err, size_t err_size);
/* The same from n words in memory (copied). */
int jbc_parse(const uint32_t *words, size_t n, jbc_list *l, char *err, size_t err_size);
void jbc_free(jbc_list *l);

/* FNV-1a, 64 bits, over the list's bytes as stored (little-endian words):
   the fixture's identity (tests/conformance/fixtures.txt). */
uint64_t jbc_checksum(const jbc_list *l);

/* FNV-1a, 64 bits, over a file's bytes: a reference's identity (the
   REFSUM of fixtures.txt). 0, or -1 when it cannot be read. */
int jbc_file_checksum(const char *path, uint64_t *sum);
uint64_t jbc_bytes_checksum(const unsigned char *bytes, size_t size);

/* A whole file (at most 16 MiB; free it), or NULL with the reason. What
   is checked and what is used must be the same bytes, so a file is read
   once and then hashed, parsed or decoded from memory. */
unsigned char *jbc_read_file(const char *path, size_t *size, const char **why);

/* A whole text file with a terminating zero added (free it); a zero byte
   inside it is refused. jbc_text_line copies the next line, its newline
   included, into line: 1, 0 at the end, -1 when it does not fit. */
char *jbc_read_text(const char *path, size_t *size, const char **why);
int jbc_text_line(const char *text, size_t size, size_t *pos, char *line, size_t cap);

/*
 * What a ctest test says it plays (fixtures.txt; FORMAT.md, "Fixture tables"):
 * the program, the kind of reference it is compared with, the passes of
 * the main loop after which it is compared (0 for a list without one),
 * its number of EXPECT ops, its size in words and its checksum.
 */
typedef enum { JBC_REF_DUMP, JBC_REF_LOG, JBC_REF_PROBE } jbc_reference;

typedef struct jbc_fixture {
    const char *name; /* "triangle": the list must be "gpu/triangle", or a
                         name with a slash ("fuzz/quad-order"): the list's own */
    jbc_reference reference;
    unsigned passes;
    unsigned expects;
    unsigned long words;
    uint64_t checksum;
} jbc_fixture;

/* 0 when l is the fixture f describes, or -1 with each difference in err. */
int jbc_check_fixture(const jbc_list *l, const jbc_fixture *f, char *err, size_t err_size);

/*
 * A line of a fixture table (fixtures.txt, open/fixtures.txt; FORMAT.md,
 * "Fixture tables"): the program's whole identity, which the runner takes from
 * the table and from nowhere else. fixture.name points at name ("triangle",
 * or "fuzz/quad-order" for a line whose WHERE is open), so an entry is
 * used where it was filled, not copied.
 */
typedef struct jbc_table_entry {
    jbc_fixture fixture;
    char name[160];
    char key[128];     /* the line's NAME field ("triangle", "quad-order") */
    char where[8];     /* repo, fetched or open */
    /* REFSUM: FNV-1a, 64 bits, of the reference file's bytes (the dump's
       vram.png, or the log's psx.log), present on every dump or log line
       and on no other (a probe result is bound by its own CRC-32s). */
    int has_ref_checksum;
    uint64_t ref_checksum;
    char exclude[128]; /* the EXCLUDE file (in the table's directory), or "" */
    unsigned exclude_pixels;
    uint64_t exclude_checksum;
} jbc_table_entry;

/* The line whose NAME is key. Every line of the table is checked before
   any is taken: each must be well formed (7 fields for a probe reference,
   8 for a dump or log, 11 for a dump with an exclusion, each of the right
   kind), and no NAME may appear twice anywhere in it. 0, or -1 with the
   reason in err. */
int jbc_table_lookup(const char *table, const char *key, jbc_table_entry *e, char *err,
                     size_t err_size);

/*
 * The line of a fixture table for the list l, found by its name: "gpu/NAME"
 * for a line whose WHERE is repo or fetched, "fuzz/NAME" for one whose
 * WHERE is open. Fills f, with f->name in name (name_size bytes), and where
 * ("repo", "fetched" or "open"; 8 bytes). 0, or -1 with the reason in err
 * (no line for l, a malformed table). Whether l is that fixture is still
 * jbc_check_fixture's to say. For jbprobe send, which is given a list and
 * a table.
 */
int jbc_fixture_find(const char *table, const jbc_list *l, jbc_fixture *f, char *name,
                     size_t name_size, char *where, char *err, size_t err_size);

/* A copy of l with words inserted just before its END (selftest.c). */
int jbc_append(const jbc_list *l, const uint32_t *words, size_t n, jbc_list *out);

/* The text of a string payload at word i (a label), into buf. */
void jbc_string(const jbc_list *l, size_t i, char *buf, size_t size);

typedef enum {
    JBC_SPLIT_OP,     /* one jb_gp0 call for each GP0 op */
    JBC_SPLIT_WORDS,  /* one jb_gp0 call for each word */
    JBC_SPLIT_RANDOM  /* consecutive GP0 ops' words joined and cut at random */
} jbc_split;

typedef struct jbc_options {
    int threads;       /* jb_config.threads; 0 means 1 */
    int scale;         /* jb_config.scale; 0 means 1. The 1x VRAM
                          (jb_vram_read) is what is compared at any scale */
    jbc_split split;
    uint32_t seed;     /* for JBC_SPLIT_RANDOM */
    unsigned max_loops; /* passes of the main loop before giving up; 0 means 1000 */
    /* The passes of the main loop after which VRAM is compared, counting
       the first (FORMAT.md, "Playing a list"); 0 to play until a pass
       leaves VRAM as it found it. With passes set, `window` (0 means 2)
       more passes are played, and VRAM and the state read back
       (jbc_outcome.state_steady) must not change in them. */
    unsigned passes;
    unsigned window;
    FILE *log;         /* checks and waits that fail are reported here; may be NULL */
    int picture;       /* also keep the whole scaled picture (jbc_outcome) */
} jbc_options;

typedef struct jbc_outcome {
    uint16_t *vram;   /* the 1x VRAM at the end (free it) */
    uint32_t *reads;  /* every value read, in order (free it) */
    unsigned nreads;
    unsigned loops;   /* times the main loop's body was played */
    unsigned passes;  /* passes of the main loop at the comparison, the first
                         included (0 without a main loop) */
    int steady;       /* 1 when VRAM stayed the same after it (or there is
                         no main loop); with jbc_options.passes, through
                         the whole window */
    int early;        /* with jbc_options.passes above 1: 1 when VRAM had
                         stopped changing a pass before (passes is not the
                         first pass of the fixed point) */
    unsigned changed_at; /* when not steady, the pass that changed VRAM */
    /* With jbc_options.passes: 1 when the GPU state the public API reads
       back (GPUSTAT, and GP1(10h) indices 2-5: the texture window, the
       drawing area's corners, the drawing offset) was after each pass of
       the window as after pass P (FORMAT.md, "Playing a list"); else the
       first pass where it was not, and what differed. */
    int state_steady;
    unsigned state_changed_at;
    char state_diff[192];
    /* With jbc_options.passes: the passes of the window played and checked
       (the window ends at the first pass that changes VRAM), and the
       window's length. */
    unsigned window_played, window;
    unsigned checks, checks_failed;
    unsigned waits, waits_failed;
    /* With jbc_options.picture: all of VRAM as the scaled picture shows it,
       1024 x 512 times the scale, R, G, B, A bytes (free it); else NULL. */
    unsigned char *picture;
    int picture_w, picture_h;
} jbc_outcome;

/*
 * Plays the list on a new context (at o->scale). The main loop (LOOP to END)
 * is played to o->passes, then through the window (FORMAT.md, "Playing a
 * list"); or, when o->passes is 0, until a pass leaves VRAM as it found it. A WAIT or DRAWSYNC
 * whose condition does not hold is counted as failed (the console would
 * wait there for ever). 0, or -1 with the reason in err (no context).
 */
int jbc_play(const jbc_list *l, const jbc_options *o, jbc_outcome *out, char *err, size_t err_size);
void jbc_outcome_free(jbc_outcome *out);

/* What the main loop's window showed (an outcome of jbc_play with
   jbc_options.passes), as two lines: VRAM, then the state read back. Each
   speaks only of the passes that were played and checked. */
void jbc_describe_loop(const jbc_outcome *out, char *buf, size_t size);

/* A console VRAM dump (a 1024 x 512 PNG, each 5-bit channel shifted left
   by 3, opaque) as 15-bit pixels (free them), or NULL with the reason. */
uint16_t *jbc_load_dump(const char *path, const char **why);
/* The same from the PNG's bytes in memory. */
uint16_t *jbc_decode_dump(const unsigned char *bytes, size_t size, const char **why);

/* Our VRAM against a dump on the 15 colour bits: the number of pixels
   that differ and their bounding box (x0 > x1 when there are none). */
typedef struct jbc_diff {
    unsigned long count;
    int x0, y0, x1, y1;
} jbc_diff;

void jbc_compare(const uint16_t *ours, const uint16_t *dump, jbc_diff *d);

/*
 * Exclusions (FORMAT.md, "Exclusions"): pixels of a dump that are not
 * compared, each line of the file giving them and its reason. Sets
 * excluded[y * 1024 + x] to 1 for each (a 1024 x 512 array the caller
 * zeroes) and counts them in *pixels and the lines in *entries. 0, or -1
 * with the reason in err.
 */
int jbc_load_exclusions(const char *path, unsigned char *excluded, unsigned *pixels,
                        unsigned *entries, char *err, size_t err_size);
/* The same from the file's text (name is for messages). */
int jbc_parse_exclusions(const char *text, const char *name, unsigned char *excluded,
                         unsigned *pixels, unsigned *entries, char *err, size_t err_size);

/* jbc_compare without the excluded pixels (excluded may be NULL). */
void jbc_compare_excluding(const uint16_t *ours, const uint16_t *dump,
                           const unsigned char *excluded, jbc_diff *d);
/* jbc_compare_excluding on the bits given of each side: 0x7fff for a
   ps1-tests dump, 0xffff for a probe result (tools/probe), which has the
   mask bit. */
void jbc_compare_bits(const uint16_t *ours, const uint16_t *ref, const unsigned char *excluded,
                      unsigned bits, jbc_diff *d);

/* FNV-1a, 64 bits, over the index (y * 1024 + x, a little-endian 32-bit
   word) of each excluded pixel in increasing order: the identity of an
   exclusion set, which the runner checks against the one documented
   (fixtures.txt). */
uint64_t jbc_exclusion_checksum(const unsigned char *excluded);

/*
 * The verdict on one run (conformance.c): from what differed, the exit
 * status and the summary line ("NAME: MATCHES", "NAME: MATCHES (64 pixels
 * excluded)", "NAME: DIFFERS", ...), written to line.
 *   vram_differs, checks_failed, reads_differ: the comparison with the
 *     console (reads_differ: the words read, against a probe result's);
 *   waits_failed, loop_failed: the list could not be played as the console
 *     ran it (a wait that would never end, a main loop whose VRAM did not
 *     settle when the fixture says);
 *   expect_mismatch: the program is in expected-failures.txt;
 *   console_checks_failed: the console of a probe result failed the list's
 *     own checks (a difference between consoles, never accepted).
 * Only a VRAM, check or read difference satisfies expect_mismatch: a failed wait
 * or loop, or a check the console failed, is always a failure (exit 1).
 * Status: 0 pass, 1 fail.
 */
typedef struct jbc_verdict {
    int vram_differs;
    unsigned checks_failed;
    unsigned waits_failed;
    int loop_failed;
    int expect_mismatch;
    unsigned excluded; /* pixels excluded from the comparison */
    unsigned reads_differ;
    /* With a probe result as the reference: the list's EXPECT ops that the
       console itself failed (its read differs from the value the list
       carries, which another console read): reported, and a failure. */
    unsigned console_checks_failed;
} jbc_verdict;

int jbc_judge(const char *name, const jbc_verdict *v, char *line, size_t size);

#endif

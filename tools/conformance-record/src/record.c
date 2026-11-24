/*
 * The list being recorded (tests/conformance/FORMAT.md) and its end.
 *
 * Ops are appended to one array of words. Runs of the same kind are
 * merged: GP0, GP0 by DMA and GP1 words extend the op before them when it
 * is of the same kind, reads of the same port add to its count, and a
 * DRAWSYNC or WAIT right after an identical one is left out (waiting twice
 * for the same thing is waiting once). A CALL op names the outermost call
 * whenever it differs from the last named.
 *
 * Environment: JBREC_OUT, the list to write; JBREC_NAME, the program
 * ("gpu/triangle"); JBREC_SOURCE, a provenance line for the header;
 * JBREC_WORDS_ONLY=1 for a recording made only to compare the words a
 * program sends with another recording's (record.sh): every read then
 * gives 1C000000h, GPUSTAT's three ready bits, so that a polling loop of
 * the program's own ends after one read.
 */
#include "rec.h"

#include <jbc_format.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The recording ends at this vertical blank: the frames between the
   first and this one must be identical (docs/testing.md, "How
   `tools/conformance-record` works",
   "Main loop & checks"). */
enum { FRAMES = 4 };

static struct {
    uint32_t *w;
    size_t n, cap;
    size_t last;    /* index of the last op's word, or (size_t)-1 */
    unsigned depth; /* nesting of rec_enter */
    char call[64];  /* the last CALL written; "" for none since a VSYNC */
    uint32_t reads;
    int last_port;
    int probe;
    uint32_t probe_value;
    unsigned probe_reads;
    size_t seg[FRAMES + 1]; /* seg[k]: the end of frame k's ops */
    int vsyncs;
    int done;
} R = {NULL, 0, 0, (size_t)-1, 0, "", 0, -1, 0, 0, 0, {0}, 0, 0};

void rec_fail(const char *fmt, ...)
{
    va_list ap;
    const char *name = getenv("JBREC_NAME");

    fprintf(stderr, "conformance-record: %s: ", name ? name : "?");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    R.done = 1;
    _Exit(1);
}

static void put(uint32_t v)
{
    if (R.n == R.cap) {
        R.cap = R.cap ? R.cap * 2 : 1 << 16;
        R.w = realloc(R.w, R.cap * sizeof *R.w);
        if (R.w == NULL)
            rec_fail("out of memory");
    }
    R.w[R.n++] = v;
}

static void op(unsigned code, size_t n)
{
    if (n > JBC_MAX_COUNT)
        rec_fail("an op of %zu words", n);
    R.last = R.n;
    put((uint32_t)JBC_OP(code, n));
}

static unsigned last_code(void)
{
    return R.last == (size_t)-1 ? JBC_OP_LIMIT : JBC_OP_CODE(R.w[R.last]);
}

/* A string as a payload: its length, then its bytes padded with zeros. */
static size_t string_words(const char *s)
{
    return 1 + (strlen(s) + 3) / 4;
}

static void put_string(const char *s)
{
    size_t len = strlen(s), i;

    put((uint32_t)len);
    for (i = 0; i < len; i += 4) {
        uint32_t v = 0;
        size_t k;
        for (k = 0; k < 4 && i + k < len; ++k)
            v |= (uint32_t)(unsigned char)s[i + k] << (8 * k);
        put(v);
    }
}

void rec_enter(const char *name)
{
    if (R.probe)
        return;
    if (R.depth == 0) {
        mmio_flush();
        if (strcmp(name, R.call) != 0) {
            if (strlen(name) >= sizeof R.call)
                rec_fail("call name too long: %s", name);
            op(JBC_CALL, string_words(name));
            put_string(name);
            strcpy(R.call, name);
        }
    }
    ++R.depth;
}

void rec_leave(void)
{
    if (R.probe)
        return;
    if (R.depth == 0)
        rec_fail("rec_leave without rec_enter");
    --R.depth;
}

static void words(unsigned code, const uint32_t *w, size_t n)
{
    size_t i;

    if (R.probe || n == 0)
        return;
    if (last_code() == code && JBC_OP_COUNT(R.w[R.last]) + n <= JBC_MAX_COUNT)
        R.w[R.last] += (uint32_t)n;
    else
        op(code, n);
    for (i = 0; i < n; ++i)
        put(w[i]);
}

void rec_gp0(const uint32_t *w, size_t n, int dma)
{
    words(dma ? JBC_GP0_DMA : JBC_GP0, w, n);
}

void rec_gp1(uint32_t word)
{
    words(JBC_GP1, &word, 1);
}

uint32_t rec_read(int port)
{
    static int words_only = -1;

    if (words_only < 0) {
        const char *v = getenv("JBREC_WORDS_ONLY");
        words_only = v != NULL && strcmp(v, "1") == 0;
    }
    if (R.probe) {
        ++R.probe_reads;
        return R.probe_value;
    }
    if (R.vsyncs > 0)
        rec_fail("a read in the main loop (not supported: READ and EXPECT ops are "
                 "numbered from the start)");
    if (last_code() == JBC_READ && R.w[R.last + 1] == (uint32_t)port)
        ++R.w[R.last + 2];
    else {
        op(JBC_READ, 2);
        put((uint32_t)port);
        put(1);
    }
    ++R.reads;
    R.last_port = port;
    return words_only ? 0x1c000000u : 0;
}

void rec_wait(uint32_t mask, uint32_t value)
{
    if (R.probe)
        return;
    if (last_code() == JBC_WAIT && R.w[R.last + 1] == mask && R.w[R.last + 2] == value)
        return;
    op(JBC_WAIT, 2);
    put(mask);
    put(value);
}

void rec_drawsync(void)
{
    if (R.probe || last_code() == JBC_DRAWSYNC)
        return;
    op(JBC_DRAWSYNC, 0);
}

void rec_expect(uint32_t read_index, uint32_t mask, uint32_t value, const char *label)
{
    op(JBC_EXPECT, 3 + string_words(label));
    put(read_index);
    put(mask);
    put(value);
    put_string(label);
}

uint32_t rec_reads(void)
{
    return R.reads;
}

int rec_last_port(void)
{
    return R.last_port;
}

void rec_probe_begin(uint32_t value)
{
    R.probe = 1;
    R.probe_value = value;
    R.probe_reads = 0;
}

unsigned rec_probe_end(void)
{
    R.probe = 0;
    return R.probe_reads;
}

int rec_probing(void)
{
    return R.probe;
}

unsigned char *rec_read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *p = NULL;
    size_t cap = 0, n = 0, got;

    if (f == NULL)
        rec_fail("cannot open %s", path);
    do {
        if (n == cap) {
            cap = cap ? cap * 2 : 65536;
            p = realloc(p, cap + 1);
            if (p == NULL)
                rec_fail("out of memory");
        }
        got = fread(p + n, 1, cap - n, f);
        n += got;
    } while (got > 0);
    if (ferror(f))
        rec_fail("cannot read %s", path);
    fclose(f);
    p[n] = 0;
    *size = n;
    return p;
}

/* Writes the header, the ops [0, end), then (when loop is not 0) LOOP and
   the ops [loop_begin, loop_end), then END. */
static void write_list(size_t end, int loop, size_t loop_begin, size_t loop_end)
{
    const char *path = getenv("JBREC_OUT"), *name = getenv("JBREC_NAME");
    const char *source = getenv("JBREC_SOURCE");
    uint32_t *body = R.w;
    size_t i;
    FILE *f;
    const uint32_t probe = 1;

    if (*(const unsigned char *)&probe != 1)
        rec_fail("the host is not little-endian");
    if (path == NULL || name == NULL)
        rec_fail("JBREC_OUT and JBREC_NAME must be set");
    if (source == NULL)
        source = "";

    /* The header, built in the same array after the ops. */
    R.w = NULL;
    R.n = R.cap = 0;
    put(0); /* the magic, below */
    put(0);
    put(JBC_VERSION);
    put(0); /* header bytes, below */
    put_string(name);
    put_string(source);
    memcpy(R.w, JBC_MAGIC, 8);
    R.w[3] = (uint32_t)(R.n * 4);
    for (i = 0; i < end; ++i)
        put(body[i]);
    if (loop) {
        put((uint32_t)JBC_OP(JBC_LOOP, 0));
        for (i = loop_begin; i < loop_end; ++i)
            put(body[i]);
    }
    put((uint32_t)JBC_OP(JBC_END, 0));
    free(body);

    f = fopen(path, "wb");
    if (f == NULL || fwrite(R.w, 4, R.n, f) != R.n || fclose(f) != 0)
        rec_fail("cannot write %s", path);
    fprintf(stderr, "conformance-record: %s: %zu words to %s\n", name, R.n, path);
}

/* Whether the ops in the words [a, end) include a READ or an EXPECT (not
   allowed in the loop). */
static int has_reads(size_t a, size_t end)
{
    size_t i = a;

    while (i < end) {
        unsigned code = JBC_OP_CODE(R.w[i]);
        if (code == JBC_READ || code == JBC_EXPECT)
            return 1;
        i += 1 + JBC_OP_COUNT(R.w[i]);
    }
    return 0;
}

void rec_vsync(void)
{
    if (R.probe)
        rec_fail("VSync while probing a check");
    mmio_flush();
    op(JBC_VSYNC, 0);
    R.call[0] = 0; /* each frame names its calls afresh */
    R.seg[++R.vsyncs] = R.n;
    if (R.vsyncs < FRAMES)
        return;

    /* Frames 2 to FRAMES are the main loop's body; they must all be the
       same words. Frame 1 (everything up to the first vertical blank) is
       the start, which includes the loop's first pass. */
    {
        size_t len = R.seg[2] - R.seg[1];
        int k;
        for (k = 2; k < FRAMES; ++k)
            if (R.seg[k + 1] - R.seg[k] != len ||
                memcmp(R.w + R.seg[1], R.w + R.seg[k], len * 4) != 0)
                rec_fail("frame %d differs from frame 2: the main loop does not repeat the "
                         "same words",
                         k + 1);
        if (has_reads(R.seg[1], R.seg[2]))
            rec_fail("reads in the main loop are not supported");
        check_finish();
        write_list(R.seg[1], 1, R.seg[1], R.seg[2]);
    }
    R.done = 1;
    free(R.w);
    exit(0);
}

/* A program that returns from main without a main loop: the whole list. */
static void at_exit(void)
{
    if (R.done)
        return;
    R.done = 1;
    mmio_flush();
    check_finish();
    write_list(R.n, 0, 0, 0);
}

__attribute__((constructor)) static void rec_init(void)
{
    atexit(at_exit);
}
